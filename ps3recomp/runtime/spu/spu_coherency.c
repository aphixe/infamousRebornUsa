/*
 * ps3recomp - PPU <-> SPU lock-line coherence
 *
 * See spu_coherency.h for what this is for. The state here is deliberately
 * plain: a bit per 128-byte line, a small table of the SPU contexts that have
 * ever reserved one, and the lock-line spinlock. Everything that WRITES it runs
 * with the lock-line lock held (the SPU's GETLLAR, the PPU's coherent store),
 * so the read-modify-write on a shared bitmap byte is serialized. Only
 * spu_coh_is_reserved reads without the lock, which is the entire point: it is
 * on every PPU store.
 */

#include "../../libs/video/rsx_getenv_cache.h"   /* cached getenv(): SPU hot paths called getenv per PM run (2026-10-04) */
#include <stdlib.h>
#include "spu_coherency.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * The lock-line lock
 *
 * Moved here from spu_channels.c, where it was a file-static and so could
 * serialize SPU against SPU but not SPU against PPU -- which left the PPU free
 * to write a line in the middle of a PUTLLC's compare-and-commit. Same
 * spinlock, same semantics, now shared. _InterlockedExchange is a clang-cl/MSVC
 * intrinsic (no runtime library symbol needed); elsewhere use the C11
 * equivalent, which lowers to the same LL/SC or lock-xchg on every target.
 * ===========================================================================*/
#if defined(_MSC_VER)
#include <intrin.h>
#include <windows.h>   /* SwitchToThread */
/* v32d: STRIPED. One global spinlock serialised every GETLLAR/PUTLLC/coherent store of
 * ~50 host threads (PC profile: spu_lockline_lock 6-14% of every SPU thread). Now 64
 * cache-line-padded stripes keyed by the 128-byte line: per-line operations take only
 * their line's stripe (spu_lockline_lock_ea), and the old whole-lock API takes ALL
 * stripes in order (rare HLE paths, registry changes), so it still excludes everyone.
 * Rule: never take a second stripe while holding one. SPU_LOCKLINE_STRIPES=0 -> one
 * stripe (old behaviour). */
#define LL_STRIPES 64
/* v100: + owner (host thread id of the holder, one store per acquire into the line we already own)
 * for the long-wait report below. Still one 64-byte line per stripe. */
typedef struct { volatile long v; volatile unsigned long owner; char pad[56]; } LLStripe;
static LLStripe g_ll[LL_STRIPES];
static int g_ll_n = -1;
static inline int ll_count(void)
{
    if (g_ll_n < 0) { const char* e = getenv("SPU_LOCKLINE_STRIPES"); g_ll_n = (e && *e == '0') ? 1 : LL_STRIPES; }
    return g_ll_n;
}
static inline unsigned ll_idx(uint32_t ea) { return (ll_count() == 1) ? 0u : ((ea >> 7) & (LL_STRIPES - 1)); }
/* v100 slow path (only after 4096 pauses on a held stripe -- never on the fast path).
 * play99 t=319 s: every SPURS lane sat ~1 s in atomics ([LANEPROF] atom 214%), the main thread
 * waited 1015 ms on a task, a Bink mutex_lock with a 5 ms timeout took 890 ms: a stripe holder
 * that is not getting CPU (ABOVE_NORMAL spinners pinned to the 16 P-core CPUs; SwitchToThread
 * only yields to threads ready on the SAME CPU, never to a lower-priority holder elsewhere).
 * After SPU_LL_SLEEP_MS (default 4) of waiting, every 4th yield becomes Sleep(1), which lets ANY
 * ready thread run, so a preempted holder can finish. SPU_LL_SLEEP_MS=0 = old behaviour.
 * A wait > 50 ms is reported ("[LL-WAIT]", rate-limited) with the holder's host tid. */
