// R11 bosses of the deep missions (docs/DESIGN-R11.md 2, 3, 6): the table, the burst routine, the shot pool, the
// boss state machine and the hit on the hero. The hero's own decision (ACT_BOSS) is chunk C; see boss.h.
#include "boss.h"
#include "delve.h"

BossShot g_bshot[BOSS_SHOTS + BOSS_GUARD];
#if !defined(ARDUINO_ARCH_ESP8266)
unsigned g_dbg_boss_work, g_dbg_boss_forced;
#endif

enum { K_BURST, K_CHARGE, K_HOP };
enum { TIRE_MS = 70000, END_MS = 110000, REST_MS = 2000, STUN_MS = 1400, FLASH_MS = 240 };

// Shared attacks, 2 bytes each. b0: kind | mask << 2 | aim << 5 (0 at the hero, 1 fixed dir 0, 2 fixed dir 2,
// 3 turns one step per use). b1: telegraph in 100 ms | speed index << 4 | life index << 6.
#define AT(kind, mask, aim, tele, sp, lf) {(uint8_t)((kind) | (mask) << 2 | (aim) << 5), (uint8_t)((tele) | (sp) << 4 | (lf) << 6)}
static const uint8_t ATK[12][2] GC_ROM_ATTR = {
  AT(K_BURST, 0, 0, 4, 1, 0),   // SHOT    30 px/s
  AT(K_BURST, 0, 0, 5, 3, 0),   // BOLT    60
  AT(K_BURST, 1, 0, 5, 2, 0),   // SPREAD  42
  AT(K_BURST, 2, 0, 6, 1, 0),   // FAN     30
  AT(K_BURST, 4, 1, 6, 1, 0),   // RING    30
  AT(K_BURST, 4, 3, 3, 2, 0),   // SPIRAL  42, turns
  AT(K_BURST, 3, 1, 6, 3, 0),   // CROSS   60
  AT(K_BURST, 3, 2, 6, 3, 0),   // XSHOT   60, diagonals
  AT(K_BURST, 4, 1, 9, 3, 1),   // SLAM    60, 12 steps (24 px)
  AT(K_BURST, 0, 1, 0, 0, 0),   // MINE    speed 0
  AT(K_CHARGE, 0, 0, 7, 0, 0),  // CHARGE
  AT(K_HOP, 0, 0, 3, 0, 0),     // HOP
};
static const uint8_t SPEED[4] GC_ROM_ATTR = {0, 15, 22, 31};   // 1/16 px per 32 ms step: 0, 30, 42, 60 px/s
static const uint16_t MASK[5] GC_ROM_ATTR = {0x0001, 0x8003, 0xC007, 0x1111, 0x5555};   // single spread fan cross ring
static const uint8_t COS5[5] GC_ROM_ATTR = {16, 15, 11, 6, 0};

struct BossRow { uint8_t hp, ms, walk, cd, icon, atk[3], flags; };   // 9 bytes; cd in 100 ms; flags reserved
static const BossRow BOSS[BOSS_COUNT] GC_ROM_ATTR = {
  {8, 40, 2, 16, WI_HERB, {BA_CHARGE, BA_SPREAD, BA_CHARGE}, 0},      // THORNBACK
  {10, 0, 0, 14, I_WOOD, {BA_SLAM, BA_SHOT, BA_FAN}, 0},              // OLD STUMP
  {6, 30, 3, 10, I_BOBBER, {BA_BOLT, BA_HOP, BA_BOLT}, 0},            // QUEEN BEE
  {9, 0, 0, 15, WI_GEL, {BA_HOP, BA_RING, BA_MINE}, 0},               // MOSSMAW
  {8, 90, 1, 12, WI_MUSHROOM, {BA_SPIRAL, BA_SPIRAL, BA_MINE}, 0},    // CAPTAIN CAP
  {10, 80, 2, 16, WI_STONE, {BA_SLAM, BA_CROSS, BA_SLAM}, 0},         // ROCKJAW
  {6, 30, 3, 10, WI_POOF2, {BA_CHARGE, BA_CHARGE, BA_FAN}, 0},        // GLOOMWING
  {8, 60, 2, 14, WI_GEM, {BA_CROSS, BA_XSHOT, BA_SPREAD}, 0},         // SHARDSHELL
  {7, 0, 0, 13, WI_ORE, {BA_HOP, BA_RING, BA_SHOT}, 0},               // BORER
  {8, 55, 3, 12, WI_BERRY, {BA_MINE, BA_SPREAD, BA_MINE}, 0},         // CINDER
  {10, 45, 2, 15, WI_SWORD_UP, {BA_CHARGE, BA_BOLT, BA_CROSS}, 0},    // THE WARDEN
  {8, 70, 2, 14, WI_SHELL, {BA_FAN, BA_RING, BA_SHOT}, 0},            // RATTLE KING
  {7, 0, 0, 10, I_BOBBER, {BA_BOLT, BA_BOLT, BA_SPIRAL}, 0},          // THE EYE
  {6, 0, 0, 12, I_STAR, {BA_HOP, BA_SPREAD, BA_XSHOT}, 0},            // HEX
  {12, 0, 0, 13, WI_STONE, {BA_CROSS, BA_XSHOT, BA_SLAM}, 0},         // STONE IDOL
};
#define BR(b, f) GC_RD8(&BOSS[b].f)

