# DESIGN-R11: 15 bosses for the deep missions (issue #1)

Status: design only, nothing implemented. Base: quest image 444,880 bytes (R10 plus whatever the task-duration /
date-line change that is in flight on this branch adds). Gates: check_bin warns at 450,560, CI fails at 481,280,
hard limit about 500 KB. Read together with docs/DESIGN-R8.md (the deep missions).

## 0. Summary

- Every deep mission ends in a boss room. The boss is `(biome - 1) * 5 + dvh(dv_seed, 0xB055, 7) % 5`: a pure
  function of the saved seed, so a mission resumed after a power cut meets the same boss. Nothing new is saved.
- 15 bosses, 5 per biome, each with its own 16x16 sprite (one frame; life comes from code: flip toward the hero,
  flash when struck, "!" before an attack). The art is in section 4, already checked against the real row table:
  **160 new pixel rows, 1,776 bytes**.
- All fights run on one small engine in a new file `lib/gamecore/src/boss.cpp`: a pool of 10 projectiles, one
  "burst" routine that produces every shot pattern, plus charge and teleport. A boss is a 9-byte table row.
- The hero does not use hearts here. He runs a private activity `ACT_BOSS`: each time he stands on a tile he
  scores that tile and its four neighbours (predicted shot positions, boss telegraph, wanted distance) and steps
  to the best; he darts in to strike when it is safe, and backs off while his swing recovers. A hit on him is a
  knockback of up to 2 tiles, 0.6 s stun and 0.8 s of grace. No heart is lost and he cannot faint.
- The fight always ends: the boss tires at 70 s (stops attacking), and at 110 s its health is set to zero.
  The 60-minute mission cap is unchanged; from minute 57 the hero walks past the boss.
- Budget: **about 4.9 KB flash (expect 4.3 to 5.6 KB), 100 bytes RAM**. Said plainly: the brief allows 7 KB, but
  only 5,680 bytes separate today's image from the check_bin warning line. The design is sized to fit under that
  line; if the measured total goes over, section 8 has the cut list. 7 KB cannot be spent without the warning.
- No recovery-layer file changes (section 11). The save record is untouched (24 reserved bytes stay).

## 1. What the code looks like today (facts the design rests on)

- Monsters are `Actor`s of kind `AK_MONSTER` (npc.cpp). They wander; `npc_monster_slot()` hands one to the hero's
  fight machine. `fight_step` (hero.cpp) is a dice roll: approach, three swings, win (poof, coins) or lose a
  heart. The story boss (Hollow King, chars cells 60..62) is the same machine with a 30-point penalty. There are
  no projectiles and no monster attacks anywhere in the game today.
- Deep exit room: `delve_populate` (npc.cpp) spawns 3 monsters near `SP_DOOR`; in the ruins the first is
  `GQ_MON_BOSS` (Hollow King sprite) and the others ghosts. `delve_step` phase 2 fights at decision 0 and again at
  decision 3 in the exit room, only with 2 or more hearts. A deep boss kill is reported as `GQ_MON_GHOST`.
- Drawing: `actor_ops` (world.cpp) turns each actor into up to 6 `Op`s (x, y, sheet, cell, flip, remap) in
  `s_ops[48]`; `rebuild_ops` hashes each actor's ops and marks the strips dirty when they change; `world_render`
  blits them. `Op.cell` is a uint8, `GC_CHARS_MAP` is uint8 per cell: up to 256 cells work unchanged.
- Sprites: `tools/paint_art.py` paints cells of `assets/src/chars.png` from ASCII rows; `tools/png2tiles.py` cuts
  whatever size the PNG has, stores every distinct 8-byte row once (`GC_ROWTAB`, 1,221 rows today) and 16 uint16
  row numbers per distinct cell. **chars.png is 8x8 = 64 cells with exactly one free cell (63, a grey checker
  placeholder). 15 bosses do not fit: the sheet must grow by two rows to 8x10 = 80 cells** (17 free, 15 used,
  78 and 79 stay blank). png2tiles needs no change; paint_art needs a "grow the sheet" step and the new cells.
- Tick: the device runs about 30 fps; the host tests tick with dt = 250 ms. Anything time-based must give the
  same kind of result at both, so the projectiles use fixed 32 ms sub-steps.
- Hero speed is `ms_per_px` (24, or 18 with boots; being raised 20-25 % on this branch). The design reads
  `h->ms_per_px` at run time and never assumes a value.
- Watchdogs: a hero phase may last 120 s, an activity 240 s (`ACT_DELVE` excepted). The fight is capped at 110 s.
- Save: `GqSave.dv_*` at 154..167, `reserved[24]` at 168. A restore puts the hero in the village; `ACT_DELVE`
  walks him back into room `dv_cur`, which is rebuilt and repopulated.

## 2. The engine (boss.cpp)

### 2.1 Reusable attack behaviours

Five code paths give every fight. Everything that flies is one routine, `burst()`.

| id | name | what it does | parameters |
|----|------|--------------|------------|
| B  | burst | emits one shot for every set bit of a 16-bit direction mask, rotated by the aim | mask id, speed, life, aimed / rotating |
| C  | charge | 0.7 s "!" telegraph, then the boss runs in a straight line along the row or column toward the hero until a wall, at 14 ms/px; touching the hero during the run is a hit | - |
| H  | hop | poof, the boss reappears on the hashed free tile farthest from the hero among 4 candidates | - |
| M  | walk | between attacks the boss follows `find_path` toward the hero for at most N tiles (N = 0: it stands still) | N, ms/px from the boss row |
| R  | rest | after each full cycle of three attacks the boss does nothing for 2 s: the hero's opening | - |

Burst masks (bit d = direction d of 16, 22.5 degrees apart, relative to the aim):