static long long ll_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
static long long ll_qpf(void) { static long long f; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = q.QuadPart; } return f; }
static int ll_sleep_ms(void) { static int v = -1; if (v < 0) { const char* e = getenv("SPU_LL_SLEEP_MS"); v = e ? atoi(e) : 4; if (v < 0) v = 0; } return v; }
static void ll_slow_round(volatile long* l, long long* t0, unsigned* rounds, int* reported)
{
    long long now = ll_qpc();
    if (!*t0) { *t0 = now; SwitchToThread(); return; }
    long long f = ll_qpf();
    long long us = (now - *t0) * 1000000LL / f;
    int sl = ll_sleep_ms();
    ++*rounds;
    if (sl > 0 && us >= (long long)sl * 1000LL && (*rounds & 3u) == 0) Sleep(1);
    else SwitchToThread();
    /* report at 50 ms, 250 ms and 1 s of waiting (levels 1..3), never while holding this stripe */
    { int lvl = us >= 1000000 ? 3 : us >= 250000 ? 2 : us >= 50000 ? 1 : 0;
      if (lvl > *reported) {
        static volatile long s_n = 0;
        *reported = lvl;
        long n = _InterlockedIncrement(&s_n);
        if (n <= 60 || (n % 100) == 0) {
            const LLStripe* st = (const LLStripe*)(const void*)l;
            fprintf(stderr, "[LL-WAIT] #%ld stripe %d held >= %lld ms: waiter host tid %lu, holder host tid %lu (v100)\n",
                    n, (int)(st - g_ll), us / 1000, (unsigned long)GetCurrentThreadId(), (unsigned long)st->owner);
        }
      } }
}
/* Test-and-test-and-set: spin on a plain read (no cache-line ping-pong) with a
 * pause, and yield after a long wait. */
static inline void ll_acq(volatile long* l)
{
    unsigned spins = 0, rounds = 0; long long t0 = 0; int reported = 0;
    while (_InterlockedExchange(l, 1)) {
        do {
            _mm_pause();
            if (++spins >= 4096) { spins = 0; ll_slow_round(l, &t0, &rounds, &reported); }
        } while (*l);
    }
    ((LLStripe*)(void*)l)->owner = GetCurrentThreadId();
}
static inline void ll_rel(volatile long* l) { _InterlockedExchange(l, 0); }
#else
static inline int ll_count(void);
#define LL_STRIPES 64
typedef struct { volatile long v; char pad[60]; } LLStripe;
static LLStripe g_ll[LL_STRIPES];
static int g_ll_n = -1;
static inline int ll_count(void)
{
    if (g_ll_n < 0) { const char* e = getenv("SPU_LOCKLINE_STRIPES"); g_ll_n = (e && *e == '0') ? 1 : LL_STRIPES; }
    return g_ll_n;
}
static inline unsigned ll_idx(uint32_t ea) { return (ll_count() == 1) ? 0u : ((ea >> 7) & (LL_STRIPES - 1)); }
static inline void ll_acq(volatile long* l) { while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) { while (__atomic_load_n(l, __ATOMIC_RELAXED)) { } } }
static inline void ll_rel(volatile long* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }
#endif
void spu_lockline_lock_ea(uint32_t ea)   { ll_acq(&g_ll[ll_idx(ea)].v); }
void spu_lockline_unlock_ea(uint32_t ea) { ll_rel(&g_ll[ll_idx(ea)].v); }
void spu_lockline_lock(void)   { int n = ll_count(); for (int i = 0; i < n; i++) ll_acq(&g_ll[i].v); }
void spu_lockline_unlock(void) { int n = ll_count(); for (int i = n - 1; i >= 0; i--) ll_rel(&g_ll[i].v); }

