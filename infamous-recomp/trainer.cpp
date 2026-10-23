/*
 * inFamous in-game trainer, game side.  See trainer.h for what works and rsx_ui_trainer.h for the overlay contract.
 *
 * Addresses (EBOOT BCES00220), all from earlier reverse engineering -- nothing new is assumed here:
 *   player object   = [0x0091032C] (majority vote with 0x007BA080 / 0x007CCE30 / 0x008F7400 / 0x009136BC),
 *                     class 0x00727A28
 *   obj+0x750 / +0x754   f32 max / current health
 *   ModifyHealth(obj, f1 delta, r5 source=0) = guest 0x002AF108 (runs the game's death path)
 *   character registry 0x007BA070 {entries*, count}, 12-byte entries {obj, tick, u8 active}
 *   obj+0x8AC team id, obj+0x8C0 relation table {entries*, count} of {team, relation}; relation <= 2 = hostile
 *
 * Threading: the overlay calls row/status/hud/change on the render thread; they only read guest memory or set
 * atomics.  Every write and every guest call happens in trainer_tick() on the game's main PPU thread.
 */
#include "trainer.h"
#include "rsx_ui_trainer.h"
#include "ppu_recomp.h"
#include "infamous_region.h"

/* The lifted functions called directly below, by their EU names (infamous_region.h). */
#if defined(INFAMOUS_REGION_USA)
#define func_0004E658 func_0004E638
#define func_00051BE0 func_00051BC0
#define func_00200568 func_00200700
#define func_00203008 func_002031A0
#define func_00216548 func_002166D0
#define func_00218060 func_002181E8
#define func_00218F30 func_002190B8
#define func_00274630 func_002747B8
#define func_0027A020 func_0027A1A8
#define func_0027A2C0 func_0027A448
#define func_0027A720 func_0027A8A8
#define func_0027A840 func_0027A9C8
#define func_0027A950 func_0027AAD8
#define func_0027FC18 func_0027FDA0
#define func_002AF108 func_002AF0C8
#define func_003562C0 func_00356230
#define func_004BA678 func_004BA770
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
extern uint8_t* vm_base;
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;          /* ppu_loader.cpp */
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);     /* lifted code */
}

namespace {

/* ---- addresses ------------------------------------------------------------------------------------------------ */
constexpr uint32_t kPlayerSlots[] = { 0x0091032Cu, 0x007BA080u, 0x007CCE30u, 0x008F7400u, 0x009136BCu };
constexpr uint32_t kClassCole     = 0x00727A28u;
constexpr uint32_t kOffHpMax      = 0x750u, kOffHp = 0x754u;
/* Energy, found 2026-10-21: obj+0xED4 f32 current (poke 112 -> the HUD meter showed
 * half; the game copies it to +0xEE0), obj+0xED8 f32 = 225 at full on the test save, used as the maximum.
 * (+0xECC / +0xED0 also read 225 and did not change while draining/recharging.) */
constexpr uint32_t kOffEnergy     = 0xED4u, kOffEnergyMax = 0xED8u;
/* Player progress, a fixed global (3 full dumps with the HUD XP 2424/2425/2477):
 * 0x007D5F9C u32 spendable XP, 0x007D5FA0 u32 total XP earned (same deltas), 0x007D5F98 f32 karma
 * (361.1 unchanged through combat, 374.9 after healing civilians; user confirmed both XP and karma in game). */
constexpr uint32_t kXp            = 0x007D5F9Cu, kXpTotal = 0x007D5FA0u, kKarma = 0x007D5F98u;
constexpr uint32_t kXpStep        = 1000u;
constexpr float    kKarmaStep     = 100.0f;
constexpr uint32_t kOffPos        = 0x90u;
/* Session 3b: Cole's movement component C = [cole+0xF4] (C+0x04 = back-pointer to cole); C+0x10/+0x14/+0x18 =
 * velocity x,y,z cm/s, world.  Writing horizontal velocity every frame moves him (85 m test); vertical is ground-snapped. */
constexpr uint32_t kOffMoveComp   = 0xF4u, kMoveOwner = 0x04u, kMoveVel = 0x10u;
constexpr float    kSpeedMult[]   = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f };
constexpr const char* kSpeedName[] = { "Off", "x1.5", "x2", "x3", "x5" };
constexpr int      kSpeedN        = (int)(sizeof kSpeedMult / sizeof kSpeedMult[0]);
constexpr float    kSpeedMinRun   = 100.0f;     /* cm/s: below this he is standing / turning -- leave it alone */
constexpr float    kSpeedCap      = 8000.0f;    /* cm/s, safety cap on the result */
constexpr int      kStickDead     = 40;         /* left stick deflection (of 128) that counts as 'held' */
constexpr float    kExcessKeep    = 0.46f;      /* share of our extra velocity the game keeps per tick (fit: 0.44-0.49) */
constexpr uint32_t kOffTeam       = 0x8ACu, kOffRel = 0x8C0u;
constexpr uint32_t kCharList      = 0x007BA070u;
constexpr uint32_t kCharEntrySize = 12u;
constexpr int      kCharMax       = 64;
constexpr uint32_t kModifyHealth  = INF_RA(0x002AF108u, 0x002AF0C8u);
constexpr uint32_t kTitleToc      = INFAMOUS_TOC;

/* ---- guest memory helpers ------------------------------------------------------------------------------------- */
inline bool  heap_ptr(uint32_t p)  { return p >= 0x10000000u && p < 0xC0000000u && (p & 3u) == 0; }
inline bool  data_ptr(uint32_t p)  { return p >= 0x00700000u && p < 0x00A00000u && (p & 3u) == 0; }
inline bool  vtable_ok(uint32_t v) { return v >= 0x00700000u && v < 0x00800000u && (v & 3u) == 0; }
inline float rdf(uint32_t a) { uint32_t u = vm_read32(a); float f; std::memcpy(&f, &u, 4); return f; }
inline void  wrf(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); vm_write32(a, u); }

long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/* Cole's character object or 0 (majority vote over the player pointers listed at the top).  Current health may be
 * above max here: the game can lower max under a value the Max Health option raised, and we still need Cole then. */
uint32_t player_object()
{
    if (!vm_base) return 0;
    uint32_t v[5]; int n = 0;
    for (uint32_t s : kPlayerSlots) v[n++] = vm_read32(s);
    uint32_t best = 0; int best_votes = 0;
    for (int i = 0; i < n; ++i) {
        if (!heap_ptr(v[i])) continue;
        int votes = 0;
        for (int j = 0; j < n; ++j) votes += (v[j] == v[i]);
        if (votes > best_votes) { best_votes = votes; best = v[i]; }
    }
    if (!best || best_votes < 2) return 0;
    const float mx = rdf(best + kOffHpMax), cur = rdf(best + kOffHp);
    if (!(mx >= 1.0f && mx <= 1.0e6f) || !(cur >= -1.0f && cur <= 1.0e6f)) return 0;
    return best;
}

/* An actor with a plausible transform (vtable word, finite position, w == 1). */
bool actor_ok(uint32_t obj)
{
    if (!heap_ptr(obj) || !vtable_ok(vm_read32(obj))) return false;
    if (rdf(obj + kOffPos + 12) != 1.0f) return false;
    for (int i = 0; i < 3; ++i) { const float v = rdf(obj + kOffPos + 4u * i); if (!std::isfinite(v) || std::fabs(v) > 1.0e7f) return false; }
    return true;
}

int relation_toward(uint32_t obj, uint32_t team)
{
    const uint32_t tab = vm_read32(obj + kOffRel), n = vm_read32(obj + kOffRel + 4);
    if (!tab || n == 0 || n > 64 || !heap_ptr(tab)) return 6;   /* 6 = neutral, the game's default */
    for (uint32_t i = 0; i < n; ++i)
        if (vm_read32(tab + 8u * i) == team) return (int)vm_read32(tab + 8u * i + 4u);
    return 6;
}

/* ModifyHealth(obj, delta, 0) on a scratch frame below the calling guest thread's stack.
 * Main PPU thread only, inside an HLE call. */
void guest_modify_health(uint32_t obj, float delta)
{
    ppu_context* caller = g_active_ctx;
    if (!caller) return;
    ppu_context ctx;
    std::memcpy(&ctx, caller, sizeof ctx);
    ctx.gpr[1] = ((uint32_t)caller->gpr[1] - 0x4000u) & ~0xFu;
    ctx.gpr[2] = kTitleToc;
    ctx.gpr[3] = obj;
    ctx.gpr[5] = 0;
    ctx.fpr[1] = (double)delta;
    ctx.lr = 0;
    ctx.cia = kModifyHealth;
    ctx.reserve_valid = 0;
    void (*saved_tramp)(void*) = g_trampoline_fn;
    g_trampoline_fn = 0;
    g_active_ctx = &ctx;
    func_002AF108(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&ctx); }
    g_active_ctx = caller;
    g_trampoline_fn = saved_tramp;
}

/* Generic main-thread guest call with r3 = a3 (same context recipe as ModifyHealth above). */
void guest_call1(void (*fn)(ppu_context*), uint32_t cia, uint32_t a3)
{
    ppu_context* caller = g_active_ctx;
    if (!caller || !fn) return;
    ppu_context ctx;
    std::memcpy(&ctx, caller, sizeof ctx);
    ctx.gpr[1] = ((uint32_t)caller->gpr[1] - 0x4000u) & ~0xFu;
    ctx.gpr[2] = kTitleToc;
    ctx.gpr[3] = a3;
    ctx.lr = 0;
    ctx.cia = cia;
    ctx.reserve_valid = 0;
    void (*saved_tramp)(void*) = g_trampoline_fn;
    g_trampoline_fn = 0;
    g_active_ctx = &ctx;
    fn(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&ctx); }
    g_active_ctx = caller;
    g_trampoline_fn = saved_tramp;
}

