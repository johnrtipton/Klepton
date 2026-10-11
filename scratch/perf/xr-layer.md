# Perf findings: XR layer (kl_ovrp, kl_openxr, kl_ovrplat)

Scope read (lead): `runtime/xr/kl_ovrp.c` (hit/slot instrumentation, pose
query, pose latch, frame records), `runtime/xr/kl_openxr.c` (WaitFrame /
BeginFrame / EndFrame / LocateViews / LocateSpace), `mains/m_boot.c` (pump),
`runtime/xr/kl_ovrp_sret.S` (skimmed via the file list).

## Top findings

1. **MED — `ovrp_slot`: a global mutex + linear strcmp scan over up to 512
   registered names, on EVERY ovrp_* entry point call**
   (`runtime/xr/kl_ovrp.c:44-56`). Every instrumented entry (146 `ovrp_hit`
   call sites; a Unity frame makes dozens of ovrp calls — GetNodePoseState per
   node, controller state, frame begin/end, capability polls) pays
   lock + O(n) strcmp scan just to bump a per-name counter. **Fix:** the entry
   name is a compile-time constant at every call site — bake the slot index
   into the per-name stub at registration time (a slot word in the stub
   descriptor or `static int slot` per call site), or hash the name table
   once. The call counter is exactly the kind of thing that must cost nothing
   when nobody reads it.

2. **MED — `ovrp_log_arg`: second mutex + scan-to-128 per call, forever**
   (`runtime/xr/kl_ovrp.c:215-231`). Called alongside ovrp_hit from hot
   entries (GetNodePoseState :3426). The seen[] table saturates at 128 and the
   lock+scan still runs on every call for the rest of the session.
   **Fix:** an early-out `if (nseen >= 128) return;` before the lock, or gate
   the whole thing behind KL_OVRP_VERBOSE.

3. **LOW — uncached `kl_env_int("KL_XR_CAPTURE_LAYER")` per xrEndFrame**
   (`runtime/xr/kl_openxr.c:5334`) — one environ scan per frame on the OpenXR
   path. Every other knob in this file's frame path is properly latched
   (`KL_GLFB_DRAW_CENSUS` :5305, `KL_XR_FOVEA` :5420-5423, `KL_XR_LAYERS`
   :5437-5439). Latch it.

4. **LOW — the EndFrame layer-selection pre-pass re-walks the layer array
   twice** (`kl_openxr.c:5343-5410`): one pass to count/measure spans, a
   second to pick. Layer counts are ≤8; this is noise — listed only because
   the two loops could merge trivially if anyone touches the function.

## Verified clean (do not "fix")

- **Pose queries are step-latched and cheap** (`kl_ovrp.c:3424-3505`):
  GetNodePoseState reads the pose the guest sampled at ovrp_Update2 — no live
  re-read, no lock; the eye-offset math is one qrot per query.
- **The pose handoff is a bounded seqlock** (`kl_ovrp.c:1625-1640`):
  `klovrp_pose_read` — 8 tries then unsynchronised take, one writer thread.
  Exactly right for the compositor→guest seam.
- **kl_ovrp_frame_latch** (`:1876-`) is knob-gated, does three seqlock reads
  and a handful of quaternion ops per frame.
- **xrWaitFrame/xrBeginFrame** (`kl_openxr.c:5103-5166`): pacer call + latch
  + a few stores; `KLXR_TRACE` is latched off (:2115-2123);
  `klxr_log_chain` early-outs on next==NULL (:1067-1074).
- **xrLocateViews / xrLocateSpace** (:6244-, :6326-): small constant math,
  no locks, no allocation; the pose-agreement instrumentation
  (`klxr_pose_note` :5204) early-outs unless KL_XR_POSE_TRACE.
- The frontend→runtime publish (`kl_app_guest_publish`) is one mutex+cond
  signal per frame in kl_app.c — fine at 90 Hz.

## Not swept in person
- `kl_ovrplat.c` (Oculus platform shim) — mostly one-shot calls; low traffic.
- The haptics queue (`kl_ovrp_haptics_pull`) — pulled per frame by drivers,
  small fixed work per the t_haptics design.
- `kl_ovrp_sret.S` — the sret bounce thunk: one extra branch per struct-return
  entry; the ovrpabi gate (tools/ovrp_abi.py) checks its correctness, not its
  cost. Candidate for a direct-return variant only if profiling shows the
  bounce in hot samples.
