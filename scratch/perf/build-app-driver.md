# Perf findings: build system, app glue, and the shared frame driver

Scope: `Makefile`, `visionos/gen_xcodeproj.py`, `visionos/run.sh`, `build_run_*.sh`,
`visionos/Sources/kl_app.c`, `visionos/Sources/KleptonCompositor.swift` (frame loop,
primeDisplay, encodeComposite prologue), `visionos/Sources/KleptonControllers.swift`,
`visionos/Sources/KleptonAudio.swift`, `runtime/guest/kl_driver.c`, `runtime/kl_env.c`,
`runtime/diag/kl_mprobe.c` (tick path), `runtime/libc/kl_libc.c` (mem-pressure poll),
`runtime/diag/kl_sample.c` (gating), `runtime/gfx/kl_gl_trace.S`, `kl_gl_lock.S`,
`runtime/kl_image.c` (load-time only scans).

## Top 5

1. **The whole shipping runtime is compiled at `-O1`, and the visionOS app itself
   ships Debug (`-O0` / Swift `-Onone`).** `Makefile:15` (`CFLAGS := -g -O1`),
   `Makefile:740` (`XROS_CFLAGS := -g -O1`), and `visionos/run.sh:189,404` build the
   app with `-configuration Debug` while `gen_xcodeproj.py:547,551` pins
   `GCC_OPTIMIZATION_LEVEL = 0` / `SWIFT_OPTIMIZATION_LEVEL = "-Onone"`. Every
   per-frame C hot path (glfb, reproject, ovrp, audio) runs at -O1 and every
   per-frame Swift line (compositor math, controller merge) runs at -Onone.
2. **`kl_driver_frame` pays ~6 uncached `getenv` environ scans per frame** through
   `kl_mem_pressure_poll` (`runtime/libc/kl_libc.c:1123,1125`) and `kl_mprobe_tick`
   (`runtime/diag/kl_mprobe.c:401,408,414` — the `!st_done` guards never latch when
   the knob is unset, so they scan environ *every frame forever*), plus an
   `os_proc_available_memory()` call every frame.
3. **No LTO anywhere**, and the runtime is linked as one `-all_load` archive —
   cross-module inlining between kl_* translation units is off the table at any
   -O level without it (or unity builds of the hottest cluster).
4. **xros/xrsim archive rules recompile every source serially** when any header
   changes (`Makefile:783-799`: `RUNTIME_ALL_HDRS` wildcard prerequisite + a
   per-source `for` loop with no `-j` parallelism inside the loop and no ccache).
   ~15 s of a 30 s incremental `make xros` is this; a header edit is a full
   serial rebuild.
5. **Compositor startup trace logs 300 NSLogs in the first seconds** —
   `KleptonCompositor.swift:2049-2054` — plus `stageSummary()`/`cadenceSummary()`
   string building twice a second forever (`:1483-1484`). Small, but NSLog on
   device is a write to a pipe; the startup trace is the one that overlaps the
   shader-warmup window where the guest is slowest.

## Findings

### X1 — HIGH — runtime at -O1, app at Debug/-O0/-Onone on device
`Makefile:15`:
```make
CFLAGS  := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64 ...
```
`Makefile:740`:
```make
XROS_CFLAGS := -g -O1 -Wall -Wextra -Wno-unused-parameter -arch arm64 ...
```
`visionos/run.sh:189` and `:404` build the app `-configuration Debug`;
`visionos/gen_xcodeproj.py:547,551` pin the Debug config to `-O0`/`-Onone`.

The guest's own code is untranslated ARM64 (not ours to optimize), but everything
around it — GL thunk dispatch, reproject uniform building, OVRPlugin pose queries,
audio mixing, JNI — is compiled at -O1, and the per-frame Swift compositor
(`renderFrame`, `updatePoses`, `KleptonControllers.update`, haptics pump) runs
completely unoptimized on the device: no SIMD fusion in the simd_float4x4 math,
no retain/release elision, boxed optionals.

**Fix sketch:**
- Bump `XROS_CFLAGS` to `-O2` (keep `-g`; consider `-O3` for `kl_reproject.c`,
  `kl_audio.c`, `kl_opensl.c`, `kl_glfb.c` specifically if -O2 shows wins).
- Add a Release lane to `run.sh` (e.g. `KL_RELEASE=1 ./visionos/run.sh` selects
  `-configuration Release`), and give the generated project's Release config an
  explicit `GCC_OPTIMIZATION_LEVEL = s` (or 2) so it does not depend on Xcode
  defaults. Swift `-O` in Release is the default once the config isn't Debug.
