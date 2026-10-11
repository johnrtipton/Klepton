## What this is

A performance pass over the runtime, with the benchmarks that justify it. It
started as a read of every hot path (findings kept in `scratch/perf/`: six
domain files plus a ranked master) and landed the items that could be measured
on this machine, along with the instrument to measure them.

## Benchmarks (new)

`tests/t_perf.c` — `make perf`, `make perf-O2`. Drives the runtime's own entry
points at guest-call granularity (env lookups, the shim table, the pthread
shim, an ovrp entry, JNI dispatch, the audio converter). No guest, no window,
no headset, so it runs on any checkout and works as a before/after pair.
Deliberately **not** in `make check`: its numbers are not a pass/fail.

## Measured impact

Best-of-5 rounds, n=200 000, 3-run averages. Full table and method in
`scratch/perf/PERF-BENCH.md`.

| bench | before | after | change | what it measures |
|---|---:|---:|---:|---|
| `shim_hit` | 715.3 ns | **11.4 ns** | **63×** | `kl_shim_lookup("strlen")` |
| `shim_miss` | 1542.2 ns | **168.2 ns** | **9.2×** | a name not in the table |
| `jni_call` | 797.8 ns | **27.1 ns** | **29×** | one `CallStaticLongMethod` |
| `ovrp_get` | 70.5 ns | **25.9 ns** | **2.7×** | one `ovrp_GetNodePositionValid` |
| `mtx_contended(4t)` | 111.7 ns | **51.8 ns** | **2.2×** | 4 threads × one guest mutex |
| `mtx_uncontended` | 31.4 ns | **21.8 ns** | **1.44×** | single-thread lock/unlock |
| `audio_convert` | 3436.2 ns | **1570.0 ns** | **2.2×** | one 1024-frame stereo write |

**`-O2` A/B** (whole runtime rebuilt): `audio_convert` 1570 → **280 ns (5.6×)**;
the dispatch benches unchanged within noise — they are bound by `getenv`,
`strcmp` walks and syscalls, not arithmetic. So the build-flag change and the
dispatch work are additive, not redundant.

## What changed

- **`runtime/libc/kl_shim.c`** — open-addressed name→fn index over the ~650-row
  shim table, built once on first use, with a self-check that discards the index
  and falls back to the linear scan if any row is not findable (correctness by
  construction; the linear path is still there).
- **`runtime/kl_jni.c` + `runtime/jni/kl_jni_int.h`** — the resolved host binding
  is cached on the interned jmethodID instead of re-walking every family table
  (two strcmps × ~614 rows, per superclass hop) on every `Call*Method`.
- **`runtime/xr/kl_ovrp.c`** — open-addressed name→slot index for `ovrp_slot`
  (lock-free reads, inserts under the existing mutex), and `ovrp_log_arg`
  saturates early instead of taking a mutex and scanning 128 entries per call
  forever.
- **`runtime/libc/kl_pthread.c`** — the mutex/cond/sem **waiter census** is now
  opt-in (`KL_TRACE_MTXWAIT=1`). It was a 64-slot CAS scan shared by every
  thread, on every guest lock/unlock, cond wait and sem wait.
- **`runtime/libc/kl_libc.c`** — `kl_mem_pressure_poll` latches its knobs and
  samples the jetsam headroom at 4 Hz instead of every frame.
- **`runtime/diag/kl_mprobe.c`** — the `KL_PROBE_STACKTRACE/RESOLVE/XR` reads
  latch. They previously never latched (the guard only closed if the probe
  actually ran), so with the knobs unset — the normal case — all three did an
  environ walk on every frame for the whole session.
- **Per-frame env reads latched**: `KL_GLFB_OUT` (was per `eglSwapBuffers`),
  `KL_XR_CAPTURE_LAYER` (was per `xrEndFrame`), and the reproject
  `KL_SRGB_DECODE` / `KL_OVERLAY_FLIP_X/Y/OPAQUE` (were per eye / per overlay
  per frame).
- **`runtime/media/kl_audio.c`** — `convert()` fast path for the ratio == 1.0
  stereo case (the common one), proved bit-identical over 500 random buffers
  plus the carry case (`scratch/audio_fastpath_check.c`).

## Two pre-existing build bugs fixed on the way

Both hit a bare checkout:

- `kl_vulkan.c`'s no-MoltenVK stub was missing two entry points that
  `kl_ovrp.c` / `kl_glfb.c` call (`kl_vulkan_display_luid`,
  `kl_vulkan_capture_layers`), so the gates **failed to link** without
  `make mvk`.
- `LDLIBS` lacked `-framework CoreAudio`; `AudioObjectGetPropertyData`
  (kl_audio.c) no longer resolves through AudioToolbox on this SDK.

## Verification

- All nine guest-free gates pass with the changes at `-O1`: `t_variadic`,
  `t_fault`, `t_haptics`, `t_softinput`, `t_xrspace`, `t_xrinput`, `t_trace`,
  `t_ctr`, `t_bcast` (plus `t_hevc`).
- The whole runtime rebuilt at **`-O2`** and every guest-free gate re-run:
  all pass (`scratch/o2_gates.sh`).
- Audio fast path bit-identical; shim index self-check clean (no fallback).

## Behavioural caveats (deliberate, stated)

1. `KL_TRACE_MTXWAIT` defaults **off**, so the hang report loses the *waiter*
   census — it still names every lock **holder** and their stacks, and prints a
   line saying the census is disabled and how to enable it. This is the one
   change with a diagnostic price, taken because the cost was measured on every
   guest lock from every thread. Documented in `DEBUG_ENV_VARS.md`.
2. The env latches mean a knob changed with `setenv()` after its first read is
   ignored — the existing convention for every other latched knob here.

## Not in this PR (sketches in `scratch/perf/`, need hardware this machine lacks)

`kl_sym`'s per-image `.dynsym` scan and `kl_guest_sym_global` (needs a staged
guest ELF to verify against); the Vulkan items (`vkQueueWaitIdle` per frame,
handle-table scans, dlsym per draw — need `make mvk` + a Vulkan guest); the
per-draw `glGetIntegerv` and ES3.1 vertex re-apply (need ANGLE + a frame-time
measurement under a guest).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
