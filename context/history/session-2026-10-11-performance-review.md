# Performance review — 2026-10-11

- Reviewed johnrtipton/Klepton PR #1 at head 4b68097 against ce74582.
- Created a separate clone in AVP Play's scratch/Klepton; branch
  codex/performance-review. Existing game development checkout left untouched.
- Fixed JNI lazy-cache race by resolving on interning, OVR index publication
  ordering, shim incompatible-struct aliasing and pthread latch race.
- Shipping C runtime now O2; Makefile invalidates platform archives. epoll
  uses a 64-event stack buffer. No measured epoll timing gain claimed.
- Added tests of actual audio conversion, concurrent OVR/JNI operation and
  epoll behavior. Added fail-fast check-host/check-host-O2; repaired o2 script.
- All 12 host gates pass at O1, O2 and with waiter tracing enabled. Runtime
  device/simulator slices compile and link with and without MoltenVK headers.
- Benchmark details and review findings in scratch/perf/REVIEW-2026-10-11.md.
  Evidence logs in context/terminal; temporary scripts/base checkout in
  scratch/review. Do not add those large scratch/log directories to Git.
- Staged Beat Saber libraries available in /Users/tip/avpplay-fix/beatsaber140;
  three guest libraries load without constructors. A physical Vision Pro is
  connected. No game was installed/launched; gameplay FPS is still unmeasured.
- Full compatibility suite and live gameplay/audio acceptance remain outside
  the guest-free results. Next: controlled headset scene comparison and profile
  Vulkan GPU synchronization, GLES diagnostics and per-image symbol lookup.

## Graphics continuation

- Gated GL per-draw census and error formatting; retained BLIT_PROBE's live-FBO
  tracking and documented opt-in diagnostics. Five modes added to check-host.
- Replaced Vulkan image registry scan with locked hash/back-shift index; kept
  duplicate/capacity/fail-open behavior. Differential + shared-handle stress pass.
- Three O2 comparisons vs 16c2f1e: DrawElements 121.04 ->15.54 ns,
  instanced 117.37 ->12.42 ns; DrawArrays unchanged. Registry at12K entries
  about2380 ->5.6 ns. These are CPU microbenchmarks, not headset FPS.
- O2 host suite passes (UDP bind required escalation); device/sim full Vulkan
  and fallback builds both compile/all-load link. Queue synchronization preserved.
- Detailed evidence: scratch/perf/GRAPHICS-2026-10-11.md. Controlled device
  protocol: scratch/perf/GAMEPLAY-VALIDATION.md. No headset game launched.

## Beat Saber comparison preparation

- User selected Beat Saber 1.40.8 and explicitly requested preparing builds for
  later. No test app installed or launched; no gameplay/FPS claim.
- Created isolated baseline16c2f1e/optimized91b45d5 source copies with identical
  local beatsaber140 target config, frontend, ANGLE and 23 fresh translations.
  APK/25 ELF hashes and exact source/build identities saved in scratch manifest.
- Both full Release app builds pass, C O2/Swift O. Both development signatures
  deep/strict verify; profiles carry memory capabilities and this physical
  device. New separate bundle IDs prevent replacing the installed game.
- Retrieved matching1.40.8 OBB read-only from installed app container into local
  scratch. Local hash and full ZIP CRC validation pass. Future staging script
  has syntax checked only; it has not installed/staged either variant.
- Artifacts and scripts: scratch/review/beat-saber-ab (untracked/local).
  Evidence and later-run instructions: scratch/perf/BEATSABER-AB-2026-10-11.md.