| mask id | name | shots | bits |
|---|---|---|---|
| 0 | single | 1 | 0 |
| 1 | spread | 3 | 15, 0, 1 |
| 2 | fan | 5 | 14, 15, 0, 1, 2 |
| 3 | cross | 4 | 0, 4, 8, 12 |
| 4 | ring | 8 | every even bit |

The shared attack table `ATK[]` (2 bytes each: kind/mask/flags, speed/life) has 12 entries:

| # | name | kind | mask | aim | speed px/s | life | telegraph |
|---|------|------|------|-----|-----------|------|-----------|
| 0 | SHOT | B | single | at the hero | 30 | wall | 0.4 s |
| 1 | BOLT | B | single | at the hero | 60 | wall | 0.5 s |
| 2 | SPREAD | B | spread | at the hero | 42 | wall | 0.5 s |
| 3 | FAN | B | fan | at the hero | 30 | wall | 0.6 s |
| 4 | RING | B | ring | fixed (dir 0) | 30 | wall | 0.6 s |
| 5 | SPIRAL | B | ring | rotates one step (22.5 deg) per use | 42 | wall | 0.3 s |
| 6 | CROSS | B | cross | fixed (dir 0) | 60 | wall | 0.6 s |
| 7 | XSHOT | B | cross | fixed (dir 2: the diagonals) | 60 | wall | 0.6 s |
| 8 | SLAM | B | ring | fixed | 60 | 24 px (1.5 tiles) | 0.9 s |
| 9 | MINE | B | single | - (speed 0, stays where the boss stands) | 0 | 4 s | 0 |
| 10 | CHARGE | C | | | | | 0.7 s |
| 11 | HOP | H | | | | | 0.3 s |

Optional 13th (first on the cut list): HOMING = SHOT whose velocity is re-aimed at the hero every 0.5 s for 3 s
(about 70 bytes). The table in section 3 marks where it would replace a SHOT.

A telegraph is the boss standing still with the existing `WI_EXCL` emote above it. It is what the hero's dodge
rule reacts to, and what makes the fight readable for a person watching.

### 2.2 Projectile pool

```
struct Shot { int16_t x, y;      // centre, 1/16 pixel (0..3839 fits)
              int8_t  vx, vy;    // 1/16 pixel per 32 ms step (speed 60 px/s = 31)
              uint8_t life;      // steps left; 0 = slot free
              uint8_t icon; };   // items.png cell                     8 bytes
static Shot s_shot[10];          // 80 bytes
```

- Count 10: the largest burst is 8. A burst that finds the pool full drops the shots that do not fit (never
  writes outside the pool; the test asserts it). A boss does not start a new burst while more than 2 shots live,
  so in practice nothing is dropped.
- Movement: an accumulator turns dt into 32 ms steps, at most 8 per tick (250 ms test tick; the device does 1).
  Extra time beyond 8 steps is discarded. Per step: `x += vx; y += vy; life--`.
- Direction table: 5 bytes `{16, 15, 11, 6, 0}` (cosine x 16 for 0, 22.5, 45, 67.5, 90 degrees), mirrored for
  the other quadrants. `vx = cos16[d] * v >> 4` with v = 15 / 22 / 31 for 30 / 42 / 60 px/s.
- Aim: the hero's direction quantised to 16 with four integer compares on |dx|, |dy|
  (`5*ay < ax` -> 0, `3*ay < 2*ax` -> 1, `2*ay < 3*ax` -> 2, `ay < 5*ax` -> 3, else 4), then the quadrant.
- Death: life 0, or the tile under the centre is not `walkable()`, or that tile is in column/row 0 or 14.
  The last rule closes the door gaps, so **a shot can never leave the room**. Rocks, the altar and ponds stop
  shots: cover for the hero, for free.
- Hit test against the hero: pixel box, `|shot.x/16 - (h->x + 8)| < 7 && |shot.y/16 - (h->y + 8)| < 7`.
  Skipped while he is stunned or in grace.