#if !BOSS_CUT_NAMES
static const char NAMES[] GC_ROM_ATTR =
  "THORNBACK\0OLD STUMP\0QUEEN BEE\0MOSSMAW\0CAPTAIN CAP\0ROCKJAW\0GLOOMWING\0SHARDSHELL\0BORER\0CINDER\0"
  "THE WARDEN\0RATTLE KING\0THE EYE\0HEX\0STONE IDOL";
#endif

// fight state
static struct {
  uint32_t ms;                   // fight clock
  uint16_t acc;                  // sub-step accumulator, ms
  int32_t  t, flash, stun;       // timer of the boss state; boss blink left; hero stun + grace left (stun while > 800)
  uint8_t  slot, st, ai, rot, walk, hc, atk, res, on, hit;
} F;

static int tx_(const Actor* a) { return (a->x + 8) >> 4; }
static int ty_(const Actor* a) { return (a->y + 8) >> 4; }
static int ab(int v) { return v < 0 ? -v : v; }
static int sgn(int v) { return (v > 0) - (v < 0); }

// ---- table access
int boss_for(int biome, uint32_t seed) {
  int b = biome - 1;
  if (b < 0 || b > 2) b = 0;
  return b * 5 + (int)(dvh(seed ? seed : 1, 0xB055, 7) % 5);
}

static int boss_ms(int b) { int m = BR(b, ms); return m ? m : 100; }

void boss_init_actor(Actor* a, int b) {
  a->phase = (uint8_t)(b + 1);
  a->counter = BR(b, hp);
  a->ms_per_px = (uint8_t)boss_ms(b);
  a->bob = 0;
}

void boss_name(int b, char* out) {
#if BOSS_CUT_NAMES
  (void)b;
  str_copy(out, "BOSS", 5);
#else
  const char* p = NAMES;
  if (b < 0 || b >= BOSS_COUNT) b = 0;
  while (b-- > 0) while (GC_RD8(p++)) {}
  int i = 0;
  for (char c; (c = (char)GC_RD8(p++)) && i < 11;) out[i++] = c;
  out[i] = 0;
#endif
}

int boss_slot(void) {
  for (int i = 1; i < A_COUNT; i++) if (A[i].kind == AK_MONSTER && A[i].phase) return i;
  return -1;
}

int boss_band(char* buf, int cap) {
  int s = boss_slot();
  if (s < 0 || cap < 20) return 0;
  boss_name(A[s].phase - 1, buf);
  int n = str_len(buf);
  str_copy(buf + n, " HP ", 5);
  itoa_into(buf + n + 4, A[s].counter);
  return 1;
}

// ---- small helpers
static void freeze(Actor* a) {
  if (aligned(a)) a->plen = a->pidx;
  else if (a->pidx < a->plen) a->plen = (uint8_t)(a->pidx + 1);
}

static int live(void) { int n = 0; for (int i = 0; i < BOSS_SHOTS; i++) n += g_bshot[i].life != 0; return n; }

static void shot_rows(const BossShot* s) { mark_rows((s->y >> 4) - 8, 16); }

