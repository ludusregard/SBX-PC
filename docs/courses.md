# Courses (`BUILD_CRS`)

The course files in full: what each one holds, how the game reads it and
what is code rather than data. The course table, the ids and the load
order are in `game_data.md` ("Courses"); this file goes inside the files.

How much is known, per item:

- **Checked**: read in this game's code and checked against this copy's
  files (`DATA/BUILD/TXR/BUILD_CRS_OUT`, the GUT Archive Tools dump; file
  `NNNNNNNN` is id 23009 + N).
- **From ITC**: from ITC-Recomp's notes (`docs/functions/track-route.md`,
  `ai-drivers.md`, `collision-world.md`, `docs/known-data.md`) for the
  same functions in Import Tuner Challenge, mapped with
  `itc_to_sbx_functions.tsv` (exact or call-matched) and checked against
  this game's files where a file shows it. Field names there are guesses
  unless the row says read in full.
- Anything else says "probably" or "not known".

All examples are the race map's Night ids (variant 0, 23011-23383).

## Time of day (checked)

The race map's three variants are the time of day: 0 Night, 1 Midnight,
2 Dawn (race data +32, written only by `sub_820FFAB8`; see `game_data.md`).
Each time is a whole set of files, but much of it is shared or the same:

| File | Night / Midnight / Dawn |
|---|---|
| Collision mesh (23011, 23384, 23757) | Byte-identical |
| Route (23012, 23385, 23758) | Byte-identical |
| Set file 2, entry 0 (the areas) | Byte-identical |
| Set file 2, entries 1 and 2 (light groups) | Differ: 5,280, 1,680 and 1,072 bytes of light-group textures and animations (most at Night) |
| Area light files | Same size; area 0's has the same 825 lights (positions, colors, ranges) at each time, a few bytes differ |
| Set file 1, entry 0 (the course XPR2) | Differ: 95.6, 92.7 and 92.7 MB |
| Set file 1, entries 1-7 | Most differ in content, same sizes |
| Area models (364) | None byte-identical; 167 the same size (median 73 bytes differ, header words repeated every 512 bytes), 197 different sizes |

So the time of day is textures, light groups (lamps, signs, flares) and
small model differences, over one driving surface and one route. Times
also choose:

- The music: `Music::PlayRequested` (`sub_821741B8`) picks one of three
  track lists by race data +32.
- A light kit level: `LightKit::BlinkLevel` (`sub_820DC120`) and
  `LightKit::ChaseLevel` (`sub_820DC238`, from ITC; the cars' blinking
  and chasing LEDs) use a full level of 0.99 (`0x82002A08`) at Dawn.
- The color filters: `RaceScene::SetEffectParams` (`sub_8228C9A8`) takes a
  44-byte record per course index (place × 3 + time) from
  `0x82627C18` (default `0x82627BEC` for places 8 and up). All 24 records
  and the default hold the same 11 floats (0.5, 0.7, 0.5, 0.5, 1.5, 0.75,
  0.7, 0.7, 0.49, 0.32, 0), so the game has per-course, per-time grading
  that it doesn't use. A mod can give each time its own.

No per-time sun, ambient or fog table has been found; the race scene's
11 lights (`RaceLights::SetUp`, `sub_8215AF70`) don't read the time.

## The route (from ITC, checked against 23012)

The road as a graph. Loaded once a race (`TrackRoute::Load`,
`sub_820F9D08`) to `0x826B57F0` (+0 the file, +4 the slices' total, +8
the state). The file is big-endian:

```
+0   u32  section count            (295)
+4   u32  corner count             (315)
+8   f32  slice length in meters   (6.25)
+12  u32  offset of the section table (a u32 offset per section)
+16  u32  offset of the corner table  (a u32 offset per corner)
+20  12 × u32 offsets of the route-bit lists
```

`TrackRoute::OnLoaded` (`sub_820F8F30`, checked) turns every offset into a
pointer, applies the route-bit lists (below) and counts the slices
(22,283: 139.3 km of road).

### A section