/* ===========================================================================
 * The reserved-line bitmap
 *
 * One bit per 128-byte line over the whole 32-bit guest address space: 4 MiB of
 * zero-initialized BSS, of which only the pages around addresses an SPU has
 * actually reserved are ever touched. The reference implementation scopes its
 * bitmap to the one window a single title's SPURS structures live in; this
 * runtime hands out guest memory from several disjoint regions (the ELF image,
 * the sys_memory window at 0x40000000, the raw-SPU local-store windows), and a
 * line outside a hardcoded window would be silently non-coherent, which is the
 * failure this whole file exists to remove.
 * ===========================================================================*/
#define SPU_COH_LINE_SHIFT  7                                  /* 128-byte lines */
#define SPU_COH_LINES       (1u << (32 - SPU_COH_LINE_SHIFT))   /* 2^25 lines */
#define SPU_COH_BITMAP_SZ   (SPU_COH_LINES / 8)                 /* 4 MiB */

unsigned char s_coh_bitmap[SPU_COH_BITMAP_SZ];   /* non-static: inlined by ppu_loader.cpp VM_WRITE_COH (v28) */

/* Zero until the first GETLLAR anywhere. Keeps the PPU fast path off the
 * bitmap entirely for a title that runs no SPU code, and keeps every existing
 * test that stores through vm_write* paying one compare against a hot global.
 * Written once, under the lock; read unlocked, where a stale zero costs at most
 * one missed event on the very first reservation of the run. */
volatile int s_coh_armed;   /* non-static: see s_coh_bitmap */

/* The SPU contexts that have reserved a line. Sized to match the MFC engine
 * registry in spu_channels.c, which is the ceiling on live SPU contexts. */
#define SPU_COH_MAX_CTX 256  /* = SPU_MAX_CONTEXTS (spu_channels.c); was 64 (inFamous cutscene: 65+ PM contexts) */
static spu_context* s_coh_ctxs[SPU_COH_MAX_CTX];
/* High-water mark: walkers stop here instead of scanning all 256 slots. */
static volatile int s_coh_hi = 0;
/* v58: only contexts with a live (or just-consumed) reservation need visiting.
 * Walking every context ever registered on every PUTLLC was catastrophic for
 * atomic-heavy workloads: millions of commits times tens/hundreds of stale
 * registry entries.  Four words cover the complete 256-context registry. */
static volatile uint64_t s_coh_active[SPU_COH_MAX_CTX / 64];

/* Policy contexts are reset with memset between runs, so a slot cannot live in
 * spu_context itself.  Cache recent ctx->slot mappings per host worker instead;
 * the pointer is validated against the registry to make reuse/unregister safe. */
typedef struct { spu_context* ctx; int slot; } CohTlsEnt;
static SPU_THREAD_LOCAL CohTlsEnt t_coh_slots[16];
static SPU_THREAD_LOCAL unsigned t_coh_slot_next;

unsigned long g_spu_lr_raise = 0;

/* v221A V221A_COH (ABX 0x200000 SET or V221A_COH=0 = old).
 * (1) The bitmap byte and s_coh_armed were STORED on every GETLLAR (~300k/s non-re-poll GETLLARs in the city), even
 *     when already set: each store takes the cache line exclusive, so the next lifted PPU store anywhere in the 64 KB
 *     of guest memory that bitmap line covers (job lists, task managers) misses, and the SPU lanes bounce the line
 *     among themselves. Now: write only when the bit is clear, with an atomic OR (the old plain |= could even lose a
 *     neighbour line's first bit to a GETLLAR under another stripe -- healed only by that line's next GETLLAR).
 * (2) s_coh_rline[slot] = (reserved line index + 1) of the slot's latest spu_coh_reserve, ALWAYS maintained (also
 *     while the bit selects the old path, so flipping it mid-run is safe). Every resv_valid = 1 is preceded by
 *     spu_coh_reserve(ctx, ea) with the same ea (spu_channels.c GETLLAR; the v215 re-poll path needs resv_ea == ea
 *     from that earlier reserve), so a context holding a valid reservation on line L always has s_coh_rline == L+1:
 *     the notify walks may skip a context whose entry differs without loading the context (a cache miss on a line its
 *     owner writes constantly). A stale entry only costs the old dereference. */
