# Controlled gameplay comparison

Status: Beat Saber 1.40.8 selected; signed baseline/optimized Release builds
prepared for later at the user's request. No headset comparison has run.
Build identities and preparation evidence: BEATSABER-AB-2026-10-11.md.
Use a Vulkan game separately to assess the image registry and frame wait.

## Freeze the comparison

Compare commit `16c2f1e` (before this graphics follow-up) against the new graphics
commit, using identical guest APK/libraries/assets, frontend source, device,
release configuration, runtime O2, resolution, foveation, MSAA, refresh rate,
controller setup and audio route. Record all environment overrides. Do not use
`KL_VK_FRAME_SYNC=0` or `KL_CP_NOFENCE=1` as a performance configuration.
Use separate build/derived-data directories; do not replace the user's existing
working game checkout. Confirm actual installed build identity before each run.

## Run order and workload

Warm shader caches with the same level first. Capture three alternating A/B
pairs: 60 seconds in the same menu, the same 120-second level segment at the
same difficulty/modifiers, and the same level-load transition. Keep diagnostic
logging identical and draw census, readbacks and error probes off. Include a
separate diagnostic run if needed; never mix its timings into the default run.
A user must be wearing the headset and reproduce the workload for comparison.

## Record and decide

Use Instruments Time Profiler and Metal System Trace to record guest CPU work,
GPU duration and presentation behavior. Report median/p95/p99 frame time, not
just average FPS. Collect the existing `[cp]` cadence reports (held presents and
missed deadlines), `[cp] timewarp` pose age/stale runs, memory peak, audio
underruns and level-load duration. Inspect stereo image correctness, controller
latency and audio continuity. Retain raw captures and build/settings manifest.

Draw-wrapper microbenchmarks use fake GL dispatch and exclude driver/GPU work.
Registry microbenchmarks time a mutex and membership check, excluding Vulkan
calls and GPU work. Their gains establish less CPU work in those operations;
only this controlled device comparison can establish gameplay improvement.

## Vulkan queue-wait follow-up

The compositor currently uses `kl_vulkan_frame_serial()` after queue-idle; it
has no Vulkan event corresponding to GLES's ANGLE-signaled Metal shared event.
Measure `kl_vulkan_frame_done()` and its queue wait with the trace first. Any
replacement must order compositor sampling after guest rendering, protect eye
texture reuse/lifetime and correctly handle queue/device recreation. Test that
completion path under multiple in-flight frames and level transitions before
changing the default. Preserve the existing wait until then.
