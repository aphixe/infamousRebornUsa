/* ppu_memory_v243.h -- v243A lean guest accessors for the lifted PPU code (2026-10-20).
 * Included ONLY by TUs rewritten by ps3recomp/tools/ppu_memacc_v243.py (CMake PORT_PPU_MEMLEAN=AB), right after
 * ppu_recomp.h. Nothing else includes it, so with the option OFF no file changes and nothing recompiles. */
#pragma once
/* ---- v243A: lean guest accessors (guest memory model) ------------------------------------------------
 * Used ONLY by lifted functions compiled inside V243A_LEAN_BEGIN / V243A_LEAN_END (twins made by
 * ps3recomp/tools/ppu_memacc_v243.py), and those run only while g_v243a_sel says so: the dispatcher picks the lean
 * copy only after v243a_lean_try() (spu_coherency.c) has verified that the runtime is in the STANDARD fast mode,
 * i.e. the v96b limits of this header have exactly these values and can no longer change:
 *   g_ppu_ld_lim = ~0 (ppu_vm_size 0: the whole 4 GB + guard is reserved), g_ppu_ld32_lim = 0xE0000000,
 *   g_ppu_st_lim2 = g_ppu_vst_lim2 = 0xFFFFF000, g_ppu_st32_lim2 = 0xDFFFF000, PS3_NULL_SWEEP unset.
 * Under those values the old inline predicates reduce to:
 *   ld8/16/64: always inline (no test at all)          ld32: x <= 0xDFFFFFFC && x != INJECT+0x2008 (immediates)
 *   st8/16/64/stvx: 0x1000 <= x <= 0xFFFFFFF8 (stvx: aligned x <= 0xFFFFFFF0) && no SPU reservation on x's line
 *   st32: 0x1000 <= x <= 0xDFFFFFFC && x != INJECT+0x2000 && no reservation
 * The store tests are folded into ONE byte per 128-byte guest line, g_v243a_stflag[x >> 7]:
 *   bit0 = an SPU reserved the line (mirror of s_coh_bitmap, set by spu_coh_reserve / the enable scan),
 *   bit1 = out of line for every store width (lines 0..31 = null page, the last line = 4 GB wrap),
 *   bit2 = out of line for 32-bit stores (lines >= 0xE0000000, the line of 0xDFFFFFFC, the GCM FIFO-put line).
 * Every address the old predicate sends out of line is flagged; some extra in-range addresses (rest of the flagged
 * lines) go out of line too, where vm_write* in VM_FAST mode does exactly the plain store -> identical results.
 * The store into the guest is the same movbe; the slow call is the same ppu_slow_* / vm_write* call. */
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char g_v243a_stflag[];
extern volatile int  g_v243a_sel;      /* -1 pending, 0 old code, 1 lean in even PS3_AB phases, 2 odd, 3 always */
void v243a_lean_try(void);
extern volatile int  g_ab_phase;     /* ppu_loader.cpp: PS3_AB phase counter */
#ifdef __cplusplus
}
#endif
#define V243A_PICK() (g_v243a_sel == 3 || (g_v243a_sel > 0 && (g_ab_phase & 1) == g_v243a_sel - 1))
static inline uint8_t ppu__ld8L(uint8_t* b, uint64_t a) { return b[(uint32_t)a]; }
static inline uint16_t ppu__ld16L(uint8_t* b, uint64_t a) { uint16_t v; memcpy(&v, b + (uint32_t)a, 2); return __builtin_bswap16(v); }
static inline uint32_t ppu__ld32L(uint8_t* b, uint64_t a) {
    uint32_t x = (uint32_t)a;
    if (__builtin_expect(x <= 0xDFFFFFFCu && x != PPU_INJECT_BASE_C + 0x2008u, 1)) {
        uint32_t v; memcpy(&v, b + x, 4); return __builtin_bswap32(v); }
    return PPU_SLOW_R(32, a);
}
static inline uint64_t ppu__ld64L(uint8_t* b, uint64_t a) { uint64_t v; memcpy(&v, b + (uint32_t)a, 8); return __builtin_bswap64(v); }
static inline void ppu__st8L(uint8_t* b, uint64_t a, uint8_t v) {
    uint32_t x = (uint32_t)a;
    if (__builtin_expect(!(g_v243a_stflag[x >> 7] & 3u), 1)) { b[x] = v; return; }
    PPU_SLOW_W(8, a, v);
}
static inline void ppu__st16L(uint8_t* b, uint64_t a, uint16_t v) {
    uint32_t x = (uint32_t)a;
    if (__builtin_expect(!(g_v243a_stflag[x >> 7] & 3u), 1)) { uint16_t w = __builtin_bswap16(v); memcpy(b + x, &w, 2); return; }
    PPU_SLOW_W(16, a, v);
}
static inline void ppu__st32L(uint8_t* b, uint64_t a, uint32_t v) {
    uint32_t x = (uint32_t)a;
    if (__builtin_expect(!(g_v243a_stflag[x >> 7] & 7u), 1)) { uint32_t w = __builtin_bswap32(v); memcpy(b + x, &w, 4); return; }
    PPU_SLOW_W(32, a, v);
}
static inline void ppu__st64L(uint8_t* b, uint64_t a, uint64_t v) {
    uint32_t x = (uint32_t)a;
    if (__builtin_expect(!(g_v243a_stflag[x >> 7] & 3u), 1)) { uint64_t w = __builtin_bswap64(v); memcpy(b + x, &w, 8); return; }
    PPU_SLOW_W(64, a, v);
}
static inline void ppu__stvx16L(uint8_t* b, uint32_t ea, const void* src) {
    uint32_t x = ea & ~0xFu;
    if (__builtin_expect(!(g_v243a_stflag[x >> 7] & 3u), 1)) { memcpy(b + x, src, 16); return; }
    PPU_SLOW_STVX(ea, src);
}
/* ---- end v243A block ---- */
