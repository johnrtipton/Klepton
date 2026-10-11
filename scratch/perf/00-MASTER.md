# Klepton performance deep-dive — master findings

> **Status 2026-10-11: the top items below are IMPLEMENTED and MEASURED — see
> `PERF-BENCH.md`** for before/after numbers (`make perf`), the -O1 vs -O2 A/B,
> and the source-change list. Headline results: shim lookup 715→11 ns (63×),
> JNI dispatch 798→27 ns (29×), ovrp entry 70→26 ns (2.7×), contended guest
> mutex 112→52 ns (2.2×), audio convert 3436→1570 ns (2.2×, and 5.6× more at
> -O2). Items 1 (build flags), 2, 5 (partly), 6 (partly), 7, 9 and 12-ish
> (per-frame env reads) are the ones landed; the rest remain open with their
> fix sketches below.

Date: 2026-10-11. Method: six-domain read of the runtime by the lead session
(subagents were dispatched but rate-limited; all domains below were read
first-hand except reprojection/Vulkan, whose agent delivered before dying).
Every finding cites file:line and carries a fix sketch in its domain file.

Domain files (all in `scratch/perf/`):

| file | scope |
|---|---|
| `build-app-driver.md`  | build flags, Xcode/run.sh configs, app+driver glue, cross-cutting |
| `reproject-vulkan.md`  | kl_reproject, kl_vulkan, kl_cvmtl (agent-written) |
| `gfx-present.md`       | kl_glfb, kl_egl, kl_present, GL thunks |
| `xr-layer.md`          | kl_ovrp, kl_openxr, pose seam |
| `translation-layer.md` | kl_shim, kl_libc, kl_pthread, kl_dl/kl_image sym resolution, kl_va, x18, klepton-ld |
| `jni-assets.md`        | kl_jni dispatch/strings, OBB, AAsset |
| `media-streaming.md`   | kl_audio, OpenSL, AAudio, vtdec, mediandk, Steam Link shims |

---

## The 12 highest-leverage items, ranked

1. **Ship optimized code.** The runtime is built `-g -O1` (`Makefile:15,740`)
   and the visionOS app is built `-configuration Debug` with Swift `-Onone`
   (`visionos/run.sh:189,404`, `gen_xcodeproj.py:547,551`). Every per-frame C
   and Swift path runs unoptimized on the only platform that ships. Bump to
   `-O2`, add a Release lane to run.sh, add `-flto=thin`. Cheapest big win in
   the repo. [build-app-driver X1, X3]

2. **Stop re-resolving JNI bindings on every call.** `Call*Method` scans all
   ~614 binding rows (× superclass levels) with strcmp per call
   (`kl_jni.c:991-1047`). Cache the binding pointer on the interned
   jmethodID at first call → O(1) dispatch. [jni-assets J1]

3. **Stop draining the Vulkan queue every frame.** `KL_VK_FRAME_SYNC`
   (default ON) does `vkQueueWaitIdle` per frame end (`kl_vulkan.c:2448-2472`)
   — no CPU/GPU pipelining, up to a frame of added latency. The in-file
   comment already names the fix: export a MTLSharedEvent via
   VK_EXT_metal_objects (the extension is already enabled) and signal instead
   of waiting; the GL path already works this way (`klfb_gpu_frame_now`).
   [reproject-vulkan F1]

4. **Hash the Vulkan image/view handle tables.** `klvk_img_reg_has` (16384
   entries), `klvk_view_image` (8192), `klvk_img_info_get` (4096) are linear
   scans under a mutex, hit per image command AND per barrier in every
   barrier array (`kl_vulkan.c:1431,1416,1820,2784`). The file already
   contains the hash pattern (`klvk_hash64` :4573). [reproject-vulkan F2]

5. **Make symbol resolution O(1).** `kl_sym` linearly scans each image's
   whole .dynsym (`kl_image.c:1204-1236`) and `kl_shim_lookup` the ~650-row
   shim table (`kl_shim.c:2258`) — per import at load and per dlsym, with
   `kl_guest_sym_global` multiplying by image count (`kl_dl.c:295-303`).
   Every Android lib carries DT_GNU_HASH; use it (linear fallback stays).
   This is the host-side load-time and level-load fix. [translation-layer T1, T2]

6. **Delete the per-draw glGetIntegerv.** `klfb_note_draw` issues a
   synchronous GL query on every draw call (`kl_glfb.c:5737-5748`) whose only
   consumers are debug censuses. Gate it or use the wrapper-tracked binding
   the function already falls back to. Plus the unconditional per-draw
   snprintf one line later (:6145-6146). [gfx-present G1, G2]