/* Player Model, step 1: hide Cole's body (session 4b).  The entity refresh func_0027FC18 (called by OnEnable /
 * OnDisable) keeps every entity in two global per-frame lists through slots embedded in the object, {vtable, s32 index}:
 *   obj+0x100 -> list A = the DRAW list, header {array*, count} at [[TOC-0x7C0C]-0x7FD4] + 0x10000 - 0x4828 (0x00914DD8)
 *   obj+0x108 -> list B = the SHADOW list, header at ... - 0x4800 (0x00914E00)
 * Live MEMCMD tests: removing Cole's list-A entry made his body invisible while he kept running, jumping and fighting;
 * removing his list-B entry removed only his shadow.  His power effects are drawn elsewhere and stay (wanted: they will
 * come from the stand-in's hands).  The game
 * compacts a hole on its own and fixes the index of the entry it moves.  Removal here is exactly the refresh's own
 * (entry = 0, index = -1); putting him back = calling the refresh, which re-adds a slot whose index is < 0.
 * (The old A-D OnDisable test: A = obj+0x120 is his collision -- he fell through the world; B/C/D did nothing.) */
constexpr uint32_t kOffDrawSlot   = 0x100u, kOffShadowSlot = 0x108u;
constexpr uint32_t kDrawListLo    = 0x4828u, kShadowListLo  = 0x4800u;   /* header = base + 0x10000 - this */
constexpr uint32_t kRefreshEntity = INF_RA(0x0027FC18u, 0x0027FDA0u);
std::atomic<int> g_hide_cole{0};

uint32_t entity_list_header(uint32_t lo)
{
    const uint32_t t = vm_read32(kTitleToc - 0x7C0Cu);
    if (!data_ptr(t)) return 0;
    const uint32_t b = vm_read32(t - 0x7FD4u);
    if (!data_ptr(b)) return 0;
    return b + 0x10000u - lo;
}

/* Take an entity slot out of one of the two lists.  True when it is out (now or already).  Main thread only. */
bool entity_list_remove(uint32_t lo, uint32_t slot)
{
    const uint32_t hdr = entity_list_header(lo);
    if (!hdr) return false;
    const uint32_t arr = vm_read32(hdr), cnt = vm_read32(hdr + 4);
    if (!heap_ptr(arr) || cnt > 0x4000u) return false;
    const int32_t idx = (int32_t)vm_read32(slot + 4);
    if (idx < 0) return true;
    if ((uint32_t)idx < cnt && vm_read32(arr + 4u * (uint32_t)idx) == slot) vm_write32(arr + 4u * (uint32_t)idx, 0);
    else for (uint32_t i = 0; i < cnt; ++i)                 /* stale index: find the entry */
        if (vm_read32(arr + 4u * i) == slot) { vm_write32(arr + 4u * i, 0); break; }
    vm_write32(slot + 4, 0xFFFFFFFFu);
    return true;
}

/* ---- Player Model, step 2: a pedestrian wears Cole's animation ------------------------------------------------------
 * Skeleton facts (checked 2026-10-22 with paused memory dumps):
 *   char+0xF8 -> world bone matrices B[n] (n = +0xFC), 4x4 f32 row-major, row-vector convention (row 3 = translation);
 *   char+0x238 -> skin palette A[n-1] (+0x23C = n-1, +0x234 = frame stamp of the last rebuild, visible actors only),
 *   A[i] = invBind[i] * B[i+1].  So invBind[i] = A[i] * B[i+1]^-1 (identical across peds of one mesh to 0.03 cm).
 *   Cole (247 bones) and both ped skeletons (208 / 217) share the bone numbering of the body/arms/hands/legs
 *   (first ~116 bones: same orientation in the bind pose, positions differ by proportions); face / extra bones differ.
 * Retarget (constant per bone, built once): ped palette bone i follows Cole bone c(i):
 *   c(i) = i when that bone matches in orientation (< 20 deg) and position (< 16 cm after scaling), else the nearest
 *   Cole bone; M_i = Bind_ped[i] * S * Bind_cole[c]^-1 (S = uniform scale Cole height / ped height, mesh space);
 *   every frame B_ped[i+1] = M_i * B_cole[c+1], B_ped[0] = B_ped[1].  The game rebuilds the ped's palette from that:
 *   A_ped = S * Bind_cole^-1 * B_cole = the ped's mesh skinned like Cole's.
 * The ped's own animation job is skipped (infamous_trainer_anim_hook, called from stubs.cpp infamous_anim_lod_skip),
 * it is posed on Cole's spot each frame, its health is kept full and its
 * collision list (obj+0x120, the same OnDisable step that dropped Cole through the world) is switched off so it
 * cannot push Cole.  Cole is hidden (draw + shadow lists) once both bind poses are known. */
constexpr uint32_t kOffBones = 0xF8u, kOffBoneN = 0xFCu, kOffPalStamp = 0x234u, kOffPal = 0x238u, kOffPalN = 0x23Cu;
constexpr uint32_t kOffAnim = 0x160u, kOffQuat = 0x80u, kOffMtx = 0xB0u, kOffMtxT = 0xE0u, kOffScale = 0x70u;
constexpr uint32_t kOffPhysPos = 0x220u, kOffModelInst = 0x18u, kModelInstId = 0x28u, kOffCollList = 0x120u;
constexpr uint32_t kClassPed = 0x0072A9D8u, kClassEnemy = 0x0072A770u;
constexpr uint32_t kCollOff = INF_RA(0x00200568u, 0x00200700u), kCollOn = INF_RA(0x00203008u, 0x002031A0u);
constexpr int      kMaxBones = 320;
constexpr float    kModelRange = 6000.0f;        /* cm */
constexpr int      kBindTries = 180;             /* ticks to wait for two agreeing palette samples */