- Measure first with the built-in gates: `make check` timings and an on-device
  `KL_SAMPLE_MS` capture before/after.

### X2 — MED — per-frame getenv scans on the Unity frame path
`runtime/guest/kl_driver.c:654` calls `kl_mem_pressure_poll()` per frame, which
reads `kl_env_int("KL_LOWMEM_AT")` and `kl_env_int("KL_LOWMEM_CLEAR")` **uncached**
(`runtime/libc/kl_libc.c:1123-1125`) — `kl_env_int` is a raw `getenv`+strtol
(`runtime/kl_env.c:64-68`), and `getenv` is a linear environ scan with strcmps.

Same shape in `kl_mprobe_tick` (`runtime/diag/kl_mprobe.c:396-414`): the
`KL_PROBE_STACKTRACE` / `KL_PROBE_RESOLVE` / `KL_PROBE_XR` reads are guarded by
`!st_done`-style latches that **only latch when the knob is set and the probe
ran**. With all probes off (the normal case), all three `getenv`s run every
frame for the entire session.

Also in the same call: `kl_proc_available()` → `os_proc_available_memory()` every
frame. That one is an OS query; a memory watermark does not need 90 Hz.

**Fix sketch:** latch all five knobs statically (`static int v = -1; if (v < 0)
v = kl_env_int(...)`), and rate-limit the headroom check to ~4 Hz via a
CLOCK_MONOTONIC last-poll stamp. Suggestion: add `kl_env_on_cached` /
`kl_env_int_cached` helpers in kl_env.c so the idiom is one line and the
"never latches when unset" bug class is closed by construction.

### X3 — MED — no LTO; runtime ships as one -all_load archive
`Makefile:783-799` compiles each `RUNTIME_SHIP` source to its own `.o` and `ar`s
them; the dylib gate links with `-Wl,-all_load`. Nothing crosses TU boundaries:
the guest→runtime call chain (stub cell → shim → host API) pays full call
overhead at every boundary, and hot small helpers (kl_env_*, table accessors,
lock helpers) are never inlined across files.

