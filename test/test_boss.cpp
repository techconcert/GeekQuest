// R11 chunks B and C (docs/DESIGN-R11.md 9): boss mapping and spawn, every attack once, the shot pool, tired and forced
// end, the hit on the hero, determinism, work bound, aborts, drawing (engine part: the test drives the hero); then the
// hero's own fight through ACT_BOSS: every boss x seeds x tool levels x speeds x tick sizes, aborts, late start,
// restore, and whole deep missions.
#include "gq_test.h"
#include "../lib/gamecore/src/world_priv.h"
#include "../lib/gamecore/src/delve.h"
#include "../lib/gamecore/src/boss.h"

static const uint8_t HP[15] = {8, 8, 5, 5, 8, 10, 6, 8, 4, 8, 10, 8, 7, 4, 12};   // tuned against test_beatable

// the world's move_actor (static there), so the test can move the boss and the hero's knockback slide
static void mv(Actor* a, uint32_t dt) {
  a->move_acc = (uint16_t)(a->move_acc + dt);
  for (int guard = 0; a->move_acc >= a->ms_per_px && guard < 64; guard++) {
    a->move_acc = (uint16_t)(a->move_acc - a->ms_per_px);
    if (a->pidx >= a->plen) { a->move_acc = 0; break; }
    uint8_t t = a->path[a->pidx];
    int tx = TX(t) * 16, ty = TY(t) * 16;
    if (aligned(a) && !walkable(TX(t), TY(t))) { a->plen = a->pidx; continue; }
    if (a->x != tx) a->x = (int16_t)(a->x + (tx > a->x ? 1 : -1));
    else if (a->y != ty) a->y = (int16_t)(a->y + (ty > a->y ? 1 : -1));
    if (a->x == tx && a->y == ty) a->pidx++;
  }
}

static int find_seed(int biome, int boss) {
  for (uint32_t s = 1; s < 5000; s++) if (boss_for(biome, s) == boss) return (int)s;
  CHECK(0);
  return 0;
}

static int boss_room(int boss, int* seedp = 0) {   // stand in the exit room of a level whose boss is `boss`
  int biome = boss / 5 + 1, seed = find_seed(biome, boss);
  G.s.dv_state = (uint8_t)biome; G.s.dv_seed = (uint32_t)seed; G.s.dv_cur = delve_gen((uint32_t)seed)->exit;
  G.s.dv_seen = 0; G.s.dv_min = G.s.dv_moves = 0;
  world_instant_area(GQ_AREA_DELVE, SP_ENTRY);
  if (seedp) *seedp = seed;
  int s = boss_slot();
  CHECK(s > 0 && A[s].phase == boss + 1);
  return s;
}

static void hero_at(int tx, int ty) {
  Actor* h = &A[A_HERO];
  place(h, tx, ty);
  h->pose = POSE_NORMAL; h->visible = 1; h->ms_per_px = 24;
}

// a lane of free tiles from the boss toward the middle of the room, `n` tiles long; 1 = found
static int lane_home(int s, int n, int* hx, int* hy) {
  static const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
  int bx = tile_x(&A[s]), by = tile_y(&A[s]);
  for (int d = 0; d < 4; d++) {
    int ok = 1;
    for (int k = 1; k <= n; k++) {
      int x = bx + DX[d] * k, y = by + DY[d] * k;
      if (!walkable(x, y) || x < 1 || x > 13 || y < 1 || y > 13) ok = 0;   // the border tiles are where shots die
    }
    if (ok) { *hx = bx + DX[d] * n; *hy = by + DY[d] * n; return 1; }
  }
  return 0;
}

static int g_s;   // boss slot of the current fight
static void step(uint32_t dt) {
  boss_tick(dt);
  mv(&A[A_HERO], dt);
  if (boss_slot() >= 0) mv(&A[g_s], dt);
}

static int live() { int n = 0; for (int i = 0; i < BOSS_SHOTS; i++) n += g_bshot[i].life != 0; return n; }

static void check_pool() {
  CHECK(live() <= BOSS_SHOTS);
#if defined(BOSS_GUARD) && BOSS_GUARD
  CHECK(g_bshot[BOSS_SHOTS].x == 0 && g_bshot[BOSS_SHOTS].y == 0 && g_bshot[BOSS_SHOTS].vx == 0 && g_bshot[BOSS_SHOTS].vy == 0 &&
        g_bshot[BOSS_SHOTS].life == 0 && g_bshot[BOSS_SHOTS].icon == 0);
#endif
  for (int i = 0; i < BOSS_SHOTS; i++) {
    const BossShot& s = g_bshot[i];
    if (!s.life) continue;
    int tx = s.x >> 8, ty = s.y >> 8;
    CHECK(tx >= 1 && tx <= 13 && ty >= 1 && ty <= 13 && walkable(tx, ty));
  }
}

// begin a fight with the hero on a lane tile, then put him out of every shot's way (corner tile 0,0 is a wall:
// shots die in the border tiles and never reach it)
static int start_fight(int boss, int hidden) {
  g_s = boss_room(boss);
  int hx = 7, hy = 12;
  if (!lane_home(g_s, 4, &hx, &hy)) return 0;
  hero_at(hx, hy);
  CHECK(boss_begin(&A[A_HERO]) == 1);
  CHECK(boss_result() == 0);
  if (hidden) hero_at(0, 0);
  return 1;
}

