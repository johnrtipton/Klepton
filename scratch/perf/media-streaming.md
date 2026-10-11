# Perf findings: audio + video decode + streaming

Scope read (lead): `runtime/media/kl_audio.c` (ring, converter, write paths),
`kl_opensl.c` (feeder, player lifecycle), `kl_aaudio.c` (feeder threads),
`kl_vtdec.c` (VideoToolbox session, output format, NAL handling),
`kl_mediandk.c` (AImage/AHardwareBuffer handoffs), `kl_libc_slink.c` (epoll —
also in translation-layer.md), `kl_avdec.m` (skimmed), phonon HRTF (noted).

## Top findings

1. **MED — the guest-rate→device-rate resampler is per-sample branchy C**
   (`runtime/media/kl_audio.c:566-613`, `convert`). Runs per buffer on every
   feeder thread; every output frame pays `floor()`, a `pos` double add, and a
   per-channel `FETCH` macro with two clamps. The overwhelmingly common case
   (guest 48 kHz == device 48 kHz) degenerates to "a straight copy" through
   the same loop. **Fix:** special-case ratio==1.0 && stereo with
   `vDSP_vflt16(src, 1, dst, 1, n*2)` (one call, no branches); for the
   resampling case keep the loop but hoist the clamps (they are loop-invariant
   per chunk: only `i` and `i+1` are ever read, so clamp once to `n-1`) and
   track the integer part incrementally instead of `floor(pos)` per frame.

2. **MED-LOW — AAudio float→int16 conversion is a per-sample C loop**
   (`runtime/media/kl_aaudio.c:203-211`): clamp + `lrintf` per sample at
   96K samples/s per float stream (Steam Link VR runs two). **Fix:**
   `vDSP_vclip` + `vDSP_vfixri16`, or a branch-free `x*32767.0f + 0.5f`
   saturating convert; also fold into a vDSP pair the OpenSL path's own
   int16→float staging in `convert`.

3. **LOW — peak meter walks every sample of every buffer**
   (`runtime/media/kl_audio.c:754-761`): the abs-max loop per write is
   diagnostic (g_peak / silent-buffer counting). `vDSP_maxmgvi` (or a 4-way
   NEON max-abs) replaces it; or skip the loop when neither the report nor the
   watchdog is armed.

4. **LOW — per-burst small allocations are absent (good), but AImage
   handoff callocs per frame** (`runtime/media/kl_mediandk.c:459-465`):
   one 24-byte calloc per decoded frame — negligible at 30-90 fps; a freelist
   only if a profiler ever shows it.

## Verified clean (do not "fix")

- **The render callback is genuinely RT-safe** (`kl_audio.c:219-245`): "a
  load, a memcpy and a store" — SPSC ring with monotonic counters, no locks,
  no allocation, no logging on the CoreAudio thread. Producer blocks by
  polling (documented reason at :11-15). This is the right shape.
- **Video decode is zero-copy and native-format**: vtdec requests the
  biplanar native format rather than BGRA (2.67× fewer bytes, `kl_vtdec.c:265-290`),
  sets Metal+IOSurface compatibility keys, and keeps a CFRetain'd ring
  (`:174-188`). The compositor samples via a once-created CVMetalTextureCache
  (`kl_cvmtl.m:17-38`, per the reproject agent's file). No CPU color convert
  anywhere on the video path.
- **OpenSL feeder** (`kl_opensl.c:304-394`): one thread per player at
  USER_INTERACTIVE QoS, device-paced writes, careful generation re-check
  around callbacks. Per-buffer cost is one mutex cycle + ring write.
- **kl_audio_write_src's mixing** (`kl_audio.c:748-…`): single-producer fast
  path is lock-free against the reader; the producer mutex (g_wlock) only
  serialises the rare multi-source case (two Steam Link streams).
- **NAL parameter-set dedup** (`kl_vtdec.c:469-486`) runs per access unit on
  tiny buffers — fine.
- **The kl_audio watchdog/heartbeat** is on its own thread, not the render
  callback.

## Notes / follow-ups not fully swept
- `runtime/media/kl_avdec.m` (AVPlayer loading videos) — skimmed only; runs
  cold (loading screens) and hands frames through the same CVMetalTextureCache.
- **Phonon HRTF**: `kl_phonon_hrtf.S` is a data blob (the CIPIC SOFA .incbin'd
  for iplHRTFCreate) — the convolution itself runs inside Steam Audio's own
  `libphonon`, i.e. in *guest* code, so its per-sample cost is the guest's
  implementation; nothing host-side to optimize without replacing Steam Audio.
- Steam Link's network loop is the guest's own (libvrlink_scene); our shims
  (recv/epoll) are the only host-side surface — epoll_wait's per-call calloc
  is translation-layer.md finding 5.