static volatile uint32_t s_coh_rline[SPU_COH_MAX_CTX];
static inline int coh221_on(void)
{
    extern volatile int g_abx;
    static int e = -1;
    if (__builtin_expect(e < 0, 0)) { const char* s = getenv("V221A_COH"); e = (s && *s == '0') ? 0 : 1;
        fprintf(stderr, "[spu-coh] v221A V221A_COH=%d (write-once bitmap/armed + notify line filter; ABX 0x200000 = old)\n", e); }
    return e && !(g_abx & 0x200000);
}

/* v243A: per-line store-flag table of the lean lifted-code accessors (ppu_memory_v243.h); see the block at the end. */
unsigned char g_v243a_stflag[1u << 25];          /* one byte per 128-byte guest line, 32 MiB BSS, touched lazily */
static volatile int s_v243a_mirror = 0;          /* 1 = spu_coh_reserve mirrors reserved lines into bit0 */
static inline void v243a_mirror(uint32_t line)
{
    if (__builtin_expect(__atomic_load_n(&s_v243a_mirror, __ATOMIC_SEQ_CST) != 0, 0) && !(g_v243a_stflag[line] & 1u))
        __atomic_fetch_or(&g_v243a_stflag[line], (unsigned char)1u, __ATOMIC_SEQ_CST);
}

