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