static void pool_clear(void) {
  for (int i = 0; i < BOSS_SHOTS; i++) if (g_bshot[i].life) { shot_rows(&g_bshot[i]); g_bshot[i].life = 0; }
}

static int hero_dest(void) {
  const Actor* h = &A[A_HERO];
  return h->pidx < h->plen ? h->path[h->pidx] : TI(tx_(h), ty_(h));
}

static int cosd(int d) {   // 16 * cos(d * 22.5 degrees)
  d &= 15;
  if (d > 8) d = 16 - d;
  return d <= 4 ? (int)GC_RD8(&COS5[d]) : -(int)GC_RD8(&COS5[8 - d]);
}

static int aim_dir(const Actor* b) {   // the hero's direction in 1/16 turns, y down: 0 east, 4 south
  const Actor* h = &A[A_HERO];
  int dx = h->x - b->x, dy = h->y - b->y, ax = ab(dx), ay = ab(dy);
  int k = 5 * ay < ax ? 0 : (3 * ay < 2 * ax ? 1 : (2 * ay < 3 * ax ? 2 : (ay < 5 * ax ? 3 : 4)));
  return dx >= 0 ? (dy >= 0 ? k : (16 - k) & 15) : (dy >= 0 ? 8 - k : 8 + k);
}

// ---- the burst: one shot for every set bit of the mask, turned by the aim
static void burst(const Actor* b, int e0, int e1) {
  int aim = (e0 >> 5) & 3, d0 = aim == 3 ? F.rot++ : (aim == 2 ? 2 : (aim == 1 ? 0 : aim_dir(b)));
  int v = GC_RD8(&SPEED[(e1 >> 4) & 3]), life = (e1 >> 6) & 1 ? 12 : 128, icon = BR(b->phase - 1, icon);
  uint16_t m = GC_RD16(&MASK[(e0 >> 2) & 7]);
  for (int bit = 0, i = 0; bit < 16; bit++) {
    if (!((m >> bit) & 1)) continue;
    while (i < BOSS_SHOTS && g_bshot[i].life) i++;
    if (i >= BOSS_SHOTS) return;                       // pool full: the rest is dropped
    int d = (d0 + bit) & 15;
    BossShot* s = &g_bshot[i];
    s->x = (int16_t)((b->x + 8) * 16); s->y = (int16_t)((b->y + 8) * 16);
    s->vx = (int16_t)(cosd(d) * v / 16); s->vy = (int16_t)(cosd(d - 4) * v / 16);
    s->life = (uint8_t)life; s->icon = (uint8_t)icon;
  }
}

// ---- the hit on the hero: slide back up to 2 tiles along the main axis, stun, grace. No heart.
static void hurt(int vx, int vy) {
  Actor* h = &A[A_HERO];
  int sx = 0, sy = 0, tx = tx_(h), ty = ty_(h), n = 0;
  if (ab(vx) > ab(vy)) sx = sgn(vx); else sy = vy > 0 ? 1 : -1;
  for (int k = 1; k <= 2 && !BOSS_CUT_KNOCKBACK && walkable(tx + sx * k, ty + sy * k); k++, n++) h->path[n] = TI(tx + sx * k, ty + sy * k);
  h->pidx = 0; h->plen = (uint8_t)n;
  h->pose = POSE_HURT; h->timer = 0;
  F.stun = STUN_MS; F.hit = 1;
}

static void shots(uint32_t dt) {
  Actor* h = &A[A_HERO];
  F.acc = (uint16_t)(F.acc + dt);
  int n = F.acc >> 5;
  F.acc &= 31;
  if (n > 8) n = 8;                                   // time beyond 8 steps is dropped
  for (; n > 0; n--)
    for (int i = 0; i < BOSS_SHOTS; i++) {
      BossShot* s = &g_bshot[i];
      if (!s->life) continue;
#if !defined(ARDUINO_ARCH_ESP8266)
      g_dbg_boss_work++;
#endif
      shot_rows(s);
      s->x = (int16_t)(s->x + s->vx); s->y = (int16_t)(s->y + s->vy);
      int tx = s->x >> 8, ty = s->y >> 8;
      if (--s->life == 0 || tx < 1 || tx > 13 || ty < 1 || ty > 13 || !walkable(tx, ty)) { s->life = 0; continue; }
      shot_rows(s);
      int px = (s->x >> 4) - (h->x + 8), py = (s->y >> 4) - (h->y + 8);
      if (!F.stun && ab(px) < 7 && ab(py) < 7) {
        int vx = s->vx, vy = s->vy;
        if (!vx && !vy) { vx = -px; vy = -py; }       // a mine: away from where it lay
        s->life = 0;
        hurt(vx, vy);
      }
    }
}