void spu_coh_reserve(spu_context* ctx, uint32_t ea)
{
    uint32_t line = ea >> SPU_COH_LINE_SHIFT;
    if (coh221_on()) {   /* v221A: no store when nothing changes */
        const unsigned char bit = (unsigned char)(1u << (line & 7));
        if (!(*(volatile unsigned char*)&s_coh_bitmap[line >> 3] & bit))
            __atomic_fetch_or(&s_coh_bitmap[line >> 3], bit, __ATOMIC_SEQ_CST);
        if (!s_coh_armed) s_coh_armed = 1;
    } else {
        s_coh_bitmap[line >> 3] |= (unsigned char)(1u << (line & 7));
        s_coh_armed = 1;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* v243A: bitmap store before the mirror-flag load (Dekker) */
    }
    v243a_mirror(line);   /* v243A: before the reservation becomes observable, like the bitmap bit (no-op while off) */

    if (!ctx) return;
    int slot = -1;
    for (int k = 0; k < 16; k++) {
        int s = t_coh_slots[k].slot;
        if (t_coh_slots[k].ctx == ctx && s >= 0 && s < SPU_COH_MAX_CTX &&
            __atomic_load_n(&s_coh_ctxs[s], __ATOMIC_ACQUIRE) == ctx) { slot = s; break; }
    }
    if (slot < 0) for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        spu_context* cur = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
        if (cur == ctx) { slot = i; break; }
        if (cur == NULL) {
            spu_context* nil = NULL;
            if (__atomic_compare_exchange_n(&s_coh_ctxs[i], &nil, ctx, 0,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                slot = i;
                int hi = __atomic_load_n(&s_coh_hi, __ATOMIC_RELAXED);
                while (hi < i + 1 && !__atomic_compare_exchange_n(&s_coh_hi, &hi, i + 1, 0,
                                                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
                break;
            }
            if (nil == ctx) { slot = i; break; }
        }
    }
    if (slot >= 0) {
        { const uint32_t lv = line + 1u; if (s_coh_rline[slot] != lv) s_coh_rline[slot] = lv; }   /* v221A: always kept */
        unsigned c = t_coh_slot_next++ & 15u;
        t_coh_slots[c].ctx = ctx; t_coh_slots[c].slot = slot;
        __atomic_fetch_or(&s_coh_active[slot >> 6], 1ull << (slot & 63), __ATOMIC_RELAXED);
        return;
    }
    /* More live SPU contexts than the registry holds. The ones already in it
     * still get their events; this one would silently never wake, so say so. */
    { static int warned = 0;
      if (!warned) { warned = 1;
          fprintf(stderr, "[spu-coh] more than %d reserving SPU contexts -- "
                          "spu=0x%X will not receive lock-line events\n",
                  SPU_COH_MAX_CTX, ctx->spu_id);
          fflush(stderr); } }
}

void spu_coh_release(spu_context* ctx)
{
    if (!ctx) return;
    for (int k = 0; k < 16; k++) {
        int s = t_coh_slots[k].slot;
        if (t_coh_slots[k].ctx == ctx && s >= 0 && s < SPU_COH_MAX_CTX &&
            __atomic_load_n(&s_coh_ctxs[s], __ATOMIC_ACQUIRE) == ctx) {
            __atomic_fetch_and(&s_coh_active[s >> 6], ~(1ull << (s & 63)), __ATOMIC_RELAXED);
            return;
        }
    }
    /* Rare fallback (release on a different host thread than registration). */
    for (int i = 0, hi = __atomic_load_n(&s_coh_hi, __ATOMIC_RELAXED); i < hi; i++)
        if (__atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE) == ctx) {
            __atomic_fetch_and(&s_coh_active[i >> 6], ~(1ull << (i & 63)), __ATOMIC_RELAXED);
            return;
        }
}

/* Drop a context from the reserving set.
 *
 * There was no way to do this, and the contexts that most need it are
 * TRANSIENT: spu_run_lifted_job_abi runs each job on a STACK-LOCAL
 * spu_context, which registered itself here on its first reservation and then
 * returned. The entry outlived the object, so spu_coh_notify_write walked a
 * dangling pointer -- reading resv_valid, writing event_status and calling
 * spu_ch_wake on reclaimed stack. Undefined behaviour, and observable: Guitar
 * Hero III showed THREE live-looking contexts for one dispatch, stealing each
 * other`s reservations, with every PUTLLC failing "no reservation". */
void spu_coh_unregister(spu_context* ctx)
{
    if (!ctx) return;
    /* Under the lock-line lock, which every walker of s_coh_ctxs holds: a
     * notifier that loaded this pointer just before the NULL store would
     * otherwise go on dereferencing it after the job returned and its stack
     * (or its whole async thread) was gone. GH3 at -O2 crashed that way in
     * spu_coh_notify_write, reading a freed thread stack. */
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++)
        if (s_coh_ctxs[i] == ctx) {
            __atomic_fetch_and(&s_coh_active[i >> 6], ~(1ull << (i & 63)), __ATOMIC_RELAXED);
            s_coh_ctxs[i] = NULL; break;
        }
    for (int k = 0; k < 16; k++) if (t_coh_slots[k].ctx == ctx) t_coh_slots[k].ctx = NULL;
    spu_lockline_unlock();
}

/* Drop every registered context that lies in [lo, hi) -- a host thread's
 * stack, called as that thread exits. A stack-local context still registered
 * then has leaked past its unregister; the next walker would read an unmapped
 * stack (GH3 at the end of a Havok task: spu_coh_notify_write faulting).
 * ponytail: safety net that also names the leak; find the path that skips
 * spu_coh_unregister if it keeps firing. */
void spu_coh_forget_range(uintptr_t lo, uintptr_t hi)
{
    spu_lockline_lock();
    for (int i = 0; i < SPU_COH_MAX_CTX; i++) {
        uintptr_t c = (uintptr_t)s_coh_ctxs[i];
        if (c >= lo && c < hi) {
            static int _n = 0;
            if ((_n < 16 && _n++ < 16))
                fprintf(stderr, "[spu-coh] leaked ctx %p (img=%d) dropped at thread exit\n",
                        (void*)c, s_coh_ctxs[i]->image_id);
            __atomic_fetch_and(&s_coh_active[i >> 6], ~(1ull << (i & 63)), __ATOMIC_RELAXED);
            s_coh_ctxs[i] = NULL;
        }
    }
    spu_lockline_unlock();
}

