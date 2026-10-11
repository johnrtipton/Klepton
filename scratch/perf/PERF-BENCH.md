# Perf benchmark results — the measured impact of the deep-dive changes

Date: 2026-10-11. Machine: Apple silicon (M-series), macOS 27 SDK, Homebrew clang 22.
Benchmarks: `tests/t_perf.c`, `make perf` / `make perf-O2`.
Method: best-of-5 rounds (best-of, not mean — cleanest estimator on a shared
laptop), `n = 200000` inner iterations, 3 runs averaged below.
All numbers are ns per operation, i.e. per guest call / per buffer.

## Results

| bench | before | after | change | what it measures |
|---|---:|---:|---:|---|
| `shim_hit`  | 715.3 | **11.4** | **63×** | `kl_shim_lookup("strlen")` — the shim table |
| `shim_miss` | 1542.2 | **168.2** | **9.2×** | the same for a name not in the table |
| `jni_call`  | 797.8 | **27.1** | **29×** | one `CallStaticLongMethod` (nanoTime) — binding re-resolution |
| `ovrp_get`  | 70.5 | **25.9** | **2.7×** | one `ovrp_GetNodePositionValid` with a realistic 50-name table |
| `mtx_contended(4t)` | 111.7 | **51.8** | **2.2×** | 4 threads × lock/unlock one guest mutex |
| `mtx_uncontended`  | 31.4 | **21.8** | **1.44×** | single-thread lock/unlock |
| `audio_convert` | 3436.2 | **1570.0** | **2.2×** | one 1024-frame stereo `kl_audio_write_src` |
| `env_hit` / `env_miss` | 129.1 / 100.3 | 127.1 / 98.0 | — | raw `getenv` (see note) |

`ovrp_get` before/after uses the realistic table in both runs (single-name
tables measure 14.9 ns and hide the scan — the bench resolved 50 entry points
first for both numbers above).

### What `-O2` buys (same benches, whole runtime rebuilt at -O2)

| bench | -O1 | -O2 |
|---|---:|---:|
| `audio_convert` | 1570.0 | **280.1** (5.6×) |
| everything else | unchanged within noise | |

So `-O2` is a large win exactly where the runtime does arithmetic on buffers
(audio conversion, and by extension any per-sample loop), and worth ~nothing
where a call is already bound by a `getenv`, a `strcmp` walk or a syscall.
Both are useful facts: it means the build-flag change is cheap to justify and
the dispatch work above is not made redundant by it.

### `-O2` correctness

The whole runtime was rebuilt at `-O2` and every guest-free gate re-run:
`t_variadic`, `t_haptics`, `t_softinput`, `t_xrspace`, `t_xrinput`, `t_ctr`,
`t_fault`, `t_bcast` — **all pass** (`scratch/o2_gates.sh` reproduces the
sweep). No behavioural difference observed; the gates that could catch an
optimizer-induced change (variadic ABI, sret thunks, veneer execution, crash
reporter) all pass.

## Per-frame consequence (why the per-call numbers matter)

A Unity frame on Beat Saber makes, conservatively: dozens of ovrp calls,
hundreds of JNI calls, thousands of `kl_shim_lookup` calls during load and
`dlsym`, and (on the frame path) the env reads converted here. At 90 Hz:

- **JNI dispatch**: 798 → 27 ns × ~300 calls/frame ≈ 0.23 ms/frame saved.
- **ovrp**: 70 → 26 ns × ~60 calls/frame ≈ 2.6 µs/frame.
- **shim**: dominated by load/dlsym volume — the host load and every level-load
  `dlopen`/`dlsym` burst (thousands of lookups) is the measured 63×/9×.
- **mutex**: the contended number is the one that matters — job-system threads
  hammering one lock at 112 → 52 ns; on the frame's critical path this is the
  difference between making and missing a deadline under load.
- **audio**: the converter runs per buffer per feeder; 2.2× (and 5.6× more at
  -O2) directly reduces the feeder's CPU and its scheduling pressure.
- **env**: each latch removes a ~100-127 ns environ walk; ~6 per frame (driver
  mem-poll ×2-3, mprobe ×3, egl swap, xr EndFrame, reproject ×2-8) → 0.6-1.1 µs
  per frame removed, plus the rate-limited `os_proc_available_memory()`.

## What changed (source)

