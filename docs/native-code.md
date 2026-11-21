# Native game code

The game's functions rewritten as our own C++, one subsystem at a time,
each behind its own `--native_<name>` flag. On the F4 menu they're the
Overrides tab, grouped by the subsystem's first word (Race, Cars, Scene,
Menus and UI, Data and Saves, Audio, Runtime): a flag's
`SBX_SETTING_BOOL(name, true, "Overrides", "<group>", ...)` places it
(`src/base/settings.h`, "Settings" in `docs/build_run.md`). The fixes that had a tab of their own
are groups of the Hooks tab ("Fixes: Race", "Fixes: Interpolation").
Every flag is on by default. With a flag off the recompiled
function runs,
so a flag can be turned on and off between runs to compare. Each
function has a row in `docs/functions.md`, "Native game code".

`build_screen` has no flag: its functions are the build screen's lists
and draws, none a plain set-up or getter.

Stays recompiled, by rule: the boot-once set-ups that copy stack words
they never write (RaceSceneMachine::Init, RaceFlow::Init `sub_8215E9A8`:
only the original reproduces them), and the C++ library's containers the
game instantiated on its heap (std::list, std::string, and std::map as the
screen object's UvMap and UvColumn).

## How each one is made

ITC-Recomp has converted much of its build of the game to C++. We use it as
a reference only; nothing of ITC's is compiled in.

1. `tools/port_native.py` rewrites ITC's source to SBX's addresses, and
   grades each function: A, the same instructions (addresses masked); B or
   C, similar code that differs somewhere.
2. For each function, SBX's generated code (`generated/default`) is read
   until it's understood: what object it works on, what each field is,
   what it calls and why. ITC's C++ says what the function does; for B and
   C functions the differences are found in SBX's code and written down
   here.
3. The function is written as plain C++ over typed structs (below, and
   `CLAUDE.md`, "Native game code: plain C++ only"): the game's object as a
   struct in `src/game/`, fields named or kept as `unknown_<offset>`, calls
   through `Guest::Call`. Struct offsets aren't addresses, so a struct SBX
   lays out differently from ITC gets SBX's offsets.
4. Where ITC's code calls into a part of its native layer we don't have
   (its C runtime, its heap, its archive), ours calls the recompiled
   function instead.
5. It's compiled here; then tested in play with the flag on and off.

The tools in `tools/native_gen/` (its README) help with step 2: an
instruction viewer, the registers a function reads, a symbolic runner that
lists a function's stores and calls. Their generators (fill tables,
copies) made the older files; their output isn't committed any more.

Calls into recompiled code set the argument registers the callee reads
(`live.py` lists them) and nothing else (`Guest::Call` does this). The game
was built with whole-program optimization, and a few callers read a
register after a call when they know the callee doesn't touch it
(String::Find's callers read r5 after the wildcard search); keep those.

`tools/native_gen/difftest` (Linux) runs an override beside the recompiled
function it replaces on random memory with stub callees and compares what
they did. It's optional (since 2026-11-10): play testing is the check, and
difftest is for chasing a difference found in play. It can't run functions
that loop on what they read.

An address that already has an override of SBX's own (a wrapper that
watches, logs or reorders the call) keeps it: our C++ for the function is
in its subsystem's file, declared in `src/hooks/native_calls.h`, and the
wrapper calls it in place of the recompiled function.

Float math is written plainly, in float (a single add, subtract, multiply
or divide in float gives the generated code's result). Fused steps
(fmadds, fnmsubs) are `float(std::fma(...))` where the exact result
matters (the physics). Arguments in f1 are the callers' doubles.

## Typed game structs (`src/game/`)

Since 2026-11-10 the native game code is being moved from `src/hooks/` to
`src/game/` and rewritten as ordinary C++ over typed structs; `src/hooks/`
keeps SBX's own features (the patch loader, custom music, autosave, the
fixes). The aim is code that can be read and extended, not register-for-
register fidelity: behavior the game depends on is kept, stack garbage and
null-pointer quirks aren't.

`src/game/guest_types.h` is the shared layer:

| Piece | What it is |
|---|---|
| `be<T>` (`u16_be`, `i16_be`, `u32_be`, `i32_be`, `f32_be`) | A big-endian field that reads and writes as a normal value |
| `ptr<T>` | A 4-byte guest pointer field, used like a `T*` |
| `At<T>(address)`, `Element<T>(table, index)`, `AddressOf(p)` | A host pointer from a guest address (0 gives `nullptr`), and back |
| `Guest g(ctx, base)` | `g.Call(sub_X, args...)`, `g.CallAddress`, `g.CallVirtual(object, slot, args...)`: arguments in r3 onward, nothing else touched |
| `ReturnAddress` | The return address callees see, for the calls a hook tells apart by it (custom music, autosave, subtick) |
| `GUEST_SIZE`, `GUEST_OFFSET` | Compile-time checks of a struct's layout |