- Drawing: each live shot is one extra `Op` (sheet `SH_ITEMS`, the boss's icon, x - 8, y - 8, top = 1) appended
  in `rebuild_ops` through `boss_ops(Op*, room)`. 10 + hero 6 + boss 2 = 18 of the 48 op slots. Dirty rows: the
  shot tick calls `mark_rows(old_y - 8, 16)` and `mark_rows(new_y - 8, 16)` when a shot moved a whole pixel.
- Icons are existing item cells, **no new projectile art**: stone 16, mushroom 17, berry 18, shell 20, ore 21,
  gem 22, herb 25, gel 26, wood 9, bobber 3 (a 4-pixel red and white ball), star 32, sword 11, poof 39.

### 2.3 Boss state

The boss is an ordinary actor: `kind = AK_MONSTER`, `id = GQ_MON_BOSS`, **`phase = boss number + 1`** (0 = the
story Hollow King, so the old code paths can tell them apart), `counter` = health, `ms_per_px` from its row.
`npc_tick` returns early for an actor with `phase != 0` (boss.cpp drives it); `npc_monster_slot` skips it (the
dice fight never takes a deep boss). Fight state in boss.cpp, 16 bytes:

```
static struct { uint16_t acc;        // sub-step accumulator
                uint32_t ms;         // fight clock
                int16_t  t;          // timer of the boss state
                uint8_t  slot, st;   // boss actor; state: 0 walk, 1 telegraph, 2 charging, 3 rest, 4 tired
                uint8_t  ai, rot;    // attack index in the cycle; spiral rotation
                int16_t  cool;       // hero swing cooldown
                int16_t  stun;       // hero stun + grace left (stun while > 800)
                uint8_t  hand, pad;  // which way the hero circles (flips on a hit or a blocked step)
              } F;
```

Boss loop: walk N tiles (or stand) for the cooldown of its row -> telegraph -> attack -> next; after the third
attack rest 2 s. From `F.ms >= 70000` the state is "tired" for good: no moves, no attacks, `I_ZZZ` emote.

### 2.4 Boss row (flash, 9 bytes x 15 = 135 bytes, plus the names)

`{ hp, ms_per_px, walk_tiles, cooldown_100ms, icon, atk[3], flags }`; names are a packed string list
(about 150 bytes) read with a small indexed copy, as text_data.h does.

## 3. The 15 bosses

Health is in hero hits (1 per hit, 2 with sword level 2 or more). Chunk C lowered the five bosses that vanish or turn the hero around (rows 1, 2, 3, 8, 13) to the values below so that test_beatable reaches 95 % of fights before 70 s. Cells are in chars.png after the sheet grows:
`WC_DBOSS = 63`, boss b uses cell `63 + b`.

| b | cell | name | biome | hp | ms/px | walks | pause | attack cycle | shot icon |
|---|------|------|-------|----|-------|-------|-------|--------------|-----------|
| 0 | 63 | THORNBACK | forest | 8 | 40 | 2 tiles | 1.6 s | CHARGE, SPREAD, CHARGE | herb |
| 1 | 64 | OLD STUMP | forest | 8 | - | rooted | 1.4 s | SLAM, SHOT, FAN | wood |
| 2 | 65 | QUEEN BEE | forest | 5 | 30 | 3 tiles | 1.0 s | BOLT, HOP, BOLT | bobber |
| 3 | 66 | MOSSMAW | forest | 5 | - | hops only | 1.5 s | HOP, RING, MINE | gel |
| 4 | 67 | CAPTAIN CAP | forest | 8 | 90 | 1 tile | 1.2 s | SPIRAL, SPIRAL, MINE | mushroom |
| 5 | 68 | ROCKJAW | cave | 10 | 80 | 2 tiles | 1.6 s | SLAM, CROSS, SLAM | stone |
| 6 | 69 | GLOOMWING | cave | 6 | 30 | 3 tiles | 1.0 s | CHARGE, CHARGE, FAN | poof |
| 7 | 70 | SHARDSHELL | cave | 8 | 60 | 2 tiles | 1.4 s | CROSS, XSHOT, SPREAD | gem |
| 8 | 71 | BORER | cave | 4 | - | hops only | 1.3 s | HOP, RING, SHOT | ore |
| 9 | 72 | CINDER | cave | 8 | 55 | 3 tiles | 1.2 s | MINE, SPREAD, MINE | berry |
| 10 | 73 | THE WARDEN | ruins | 10 | 45 | 2 tiles | 1.5 s | CHARGE, BOLT, CROSS | sword |
| 11 | 74 | RATTLE KING | ruins | 8 | 70 | 2 tiles | 1.4 s | FAN, RING, SHOT (HOMING if kept) | shell |
| 12 | 75 | THE EYE | ruins | 7 | - | hovers in place | 1.0 s | BOLT, BOLT, SPIRAL | bobber |
| 13 | 76 | HEX | ruins | 4 | - | hops only | 1.2 s | HOP, SPREAD, XSHOT | star |
| 14 | 77 | STONE IDOL | ruins | 12 | - | rooted | 1.3 s | CROSS, XSHOT, SLAM | stone |

How they differ for someone watching: Thornback and Gloomwing rush him (he sidesteps out of the lane); Old Stump
and the Idol are turrets he must approach between volleys (the Idol forces a diagonal, then a straight, approach);
Mossmaw, Borer and Hex vanish and reappear, so he keeps having to cross the room; Captain Cap and the Eye fill the
room with slow turning rings he threads through; Cinder and Mossmaw leave hazards that close off floor; the Bee and
the Warden snipe with fast single shots; Rockjaw punishes standing next to it.

Looks: 0 a brown boar with a row of green thorns and a tusk; 1 a tree stump with two leafy sprouts, yellow eyes
and roots; 2 a striped bee with pale wings and a red crown; 3 a wide green toad with a red mouth; 4 a red
toadstool with white spots and a stern face; 5 a brown rock golem with ore-yellow eyes and teeth; 6 a dark blue
bat with spread wings and yellow eyes; 7 a brown crab carrying blue crystals; 8 a pink ringed worm rising from a
dirt mound, mouth full of teeth; 9 a red lava blob with yellow flames; 10 a grey armoured knight with a red plume
and a blue shield; 11 a crowned skull with red eyes; 12 a white eyeball with a blue iris and red tentacles;
13 a dark blue hooded mage with yellow eyes and a staff; 14 a grey stone face with moss.
## 4. The art (paste into tools/paint_art.py, CHARS section, after the Hollow King)

Format: full 16x16 cells in the palette letters of tools/palette.py (`.` transparent, k outline, g G l greens,
b B browns, s sand, w W blues, f foam, p skin, r red, o white, a grey, Y yellow). Every cell passes
`check_cell` (at least 20 outline and 30 transparent pixels; measured: 52..110 and 50..114). The rows were run
through the same de-duplication as png2tiles against the current three sheets: 160 of the 240 rows are new.

```python
# 63..77 the bosses of the deep missions (R11), one frame each: forest 63..67, cave 68..72, ruins 73..77
# 63 THORNBACK
C[63] = S([
    "................",
    "....l..l..l.....",
    "...kGkkGkkGkk...",
    "..kGgkGgkGgkGk..",
    ".kkbBBBBBBBBbkk.",
    "kBBBBBBBBBBBBbbk",
    "kBkoBBBBBBBBBbbk",
    "kBkkBBBBBBBBBbbk",
    "ksBBBBBBBBBBBbbk",
    "koksBBBBBBBBbbk.",
    ".kokkBBBBBbbbk..",
    "..kkbBkkkkbBk...",
    "...kbbk..kbbk...",
    "...kkkk..kkkk...",
    "................",
    "................",
])
# 64 OLD STUMP
C[64] = S([
    "....kk....kk....",
    "...kGGk..kGGk...",
    "..kGlGGkkGGlGk..",
    "..kGGGGGGGGGGk..",
    "...kkGGGGGGkk...",
    "..kkBBkkkkBBkk..",
    ".kBBBBBBBBBBBBk.",
    ".kBkkBBBBBBkkBk.",
    ".kBkYBBBBBBYkBk.",
    ".kBBBBBBBBBBBBk.",
    ".kbBBkkkkkkBBbk.",
    ".kbBBBkookBBBbk.",
    ".kbbBBBBBBBBbbk.",
    "kkbkbbBBBBbbkbkk",
    "kbkkkbbkkbbkkkbk",
    "kkk.kkk..kkk.kkk",
])
# 65 QUEEN BEE
C[65] = S([
    "......r..r......",
    "..kk..kYYk..kk..",
    ".kffk.kYYk.kffk.",
    "kffffkkYYkkffffk",
    "kfffkYYYYYYkfffk",
    ".kfkYkYYYYkYkfk.",
    "..kkYkYYYYkYkk..",
    "..kYYYYYYYYYYk..",
    "..kkkkkkkkkkkk..",
    "..kYYYYYYYYYYk..",
    "..kkkkkkkkkkkk..",
    "...kYYYYYYYYk...",
    "....kkkkkkkk....",
    ".....kYYYYk.....",
    "......kkkk......",
    ".......kk.......",
])
# 66 MOSSMAW
C[66] = S([
    "................",
    "................",
    "..kkkk....kkkk..",
    ".kooookkkkooook.",
    ".kokkoGGGGokkok.",
    ".kooooGGGGooook.",
    "kGkkkkGGGGkkkkGk",
    "kGGGGGGGGGGGGGGk",
    "kGkGGGGGGGGGGkGk",
    "kGGkkkkkkkkkkGGk",
    "kGlrrrrrrrrrrlGk",
    "kGGkkkkkkkkkkGGk",
    ".kGGllllllllGGk.",
    "kkGgkGGGGGGkgGkk",
    "kGGgkkkkkkkkgGGk",
    "kkkkk......kkkkk",
])
# 67 CAPTAIN CAP
C[67] = S([
    ".....kkkkkk.....",
    "...kkrrrrrrkk...",
    "..krrorrrrorrk..",
    ".krroooorrrrrrk.",
    ".krrorrrrroorrk.",
    "krrrrrrorroooork",
    "krrorrrrrrrorrrk",
    "kkkkkkkkkkkkkkkk",
    "...kssssssssk...",
    "...kskksskksk...",
    "...kssssssssk...",
    "...ksskkkkssk...",
    "...kssssssssk...",
    "...kssssssssk...",
    "..kkssbkkbsskk..",
    "..kkkkk..kkkkk..",
])
# 68 ROCKJAW
C[68] = S([
    "....kkkkkkkk....",
    "...kBsBBBBBBk...",
    "..kBsBBBBBBBBk..",
    "..kBkkBBBBkkBk..",
    "..kBkYBBBBYkBk..",
    ".kkBBBBBBBBBBkk.",
    "kBBkkokokokkkBBk",
    "kBbkBBBBBBBBkBbk",
    "kBbkkkkkkkkkkBbk",
    "kBbk.kBBBbk.kBbk",
    "kkk..kBBBbk..kkk",
    ".....kBYBbk.....",
    "....kkbkkbkk....",
    "...kBBbk.kBBbk..",
    "...kBbbk.kBbbk..",
    "...kkkkk.kkkkk..",
])
# 69 GLOOMWING
C[69] = S([
    "................",
    "k..............k",
    "kk....k..k....kk",
    "kwk..kwkkwk..kwk",
    "kwwkkwwwwwwkkwwk",
    "kwWwkwYwwYwkwWwk",
    "kwWwwwwwwwwwwWwk",
    "kwWwwkokkokwwWwk",
    "kwkwwwkkkkwwwkwk",
    "kk.kwwwwwwwwk.kk",
    "k...kwwwwwwk...k",
    "....kkwwwwkk....",
    ".....kwkkwk.....",
    ".....kk..kk.....",
    "................",
    "................",
])
# 70 SHARDSHELL
C[70] = S([
    "......kk........",
    ".....kfWk..kk...",
    "..kk.kWWk.kfWk..",
    ".kfWkkWWkkkWWk..",
    ".kWWkWfWWkWWwk..",
    "..kkBBBBBBBBkk..",
    "kk.kBBBBBBBBk.kk",
    "kBkkBokBBokBkkBk",
    "kBBkBkkBBkkBkBBk",
    "kkBkBBBBBBBBkBkk",
    ".kkkbBBBBBBbkkk.",
    "..kkkbbbbbbkkk..",
    ".kk.kkkkkkkk.kk.",
    ".k..k..kk..k..k.",
    "................",
    "................",
])
# 71 BORER
C[71] = S([
    ".....kkkkkk.....",
    "....kppppppk....",
    "...kpkppppkpk...",
    "...kppppppppk...",
    "...kkokokokkk...",
    "...kprrrrrrpk...",
    "...kkokokokkk...",
    "....kppppppk....",
    "....kkkkkkkk....",
    "....kppppppk....",
    "....kkkkkkkk....",
    "....kppppppk....",
    "..kkkkkkkkkkkk..",
    ".kbbakbbbbkabbk.",
    ".kkbbbbaabbbbkk.",
    "...kkkkkkkkkk...",
])
# 72 CINDER
C[72] = S([
    "......Y.........",
    ".....YrY...Y....",
    "..Y..YrrY.YrY...",
    ".YrY.kkkkkkrY...",
    ".YrkkrrrrrrkkY..",
    "..krrYrrrrrrrk..",
    ".krrYrrrrrrrrrk.",
    ".krrrkkrrkkrrrk.",
    ".krrrkYrrkYrrrk.",
    ".krrrrrrrrrrrrk.",
    ".krYrrkkkkrrrrk.",
    ".krrrrrYYrrrYrk.",
    ".kkrrrrrrrrrrkk.",
    "..kkkrrrrrrkkk..",
    "....kkkkkkkk....",
    "................",
])
# 73 THE WARDEN
C[73] = S([
    ".......rr.......",
    "......krrk......",
    ".....kkkkkk.....",
    "....kaaoaaak....",
    "....kakkkkak....",
    "....kaYkkYak....",
    "....kaakkaak....",
    "..kkkkaaaakkkk..",
    ".kaakaaaaaakWWk.",
    ".kaakaaoaaakWYk.",
    ".kkakaaaaaakWWk.",
    "..kkkaaaaaakkk..",
    "....kakkkkak....",
    "...kaak..kaak...",
    "...kaak..kaak...",
    "...kkkk..kkkk...",
])
# 74 RATTLE KING
C[74] = S([
    "..k...k..k...k..",
    "..kYk.kYYk.kYk..",
    "..kYYkYYYYkYYk..",
    "..kkkkkkkkkkkk..",
    ".kooooooooooook.",
    ".kokkkooookkkok.",
    ".kokrkooookrkok.",
    ".kokkkooookkkok.",
    ".koooookkoooook.",
    "..kooookkooook..",
    "..kkokokokokkk..",
    "...kkkkkkkkkk...",
    "...kokokokokk...",
    "....kkkkkkkk....",
    "................",
    "................",
])
# 75 THE EYE
C[75] = S([
    ".....kkkkkk.....",
    "...kkooooookk...",
    "..koooWWWWoook..",
    ".koroWWkkWWorok.",
    ".kooWWkkkkWWook.",
    ".kooWWkkkkWWook.",
    ".koroWWkkWWorok.",
    "..koooWWWWoook..",
    "...kkooooookk...",
    "....kkkkkkkk....",
    "..krk.krrk.krk..",
    "..krk..kk..krk..",
    ".krk........krk.",
    ".kk..........kk.",
    "................",
    "................",
])
# 76 HEX
C[76] = S([
    ".....kkkk....kk.",
    "....kwwwwk..kYYk",
    "...kwwwwwwk..kk.",
    "...kwkkkkwk..kb.",
    "..kwkYkkYkwk.kb.",
    "..kwkkkkkkwk.kb.",
    "..kwwwkkwwwkkpk.",
    ".kwwwwwwwwwwkkb.",
    ".kwwWwwwwwwwkkb.",
    ".kwwWwwwwwwwkkb.",
    ".kwwwwwwwwwwkkb.",
    "kwwwwwwwwwwwwkb.",
    "kwwYwwwwwwYwwkb.",
    "kwwwwwwwwwwwwkb.",
    "kkwwkkwwkkwwkkk.",
    ".kk..kk..kk.....",
])
# 77 STONE IDOL
C[77] = S([
    "..kkkkkkkkkkkk..",
    ".kaoaaaaaaaaaak.",
    ".kakkkaaaakkkak.",
    ".kakrkaaaakrkak.",
    ".kakkkaaaakkkak.",
    ".kaaaaakkaaaaak.",
    ".kaaaakaakaaaak.",
    ".kakkkkkkkkkkak.",
    ".kakokokokokkak.",
    ".kakkkkkkkkkkak.",
    ".kaaaaaaaaaaaak.",
    "..kkkkkkkkkkkk..",
    ".kkaaGaaaaaaakk.",
    ".kaaaaaaaaGaaak.",
    ".kkkkkkkkkkkkkk.",
    "................",
])
```
### 4.1 Tool changes and art cost

- `tools/paint_art.py`: (a) before painting, if a sheet is shorter than its highest expected cell needs, make a
  new image of the needed height filled with magenta and paste the old one at the top (chars.png 128x128 ->
  128x160); (b) add cells 63..77 to `EXPECTED["chars.png"]`; (c) fix the header comment (cell 63 is no longer
  spare; 78 and 79 are). The script stays idempotent.
- `tools/png2tiles.py`: no change (`GC_CHARS_COUNT` becomes 80, `GC_CHARS_MAP` 80 bytes).
- world_priv.h: `WC_DBOSS = 63`.
- Cost, measured with the de-duplication of png2tiles: 160 new rows x 8 = 1,280; 15 cells x 32 = 480 (14 new
  cells, the placeholder in 63 is replaced, plus one blank distinct cell for 78/79); map +16.
  **Total 1,776 bytes** (about 118 per boss). The two checker rows of the old cell 63 leave the table (-16).
- A second frame was drawn for none of them. Where it would be cheap (Queen Bee wings, Mossmaw mouth, Eye
  blink: 32 bytes of row numbers plus 2 to 4 new rows, about 55 bytes each) it can be added later; the engine
  would need 1 flag bit and about 20 bytes. Not in the budget.
- Free animation in code: flip so the boss faces the hero; `visible` toggles when struck (as today);
  the "!" emote during a telegraph; Zzz when tired; `WI_POOF` on a hop and on death.

## 5. The hero in a boss fight (ACT_BOSS in hero.cpp, logic in boss.cpp)

`ACT_BOSS = 36`, reported as `GQ_ACT_FIGHT`. hero.cpp only starts it and handles the result; `boss_step(h, dt)`
does everything else and returns 0 (running), 1 (ended without a win), 2 (boss beaten).

### 5.1 Each tick
1. Fight clock, boss state machine, projectile sub-steps, hit test (section 2).
2. If stunned: nothing else. If swinging (`h->timer > 0`, `POSE_SWING`): nothing else.
3. If the hero is `arrived()` (standing exactly on a tile): one **decision** (5.2). Otherwise he keeps walking
   the single tile he chose; he is never committed for more than one tile.

### 5.2 The decision (integer only, no RNG, fixed order)
Inputs: hero tile, boss tile, the 10 shots, the boss state, `F.cool`, `F.hand`, his current direction.
`ready` = swing cooldown over and the boss is not in a telegraph or a charge.

a. **Strike**: if `ready`, he is next to the boss (Manhattan distance 1) and his own tile has danger 0: face
   the boss, `POSE_SWING` for 260 ms, the boss flashes and loses health, `F.cool = 900`. Done.
b. If `ready`: `find_path(h, tile next to the boss)` (the existing BFS and `stand_next_to`); remember its first
   tile as `goal`. Otherwise there is no goal.
c. Score five candidates: stay, and the four neighbours in the order "ahead, `F.hand` side, other side, back".
   A candidate that is not walkable or is the boss's tile is skipped. Lower is better:
   - **shots**: let N = `16 * h->ms_per_px / 32` (steps he needs for one tile). For every live shot, look at its
     position after N/2, N and 3N/2 steps (three adds); each one inside a 14-pixel box around the candidate's
     centre costs 64. For "stay" the samples are 0, N/2 and N.
   - **telegraph**: while the boss telegraphs or charges: +40 if the candidate is within 2 tiles of the boss
     (Chebyshev); for CHARGE also +40 if it is in the boss's row or column; for CROSS / XSHOT +40 if it is on the
     boss's row or column / on a diagonal.
   - **range**: with a goal, -8 for the goal tile. Without one: `4 * |Manhattan distance to the boss - 3|`, plus
     3 if the candidate shares the boss's row or column. So while his swing recovers he drifts to 3 tiles away
     and off the firing lines; when ready he comes straight in.
   - **style**: -2 for "ahead" (he does not jitter), +1 for "stay" (he keeps moving), +2 for "back".
   Ties go to the earlier candidate. He takes the best one: `path[0] = tile, plen = 1`.
d. If the step he wanted was not walkable, or he was just hit, `F.hand` flips: he starts circling the other way.

Why it looks intentional: he circles the boss at a steady distance in one direction, steps out of a lane the
moment the "!" appears, comes in only during the rest or between volleys, strikes once or twice and backs out.
All of it follows from the four score terms; there is no random walk in the fight.

Work bound: per decision 5 candidates x 10 shots x 3 samples = 150 box tests, 5 telegraph tests, at most one BFS
(225 tiles, the same call every walk uses today). At most one decision per tick, and on the device one per tile
(about 0.4 s). Per tick besides: 10 shots x at most 8 sub-steps, each one `walkable()` and one box test.
A host-only counter (`#if !defined(ARDUINO_ARCH_ESP8266)`) counts box tests and sub-steps for the test.

### 5.3 When he is hit
A shot inside his box, or the charging boss within 10 pixels: the shot is removed; `POSE_HURT`; knockback =
up to 2 walkable tiles straight away from the shot's larger velocity axis (away from the boss for a charge or a
mine), written as his path so he slides there at walking speed in the hurt pose; `F.stun = 1400`: he is stunned
(no decisions) until the slide is over and 600 ms passed, and shots pass through him for the remaining 800 ms.
No heart is lost, no `GQ_EV_HURT`, no faint. The cost of a hit is time and position, nothing else.