7. **De-instrument the pthread hot path.** Every guest mutex lock CASes a
   shared 64-slot global array (`mtx_wait_enter/leave`,
   `kl_pthread.c:387-405`) + 3-4 atomics; every cond signal pays
   clock_gettime + 2 atomics (:757-767); every cond wait ~8 atomics + a frame
   walk (:786-806). All hang-diagnosis bookkeeping, always on. TLS slots +
   a latched knob. [translation-layer T3, T4]

8. **Cache dlsym lookups per draw call.** `klvk_have_buf_mtl` calls `dlsym`
   per `vkCmdDrawIndexed` when the intotheradius guard is on
   (`kl_vulkan.c:4179-4181,4241`) — the dyld lock per draw. One static flag
   fixes it. [reproject-vulkan F6]

9. **Stop the per-frame environ scans.** Uncached `getenv` per frame today:
   `KL_LOWMEM_AT/CLEAR` + `os_proc_available_memory` per frame
   (`kl_libc.c:1122-1133`), three never-latching probes in `kl_mprobe_tick`
   (`kl_mprobe.c:401-414`), `KL_GLFB_OUT` per swap (`kl_egl.c:1672`),
   `KL_XR_CAPTURE_LAYER` per EndFrame (`kl_openxr.c:5334`), and the reproject
   builders' 2-8 per frame (`kl_reproject.c:105-110,942-968`). A
   `kl_env_*_cached` helper closes the class. [build-app-driver X2, X9; xr X3; reproj F4]

10. **ovrp_slot / ovrp_log_arg: lock + linear scan per ovrp call, forever**
    (`kl_ovrp.c:44-56,215-231`). Bake the slot into the stub; early-out the
    saturated seen-table. [xr-layer XR1, XR2]

11. **Mirror-copy dedup on the UE5 Vulkan path.** Every descriptor re-bind
    re-records 2 barriers + 1-2 copies per mirror (up to 128) — hundreds of
    redundant GPU copies per frame on olar/wanderer. Dedupe per submit or
    dirty-track. [reproject-vulkan F3]

12. **The ES3.1→3.0 vertex translation re-applies every attrib per draw**
    (`kl_glfb.c:6098-6132`) — multiplies Source-engine per-draw GL call
    count 3-4×. Dirty-check with a per-VAO serial. [gfx-present G3]

## Themes worth naming

- **The architecture is right; the instrumentation is always-on.** Nearly
  every hot-path finding is *diagnostic bookkeeping with no gate*: the draw
  census, the ovrp call counter, the mutex waiter table, the cond signal
  clock, the mem-pressure poll, the per-swap env read. A systematic pass
  ("every counter is free or gated") is worth as much as any single fix.
- **Tables grow, scans never shrink.** kl_sym, kl_shim_lookup, klj_want,
  klj_find_binding, ovrp_slot, the Vulkan handle tables — all linear, all on
  paths that run per-call or per-import. One shared open-addressed hash
  utility (the codebase already has two local implementations to copy) covers
  them all.
- **Zero correct places were found doing the classic VR-runtime sins**: no
  CPU blits in the reproject path (GPU-only, verified), no CPU color convert
  on video (native biplanar + IOSurface), the audio render callback is a pure
  memcpy, the pose seam is a bounded seqlock, the GL eye textures are
  zero-copy MTLTextures, and the composite is a single amplified foveated
  pass. The foundation is sound — the wins are in dispatch overhead,
  pipelining, and build configuration.
- **There are no benchmarks.** Every gate in `make check` is correctness.
  Before/with any of the above: add a ~30-line timing loop to t_reproject
  (uniform builds), a barrier-path microbench to the Vulkan gates, and use
  the existing KL_SAMPLE_MS on-device profiler to capture a baseline now.
  Several "wins" above are µs-scale and must be measured, not assumed.

## Recommended order of attack

1. X1 build flags (+ baseline KL_SAMPLE_MS capture before/after) — hours, whole-system win.
2. J1 JNI binding cache, T1/T2 symbol hashing — small diffs, big dispatch win.
3. F1 Vulkan event-based frame sync, F6 dlsym latch — pipelining.
4. G1/G2 draw-path de-instrumentation, X2 env latching pass — an afternoon.
5. F2/T-tables hash sweep — mechanical.
6. T3/T4 pthread bookkeeping, XR1/XR2 ovrp slots — careful, contention-sensitive.
7. G3 vertex-bind dirty check, F3 mirror dedup — targeted at Source / UE5 titles.
8. M1/M2/M3 audio vectorization — with an audio-bench first.

## Follow-ups deliberately not done

- `kl_glfb.c`'s mirror path (~:4456) and BlitFramebuffer thunk — not line-read.
- `kl_jni_io.c` / `kl_jni_prefs.c` per-call file I/O — not read.
- `kl_avdec.m` — skimmed.
- A device boot-time profile (the "tens of seconds" attribution) — instrument
  exists (KL_SAMPLE_MS works on device); one capture session would localize it.