struct M4 { double m[4][4]; };
M4 rd_m4(uint32_t a)
{
    M4 r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r.m[i][j] = rdf(a + 16u * i + 4u * j); return r;
}
void wr_m4(uint32_t a, const M4& x)
{
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) wrf(a + 16u * i + 4u * j, (float)x.m[i][j]);
}
M4 mul(const M4& a, const M4& b)
{
    M4 r; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        double s = 0; for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; }
    return r;
}
bool inv4(const M4& a, M4& out)
{
    double m[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { m[i][j] = a.m[i][j]; m[i][j + 4] = (i == j); }
    for (int c = 0; c < 4; ++c) {
        int p = c; for (int r = c + 1; r < 4; ++r) if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (!(std::fabs(m[p][c]) > 1e-12)) return false;
        if (p != c) for (int j = 0; j < 8; ++j) std::swap(m[p][j], m[c][j]);
        const double d = m[c][c]; for (int j = 0; j < 8; ++j) m[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = m[r][c]; if (f != 0) for (int j = 0; j < 8; ++j) m[r][j] -= f * m[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out.m[i][j] = m[i][j + 4];
    return true;
}

uint32_t model_id(uint32_t obj)
{
    const uint32_t mi = vm_read32(obj + kOffModelInst);
    return heap_ptr(mi) ? vm_read32(mi + kModelInstId) : 0;
}

/* Skeleton pointers of a character, or false. */
bool skel(uint32_t obj, uint32_t& bp, int& bn, uint32_t& ap)
{
    bp = vm_read32(obj + kOffBones); ap = vm_read32(obj + kOffPal);
    const uint32_t n = vm_read32(obj + kOffBoneN), an = vm_read32(obj + kOffPalN);
    if (!heap_ptr(bp) || !heap_ptr(ap) || n < 8 || n > (uint32_t)kMaxBones || an != n - 1) return false;
    bn = (int)n; return true;
}

/* Bind capture: invBind[i] = A[i] * B[i+1]^-1, accepted when two samples with different palette stamps agree. */
struct BindCap { uint32_t obj = 0, model = 0, stamp = 0; int state = 0, n = 0, tries = 0; M4 a[kMaxBones]; };
M4 g_bind_tmp[kMaxBones];
bool bind_capture(BindCap& c, uint32_t obj)
{
    if (c.obj != obj) { c.obj = obj; c.model = model_id(obj); c.state = 0; c.tries = 0; }
    if (c.state == 2) return true;
    uint32_t bp, ap; int bn;
    if (!skel(obj, bp, bn, ap)) { ++c.tries; return false; }
    const uint32_t st = vm_read32(obj + kOffPalStamp);
    if (c.state == 1 && st == c.stamp) { ++c.tries; return false; }          /* palette not rebuilt since the last sample */
    bool good = true;
    for (int i = 0; i < bn - 1 && good; ++i) {
        M4 bi; if (!inv4(rd_m4(bp + 64u * (i + 1)), bi)) good = false;
        else g_bind_tmp[i] = mul(rd_m4(ap + 64u * i), bi);
    }
    if (good && c.state == 1 && c.n == bn - 1) {
        double dt = 0, db = 0;
        for (int i = 0; i < c.n; ++i) for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) {
            const double d = std::fabs(g_bind_tmp[i].m[r][k] - c.a[i].m[r][k]);
            if (r == 3) dt = std::max(dt, d); else db = std::max(db, d);
        }
        if (dt < 0.25 && db < 0.002) { c.state = 2; return true; }
    }
    ++c.tries;
    if (!good) { c.state = 0; return false; }
    std::memcpy(c.a, g_bind_tmp, sizeof(M4) * (size_t)(bn - 1));
    c.n = bn - 1; c.stamp = st; c.state = 1;
    return false;
}

constexpr double kHandSpan = 0.70;      /* |y| beyond 70 % of the bind arm span = hand / fingers */
struct Retarget { int n = 0, shared = 0, hand = 0, head = 0; int src[kMaxBones]; M4 off[kMaxBones]; double scale = 1, max_dist = 0; };
M4 g_bind_c[kMaxBones];
/* Head lock (Trish, Alden, John, pedestrians): Cole's head is ONE rigid group of 39 palette bones (64-66, 71, 72, 77, 78,
 * 80, ... -- identical skinning matrices in two paused captures, _xfer/hunt/skel cole2 + 49EAFC40); his jaw (79/81),
 * eyes (82-84), brows (85-90), lids (67-70, 73-76) and bone 113 move on their own.  Ped head / face / hair bones that
 * followed those (by number or by nearest bone) stretched the face down / the hair into a pillar (user screenshots
 * 2026-10-22 17:02).  With the lock every ped bone above the neck (z > 91.5 % of the height; Cole's neck is at 89.9 %,
 * shoulders 79 %) follows palette bone 66 rigidly: the head turns as one piece, no facial animation. */
constexpr int    kColeHeadBone = 66, kColePalN = 246;
constexpr double kHeadZ = 0.915, kHeadY = 20.0;
bool build_retarget(const BindCap& ped, const BindCap& cole, Retarget& r, bool extras_shared_only, bool head_lock)
{
    static M4 bp[kMaxBones];
    double hp = 0, hc = 0;
    for (int i = 0; i < ped.n; ++i)  { if (!inv4(ped.a[i], bp[i])) return false;          hp = std::max(hp, bp[i].m[3][2]); }
    for (int i = 0; i < cole.n; ++i) { if (!inv4(cole.a[i], g_bind_c[i])) return false;   hc = std::max(hc, g_bind_c[i].m[3][2]); }
    if (!(hp > 50 && hp < 500 && hc > 50 && hc < 500)) return false;
    const double s = hc / hp;
    auto unit_rows = [](const M4& m, double out[3][3]) {
        for (int i = 0; i < 3; ++i) { double l = 0; for (int j = 0; j < 3; ++j) l += m.m[i][j] * m.m[i][j];
            l = std::sqrt(l); for (int j = 0; j < 3; ++j) out[i][j] = l > 1e-9 ? m.m[i][j] / l : 0; } };
    r.n = ped.n; r.shared = 0; r.scale = s; r.max_dist = 0; r.hand = 0; r.head = 0;
    if (cole.n != kColePalN) head_lock = false;   /* another Cole mesh: bone 66 unknown */
    auto pos = [&](int i, double p[3]) { for (int k = 0; k < 3; ++k) p[k] = bp[i].m[3][k] * s; };
    auto dist = [&](const double p[3], int c) { double d = 0; for (int k = 0; k < 3; ++k) { const double e = p[k] - g_bind_c[c].m[3][k]; d += e * e; } return std::sqrt(d); };
    /* pass 1: bones with the same number that match in orientation and position follow their Cole twin */
    static bool is_shared[kMaxBones], cole_target[kMaxBones];
    /* Hands: in the T-pose bind the arms lie along +-Y, so the outer part of the span is hand + fingers.  Finger bones
     * pinned to Cole's (differently sized / placed) fingers stretched Trish's hands into claws (session 6e), so hand
     * bones follow the wrist rigidly instead (no finger animation, no stretching). */
    double ymax_p = 0, ymax_c = 0;
    for (int i = 0; i < ped.n; ++i)  ymax_p = std::max(ymax_p, std::fabs(bp[i].m[3][1] * s));
    for (int c = 0; c < cole.n; ++c) ymax_c = std::max(ymax_c, std::fabs(g_bind_c[c].m[3][1]));
    /* the wrist = innermost bone of that outer cluster: it stays a normal (shared) bone, everything 1 cm past it is hand */
    double in_p = ymax_p, in_c = ymax_c;
    for (int i = 0; i < ped.n; ++i)  { const double y = std::fabs(bp[i].m[3][1] * s);   if (y > kHandSpan * ymax_p) in_p = std::min(in_p, y); }
    for (int c = 0; c < cole.n; ++c) { const double y = std::fabs(g_bind_c[c].m[3][1]); if (y > kHandSpan * ymax_c) in_c = std::min(in_c, y); }
    auto hand_p = [&](int i) { return std::fabs(bp[i].m[3][1] * s) > in_p + 1.0; };
    auto hand_c = [&](int c) { return std::fabs(g_bind_c[c].m[3][1]) > in_c + 1.0; };
    int nhand = 0;
    for (int c = 0; c < cole.n; ++c) cole_target[c] = false;
    for (int i = 0; i < ped.n; ++i) {
        is_shared[i] = false;
        if (hand_p(i)) { ++nhand; continue; }
        if (i >= cole.n || hand_c(i)) continue;
        double p[3]; pos(i, p);
        double a[3][3], b[3][3], tr = 0; unit_rows(bp[i], a); unit_rows(g_bind_c[i], b);
        for (int x = 0; x < 3; ++x) for (int y = 0; y < 3; ++y) tr += a[x][y] * b[x][y];
        if ((tr - 1) / 2 > 0.9397 && dist(p, i) < 16.0) { is_shared[i] = true; cole_target[i] = true; ++r.shared; }   /* cos 20 deg */
    }
    /* pass 2: every other bone (face, extras, costume) follows the NEAREST bone rigidly.  Hand bones and Zeke (extras_shared_only):
     * the nearest SHARED bone -- following Cole's own
     * face / extra bones stretched Zeke's head (session 6d screenshot) */
    for (int i = 0; i < ped.n; ++i) {
        double p[3]; pos(i, p);
        int src = is_shared[i] ? i : -1;
        if (head_lock && !hand_p(i) && p[2] > kHeadZ * hc && std::fabs(p[1]) < kHeadY) { src = kColeHeadBone; ++r.head; }
        if (src < 0) {
            double best = 1e30;
            for (int c = 0; c < cole.n; ++c) if (cole_target[c] || r.shared == 0 || (!extras_shared_only && !hand_p(i) && !hand_c(c))) { const double d = dist(p, c); if (d < best) { best = d; src = c; } }
        }
        if (!is_shared[i] && hand_p(i)) { /* not counted in max_dist: a fingertip is far from the wrist by design */ }
        else r.max_dist = std::max(r.max_dist, dist(p, src));
        M4 S{}; S.m[0][0] = S.m[1][1] = S.m[2][2] = s; S.m[3][3] = 1;
        r.src[i] = src;
        r.off[i] = mul(mul(bp[i], S), cole.a[src]);
    }
    r.hand = nhand;
    return true;
}

/* main-thread state */
std::atomic<int>      g_model_req{0};       /* row value: 0 = Cole, 1..kCharsN = kChars, kModelPed = nearby pedestrian */
std::atomic<int>      g_model_status{0};    /* 0 Cole, 3 loading, 1 preparing, 2 live, -1 nobody nearby, -2 failed */
std::atomic<uint32_t> g_model_show{0};      /* model id shown in the row */
int      s_model_k = 0;
uint32_t s_si = 0, s_si_anim = 0, s_cole = 0;
bool     s_si_coll_off = false, s_rt_ok = false, s_si_spawned = false;
BindCap  s_cap_cole, s_cap_ped;
Retarget s_rt;

bool standin_live() { return s_si && s_rt_ok; }

/* The stand-in is FROZEN: every one of its behavior and locomotion/animation updates is skipped by
 * trainer_entity_hook (below), so it cannot walk, fight or animate on its own; render prep still runs.
 * Session 6 in-game test without this: spawned Zeke (enemy class) / Kessler (boss) attacked Cole, kept their own
 * pose beside him and the camera went wild. */
std::atomic<uint32_t> g_frozen_actor{0};
std::atomic<unsigned> g_freeze_skips{0};
bool freeze_hooks_install();
uint32_t s_si_team = 0; bool s_si_team_set = false;    /* stand-in's own team (obj+0x8AC), restored on release */

/* Is the ped still a live, listed character? */
bool char_listed_alive(uint32_t obj)
{
    if (!actor_ok(obj)) return false;
    const uint32_t cls = vm_read32(obj);
    if (cls != kClassPed && cls != kClassEnemy) return false;
    const uint32_t arr = vm_read32(kCharList), cnt = vm_read32(kCharList + 4);
    if (!data_ptr(arr) || cnt > (uint32_t)kCharMax) return false;
    for (uint32_t i = 0; i < cnt; ++i)
        if (vm_read32(arr + kCharEntrySize * i) == obj) return (vm_read32(arr + kCharEntrySize * i + 8) >> 24) != 0;
    return false;
}

/* k-th nearest distinct model (1-based) among live peds / enemies near Cole. */
uint32_t pick_standin(uint32_t cole, int k)
{
    struct C { float d; uint32_t o, m; } c[kCharMax]; int n = 0;
    const uint32_t arr = vm_read32(kCharList), cnt = vm_read32(kCharList + 4);
    if (!data_ptr(arr) || cnt > (uint32_t)kCharMax) return 0;
    const float cx = rdf(cole + kOffPos), cy = rdf(cole + kOffPos + 4), cz = rdf(cole + kOffPos + 8);
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t o = vm_read32(arr + kCharEntrySize * i);
        if (!o || o == cole || !char_listed_alive(o)) continue;
        if (!(rdf(o + kOffHp) > 0.0f)) continue;
        uint32_t bp, ap; int bn; if (!skel(o, bp, bn, ap)) continue;
        const float dx = rdf(o + kOffPos) - cx, dy = rdf(o + kOffPos + 4) - cy, dz = rdf(o + kOffPos + 8) - cz;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < kModelRange) c[n++] = { d, o, model_id(o) };
    }
    for (int i = 1; i < n; ++i) for (int j = i; j > 0 && c[j].d < c[j - 1].d; --j) std::swap(c[j], c[j - 1]);
    int seen = 0; uint32_t models[kCharMax];
    for (int i = 0; i < n; ++i) {
        bool dup = false; for (int j = 0; j < seen; ++j) dup |= models[j] == c[i].m;
        if (dup) continue;
        models[seen++] = c[i].m;
        if (seen == k) return c[i].o;
    }
    return 0;
}

void standin_release(const char* why)
{
    if (s_si) {
        if (s_si_coll_off && actor_ok(s_si)) guest_call1(func_00203008, kCollOn, s_si + kOffCollList);
        std::fprintf(stderr, "[TRAINER] player model: released 0x%08X (%s)\n", s_si, why);
    }
    g_frozen_actor.store(0);
    if (s_si && s_si_team_set && actor_ok(s_si)) vm_write32(s_si + kOffTeam, s_si_team);
    s_si_team_set = false;
    s_si = s_si_anim = 0; s_si_coll_off = false; s_rt_ok = false; s_si_spawned = false; s_cap_ped.obj = 0; s_cap_ped.state = 0; s_cap_ped.tries = 0;
}

/* Put the ped on Cole's feet with Cole's heading. */
void standin_place(uint32_t ped, uint32_t cole)
{
    float p[3]; for (int i = 0; i < 3; ++i) p[i] = rdf(cole + kOffPos + 4u * i);
    const float qx = rdf(cole + kOffQuat), qy = rdf(cole + kOffQuat + 4), qz = rdf(cole + kOffQuat + 8), qw = rdf(cole + kOffQuat + 12);
    const float yaw = std::atan2(2.0f * (qw * qz + qx * qy), 1.0f - 2.0f * (qy * qy + qz * qz));
    const float ox = rdf(ped + kOffPos), oy = rdf(ped + kOffPos + 4), oz = rdf(ped + kOffPos + 8);
    const bool mirror = std::fabs(rdf(ped + kOffPhysPos) - ox) < 100.0f && std::fabs(rdf(ped + kOffPhysPos + 4) - oy) < 100.0f &&
                        std::fabs(rdf(ped + kOffPhysPos + 8) - oz) < 100.0f && rdf(ped + kOffPhysPos + 12) == 1.0f;
    for (int i = 0; i < 3; ++i) {
        wrf(ped + kOffPos + 4u * i, p[i]); wrf(ped + kOffMtxT + 4u * i, p[i]);
        if (mirror) wrf(ped + kOffPhysPos + 4u * i, p[i]);
    }
    const float h = 0.5f * yaw;
    wrf(ped + kOffQuat, 0.0f); wrf(ped + kOffQuat + 4, 0.0f); wrf(ped + kOffQuat + 8, std::sin(h)); wrf(ped + kOffQuat + 12, std::cos(h));
    float sc = rdf(ped + kOffScale); if (!(sc > 0.01f && sc < 100.0f)) sc = 1.0f;
    const float cy = std::cos(yaw) * sc, sy = std::sin(yaw) * sc;
    const float r0[4] = { cy, sy, 0.0f, 0.0f }, r1[4] = { -sy, cy, 0.0f, 0.0f };
    for (int i = 0; i < 4; ++i) { wrf(ped + kOffMtx + 4u * i, r0[i]); wrf(ped + kOffMtx + 0x10u + 4u * i, r1[i]); }
}

/* B_ped[i+1] = M_i * B_cole[c(i)+1], B_ped[0] = B_ped[1], and the skin palette A[i] = invBind[i] * B[i+1] written
 * right away: the frozen stand-in's own update / animation path (which rebuilds the palette for characters -- session 6c
 * test: with all its updates skipped the body stayed in its last pose at the spawn spot while its held gun, which reads
 * B, followed Cole's hand) no longer runs, so the palette is ours to keep current. */
void standin_write_bones()
{
    if (!standin_live() || !s_cole) return;
    uint32_t pb, pa, cb, ca; int pn, cn;
    if (!skel(s_si, pb, pn, pa) || !skel(s_cole, cb, cn, ca) || pn - 1 != s_rt.n || cn - 1 != s_cap_cole.n) return;
    if (s_cap_ped.n != s_rt.n) return;
    for (int i = 0; i < s_rt.n; ++i) {
        const M4 b = mul(s_rt.off[i], rd_m4(cb + 64u * (s_rt.src[i] + 1)));
        wr_m4(pb + 64u * (i + 1), b);
        if (i == 0) wr_m4(pb, b);
        wr_m4(pa + 64u * i, mul(s_cap_ped.a[i], b));
    }
}

/* ---- Player Model, step 3: named game characters (session 6) -------------------------------------------------------
 * _xfer/chars/NOTES_script_re.md (mission script msn_transition_mission_1 decoded: zeke =
 * ObjFind(prxs_mission_zeke).SPAWN().ROOT_OBJECT).  Every named character is its own package; loading is driven by
 * CharSet objects in the registry 0x00907680 (higher priority byte first, 5 packs loaded in total):
 *   init func_0027A020(cs, prio)  register func_0027A720(R, cs)  clear func_0027A2C0(cs)  add func_0027A950(cs, oid)
 *   loaded? func_0027A840(cs)
 * Spawn = the ProxySite prxs_mission_zeke (OID 0x1C51E, in EMPIRE): ObjFind = func_00051BE0([[TOC-0x79E4]-0x8000],
 * oid, 0, 0); site data = [site+0x14] {+0x90 pack OID, +0x94 merged_geom OID, +0x98 merged_phys OID, +0x9C spawn kind};
 * spawn func_00218F30(site) -> ProxyFile (NULL if the pack is not loaded / single-spawn site already used), its
 * character = func_00216548(pf); the SPAWN message handler keeps a reference ([pf+4] += 1; 0 -> func_0004E658).
 * UNSPAWN_ALL = func_00218060(site).  Other characters borrow Zeke's site: their pack/geom/phys OIDs are written into
 * the site data just for the spawn call and restored right after.  Position: func_003562C0(site, v2 = {x,y,z,1}).
 * Guest memory for the CharSet: one 64 KB page from the port's sys_memory_allocate. */
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);   /* ps3recomp runtime/syscalls/sys_memory.c */