**Fix sketch:** `-flto=thin` on both `CFLAGS` and `XROS_CFLAGS` plus the archive
rules (ar handles LTO objects fine with clang's libLTO on the link). If LTO
fights the hand-written asm or the `__asm__` symbol games in the veneer code,
scope it to the plain-C cluster (jni, media, gfx minus the .S files).

### X4 — MED — serial full rebuilds in the xros/xrsim archive rules
`Makefile:783-799`: every header is a prerequisite of every object via the
`RUNTIME_ALL_HDRS` wildcard, and the build is a shell `for` loop — one `clang`
invocation at a time, no parallelism, no ccache. Any header touch rebuilds all
~60 sources serially. The Makefile comment (lines 715-721) already measures this
at ~15 s of a 30 s incremental `make xros`.

**Fix sketch:** generate per-file targets (`$(eval)` pattern rules) so make's
own `-j` applies, and/or add `ccache` when present. Keep the all-headers
prerequisite (it is deliberate — the comment at lines 69-86 explains the
missed-FAILURE history); just parallelize it.

### X5 — LOW — compositor per-frame logging/diagnostics residue
- `KleptonCompositor.swift:2049-2054`: the startup trace NSLogs every frame for
  the first 300 frames — precisely the window where the guest is compiling
  shaders and the process is most fragile.
- `:1471-1484`: every 0.5 s, `cadenceSummary()` + `stageSummary()` build
  multi-line strings and walk the stage table; `stageSummary` also takes
  `eyesLock.try()` per stage per eye and calls into `kl_ovrp_stage_render_pose`.
  Cheap, but on the render thread; could move to the heartbeat thread's cadence.
- `:2035-2045`: `addCompletedHandler` allocates a closure per command buffer
  per frame; unavoidable-ish for the counter, but the counter could be an
  atomic bump without the lock pair (`cmdLock` taken twice per frame +
  once per completion callback).

### X6 — LOW (verified fine, do not "fix") — diagnostics are correctly gated
Checked so these don't get "optimized" by mistake:
- `kl_sample.c` sampler thread: only starts with `KL_SAMPLE_MS` set
  (`runtime/guest/kl_driver.c:452-465`, `mains/m_boot.c:612-620`).
- `kl_gl_trace.S` / `kl_gl_lock.S`: only armed by `KL_GLFB_TRACE=1` /
  `KL_GLFB_LOCK=1`; the trampoline cost (~224-byte register save + C call per GL
  call) is not in the default path. The `kl_env.c:44` comment documents the
  trace mode's ~1 GB/session log volume.
- `kl_app.c` heartbeat: 250 ms thread doing `task_info` + one fprintf every 2 s;
  negligible, and load-bearing on device (it is the only "killed vs suspended vs
  stuck" witness).
- `kl_image.c` linear strcmp scans over stub/missing/weak/interpose tables are
  **load-time only**, and on device the dyld path (klepton-ld translations)
  bypasses the mmap loader entirely. Not worth hashing.
- `kl_env_on` is uncached, but the codebase is disciplined: 74 uses in
  kl_glfb.c are nearly all behind `static int x = -1; if (x < 0)` latches.
  The exceptions found are X2's.

### X7 — LOW — SwiftUI/app layer runs in Debug on the device loop
`visionos/run.sh` never offers a Release build of the app. The launcher UI
(`KleptonLauncher.swift`, `KleptonShell.swift`) and `KleptonTuning.swift` are
-SwiftUI at `-Onone` on device; mostly cold, but `KleptonControllers.swift`'s
per-frame merge and `KleptonCompositor.swift`'s `renderFrame` are not cold.
Same fix as X1's Release lane.

### X11 — MED — x18 veneers repeat `mrs tpidrro_el0` per site; no per-function hoisting
`runtime/kl_x18.c:343-365` — every x18 site becomes its own veneer:
`stp; mrs tp; ldr; <insn>; str?; ldp; b` (6 instr read-only, 7 read-write).
libunity alone has 14,536 sites (`make check` prints them), and hot guest code
hits these constantly. The `mrs` + the spill/reload pair are per-site overhead
that a per-function (or per-run-of-adjacent-sites) shared veneer could pay once:
spill once at the first site of a straight-line run, keep the TSD base in a
scratch register across the run, restore at the end. This is a klepton-ld
emission change (`tools/klepton_ld.c:395-412` is the emit loop), not a runtime
change. **Measure first**: count dynamic hits per site (the sampler at
KL_SAMPLE_MS=10 will show veneer addresses as guest frames) — if the live hot
set is small, this is not worth the decoder risk; if render-loop functions
cluster sites, it could be a few percent of guest CPU.

### X10 — MED — every ovrp_* entry pays a mutex + O(n) strcmp scan
`runtime/xr/kl_ovrp.c:44-56` (`ovrp_slot`): every instrumented OVRPlugin entry
point (146 `ovrp_hit` call sites; Unity calls dozens of them per frame — pose
queries per node, controller state, frame begin/end) locks `g_ovrp_mu` and
linear-scans `g_ovrp[512]` with strcmp, just to bump a per-name call counter.
On top of that, `ovrp_log_arg` (`:215-231`) takes ANOTHER mutex and scans up to
128 entries per call *forever* (its scan never short-circuits once saturated).
Fix: bake the slot index into the per-name stub at registration time (the entry
name is a compile-time constant at every call site — a per-callsite
`static int slot = -1` or a slot word in the stub descriptor removes both the
lock and the scan), and give `ovrp_log_arg` an early-out atomic (`if
(saturated) return;` before the lock).

### X9 — LOW — uncached `getenv("KL_GLFB_OUT")` on every eglSwapBuffers
`runtime/gfx/kl_egl.c:1672` — `const char *out = kl_env_str("KL_GLFB_OUT", NULL)`
runs per swap (once per frame), and it is NOT behind the `static int = -1` latch
idiom the rest of the file uses (`KLEGL_TRACE` at :1210 is latched; this one is
not). One linear environ scan per frame, forever, to answer a knob that is set
at launch. Fix: latch it (`static const char *out; static int read; if (!read)
{ read = 1; out = kl_env_str(...); }`). Adjacent same-shape instances the
reproject agent found (`kl_reproject.c:105-110`, `:942/:949/:968`) confirm the
pattern; a `kl_env_str_cached` helper in kl_env.h would close the class.

### X8 — LOW — boot time on device is "tens of seconds"; contributors not yet profiled
kl_app.c and KleptonCompositor comments repeatedly note device boot takes tens
of seconds (guest startup, OBB mmap, shader warmup, il2cpp init). Nobody has a
breakdown. The runtime already has the instrument (`KL_SAMPLE_MS` works on
device since `kl_driver.c:452`), plus the boot-log phase heartbeat. A one-time
sampling run at boot to attribute those seconds (dyld map vs init arrays vs
ANGLE pipeline-cache vs il2cpp metadata parse vs OBB decompress) would tell us
whether any of it is ours to fix. Suggest adding `KL_SAMPLE_MS=100` during one
device boot and archiving the profile next to the boot log.