```
+0    u16  slice count n
+2    u8   kind (low 4 bits): 0 main road (256), 1 (14), 3 (6), 4 (6), 5 (13)
+3    u8   branch number: a bit in the branch masks (1-14, 29 used)
+4    3 × f32  bounds minimum (x, y, z)
+16   3 × f32  bounds maximum
+28   12 links, 4 bytes each: s16 section (-1 none), s16 slice there.
           0-2 forward, 3-5 back, 6 and 8 the sections beside it
+76   12 × 8 signed bytes: the lane each lane ends up in over that link
+124  n × 84-byte slices
then  4 tables of n 12-byte points (x, y, z), one per slice (checked):
      +124 + 84n   the road's edge toward -side
      +124 + 96n   the road's edge toward +side
      +124 + 108n  the outer margin toward -side (the edge plus +16)
      +124 + 120n  the outer margin toward +side (the edge plus +20)
```

Kinds 1-4 are roads the links treat apart (2 and 4 skipped going
forward, 1 and 3 going back), probably ramps, junctions and parking
areas. The default branch mask `0xDFFFFE7F` leaves out branches 7, 8
and 29 (`TrackRoute::IsMainRoad`, `sub_820F8D78`).

### A slice (84 bytes)

```
+0   3 × f32  center
+12  f32  road width (m)
+16  f32  margin beyond the -side edge
+20  f32  margin beyond the +side edge
+24  f32  lower limit across the road (route bits raise it)
+28  f32  upper limit across the road (route bits lower it)
+32  2 × u16  the collision mesh's section and slice here
+36  2 × f32  side direction (x, z)
+44  2 × f32  forward direction (x, z)
+52  (4 bytes, not known)
+56  u8   lane count (low 4 bits): 2 on most of the map, 1, 3 and 4 too
+57  u8   kind: 0, 1, 3, 4, 5 (3 and 4 tested by `TrackRoute::SliceTest`
          and `TrackRoute::SpecialSection`)
+58  u16  count of corners listed here (0-6)
+60  up to 6 × 4 bytes: u16 corner number, u16 slice (where on the
          corner this slice is)
```

A position on the route is (section, slice). `TrackRoute::Follow`
(`sub_820FB560`) keeps a point's position as it moves;
`TrackRoute::LanePoint` (`sub_820F8548`) places cars by lane;
`TrackRoute::Distance` (`sub_820F9158`) gives meters between positions.

### Route bits: closures (checked)

Twelve lists, one per bit of race data +24. For each bit set,
`OnLoaded` changes the listed slices:

```
+0  u16  entry count
+2  u16  mode: 0 raises +24 to the value, else lowers +28 to it
+4  entries of 8 bytes: s16 section, s16 slice, f32 value
```

On the map the lists hold 19-39 slices each, with limits of about 0.44
to 7.2 m across the road. The collision mesh pairs with them: wall kinds 50-61
are kept only when bit (kind - 50) is set (`WallQuery::IgnoresWall`,
`sub_820FD5A8`, from ITC), and the mesh has about 31 walls of each. So a
route bit closes an entrance or a lane with walls, and the AI's road
narrows to match. Who sets race data +24 isn't traced yet.

## Corners: the AI's speed and line (checked fields, ITC's meanings)

The AI doesn't drive a fixed line. Each tick it plans up to 80 entries
ahead (one a slice) with an offset across the road, shaped by the
corners listed on the slices it will pass, and sets a target speed
from them. A corner in the route file's +16 table:

```
+0   u16  entry count n (the corner's length in slices)
+2   u16  count of sections in its path (1-6)
+4   f32  sign = the corner's direction (left or right)
+8   f32  t: a blend floor for the speed values below
+12  f32  speed value before the turn-in      +16 f32  room before it (m)
+20  f32  speed value turn-in to apex         +24 f32  room there
+28  f32  speed value apex to exit            +32 f32  room there
+36  6 × s16  the sections the corner runs through (-1 pad)
+48  u16  (not known)
+50  u16  turn-in, slices from the corner's start
+52  u16  apex
+54  u16  exit
+56  n × 8 bytes: u16 section, u16 slice, f32 the line's offset across
          the road at that slice (the racing line)
```

Example, corner 0: 148 slices through sections 5 then 0, values 669.0,
then (669.0, 7.6), (669.0, 7.6), (594.2, 7.6), turn-in and apex at 81,
exit at 144.

How the game uses it (all from ITC, read in SBX where noted):

1. **Taking a corner** (`AiPath::AddCornersAt`, `sub_8213D278`, read here):
   a slice's corner list adds a corner to the driver's next 8 only when
   the driver's planned sections (its +5146) match the corner's path at
   +36. A corner belongs to a way through the forks, not a spot.