| file | change |
|---|---|
| `tests/t_perf.c` (new) | the benchmark suite; `make perf`, `make perf-O2` |
| `Makefile` | the two perf targets; `-framework CoreAudio` (see below) |
| `runtime/libc/kl_shim.c` | open-addressed name→fn index over the 650-row table, built once, with a self-check that falls back to the linear scan |
| `runtime/kl_jni.c` + `jni/kl_jni_int.h` | the resolved host binding is cached on the interned jmethodID (`klj_wanted.b/b_cache`); `struct klj_binding` named |
| `runtime/xr/kl_ovrp.c` | open-addressed name→slot index for `ovrp_slot` (lock-free reads, insert under the existing mutex); `ovrp_log_arg` saturates early |
| `runtime/libc/kl_pthread.c` | the mutex/cond/sem WAITER census is now opt-in (`KL_TRACE_MTXWAIT=1`); the report says so; `last_sig_ns` clock read gated with it |
| `runtime/libc/kl_libc.c` | `kl_mem_pressure_poll` latches its knobs and samples the headroom at 4 Hz instead of every frame |
| `runtime/diag/kl_mprobe.c` | the three probe knobs latch (they never latched before — they re-read environ every frame for the whole session) |
| `runtime/gfx/kl_egl.c` | `KL_GLFB_OUT` latched (was per-swap) |
| `runtime/xr/kl_openxr.c` | `KL_XR_CAPTURE_LAYER` latched (was per-EndFrame) |
| `runtime/gfx/kl_reproject.c` | `KL_SRGB_DECODE` / `KL_OVERLAY_FLIP_X/Y/OPAQUE` latched (were per-eye/overlay per frame) |
| `runtime/media/kl_audio.c` | `convert()` fast path for ratio==1.0 stereo (bit-identical output) |
| `runtime/gfx/kl_vulkan.c` | two stub entries added (`kl_vulkan_display_luid`, `kl_vulkan_capture_layers`) — a bare checkout previously failed to LINK |

### Two build bugs found while doing this (both fixed, both pre-existing)
- `make build/t_variadic` did not link on a bare checkout: the no-MoltenVK
  stub of kl_vulkan.c was missing two entry points kl_ovrp.c/kl_glfb.c call.
- `-framework CoreAudio` was missing from `LDLIBS`; `AudioObjectGetPropertyData`
  (kl_audio.c) no longer resolves through AudioToolbox on this SDK.

## Behavioural caveats (deliberate, stated)

1. **`KL_TRACE_MTXWAIT` defaults OFF.** The hang report loses the *waiter*
   census (who is queued behind whom) but still names every lock HOLDER and its
   stacks; the report prints a line saying the census is disabled and how to
   turn it on. This is the one change with a diagnostic price, taken because
   the census cost was measured on every guest lock/cond/sem from every thread.
2. **`kl_mem_pressure_poll` samples at 4 Hz** — a memory watermark does not
   change at frame rate, and the dispatch memory-pressure source still
   announces real transitions immediately.
3. **The env latches** mean a knob changed with `setenv()` *after* its first
   read is ignored — the existing convention for every other latched knob in
   the runtime (and knobs are launch-time by design here).

## Not landed (still open, with fix sketches in the domain files)

- **`kl_sym` / `kl_shim_lookup` for the ELF path** — the shim table got its
  index (measured), but `kl_sym`'s per-image `.dynsym` linear scan and
  `kl_guest_sym_global`'s image×symbols search (translation-layer T1) are NOT
  changed: they need a real guest ELF to verify against, and no guest tree is
  staged on this machine. This is the host load-time / level-load item; it is
  also the riskiest (a wrong symbol answer is silent), so it wants the guest
  tree and `make check`'s import-count gates behind it.
- **Vulkan**: `vkQueueWaitIdle` per frame (reproject-vulkan F1), the handle-table
  scans (F2), mirror-copy dedup (F3), dlsym per draw (F6) — all need the
  MoltenVK vendor checkout and a Vulkan guest to measure; the reasoning and fix
  sketches stand.
- **gfx**: the per-draw `glGetIntegerv` (gfx-present G1) and the ES3.1 vertex
  re-apply (G3) are unmeasured here because ANGLE (`make angle-debug`, a ~12 GB
  checkout) is not present — the number that would justify them is a frame-time
  delta under a real guest, not a microbench.
- **Audio vectorization with Accelerate** (media M2/M3): measured only as the
  scalar fast path + the -O2 A/B above.

## Reproduce

```bash
make perf        # benchmarks, -O1 (the default build)
make perf-O2     # the same benchmarks with the runtime at -O2
bash scratch/o2_gates.sh   # every guest-free gate, built and run at -O2
```