// ------------------------------------------------------------ 1. mapping and spawn
static void test_mapping() {
  int seen[15] = {0};
  for (uint32_t seed = 1; seed <= 3000; seed++)
    for (int biome = 1; biome <= 3; biome++) {
      int b = boss_for(biome, seed);
      CHECK(b >= (biome - 1) * 5 && b < biome * 5);
      seen[b]++;
      if (seed % 7 == 0) delve_gen(seed + 31);   // another level in between: nothing is remembered
      CHECK(boss_for(biome, seed) == b);
    }
  for (int b = 0; b < 15; b++) CHECK(seen[b] > 300);
  // the exit room holds the boss alone, 2 tiles inward of the way out; other rooms none
  for (int b = 0; b < 15; b++) {
    int s = boss_room(b), n = 0, dx = 0, dy = 0;
    for (int i = 1; i < A_COUNT; i++) if (A[i].kind != AK_NONE) n++;
    CHECK(n == 1 && A[s].kind == AK_MONSTER && A[s].id == GQ_MON_BOSS && A[s].visible && A[s].counter == HP[b]);
    CHECK(world_spot(SP_DOOR, &dx, &dy));
    int ex = tile_x(&A[s]) - dx, ey = tile_y(&A[s]) - dy;
    CHECK(walkable(tile_x(&A[s]), tile_y(&A[s])) && ex * ex + ey * ey == 4);
    char nm[24];
    boss_name(b, nm);
    CHECK(nm[0] && std::strlen(nm) <= 11);
    CHECK(boss_band(nm, 20) && std::strstr(nm, " HP ") && nm[std::strlen(nm) - 1] == (char)('0' + HP[b] % 10));
    // the arena: no trees (forest) and no stalagmites (cave) in the interior
    for (int y = 1; y < 14; y++) for (int x = 1; x < 14; x++) {
      if (b < 5) CHECK(g_obj[y][x] != T_TREE);
      else if (b < 10) CHECK(g_obj[y][x] != WT_STALAG);
    }
  }
  // a room that is not the exit has monsters but never a boss
  for (uint32_t seed = 1; seed <= 60; seed++) {
    int biome = 1 + (int)(seed % 3);
    const DelveGraph* g = delve_gen(seed);
    G.s.dv_state = (uint8_t)biome; G.s.dv_seed = seed; G.s.dv_cur = (uint8_t)(1 + seed % (g->n - 1)); G.s.dv_seen = 0;
    if (G.s.dv_cur == g->exit) continue;
    world_instant_area(GQ_AREA_DELVE, SP_ENTRY);
    CHECK(boss_slot() < 0);
  }
  // after a save round trip the same level gives the same boss
  for (int b = 0; b < 15; b += 4) {
    int seed;
    boss_room(b, &seed);
    uint8_t buf[GC_SAVE_BYTES];
    CHECK(gc_save_export(buf, GC_SAVE_BYTES) == GC_SAVE_BYTES);
    G.s.dv_state = 0; G.s.dv_seed = 0;
    CHECK(gc_save_import(buf, GC_SAVE_BYTES) == 1);
    CHECK(G.s.dv_state == b / 5 + 1 && G.s.dv_seed == (uint32_t)seed && boss_for(G.s.dv_state, G.s.dv_seed) == b);
  }
  std::printf("mapping: 15 bosses ok\n");
}

// ------------------------------------------------------------ 2. every attack once
static void test_attacks() {
  static const int SHOTS[12] = {1, 1, 3, 5, 8, 8, 4, 4, 8, 1, 0, 0};   // SHOT BOLT SPREAD FAN RING SPIRAL CROSS XSHOT SLAM MINE CHARGE HOP
  int fired[12] = {0};
  for (int b = 0; b < 15; b++) {
    CHECK(start_fight(b, 1));
    int prev = 0, prevph = BS_WALK, bx = tile_x(&A[g_s]), by = tile_y(&A[g_s]);
    for (int t = 0; t < 30000 / 32; t++) {
      step(32);
      check_pool();
      int tel = boss_telegraph();
      if (prevph == BS_TELE && boss_phase() != BS_TELE) {       // the telegraph of attack prev - 1 just ended
        int a = prev - 1, fresh = 0;
        for (int i = 0; i < BOSS_SHOTS; i++) fresh += g_bshot[i].life == 127 || g_bshot[i].life == 11;
        if (a == BA_CHARGE) CHECK(boss_phase() == BS_CHARGE);
        else if (a == BA_HOP) CHECK(tile_x(&A[g_s]) != bx || tile_y(&A[g_s]) != by);
        else CHECK(fresh == SHOTS[a]);
        if (a == BA_XSHOT) for (int i = 0; i < BOSS_SHOTS; i++) if (g_bshot[i].life == 127) CHECK(std::abs(g_bshot[i].vx) == std::abs(g_bshot[i].vy) && g_bshot[i].vx);
        if (a == BA_CROSS) for (int i = 0; i < BOSS_SHOTS; i++) if (g_bshot[i].life == 127) CHECK((g_bshot[i].vx == 0) != (g_bshot[i].vy == 0));
        if (a == BA_BOLT) for (int i = 0; i < BOSS_SHOTS; i++) if (g_bshot[i].life == 127) CHECK(g_bshot[i].vx < 0 && g_bshot[i].vy < 0);   // the hero is in the corner
        fired[a]++;
      }
      if (tel) prev = tel;
      prevph = boss_phase();
      bx = tile_x(&A[g_s]); by = tile_y(&A[g_s]);
      if (boss_result()) break;
    }
  }
  for (int a = 0; a < 12; a++) { CHECK(fired[a] > 0); }
  std::printf("attacks: all 12 fired (");
  for (int a = 0; a < 12; a++) std::printf("%d%s", fired[a], a < 11 ? " " : ")\n");
}