// ---- boss states
static void next_attack(Actor* b) {
  F.st = BS_WALK; F.t = BR(b->phase - 1, cd) * 100; F.walk = 0;
  if (++F.ai >= 3) { F.ai = 0; F.t = REST_MS; F.walk = 255; }   // after the third attack it rests 2 s, standing
}

static int toward(const Actor* b, int* sx, int* sy) {   // unit step along the longer axis toward the hero
  int dx = A[A_HERO].x - b->x, dy = A[A_HERO].y - b->y;
  *sx = ab(dx) >= ab(dy) ? sgn(dx) : 0;
  *sy = *sx ? 0 : sgn(dy);
  return *sx | *sy;
}

static void walk_step(Actor* b) {                     // one tile toward the hero, never next to him
  const Actor* h = &A[A_HERO];
  int sx, sy, nx = tx_(b), ny = ty_(b);
  if (!toward(b, &sx, &sy)) return;
  nx += sx; ny += sy;
  if (!walkable(nx, ny) || TI(nx, ny) == hero_dest() || ab(tx_(h) - nx) + ab(ty_(h) - ny) < 2) return;
  b->path[0] = TI(nx, ny); b->plen = 1; b->pidx = 0;
}

static void do_charge(Actor* b) {                     // a straight run along the row or column toward the hero
  int sx, sy, bx = tx_(b), by = ty_(b), n = 0;
  b->plen = b->pidx = 0;
  if (toward(b, &sx, &sy))
    while (n < 14 && walkable(bx += sx, by += sy) && TI(bx, by) != hero_dest()) b->path[n++] = TI(bx, by);
  b->plen = (uint8_t)n;
  b->ms_per_px = 14;
  F.st = BS_CHARGE;
}

static void do_hop(Actor* b) {                        // the farthest of 4 hashed tiles (no random numbers)
  const Actor* h = &A[A_HERO];
  uint32_t hh = dvh(G.s.dv_seed ? G.s.dv_seed : 1, F.hc++, 0xB057);
  int best = 2, bx = -1, by = 0;
  for (int k = 0; k < 4; k++, hh >>= 8) {
    int x = 1 + (int)((hh & 15) * 13 >> 4), y = 1 + (int)(((hh >> 4) & 15) * 13 >> 4), far = ab(x - tx_(h)) + ab(y - ty_(h));
    if (walkable(x, y) && far > best) { best = far; bx = x; by = y; }
  }
  if (bx >= 0) { place(b, bx, by); b->emote = WI_POOF; }
}

static void kill(Actor* b) {                          // poof where it stood; chunk C ends the fight on result 2
  b->counter = 0; b->visible = 0; b->emote = WI_POOF; b->act = 1;
  freeze(b);
  pool_clear();
  F.on = 0; F.res = 2;
}