struct GameChar { const char* name; uint32_t pack, geom, phys; };
const GameChar kChars[] = {          /* OIDs = 0-based lines of USRDIR/cache/oid_manifest.txt */
    { "Zeke",    0x152u, 0x1C4A4u, 0x1C539u },
    { "Trish",   0x150u, 0x1D5E3u, 0x1D5BEu },
    { "Kessler", 0x153u, 0x21E00u, 0x21DC8u },
    { "Alden",   0x147u, 0x21579u, 0x215B8u },
    { "John",    0x154u, 0x21C29u, 0x21C52u },
};
constexpr int      kCharsN       = (int)(sizeof kChars / sizeof kChars[0]);
constexpr int      kModelPed     = kCharsN + 1;      /* row value: 0 Cole, 1..kCharsN characters, kModelPed nearby ped */
constexpr uint32_t kCharSetReg   = 0x00907680u;
constexpr uint8_t  kCharSetPrio  = 5;                /* the game's own sets use -1 and 3 */
constexpr uint32_t kZekeSite     = 0x1C51Eu;
constexpr int      kLoadTicks    = 60 * 40;          /* ~40 s */

/* Main-thread guest call with r3..r6 (and optionally v2 = 4 floats); returns r3. */
uint32_t guest_call(void (*fn)(ppu_context*), uint32_t cia, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0,
                    const float* v2 = nullptr)
{
    ppu_context* caller = g_active_ctx;
    if (!caller || !fn) return 0;
    ppu_context ctx;
    std::memcpy(&ctx, caller, sizeof ctx);
    ctx.gpr[1] = ((uint32_t)caller->gpr[1] - 0x4000u) & ~0xFu;
    ctx.gpr[2] = kTitleToc;
    ctx.gpr[3] = a3; ctx.gpr[4] = a4; ctx.gpr[5] = a5; ctx.gpr[6] = a6;
    if (v2) for (int i = 0; i < 4; ++i) {            /* vector registers hold guest (big-endian) bytes */
        uint32_t u; std::memcpy(&u, &v2[i], 4);
        uint8_t* p = (uint8_t*)&ctx.vr[2] + 4 * i;
        p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 8); p[3] = (uint8_t)u;
    }
    ctx.lr = 0;
    ctx.cia = cia;
    ctx.reserve_valid = 0;
    void (*saved_tramp)(void*) = g_trampoline_fn;
    g_trampoline_fn = 0;
    g_active_ctx = &ctx;
    fn(&ctx);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; tf(&ctx); }
    g_active_ctx = caller;
    g_trampoline_fn = saved_tramp;
    return (uint32_t)ctx.gpr[3];
}