int spu_coh_is_reserved(uint32_t addr)
{
    if (!s_coh_armed) return 0;
    uint32_t line = addr >> SPU_COH_LINE_SHIFT;
    return (s_coh_bitmap[line >> 3] >> (line & 7)) & 1u;
}

/* A DMA PUT is issued BY an SPU, and hardware does not take that SPU's own
 * reservation away for its own MFC write -- the MFC is part of the same SPE.
 * Our notify had no way to say "everyone but me", so an SPU that reserved a
 * line and then DMA'd a buffer overlapping it killed its own reservation and
 * its next PUTLLC could never succeed. Guitar Hero III's job policy module
 * livelocks exactly there: 16 million atomic ops on one line, every PUTLLC
 * failing for "no reservation", 600k of them self-inflicted.
 *
 * The PUTLLC path already guards this case by dropping its own reservation
 * before notifying; this gives the DMA path the same ability. */
void spu_coh_notify_write_except(uint32_t ea, const void* self)
{
    uint32_t line = ea & ~127u;
    const int filt = coh221_on(); const uint32_t lv = (ea >> SPU_COH_LINE_SHIFT) + 1u;   /* v221A */
    for (int w = 0; w < SPU_COH_MAX_CTX / 64; w++) {
      uint64_t bits = __atomic_load_n(&s_coh_active[w], __ATOMIC_RELAXED);
      while (bits) {
        int b = __builtin_ctzll(bits), i = w * 64 + b; bits &= bits - 1;
        if (filt && s_coh_rline[i] != lv) continue;   /* v221A: reserved line differs -> no context load */
        spu_context* c = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
        if (!c) { __atomic_fetch_and(&s_coh_active[w], ~(1ull << b), __ATOMIC_RELAXED); continue; }
        if ((const void*)c == self) continue;
        if (!c->resv_valid) continue;
        if ((c->resv_ea & ~127u) == line) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);   /* v32d: stripes -> atomic */
            c->resv_valid = 0;
            __atomic_fetch_and(&s_coh_active[w], ~(1ull << b), __ATOMIC_RELAXED);
            g_spu_lr_raise++;
            spu_ch_wake(c);
        }
      }
    }
}

void spu_coh_notify_write(uint32_t ea)
{
    uint32_t line = ea & ~127u;
    const int filt = coh221_on(); const uint32_t lv = (ea >> SPU_COH_LINE_SHIFT) + 1u;   /* v221A */
    for (int w = 0; w < SPU_COH_MAX_CTX / 64; w++) {
      uint64_t bits = __atomic_load_n(&s_coh_active[w], __ATOMIC_RELAXED);
      while (bits) {
        int b = __builtin_ctzll(bits), i = w * 64 + b; bits &= bits - 1;
        if (filt && s_coh_rline[i] != lv) continue;   /* v221A: reserved line differs -> no context load */
        spu_context* c = __atomic_load_n(&s_coh_ctxs[i], __ATOMIC_ACQUIRE);
        if (!c) { __atomic_fetch_and(&s_coh_active[w], ~(1ull << b), __ATOMIC_RELAXED); continue; }
        if (!c->resv_valid) continue;
        if ((c->resv_ea & ~127u) == line) {
            __atomic_fetch_or(&c->event_status, SPU_EVENT_LR, __ATOMIC_SEQ_CST);   /* v32d: stripes -> atomic */
            c->resv_valid = 0;          /* reservation lost, PUTLLC must fail */
            __atomic_fetch_and(&s_coh_active[w], ~(1ull << b), __ATOMIC_RELAXED);
            /* SPU_PUTLLC_WHY=1: name the agent that killed it. A PUTLLC that
             * always fails for "no reservation" is useless without knowing who
             * took it away -- a peer SPU, or the PPU committing to the line. */
            { static int s_w = -1;
              if (s_w < 0) s_w = getenv("SPU_PUTLLC_WHY") ? 1 : 0;
              if (s_w) { static unsigned long long n;
                  if ((++n % 200000) == 1)
                      fprintf(stderr, "[resv-lost] %llu: line 0x%08X taken from img=%d ctx=%p\n",
                              n, line, c->image_id, (void*)c); } }
            g_spu_lr_raise++;
            /* The event_status store has to be visible before the wake. The
             * waiter re-polls its predicate on a timeout as well, so a
             * straggling store costs latency and not the wakeup itself. */
            spu_ch_wake(c);
        }
      }
    }
}