### 5.4 Why the fight always ends
1. Health only goes down, by at least 1 per strike, and starts at 6..12.
2. From 70 s the boss is tired: no attacks, no movement. The last shot is gone at most 4 s later (the longest
   life is the mine). From then on every decision has a goal, the BFS path to the boss exists (checked when the
   fight starts: no path -> no fight), danger is 0, and the "range" term makes him follow it: at most 26 tiles,
   then one strike per 1.16 s. Worst case 74 + 13 + 12 x 1.16 = about 101 s.
3. At 110 s the health is set to 0 whatever happened (counts as a win; the test asserts it is never needed).
   110 s is below the 120 s phase watchdog and the 240 s activity limit, so those never fire in a fight; if
   they ever did, they end in `go_home()`, which ends the mission as today.
4. Bed time, a festival, `go_home()` from any source: the fight is simply abandoned; `boss_clear()` empties the
   pool whenever a room is populated or the hero leaves `ACT_BOSS`.
5. The mission clock keeps counting during the fight. A fight starts only while `dv_min < 57`; later the hero
   walks to the way out past the boss. A mission therefore ends at most 2 minutes after the 60-minute mark,
   inside the 66 minutes the existing test allows.

## 6. Where the boss is, the band, the reward

- **Room**: the exit room of every biome. `delve_populate` spawns for an exit room exactly one actor: the boss,
  on the tile 2 steps inward from `SP_DOOR` (a fixed tile: it "guards the way out"; `free_tile` only if that
  tile is blocked). This replaces the Hollow King + 2 ghosts of the deep ruins and the packs of 3 at the forest
  and cave ways out. The story boss in `GQ_AREA_RUINS` (chapter 4) is not touched.