uint32_t s_page = 0;                 /* trainer's guest page */
uint32_t s_cs = 0;                   /* CharSet (registered once) */
int      s_char = -1;                /* kChars index being loaded / shown, -1 none */
int      s_char_state = 0;           /* 0 none, 1 loading, 2 spawned */
int      s_char_ticks = 0;
uint32_t s_site = 0, s_site_data = 0, s_pf = 0;
uint32_t s_site_orig[3] = { 0, 0, 0 };
std::atomic<int> g_char_err{0};      /* 1 load timeout, 2 no site, 3 spawn failed, 4 no memory */

uint32_t trainer_page()
{
    if (s_page) return s_page;
    ppu_context* caller = g_active_ctx;
    if (!caller) return 0;
    ppu_context ctx; std::memcpy(&ctx, caller, sizeof ctx);
    const uint32_t out = ((uint32_t)caller->gpr[1] - 0x8000u) & ~0xFu;
    vm_write32(out, 0);
    ctx.gpr[3] = 0x10000u; ctx.gpr[4] = 0x200u /* SYS_MEMORY_PAGE_SIZE_64K */; ctx.gpr[5] = out;
    if (sys_memory_allocate(&ctx) == 0) s_page = vm_read32(out);
    std::fprintf(stderr, "[TRAINER] guest page 0x%08X\n", s_page);
    return s_page;
}

uint32_t charset()
{
    if (s_cs) return s_cs;
    const uint32_t pg = trainer_page();
    if (!pg) return 0;
    s_cs = pg;                                         /* 0x60 bytes, 16-aligned */
    for (uint32_t i = 0; i < 0x80u; i += 4) vm_write32(s_cs + i, 0);
    guest_call(func_0027A020, INF_RA(0x0027A020u, 0x0027A1A8u), s_cs, kCharSetPrio);
    guest_call(func_0027A720, INF_RA(0x0027A720u, 0x0027A8A8u), kCharSetReg, s_cs);
    std::fprintf(stderr, "[TRAINER] character set 0x%08X registered (priority %d)\n", s_cs, kCharSetPrio);
    return s_cs;
}

uint32_t obj_find(uint32_t oid)
{
    const uint32_t t = vm_read32(kTitleToc - 0x79E4u);
    if (!data_ptr(t)) return 0;
    const uint32_t tab = vm_read32(t - 0x8000u);
    if (!tab) return 0;
    return guest_call(func_00051BE0, INF_RA(0x00051BE0u, 0x00051BC0u), tab, oid, 0, 0);
}

void char_release(const char* why)
{
    if (s_site && s_char_state == 2) {
        guest_call(func_00218060, INF_RA(0x00218060u, 0x002181E8u), s_site);                       /* UNSPAWN_ALL */
        if (s_pf && heap_ptr(s_pf)) {
            const uint32_t rc = vm_read32(s_pf + 4);
            if (rc > 0 && rc < 0x10000u) {
                vm_write32(s_pf + 4, rc - 1);
                if (rc - 1 == 0) guest_call(func_0004E658, INF_RA(0x0004E658u, 0x0004E638u), s_pf);
            }
        }
    }
    if (s_cs && s_char >= 0) guest_call(func_0027A2C0, INF_RA(0x0027A2C0u, 0x0027A448u), s_cs);   /* nothing requested any more */
    if (s_char >= 0) std::fprintf(stderr, "[TRAINER] character %s released (%s)\n", kChars[s_char].name, why);
    s_char = -1; s_char_state = 0; s_char_ticks = 0; s_pf = 0;
}

/* Returns the spawned character once it exists (0 while loading / on failure). */
uint32_t char_tick(uint32_t cole)
{
    if (s_char < 0 || !cole) return 0;
    const GameChar& c = kChars[s_char];
    if (s_char_state == 1) {
        if (!guest_call(func_0027A840, INF_RA(0x0027A840u, 0x0027A9C8u), s_cs)) {
            if (++s_char_ticks > kLoadTicks) { g_char_err.store(1); std::fprintf(stderr, "[TRAINER] %s: package did not load\n", c.name);
                                               char_release("load timeout"); }
            return 0;
        }
        std::fprintf(stderr, "[TRAINER] %s: package loaded after %d frames\n", c.name, s_char_ticks);
        s_site = obj_find(kZekeSite);
        s_site_data = heap_ptr(s_site) ? vm_read32(s_site + 0x14) : 0;
        if (!heap_ptr(s_site) || !heap_ptr(s_site_data)) {
            g_char_err.store(2); std::fprintf(stderr, "[TRAINER] %s: spawn site prxs_mission_zeke not found (0x%08X)\n", c.name, s_site);
            char_release("no site"); return 0;
        }
        if (!s_site_orig[0]) for (int i = 0; i < 3; ++i) s_site_orig[i] = vm_read32(s_site_data + 0x90u + 4u * i);
        if (vm_read32(s_site + 0xA0)) guest_call(func_00218060, INF_RA(0x00218060u, 0x002181E8u), s_site);       /* site already used */
        const uint32_t want[3] = { c.pack, c.geom, c.phys };
        for (int i = 0; i < 3; ++i) vm_write32(s_site_data + 0x90u + 4u * i, want[i]);
        /* spawn 3 m beside Cole (he hides / the stand-in moves onto him once it is live) */
        float v[4] = { rdf(cole + kOffPos) + 300.0f, rdf(cole + kOffPos + 4), rdf(cole + kOffPos + 8), 1.0f };
        guest_call(func_003562C0, INF_RA(0x003562C0u, 0x00356230u), s_site, 0, 0, 0, v);
        s_pf = guest_call(func_00218F30, INF_RA(0x00218F30u, 0x002190B8u), s_site);
        for (int i = 0; i < 3; ++i) vm_write32(s_site_data + 0x90u + 4u * i, s_site_orig[i]);
        if (!heap_ptr(s_pf)) {
            g_char_err.store(3); std::fprintf(stderr, "[TRAINER] %s: spawn returned 0x%08X\n", c.name, s_pf);
            s_pf = 0; char_release("spawn failed"); return 0;
        }
        vm_write32(s_pf + 4, vm_read32(s_pf + 4) + 1);                               /* hold a reference */
        s_char_state = 2;
        const uint32_t ch = guest_call(func_00216548, INF_RA(0x00216548u, 0x002166D0u), s_pf);
        std::fprintf(stderr, "[TRAINER] %s spawned: ProxyFile 0x%08X character 0x%08X (class 0x%08X, %u bones)\n",
                     c.name, s_pf, ch, heap_ptr(ch) ? vm_read32(ch) : 0, heap_ptr(ch) ? vm_read32(ch + kOffBoneN) : 0);
        return heap_ptr(ch) ? ch : 0;
    }
    if (s_char_state == 2 && s_pf) {
        const uint32_t ch = guest_call(func_00216548, INF_RA(0x00216548u, 0x002166D0u), s_pf);
        return heap_ptr(ch) ? ch : 0;
    }
    return 0;
}

bool char_request(int idx)
{
    char_release("switching");
    g_char_err.store(0);
    const uint32_t cs = charset();
    if (!cs) { g_char_err.store(4); return false; }
    guest_call(func_0027A2C0, INF_RA(0x0027A2C0u, 0x0027A448u), cs);
    if (!guest_call(func_0027A950, INF_RA(0x0027A950u, 0x0027AAD8u), cs, kChars[idx].pack)) {
        std::fprintf(stderr, "[TRAINER] %s: character set refused OID 0x%X\n", kChars[idx].name, kChars[idx].pack);
        g_char_err.store(1); return false;
    }
    s_char = idx; s_char_state = 1; s_char_ticks = 0;
    std::fprintf(stderr, "[TRAINER] %s: package 0x%X requested\n", kChars[idx].name, kChars[idx].pack);
    return true;
}

bool spawned_ok(uint32_t obj)
{
    uint32_t bp, ap; int bn;
    return actor_ok(obj) && skel(obj, bp, bn, ap);
}

void model_fail(const char* why) { standin_release(why); char_release(why); g_model_status.store(-2); }