/* ===========================================================================
 * v243A (2026-10-20): lean guest accessors for the lifted PPU code -- runtime side.
 *
 * ps3recomp/tools/ppu_memacc_v243.py (CMake PORT_PPU_MEMLEAN=AB) gives listed lifted functions a twin whose guest
 * loads/stores use the helpers of ps3recomp/runtime/ppu/ppu_memory_v243.h: no bounds compare on 8/16/64-bit loads,
 * immediate compares on 32-bit loads, and ONE byte test per store (g_v243a_stflag[x >> 7]) instead of the bounds
 * compare + reservation-bitmap bit extraction (+ the 32-bit special-word compare). Those helpers equal the v96b/v156
 * inline gates only in the runtime's STANDARD fast mode, so the dispatchers run them only after this function has
 * seen exactly the standard limit values (they then never change: ppu_vm_gate_update() inputs are fixed without
 * PS3_NULL_SWEEP / SPU_ATOM_EA / DEP_TRACE / the VM_SLOW-style diagnostic envs, all of which make a limit differ).
 * g_v243a_sel: -1 = undecided (old code; the dispatchers call here), 0 = old code for good,
 *              1 / 2 = lean code only in even / odd PS3_AB phases (V243A_MEM_PHASE=0 / 1), 3 = lean always (default).
 * V243A_MEM=0 forces 0. Without the AB build nothing calls this and nothing reads the table (the mirror stays off).
 * Table bits: bit0 = an SPU reserved the line (set together with s_coh_bitmap: the enable scan copies the bits set so
 * far, spu_coh_reserve mirrors every later one before the reservation is observable -- Dekker on s_v243a_mirror);
 * bit1 = every store width goes out of line (null page lines 0..31, the last line: x > 0xFFFFFFF8 wraps);
 * bit2 = 32-bit stores go out of line (lines >= 0xE0000000, the line of 0xDFFFFFFC, the GCM FIFO-put line).
 * Any address the old gate sends out of line has its line flagged; the other addresses of a flagged line also go out
 * of line now, where vm_write* in VM_FAST mode performs the identical plain store. NOTE for future code: anything that
 * sets s_coh_bitmap bits after start-up outside spu_coh_reserve must also OR bit0 here (infamous_clock_probe.h marks
 * its lines at ps3_load_prx_modules time, before guest code runs -> covered by the enable scan).
 * ===========================================================================*/
