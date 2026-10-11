# Perf findings: JNI layer + asset paths

Scope read: `runtime/kl_jni.c` (ref management, string conv, call dispatch),
`runtime/jni/kl_jni_looper.c` (Choreographer frame clock). (Written by the lead
after the assigned agent did not deliver; covers the dispatch and string cores
first-hand.)

## Top findings

1. **HIGH — every JNI `Call*Method` re-resolves its binding by scanning all
   ~614 rows of the binding tables with 2 strcmps per row, per superclass
   level.** `klj_call_common` (`runtime/kl_jni.c:1081`) → `klj_resolve_binding`
   (`:1022-1047`) → `klj_find_binding` (`:991-1002`) walks
   `klj_binding_tables[16]` (`:1994`) in full per call, plus a `g_supers` strcmp
   scan per hop (`:1042-1044`) and a final java/lang/Object fallback scan. Unity
   titles call JNI hundreds of times per frame (haptics, platform queries, view
   plumbing). **Fix:** `klj_wanted` (the jmethodID) is interned and stable —
   cache `w->binding` on first resolution (plus a resolved flag), keep the
   unreal→ue4 alias and loose-match logging on the first-resolution path only.
   Steady-state dispatch becomes one pointer load. This is the single biggest
   JNI win.

2. **MED — `klj_want` (GetMethodID/GetFieldID) is a growing linear scan under
   the global lock, with 2 strcmps per entry** (`runtime/kl_jni.c:794-819`).
   Unity caches jmethodIDs, but several plugins re-look-up per frame; the table
   only grows, so per-frame lookups degrade over the session. **Fix:** hash by
   (cls,name,sig) — or, cheaper: key by (name,sig) with cls compared last since
   cls strings are interned pointers already (compare pointers first! note
   `w->cls == cls` at :798 already exploits interning for cls, but name/sig pay
   strcmp per row).

3. **MED-LOW — `klj_intern_class_locked` linear strcmp scan under the global
   lock per FindClass/GetObjectClass/new_object** (`runtime/kl_jni.c:225-227`).
   Bounded by class count (~hundreds) so short, but it serializes with the big
   `g_lock` against every other JNI op on every thread. **Fix:** hash map keyed
   by name, or at minimum keep a per-callsite cached jclass for the hot
   well-known classes (the runtime's own callers re-intern "java/lang/String"
   on every string creation — see `kl_jni_new_string` :293).

4. **LOW — `klj_NewString` double-copies**: converts UTF-16→UTF-8 into a temp
   malloc'd buffer (`:737`), then `kl_jni_new_string` strdups it (`:763`), then
   frees the temp. Convert directly into the object's payload buffer instead.
   Only matters for text-heavy guests.

5. **LOW (verified good, do not change) — `GetStringUTFChars` is zero-copy**
   (`:669-673`: identity, isCopy=0) and Release is a no-op. `GetStringLength`'s
   UTF-8 walk (`:656-667`) is per-call but only for the rare guests that call it.

6. **LOW — PopLocalFrame takes the global lock and retires each frame object
   individually** (`:501-518`). Per guest frame this is one lock + a loop of
   per-object retire calls. Fine at current object counts; if profiling shows
   JNI lock contention, a thread-local freelist of frame structs and batch
   retire would be the fix.

7. **NOTE — the Choreographer frame clock** (`runtime/jni/kl_jni_looper.c:546-559`)
   is a free-running thread at display Hz with `nanosleep` per tick and a
   per-tick `kl_ovrp_display_frequency()` read (latched in kl_ovrp). Cheap and
   correct. `klj_frame_clock_main` could cache hz and re-read only when the
   compositor pushes a new one, but at 90 Hz this is nothing.

## Asset paths (covered after all — verified by lead)

8. **LOW — OBB read-through is sound**: `kl_obbmap_pread/read`
   (`runtime/guest/kl_obbmap.c:919-949`) take a short mutex, linear-scan a
   ≤256-entry VFD table (typically a handful of entries), then a direct `pread`.
   The `pakread_trace()` gate is latched (`:120-128`). `data_offset` double-opens
   the OBB per entry open (fopen probe + open at `:855-862`) — open-time only.
   No per-frame issue. Only note: the VFD scan could early-exit on fd equality
   order (most-recent-first), but at ≤256 entries this is noise.

9. **LOW — AAsset path is plain buffered stdio**: `kl_AAssetManager_open`
   (`runtime/guest/kl_ndk.c:381-396`) = fopen (+ OBB fallback), 2 seeks;
   `kl_AAsset_read` = fread; `kl_AAsset_getBuffer` reads whole file once and
   caches on the handle (`:405-418`). No mmap of the OBB for the APK-VFS case —
   Unity mounts the APK as a zip itself and those reads arrive as guest syscalls
   through the libc shim (translation-layer domain). Fine as-is.

## Not yet covered (agent never delivered; follow-up sweep needed)
- `runtime/jni/kl_jni_io.c`, `kl_jni_prefs.c` — per-call file I/O via JNI
- `runtime/kl_image.c` — PNG/JPEG decode loops (row conversions, no SIMD?)
  [kl_image.c the ELF loader's linear scans are load-time only — verified LOW]