// ------------------------------------------------------------ 3. pool bounds
static void test_pool() {
  CHECK(start_fight(5, 1));   // ROCKJAW: SLAM first, 8 shots
  int full = 0;
  for (int t = 0; t < 20000 / 32 && !full; t++) {
    step(32);
    if (boss_telegraph() == BA_SLAM + 1) {                       // fill 9 of the 10 slots just before the burst
      for (int i = 0; i < 9; i++) { g_bshot[i].x = (int16_t)(7 * 256 + 100); g_bshot[i].y = (int16_t)(7 * 256 + 100); g_bshot[i].vx = g_bshot[i].vy = 0; g_bshot[i].life = 200; g_bshot[i].icon = 16; }
      for (int k = 0; k < 40; k++) { step(32); check_pool(); if (boss_phase() != BS_TELE) break; }
      CHECK(live() == BOSS_SHOTS);                              // 9 + 1: the other 7 shots were dropped
      full = 1;
    }
  }
  CHECK(full);
  for (int t = 0; t < 200; t++) { step(32); check_pool(); }
  std::printf("pool: overfull burst dropped, guard intact\n");
}

// ------------------------------------------------------------ 4. tired and forced end
static void test_tired_forced() {
  for (int b = 0; b < 15; b += 2) {
    CHECK(start_fight(b, 1));
    unsigned forced0 = g_dbg_boss_forced;
    uint32_t last_burst = 0;
    int tired_seen = 0;
    for (int t = 0; boss_result() == 0 && t < 120000 / 50; t++) {
      step(50);
      check_pool();
      if (boss_telegraph()) last_burst = boss_clock();
      if (boss_clock() >= 70000 + 600 && boss_phase() != BS_CHARGE) {   // tired for good (a charge in progress ends first)
        CHECK(boss_telegraph() == 0);
        if (boss_phase() == BS_TIRED) tired_seen = 1;
      }
      if (boss_clock() > 70000 + 4200 + 1500) CHECK(live() == 0 || boss_result() == 2);
      if (boss_clock() > 70000) CHECK(last_burst <= 70000 + 1000);   // no telegraph after the tire point (plus one running)
    }
    CHECK(tired_seen && boss_result() == 2 && g_dbg_boss_forced == forced0 + 1);
    CHECK(boss_clock() >= 110000 && boss_clock() < 110100 && live() == 0 && !A[g_s].visible && A[g_s].act == 1);
    CHECK(A[g_s].emote == WI_POOF);
  }
  // strikes: health goes down by one (two with a sword of level 2), the last one ends the fight
  for (int lvl = 0; lvl <= 3; lvl += 3) {
    CHECK(start_fight(14, 1));
    G.s.tool[GQ_TOOL_SWORD] = (uint8_t)lvl;
    int hits = 0;
    while (!boss_strike()) { hits++; CHECK(A[g_s].counter == HP[14] - hits * (lvl >= 2 ? 2 : 1)); CHECK(boss_hp() == A[g_s].counter); CHECK(!A[g_s].visible); step(100); CHECK(hits < 20); }
    CHECK(boss_result() == 2 && A[g_s].counter == 0 && live() == 0);
    CHECK(hits + 1 == (lvl >= 2 ? 6 : 12));
    G.s.tool[GQ_TOOL_SWORD] = 0;
  }
  std::printf("tired at 70 s, forced end at 110 s, strikes ok\n");
}