void model_tick(uint32_t cole)
{
    s_cole = cole;
    const int want = g_model_req.load(std::memory_order_relaxed);
    if (want != s_model_k) {
        standin_release("model changed");
        char_release("model changed");
        s_model_k = want;
        g_model_status.store(0);
        if (want >= 1 && want <= kCharsN) {
            if (cole && char_request(want - 1)) g_model_status.store(3); else g_model_status.store(-2);
        } else if (want == kModelPed && cole) {
            s_si = pick_standin(cole, 1); s_si_spawned = false;
            if (s_si) {
                g_model_show.store(model_id(s_si)); g_model_status.store(1);
                std::fprintf(stderr, "[TRAINER] player model: ped 0x%08X model 0x%X -- capturing bind poses\n", s_si, model_id(s_si));
            } else { g_model_status.store(-1); std::fprintf(stderr, "[TRAINER] player model: nobody nearby\n"); }
        }
    }
    if (s_char >= 0 && !s_si) {                          /* named character: loading / spawning */
        const uint32_t ch = char_tick(cole);
        if (ch) {
            s_si = ch; s_si_spawned = true; g_model_show.store(model_id(ch)); g_model_status.store(1);
        } else if (s_char < 0) g_model_status.store(-2); /* char_tick gave up */
        return;
    }
    if (!s_si) return;
    if (!cole || !(s_si_spawned ? spawned_ok(s_si) : char_listed_alive(s_si))) { model_fail(cole ? "character gone" : "Cole not loaded"); return; }
    if (g_frozen_actor.load(std::memory_order_relaxed) != s_si && freeze_hooks_install()) {
        g_frozen_actor.store(s_si);
        std::fprintf(stderr, "[TRAINER] player model: 0x%08X frozen (all its updates skipped)\n", s_si);
    }
    if (!s_si_team_set) {                                 /* on Cole's side: not a target for his aim / lock-on */
        s_si_team = vm_read32(s_si + kOffTeam); s_si_team_set = true;
        vm_write32(s_si + kOffTeam, vm_read32(cole + kOffTeam));
    }

    if (!s_rt_ok) {
        /* Cole's bind pose is kept across respawns (same mesh = same model id and bone count) */
        bool c_ok = s_cap_cole.state == 2 && s_cap_cole.model == model_id(cole);
        if (c_ok) { uint32_t bp, ap; int bn; c_ok = skel(cole, bp, bn, ap) && bn - 1 == s_cap_cole.n; s_cap_cole.obj = cole; }
        if (!c_ok) c_ok = bind_capture(s_cap_cole, cole);
        const bool p_ok = bind_capture(s_cap_ped, s_si);
        if (!(c_ok && p_ok)) {
            if (s_cap_ped.tries > kBindTries || (!c_ok && s_cap_cole.tries > kBindTries)) {
                std::fprintf(stderr, "[TRAINER] player model: bind pose not stable (Cole %d tries, model %d)\n", s_cap_cole.tries, s_cap_ped.tries);
                s_cap_cole.tries = 0; model_fail("bind capture failed");
            }
            return;
        }
        /* user 2026-10-22: the shared-only head fix (session 6d) suits Zeke but stretched Kessler/Trish heads -> Zeke only */
        if (!build_retarget(s_cap_ped, s_cap_cole, s_rt, s_char == 0, s_char != 0 && s_char != 2)) {   /* head lock: not Zeke / Kessler */ model_fail("retarget failed"); return; }
        s_rt_ok = true;
        s_si_anim = vm_read32(s_si + kOffAnim);
        guest_call1(func_00200568, kCollOff, s_si + kOffCollList); s_si_coll_off = true;
        g_model_status.store(2);
        std::fprintf(stderr, "[TRAINER] player model LIVE: 0x%08X (%d bones) wears Cole (%d): %d shared bones (%d hand bones on the wrist, %d on the head), "
                     "scale %.3f, worst match %.1f cm\n", s_si, s_rt.n + 1, s_cap_cole.n + 1, s_rt.shared, s_rt.hand, s_rt.head, s_rt.scale, s_rt.max_dist);
    }
    standin_place(s_si, cole);
    const float mx = rdf(s_si + kOffHpMax);
    if (mx > 0.0f && mx < 1.0e6f && rdf(s_si + kOffHp) > 0.0f) wrf(s_si + kOffHp, mx);
    standin_write_bones();
}

/* ---- trainer state -------------------------------------------------------------------------------------------- */
constexpr int kMaxHpMult[] = { 1, 2, 5, 10 };
constexpr const char* kMaxHpName[] = { "Game", "x2", "x5", "x10" };
constexpr int kMaxHpN = (int)(sizeof kMaxHpMult / sizeof kMaxHpMult[0]);

enum { KILL_ENEMIES = 1, KILL_CIVILIANS = 2 };

std::atomic<int> g_god{0};
std::atomic<int> g_energy{0};
std::atomic<int> g_speed{0};          /* index into kSpeedMult */
std::atomic<int> g_maxhp{0};          /* index into kMaxHpMult */
std::atomic<int> g_req_refill{0};
std::atomic<int> g_req_kill{0};       /* KILL_* bits */
std::atomic<int> g_req_xp{0};         /* number of +kXpStep presses */
std::atomic<int> g_req_karma{0};      /* net +/- kKarmaStep presses */
std::atomic<int> g_res_xp{0};         std::atomic<long long> g_res_xp_ms{0};
/* results of the last one-shot actions, shown in the row for a few seconds */
std::atomic<int> g_res_refill{0};     std::atomic<long long> g_res_refill_ms{0};
std::atomic<int> g_res_enemies{0};    std::atomic<long long> g_res_enemies_ms{0};
std::atomic<int> g_res_civilians{0};  std::atomic<long long> g_res_civilians_ms{0};

/* ---- main-thread work ----------------------------------------------------------------------------------------- */
int kill_matching(int which)
{
    const uint32_t me = player_object();
    const uint32_t my_team = me ? vm_read32(me + kOffTeam) : 144u;   /* 144 in every dump */
    const uint32_t arr = vm_read32(kCharList), cnt = vm_read32(kCharList + 4);
    if (!data_ptr(arr) || cnt > (uint32_t)kCharMax) return 0;
    int killed = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        const uint32_t e = arr + kCharEntrySize * i;
        const uint32_t obj = vm_read32(e);
        if (!obj || obj == me || !actor_ok(obj) || vm_read32(obj) == kClassCole) continue;
        if ((vm_read32(e + 8) >> 24) == 0) continue;                /* inactive slot */
        const float mx = rdf(obj + kOffHpMax), cur = rdf(obj + kOffHp);
        if (!(mx > 0.0f && mx < 1.0e5f) || !(cur > 0.0f)) continue;  /* not a health owner / already dead */
        const bool hostile = relation_toward(obj, my_team) <= 2;
        if (!((hostile && (which & KILL_ENEMIES)) || (!hostile && (which & KILL_CIVILIANS)))) continue;
        guest_modify_health(obj, -(cur + 1000.0f));
        ++killed;
    }
    return killed;
}

void trainer_tick(void)
{
    if (!vm_base) return;
    const uint32_t obj = player_object();

    /* Max health multiplier.  s_base = the game's own max.  If the game changes max while we hold it: a LOWER value
     * means it set max outright (respawn / recompute) -> that is the new base; a HIGHER value means it added to ours
     * (upgrade) -> add the same amount to the base, so the multiplier does not compound. */
    static uint32_t s_obj = 0;
    static float s_base = 0.0f, s_written = 0.0f;
    if (obj != s_obj) { s_obj = obj; s_base = s_written = 0.0f; }
    if (obj) {
        const int k = kMaxHpMult[g_maxhp.load(std::memory_order_relaxed)];
        const float mx = rdf(obj + kOffHpMax);
        if (k > 1) {
            if (s_base <= 0.0f) s_base = mx;
            else if (s_written > 0.0f && mx != s_written) s_base = mx < s_written ? mx : s_base + (mx - s_written);
            const float want = s_base * (float)k;
            if (mx != want) {
                wrf(obj + kOffHpMax, want);
                std::fprintf(stderr, "[TRAINER] max health %.0f -> %.0f (x%d)\n", mx, want, k);
            }
            s_written = want;
        } else if (s_base > 0.0f) {
            if (mx == s_written) {
                wrf(obj + kOffHpMax, s_base);
                if (rdf(obj + kOffHp) > s_base) wrf(obj + kOffHp, s_base);
                std::fprintf(stderr, "[TRAINER] max health back to %.0f\n", s_base);
            }
            s_base = s_written = 0.0f;
        }
    }

    /* God mode / refill: top current health up to max.  Never "revives" (cur <= 0 means the game's death path ran). */
    const bool refill = g_req_refill.exchange(0, std::memory_order_relaxed) != 0;
    if (obj && (refill || g_god.load(std::memory_order_relaxed))) {
        const float mx = rdf(obj + kOffHpMax), cur = rdf(obj + kOffHp);
        if (cur > 0.0f && cur != mx) wrf(obj + kOffHp, mx);   /* also pulls a value above max back down */
        if (refill) {
            g_res_refill.store(cur > 0.0f ? 1 : -1); g_res_refill_ms.store(now_ms());
            std::fprintf(stderr, "[TRAINER] refill %.0f -> %.0f\n", cur, rdf(obj + kOffHp));
        }
    } else if (refill) { g_res_refill.store(-2); g_res_refill_ms.store(now_ms()); }

    /* Infinite energy: top current energy up to the max field next to it. */
    if (obj && g_energy.load(std::memory_order_relaxed)) {
        const float emx = rdf(obj + kOffEnergyMax), e = rdf(obj + kOffEnergy);
        if (emx >= 1.0f && emx <= 100000.0f && std::isfinite(e) && e < emx) wrf(obj + kOffEnergy, emx);
    }

    /* Super speed lives in the movement hooks below (infamous_trainer_move_pre / _set), not in this tick. */

    /* Player Model: the stand-in ped (captures bind poses first, then follows Cole every frame). */
    model_tick(obj);

    /* Hide Cole (Player Model step 1): keep his draw slot out of the draw list while the row is on (the game re-adds it
     * on respawn / refresh -- removed again next frame).  Off: the game's refresh puts him back.  Also on while a
     * stand-in is live; forced visible while Cole's bind pose is being captured (his palette only updates when drawn). */
    {
        static uint32_t s_hidden = 0;                         /* Cole object we took out of the draw list */
        const bool capturing_cole = s_si && !s_rt_ok && !(s_cap_cole.state == 2 && obj && s_cap_cole.model == model_id(obj));
        const bool hide = (g_hide_cole.load(std::memory_order_relaxed) || standin_live()) && !capturing_cole;
        if (obj && hide) {
            const bool body = entity_list_remove(kDrawListLo, obj + kOffDrawSlot);
            const bool shadow = entity_list_remove(kShadowListLo, obj + kOffShadowSlot);
            if (body && shadow && s_hidden != obj) {
                s_hidden = obj;
                std::fprintf(stderr, "[TRAINER] Cole hidden (obj 0x%08X)\n", obj);
            }
        } else if (obj && s_hidden) {
            if (obj == s_hidden && ((int32_t)vm_read32(obj + kOffDrawSlot + 4) < 0 ||
                                    (int32_t)vm_read32(obj + kOffShadowSlot + 4) < 0)) {
                guest_call1(func_0027FC18, kRefreshEntity, obj);
                std::fprintf(stderr, "[TRAINER] Cole visible again (refresh, draw index %d, shadow index %d)\n",
                             (int)(int32_t)vm_read32(obj + kOffDrawSlot + 4), (int)(int32_t)vm_read32(obj + kOffShadowSlot + 4));
            }
            s_hidden = 0;                                     /* a new Cole (respawn) was never hidden */
        }
    }

    /* XP: both the spendable and the lifetime counter, so the powers menu stays consistent. */
    if (const int n = g_req_xp.exchange(0, std::memory_order_relaxed)) {
        const uint32_t xp = vm_read32(kXp), tot = vm_read32(kXpTotal);
        if (xp < 10000000u && tot < 100000000u && tot >= xp) {
            vm_write32(kXp, xp + kXpStep * (uint32_t)n); vm_write32(kXpTotal, tot + kXpStep * (uint32_t)n);
            g_res_xp.store((int)(xp + kXpStep * (uint32_t)n)); g_res_xp_ms.store(now_ms());
            std::fprintf(stderr, "[TRAINER] XP %u -> %u (total %u -> %u)\n", xp, vm_read32(kXp), tot, vm_read32(kXpTotal));
        } else { g_res_xp.store(-1); g_res_xp_ms.store(now_ms());
                 std::fprintf(stderr, "[TRAINER] XP not changed: implausible values %u / %u\n", xp, tot); }
    }
    if (const int n = g_req_karma.exchange(0, std::memory_order_relaxed)) {
        const float k = rdf(kKarma);
        if (std::isfinite(k) && std::fabs(k) < 100000.0f) {
            wrf(kKarma, k + kKarmaStep * (float)n);
            std::fprintf(stderr, "[TRAINER] karma (0x%08X) %.1f -> %.1f\n", kKarma, k, rdf(kKarma));
        } else std::fprintf(stderr, "[TRAINER] karma not changed: implausible value %f\n", k);
    }

    /* Kills go through the game's ModifyHealth so deaths, ragdolls and karma events play out normally. */
    if (const int which = g_req_kill.exchange(0, std::memory_order_relaxed)) {
        if (which & KILL_ENEMIES) {
            const int n = kill_matching(KILL_ENEMIES);
            g_res_enemies.store(n); g_res_enemies_ms.store(now_ms());
            std::fprintf(stderr, "[TRAINER] killed %d enemies\n", n);
        }
        if (which & KILL_CIVILIANS) {
            const int n = kill_matching(KILL_CIVILIANS);
            g_res_civilians.store(n); g_res_civilians_ms.store(now_ms());
            std::fprintf(stderr, "[TRAINER] killed %d civilians\n", n);
        }
    }
}

