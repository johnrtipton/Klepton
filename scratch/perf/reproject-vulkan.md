# Perf findings: reprojection path + Vulkan translation layer

Scope read in full: `runtime/gfx/kl_reproject.c/h`, `tests/t_reproject.m`,
`runtime/gfx/kl_vulkan.c` (all 6070 lines), `runtime/gfx/kl_cvmtl.m/h`,
`runtime/gfx/kl_present.c`; call sites verified in `runtime/xr/kl_ovrp.c`,
`runtime/xr/kl_openxr.c`, `visionos/Sources/KleptonCompositor.swift`;
spikes `s12_vrr.m`, `s11_draw.c`, `s11_mtltex.m` skimmed.

## Top 5

1. **`vkQueueWaitIdle` on the guest's queue at every frame end** —
   `runtime/gfx/kl_vulkan.c:2460` (`kl_vulkan_frame_done`, `KL_VK_FRAME_SYNC` default
   ON). Full CPU stall of the guest render thread once per frame; zero CPU/GPU
   pipelining. The file's own comment (2313–2335) names the fix: an MTLSharedEvent /
   timeline-semaphore signal instead of a wait.
2. **Linear-scan handle tables under a mutex on every image command** —
   `klvk_img_reg_has` (`kl_vulkan.c:1431`, up to 16384 entries), `klvk_view_image`
   (`:1416`, up to 8192), `klvk_img_info_get` (`:1820`, up to 4096). Hit per image
   command and **per barrier in every barrier array** (`KLVK_IMG_BAD`,
   `kl_vulkan.c:2650,2784`). Hundreds of barriers/frame × thousands of entries =
   milliseconds of CPU per frame. The repo already has the fix pattern
   (`klvk_hash64` + open addressing, `kl_vulkan.c:4573–4599`).
3. **2D-mirror freshness copies duplicated per descriptor bind** —
   `kl_vulkan.c:4287–4293` + `klvk_dset_gpu_copy` `:4827–4840`: every re-bind re-records
   2 barriers + 1–2 copies per mirror (up to 128 mirrors/bind), so a UE5 frame with
   dozens of binds carries hundreds of redundant GPU copies (olar/wanderer only).
4. **Per-frame `getenv` in the uniform builders** — `kl_reproject.c:105–110`
   (`KL_SRGB_DECODE`, per eye build), `:942/:949/:968` (`KL_OVERLAY_FLIP_X/FLIP_Y/OPAQUE`,
   per overlay per eye). 2–8 environ walks per frame; latch like `klr_nocant` does.
5. **General 4×4 `simd_inverse` on rigid transforms, twice per eye per frame** —
   `kl_reproject.c:856–858` (and `:963–964` for overlays). All inputs are
   rotation+translation; inverse is transpose + negate (~30 flops vs ~300), or skip
   entirely when the pose is unchanged.

---

## Findings

### F1 — HIGH — `vkQueueWaitIdle` every frame kills frame pipelining
`runtime/gfx/kl_vulkan.c:2448–2472`, default `KL_VK_FRAME_SYNC=1`:

```c
if (kl_env_on("KL_VK_FRAME_SYNC", 1) && d && d->queue) {
    VkResult r = d->QueueWaitIdle ? d->QueueWaitIdle(d->queue)
               : d->DeviceWaitIdle ? d->DeviceWaitIdle(d->dev)
                                   : VK_SUCCESS;
```

Called once per frame from `kl_ovrp.c:2747` (EndFrame4) and `kl_openxr.c:6101`
(xrEndFrame). The guest render thread blocks until the GPU drains the whole queue
before it may build/submit the next frame — the exact "waiting on last frame's fence
before building next frame's commands" anti-pattern, but worse (queue drain, not one
fence). It exists to hand the compositor a serial that means "picture is COMPLETE"
(the design comment at `kl_vulkan.c:2313–2335` says so, and names the shape the fix
takes).