// ------------------------------------------------------------ 5. the hit on the hero
static void test_hits() {
  static const int BOSSES[4] = {0, 1, 3, 9};   // THORNBACK (charge), OLD STUMP (turret), MOSSMAW (hops, mine), CINDER (mines)
  for (int dtc = 0; dtc < 2; dtc++)
    for (int bi = 0; bi < 4; bi++) {
      uint32_t dt = dtc ? 250 : 33;
      int b = BOSSES[bi];
      g_s = boss_room(b);
      int hx, hy;
      if (!lane_home(g_s, 3, &hx, &hy)) continue;
      hero_at(hx, hy);
      CHECK(boss_begin(&A[A_HERO]));
      int hits = 0, busy_ms = 0, maxbusy = 0, was_busy = 0;
      uint32_t last_hit = 0;
      uint8_t hearts = G.s.hero_hearts;
      uint32_t faints = (uint32_t)gc_query(GQ_STAT_FAINTS);
      for (uint32_t ms = 0; ms < 60000 && boss_result() == 0; ms += dt) {
        step(dt);
        check_pool();
        if (boss_took_hit()) {
          hits++;
          CHECK(hits == 1 || boss_clock() - last_hit >= 1400);     // two hits are never closer than 1.4 s
          last_hit = boss_clock();
          CHECK(A[A_HERO].pose == POSE_HURT && boss_busy());
        }
        if (boss_busy()) { busy_ms += (int)dt; if (busy_ms > maxbusy) maxbusy = busy_ms; } else busy_ms = 0;
        Actor* h = &A[A_HERO];
        CHECK(walkable(tile_x(h), tile_y(h)));
        if (!boss_busy() && !was_busy) CHECK(h->pose != POSE_HURT);   // the slide ends after boss_tick: the pose follows one tick later
        was_busy = boss_busy();
        if (aligned(h) && aligned(&A[g_s])) CHECK(tile_x(h) != tile_x(&A[g_s]) || tile_y(h) != tile_y(&A[g_s]));
        int bdist = std::abs(hx - tile_x(&A[g_s])) + std::abs(hy - tile_y(&A[g_s]));
        if (!boss_busy() && arrived(h) && !(tile_x(h) == hx && tile_y(h) == hy) && boss_clock() - last_hit > 1500 && bdist >= 2) hero_at(hx, hy);   // the test is the hero
        CHECK(G.s.hero_hearts == hearts && (uint32_t)gc_query(GQ_STAT_FAINTS) == faints);
      }
      CHECK(maxbusy <= 1500);
      std::printf("hits: boss %d dt %u: %d hits, longest stun %d ms\n", b, (unsigned)dt, hits, maxbusy);
      if (b != 3) CHECK(hits >= 1);                              // MOSSMAW only hops and lays mines at its own feet
    }
}

static void test_mine_hit() {
  CHECK(start_fight(9, 1));                                     // CINDER lays mines at its own feet
  while (true) {
    step(32);
    int found = 0;
    for (int i = 0; i < BOSS_SHOTS; i++) found += g_bshot[i].life && !g_bshot[i].vx && !g_bshot[i].vy;
    if (found) break;
    CHECK(boss_clock() < 20000);
  }
  hero_at(tile_x(&A[g_s]), tile_y(&A[g_s]) + 1);                // beside the boss, on top of the mine's reach
  A[A_HERO].y = (int16_t)(A[A_HERO].y - 12);                    // 4 px below the mine's centre line
  step(32);
  CHECK(boss_took_hit() && A[A_HERO].pose == POSE_HURT && boss_busy());
  for (int i = 0; i < 100 && boss_busy(); i++) step(32);
  CHECK(!boss_busy());
  std::printf("mine hit ok\n");
}

// ------------------------------------------------------------ 6. determinism and work bound
static uint32_t fight_hash(int boss, uint32_t dt, uint32_t* maxwork, int* hits) {
  uint32_t h = 2166136261u;
  CHECK(start_fight(boss, 0));
  int hx = tile_x(&A[A_HERO]), hy = tile_y(&A[A_HERO]);
  uint32_t rng0 = gc_rng_get();
  *maxwork = 0; *hits = 0;
  for (uint32_t ms = 0; ms < 40000 && boss_result() == 0; ms += dt) {
    unsigned w0 = g_dbg_boss_work;
    step(dt);
    if (g_dbg_boss_work - w0 > *maxwork) *maxwork = g_dbg_boss_work - w0;
    if (boss_took_hit()) ++*hits;
    if (!boss_busy() && arrived(&A[A_HERO]) && boss_clock() % 3000 < dt) hero_at(hx, hy);
    if (ms % 5000 < dt) {
      h = (h ^ (uint32_t)(A[g_s].x * 64 + A[g_s].y)) * 16777619u;
      h = (h ^ (uint32_t)(A[A_HERO].x * 64 + A[A_HERO].y)) * 16777619u;
      h = (h ^ (uint32_t)(boss_phase() * 8 + A[g_s].counter)) * 16777619u;
      for (int i = 0; i < BOSS_SHOTS; i++) if (g_bshot[i].life) h = (h ^ (uint32_t)(g_bshot[i].x * 31 + g_bshot[i].y + g_bshot[i].life)) * 16777619u;
    }
  }
  CHECK(gc_rng_get() == rng0);                                  // the fight draws no random numbers
  return h;
}

static void test_determinism_work() {
  for (int b = 0; b < 15; b += 3)
    for (int d = 0; d < 2; d++) {
      uint32_t dt = d ? 250 : 33, w1, w2;
      int h1, h2;
      uint32_t a = fight_hash(b, dt, &w1, &h1);
      uint32_t c = fight_hash(b, dt, &w2, &h2);
      CHECK(a == c && w1 == w2 && h1 == h2);
      CHECK(w1 <= 80);                                          // sub-steps with a hit test per tick (8 steps x 10 shots at most)
    }
  std::printf("determinism and work bound ok\n");
}

