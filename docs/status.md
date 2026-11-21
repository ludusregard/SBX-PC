# Status

The state of `main` (2026-11-18). Update this when a fix, feature or plan
changes.

## Working

Boots and runs on Windows and Linux with the native Vulkan renderer
(`build_vulkan.bat`, `run_vulkan.bat`, `docs/native-renderer-port.md`).
It's the only build since 2026-10-26: the emulated GPU build was removed (the
SDK's Xenos plugin is no longer built). The game's shaders are drawn with
our own hand-written GLSL (`--native_shaders`, on by default since
2026-11-18; `docs/shaders.md`). It plays the movies through FFmpeg
(`src/movies/`) and uses our own audio output (`src/audio/`) and XMA
decoding (`src/hooks/xma.cpp`). The race's 3D view can be anti-aliased with
CMAA2 (`--cmaa2`, `--cmaa2_threshold`) or FXAA (`--fxaa`), and blurred by
blending frames away from the screen's center (`--blur_frame_blend`, off by
default, with `--subtick` and a high `--max_fps`). Codegen is clean: 15,963
functions, no unresolved branches or calls. What came over from
ITC-Recomp, and what's next: `docs/itc-parity.md`.

Native game code (`docs/native-code.md`, rules in
`docs/native-code-rules.md`): the game's own subsystems rewritten as our C++
in `src/game/`, plain C++ on typed guest structs (the 2026-11-10/11
refactor), each behind a `--native_<name>` flag (F4, the Overrides tab,
grouped by subsystem), every flag on by default. 77 flags and 1,046
overrides in `src/` (of 15,963 recompiled functions); batches 1-8 built and
played. Most of what's native so far is set-ups, resets, copies, getters,
tables and a few hot helpers (`Lights::FindNearest`, `SceneModel::Update`,
`Vehicle::UpdateTrackPositions`); the per-tick logic (the vehicle step and
tires, the collision solve, the race's ticks, UI movie and font drawing),
the heap, file I/O and XACT still run recompiled. Every override in `src/`
has a row in `docs/functions.md`
(`sbx_pc/tools/check_function_statuses.py`: no errors).

## Fixes

- The black screen after quest races: `sub_8218C240`, the post-race
  check, waits 1 cycle instead of 60 (`src/hooks/post_race.cpp`,
  `--post_race_check_cycles`, 1 by default; 60 is the game's).
- The race-end music (the tutorial's `bgm_tmp_08`) cut off with
  `--subtick`: the forced exit waits for the screen fades, which stepped
  once a frame; they step once a tick now (`src/subtick/per_frame_logic.cpp`).
- A menu bitmap showing an old frame (one frame of the garage menu's
  animation showing the last race's view, or the garage's background):
  the native renderer kept sampling a resolve destination's image at an
  address after the game had freed it and put a texture there. A memcpy
  write over the start of a resolve destination not resolved into since
  drops it from the addresses draws sample
  (`TextureCache::State::MarkWritten`, `src/renderer/vulkan/textures.cpp`).

## Features

All tested and working (John, 2026-11-18).

- Language toggle, phase 1: `--language` (EN, JP, ...) picks the files in
  `assets/ADDON_DATA/LANGUAGES/` at startup, fonts from `FONTS/SDF/` (SD the
  fallback; `docs/game_data.md`, "SDF fonts"); language ids ignore `PATCH_DATA` (`src/hooks/patch_loader.cpp`,
  "Languages" in `docs/modding-plan.md`).
- Autosave: `--autosave` (off by default) saves through the game's own save
  calls when back at the garage top after a shop or car menu, or after
  racing; a "Saved" notice on the overlay. `--log_autosave` logs the menu
  messages and save status values (`src/hooks/autosave.cpp`, "Saving" in
  `docs/menus.md`). It watches the menus through the boot mode log's
  override of `sub_82106410` (`src/hooks/title_screen.cpp`).
- XeGTAO: `--xegtao` (off by default) runs Intel's XeGTAO ambient occlusion
  on the 3D view in races, the garage, the shops and the parking area,
  kept over frames and applied before the anti-aliasing and the
  post-processing; glass, mirrors, the sky and light the game adds (glows,
  underglow, headlights) are left out of it. `--xegtao_quality` (0-3, 2),
  `--xegtao_denoise` (0-3, 2), `--xegtao_radius` (meters, 0.75),
  `--xegtao_strength` (0-1, 1), `--xegtao_temporal` (0.05),
  `--xegtao_mirror` (off: the rear-view mirror's own AO), `--xegtao_debug`
  1-3 (AO, normals, edges). How it works: "XeGTAO" in
  `docs/native-renderer-port.md`. The standalone "G-buffer lite"
  (`--gbuffer` and its debug views) was removed 2026-11-18: the depth and
  camera motion it built are XeGTAO's inputs now (`BuildDepthMotion`), and
  the materials' normals and light targets are the G-buffer to build on.
- Pipeline warm-up: the pipelines earlier sessions drew with are made at
  start on a worker thread (`cache/pipeline_list.bin`,
  `--native_renderer_warm_pipelines`), against the first-draw stutter; see
  "Pipeline warm-up" in `docs/native-renderer-port.md`.
- Frame dumps: F4, Renderer tab, "Dump this frame" writes the next frame's
  draws, constants and textures (`frame.json`, DDS files) into
  `frame_dumps/` next to `sbx.exe`; `tools/frame_dump/describe.py` names
  them. See "Frame dumps" in `docs/native-renderer-port.md`.
- Saved settings: F4's Save button writes the settings changed in the menu to
  `sbx.toml` next to `sbx.exe`, which loads at every start; launch options
  still win over it (`src/base/settings_file.cpp`, "Settings" in
  `docs/build_run.md`).
- The cut cars (ECLI, S30, L86, T86) are offered from the start of a new
  game: their bits in the save's car unlock bits are set after
  `sub_82113FB0` and `sub_82114278` (`src/hooks/unlock_cut_cars.cpp`,
  `--unlock_cut_cars`, on by default). Their prices are still 999999.
- Patch loader: archive entries are read from loose files in
  `assets/PATCH_DATA/BUILD/TXR/BUILD/` and
  `assets/PATCH_DATA/BUILD/TXR/BUILD_CRS/` named by id with any extension.
  Example: `00000072.bin` and `00000072.dat` are both id `00000072` and
  would both be valid (`src/hooks/patch_loader.cpp`, `--patch_loader`, on
  by default; see `docs/game_data.md`, "Patch data").
- Sidecar textures: a pack served by the patch loader (or a language
  folder) can have full-size textures (BC7, 16-bit, upscales) in
  `<same name>.sbpr` beside it, made by sbxr_tools; the game loads the
  small stand-ins in the pack, and the native renderer draws the full-size
  textures in their place (`src/hooks/sidecar.cpp`,
  `src/renderer/sidecar_textures.cpp`, `--sidecar_textures`, on by
  default; `docs/game_data.md`, "Sidecar textures (SBPR)").
- Custom music: tracks from `assets/ADDON_DATA/CUSTOM_MUSIC/<CATEGORY>/`
  (ogg, opus, flac, mp3, wav) play in the place of the game's music by a
  roll at each music start (`--new_music_roll`, 0 by default, or a folder's
  `metadata.xml`), with weights and loop regions. Each folder's
  `metadata.xml` also weighs the game's own songs: weight 0 blocks one, and
  another song of the folder plays in its place. `--custom_music` (on by
  default), `--custom_music_log`, `--custom_music_shuffle` (on by default),
  `--custom_music_adaptive_volume` (on by default, loudness to -11.2 LUFS;
  restart to change), and `Volume` in `metadata.xml` (track levels)
  (`src/hooks/custom_music.cpp`, `src/audio/music/`,
  `docs/custom_music.md`). The category folders and a user guide
  (`CUSTOM_MUSIC_README.md`) are in `assets/ADDON_DATA/CUSTOM_MUSIC/`.
  The categories (MENU_COURSE, MENU_MAIN, MENU_GENERAL, MENU_STORY,
  ROAMING, BATTLE, BOSS, RACE_WIN, RACE_LOSE) are from the code ("Music" in
  `docs/audio.md`).
- Cut car wheels: the ECLI gets its own stock wheel instead of AD00, and
  the L86, S30 and T86 the BOSS00's wheel (`--cut_car_wheels`), or the
  LORRY's steel wheels (`--cut_car_steelies`); both on by default
  (`src/hooks/cut_car_wheels.cpp`). The wheel shop also offers them
  inch-ups (ECLI 3, the others 4).
- Subtick: frames between the game's 30 Hz ticks (`--subtick`, on by
  default, `--max_fps`; `src/subtick/`, "Subtick" in `docs/functions.md`).
  - Races: the cars' controls fed input sampled each frame and weighted per
    tick (`--subtick_input`, `--subtick_input_sensitivity`), written into
    the control record so replays play back 1:1. Car physics stays at
    30 Hz and two steps a tick; steering stays a once-a-tick step (John's
    call, 2026-11-18). Cars, the camera, paint reflections, light glows,
    light trails and the opponent marker are blended between ticks. The
    race overlays (opponent marker, tire smoke) draw on every frame, paused
    or not, and in the rear-view mirror with the mirror's blended camera.
    A race's frames between ticks draw only the UI movies the tick drew
    (no HUD flash after a result). Turning `--subtick` off mid-race works.
  - Menus: no message delivered on frames between ticks; the UI movies,
    the garage's car and camera, the parking area's 3D and the scrolling
    text drawn between ticks and blended; 30 Hz for the tick a closing
    menu waits for its "closed" message.
  - The UI movies' animations and the course's scrolling textures step
    once a tick and move smoothly between.
  - Camera cuts: a camera breaking from its motion cuts instead of
    blending (good for the demo's cuts); the garage's and the view
    button's switches are still blended (tabled, 2026-11-11).
- Tick rate experiments (`--tick_rate_60`, `--tick_rate_half_step`): removed
  2026-10-25; their findings are in "Tick rate experiments" in
  `docs/functions.md`.

## Mod tools

The data tools (the Genki .DAT Explorer, the car specs workbook, the cut
car wheel baselines, the Blender add-on) are in their own repo, sbxr_tools
(see its README).

## Docs

- `docs/functions.md`: named functions, mostly matched from ITC-Recomp's
  findings (`docs/porting-from-itc.md`), plus the data loading code, and a
  row for every override in `src/`.
- `docs/native-code.md` and `docs/native-code-rules.md`: the native game
  code, by flag, how to test it, and the rules for writing it.
- `docs/native-renderer-port.md`: the native renderer.
- `docs/shaders.md` and `docs/part_shaders.md`: the game's shaders as GLSL,
  and the part files' shaders.
- `docs/menus.md`: the front end's screens and menu messages.
- `docs/audio.md` and `docs/custom_music.md`: sound, and custom music.
- `docs/itc-parity.md`: what came over from ITC-Recomp, and what hasn't.
- `docs/game_data.md`: the archives and data formats.
- `docs/modding-plan.md`: the patch and addon loaders, adding cars.
- `docs/cut_car_wheels.md`: the cut cars' wheel baselines.
- `docs/build_run.md` and `docs/codegen.md`: building, running, codegen.

## Not done yet

- Native game code (`docs/native-code.md`): the logic each subsystem left
  recompiled (the physics' step and solver, collision, the race's ticks,
  cameras, UI movie and font drawing), the held folders (heap, command
  list, sprites, the kernel calls), the next refactor step (real
  signatures, direct native-to-native calls instead of `Guest::Call`), and
  the xex's data (tables, strings, vtables) as C++ data, toward running
  without the xex.
- The rest of ITC-Recomp's parity plan (`docs/itc-parity.md`): XACT
  natively, the SDK's kernel calls.
- Native drawing: a scene description from native code instead of
  replaying the game's Xbox graphics command lists (`docs/shaders.md`).
- Camera cuts on the game's own switches (2026-11-18, not yet tested):
  the race camera's mode changes (the view button), the chase camera's
  reset, the TV cameras' and the scripted camera's changes
  (`src/subtick/camera_cuts.cpp`, `--subtick_camera_cuts`), with the race
  camera as our own C++ (`--native_race_camera`). The garage's presets
  aren't reported yet (the motion test was dropped 2026-11-18: it made the
  car jitter for a tick when unpausing at speed and didn't cut the garage). On a cut the effects that keep earlier
  frames start again (`--reset_effects_on_camera_cut`, not yet tested):
  the bloom's history, the motion blur's previous camera
  (`src/hooks/post_effects.cpp`), XeGTAO's history, the camera motion and
  `--blur_frame_blend` (the native renderer, by the list camera's cut
  count).
- Wheel spin steps once a tick.
- `--camera_fov` (the main camera's vertical FOV, Graphics/Experimental):
  safe range 30 to 60 degrees (the game is about 47). It changes every use
  of camera pass 0, so the replay and wanderer/boss cinematic camera tracks
  are affected too. Limiting it to racing is planned (`docs/functions.md`,
  "Main camera"); the `--log_record_matrix` diagnostic is still in the code
  (off).
- Shader table: `--dump_shader_compiles` (the dump that wrote the table
  `graphics/compiled_shaders.h` is built from) was removed. The table stays;
  a shader it doesn't hold still goes to the real compiler and is logged.
  To remake the table, restore the dump branch from git history
  (`src/hooks/shader_compiles.cpp`, before its removal) first.
- A CI build.
