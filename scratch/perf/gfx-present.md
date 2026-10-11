# Perf findings: GLES present path (kl_glfb, kl_egl, kl_present, kl_view)

Scope read (lead, after the assigned agents were rate-limited):
`runtime/gfx/kl_glfb.c` (swap path, draw thunks, texture tables, capture
machinery), `runtime/gfx/kl_egl.c` (swap, EGLImage), `runtime/gfx/kl_present.c`
(all — tiny policy module), `runtime/gfx/kl_gl_lock.S` + `kl_gl_trace.S` (all).

## Top findings

1. **HIGH — ungated synchronous `glGetIntegerv` on EVERY draw call.**
   `klfb_note_draw` (`runtime/gfx/kl_glfb.c:5737-5783`) is called from every
   draw thunk (`:6141,6152,6168,6183,6210,6257,6298` — DrawElements/DrawArrays/
   DrawRangeElements etc.) and unconditionally issues
   `a_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING)` — a round trip into ANGLE's
   context lock per draw. At 1-3K draws/frame × 90 fps this is a real, constant
   CPU tax whose only consumers are the draw census (`kl_glfb_draw_census`,
   printed only under KL_GL_CENSUS / KL_GLFB_DRAW_CENSUS) and the blit timeline
   (gated by `klfb_timeline()`). **Fix:** early-out when all consumers are off
   (latch one `static int want = census||timeline` flag), or better: drop the GL
   query entirely — the file already tracks the binding in `g_draw_fb` from its
   own BindFramebuffer wrapper (note_draw's own fallback at :5745), which makes
   the query redundant in the common case. Keep the query only when the census
   is actually armed.

2. **MED — unconditional `snprintf` per DrawElements** (`kl_glfb.c:6145-6146`):
   builds the `detail` string for `klfb_errprobe("glDrawElements(after)", d)`
   even though errprobe is latched OFF by default (`:5853-5856`). One snprintf
   per draw × thousands of draws per frame. **Fix:** move the snprintf inside a
   `if (errprobe_on)` gate, or make errprobe0/errprobe macro-gated on the
   latched flag before formatting.

3. **MED — `klfb_apply_vertex_bindings` re-applies every attrib on every draw
   when the ES3.1-binding translation is active** (`kl_glfb.c:6098-6132`):
   `glGetIntegerv(ARRAY_BUFFER_BINDING)` + per-attrib `BindBuffer +
   AttribPointer + Divisor` + restore, on EVERY draw, even when nothing changed
   since the last draw on the same VAO. For Source-engine guests (hl2/portal)
   that draw with divisors this multiplies per-draw GL call count ~3-4×.
   **Fix:** dirty-check — bump a serial in the BindBuffer/VertexAttribBinding/
   Divisor thunks, remember the applied serial per VAO, skip when unchanged.
   (When no VAO translation is active it early-outs at :6100 — fine.)

4. **LOW — `klfb_tex_info` linear scan over up to 4096 entries under a mutex**
   (`kl_glfb.c:2724-2737`). Verified callers are cold (debug dump, capture,
   probe paths) or knob-gated (`klfb_CompressedTexSubImage2D:5679` only does the
   lookup under KL_GLFB_PROBE_CTE/TRACE_TEX). Not hot today, but the table is
   one feature away from being on a hot path — a small open-addressed hash by
   texture name would make it safe everywhere. Cheap insurance.

5. **LOW — uncached `kl_env_str("KL_GLFB_OUT")` per eglSwapBuffers**
   (`runtime/gfx/kl_egl.c:1672`) — one environ scan per frame, forever. Latch
   it. (Everything around it — KLEGL_TRACE :1210, KL_GL_CENSUS :1663,
   kl_glfb_enabled — is properly latched.)

## Verified clean (do not "fix")

- **The per-frame GPU fence signal is the right design**: `klfb_gpu_frame_now`
  (`kl_glfb.c:6768-6812`) = eglCreateSync(METAL_SHARED_EVENT) + glFlush, no
  glFinish, sync object destroyed one frame late. CPU never blocks.
- **`kl_glfb_present`** (`:7877-7907`): pthread_once + pthread_getspecific +
  early-outs; capture paths (PNG dump, FBO probe with glFinish) are debug-gated
  and off by default.
- **kl_present.c**: tiny policy module, no per-frame cost.
- **kl_gl_trace.S / kl_gl_lock.S**: the 224-byte register-save trampolines are
  opt-in (`KL_GLFB_TRACE`/`KL_GLFB_LOCK`) and off by default. `kl_env.c:36-46`
  documents why the trace family must never ride KL_FULL (22M lines/session).
- **klfb_note_fbo_color** (`:1970-1982`): 48-entry linear scan on FBO-attach
  calls — attach calls are rare relative to draws; fine.
- **klfb_CompressedTexSubImage2D** (`:5659-5707`): the block-geometry math is
  per-upload but trivial; the expensive part (tex_info lookup + GL query) is
  knob-gated.
- **klfb_note_tex_storage** (`:2699-2722`): mutex + scan per texture
  *allocation* — allocations are load-time-ish, not per-frame.

## Sweep gaps (file is 8.8K lines; these regions were not line-read)
- The mirror path (`KL_HL2_THREADED_MIRROR` ~:4456) — the spectator/2D mirror
  blit for Source guests.
- The eye FBO composite / stage machinery (:8400-8600 region partially read —
  capture-side only).
- `klfb_BlitFramebuffer` thunk and invalidate handling.
- `kl_glfb_image_from_pixels` (per decoded video frame EGLImage creation).