// ------------------------------------------------------------ 7. aborts and drawing
static void test_abort_draw() {
  // drawing: a live shot shows on the screen (same room, same hero: only the pool differs) and leaves with it
  CHECK(start_fight(5, 1));
  while (live() == 0 && boss_result() == 0) step(32);
  CHECK(live() > 0);
  gc_tick(1);
  uint32_t with = frame_hash();
  boss_clear();
  gc_tick(1);
  CHECK(frame_hash() != with);
  // the room changes under a fight: shots gone, fight over
  CHECK(start_fight(5, 1));
  while (live() == 0) step(32);
  world_instant_area(GQ_AREA_VILLAGE, SP_ENTRY);
  boss_tick(32);
  CHECK(live() == 0 && boss_result() == 1 && boss_slot() < 0);
  // leaving through boss_clear and repopulating
  CHECK(start_fight(7, 1));
  while (live() == 0) step(32);
  boss_clear();
  CHECK(live() == 0 && boss_result() == 1 && A[g_s].visible && A[g_s].counter == HP[7]);
  CHECK(start_fight(7, 1));
  while (live() == 0) step(32);
  boss_room(7);                                                 // populate again: pool empty, fight over
  CHECK(live() == 0 && boss_result() == 1);
  // no fight late in the mission, or when the hero cannot reach him
  boss_room(2);
  G.s.dv_min = 58;
  hero_at(7, 12);
  CHECK(boss_begin(&A[A_HERO]) == 0);
  G.s.dv_min = 0;
  // the strips of a room with shots render without overrun
  CHECK(start_fight(12, 1));
  for (int t = 0; t < 400; t++) { step(32); if (t % 20 == 0) { gc_tick(1); for (int s = 0; s < GC_ROWS; s++) gqt::render_strip_guarded(s); } }
  std::printf("aborts and drawing ok\n");
}


// ============================================================ chunk C: the hero's fight (ACT_BOSS)
enum { ACT_DELVE_T = 35, ACT_BOSS_T = 36 };

static int hp_of(int boss) { return HP[boss]; }

static void fresh_game(uint32_t seed, int sword, int boots, int minute = 8 * 60) {
  sim_start(seed, minute);
  gc_set_date(2026, 3, 3, 2, 1000);
  gc_set_option(GC_OPT_SLEEP_MODE, 0);
  G.s.day = 5;
  G.s.tool[GQ_TOOL_PICK] = 1; G.s.tool[GQ_TOOL_LANTERN] = 1;
  G.s.tool[GQ_TOOL_SWORD] = (uint8_t)sword; G.s.tool[GQ_TOOL_BOOTS] = (uint8_t)boots;
  G.s.weather = GQ_W_CLEAR; G.s.weather_hours = 8;
  G.s.hero_hearts = (uint8_t)hero_max_hearts();
}

// the nth level seed whose boss is `boss`
static uint32_t nth_seed(int boss, int n) {
  for (uint32_t s = 1;; s++) if (boss_for(boss / 5 + 1, s) == boss && n-- == 0) return s;
}

// A mission of the nth level of `boss` with the hero standing in its exit room (the mission state is as after a walk through the rooms).
static void to_exit_room(int boss, int nth, int sword, int boots, int minute = 8 * 60) {
  fresh_game(1000 + (uint32_t)nth, sword, boots, minute);
  uint32_t seed = nth_seed(boss, nth);
  G.s.dv_state = (uint8_t)(boss / 5 + 1); G.s.dv_seed = seed; G.s.dv_cur = delve_gen(seed)->exit;
  G.s.dv_seen = 0; G.s.dv_min = G.s.dv_moves = 0; G.s.dv_day = (uint8_t)G.s.day;
  hero_force_activity(ACT_DELVE_T);
  world_instant_area(GQ_AREA_DELVE, SP_ENTRY);
}

struct FightStat { int win, hits, strikes, still_max, stun_max, bad; uint32_t ms; unsigned forced; };