/* ---- rows ----------------------------------------------------------------------------------------------------- */
enum Row {
    R_H_PLAYER, R_GOD, R_REFILL, R_MAXHP, R_ENERGY,
    R_H_MOVE, R_JUMP, R_SPEED, R_TELEPORT,
    R_H_PROGRESS, R_XP, R_KARMA_GOOD, R_KARMA_EVIL,
    R_H_LOOK, R_MODEL, R_HIDE_COLE,
    R_H_WORLD, R_KILL_ENEMIES, R_KILL_CIVILIANS,
    R_N
};

const char* kLocked = "not mapped yet";

void set(uiov_trainer_row* o, int kind, const char* label, const char* value = "", int on = 0)
{
    o->kind = kind; o->on = on;
    std::snprintf(o->label, sizeof o->label, "%s", label);
    std::snprintf(o->value, sizeof o->value, "%s", value);
}

/* "killed 4" for 4 s after an action ran */
bool recent(const std::atomic<long long>& ms) { const long long t = ms.load(); return t && now_ms() - t < 4000; }

int api_count(void) { return R_N; }

int api_row(int i, uiov_trainer_row* o)
{
    char v[64];
    switch (i) {
    case R_H_PLAYER:   set(o, UIOV_TR_HEADER, "Player"); return 1;
    case R_GOD:        { const int on = g_god.load(); set(o, UIOV_TR_CHOICE, "God Mode", on ? "On" : "Off", on); return 1; }
    case R_REFILL:
        if (recent(g_res_refill_ms)) { const int r = g_res_refill.load();
            set(o, UIOV_TR_ACTION, "Refill Health", r > 0 ? "done" : r == -1 ? "Cole is down" : "Cole not loaded"); }
        else set(o, UIOV_TR_ACTION, "Refill Health");
        return 1;
    case R_MAXHP:      { const int k = g_maxhp.load(); set(o, UIOV_TR_CHOICE, "Max Health", kMaxHpName[k], k != 0); return 1; }
    case R_ENERGY:     { const int on = g_energy.load(); set(o, UIOV_TR_CHOICE, "Infinite Energy", on ? "On" : "Off", on); return 1; }
    case R_H_MOVE:     set(o, UIOV_TR_HEADER, "Movement"); return 1;
    case R_JUMP:       set(o, UIOV_TR_LOCKED, "Super Jump", kLocked); return 1;
    case R_SPEED:      { const int k = g_speed.load(); set(o, UIOV_TR_CHOICE, "Super Speed", kSpeedName[k], k != 0); return 1; }
    case R_TELEPORT:   set(o, UIOV_TR_LOCKED, "Teleport to Waypoint", kLocked); return 1;
    case R_H_PROGRESS: set(o, UIOV_TR_HEADER, "Progression"); return 1;
    case R_XP:
        if (recent(g_res_xp_ms)) { const int r = g_res_xp.load();
            if (r >= 0) std::snprintf(v, sizeof v, "now %d", r); else std::snprintf(v, sizeof v, "not loaded");
            set(o, UIOV_TR_ACTION, "Add 1000 XP", v); }
        else set(o, UIOV_TR_ACTION, "Add 1000 XP");
        return 1;
    case R_KARMA_GOOD: set(o, UIOV_TR_ACTION, "Karma +100 (Good)"); return 1;
    case R_KARMA_EVIL: set(o, UIOV_TR_ACTION, "Karma -100 (Evil)"); return 1;
    case R_H_LOOK:     set(o, UIOV_TR_HEADER, "Appearance"); return 1;
    case R_MODEL: {
        const int k = g_model_req.load(), st = g_model_status.load();
        const char* nm = k == 0 ? "Cole" : k <= kCharsN ? kChars[k - 1].name : "Nearby pedestrian";
        static const char* kErr[] = { "failed", "did not load", "no spawn site", "spawn failed", "no memory" };
        const int e = g_char_err.load();
        if (k == 0 || st == 2) std::snprintf(v, sizeof v, "%s", nm);
        else if (st == 3) std::snprintf(v, sizeof v, "%s  loading...", nm);
        else if (st == 1) std::snprintf(v, sizeof v, "%s  preparing...", nm);
        else if (st == -1) std::snprintf(v, sizeof v, "%s  nobody nearby", nm);
        else if (st == -2) std::snprintf(v, sizeof v, "%s  %s", nm, kErr[(e >= 0 && e <= 4) ? e : 0]);
        else std::snprintf(v, sizeof v, "%s", nm);
        set(o, UIOV_TR_CHOICE, "Player Model", v, k != 0); return 1; }
    case R_HIDE_COLE:  { const int on = g_hide_cole.load(); set(o, UIOV_TR_CHOICE, "Hide Cole", on ? "On" : "Off", on); return 1; }
    case R_H_WORLD:    set(o, UIOV_TR_HEADER, "World"); return 1;
    case R_KILL_ENEMIES:
        if (recent(g_res_enemies_ms)) { std::snprintf(v, sizeof v, "killed %d", g_res_enemies.load()); set(o, UIOV_TR_ACTION, "Kill Enemies", v); }
        else set(o, UIOV_TR_ACTION, "Kill Enemies");
        return 1;
    case R_KILL_CIVILIANS:
        if (recent(g_res_civilians_ms)) { std::snprintf(v, sizeof v, "killed %d", g_res_civilians.load()); set(o, UIOV_TR_ACTION, "Kill Civilians", v); }
        else set(o, UIOV_TR_ACTION, "Kill Civilians");
        return 1;
    default: return 0;
    }
}

void api_change(int i, int dir)
{
    switch (i) {
    case R_GOD: {
        const int v = !g_god.load(); g_god.store(v);
        std::fprintf(stderr, "[TRAINER] god mode %s\n", v ? "ON" : "OFF");
        break; }
    case R_ENERGY: {
        const int v = !g_energy.load(); g_energy.store(v);
        std::fprintf(stderr, "[TRAINER] infinite energy %s\n", v ? "ON" : "OFF");
        break; }
    case R_MAXHP: {
        const int d = dir < 0 ? -1 : 1;
        g_maxhp.store((g_maxhp.load() + d + kMaxHpN) % kMaxHpN);
        break; }
    case R_SPEED: {
        const int d = dir < 0 ? -1 : 1;
        const int v = (g_speed.load() + d + kSpeedN) % kSpeedN; g_speed.store(v);
        std::fprintf(stderr, "[TRAINER] super speed %s\n", kSpeedName[v]);
        break; }
    case R_MODEL: {
        const int d = dir < 0 ? -1 : 1;
        const int v = (g_model_req.load() + d + kModelPed + 1) % (kModelPed + 1); g_model_req.store(v);
        std::fprintf(stderr, "[TRAINER] player model -> %d\n", v);
        break; }
    case R_HIDE_COLE: {
        const int v = !g_hide_cole.load(); g_hide_cole.store(v);
        std::fprintf(stderr, "[TRAINER] hide Cole %s\n", v ? "ON" : "OFF");
        break; }
    case R_REFILL:         if (dir == 0) g_req_refill.store(1); break;
    case R_XP:             if (dir == 0) g_req_xp.fetch_add(1); break;
    case R_KARMA_GOOD:     if (dir == 0) g_req_karma.fetch_add(1); break;
    case R_KARMA_EVIL:     if (dir == 0) g_req_karma.fetch_sub(1); break;
    case R_KILL_ENEMIES:   if (dir == 0) g_req_kill.fetch_or(KILL_ENEMIES); break;
    case R_KILL_CIVILIANS: if (dir == 0) g_req_kill.fetch_or(KILL_CIVILIANS); break;
    default: break;
    }
}