2. **The line** (`AiPath::PlanCorners`, `sub_8213D0C0`, and its
   `ApproachCorner`, `CornerToEntry`, `CornerToApex`, `CornerToExit`):
   the planned offsets move toward the corner's table offsets (+56) in
   four stretches: up to the corner, to the turn-in (+50), to the apex
   (+52), to the exit (+54, at 0.8 of the step). `AiPath::ApplyCornerLine`
   (`sub_8213A5D8`) copies the table's offsets into the plan.
3. **The speed** (`AiDriver::LimitForCorner`, `sub_821390C0`, read here: it
   loads +8, +12, +20, +28 and +50, +52, +54): for each of the three
   stretches, the pair (value, room) gives a speed through
   `AiDriver::CornerFactor` (`sub_82143D10`, read here):

   ```
   speed = 30 + 10 × sqrt(value - 10) × grip × weight   (value above 10)
           - 10 × (car diagonal - room)                 (room below it)
           at least 30
   ```

   Then `AiDriver::LimitSpeed` (`sub_82137CC8`) adds the distance still
   to go: `+ distance × (100 / speed above 100 km/h, else 1) × brakes`
   (`AiDriver::SpeedOverDistance`, `sub_82143A50`). The target speed is
   lowered to the smallest. That's the braking point: the AI brakes once
   its speed passes the corner's speed plus what it can still lose over
   the distance left.