// A whole fight through the real hero: from the exit room's door until ACT_BOSS ends. `dt` = tick size.
static FightStat run_fight(int boss, int nth, int sword, int boots, uint32_t dt, uint32_t max_ms = 130000) {
  FightStat f;
  std::memset(&f, 0, sizeof f);
  to_exit_room(boss, nth, sword, boots);
  uint32_t faints0 = (uint32_t)gc_query(GQ_Q_STAT_BASE + GQ_STAT_FAINTS);
  unsigned forced0 = g_dbg_boss_forced, hits0 = g_dbg_boss_hits;
  Actor* h = &A[A_HERO];
  int started = 0, hearts = G.s.hero_hearts, last_tx = -1, last_ty = -1, bi = -1, prev_pose = POSE_NORMAL, hp_prev = 0, sw_tx = 0, sw_ty = 0, pbx = -9, pby = -9;
  uint32_t last_move = 0, last_hit = 0, stun_ms = 0, ms = 0, clock0 = 0;
  for (; ms < max_ms + 10000; ms += dt) {
    gc_tick(dt);
    if (h->act == ACT_BOSS_T) {
      if (!started) { started = 1; bi = boss_slot(); CHECK(bi > 0 && A[bi].phase == boss + 1); pbx = tile_x(&A[bi]); pby = tile_y(&A[bi]); clock0 = boss_clock(); hp_prev = boss_hp(); CHECK(hp_prev == hp_of(boss)); }
      uint32_t now = boss_clock();
      int tx = tile_x(h), ty = tile_y(h);
      if (tx != last_tx || ty != last_ty) { last_tx = tx; last_ty = ty; last_move = now; }
      if (h->pose == POSE_SWING) {
        if (prev_pose != POSE_SWING) {                          // a strike: next to him, not stunned, health down, no moving while the swing lasts
          f.strikes++;
          CHECK(std::abs(tx - pbx) + std::abs(ty - pby) == 1 && aligned(h) && boss_hp() < hp_prev);   // next to where he stood when the blow fell
          sw_tx = tx; sw_ty = ty;
        }
        CHECK(tx == sw_tx && ty == sw_ty && h->plen == h->pidx && !boss_busy());
        last_move = now;
      }
      prev_pose = h->pose;
      hp_prev = boss_hp();
      pbx = tile_x(&A[bi]); pby = tile_y(&A[bi]);
      if (g_dbg_boss_hits - hits0 > (unsigned)f.hits) { f.hits++; if (f.hits > 1 && now - last_hit < 1400) f.bad++; last_hit = now; }
      if (boss_busy()) { stun_ms += dt; if (stun_ms > (uint32_t)f.stun_max) f.stun_max = (int)stun_ms; last_move = now; } else stun_ms = 0;
      if (now - last_move > (uint32_t)f.still_max) f.still_max = (int)(now - last_move);
      // never leaves the room, never in a wall or on the boss, no heart is lost
      CHECK(world_area() == GQ_AREA_DELVE && walkable(tx, ty) && !world_transition_busy());
      if (aligned(h) && aligned(&A[bi]) && boss_hp() > 0) CHECK(tx != tile_x(&A[bi]) || ty != tile_y(&A[bi]));
      CHECK(gc_query(GQ_Q_HERO_PRESENCE) == GQ_PRES_SPRITE);
      CHECK(G.s.hero_hearts == hearts && (uint32_t)gc_query(GQ_Q_STAT_BASE + GQ_STAT_FAINTS) == faints0);
      check_pool();
    } else if (started) break;
    else CHECK(ms < 20000);                                      // the fight starts within seconds
  }
  CHECK(started && live() == 0 && boss_result() == 1);
  f.win = A[bi].counter == 0;
  f.ms = boss_clock() - clock0;
  f.forced = g_dbg_boss_forced - forced0;
  CHECK(f.strikes >= 1);
  return f;
}

static int cmp_u(const void* a, const void* b) { uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b; return x < y ? -1 : x > y; }

// Tests 2, 3, 4: every boss x seeds x sword 0 and 3 x both hero speeds x both tick sizes (33 and 250 ms).
static void test_beatable(int nseeds) {
  static uint32_t all[15 * 64 * 8];
  int total = 0, under70 = 0;
  for (int b = 0; b < 15; b++) {
    uint32_t mine[64 * 8];
    int n = 0, hits = 0, wins = 0, still = 0, stun = 0;
    uint32_t worst = 0;
    for (int seed = 0; seed < nseeds; seed++)
      for (int sw = 0; sw < 2; sw++)
        for (int boots = 0; boots < 2; boots++)
          for (int d = 0; d < 2; d++) {
            FightStat f = run_fight(b, seed, sw * 3, boots, d ? 250 : 33);
            CHECK(f.win && f.forced == 0 && f.ms <= 110400 && f.bad == 0);   // won, the 110 s rule never needed, hits 1.4 s apart
            CHECK(f.stun_max <= 1500 && f.still_max <= 6000);                // never stunned over 1.5 s, never idle over 6 s
            wins += f.win; hits += f.hits;
            if (f.ms > worst) worst = f.ms;
            if (f.still_max > still) still = f.still_max;
            if (f.stun_max > stun) stun = f.stun_max;
            mine[n++] = f.ms; all[total++] = f.ms; under70 += f.ms < 70000;
          }
    qsort(mine, (size_t)n, sizeof mine[0], cmp_u);
    std::printf("beatable: boss %2d: %d/%d won, median %5u ms, worst %6u ms, %4.1f hits per fight, longest idle %d ms, longest stun %d ms\n",
                b, wins, n, (unsigned)mine[n / 2], (unsigned)worst, (double)hits / n, still, stun);
  }
  qsort(all, (size_t)total, sizeof all[0], cmp_u);
  std::printf("beatable: %d fights, median %u ms, %d%% before 70 s\n", total, (unsigned)all[total / 2], under70 * 100 / total);
  CHECK(under70 * 100 >= 95 * total);
  CHECK(all[total / 2] >= 20000 && all[total / 2] <= 60000);
}

// ------------------------------------------------------------ aborts, late start, restore
static int in_fight() { return A[A_HERO].act == ACT_BOSS_T && boss_result() == 0; }

static void run_into_fight(uint32_t min_clock_ms) {
  for (int k = 0; k < 4 * 60 && !(in_fight() && boss_clock() >= min_clock_ms); k++) sim_step();
  CHECK(in_fight() && boss_clock() >= min_clock_ms);
}

