# Status

The state of `main` (2026-11-09). Update this when a fix, experiment or
plan changes.

## Working

Boots and runs on Windows with the native Vulkan renderer ported from
ITC-Recomp (`build_vulkan.bat`, `run_vulkan.bat`, `docs/native-renderer-port.md`).
It's the only build since 2026-10-26: the emulated GPU build was removed (the
SDK's Xenos plugin is no longer built). It also plays the movies
through FFmpeg (`src/movies/`). It uses our own audio output (`src/audio/`, and its own XMA decoding, `src/hooks/xma.cpp`; neither has a flag since 2026-11-09). The Vulkan build can
also anti-alias the race's 3D view with CMAA2 (`--cmaa2`,
`--cmaa2_threshold`) or FXAA (`--fxaa`), both working (the user's test,
2026-10-25), and blur it by blending frames away from the screen's centre
(`--blur_frame_blend`, off by default, with `--subtick` and a high `--max_fps`; not yet tested). Codegen is clean: 15,963
functions, no unresolved branches or calls. What else came over from
ITC-Recomp, and what's next: `docs/itc-parity.md`.

Native game code (`docs/native-code.md`): the game's own subsystems
rewritten as our C++, each behind a `--native_<name>` flag (F4, the Overrides
tab, grouped by subsystem), tested by playing with the flag on and off; every flag is on
by default. Since 2026-11-09 the generated ones (set-ups, resets, setters,
copies, getters: 46 more files, 301 functions across 36 new flags and 10
earlier ones, not yet played) are checked against the recompiled functions
by `tools/native_gen/difftest` before they go in; 1,038 overrides in all.
In and tested (2026-11-09): `native_menu_sounds`,
`native_data_files`, `native_framework`, `native_car_specs`,
`native_menu_helpers`, `native_race_hud`, `native_data_tables`,
`native_file_layer`, `native_track_zones` and `native_race_scene`. Written
2026-11-09, to test: batch 3 (`native_shader_objects`,
`native_screen_objects`, `native_scene_models`, `native_save_data`,
`native_accessories`, `native_strings_and_font`,
`native_screen_constructors`, `native_game_modes`) and batch 4
(`native_collision`, `native_car_physics`, `native_vehicle`,
`native_race_states`, `native_material_passes`,
`native_replay_snapshots`) and batch 5 (`native_ui_movies`,
`native_data_load`, `native_race_sounds`, `native_race_screens`,
`native_race_hud_object`, `native_course_scenery`) and batch 6
(`native_scene_loaders`, `native_scene`, `native_second_set`,
`native_load_poll`, `native_car_makers`, `native_texture_compression`,
`native_save_interface`). Every override in
`src/` has a row in `docs/functions.md`
(`sbx_pc/tools/check_function_statuses.py`: no errors).

## Fixes