volatile int g_v243a_sel = -1;
static volatile int s_v243a_once = 0;
extern int      g_vm_fast_inline;                               /* ppu_loader.cpp */
extern uint64_t g_ppu_ld_lim, g_ppu_st_lim2, g_ppu_vst_lim2, g_ppu_ld32_lim, g_ppu_st32_lim2;
extern uint32_t ppu_vm_size, ppu_hle_inject_base;
void v243a_lean_try(void)
{
    if (__atomic_load_n(&g_v243a_sel, __ATOMIC_ACQUIRE) >= 0) return;
    /* vm_fast_init() sets g_vm_fast_inline first and the limits right after: wait until both are visible */
    if (!__atomic_load_n(&g_vm_fast_inline, __ATOMIC_ACQUIRE) || !__atomic_load_n(&g_ppu_ld_lim, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&s_v243a_once, 1, __ATOMIC_ACQ_REL)) return;   /* another thread decides; old code meanwhile */
    const int std = g_ppu_ld_lim == ~0ull && g_ppu_ld32_lim == 0xE0000000ull && g_ppu_st_lim2 == 0xFFFFF000ull &&
                    g_ppu_vst_lim2 == 0xFFFFF000ull && g_ppu_st32_lim2 == 0xDFFFF000ull && ppu_vm_size == 0 &&
                    !getenv("PS3_NULL_SWEEP");
    const char* e = getenv("V243A_MEM");
    const char* ph = getenv("V243A_MEM_PHASE");
    const int want = (e && *e == '0') ? 0 : (ph && (*ph == '0' || *ph == '1')) ? 1 + (*ph - '0') : 3;
    if (!std || !want) {
        fprintf(stderr, "[v243A] lean guest accessors OFF (%s): ld %llx ld32 %llx st %llx vst %llx st32 %llx vm_size %x\n",
                !want ? "V243A_MEM=0" : "runtime not in the standard fast mode",
                (unsigned long long)g_ppu_ld_lim, (unsigned long long)g_ppu_ld32_lim, (unsigned long long)g_ppu_st_lim2,
                (unsigned long long)g_ppu_vst_lim2, (unsigned long long)g_ppu_st32_lim2, (unsigned)ppu_vm_size);
        fflush(stderr);
        __atomic_store_n(&g_v243a_sel, 0, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&s_v243a_mirror, 1, __ATOMIC_SEQ_CST);      /* from here on spu_coh_reserve mirrors */
    const uint32_t NL = 1u << 25;
    for (uint32_t l = 0; l < (0x1000u >> 7); l++) __atomic_fetch_or(&g_v243a_stflag[l], (unsigned char)2u, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_v243a_stflag[NL - 1u], (unsigned char)2u, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_v243a_stflag[0xDFFFFFFCu >> 7], (unsigned char)4u, __ATOMIC_RELAXED);
    for (uint32_t l = 0xE0000000u >> 7; l < NL; l++) __atomic_fetch_or(&g_v243a_stflag[l], (unsigned char)4u, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_v243a_stflag[(ppu_hle_inject_base + 0x2000u) >> 7], (unsigned char)4u, __ATOMIC_RELAXED);
    unsigned long copied = 0;
    for (uint32_t i = 0; i < SPU_COH_BITMAP_SZ; i++) {
        unsigned b = __atomic_load_n(&s_coh_bitmap[i], __ATOMIC_SEQ_CST);
        while (b) { unsigned k = (unsigned)__builtin_ctz(b); b &= b - 1u;
            __atomic_fetch_or(&g_v243a_stflag[i * 8u + k], (unsigned char)1u, __ATOMIC_SEQ_CST); copied++; }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&g_v243a_sel, want, __ATOMIC_RELEASE);
    fprintf(stderr, "[v243A] lean guest accessors ON (sel %d: %s), %lu reserved lines copied from s_coh_bitmap\n",
            want, want == 3 ? "always" : want == 1 ? "even PS3_AB phases" : "odd PS3_AB phases", copied);
    fflush(stderr);
}

/* v245 PORT_PPU_MEMLEAN=ALL startup gate. Direct lean bodies deliberately have
 * no old-code twin and no per-call selector, so validate the model once before
 * dispatching the first guest instruction. Fail closed instead of executing a
 * direct body with an uninitialised flag table. AB builds retain V243A_MEM=0. */
int v243a_lean_require(void)
{
    v243a_lean_try();
    const int sel = __atomic_load_n(&g_v243a_sel, __ATOMIC_ACQUIRE);
    if (sel == 3) return 1;
    fprintf(stderr, "[v245] PORT_PPU_MEMLEAN=ALL requires the standard fast memory mode; "
                    "use an AB/OFF build for V243A_MEM=0 or memory diagnostics (selector %d)\n", sel);
    fflush(stderr);
    return 0;
}

#ifdef __cplusplus
}
#endif