- **Arena**: in an exit room `delve_build_room` skips the middle block and the first scatter row (14 trees /
  5 stalagmites / 3 rocks). The chest, the altar and the minor scatter stay as cover. About 40 bytes. The golden
  room hashes in test_delve.cpp change only if one of the three golden rooms is an exit room; re-record if so.
- **Order in the room** (`delve_step` phase 2): boss first, whatever his hearts (he needs none), also in exit
  mode while `dv_min < 57`; then the existing chest, node and wander decisions; then the way out. The second
  "pack" fight at decision 3 is removed.
- **Band**: `hero_status_text` returns `<NAME> HP <n>` during the fight, e.g. `RATTLE KING HP 5` (at most 17 of
  the 19 characters), in the same slot as `DEEP RUINS 12/24`. It does not touch the date line being added to the
  band on this branch. When the boss falls the hero says `<NAME>|IS BEATEN!` (one say, existing box).
- **Reward** (in hero.cpp, on result 2): hearts back to the maximum, `find_coins()` (1..3 coins under the
  existing 10-a-day cap), one biome trophy through `gain()` (forest herb, cave ore, ruins gel: all capped by
  `ITEM_CAP`), `GQ_EV_SLAIN` with `GQ_MON_GHOST` as today (statistics count it, the story does not advance).
  The room's chest and the errand payment on return are unchanged. A gem would be a better trophy but the
  fountain needs only 2 gems and test_econ limits income; I do not propose it.