- The black screen after quest races: `sub_8218C240`, the post-race
  check, waits 1 cycle instead of 60 (`src/hooks/post_race.cpp`,
  `--post_race_check_cycles`, 1 by default; 60 is the game's).
- The race-end music (the tutorial's `bgm_tmp_08`) cut off with
  `--subtick`: the forced exit waits for the screen fades, which stepped
  once a frame; they step once a tick now (`src/subtick/per_frame_logic.cpp`).
- A menu bitmap showing an old frame (one frame of the garage menu's
  animation showing the last race's view, or the garage's background),
  now and then: the native renderer kept sampling a resolve destination's
  image at an address after the game had freed it and put a texture
  there. A memcpy write over the start of a resolve destination not
  resolved into since drops it from the addresses draws sample
  (`TextureCache::State::MarkWritten`, `src/renderer/vulkan/textures.cpp`;
  2026-11-09, not yet confirmed in play: the bug is rare). A button
  bitmap sometimes drawn as a faint quad in the sub-menus may be the same
  thing (its memory where a glow or blur target was); to watch.

## Experiments

- Built 2026-11-06, in the Windows builds since 2026-11-08; autosave and
  the G-buffer confirmed in play (the user, 2026-11-09); the language toggle
  works as far as it can without its files (to test once the patch's
  language files are in `assets/ADDON_DATA/LANGUAGES/`):
  - Language toggle, phase 1: `--language` (EN, JP, ...) and
    `--font_quality` (hd, sd) pick the files in `assets/ADDON_DATA/LANGUAGES/`
    at startup; language ids ignore `PATCH_DATA` (`src/hooks/patch_loader.cpp`,
    "Languages" in `docs/modding-plan.md`). Nothing changes until that folder
    holds files.
  - Autosave: `--autosave` (off by default) saves through the game's own save
    calls when back at the garage top after a shop or car menu, or after
    racing; a "Saved" notice on the overlay. `--log_autosave` logs the menu
    messages and save status values to confirm the ids
    (`src/hooks/autosave.cpp`, "Saving" in `docs/menus.md`). It watches the
    menus through the boot mode log's override of `sub_82106410`
    (`src/hooks/title_screen.cpp`): the first version overrode it too.
  - G-buffer lite: `--gbuffer` (off by default) builds the 3D view's depth
    (as drawn, and view distance) and the camera's motion vectors into our own
    buffers at its second color resolve (`--gbuffer_resolve`, 2: the whole
    scene; the cars, and the car body around the cockpit, are drawn after the
    first, which has only the scenery; John's test, 2026-11-16), from camera
    pass 0's view-projection handed over with each command list.
    `--gbuffer_debug` 1-4 draws one over the frame (distance, motion, the
    game's own velocity buffer, raw depth). Nothing else uses the buffers yet
    (`BuildGBuffer` in `src/renderer/vulkan/passes.cpp`, `glsl/gbuffer*.frag`).
  - XeGTAO (2026-11-17): `--xegtao` (off by default) runs Intel's XeGTAO
    ambient occlusion on the 3D view in races, right after the G-buffer is
    built (it's built for it even without `--gbuffer`): the depth's MIPs,
    normals from the depth, the AO term and its denoise, then the AO
    multiplied into every sample of the main view, before the
    anti-aliasing and the post-processing. Its depth is unpacked with camera
    pass 0's projection (+0x50), now handed over with the view-projection.
    `--xegtao_quality` (0-3, 2), `--xegtao_denoise` (0-3, 2),
    `--xegtao_radius` (meters, 0.75 since 2026-11-17), `--xegtao_strength` (0-1, 1);
    `--xegtao_debug` 1-3 draws the AO, the normals or the edges over the
    frame (`ApplyXeGTAO` in `src/renderer/vulkan/passes.cpp`,
    `glsl/xegtao/`, `glsl/compile_xegtao.py`). John's test (2026-11-17):
    every setting works; the denoise left a fixed, screen-locked pattern of
    larger blotches; glass, underglow and smoke were darkened and glass hid
    what's behind it from the AO; no AO in the garage or the PA (no camera
    there yet); car normals look faceted (they come from depth, so they're
    the polygons'). Since then, not tested yet:
    - The AO is kept over frames (`--xegtao_temporal`, 0.1: each frame's
      share), reprojected with the G-buffer's camera motion, dropped where
      the view distance changed by over 15% and clamped to the 3x3
      neighborhood; XeGTAO's noise now moves each frame so it averages out
      (`glsl/xegtao/xegtao_temporal.comp`).
    - No reordering any more (the earlier tries are in the comments of
      `passes.cpp`, `XeGtao`): the AO runs after the whole 3D scene, at the
      race view's resolve `gbuffer_resolve` or the menu view's first tiled
      resolve. Instead, with `--native_shaders`, every hand-written shader
      writes its color to a light target of ours as well (`xe_color_output`
      in `glsl/game/xe_interface.glsl`, location 2, RGBA16F, blended as the
      color): the color itself for draws that only add light (the models'
      unlit material, `unlit.frag`: glow planes, emissive bitmaps; additive blends: flares, underglow; spec constant `XE_LIGHT`,
      900), else black with the color's alpha. The AO is applied as color x
      ao + light x (1 - ao), so that light isn't darkened
      (`--xegtao_light`, on).
    - `--xegtao_alpha_depth` (on): the models' lit blended draws without
      depth writes (smooth-alpha solids: the type B models' DXT5 wheels) get
      a depth-only copy, with their normals, where alpha >= 0.5; not course
      draws (the PA's decal strip), not depth-biased or equal-tested ones.
      Families from spec constant `SBX_FAMILY` (901) in the materials.
    - All 135 game shaders recompiled (`generated_game_shaders.h`) for the
      light output.
    - The garage, shops and parking area: their camera is the camera pass
      their 3D scene updates (the garage stage's step, the parking area's
      scene; `CarTransforms_MenuSceneCamera`, pass 0's if it's one of them;
      the passes used are logged as `menu scene camera passes:`). The menu
      view (pitch 1280, 4x MSAA, drawn in tiles) has its own G-buffer and
      AO, run before its first blended draw or at its first tiled resolve.
    - Smooth normals (`--xegtao_normals`, on): with `--native_shaders`, the
      hand-written materials with a normal (57 shaders: `lit.frag`,
      `course_projected.frag`, `course.frag` with `NORMAL_REG`) write their
      interpolated world-space normal to location 1
      (`glsl/materials/normals_out.glsl`). The 3D views get a second color
      attachment of ours for it (A2B10G10R10, the view's samples, cleared
      each frame); only opaque, depth-writing draws of those shaders write
      it. XeGTAO takes it over its normals from depth where written
      (`glsl/xegtao/xegtao_normals.comp`, through the camera's view matrix).
      The pipeline key gained the attachment (`pipeline_list.bin` starts
      over once).
    - John's test (2026-11-17): the neon underglow (texture file 143,
      `neon_*`) was still darkened; the dark meshes behind the front bumper
      were in neither the normals nor the AO's depth; the PA's parking-line
      strip still made AO. Since then, not tested yet:
      - The underglow is `course_projected.frag`'s projector 1 (`prjtx1`,
        added to the road's color, faded over 3 units of height), so it was
        course color. A shader can now mark light it adds
        (`xe_light_extra` in `xe_interface.glsl`); `xe_color_output` writes
        that share of the output to the light target. The projector's light
        is marked (so the headlights' too).
      - `--xegtao_alpha_depth` covers the models' unlit material as well
        (the grille meshes), not additive blends (glow planes, flares).
        `unlit.frag` writes "use the depth's normal" (alpha 0) to the normals
        target, so an unlit solid no longer leaves the normal of what's
        behind it there.
      - `--xegtao_plane_bias` (meters, 0.05; 0 off): a sample no higher than
        that over the center's tangent plane, by its normal (the smooth one),
        doesn't occlude, fading in up to twice that; so a decal's few
        centimeters make no AO. A patch `compile_xegtao.py` applies to
        `XeGTAO.hlsli` as it compiles it (the file stays Intel's), with the
        bias in `GTAOConstants.Padding0`.
    - John's test (2026-11-17): the plane bias works. With
      `--xegtao_normals` off, the grille meshes and the wheels' alpha
      cutouts were right in the AO (the glass solid): they write depth, but
      not our normals, so the normals target kept the normal of what's
      behind them. Since then, not tested yet: the normals target is
      R32G32_UINT, the packed normal and the writing fragment's view
      distance (`normals_out.glsl`); every depth-writing draw of a shader
      with a normal writes it, blended or not; the merge
      (`xegtao_normals.comp`) takes a normal only where that distance is the
      depth's (0.2% plus the depth's slope over a pixel), else the normal
      from depth. The glass stays solid in the AO, as it is in the depth.
    - John's test (2026-11-17): almost right; the glass still in the AO
      (the cockpit view's AO is the windshield's), and the wheels' cutout a
      hard 1-bit edge. Since then, not tested yet:
      - The AO has its own depth (`glsl/xegtao/xegtao_depth.comp`): the
        normals target's view distances, which every hand-written shader
        writes in its depth-writing (and opaque equal-tested) draws
        (`xe_out_normal` in `xe_interface.glsl`), over the G-buffer's depth
        where nothing wrote one. The glass doesn't write it: the shaders
        with a third texture coordinate (`UV2_REG`, which only the windows
        and light covers have, stream type 0x10) are `SBX_FAMILY` 4. So the
        AO looks through the glass and is applied to it. A surface drawn by
        a translated shader or without a pixel shader isn't in the AO's
        depth now.
      - `--xegtao_alpha_depth` (on): the models' lit, blended, not additive
        draws (the wheels) write the normals target only in a copy drawn
        first, color and depth off, whose alpha test is a dither moved each
        frame (bit 31 of the system flags, `kXeFlagSbxDitheredAlpha`); the
        history averages it into a smooth edge. The copy no longer writes
        the game's depth, and the draw itself is as the game made it.
    - John's test (2026-11-17): the glass and wheels work (dithered, fine
      in the final image), but in the cockpit view an "untextured cover
      glow" plane over the windshield (additive, "how solid" alpha) was
      solid in the AO. Since then, not tested yet:
      - Additive draws never write the normals target (they're light).
      - `--xegtao_mirror` (off): the rear-view mirror in races gets its own
        G-buffer and AO. Its target is pitch 520, the guest's 4x MSAA,
        8_8_8_8, resolved at 512x128 (from the target and resolve log
        lines); the AO runs at that resolve. Its camera: the first camera
        pass after 0 with a perspective projection of aspect 4
        (`CaptureListCamera` in `subtick/renderer_hooks.cpp`; the passes
        with a perspective projection are logged as `race camera passes
        with a perspective projection:`, the chosen one marked `<- mirror`).
        Not yet confirmed which pass that is.
      - The flags `xegtao_normals`, `xegtao_light`, `xegtao_alpha_depth`
        and `xegtao_plane_bias` are gone (always on; the bias 0.05 m).
        `--xegtao_temporal` now defaults to 0.05, `--xegtao_radius` to 0.75.
    - John's test (2026-11-17): the mirror did nothing and the cockpit's
      cover glow was still solid. The log showed the mirror pick was wrong:
      passes 1 and 2 have a perspective of aspect 3.56 (16:9) and pass 3
      the mirror's 4.00; the range (3.5-4.5) took pass 1. Now within 0.05
      of 4. The mirror's G-buffer was made but never built (the reason
      wasn't logged; the view-projection inverse and the descriptor set
      failures now are). The cover glow isn't additive: the models' unlit
      blended draws now take the dithered copy as the lit ones do, so it's
      in the AO's depth only as often as its alpha.
    - John's test (2026-11-17): the mirror works; the cockpit's solid
      windshield plane is still there. A frame dump of the cockpit view
      (2026-11-18; its draw-ID image) shows the windshield is draw 2599/2600:
      `ps_4F4860F071E25904` (`textured_color.frag`, BUILD 22811, the shared
      pair 1005's, which no part file uses), alpha-blended, depth-writing,
      over a lit layer with the same texture (2598, `ps_BDEC67D7B4DF91A1`,
      black, alpha 0.64, dithered as a smooth-alpha solid). It isn't a
      model shader, so it wrote the AO's depth whole. It's now glass
      (`SBX_GLASS` in `post/variants.txt`, `SBX_FAMILY` 4). (A rule tried
      before it, ps_08 when blended, never applied: ps_08's records with
      +0x004 = 1, such as the Z33 cabin's, are drawn opaque. Removed.)
    - The parking area (frame dump, 2026-11-18): its scenery is drawn with
      the course shaders: a depth-only pass first, then `92355BEBF0E0C743`
      (`course.frag` without a normal, 6 draws including the largest, 17,201
      vertices), `959EB72BB94E0226` (no normal) and `A38DF64E59CB943C`
      (`course_projected.frag`, with one). Surfaces drawn without a normal
      get the normal from depth: faceted on round shapes such as the pipes.
      The normal-mapped draws read a third stream from the same vertex
      buffer, so the area's normals look to be there for the others too.
      In the dump the area's three streams sit back to back: fetch 95
      (229,488 bytes, 16 a vertex), 94 (172,116, positions, 12 a vertex,
      14,343 vertices) and 93 (229,488, 16 a vertex: a normal and a second
      vector, as stream type 0x0D), and only the normal-mapped draws bind 93.
    - John's test (2026-11-18): the glass is solved. Since then, not tested
      yet: the mirrors' surfaces (the car's side mirrors, the interior
      mirror) are glass to the AO: any draw sampling the rear-view mirror's
      resolved view (recorded at the mirror target's resolve,
      `mirror_texture`). In the cockpit dump those are two draws of
      `ps_72DE42674BE25160`. John's test: that made them holes; they should
      be solid but take no AO. Now they stay in the AO's depth and normals,
      and their view distance in the normals target carries its sign bit
      (bit 30 of the system flags, `kXeFlagSbxNoAo`, set by Draw;
      `xe_normal_distance`); `xegtao_depth.comp` writes a no-AO mask (R8)
      from it and the history pass sets the visibility to 1 there.
    - For the parking area's normals: frame dumps now list
      `unread_vertex_fetches`, fetch constants 88-95 that hold a vertex
      buffer the draw's vertex shader doesn't read, to check that the
      course draws without a normal still have the normal stream (93) set.
    - John's test (2026-11-18): the side mirrors right; the screen's
      rear-view box and the sky still took AO (the sky: a gray gradient, AO
      worked out on the cleared depth). Since then, not tested yet: no AO
      also for opaque draws without the depth test (the sky, 4 draws of
      `92355BEBF0E0C743` with vertex shader `E332602F43374F92`, first in
      the 3D view, in races and the parking area) and every draw sampling
      the mirror's view (the box: `ps_9567C79307ACC6F5`, draw 2601). A
      no-AO draw now writes only the sign bit as its distance, so the AO's
      depth there is the game's (the box writes no depth: the world behind
      it stays in the AO's depth).
    - The parking-area dump (2026-11-18, `unread_vertex_fetches`): every
      course draw without a normal still has fetch 93 bound to the area's
      normal stream (`198190C4`, 229,488 bytes, right after the positions
      at `197EF070`), the one the normal-mapped draws read. The sky's draws
      have it too, left over from the area (their own streams are small),
      so a shader reading it has to check it belongs to the draw.
      Since then, not tested yet: the two vertex shaders those draws use
      (`vs_E76C7F598230D74B`, 00001247, and `vs_86F28F82FA50FE98`, 00001249;
      the latter also draws the race courses' vertex-colored surfaces) pass
      the mesh's own normal in interpolator 15 (`STREAM_NORMAL_REG`,
      `vertex/model.vert`): read from fetch 93 as 16_16_16_16_FLOAT, 4 words
      a vertex, only when that stream starts right after the position
      stream with as many vertices (else 0). The 9 `course.frag` variants
      without a normal write it to the normals target (`STREAM_NORMAL_REG`),
      for the AO only; the shading is unchanged. Those shaders have no
      `matWorldVS`, so the normal is taken as already in world space (the
      course's meshes are static); to check in the normals view against
      the normal-mapped surfaces next to them.
    - John's test (2026-11-18): no visible change in the parking area; and
      AO still around the parking markings. From the data (BUILD_CRS 1244,
      the Yoyogi parking area's model, `mdl_Detail`, 25,461 vertices, 79
      materials; a part file in a 0x20-byte container): the normal stream
      is there and read, but most of the materials drawn without a normal
      are authored flat (every triangle's vertex normals equal its face
      normal: e.g. 1,577 of 1,581 triangles of the 4,175-index one), so
      their mesh normals are the same as the ones from depth. Only a few
      (the 1,891-index one, 22 and 33) are smoothed. Not a bug in the
      reading; the meshes aren't smoothed. The markings (`mj_kogata_y`,
      `mj_kurumaisu`, `mj_oogata_y`, `mj_rinsya_y`, materials 55-58) sit 4
      to 8 cm over the ground, so the plane bias (0.05, full at 0.1) left
      the higher ones in: now 0.1 (no occlusion to 10 cm, full at 20 cm).
      The tile walkway (`PA_yoyogi_TileFloor_a`, ~22 cm over what's under
      it) is real geometry and keeps its AO.
  - Pipeline warm-up (2026-11-16): the pipelines earlier sessions drew with
    are made at start on a worker thread (`cache/pipeline_list.bin`,
    `--native_renderer_warm_pipelines`), against the first-draw stutter; see
    "Pipeline warm-up" in `docs/native-renderer-port.md`. Not tested yet.
  - Frame dumps (2026-11-16): F4, Renderer tab, "Dump this frame" writes the
    next frame's draws, constants and textures (`frame.json`, DDS files)
    into `frame_dumps/` next to `sbx.exe`; `tools/frame_dump/describe.py`
    names them. See "Frame dumps" in `docs/native-renderer-port.md`. Not
    tested yet.
- Saved settings: F4's Save button writes the settings changed in the menu to
  `sbx.toml` next to `sbx.exe`, which loads at every start; launch options
  still win over it (`src/base/settings_file.cpp`, "Settings" in
  `docs/build_run.md`). Not yet tested.
- The cut cars (ECLI, S30, L86, T86) are offered from the start of a new
  game: their bits in the save's car unlock bits are set after
  `sub_82113FB0` and `sub_82114278` (`src/hooks/unlock_cut_cars.cpp`,
  `--unlock_cut_cars`, on by default). Their prices are still 999999.
- Patch loader: archive entries are read from loose files in `assets/PATCH_DATA/BUILD/TXR/BUILD/`
  and `assets/PATCH_DATA/BUILD/TXR/BUILD_CRS/` named by id with any extension.
  Example: `00000072.bin` and `00000072.dat` are both id `00000072` and would both be valid. 
  (`src/hooks/patch_loader.cpp`, `--patch_loader`, on by default; see
  `docs/game_data.md`, "Patch data"). Not yet tested.
- Sidecar textures: a pack served by the patch loader (or a language
  folder) can have full-size textures (BC7, 16-bit, upscales) in
  `<same name>.sbpr` beside it, made by sbxr_tools; the game loads the
  small stand-ins in the pack, and the native renderer draws the full-size
  textures in their place (`src/hooks/sidecar.cpp`,
  `src/renderer/sidecar_textures.cpp`, `--sidecar_textures`, on by
  default; `docs/game_data.md`, "Sidecar textures (SBPR)"). Not yet tested
  in game.
- Custom music: tracks from `assets/ADDON_DATA/CUSTOM_MUSIC/<CATEGORY>/`
  (ogg, opus, flac, mp3, wav) play in the place of the game's music by a
  roll at each music start (`--new_music_roll`, 0 by default, or a folder's
  `metadata.xml`), with weights and loop regions. Each folder's
  `metadata.xml` also weighs the game's own songs: weight 0 blocks one, and
  another song of the folder plays in its place. `--custom_music` (on by
  default), `--custom_music_log`, `--custom_music_shuffle` (on by default),
  `--custom_music_adaptive_volume` (on by default, loudness to -11.2 LUFS; restart to change),
  and `Volume` in `metadata.xml` (track levels) (`src/hooks/custom_music.cpp`, `src/audio/music/`,
  `docs/custom_music.md`). FFmpeg now also has the
  ogg, flac, mp3 and wav demuxers, their decoders and libswresample.
  Built on Windows and working in the game (2026-10-26): the menu songs,
  roaming, looping and shuffle. The category folders and a user guide
  (`CUSTOM_MUSIC_README.md`) are in `assets/ADDON_DATA/CUSTOM_MUSIC/`.
  The categories (MENU_COURSE, MENU_MAIN, MENU_GENERAL, MENU_STORY, ROAMING, BATTLE, BOSS, RACE_WIN, RACE_LOSE) are from the code
  ("Music" in `docs/audio.md`); BATTLE, BOSS, RACE_WIN and RACE_LOSE are
  still to confirm in play (each music start is logged with its caller).
- Cut car wheels: the ECLI gets its own stock wheel instead of AD00, and
  the L86, S30 and T86 the BOSS00's wheel (`--cut_car_wheels`), or the
  LORRY's steel wheels (`--cut_car_steelies`); both on by default
  (`src/hooks/cut_car_wheels.cpp`). Working in the garage. The wheel shop
  also offers them inch-ups (ECLI 3, the others 4); not yet tested.

- Subtick: frames between the game's 30 Hz ticks in
  races (`--subtick`, `--max_fps`), the cars' controls fed input sampled
  each frame and weighted per tick (`--subtick_input`,
  `--subtick_input_sensitivity`), written into the control record so
  replays play back 1:1. Car physics stays at 30 Hz and two steps a tick.
  Cars, the camera, paint reflections, light glows, light trails and the
  opponent marker are blended between ticks. The race overlays (opponent
  marker, tire smoke) draw on every frame, paused or not, in the rear-view
  mirror with the mirror's blended camera (fix for their judder there,
  2026-11-10; not yet confirmed in game). Open: wheel spin
  and steering step per tick; check the light sources on car models for the
  same camera-position shake the paint reflections had. On by default (`src/subtick/`; "Subtick" in
  `docs/functions.md`). The menus too (frames between ticks: no message
  delivered on them, the UI movies, the garage's car and camera, the
  parking area's 3D and the scrolling text drawn between ticks and blended;
  30 Hz for the tick a closing menu waits for its "closed" message; works,
  John's tests 2026-11-11; the `--subtick_menus` flag is gone). The UI
  movies' animations are drawn between ticks (works). The course's
  scrolling textures step once a tick and move smoothly between (works).
  Turning `--subtick` off mid-race works (it froze the game before).
  Camera cuts: tabled (2026-11-11). A camera breaking from its motion
  cuts instead of blending: about as good as the old 8 m test for the
  demo's cuts, but the garage's and the view button's switches are still
  blended between ticks (John's test). The HUD drawn for a frame after a
  race's result took it down: a race's frames between ticks now draw only
  the UI movies the tick drew; works (John's test, 2026-11-11).
- Tick rate experiments (`--tick_rate_60`, `--tick_rate_half_step`): removed
  2026-10-25; `cull_emulated_build.ps1` deletes `src/tick_rate/`. Their findings are in
  "Tick rate experiments" in `docs/functions.md`.

## Mod tools

- `sbxr_tools/car_specs.py`: the car specs to an Excel workbook and back,
  byte for byte.
- The Blender add-on (`sbxr_tools/blender_plugin/`, 1.0.0): imports cars
  and parts, exports edited parts for `assets/PATCH_DATA/`
  (`sbxr_tools/blender_plugin/README.md`).
- `sbxr_tools/cut_car_wheels.py`: the cut cars' wheel baselines
  (`docs/cut_car_wheels.md`). Not yet tested in game.

## Docs

- `docs/functions.md`: named functions, mostly matched from ITC-Recomp's
  findings (`docs/porting-from-itc.md`), plus the data loading code, and a
  row for every override in `src/`.
- `docs/native-code.md`: the native game code, by flag, and how to test it.
- `docs/menus.md`: the front end's screens and menu messages.
- `docs/audio.md` and `docs/custom_music.md`: sound, and custom music.
- `docs/itc-parity.md`: what came over from ITC-Recomp, and what hasn't.
- `docs/native-renderer-port.md`: the native renderer's port.
- `docs/game_data.md`: the archives and data formats.
- `docs/modding-plan.md`: the patch and addon loaders, adding cars.
- `docs/cut_car_wheels.md`: the cut cars' wheel baselines.

## Not done yet

- The rest of ITC-Recomp's parity plan (`docs/itc-parity.md`): XACT
  natively, the SDK's kernel calls. (Our own XMA decoding is in.)
- Native game code (`docs/native-code.md`): test batches 3 to 6 in play
  (each flag on and off); then the merge step (the addresses SBX's own
  overrides already hold, SBX's version the base), the held folders (the
  heap's, the renderer's, the kernel calls), and the larger functions each
  subsystem left recompiled (the physics' step and solver, the race's ticks,
  the movies' update and drawing).
- `--skip_cutscenes`'s wait once the fast-forward ends: the pacer's next tick
  is now clamped to one tick ahead (`subtick/frame_pacer.cpp`); to confirm.
- `--camera_fov` (the main camera's vertical FOV, Graphics/Experimental): works in the game (the user's test, 2026-10-25). Safe range 30 to 60 degrees (the game is about 47). Limitation: it changes every use of camera pass 0, so the replay and wanderer/boss cinematic camera tracks are affected too. Limiting it to racing is planned (`docs/functions.md`, "Main camera"), and the `--log_record_matrix` diagnostic is still in the code (off).
- Shader table: `--dump_shader_compiles` (the dump that wrote the table
  `graphics/compiled_shaders.h` is built from) was removed. The table stays;
  a shader it doesn't hold still goes to the real compiler and is logged.
  To remake the table, restore the dump branch from git history
  (`src/hooks/shader_compiles.cpp`, before its removal) first.
- A CI build.
