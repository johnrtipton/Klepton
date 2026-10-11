# Perf findings: translation layer (shim, libc, pthread, dl, va, x18, klepton-ld)

Scope read (lead): `runtime/libc/kl_shim.c` (dispatch table, stdio), `kl_libc.c`
(mem-pressure poll — in build-app-driver.md X2), `kl_libc_slink.c` (epoll),
`runtime/libc/kl_pthread.c` (mutex/cond maps), `runtime/kl_dl.c` (dlsym),
`runtime/kl_image.c` (sym/reloc), `runtime/libc/kl_va.c` (variadic),
`runtime/kl_stub_cells.S`, `runtime/kl_x18.c` (veneer emission — also
build-app-driver.md X11), `tools/klepton_ld.c`.

## Top findings

1. **HIGH — `kl_sym` is a linear strcmp scan over the whole .dynsym, per
   resolution** (`runtime/kl_image.c:1204-1236`). Called per import at load
   (`apply_relocs` :394-408 → `sym_value` :376 → `kl_guest_sym_global`
   kl_dl.c:295-303, which scans EVERY loaded image's whole dynsym under
   `g_lock`), and per guest `dlsym` (`kl_dl.c:880, :858-863`). libunity has
   tens of thousands of exports; binding thousands of imports is O(imports ×
   exports) — this is a large slice of the multi-second host load and of every
   guest `dlopen` during level loads. The ELF already carries DT_GNU_HASH
   (every Android lib; the file even notes Steam Link libs are `.gnu.hash`
   ONLY, kl_image.c:1082-1087) — implementing the gnu-hash probe makes lookup
   O(1) with the linear scan as fallback. Also add a process-wide
   {name → (image,addr)} cache filled at load so repeated lookups skip images
   entirely.

2. **HIGH — `kl_shim_lookup` is a linear strcmp scan over the whole shim table
   (~650+ entries) per unresolved-symbol event** (`runtime/libc/kl_shim.c:2258-2261`),
   and `sym_value` calls it for EVERY undefined symbol at load
   (`runtime/kl_image.c:384`). 454 KL_FWD rows + ~190 E() rows, scanned once
   per import during relocation → hundreds of thousands of strcmps per
   library load. **Fix:** sort the table at init (it is `static const`, so
   sort a runtime index array) + bsearch, or build a small FNV hash map once.

3. **MED — every guest `pthread_mutex_lock` does a CAS scan over a shared
   64-slot global array** (`runtime/libc/kl_pthread.c:387-405`,
   `mtx_wait_enter`/`mtx_wait_leave`, called unconditionally from
   `klb_pthread_mutex_lock` :594-606 and unlock). Every locking thread CASes
   the same cache lines (claim on enter, find-self on leave) — a global
   coherence funnel across ALL guest threads, plus 2 more atomic stores
   (`locksite`, `owner`) and a `depth` RMW per lock. This is diagnostic
   bookkeeping for hang reports, always on. **Fix:** move waiter tracking to
   TLS (thread-local slot, zero shared traffic) and gate the owner/locksite
   writes behind the trace knob; keep the address-keyed host-mutex map as is
   (it's already a proper open-addressed hash, :112-134).

4. **MED — cond signal/broadcast pay `clock_gettime` + 2 atomics per call,
   cond wait pays ~8 atomics + a frame walk** (`runtime/libc/kl_pthread.c:757-767,
   786-806`). `cnd_now_ns()` (a syscall-ish read) per signal; `waiter_ra2`
   does a frame walk with 2 pthread stack queries per wait. All unconditional
   instrumentation for the KL_TRACE_CONDDUMP hang report. Unity job systems
   signal conds thousands of times/sec. **Fix:** gate the counters on the
   trace knob (latched), or arm them lazily only when the watchdog fires.

5. **MED — `klb_epoll_wait` callocs per call** (`runtime/libc/kl_libc_slink.c:361-363`):
   `calloc(maxevents, sizeof(kevent))` + free on EVERY epoll_wait. Steam
   Link's network thread and SDL3 poll loops hit this constantly. **Fix:**
   keep a per-epfd (or thread-local) scratch array, grown only when maxevents
   grows; stack buffer for the common maxevents ≤ 64.

6. **LOW — all host mutexes are RECURSIVE** (`mtx_make` :136-143). Guest
   normal mutexes pay Darwin's recursive-mutex owner check per lock/unlock
   even when never re-entered. **Fix:** default to a normal mutex; upgrade to
   recursive only after the first re-entrant lock is observed (or on
   mutexattr_settype(RECURSIVE)). Small but on every guest lock.

7. **LOW — interpose table scanned per symbol** (`runtime/kl_image.c:380-382`):
   `g_interpose` linear strcmp per sym_value call. Tiny table (a dozen FMOD
   entries); fine today, same hash-map fix as (2) covers it.

## Verified clean (do not "fix")

- **Stub cells are near-optimal** (`runtime/kl_stub_cells.S`): 8-byte cell
  (`mov w16,#i; b common`) + 5-instruction common dispatcher; no runtime code
  generation (AMFI-safe), i-cache friendly.
- **`kl_host_file` is O(1)** (`runtime/libc/kl_shim.c:163-187`): pointer
  identity + a range check; the stdio translation adds no per-call cost.
- **Variadic marshaling** (`runtime/libc/kl_va.c`) walks the format string
  once per call and reads the AAPCS64 save area directly — the only possible
  shape for forwarding an unknown-signature variadic; cost is proportional to
  format length, inherent.
- **kl_va_gp/kl_va_fp** read register-save areas with no copying.
- **The mutex/cond ADDRESS-keyed open-addressed maps** (262144 slots,
  `mtx_hash` :132-134) — O(1), atomically published, correct; per-call cost is
  one hash + probe.
- **klb_pthread_cond_timedwait's monotonic→realtime rebase** does 2
  clock_gettime per timedwait — could cache the offset (it's constant between
  NTP steps) but timedwaits are not the hot loop; note only.
- **klepton-ld is single-pass**; the one per-byte loop (`tools/klepton_ld.c:699-703`)
  is the content-derived UUID hash — one pass over the file per translation.
  Not worth vectorizing at ~0.1 s per 100 MB input.
- **pthread mutex lock/unlock forward directly to host pthreads** — no
  double bookkeeping beyond finding the host object (the finding is the
  *instrumentation*, not the design).