The structs are in `src/game/*.h` (`math.h` Vector and Matrix,
`std_types.h` String and PtrList, `vehicle.h`, `collision.h`, `menu.h`,
`model.h`, and one header per subsystem converted since, such as
`race_overlays.h`, `scene_points.h` or `car_spec.h`), laid out byte for
byte as the game's, with `unknown_<offset>`
members for what isn't worked out. Each override is a thin adapter: the
flag's test, the registers read into typed pointers, a plain C++ function,
the result in r3. Constants from the image's read-only data are written as
literals (checked against the image); addresses in its writable data
(`0x82074000` and up) stay named addresses, read from guest memory each
time, even when the file holds a constant or zeros there: the game sets
them at run time. `difftest` with `SBX_IMAGE` set catches a literal made
from writable data (it loads only the read-only data, so the original reads
test memory and the native doesn't).

Every native game file is in this form now; `src/hooks/` keeps SBX's own
features only. The old form (long runs of stores as tables, copies as runs,
register-by-register bodies generated by a script) is gone, and nothing new
is written that way. Type names are shared
across `src/game/` (one `game::Wheel`, one `game::SceneLights`): a new
struct gets a name no other header uses.

## Subsystems

| Flag | File | What | Differences from ITC's build |
|---|---|---|---|
| `native_menu_sounds` | `src/game/menu_sounds.cpp` | MenuSound::Play `sub_82110318`, Sound::Play `sub_82173710`, SoundOptions::SetEffectsVolume `sub_821B80C8` and SetMusicVolume `sub_821B8198`; the sound manager's calls stay recompiled | Sound::Play's slot call returns to `0x8217374C` (ITC `0x82173750`) |
| `native_data_files` | `src/game/data_file.cpp` | DataFile::GetEntry `sub_82292170` (the containers' entries: light sets, course and car data, the font) | none |
| `native_framework` | `src/game/framework.cpp` | The framework's getters and its interfaces' thunks, DebugText::SetPosition `sub_82091120`, DebugFont::SetPosition `sub_82571988` | `sub_82090858` reads +80 (ITC +88). ITC's run loop containers and constructors need its heap: not done |
| `native_car_specs` | `src/game/car_specs.cpp` (types in `src/game/car_spec.h`) | The car parameter tables (assets 72-83): Params::Init, Load and Free, the loads' callbacks and every byte swap, the getters, the aero parts; Car::Refresh `sub_820C47A8` with CopySpec, CopyAeroParts and Tire::InchUp `sub_820C44C8`. The archive, the heap and memcpy stay recompiled | SBX's car struct has its settings 4 bytes earlier from +1652 on (engine stage, tires, inch-ups, wheels, aero levels); Car::Refresh and its callees are written from SBX's code. Tire::InchUp subtracts the 0.5 inch of rim width from 175 mm up (not below) |
| `native_menu_helpers` | `src/game/menu_helpers.cpp` | MenuCursor::Update `sub_82184468` and MenuCursor::Pressed `sub_821843C8` (`docs/menus.md`); the action mapper's slots stay recompiled | none |
| `native_race_hud` | `src/game/race_hud.cpp` | RaceHud::RunMovie `sub_82211F50` (which UI movie the race HUD runs by its state); the movies stay recompiled | none. RivalLogo::Draw `sub_8220F7B0` too, called from subtick's override (`src/hooks/native_calls.h`) |
| `native_data_tables` | `src/game/data_tables.cpp` | The data tables at `[0x826B5980]` (the item records, asset 94, and the 11 tables loaded with them): DataTables::Init and Load, the loads' callbacks and byte swaps (SwapWord `sub_820C9578` included), the lookups | The item records are 3,004 bytes (ITC 3,008). The swaps swap the integer (the 360's lfs/stfs keeps a signaling NaN's bits) |
| `native_file_layer` | `src/game/file_layer.cpp` | The file classes' constructors and destructors, deleting destructors included (File, AsyncFile, GameFile), GameFile's opens by name, AsyncFile::Detach, Path and GamePath (Path::Build `sub_82579250`, GamePath::SetGame and Set); opening and reading files stay recompiled | SBX's own vtables and format strings |
| `native_track_zones` | `src/game/track_zones.cpp` | The HUD's route zones each tick: the fixed-size lists, RouteRange's tests, Zone::Update, TrackZones::Update and IsInside, Racers::Vehicle `sub_820C94B8`. The once-a-race set-up (TrackZones::TrackZones, Create, the 70 zones' set-ups) stays recompiled | Racers::Vehicle reads the racers at `[0x826C7D9C]` + 4 x (racer + 717) (ITC +577) |
| `native_race_scene` | `src/game/race_scene.cpp` | The race scene at `[0x826C7D7C]`: RaceScene::RaceScene, Create, Destroy, DestroyCars; RaceLights::InitEntry and SetUp; Glows::SetParams `sub_8228C498` and RaceScene::SetEffectParams `sub_8228C9A8` (through the effects' vtables, so `--bloom` still applies); RaceSceneMachine::TogglePause and Resume. RaceSceneMachine::Init stays recompiled (it copies stack words it never writes) | none |
| `native_shader_objects` | `src/game/shader_objects.cpp` | The graphics library's shader objects: ShaderFile::Parse, the vertex and pixel shaders' Init, CopyHeader and Create, Mesh::LoadShaders `sub_8257A620`, Shaders::OnPairLoaded `sub_820A08B8`; the allocations, XMemCpy and memcpy stay recompiled | The object's size test masks 0x7F (ITC 0x3F); VertexShader::Init's fetch words at +68 and constants at +196 (ITC +64, +192); the vertex microcode XMemAlloc'd 0xA5800000 (ITC 0xB5800000) with a 0x0BADF00D word after each microcode; the loaders' vertex microcode copied with XMemCpy and AllocPhysicalEx'd with flags 4 (ITC memcpy, 0x404) |
| `native_screen_objects` | `src/game/screen_objects.cpp` | The race's screen object: SetSpeed `sub_8212E928` (every tick), Release, the destructor. Its UvMap and UvColumn trees (the game's std::map, on its heap) and the rings' set-up stay recompiled | The strength at +52 (ITC +76), the UvMap at +76096 (ITC +76124); SetSpeed calls `sub_8212E440` with 0.0 when the object is running and goes on (ITC returned) |
| `native_scene_models` | `src/game/scene_models.cpp` | The scene models: ScenePools::SetUpB and the car scene's pools' set-ups (car model, parts, cockpit, interior, lights, traffic body), SceneModel::SetUp (its engine sounds), Build and ApplyRecord, the records' defaults and copies, the parts' links, the accessories' getter, the sound loads' callback, SceneModel::Update `sub_820D9E48` (every frame), the Apply steps, the first set's part loads (CarParts::Load and both sets, the lights' asset, TrafficCar::Load) and their callbacks. memcpy, the first set's pools, the wheels' set-up and the loads' polls stay recompiled | CarRecord::Copy's memcpy is 44 bytes and its fields are at SBX's own offsets; SceneModel::Update tests the scene's +5000; SceneModel::SetUp's engine sound variant for parts word 11 of 3 or 4 is 1 (no tail call) |
| `native_save_data` | `src/game/save_data.cpp` | The save data's defaults (stats, progress, options, control maps, course records), a slot's create and delete, the two-player block, a course record's look-up, the boot objects' teardown, the players' controllers, MenuList::Init `sub_82209868`, TitleScreen::ResetPlayer. SaveData::NewGame `sub_82113FB0` too, called from `unlock_cut_cars`' override (`src/hooks/native_calls.h`) | The stats' and options' layouts as SBX's code has them; ControlMap::SetDefault's 15 buttons; the two-player block is 240,100 bytes (two parts of 119,908) |
| `native_accessories` | `src/game/accessories.cpp` | The interior accessories' set at `[0x826B51F0]` (create, scale, acquire, release), the accessory's constructor and destructor, the skeleton's destructor, and the 36-byte AccessoryFile (its load and callback). The archive, the entries' own constructors and destructors, the skeleton's construction and the textures stay recompiled | none found |
| `native_strings_and_font` | `src/game/strings_and_font.cpp` | The string table at `[0x826B5914]` (Init, Release, the load checks, Strings::Get, a text's pages), a string with its arguments (Strings::FormatV `sub_8210AAC0`, Format, FormatGrouped), the font at `[0x826B58E0]`: Text::Init, Load and Draw, Font::Init, Load, Draw, SetParams, the layout (TextWidth, LineWidth, LineX, LineY, LineGlyphs) and the glyphs' vertices (CountGlyphs, BuildVertices `sub_82109BD0`, AddGlyph, ClipGlyph, OutCode), XPR::Release. Font::Font and its shaders, Font::DrawPass, the turned text's rotation and swprintf stay recompiled | The table is 2,256 bytes with its counts at +2188 (ITC 4,304 and +4236); the font is 8,400 bytes (ITC 16,592): its spacings at +8256/+8260, colors at +8336, shaders from +8368; FormatV's frame is 2,752 bytes (Format's 2,736) with its 2,048-byte text cleared once at the start and before 0xFFD7, 0xFFD3 and 0xFFD2 |
| `native_screen_constructors` | `src/game/screen_constructors.cpp` | 35 of the front end's screen constructors (panels, groups, node tables, list and node screens, the type 6 and 7 screens' sets). Six stay recompiled: the two panel screens (they allocate and test a member), the two list screens and PanelSet (they store through a pointer they load or get), ColorListScreen (it copies a word it loads) | The stores are SBX's own (tables) |
| `native_game_modes` | `src/game/game_modes.cpp` | The modes' state machines (StateKey::Equals, StateMachine::IndexOf, Add, Find, the constructor), Modes::Modes, Mode::Mode, the race overlays machine's flags. StateMachine::Handle `sub_82106410` too, called from `title_screen.cpp`'s override (`src/hooks/native_calls.h`); the race mode's and the overlays machine's set-ups stay recompiled | none found |
| `native_collision` | `src/game/collision.cpp` | The collision shapes: the box shape's getters and edge tables, the shapes' and queries' constructors, CollisionShape::UpdateBounds `sub_820BAA50` (every tick), the placements, a contact's set and flip, the shape's translate. The vector library, the box-box tests, the impacts and the pairs' response and solve stay recompiled | none found in these; three of the getters are shared by the linker with other classes (the same constant or field) |
| `native_car_physics` | `src/game/car_physics.cpp` | The car physics' set-ups and accessors: the car data's, vehicle's, rigid bodies' and collision objects' constructors and resets (tables), the collision surfaces' values, the vehicle's input setters and ramps, the wheels' surface values, the wheel records' copies, the rigid body's setters. The vehicle's step (suspension, tires, drivetrain, gearbox, aero, the rigid body's integration), the collision world and the tuning stay recompiled | The collision surface D's setter writes another default slot than its getter reads (as the original) |
| `native_vehicle` | `src/game/vehicle.cpp` | The vehicles' construction, refresh and removal, Vehicle::UpdateTrackPositions `sub_820C1B98` (every tick), TrackPosition::Copy, the teardown. The route positions' update, the input and the set-ups stay recompiled | none found |
| `native_race_states` | `src/game/race_states.cpp` | The race state machine's simple handlers (its step, the fades, quit, unload, the standing start, begin), RaceProgress::Reset and CountTime, and the route helpers each racer's progress uses every tick (Time::Sub, RoutePosition::NotAfter, RouteSections::Before and Beside). The race's ticks, the route following, laps, checks, finish and ranking stay recompiled | none found |
| `native_material_passes` | `src/game/material_passes.cpp` | The part models' materials: counts and records, the alpha setters, the paint table's lookups, the two block-set tests. The paints, palette, textures and culling stay recompiled | none found |
| `native_replay_snapshots` | `src/game/replay_snapshots.cpp` | The race snapshots' copies: the bodies' states, the cars' parts and members, the track positions, as lists of field copies on typed snapshot/live struct pairs. The replay's own save and restore stay recompiled | The copies are SBX's own (worked out from its code) |
| `native_ui_movies` | `src/game/ui_movies.cpp`, `ui_movie_draw.cpp`, `ui_movie_nodes.cpp`, `ui_movie_text.cpp` | The UI movies: file lookups and load fix-ups, UiMovie::Update and ApplyTrack (through subtick's overrides, `src/hooks/native_calls.h`), the drawing (transforms, pictures, primitives, nodes, the text queue, key curves), nodes, textures, text items, animation lists and times, the text setters, play/move/loop/finish, the movie table, open, load and unload. Left recompiled: the ticker's step and offset (subtick's), two functions with uninitialized std::string temporaries | none found |
| `native_data_load` | `src/game/data_load.cpp` | The boot data's set-up and unload: BootData::Load, CreateObjects and Unload, the race data's, input recordings' and replay's creates and their records' constructors, the defaults' bit sets (Defaults::Init `sub_820CCB60` with Bits224 and Bits480), the two boot movie lists' loads and frees, Text::Unload | Defaults::Init's sets at SBX's addresses, written as the ranges it leaves |
| `native_race_sounds` | `src/game/race_sounds.cpp` | The race sounds' handlers (the listener, the countdowns, the rolling start, the next stage, an abort, leaving after the race) and two of the cars' sound steps. The sounds' ticks, the music's pick and the cars' updates stay recompiled | none found |
| `native_race_screens` | `src/game/race_screens.cpp` | The race screens' page steps, the menu lists' selection and entries, the pause and after-race menus' answers, the race dialog's answer and destroy, the results screens' creates and starts. The screens' ticks, draws and transitions stay recompiled | none found |
| `native_race_hud_object` | `src/game/race_hud_object.cpp` | The race HUD object: its gauges, gear, boost, player, warning, draw, item, touching test and teardown, the movies' starts and runs, PowInt. The UI movies, the HUD's set-up and its racers' ticks stay recompiled; RaceHud::RunMovie is `native_race_hud`'s | none found |
| `native_course_scenery` | `src/game/course_scenery.cpp` (types in `src/game/course_scenery.h`) | The course scenery: the manager's create, set-up, start, load check, unload and destroy; its object pools; the streaming lists; Scenery::Update with CollectAreas, CollectModels and LimitLargeModels (every update); the draws (each pass): Scenery::Draw (through the frame state diff's wrapper), DrawArea, DrawSetLists, RecordHeadlightProjectors; the course set's, areas' lights' and area models' constructors, loads, callbacks, parses and resets; the texture animations (every frame). TextureAnimations::Advance is subtick's | none found |
| `native_nearest_lights` | `src/game/lights.cpp` (types in `src/game/lights.h`, containers in `src/game/data_file.h`) | Lights::FindNearest `sub_820F3FF8`: the course lights nearest a point and within a cone, for the car models' lighting; the tests four lights at a time (SSE), the merge and sort one by one as the original | The w written is the float at `0x82000A20` (ITC `0x82000A30`) |
| `native_scene_loaders` | `src/game/scene_loaders.cpp` (types in `src/game/scene_loads.h`) | The scene pools: their nodes, the first set's make, create and release; the seven pool objects' constructors, destructors, resets and releases; the car model's reset; the cockpit's, interior's and traffic car's file callbacks; the cockpit's and traffic car's texture binds; their loads' constructors, destructors and resets; the load checks; the parts' walks; CarParts::Part and SlotOf; ModelPoint::Find; an XPR's records by name. The loads themselves stay recompiled. `--log_scene_loaders` logs each distinct call (on or off, to compare runs) | none found |
| `native_scene` | `src/game/scene.cpp` | The scene set-up's, scene assets' and car scene's constructors, the assets' destructor and release, a list's destructor, the pages freed in a range. The scene's updates and teardown stay recompiled | none found |
| `native_second_set` | `src/game/second_set.cpp` | The scene parts' point records: the constructors and destructors for the car body's parts, the cockpit, the interior, the lights, the traffic cars and the wheels, the scene objects' resets, the finds, a point record's node and shape names, the models' stream getters, Morph::Run. The groups' defaults, the searches, the draws, the morph's blend and the parts' releases stay recompiled | none found |
| `native_load_poll` | `src/game/load_poll.cpp` (types in `src/game/load_poll.h`) | SceneModel::PollLoad and its finishers (the car model's check, the parts' build and selected texture, the interior with its dials and gauges, the cockpit, the lights, the traffic body, the wheels and their points), the palettes, the parts' vinyl textures, the paints (body and wheels, down to the first set's objects), the light kit's and lights' records, the gauges' table. Wheels::BindTextures and Wheels::ApplyPalette stay recompiled (they pass stack words they never write) | The scene model's flags at SBX's offsets (+5028 to +5104) |
| `native_car_makers` | `src/game/car_makers.cpp` (types in `src/game/car_makers.h`) | A garage car's reset, the garage cars' static set-up; the garage stage's car (SetCar, SetCarClass, FitCar, ShowCarById, ReloadModel, the record's copy and re-applies); SceneModelRecord::FromCar `sub_82159880` with its paint, wheels, interior choices, parts, decals and part order; CarModels::BlockOf; the tuning screen's car data (CarData::FromGarageCar); the stand's busy marker; the rival list screen's update `sub_8219F8A0` (its transitions stay recompiled) | The save's garage cars are 1,964 bytes (ITC 1,968); SetBytes' fields, SetPaint's wheel colors and FromCar's car offsets (4 bytes earlier from +1652), its five interior choices and its body kit table (0x82009318) are SBX's own; the rival list's globals are `0x826C7EE4` and `0x826C7EF0` (ITC `0x826D843C`, `0x826D8448`) |
| `native_texture_compression` | `src/game/texture_compression.cpp` | The blitter's constructor and the image codec's destructor. The encoders stay recompiled | none found |
| `native_save_interface` | `src/game/save_interface.cpp` | The save interface's calls into the save devices (set-up, queries, save, load and delete requests), the save devices' constructor, getters and requests, the slot state handlers and their waits, the devices' teardown and the slots' buffers, the signed-in users, the save layer (SaveSlots, GameData::Find, the save buffers, the replays' list, entries, count, sort, save, load, delete and new numbers), PlayTime::Update and Add, the devices' root name. SaveGame::Write `sub_8228CEE8` too, called from autosave's override (`src/hooks/native_calls.h`). The content and file calls and the save and load steps stay recompiled | SBX's slot at its own offsets; the replay buffer is 1,334,880 bytes (ITC 1,231,088) and the game data's 129,200 (ITC 129,400) |
| `native_std_list` | `src/game/std_list.cpp` | The C++ library's list of pointers: a node allocated, a list made, insert, erase. operator new and delete stay recompiled; the error paths (a count or size too big, a bad iterator) run the recompiled functions | none found |
| `native_std_string` | `src/game/std_string.cpp` | The C++ library's string: its storage (tidy, a new block, grow), assign, append, the constructors; String::Find and String::ReplaceAll. operator new and delete, memcpy_s, erase, replace and the wildcard search stay recompiled | none found |
| `native_c_runtime` | `src/game/c_runtime.cpp` | The array constructor and destructor loops, wcslen, wcscmp, the ASCII case-blind compare, rand. The heap, the formatters, the parsers, the math kernels and the copies stay recompiled | none found |
| `native_action_mappers` | `src/game/action_mappers.cpp` | 6 of the action mappers' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_ai_drivers` | `src/game/ai_drivers.cpp` | 11 of the ai drivers' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_app_start_up` | `src/game/app_start_up.cpp` | 7 of the app start up' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_app_states` | `src/game/app_states.cpp` | 8 of the app states' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_archive` | `src/game/archive.cpp` (types in `src/game/archive.h`, UCL in `src/game/ucl.cpp`) | The whole archive: Archive::Init (TOCs sized from and read from the `.TOC` files on the host; called from the patch loader's wrapper) and Shutdown, the TOC lookups, Archive::Load, the request queue (queue, cancel, cancel all, length, wait for idle), its list, Archive::FindLooseFile's walk (from the patch loader's wrapper), Ucl::Unpack, and the loader thread (queue order; the next `.DAT` read runs on a host thread into host buffers while the current request is unpacked and its callback runs; loose files through the game's read; UCL natively; decided once at boot). The shared load buffer's size is `--archive_shared_buffer_kb` (restart only). `--log_archive` logs each load | Written from SBX's code (not ported from ITC) |
| `native_boot_mode` | `src/game/boot_mode.cpp` | 1 of the boot mode' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_c_runtime` (more) | `src/game/c_runtime.cpp` | 9 more of its set-ups, setters and getters | none found |
| `native_camera_modes` | `src/game/camera_modes.cpp` | 2 of the camera modes' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_cameras` | `src/game/cameras.cpp` | 3 of the cameras' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_race_camera` | `src/game/race_camera.cpp` (types in `src/game/race_camera.h`) | The race camera: RaceCamera::SetMode, Update, Start, Restore and ResetRolls, the cockpit camera (its start and update, reading the per-car cockpit points), CameraPass::SetFromViewport, Viewport::SetRoll and SetNearClip. SetMode and Update are called from subtick's overrides (`src/hooks/native_calls.h`). The chase, hood, TV and scripted cameras' own updates, Viewport::Update and the matrix building stay recompiled | Written from SBX's code (not ported from ITC) |
| `native_car_models` | `src/game/car_models.cpp` (types in `src/game/car_models.h`) | The car models' loads: the load slots (reset, load, shared load, release), the deferred releases, the car scene's set-up (its pools' objects made), the car model's sets a and b, the node files' and bodies' arrival (the node matrices and points, the body textures, the plate's call, the interior texture names), decals, exterior load, decal reload, release and decal callback, the vinyl sheet's corner, Texture::Create and Free, the interior's, cockpit's (set c), wheels' and second-set wheels' loads, the TGA reader (CreateImage, the reader, ParseHeader, Decode, DecodeRle, ToARGB), the team emblem, the plate's image lookup and CopyRect. CarModel::MakePlate and the vinyl drawing and compression stay recompiled | SBX's asset ids are one lower (sets a 892/890, b 11062/11060, c 19927/19925; decals + 162, + 188, + 382; tires 581/583, calipers 886; emblems 167-187); the plate-image test reads the car model's +3720 (+3724 there); CopyRect's target pitch is r6's high word; CarScene::SetUp keeps r5 at +4996 (+5044 there); the plate's by-value struct is 44 bytes (the part order) |
| `native_car_physics` (more) | `src/game/car_physics.cpp` | 2 more of its set-ups, setters and getters | none found |
| `native_car_sounds` | `src/game/car_sounds.cpp` | 7 of the car sounds' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_career` | `src/game/career.cpp` | 5 of the career' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_collision` (more) | `src/game/collision.cpp` | 1 more of its set-ups, setters and getters | none found |
| `native_framework` (more) | `src/game/framework.cpp` | 3 more of its set-ups, setters and getters | none found |
| `native_front_end_car_draw` | `src/game/front_end_car_draw.cpp` | 2 of the front end car draw' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_game_modes` (more) | `src/game/game_modes.cpp` | 10 more of its set-ups, setters and getters | none found |
| `native_input` | `src/game/input.cpp` | 6 of the input' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_location_screen` | `src/game/location_screen.cpp` | 1 of the location screen' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_matrix_math` | `src/game/matrix_math.cpp` | 7 of the matrix math' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_mode_tick` | `src/game/mode_tick.cpp` | 2 of the mode tick' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_model_animation` | `src/game/model_animation.cpp` (types in `src/game/model_animation.h`) | 33 of the models' animation functions: the track sets' set-ups, frees, sampling and stepping, the skeleton's construction, frees, node values and channel animation (with CursorCurve::Sample), the cursors, the primitive drawer's constructor, Init (its shader compiles answered by SBX's stored shaders), SetShaders and Draw. Model::Animate's C++ (`hooks::NativeModelAnimation_Animate`) is there for SBX's wrapper in `src/subtick/per_frame_logic.cpp` to call. Curve::Sample and Key::Interpolate stay recompiled | Written from SBX's code (not ported from ITC) |
| `native_models` | `src/game/models.cpp` (types in `src/game/model.h` and `src/game/model_animation.h`) | 48 of the models' functions: Model::Load (its weights rounded as the game rounds them) and Mesh::Parse, the models' and part models' constructors, destructors and releases, the XPR lists (`std::vector` of XPR pointers: reserve, assign, insert, clear, at, Get), the materials' lookups, XPR::Release and FindTexture, the resources' and textures' relocation, the vertex declarations and shared shaders, Shaders::LoadAll and LoadPair, the skeleton's set-up, bind, pose reset, local matrices and node matrices, Model::RecordShape and Model::RecordMaterials (each frame, into the command list), and the memory blocks (AllocPhysical, the put-off frees and their list). Calls into `native_model_animation`'s functions are guest calls | Written from SBX's code (not ported from ITC). SBX's Mesh::Parse sets no fetch-word bits on the buffers; its XPR list is ITC's texture provider |
| `native_post_effects` | `src/game/post_effect_objects.cpp` | 4 of the post effects' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_race_event` | `src/game/race_event.cpp` | 1 of the race event' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_race_loading` | `src/game/race_loading.cpp` | 2 of the race loading' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_race_overlays` | `src/game/race_overlays.cpp` | The race overlays' objects: the sprites', smoke's, particles' and sparks' constructors, resets and destructors, their small setters, the overlay views' getters. The updates and recordings stay recompiled | none found |
| `native_race_setup` | `src/game/race_setup.cpp` | The race set-up's records: the racers' car records cleared (settings, decals, options), the racers' and the race data's entries reset, the stages and course record reset. The race set-up itself stays recompiled | none found |
| `native_race_world` | `src/game/race_world.cpp` | 4 of the race world' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_racer_controllers` | `src/game/racer_controllers.cpp` | 4 of the racer controllers' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_scene_target` | `src/game/scene_target.cpp` | 1 of the scene target' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_scenery_lists` | `src/game/scenery_lists.cpp` | The course set's model lists (ModelList::Load `sub_82146CD0`, LoadPoints, Release), which set list entries the race draws (IsBEntryOn, IsCEntryOn, PointList::IsEntryOn) and Race::CourseLayout `sub_8228C7F0`, SceneryModel::ResetDrawState, AnimationSet::CurrentFrame, XPRList::Set, the depth pass's declaration, PassDescription's setters, Recorder::RecordSceneLights `sub_8257AD90` with the projectors. The draws and the other recorders stay recompiled | Race::CourseLayout is one whole function (ITC splits it at its mode 8 test); the race data's mode is at +0xCA1F4 and play mode at +0xCA1F0 (ITC +0xA3AF4, +0xA3AF0); the scenery model's vtable is 0x8200673C |
| `native_sound_cues` | `src/game/sound_cues.cpp` | 12 of the sound cues' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_stand_models` | `src/game/stand_models.cpp` | The garage stand's car: GarageStage::MakeModel, the tuning screens' setters (parts, interior, paint, decals, stickers, lights, wheels and tires), GarageStage::SetWheel and FitAxles, TuningScreen::PreviewWheel, SetAeroSet, the second wheels' steps, the wheels' tire textures and grades (46 functions; `docs/functions.md` lists them). The re-applies are guest calls | SetWheel's 179-entry table (only each entry's model read) is a constant; SetDecalA/B and TuningScreen::SetTireGrade don't read past their tables for an index the game never passes; MakeModel's record has 0 where the original's reset left stack leftovers (none of them read) |
| `native_strings_and_font` (more) | `src/game/strings_and_font.cpp` | 2 more of its set-ups, setters and getters | none found |
| `native_track_collision` | `src/game/track_collision.cpp` | 3 of the track collision' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_track_zones` (more) | `src/game/track_zones.cpp` | 1 more of its set-ups, setters and getters | none found |
| `native_tuning_pages` | `src/game/tuning_pages.cpp` | 3 of the tuning pages' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_tuning_screen` | `src/game/tuning_screen.cpp` | 3 of the tuning screen' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_type67_modes` | `src/game/type67_modes.cpp` | 12 of the type67 modes' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_vehicle_accessors` | `src/game/vehicle_accessors.cpp` | 33 of the vehicle accessors' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_xact` | `src/game/xact.cpp` | 23 of the xact' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_xaudio` | `src/game/xaudio.cpp` | 19 of the xaudio' set-ups, resets, small setters, copies and getters (`docs/functions.md` lists them). The rest stays recompiled | none found |
| `native_options_load_save` | `src/game/options_load_save.cpp` | The options screen's load and save pages (9 and 10): their sub-pages (device check, question, note, the save's wait, close, cancel, the load's device answer), SetDialog and the Leave steps. The menu, the sounds, the UI movies and the save layer are called as they are | The options screen's words at +672 to +760 (ITC +624 to +712); LoadPage::Note's kind 11 shows note 10, and LoadPage::DeviceAnswer acknowledges the failure for 3 and 7 |

## Testing a subsystem

Set its flag in F4 (or `--native_<name>=true` or `=false`), restart (the
loads run at boot), and play what it touches. Subtick is on by default, so
the tests below run with it on:

- `native_menu_sounds`: menu sounds, the sound and music volume options.
- `native_data_files`: boot, a race (course and car data), the menus' text.
- `native_framework`: boot to a race; the debug text if shown.
- `native_car_specs`: the garage (car stats, tuning shop, tires, aero parts,
  wheels) and a race. A wrong swap or offset shows as wrong stats or a
  crash at boot.
- `native_menu_helpers`: up and down in the main, garage and pause menus
  (hold a direction to repeat), left and right on pages that step with them,
  accept and cancel.
- `native_race_hud`: a race's HUD movies: the countdown, the timer, the
  race-end overlays.
- `native_data_tables`: boot, then the shop's and the parts lists' names and
  prices, a rival battle's music. A wrong swap shows as wrong names or
  prices, or a crash at boot.
- `native_file_layer`: boot, a race, an autosave and a reload; the patch
  loader's files still load (its log).
- `native_track_zones`: a free run across several highway sections and a
  rival battle: the HUD's road names and signs.
- `native_race_scene`: start a race, pause and resume a few times, exit to
  the garage, start another: the cars' lighting, the glow, no hang on exit.
- `native_shader_objects`: boot (the font's and the menus' shaders), a race
  (the cars' and the course's meshes): a wrong shader shows as missing or
  garbled drawing, or a crash at a load.
- `native_screen_objects`: a race at speed (the screen effect's strength),
  then exit to the garage (its release).
- `native_scene_models`: the garage (a car's parts, interior, wheels and
  decals as tuned), the tuning shop's changes, a race (the cars' models).
- `native_save_data`: a new game (the defaults), the options and the
  controller settings, a save and a reload, the title screen after a
  return to it.
- `native_accessories`: a car with an interior accessory in the garage and
  a race (cockpit view).
- `native_race_camera`: a race from its intro: the start view (each of the
  options' three), the view button through chase, cockpit and hood, the
  cockpit's look stick and the chase camera's reset button, a pause and
  resume, a replay (with the TV cameras), a rival battle's intro, split
  screen. A wrong value shows as a camera in the wrong place, a wrong
  view after a switch, or a crash at the race start.
- `native_strings_and_font`: boot, the menus' text in each language, the
  pages of the help and the messages (paged text).
- `native_screen_constructors`: every front-end screen (type 6 and 7
  menus): the main menu, the garage, the shop, the tuning pages.
- `native_game_modes`: boot to a race and back (every mode change goes
  through the state machines), pause and resume, the race overlays.
- `native_collision`, `native_car_physics`, `native_vehicle`: a race on a
  few cars (the handling, the throttle, brake and handbrake, the surfaces:
  road, grass, walls), contact with the walls and other cars, a reset onto
  the road after a crash. A wrong value shows as a car that drives
  differently, goes through a wall or flips.
- `native_race_states`: a race from the start (rolling and standing), its
  countdown, laps, the finish and its fade; a quit from the pause menu.
  The HUD's positions and times.
- `native_material_passes`: the garage and a race: the cars' paint, their
  parts' transparency (windows, lights).
- `native_replay_snapshots`: a race, then its replay from the start to the
  end; restart a race (the state restored).
- `native_ui_movies`, `native_race_hud_object`, `native_race_screens`: the
  menus' and the race HUD's movies (the gauges, the gear, the boost, the
  timer, the warning), the pause and after-race menus and their answers,
  the results screens.
- `native_data_load`: boot (every object it makes), a race and its
  replay.
- `native_race_sounds`: a race from the countdown to the end and back to
  the garage, a rolling start, an abort from the pause menu, a rival
  battle's next stage: the music's start, stop and volume.
- `native_course_scenery`: a free run over the whole highway: the
  scenery loading in and out, the animated textures (signs, lights).
- `native_nearest_lights`: a night race past the lit stretches (tunnels,
  junctions): the car bodies lit by the lights they pass, as with the flag
  off.
- `native_scene_loaders`, `native_scene`, `native_second_set`,
  `native_load_poll`, `native_car_makers`: the garage (every car, its
  parts, interior, wheels, lights and paint as they load), the tuning
  shop's changes, a race with traffic (the traffic cars' models and
  lights), back to the garage.
- `native_texture_compression`: a car's custom decals or paint in the
  garage (the blitter and the codec).
- `native_save_interface`: the save and load screens (a device chosen, the
  saves listed, free space), an autosave, the play time shown.
- `native_std_list`, `native_std_string`, `native_c_runtime`: used all over
  (file names, menu text, the managers' listener lists, the traffic's and
  rivals' random picks): a boot to the garage, a race with traffic, the
  menus' text, a save and a load.