static void machine(Actor* b, uint32_t dt) {
  int bi = b->phase - 1;
  F.t -= (int32_t)dt;
  if (F.ms >= TIRE_MS && F.st < BS_TIRED && F.st != BS_CHARGE) { freeze(b); b->emote = I_ZZZ; F.st = BS_TIRED; }
  switch (F.st) {
    case BS_WALK:
      if (F.t > 0 && F.walk < BR(bi, walk) && arrived(b)) { F.walk++; walk_step(b); }
      else if (F.t <= 0 && arrived(b) && live() <= 2) {
        int a = BR(bi, atk[F.ai]), tl = GC_RD8(&ATK[a][1]) & 15;
        F.atk = (uint8_t)a; F.st = BS_TELE; F.t = tl * 100;
        if (tl) b->emote = WI_EXCL;
      }
      break;
    case BS_TELE: {
      if (F.t > 0) break;
      int e0 = GC_RD8(&ATK[F.atk][0]), e1 = GC_RD8(&ATK[F.atk][1]), k = e0 & 3;
      b->emote = I_NONE;
      if (k == K_CHARGE) { do_charge(b); break; }
      if (k == K_HOP) do_hop(b); else burst(b, e0, e1);
      next_attack(b);
      break;
    }
    case BS_CHARGE: {
      Actor* h = &A[A_HERO];
      int ht = TI(tx_(h), ty_(h)), hd = hero_dest();
      for (int i = b->pidx; i < b->plen; i++)         // never run into him: stop on the tile before
        if (b->path[i] == ht || b->path[i] == hd) { b->plen = (uint8_t)(i == b->pidx && !aligned(b) ? i + 1 : i); break; }
      int dx = h->x - b->x, dy = h->y - b->y;
      if (!F.stun && ((ab(dx) < 17 && ab(dy) < 11) || (ab(dx) < 11 && ab(dy) < 17))) { hurt(dx, dy); freeze(b); }
      if (arrived(b)) { b->ms_per_px = (uint8_t)boss_ms(bi); next_attack(b); }
      break;
    }
    default: break;
  }
}

// ---- control
int boss_begin(Actor* h) {
  int s = boss_slot();
  if (s < 0 || !A[s].visible || !A[s].counter || G.s.dv_min >= 57 || world_area() != GQ_AREA_DELVE) return 0;
  int ok = find_path(h, tx_(&A[s]), ty_(&A[s]));   // the boss's own tile is walkable: a path to it exists iff he can reach him
  h->plen = h->pidx = 0;
  if (!ok) return 0;
  pool_clear();
  for (uint8_t* p = (uint8_t*)&F; p < (uint8_t*)(&F + 1); p++) *p = 0;
  F.slot = (uint8_t)s; F.on = 1;
  F.t = BR(A[s].phase - 1, cd) * 100;
  A[s].ms_per_px = (uint8_t)boss_ms(A[s].phase - 1);
  return 1;
}

void boss_clear(void) {
  pool_clear();
  Actor* b = &A[F.slot];
  if (b->kind == AK_MONSTER && b->phase) { b->emote = I_NONE; if (b->counter) b->visible = 1; }
  if (A[A_HERO].pose == POSE_HURT) A[A_HERO].pose = POSE_NORMAL;
  F.on = 0; F.res = 1;
}

void boss_tick(uint32_t dt) {
  if (!F.on) return;
  Actor* b = &A[F.slot];
  if (world_area() != GQ_AREA_DELVE || b->kind != AK_MONSTER || !b->phase) { boss_clear(); return; }
  if (dt > 1000) dt = 1000;
  F.ms += dt;
  if (F.ms >= END_MS) {                               // the forced end: counts as a win
#if !defined(ARDUINO_ARCH_ESP8266)
    g_dbg_boss_forced++;
#endif
    kill(b);
    return;
  }
  machine(b, dt);
  shots(dt);
  if ((F.stun -= (int32_t)dt) < 0) F.stun = 0;
  if (F.flash > 0) {
    F.flash -= (int32_t)dt;
    b->visible = F.flash <= 0 || !((F.flash / 80) & 1);
  }
  if (A[A_HERO].pose == POSE_HURT && !boss_busy()) A[A_HERO].pose = POSE_NORMAL;
}

int boss_strike(void) {
  if (!F.on) return 0;
  Actor* b = &A[F.slot];
  int d = econ_tool(GQ_TOOL_SWORD) >= 2 ? 2 : 1;
  if (b->counter > d) { b->counter = (uint8_t)(b->counter - d); F.flash = FLASH_MS; b->visible = 0; return 0; }
  kill(b);
  return 1;
}

int boss_result(void) { return F.res; }
int boss_busy(void) { return F.on && (F.stun > 800 || (F.stun > 0 && !arrived(&A[A_HERO]))); }
int boss_took_hit(void) { int r = F.hit; F.hit = 0; return r; }
int boss_phase(void) { return F.st; }
int boss_telegraph(void) { return F.on && (F.st == BS_TELE || F.st == BS_CHARGE) ? F.atk + 1 : 0; }
int boss_hp(void) { return A[F.slot].counter; }
uint32_t boss_clock(void) { return F.ms; }