4. **The car's factors** (`AiDriver::ReadCar`, `sub_821438C0`, read here):
   the car diagonal from the vehicle's bounds (+5136 to +5160); grip from
   vehicle +3900, weight from 2000 / vehicle +3444 (at most 1.0), brakes
   from vehicle +3508 + +3512. These are the vehicle's physics data
   (what each is isn't confirmed here).
5. **The driver**: the skill's halfword +10 scales every target speed by
   1 + p × 0.0025 (`AiDriver::UpdateSpeed`, `sub_82139770`); its +4 is a
   base speed and +6 a throttle cap. The skill comes from the AI profile
   (the driver's +500, its +120; `AiSkill::Load`, `sub_8213E038`).

The value works like a radius: 181 gives about 161 km/h for a car with
both factors at 1.0, 669 about 287 km/h. That it's in km/h is
inferred from the driver's speed (+100) being km/h.

### Forks

Which way a driver goes is code: `AiPath::PreferredSections`
(`sub_8213ADD8`) picks one of 12 lists of 64 bytes at `0x82008950`
(`.rdata`; section numbers ending with -1) by the AI profile's bytes +36
(4-9) and +90. The lists (checked, from the image):

| List | Sections |
|---|---|
| 0 | 248 247 246 291 290 289 288 287 286 |
| 1 | 108-94, 245 244 243 287 286 |
| 2 | 193 198-195 203-199 205 204 209-206 211 231 |
| 3 | 154-151 8-1 170-168 206 211 231 |
| 4 | 246 291 127-125 120-110 146 145 |
| 5 | 216 215 213 214 212 223 221 222 220 219 228 124-122 120-110 146 145 |
| 6 | 276 275 28-25 19 18 |
| 7 | 193 198-195 203-199 205 204 24-22 25 19 18 |
| 8 | 187 194 193 198 49-44 |
| 9 | 154-151 8-1 234-232 271-264 52-50 46-45 44 |
| 10 | 107-94 180-178 224 182 181 185-183 186 192 82-79 76 75 |
| 11 | 276-257 86-83 76 75 |

The branch mask lists (`AiPath::BranchMasks`, `sub_8213CE30`) are in
`.rdata` too.

## The collision mesh (from ITC, checked against 23011)

Loaded to `0x826B57FC` by `TrackMesh::Load` (`sub_820F7680`):

```
+0  u32  section count (252)
+4  u32  offset of the section table
```

A section:

```
+0   u16  slice count
+2   u16  offset of its outline (8-byte x, z points)
+4   6 links, 6 bytes each: s16 a slice here, s16 another section,
         s16 its slice (-1 unused)
+64  u32  offset of the vertices (12 bytes: x, y, z)
+68  u32  offset of a u16 per vertex (probably an RGB565 color)
+72  u32  offset of the slices (28 bytes)
+76  u32  offset of the faces (12 bytes)
+80  u32  offset of the walls (8 bytes)
```

- A slice: +0 face count, +2 first face, +4 wall count, +6 first wall,
  +12 to +18 the outline's 3 or 4 corners (0xFFFF none), +20 a height,
  +24 two u16 kinds (0, or 30, 31, 32: a track position's flags 2, 4, 8).
- A face: 4 × u16 vertices (all quads in this copy), +8 u16 the surface
  kind (0 or 20; 4,870 of 61,989 are 20), +10 0xFFFF.
- A wall: 2 × u16 vertices, +4 u16 kind. 60,071 have kind 0 (always
  solid). The others are switched by `WallQuery::IgnoresWall`: 2 and 3
  by the car and the race mode; 8-10 by race data +20 bits; 16-22 and
  30-41 kept only when the race mode (race data +4) is the kind's
  number (ITC's mapping: 16-22 modes 0-6, 30-37 11-18, 38-39 21-22,
  40-41 19-20); 23 kept for layouts 4, 5 and 7; 50-61 by the route bits.

## The set files and areas (checked)

Containers (little-endian count, then offsets).

Set file 1 (23383, 113 MB, stored):

| Entry | Night size | What |
|---|---|---|
| 0 | 95.6 MB | XPR2: the course's textures |
| 1 | 10 KB | A model |
| 2 | 5.6 MB | 45 models |
| 3 | 77 KB | 4 models; the minimap draws the first ones as its map |
| 4 | 16 bytes | An empty list |
| 5 | 11.7 MB | 12 models |
| 6 | 336 KB | 25 models |
| 7 | 6 KB | 2 point lists (the minimap's markers) |

Set file 2 (23013): entry 0 the 5 areas (each its four corners in x and
z, used to stream areas near the camera, and 96-byte model records),
entry 1 the light groups' textures and flipbook animations, entry 2 the
6 light groups (billboards: lamps, flares).

An area's light file (23014...): one entry, a light set (area 0's: 13
groups, 825 lights). A light is 48 bytes: position
(x, y, z), range (12.5, 2.5 or 0), color (RGBA, 0-255 floats), two
sizes. `Lights::FindNearest` gives the 10 nearest to what's being lit
(called from the car draws).

An area model (23015...): entry 0 the model (node and part files, as in
`BUILD`), entry 1 its XPR2: 288 have their own textures, 76 a 16-byte
empty stand-in (they use the course XPR2's textures).

## What's code, not data

For new courses or a changed map, these live in the image, not the
files:

- The course table (`0x825F2E40`, 24 entries: place × 3 + time) and the
  place choice (place 1 for race modes 8-10).
- The 70 track zones (`TrackZones::TrackZones`, `sub_822889F0`; road
  names and the expressway signs): route ranges per zone, hard-coded.
- The preferred sections (above) and the branch masks.
- `TrackRoute::SpecialSection` (`sub_820F8C18`): sections 293, 230, 156,
  20, 39 and 77 by number.
- The wall kinds' rules (`WallQuery::IgnoresWall`) by race mode.
- The color filter records (`0x82627C18`) and the music lists by time.

## For native code and the tools

- One route and one mesh serve all three times. A new time of day needs
  only visual files: set files, light files and area models.
- Speed and brake changes: the corner values (+12, +20, +28) and rooms
  (+16, +24, +32) per stretch, the turn-in, apex and exit slices, and
  the line's offsets. Safest as an override applied after
  `TrackRoute::OnLoaded` (the same step that applies the route bits), or
  per driver through the skill's speed scale. Replays and online need
  every player to have the same values.
- sbxr_tools: the route's slices, edges, margins and corners (colored by
  speed) can be drawn over the BUILD_CRS 3D view from the files alone.
  The area models' 16-byte entries are empty XPR2 stand-ins; their
  textures are in set file 1's XPR2 (the "course XPR link" it's missing).

## Not known yet

- What sets race data +24 (the route bits).
- Corner +48 and the meaning of t (+8); slice +52.
- What the model differences between times are (vertex colors,
  light-group links, extra shapes in the 197 of other sizes).
- Which bytes of the light files differ between times.
- Surface kind 20, and the vertices' u16.
- The 16-byte pairs in BUILD_CRS 24167-24250.