**Fix sketch** (matches the GL path's proven `klfb_gpu_frame_now` mechanism):
- At frame_done, submit an **empty submit on the guest's queue that signals a
  VkSemaphore created with `VkExportMetalObjectEXT` → MTLSharedEvent** (the device
  already enables `VK_EXT_metal_objects`, added at `kl_vulkan.c:614`). Publish the
  MTLSharedEvent next to `g_frame_serial`; the compositor encodes a wait on the event
  value before sampling the eye texture. CPU never waits; the eye image stays
  "last complete frame" by construction.
- Alternative without new API: keep the wait but move it off the guest thread —
  the compositor-side gate. Either way `KL_VK_FRAME_SYNC=1`'s default should become
  the event path once landed, keeping the wait as the fallback A/B.
- Expected win: removes one full GPU drain from the critical path per frame
  (frame time today is max(guest render, queue drain) + compositor; after, it is
  pipelined), and removes the corresponding presentation latency of up to a full
  frame.

### F2 — HIGH — linear handle tables under mutex on the command path
Three tables, all scanned linearly under `pthread_mutex_t` per lookup:

- `g_img_reg` — `kl_vulkan.c:1355–1439`, `KLVK_IMG_REG_MAX 16384`,
  `klvk_img_reg_has` at `:1431`. Fail-open when full (immediate `return 1`), so the
  *cost* is the scan while below capacity — and capacity is large: the file itself
  measures Unity titles at "olar's ~500–2000 ASTC images" (`:2821`), plus every
  render target and view.
- `g_view_map` — `kl_vulkan.c:1407–1422`, 8192 entries, reverse linear scan.
- `g_img_info` — `kl_vulkan.c:1809–1838`, 4096 entries, linear scan.

Call frequency on the hot path:
- `KLVK_IMG_BAD(img)` = `img && !klvk_img_reg_ok(img)` (`:2650`) → `klvk_img_reg_has`.
  Used in `klvk_CmdBlitImage` `:2656`, `klvk_CmdResolveImage` `:2669`,
  `klvk_CmdClearColorImage` `:2681`, `klvk_CmdCopyBufferToImage` `:2692`,
  `klvk_CmdCopyImageToBuffer` `:2768`, `klvk_CmdCopyImage` `:2531` (twice: src and dst),
  and — the expensive one — **`klvk_CmdPipelineBarrier` `:2784` calls it once per
  barrier in the array** (and twice more per bad barrier in the rewrite loop `:2798`).
  A Unity/UE frame records hundreds of barriers with multiple image barriers each.
- `klvk_view_image` is called per descriptor image write when emu/probe is on
  (`klvk_probe_img_binding`, `:5354`, from `klvk_UpdateDescriptorSets` `:5370–5376`
  and `klvk_tmpl_transform` `:5547–5555`) — per-frame descriptor writes × reverse
  scan of up to 8192 views.

**Fix sketch:** replace all three with the open-addressed hash pattern already in
this file (`klvk_hash64` at `:4573`, `klvk_bvmap` at `:4570–4599`): key = the handle
itself, hashed; O(1) expected. Keep the mutex (guest worker threads create
concurrently) but hold it for one bucket probe instead of a 16K scan. Lowest-risk
version: hash only `g_img_reg` (the hottest), leave the cold tables alone.

### F3 — MED-HIGH — mirror freshness copies re-recorded on every bind (olar/wanderer)
`kl_vulkan.c:4287–4293`:

```c
if (emu_on() && sets) {
    int seen[KLVK_BIND_SEEN]; int nseen = 0;
    ... klvk_dset_gpu_copy(cb, sets[i], seen, &nseen);
```

`klvk_dset_gpu_copy` (`:4827–4840`) records `klvk_mirror_gpu_copy` (`:4691–4775`) per
mirror per bind — that is **2 `vkCmdPipelineBarrier` + 1–2 `vkCmdCopyBufferToImage`
per mirror, deduplicated only within one bind** ("a later re-bind copies again
(freshness)"). UE5 re-binds the same sets every draw batch; with dozens of binds per
frame and up to `KLVK_BIND_SEEN` 128 mirrors per bind, the guest's command buffer
carries hundreds of redundant copies and twice as many barriers, all transferring the
same unchanged pool bytes into the same mirror images, every frame.

**Fix sketch:**
1. Dedupe **per submit** instead of per bind: tag the command buffer (the file
   already has `g_cb_idx`, `:4149–4164` — same trick) with a mirror bitmask cleared
   at `klvk_BeginCommandBuffer` (`:4185`), or simpler: keep a per-mirror
   `lastCopiedSubmit` counter bumped in `klvk_QueueSubmit` and skip the copy when
   equal.
2. Better: copy only when dirty. `klvk_CmdCopyBuffer`/`klvk_CmdUpdateBuffer`
   (`:4393/:4411`) already see every write to the pool; mark the mirror (or the pool
   buffer) dirty there and copy only dirty mirrors at the next bind.
3. Cheapest of all: the whole pool the views window into is usually re-uploaded once
   per frame in one copy — one copy per pool per frame at the first bind covers every
   mirror in it.

### F4 — MED — per-frame `getenv` in the reproject/overlay builders
`runtime/gfx/kl_reproject.c`:
- `klr_srgb_forced` `:105–110` calls `getenv("KL_SRGB_DECODE")` on **every**
  `kl_reproject_srgb_decode()` `:121–125`, which `kl_reproject_build` calls per eye
  per frame (`:736`), `kl_proj_layer_build` inherits (`:887`), `kl_overlay_build`
  calls again (`:965`).
- `kl_overlay_build` `:942`, `:949`, `:968`: `kl_env_on("KL_OVERLAY_FLIP_X" ... /
  "KL_OVERLAY_FLIP_Y" ... / "KL_OVERLAY_OPAQUE" ...)` per overlay per eye per frame.
  `kl_env_on` is a raw `getenv` every call (runtime/kl_env.c:32–40) — an environ
  linear walk with strcmps.

That is 2–8 environ walks per frame on the compositor thread. Not huge, but free to
remove and it sits on the per-eye build.

**Fix sketch:** latch exactly like `klr_nocant`/`klr_translate` in the same file
(`:26–30`, `:84–88`): `static int on = -1; if (on < 0) on = kl_env_on(...); return on;`
The one caveat: the file's comment says FLIP_X/FLIP_Y are "promoted per-target once
confirmed" knobs — if mid-session flipping is wanted, read them once per N frames or
keep `KL_REPROJECT_NOCANT`-style latching and drop the live-toggle ability
(deliberately).

### F5 — LOW — general matrix inverses on rigid transforms, per eye per frame
`kl_reproject.c:856–858`:

```c
simd_float4x4 chain = simd_mul(simd_mul(simd_inverse(render_m), device_m),
                               view_rot);
u.model_view = simd_inverse(chain);
```

`render_m` and `device_m` are rotation+translation (rigid), `view_rot` pure rotation;
`chain` is rigid. Two general 4×4 `simd_inverse` (~300 flops each incl. branches)
could each be a rigid inverse: rotation transpose + negated translation (~30 flops).
Same shape in `kl_overlay_build` `:963–964`. Per frame today: 2 eye builds + per-layer
builds + 2 overlay builds ≈ 4–10 general inverses, order microseconds on the
compositor thread.

**Fix sketch:** a `klr_rigid_inverse(simd_float4x4)` helper (transpose 3×3, negate
column 3 through it); use it for both inverses in `kl_reproject_build`. Optionally,
skip the rebuild entirely when `(rendered->serial, origin_from_device,
device_from_view)` are bit-identical to the last build for that eye — a static head
or a repeated pose is common in menus/paused games, and the uniforms are then
unchanged by construction (the test suite already pins that the pass reduces to a
blit when nothing moves, so a cache hit is safe to assert in t_reproject too).

### F6 — MED — `dlsym` per draw call (intotheradius guard)
`kl_vulkan.c:4179–4181`:

```c
static int klvk_have_buf_mtl(void) {
    return mvk_sym("vkGetMTLBufferMVK") != NULL;
}
```

`klvk_have_buf_mtl()` is called per `vkCmdDrawIndexed` when the guard is on
(`:4241`, default ON for intotheradius) — `mvk_sym` is `dlsym`, which takes the dyld
lock, per draw. Tens of thousands of draws per frame × dlsym is measurable CPU plus
lock contention with any concurrent dlopen/dlsym.

**Fix sketch:** `static int have = -1; if (have < 0) have = mvk_sym(...) != NULL;
return have;` — identical to the caching idiom used everywhere else in this file
(e.g. `klvk_buf_mtl` `:4169–4178` right above it already does this).

### F7 — MED-LOW — `vkQueuePresentKHR` per-present overhead
`kl_vulkan.c:2474–2508`:
- `kl_env_int("KL_VK_OUT_EVERY")` `:2478` + `cap_dir()`→`getenv("KL_VK_OUT")` `:2480`
  — two environ walks per present.
- `calloc`/`free` of the wait-stage array per present `:2491–2502`.
- An **extra empty `QueueSubmit`** per present just to consume the acquire wait
  semaphores when not capturing `:2489–2503` (required — a binary semaphore nobody
  waits on is a validation error — but it is a whole submit on the guest thread for
  zero work).

**Fix sketch:** latch the env reads (`static int every = -1; static const char *dir;`);
reuse a persistent stage array (count is bounded by the swapchain's semaphore count);
fold the semaphore consumption into the next host submit that already exists on this
path (the shadow pump's submit `:2018–2022` or the acquire-signal submit) by moving
the waits onto it, so present adds no submit of its own.

### F8 — LOW — `mem_type()` re-resolves and re-queries memory properties per allocation
`kl_vulkan.c:751–762`: resolves `vkGetPhysicalDeviceMemoryProperties` through
`real_gipa` and calls it on the physical device for **every** `mem_type` call — which
is every `klvk_image_alloc`, every swapchain image, every `cap_prepare` growth, every
shadow/mirror allocation. Allocation-time, not per-frame, but AC Nexus re-creates
overlay layers across loading phases (`:1549–1556`) so it recurs in steady state.

**Fix sketch:** cache `VkPhysicalDeviceMemoryProperties` per device in `klvk_device`
at `klvk_CreateDevice` (`:628–644`) and index it directly; also cache the resolved
PFN once (`static PFN ...`).

### F9 — LOW — capture path does CPU pixel work + a 2 s fence on the guest thread
`kl_vulkan.c:972–1083` (`cap_image_layer`): when `KL_VK_OUT` is set, per captured
frame — `WaitForFences(..., 2000000000ull)` `:1041` (blocks the guest render thread
until the copy retires), CPU BGRA→RGBA swap per pixel `:1053–1056`, alpha forcing
`:1061`, lit-pixel census `:1067–1071`, then zlib `compress()` + file IO in
`png_write_rgba` `:370–395`. Diagnostic-only, but it stalls the very frame loop being
measured, so any timing taken with captures on is polluted.

**Fix sketch (when it matters):** stage the PNG work on a background thread (map →
memcpy out → unmap, then compress off-thread), and use `VK_TRUE, 0` + skip if not
signalled for the fence when running under a timing run.

### F10 — LOW — eye-shadow copy serializes on the fence when enabled
`kl_vulkan.c:2366–2446` (`KL_EYE_SHADOW=1`): Reset/Begin/copy/End/submit then
`WaitForFences(..., 2 s)` `:2433` — the full copy latency lands on the guest thread
per frame. Default OFF and measured not to help olar (`:2452–2458`), so this is a
note: if it is ever turned on for a title, the wait is the thing to remove (same
MTLSharedEvent fix as F1 — the shadow copy is ours, so we own the event).

### F11 — LOW — equirect overlay does per-pixel trig + two mat4 muls in the fragment stage
`kl_reproject.c:555–581` (`kl_eq_f`): per fragment, `inv_proj * clip` (4×4 mul),
two normalizes, `atan2`, `acos` — full-screen, both eyes, during loading screens.
The projection is a per-view constant: the view ray can be computed **in the vertex
stage** at the 3 quad vertices and perspective-interpolated (pass the view-space
direction or position as a varying, normalize in the fragment stage), leaving only
atan2/acos per fragment — or a small 2D LUT texture indexed by (u,v) if even those
are wanted gone. Only matters while an equirect layer is live; LOW.

### F12 — LOW — sRGB decode `pow` per fragment (deliberate; note only)
`kl_reproject.c:158–163`: the spec-exact piecewise EOTF runs per fragment when
`srgbDecode` is set. The comment explicitly rejects approximating it because the
error lives where the pass's own artefacts live. Cheaper exact-ish options exist
(`pow(x, 2.4)` → `exp2(2.4 * log2(x))` is what the compiler emits anyway; a 256-entry
1D LUT texture or `float3` polynomial fit keeps near-black exact to <1/255) — but
this is a colour-correctness-sensitive trade the file has already settled. Record
only.

### F13 — LOW — Swift per-frame uniform array churn at the compositor call site
`visionos/Sources/KleptonCompositor.swift:2212–2214` (`var uniforms:
[kl_reproject_uniforms] = []`, `var sources: [MTLTexture?] = []`) — rebuilt and
grown per frame per encode. Two small heap allocations + copies of ~144-byte structs
per frame; noise next to F1–F5, but the fix is a preallocated
`contiguousArray`/`UnsafeMutableBufferPointer` reused across frames. (Adjacent to my
file set; flagged for whoever owns the compositor.)

### F14 — LOW — create-time log floods on Unity titles
`klvk_CreateImage` `:2937–2947` logs up to 4096 lines (every mip>1 image for the
first 4096), `klvk_CreateImageView` `:3033–3043` logs up to 4096 more, all via
fprintf to stderr at boot. Create-time only, but UE boots with thousands of both —
seconds of stderr traffic during warmup, and stderr on device is a pipe. Cap the
"first 64 + failures" (the failure log at `:2898–2926` is the part with value).

### Notes / non-findings (verified fine, do not "fix")
- **The unwarp grid is precomputed**, exactly as the header promises:
  `kl_reproject_grid_build_eye` (`kl_reproject.c:611–680`) runs at rate-map creation
  (ObjC `mapScreenToPhysicalCoordinates` per grid vertex, ≤ 64×64+1 vertices), not
  per frame. s12_vrr proved the mechanism and its exactness. No per-frame CPU
  foveation cost exists.
- **No CPU blits in the reproject path.** GL eyes are zero-copy MTLTextures via
  EGLImage (s11_mtltex); Vulkan eyes are MoltenVK's own MTLTextures exported via
  `vkExportMetalObjectsEXT` (`kl_vulkan.c:1283–1326`); the shadow copies are GPU
  `vkCmdCopyImage` (`:2014`), not memcpy. `kl_cvmtl.m` creates its
  CVMetalTextureCache once (`:17–38`) — the wrapper-per-frame pattern is already
  avoided.
- **`klvk_shadow_pump`** correctly early-outs when no shadow is registered
  (`:1938–1939`) and uses a non-blocking `WaitForFences(0)` skip when the previous
  copy is in flight (`:1962–1966`) — good shape, no change needed beyond the table
  lookups it inherits from F2.
- **`klvk_AcquireNextImageKHR`** `:886–894` pays one empty submit per acquire to
  signal the acquire semaphore. That is the only legal way to signal a binary
  semaphore host-side; not fixable, just noted as a fixed per-frame cost.
- **`t_reproject.m` has no benchmark.** It is entirely correctness (blit-identity,
  flip, slice, crop, chroma, sRGB, amplification). Before touching F5, a ~30-line
  host microbench (build N uniforms, time it) added to t_reproject would make the
  win (and any regression) measurable — the viewer is stated to be the place this
  math can run without a device.
- `g_presented++` in `klvk_QueuePresentKHR` `:2476` is a plain increment read
  cross-thread by `kl_vulkan_stats` — benign on this platform (one aligned word),
  same argument the file makes for its other scalars.

## Suggested order of attack
1. F1 (event instead of QueueWaitIdle) — largest latency/frame-time win, fix shape
   already documented in-file, GL path has the proven analogue.
2. F2 (hash the image registry) — mechanical, pattern exists in-file, benefits every
   Vulkan title every frame.
3. F3 (dedupe mirror copies per submit / dirty-tracking) — big GPU win on
   olar/wanderer specifically.
4. F4 + F5 + F6 (latch envs, rigid inverse, cache dlsym) — an afternoon, shaves the
   per-frame CPU tail; gate behind the t_reproject microbench.
5. F7 (present-path trim) once a WSI guest is the perf target.