## 7. Save and restore

- Nothing is added to `GqSave`. Boss number = f(dv_state, dv_seed). `reserved[24]` stays 24, version stays 1,
  `gq_save_sanitize` and test_save.cpp are unchanged.
- Restore mid-fight: the hero wakes in the village with the saved `dv_*`, walks back in, the exit room is not in
  `dv_seen`, so it is populated again with the same boss at full health: the fight restarts.
- Restore after the win but before he left the room (a window of about 90 s, and only if the 20-minute save
  happened inside it): he fights the same boss again and gets the capped reward again. Closing this needs one
  saved bit (1 reserved byte, about 30 bytes of code and a test_save change). Not proposed; say if you want it.

## 8. Budget

### 8.1 Flash

| part | where | bytes |
|---|---|---|
| 15 sprites: rows 1,280 + cells 480 + map 16 (measured) | assets_gen.h | 1,776 |
| boss rows 135, attack table 24, masks 10, direction table 5 | boss.cpp | 175 |
| 15 names, packed | boss.cpp | 150 |
| shots: step, wall/edge test, hit test, ops, dirty rows | boss.cpp | 430 |
| burst + aim | boss.cpp | 230 |
| boss state machine: walk, telegraph, charge, hop, rest, tired | boss.cpp | 520 |
| hero decision: candidates, score, strike | boss.cpp | 520 |
| hit: knockback, stun, grace | boss.cpp | 150 |
| fight start / end / clear, fixed spawn tile | boss.cpp, npc.cpp | 180 |
| hero.cpp hooks: ACT_BOSS case, delve_step line, reward, band text, say | hero.cpp | 300 |
| world.cpp hooks: boss cell + flip in actor_ops, boss_ops in rebuild_ops, arena | world.cpp | 150 |
| npc.cpp: early return for bosses, slot filter; removed pack code | npc.cpp | -20 |
| optional HOMING | boss.cpp | 70 |
| **total** | | **about 4,630 without HOMING, 4,700 with; expect 4.3 to 5.6 KB** |

