// R11 bosses of the deep missions (docs/DESIGN-R11.md 2, 3, 6). Private to the game core.
// The engine: 15 boss rows, one burst routine for every shot pattern, a pool of 10 projectiles, charge, hop,
// tire at 70 s and forced end at 110 s, and the hit on the hero (knockback, stun, grace; no heart is lost).
// Everything is integer only and allocation free. The hero's dodge logic (ACT_BOSS) is chunk C and uses
// the accessors below; this file never moves the hero except for the knockback of a hit.
//
// How chunk C drives a fight (hero side):
//   boss_begin(h)        once, when the hero stands in the exit room: 1 = fight started (a boss is here, its
//                        tile can be reached, dv_min < 57), 0 = no fight (walk on).
//   each tick            world_tick() already calls boss_tick(dt) after hero_tick(), so the boss, the shots, the
//                        hit test and the clock run by themselves. The hero's part is:
//                          r = boss_result();  1 -> finish(h);  2 -> reward, say, finish(h)
//                          if (boss_busy())    return;           // stunned, sliding back: no decision
//                          if swinging         return;
//                          if (arrived(h))     decide();         // uses the accessors below
//   strike               when the hero's swing lands call boss_strike(): it takes 1 health (2 with sword
//                        level 2 or more), flashes the boss and returns 1 if that was the last health.
//   leaving              boss_clear() whenever he leaves ACT_BOSS (also called when a room is populated).
#pragma once
#include "world_priv.h"

// Size switches (the cut list of docs/DESIGN-R11.md 8; both 0 = the full design). Define as 1 before this header
// (build flag) to drop the feature: NAMES = every boss is called BOSS (band "BOSS HP 5"), saves about 250 bytes;
// KNOCKBACK = a hit stuns in place instead of sliding back 2 tiles, saves about 90 bytes.
#ifndef BOSS_CUT_NAMES
#define BOSS_CUT_NAMES 0
#endif
#ifndef BOSS_CUT_KNOCKBACK
#define BOSS_CUT_KNOCKBACK 0
#endif

#define BOSS_COUNT 15
#define BOSS_SHOTS 10

enum { BA_SHOT, BA_BOLT, BA_SPREAD, BA_FAN, BA_RING, BA_SPIRAL, BA_CROSS, BA_XSHOT, BA_SLAM, BA_MINE, BA_CHARGE, BA_HOP };
enum { BS_WALK, BS_TELE, BS_CHARGE, BS_TIRED };   // a rest after the third attack is BS_WALK with nothing to do   // boss_phase()

struct BossShot {          // 8 bytes; a slot with life 0 is free
  int16_t x, y;            // centre, 1/16 pixel (0..3839)
  int16_t vx, vy;          // 1/16 pixel per 32 ms step
  uint8_t life;            // steps left
  uint8_t icon;            // items.png cell
};
#if !defined(ARDUINO_ARCH_ESP8266)
#define BOSS_GUARD 1       // host builds: one extra slot after the pool that must stay zero (test)
extern unsigned g_dbg_boss_work;     // shot sub-steps (each with its hit test) since the test reset it
extern unsigned g_dbg_boss_forced;   // fights ended by the 110 s rule
extern unsigned g_dbg_boss_hits;     // hits on the hero (boss_took_hit is consumed by the hero's own step)
#else
#define BOSS_GUARD 0
#endif
extern BossShot g_bshot[BOSS_SHOTS + BOSS_GUARD];

struct BossFight {
  uint32_t ms;                   // fight clock
  uint16_t acc;                  // sub-step accumulator, ms
  int32_t  t, flash, stun;       // timer of the boss state; boss blink left; hero stun + grace left (stun while > 800)
  uint32_t rdy;                  // clock time of the hero's next swing
  uint8_t  slot, st, ai, rot, walk, hc, atk, res, on, hit, hand, dir;   // hand: which way he circles; dir: his last step 0..3
};
extern BossFight g_bf;

// ---- room and spawn
int  boss_for(int biome, uint32_t seed);      // biome 1..3 (dv_state), seed: the boss number 0..14, a pure function
void boss_init_actor(Actor* a, int b);        // phase = b + 1, counter = health, ms_per_px from the row
int  boss_name(int b, char* out);             // b = 0..14; out has at least 12 bytes; returns the length
int  boss_band(char* buf, int cap);           // "<NAME> HP <n>" of the live boss; 0 when there is none
int  boss_slot(void);                         // its actor slot or -1

// ---- fight control
int  boss_begin(Actor* h);                    // see above; resets clock, health stays as spawned
void boss_clear(void);                        // empties the pool, ends the fight (result 1)
void boss_tick(uint32_t dt);                  // world_tick(): state machine, shots, hit, timers
int  boss_busy(void);                         // 1 while the hero is stunned or still sliding back
int  boss_strike(void);                       // the hero's blow lands; 1 = boss dead
int  boss_step(Actor* h);                     // the hero's whole fight, each tick: 0 running, 1 ended, 2 beaten

// ---- what the hero's decision needs
// live shots: the pool g_bshot (centre x/16, y/16, vx, vy per 32 ms step, life 0 = free)

// small accessors (inline: the hero's step and the tests use them; nothing is compiled unless used)
static inline int boss_result(void) { return g_bf.res; }                 // 0 running, 1 ended without a win (or none), 2 beaten
static inline int boss_took_hit(void) { int r = g_bf.hit; g_bf.hit = 0; return r; }   // 1 once after the hero was hit (consumed)
static inline int boss_phase(void) { return g_bf.st; }                   // BS_*
static inline int boss_telegraph(void) { return g_bf.on && (g_bf.st == BS_TELE || g_bf.st == BS_CHARGE) ? g_bf.atk + 1 : 0; }   // 0, or BA_* + 1 of the attack being telegraphed / the charge running
static inline int boss_hp(void) { return A[g_bf.slot].counter; }
static inline uint32_t boss_clock(void) { return g_bf.ms; }              // ms of the fight
