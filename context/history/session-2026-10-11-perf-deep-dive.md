# Session: Klepton performance deep-dive — 2026-10-10/11

## What was done
Full-repo performance review of the Klepton Android-VR-on-Apple-Silicon
runtime (~107K lines C/ObjC/asm), goal: "deep dive ... keep a lot of
findings."

Six subagents were dispatched (gfx/reproj+xr/trans/jni/media); five died to
429 rate limits without writing (one, reproj+Vulkan, delivered a full file).
The lead then completed all remaining domains first-hand.

## Outputs (all kept in scratch/perf/)
- 00-MASTER.md — ranked top-12, themes, order of attack
- build-app-driver.md — build flags (-O1 runtime! Debug app!), env latching, veneers
- reproject-vulkan.md — agent: QueueWaitIdle/frame, linear handle tables, dlsym/draw
- gfx-present.md — per-draw glGetIntegerv + snprintf, vertex re-apply, texture tables
- xr-layer.md — ovrp_slot mutex+scan/call, log_arg scan, capture-layer getenv
- translation-layer.md — kl_sym/kl_shim_lookup linear scans, pthread waiter CAS array
- jni-assets.md — binding re-resolution ~614 rows/call (top JNI win), strings zero-copy
- media-streaming.md — audio resampler branchy C, aaudio float loop; vtdec verified zero-copy

## Key conclusions
1. Biggest single lever: build config (runtime -O1; app Debug/-Onone on device).
2. Most hot-path costs are ungated diagnostic bookkeeping (draw census, ovrp
   counter, mutex waiter table, cond clock, env reads per frame).
3. Data structures are right (seqlock pose seam, SPSC audio ring, zero-copy
   IOSurface video, amplified foveated composite) — dispatch/lookup overhead
   and pipelining are where the wins are.
4. No benchmarks exist; add microbenches + KL_SAMPLE_MS baseline before changing.

## Phase 2 (same day): benchmarks + measured impact
Goal: "write benchmarks and test the impact of these changes?"

- Wrote `tests/t_perf.c` (+ `make perf`, `make perf-O2`), a standalone audio
  fast-path equivalence check, and `scratch/o2_gates.sh` (whole runtime at -O2,
  all guest-free gates).
- Implemented and measured: shim-lookup hash (63x/9.2x), JNI binding cache
  (29x), ovrp slot hash (2.7x), pthread waiter-census gating (2.2x contended),
  audio convert fast path (2.2x; 5.6x at -O2), plus per-frame env latching.
- Verified: 9 guest-free gates pass at -O1; 8 pass rebuilt at -O2; audio fast
  path bit-identical over 500 random buffers; shim index self-check clean.
- Fixed two pre-existing bare-checkout build bugs found on the way (kl_vulkan
  stub missing two entry points; LDLIBS missing -framework CoreAudio).
- Commit `perf: benchmark suite, and the measured hot-path fixes it found` on
  branch `perf/hotpath-benchmarks` (main untouched; nothing pushed).
- Numbers + caveats: `scratch/perf/PERF-BENCH.md`.

## Issues encountered
- Subagent Write calls did not land (5/6 agents rate-limited); v2 dispatch with
  skeleton-first writes also rate-limited. Completed work personally.
- No guest trees (beatsaber/, steamlink-*/) staged on this machine and no
  vendor ANGLE/MoltenVK: benches use runtime entry points directly; kl_sym's
  ELF-path fix and the Vulkan/gfx items remain open for a machine with guests.