Against the gates: 444,880 + 4,700 = 449,580, which is 980 bytes under the 450,560 warning, before whatever the
change in flight on this branch adds. So this is tight, and the first measurement after chunk B decides.
The brief's ceiling of 7 KB would give 451,880: over the warning line, far below the CI limit.

Cut list, in order, if the image passes 450,560:
1. HOMING (-70).
2. Band shows `BOSS HP 5` and the win line is fixed text: the 15 names leave flash (-190).
3. SPIRAL rotation and XSHOT become RING and CROSS (-40); the Idol and Shardshell lose a little character.
4. Arena clearing (-40).
5. Knockback becomes stun in place (-70).
6. Last resort, and against the request: 4 bosses per biome (-125 each for art and row).

### 8.2 RAM

| part | bytes |
|---|---|
| `s_shot[10]` | 80 |
| fight state `F` | 20 |
| **total static** | **100** (target 200) |

No new field in `Actor` (8 x that would cost more than the pool). Stack: the decision uses about 40 bytes;
`find_path` uses its existing static arrays.

## 9. Host tests (new test/test_boss.cpp; ASan/UBSan like the others)

Helper: `GQ_DBG_DELVE` starts a mission; a test-only setter puts `dv_cur` on the exit room (as test_delve.cpp
does with `set_room`) so each fight is reached in seconds. Seeds are searched so that all 15 bosses come up.

1. **Mapping**: over 3,000 seeds the boss number is in the biome's range, each of the 15 occurs, and it is the
   same after export / import of the save and after `delve_gen` of other seeds in between.
2. **Beatable**: every boss x 40 seeds x sword level 0 and 3 x both hero speeds x tick sizes 33 ms and 250 ms:
   the fight ends with result 2 within 110 s of fight clock; in at least 95 % of runs before the 70 s tired
   point; the forced end at 110 s is never used; median length between 20 and 60 s (tuning guard).
3. **No hearts**: `hero_hearts` never drops during a fight, `GQ_STAT_FAINTS` unchanged, no `GQ_EV_HURT`.
4. **Never stuck**: during a fight the hero's tile changes or he strikes at least once every 6 s unless stunned;
   the stun never lasts over 1.5 s; two hits are never closer than 1.4 s; he is always on a walkable tile, never
   on the boss's tile, `GQ_Q_HERO_PRESENCE` never 0.
5. **Shots**: after every tick every live shot has its centre on a walkable tile in columns and rows 1..13;
   the live count is at most 10; a guard byte after the pool (host build) is untouched; all shots are gone
   within 4.2 s of the last burst and when the fight ends or the room changes.
6. **Determinism**: the same seed, global RNG state and tick sequence give the same frame hash
   (`frame_hash()`) every 5 s and the same fight length; `gc_rng_get()` is unchanged by a whole fight except
   through the existing reward calls (the fight itself draws no random numbers; hop tiles use `dvh`).
7. **Work bound**: the host counter is at most 240 per tick (150 decision + 80 sub-steps + margin); at most one
   BFS per tick.