static int home_within(int seconds) {   // seconds until he is in the village and idle with the mission over; -1 never
  for (int s = 0; s < seconds; s++) {
    for (int k = 0; k < 4; k++) sim_step();
    if (G.s.dv_state == 0 && world_area() == GQ_AREA_VILLAGE && !world_transition_busy() && gc_query(GQ_Q_HERO_PRESENCE) == GQ_PRES_SPRITE) return s;
  }
  return -1;
}

static void test_aborts_late() {
  // bed time (sleep mode 2) during a fight: back in the village within 2 minutes, pool empty, mission over
  to_exit_room(1, 0, 0, 0);
  run_into_fight(5000);
  gc_set_option(GC_OPT_SLEEP_MODE, 2);
  int s = home_within(120);
  CHECK(s >= 0 && live() == 0 && boss_result() == 1 && boss_slot() < 0);
  std::printf("abort: bed time during a fight, home after %d s\n", s);
  // a festival during a fight (it starts at 16:00; the fight starts at 15:59)
  int found = 0;
  for (int day = 3; day < 400 && !found; day++) {
    fresh_game(24, 0, 0, 15 * 60 + 59);
    gc_debug(GQ_DBG_SET_DAY, day);
    uint32_t seed = nth_seed(2, 1);
    G.s.dv_state = 1; G.s.dv_seed = seed; G.s.dv_cur = delve_gen(seed)->exit; G.s.dv_seen = 0; G.s.dv_min = G.s.dv_moves = 0; G.s.dv_day = (uint8_t)G.s.day;
    hero_force_activity(ACT_DELVE_T);
    world_instant_area(GQ_AREA_DELVE, SP_ENTRY);
    run_into_fight(0);
    A[boss_slot()].counter = 200;                                // a fight that outlasts the minute
    for (int k = 0; k < 4 * 90 && gc_query(GQ_Q_FESTIVAL) < 0 && in_fight(); k++) sim_step();
    if (gc_query(GQ_Q_FESTIVAL) < 0) continue;
    CHECK(boss_clock() < 70000 && A[A_HERO].act != ACT_BOSS_T);   // the festival, not the fight, ended it
    found = 1;
    int hs = home_within(120);
    CHECK(hs >= 0 && live() == 0 && boss_result() == 1);
    std::printf("abort: festival on day %d during a fight, home after %d s\n", day, hs);
  }
  CHECK(found);
  // a forced activity in the middle of a fight: village at once, pool empty, the mission carries on afterwards
  to_exit_room(5, 0, 0, 0);
  run_into_fight(8000);
  gc_debug(GQ_DBG_FORCE_ACT, GQ_ACT_FISH);
  CHECK(world_area() == GQ_AREA_VILLAGE && G.s.dv_state == 2 && live() == 0 && boss_result() == 1 && boss_slot() < 0);
  for (int k = 0; k < 4 * 60 * 20 && !(world_area() == GQ_AREA_DELVE && !world_transition_busy()); k++) sim_step();
  CHECK(world_area() == GQ_AREA_DELVE);
  // a late mission (minute 58, in the exit room) leaves without a fight and the boss stands untouched
  to_exit_room(13, 0, 0, 0);
  G.s.dv_min = 58;
  int bs = boss_slot(), fought = 0;
  CHECK(bs > 0 && A[bs].counter == hp_of(13));
  for (int k = 0; k < 4 * 60 * 4 && G.s.dv_state; k++) { sim_step(); fought |= A[A_HERO].act == ACT_BOSS_T || live() > 0; }
  CHECK(!fought && home_within(240) >= 0);
  CHECK(boss_slot() < 0);
  std::printf("late mission: no fight from minute 57\n");
  // a fight that began at minute 56 may run on past 57 and still ends
  to_exit_room(1, 1, 0, 0);
  G.s.dv_min = 56;
  run_into_fight(1000);
  G.s.dv_min = 58;
  for (int k = 0; k < 4 * 130 && in_fight(); k++) sim_step();
  CHECK(!in_fight());
}

static void test_restore_fights() {
  for (int b = 0; b < 15; b += 2) {
    to_exit_room(b, 2, 0, 0);
    run_into_fight(9000 + (uint32_t)b * 300);
    uint8_t buf[GC_SAVE_BYTES];
    CHECK(gc_save_export(buf, sizeof buf) == GC_SAVE_BYTES);
    uint32_t seed = G.s.dv_seed;
    // a different game, then the restore: the hero wakes in the village and walks back in
    sim_start(900 + (uint32_t)b, 10 * 60);
    CHECK(gc_save_import(buf, sizeof buf) == 1 && G.s.dv_state == b / 5 + 1 && G.s.dv_seed == seed && boss_for(G.s.dv_state, G.s.dv_seed) == b);
    int met = 0, win = 0;
    for (int k = 0; k < 4 * 60 * 15 && !win; k++) {
      sim_step();
      if (in_fight() && !met) { met = 1; CHECK(A[boss_slot()].phase == b + 1 && boss_hp() == hp_of(b)); }   // the same boss at full health
      int bs = boss_slot();
      if (met && bs >= 0 && A[bs].counter == 0) win = 1;
    }
    CHECK(met && win);
  }
  // a save from R10 (no boss concept: the mission fields only, reserved bytes zero) meets a boss
  for (int biome = 1; biome <= 3; biome++) {
    fresh_game(70 + (uint32_t)biome, 0, 0);
    uint32_t seed = nth_seed((biome - 1) * 5 + biome, 0);
    G.s.dv_state = (uint8_t)biome; G.s.dv_seed = seed; G.s.dv_cur = delve_gen(seed)->exit; G.s.dv_seen = 3; G.s.dv_min = 20; G.s.dv_moves = 12;
    std::memset(G.s.reserved, 0, sizeof G.s.reserved);
    uint8_t buf[GC_SAVE_BYTES];
    CHECK(gc_save_export(buf, sizeof buf) == GC_SAVE_BYTES);
    sim_start(80 + (uint32_t)biome, 10 * 60);
    CHECK(gc_save_import(buf, sizeof buf) == 1 && G.s.dv_state == biome);
    int met = 0, win = 0;
    for (int k = 0; k < 4 * 60 * 15 && !win; k++) {
      sim_step();
      if (in_fight()) met = 1;
      int bs = boss_slot();
      if (met && bs >= 0 && A[bs].counter == 0) win = 1;
    }
    CHECK(met && win);
  }
  std::printf("restore: same boss at full health, R10 saves meet a boss\n");
}

