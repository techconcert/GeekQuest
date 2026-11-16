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
#else
#define BOSS_GUARD 0
#endif
extern BossShot g_bshot[BOSS_SHOTS + BOSS_GUARD];

// ---- room and spawn
int  boss_for(int biome, uint32_t seed);      // biome 1..3 (dv_state), seed: the boss number 0..14, a pure function
void boss_init_actor(Actor* a, int b);        // phase = b + 1, counter = health, ms_per_px from the row
void boss_name(int b, char* out);             // at least 12 bytes
int  boss_band(char* buf, int cap);           // "<NAME> HP <n>" of the live boss; 0 when there is none
int  boss_cur(void);                          // number 0..14 of the live boss of this room or -1
int  boss_slot(void);                         // its actor slot or -1

// ---- fight control
int  boss_begin(Actor* h);                    // see above; resets clock, health stays as spawned
void boss_clear(void);                        // empties the pool, ends the fight (result 1)
void boss_tick(uint32_t dt);                  // world_tick(): state machine, shots, hit, timers
int  boss_result(void);                       // 0 running, 1 ended without a win (or none), 2 beaten
int  boss_busy(void);                         // 1 while the hero is stunned or still sliding back
int  boss_strike(void);                       // the hero's blow lands; 1 = boss dead
int  boss_took_hit(void);                     // 1 once after the hero was hit (consumed): circle the other way

// ---- what the hero's decision needs
int  boss_phase(void);                        // BS_*
int  boss_telegraph(void);                    // 0, or BA_* + 1 of the attack being telegraphed / the charge running
int  boss_hp(void);
uint32_t boss_clock(void);                    // ms of the fight
// live shots: the pool g_bshot (centre x/16, y/16, vx, vy per 32 ms step, life 0 = free)
