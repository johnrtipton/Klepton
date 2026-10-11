# Beat Saber 1.40.8 comparison builds

Prepared on 2026-10-11 for a later headset session at the user's request.
Neither comparison app was installed or launched. Existing installed Beat
Saber and its container were preserved.

## Frozen build pair

- Baseline source: 16c2f1e52152f6983e9f640d274ab0e9487792f1.
- Optimized source: 91b45d5f95f21b865d5fd4892ec365c9f05c7aac.
- Both: Release configuration, shipping C runtime O2, frontend Swift O.
- Identical frontend sources and all 23 freshly translated guest libraries,
  checked byte-for-byte via SHA-256. Same ANGLE xcframeworks in both builds.
- Asset metadata: 1.40.8_7379, version code1716. Source APK and 25 ELF libraries
  are hashed in the manifest. The existing fixture includes mod libraries;
  this is a comparison of that exact fixture, not a stock/unmodded-game claim.
- Both isolated source copies add the same beatsaber140 target configuration
  pointing to this fixture. No performance/runtime patch besides the graphics
  follow-up differs between variants. The build manifests identify these local
  target additions; the app binaries are not pristine Git-commit-only builds.
- Distinct IDs: tip.dev.klepton.perf.beatsaber140.baseline and
  tip.dev.klepton.perf.beatsaber140.optimized. Both apps compile successfully,
  are development signed, pass deep/strict signature verification and have
  device provisioning plus increased-memory-limit/extended-virtual-addressing.
  Profiles expire 2027-10-11. Signature verification requires normal macOS trust
  service access; its sandbox failure was resolved by verification outside it.

## Local artifacts

Under scratch/review/beat-saber-ab/:

- baseline/visionos/build/perf-release/Build/Products/Release-xros/Klepton.app
- optimized/visionos/build/perf-release/Build/Products/Release-xros/Klepton.app
- manifest.json: source/input hashes, frontend/translation equality, IDs,
  signing and provisioning checks.
- stage-variant.sh: explicit later install/staging command for one selected
  variant. Uses only that variant's separate app container. Has not been run.
- data/obb/main.1716.com.beatgames.beatsaber.obb: game data copied read-only from
  the existing installed game's Documents/android-files/obb directory. SHA-256
  recorded; all ZIP-entry CRC checks pass.

Artifacts, scripts and data are local scratch files, excluded from the PR.
Full logs are in context/terminal/beat-saber-*.log. Preparation hit Xcode cache
sandbox restrictions; both complete builds passed after granting cache access.

## Later run

Stage one variant with `bash stage-variant.sh baseline DEVICE_ID`, then the
other with `optimized`. Installing can rotate a test app's container, so stage
after each installation. Close all other Klepton game instances before running
one variant. Wear the headset, use the same settings/level/modifiers/controller
and audio route, warm caches, and follow GAMEPLAY-VALIDATION.md's alternating
A/B protocol. Capture CPU/GPU traces, cadence, pose age, memory and audio results.

No actual guest startup, rendering, gameplay correctness, device frame-time or
FPS result is established by preparing/signing these builds. Shader cache warmup
and runtime launch settings still need to be matched during the real session.