// the preview's "Fight this boss": GQ_DBG_BOSS puts the hero into the boss room of a level with that boss
static void test_debug_boss() {
  for (int b = 0; b < 15; b++) {
    fresh_game(500 + (uint32_t)b, 0, 0);
    gc_debug(GQ_DBG_BOSS, b);
    CHECK(world_area() == GQ_AREA_DELVE && boss_for(G.s.dv_state, G.s.dv_seed) == b && boss_slot() > 0 && A[boss_slot()].phase == b + 1);
    run_into_fight(0);
  }
  gc_debug(GQ_DBG_BOSS, 15);   // out of range: nothing happens
  std::printf("debug: all 15 bosses can be picked\n");
}

// ------------------------------------------------------------ whole missions (soak)
static void test_soak_missions(int per_biome) {
  int missions = 0, beaten = 0, fights = 0;
  for (int biome = 1; biome <= 3; biome++)
    for (int i = 0; i < per_biome; i++) {
      fresh_game(3000 + (uint32_t)(biome * 100 + i), i % 3 == 2 ? 3 : i % 3, i & 1);
      int faints0 = gc_query(GQ_Q_STAT_BASE + GQ_STAT_FAINTS);
      gc_debug(GQ_DBG_DELVE, biome | (int)(((((uint32_t)(i + 1) * 2654435761u + (uint32_t)biome) & 0x3FFFFFFFu) | 1) << 2));
      int exit_room = delve_gen(G.s.dv_seed)->exit, exit_min = -1, got = 0, fight_s = 0, home = 0, hearts = 0;
      for (int s = 0; s < 66 * 60 && !home; s++) {
        for (int k = 0; k < 4; k++) {
          sim_step();
          check_pool();
          if (A[A_HERO].act != ACT_BOSS_T) CHECK(live() == 0);     // the shots never outlive the fight
          if (A[A_HERO].act == ACT_BOSS_T) {
            if (!hearts) hearts = G.s.hero_hearts;
            if (boss_result() == 0) CHECK(G.s.hero_hearts == hearts);
            CHECK(world_area() == GQ_AREA_DELVE && walkable(tile_x(&A[A_HERO]), tile_y(&A[A_HERO])));
          } else hearts = 0;
          CHECK(gc_query(GQ_Q_HERO_PRESENCE) != GQ_PRES_NONE);
        }
        if (G.s.dv_state && world_area() == GQ_AREA_DELVE && G.s.dv_cur == exit_room && exit_min < 0) exit_min = G.s.dv_min;
        int bs = boss_slot();
        if (bs >= 0 && A[bs].counter == 0) got = 1;
        fight_s += A[A_HERO].act == ACT_BOSS_T;
        CHECK(G.s.dv_min <= 60 && G.s.dv_moves <= 90);
        home = G.s.dv_state == 0 && world_area() == GQ_AREA_VILLAGE && !world_transition_busy() && s > 60;
      }
      CHECK(home && gc_query(GQ_Q_STAT_BASE + GQ_STAT_FAINTS) == faints0);
      if (exit_min >= 0 && exit_min <= 55) CHECK(got);
      CHECK(fight_s <= 130);
      missions++; beaten += got; fights += fight_s > 0;
    }
  std::printf("soak: %d whole missions, %d bosses beaten (%d fights seen)\n", missions, beaten, fights);
  CHECK(beaten * 100 >= missions * 80);
}

int main() {
  sim_start(7);
  gc_set_date(2026, 3, 3, 2, 1000);
  test_mapping();
  test_attacks();
  test_pool();
  test_tired_forced();
  test_hits();
  test_mine_hit();
  test_determinism_work();
  test_abort_draw();
  test_beatable(std::getenv("BOSS_SEEDS") ? std::atoi(std::getenv("BOSS_SEEDS")) : 40);
  test_aborts_late();
  test_restore_fights();
  test_debug_boss();
  test_soak_missions(std::getenv("BOSS_SOAK") ? std::atoi(std::getenv("BOSS_SOAK")) : 12);
  std::printf("test_boss: all ok\n");
  return 0;
}