8. **Mission** (extend test_delve_hero.cpp `test_missions`): the existing limits hold with the fight included
   (home within 66 minutes, `dv_min <= 60`, no faint, 80 % reach the exit); new: every mission that reaches the
   exit room before minute 57 beats its boss; a mission forced to `dv_min = 58` in the exit room leaves without
   a fight.
9. **Aborts**: bed time, a festival and a forced activity during a fight: back in the village within 2 minutes,
   pool empty, `dv_state` as the existing abort tests expect.
10. **Restore**: export during a fight, import into a fresh core: same boss number, full health, fight
    completes; a save from R10 (no boss concept) resumes and meets a boss.
11. **Art**: `paint_art.py` twice gives identical PNGs; `png2tiles.py` output has `GC_CHARS_COUNT == 80`; a test
    renders each boss cell and checks it is not blank and differs from the other 14.
12. Existing suites unchanged and green: test_econ (income limit 132), test_soak, test_save, test_world,
    test_ui. test_delve.cpp `check_actors`: the exit room now holds 1 monster; golden hashes re-recorded if hit.

## 10. Implementation chunks (in order)

**A - art and sheet** (tools/paint_art.py, assets/src/chars.png, lib/gamecore/src/assets_gen.h regenerated,
world_priv.h `WC_DBOSS`). Paste section 4, grow the sheet, regenerate.
Accept: test 11; all existing tests pass; nothing references the cells yet, the linker keeps the tables, so the
image grows by 1,776 +/- 60 bytes: report the number. Screenshot of the 15 cells on their biome floor looked at.

**B - engine** (new boss.cpp and boss.h; world.cpp: `boss_ops` in `rebuild_ops`, boss cell and flip in
`actor_ops`; npc.cpp: boss spawn in the exit room at the fixed tile with `phase`, early return in `npc_tick`,
filter in `npc_monster_slot`, `boss_clear()` from `delve_populate`). Pool, burst, aim, boss state machine,
tables. The boss attacks a hero who does not react yet (a debug hook lets the test stand him still).
Accept: tests 1, 5, 6, 7 (shot part); every attack id fired once in a test and its shot count checked;
existing tests pass; +1.9 KB or less.

**C - the fight** (boss.cpp: decision, strike, hit, tired, forced end; hero.cpp: `ACT_BOSS`, the `delve_step`
line, reward, band text, win line, `hero_activity`; world.cpp: arena rule). hero.cpp and ui.cpp are being edited
on this branch: this chunk starts after that work is merged, and its hero.cpp diff is five small hunks.
Accept: tests 2, 3, 4, 7, 8, 9, 10, 12; a preview run of one fight per biome watched at normal speed;
+1.4 KB or less.

**D - tuning and release** (boss table numbers only, tests, docs/DESIGN-R11.md copied into docs, RELEASE-NOTES.md,
preview page if it lists monsters). Tune health and pauses against test 2 (median 30 to 45 s), final size check
against 450,560, apply the cut list if needed, 3-day soak.
Accept: full CI green, check_bin without the size warning, size and RAM lines in the report.

A and B can run in parallel (B uses the Hollow King cell until A lands). C needs B. D needs all.

## 11. Risks and the recovery layer

Recovery layer: **no change.** firmware/safety.cpp, config.cpp, prefs.*, store.*, main.cpp and
tools/lint_safety.py are not touched; the record stays 192 bytes at version 1; nothing new is written to flash;
`tools/recovery_guard.py` contract files are unchanged, so no `GQ_RECOVERY_REV` bump. All new code is in
lib/gamecore and tools/paint_art.py.

Risks:
1. **Size.** About 1 KB of room under the warning line after this feature, less after the change in flight.
   The art is measured, the code is estimated; chunk B's measurement is the checkpoint, the cut list is ready.
2. **Frame rate during a volley.** Up to 10 moving shots can make most of the 15 strips dirty every frame, so
   the screen refresh drops toward the full-redraw rate for a second or two, as heavy rain does. Mitigations
   already in: slow shots, at most 10, bosses wait until 2 or fewer shots remain. If it looks bad on the device:
   pool 8 and RING on every second direction pair (6 shots).
3. **The hero looks too good or too clumsy.** The score weights are the tuning surface (four numbers). Test 2
   guards the length; the look needs a person watching the preview in chunk C.
4. **Test tick of 250 ms.** At that tick he moves about 8 pixels a tick and decides on arrival, so fights on the
   host are coarser than on the device. Test 2 runs both tick sizes for that reason.
5. **Global RNG order.** The fight draws no random numbers, but replacing the pack fights changes how many are
   drawn in an exit room: tests with fixed expectations on a seed may need their values re-checked.
6. **hero.cpp / ui.cpp conflicts** with the work in progress: chunk C waits for it and reads `ms_per_px` live.
7. **Band refresh.** The health number changes during the fight; if ui.cpp caches the status text, it needs the
   existing "status changed" call on each strike (a call, not a ui.cpp edit). Check in chunk C.
8. **Item icons as shots** are 10 to 12 pixels wide: readable, but a ring of 8 stones is busy on a 13-tile room.
   If the owner prefers small shots everywhere, the bobber (4 pixels) can be every boss's icon at no cost, or one
   new 16x16 "orb" cell costs about 60 bytes (items.png has no free cell and would grow a row).

Met only in a reduced form (said plainly):
- "Summon minion" is not in the set: minions need their own contact rule and hero reaction (about 300 bytes) and
  add less variety than SPIRAL, MINE and HOP together. Offer: add it later for one boss per biome.
- One frame per boss; movement comes from code. A second frame is about 55 bytes per boss where rows repeat.
- The 7 KB allowance cannot be used without the size warning; the design fits in about 4.7 KB instead.
- A boss beaten just before a power cut may have to be beaten again (section 7).