int api_status(char* t, size_t cap)
{
    const uint32_t obj = player_object();
    if (!obj) { std::snprintf(t, cap, "Cole not loaded yet"); return 1; }
    std::snprintf(t, cap, "Health  %.0f / %.0f     Energy  %.0f / %.0f     XP  %u", rdf(obj + kOffHp), rdf(obj + kOffHpMax),
                  rdf(obj + kOffEnergy), rdf(obj + kOffEnergyMax), vm_read32(kXp));
    return 1;
}

int api_hud(char* t, size_t cap)
{
    const int god = g_god.load(), k = g_maxhp.load(), en = g_energy.load(), sp = g_speed.load(), hc = g_hide_cole.load();
    const int md = g_model_req.load() != 0 && g_model_status.load() == 2;
    if (!god && !k && !en && !sp && !hc && !md) return 0;
    char mdl[48] = "";
    if (md) { const int mk = g_model_req.load();
              std::snprintf(mdl, sizeof mdl, "  Playing as %s", mk <= kCharsN ? kChars[mk - 1].name : "pedestrian"); }
    std::snprintf(t, cap, "TRAINER%s%s%s%s%s%s%s%s", god ? "  God Mode" : "", en ? "  Infinite Energy" : "",
                  k ? "  Max Health " : "", k ? kMaxHpName[k] : "", sp ? "  Speed " : "", sp ? kSpeedName[sp] : "",
                  hc ? "  Cole hidden" : "", mdl);
    return 1;
}

const uiov_trainer_api kApi = { "TRAINER", api_count, api_row, api_change, api_status, api_hud };

} // namespace

/* ---- Super Speed: hooks in Cole's movement update ------------------------------------------------------------------
 * func_002B46B8(r3, r4, r5 = movement component C) reads C's previous velocity, blends toward the animation's desired
 * velocity and hands the result to the setter func_00353320(r3 = C, v2 = velocity) at 0x002B4EA4 (found with the
 * vector-store write watch, session 3d).  Poking C+0x10 from outside fed the boost back into that blend every frame
 * (runaway, no steering).  So: the setter stores k x the game's natural velocity (physics moves him with that), and
 * at the next entry of func_002B46B8 the natural value is put back, so the blend / steering / braking only ever see
 * normal numbers.  Both call sites are patched into recompiled/ppu_recomp_003.cpp (re-apply after a re-lift). */
namespace {
uint32_t s_mv_c = 0; bool s_mv_act = false; float s_mv_n[2] = {0, 0}, s_mv_w[2] = {0, 0};
uint32_t cole_move_comp()
{
    const uint32_t obj = player_object();
    if (!obj) return 0;
    const uint32_t c = vm_read32(obj + kOffMoveComp);
    return (heap_ptr(c) && vm_read32(c + kMoveOwner) == obj) ? c : 0;
}
inline float vr_get(const ppu_context* ctx, int r, int i)
{
    const uint8_t* p = (const uint8_t*)&ctx->vr[r] + 4 * i;
    const uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    float f; std::memcpy(&f, &u, 4); return f;
}
inline void vr_set(ppu_context* ctx, int r, int i, float f)
{
    uint32_t u; std::memcpy(&u, &f, 4);
    uint8_t* p = (uint8_t*)&ctx->vr[r] + 4 * i;
    p[0] = (uint8_t)(u >> 24); p[1] = (uint8_t)(u >> 16); p[2] = (uint8_t)(u >> 8); p[3] = (uint8_t)u;
}
} // namespace

extern "C" void infamous_trainer_move_pre(ppu_context* ctx)
{
    if (!s_mv_act) return;
    const uint32_t c = (uint32_t)ctx->gpr[5];
    if (c != s_mv_c) return;
    if (rdf(c + kMoveVel) == s_mv_w[0] && rdf(c + kMoveVel + 4) == s_mv_w[1]) {
        wrf(c + kMoveVel, s_mv_n[0]); wrf(c + kMoveVel + 4, s_mv_n[1]);
    }
    s_mv_act = false;
}

extern "C" void infamous_trainer_move_set(ppu_context* ctx)
{
    const int k = g_speed.load(std::memory_order_relaxed);
    if (k <= 0) { s_mv_act = false; return; }
    const uint32_t c = (uint32_t)ctx->gpr[3];
    if (!c || c != cole_move_comp()) return;
    const float vx = vr_get(ctx, 2, 0), vy = vr_get(ctx, 2, 1);
    const float sp = std::sqrt(vx * vx + vy * vy);
    if (!std::isfinite(sp) || sp <= kSpeedMinRun) { s_mv_act = false; return; }
    float m = kSpeedMult[k];
    if (sp * m > kSpeedCap) m = kSpeedCap / sp;
    s_mv_c = c; s_mv_n[0] = vx; s_mv_n[1] = vy; s_mv_w[0] = vx * m; s_mv_w[1] = vy * m; s_mv_act = true;
    vr_set(ctx, 2, 0, s_mv_w[0]); vr_set(ctx, 2, 1, s_mv_w[1]);
}

/* ---- freeze hook: the game's per-entity update.  func_00274630(entity) and its this-adjust thunk
 * func_004BA678 (r3 = entity + 0x68) are re-registered; only virtual calls (bctrl, target in ctr) come through here.
 * Entity +0x7C = its character. */
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));   /* ppu_loader.cpp */
namespace {
constexpr uint32_t kFnEntityUpdate = INF_RA(0x00274630u, 0x002747B8u), kFnEntityThunk = INF_RA(0x004BA678u, 0x004BA770u), kThunkAdj = 0x68u, kEntActor = 0x7Cu;
constexpr uint32_t kSigThunk0 = 0x3863FF98u /* addi r3,r3,-104 */, kSigThunk1 = INF_RA(0x4BDB9FB4u, 0x4BDBA044u) /* b kFnEntityUpdate */;
constexpr uint32_t kSigUpdate0 = 0x7D800026u /* mfcr r12 */;
constexpr uint32_t kOffUpdEnt2 = 0x358u;      /* group 2 = locomotion / physics / animation feed */
void trainer_entity_hook(ppu_context* ctx)
{
    const bool thunk = (uint32_t)ctx->ctr == kFnEntityThunk;
    const uint32_t fz = g_frozen_actor.load(std::memory_order_relaxed);
    if (fz) {
        const uint32_t E = (uint32_t)ctx->gpr[3] - (thunk ? kThunkAdj : 0u);
        if (heap_ptr(E) && vm_read32(E + kEntActor) == fz) {
            g_freeze_skips.fetch_add(1, std::memory_order_relaxed);
            if (vm_read32(fz + kOffUpdEnt2) == E) standin_write_bones();   /* our pose at the game's update time */
            return;                                                         /* the caller does not use r3 */
        }
    }
    if (thunk) func_004BA678(ctx); else func_00274630(ctx);
}
bool freeze_hooks_install()
{
    static int s = 0;
    if (s) return s > 0;
    if (vm_read32(kFnEntityThunk) != kSigThunk0 || vm_read32(kFnEntityThunk + 4) != kSigThunk1 ||
        vm_read32(kFnEntityUpdate) != kSigUpdate0) {
        std::fprintf(stderr, "[TRAINER] freeze hook NOT installed: EBOOT words differ (0x%08X 0x%08X 0x%08X)\n",
                     vm_read32(kFnEntityThunk), vm_read32(kFnEntityThunk + 4), vm_read32(kFnEntityUpdate));
        s = -1; return false;
    }
    ppu_register_function(kFnEntityThunk, trainer_entity_hook);
    ppu_register_function(kFnEntityUpdate, trainer_entity_hook);
    s = 1;
    std::fprintf(stderr, "[TRAINER] freeze hook installed on the entity update\n");
    return true;
}
} // namespace

/* stubs.cpp infamous_anim_lod_skip, at the entry of func_000285A0(anim) on the main thread: the stand-in's own animation
 * job is not kicked ("done"); its bones are Cole's, written here and in the tick.  1 = skip. */
extern "C" int infamous_trainer_anim_hook(uint32_t anim)
{
    if (!anim || anim != s_si_anim || !standin_live()) return 0;
    standin_write_bones();
    return 1;
}

extern "C" void infamous_trainer_install(void)
{
    const char* e = std::getenv("TRAINER");
    if (e && *e == '0') { std::fprintf(stderr, "[TRAINER] off (TRAINER=0)\n"); return; }
    g_pad_title_tick_fn = trainer_tick;
    g_uiov_trainer = &kApi;
    std::fprintf(stderr, "[TRAINER] installed (F10)\n");
}
