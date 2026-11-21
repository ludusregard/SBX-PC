# Known functions

Names for the generated `sub_XXXXXXXX` functions, worked out so far. The
address is the function's location in the original `default.xex`, so it also
matches Ghidra or IDA. Names are our own guesses; the game shipped without
symbols. The data files the code reads are described in
`docs/game_data.md`.

Status:

- **Native**: plain C++ in `sbx_pc/src/` that doesn't use the
  PowerPC context (`ctx`): normal arguments and host structs (it may
  still read and write guest memory through `src/guest.h` while
  recompiled code shares the data). The game reaches it without `ctx`,
  from native or overridden callers, and the recompiled original is no
  longer called (except behind a fallback flag or to check against it).
  The end state for every function.
- **Overridden**: an `extern "C" REX_FUNC(sub_XXXXXXXX)` override in
  `sbx_pc/src/` (or a macro defining one) does the work instead of
  the original: a thin adapter in front of native code, or a body that
  still uses `ctx` (calling guest code, reading registers, or a copy of
  the generated body with an edit, marked `PATCH:`). It may call the
  original when its flag is off, or to check itself against it.
- **Wrapped**: an override that does something of its own (skipping
  render-only passes, logging, timing) and still calls the original
  (`__imp__sub_XXXXXXXX`) for the work.

The generated code is never edited in place: every build regenerates it.
A function usually moves from recompiled or Wrapped to Overridden to
Native as the `ctx` edge moves up through its callers.
`sbx_pc/tools/check_function_statuses.py` checks this column against
`src/` (an override marked Native, a status with no override, an override
with no row); run it after changing either.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82090108` | `main` |  | Guest entry point on the main thread. |
| `sub_825739E0` | `Thread::Start` |  | Entry point of the game's threads: calls vtable slot 1 of the object at start context + 8. |
| `sub_82095FD8` |  |  | A worker thread's loop (started through `sub_825739E0`): waits on an event, runs jobs through function pointers, signals events; it opens the game's data files at boot. Busy only around loads. |
| `sub_8209A8F0` | `App::Run` |  | Main loop: calls every handler in the tick list at this+4, repeats until the quit flag at this+12 is set. |
| `sub_8209A400` | `ForEachTickHandler` |  | Calls each handler in a list through a member function pointer. |
| `sub_82093280` | `Renderer::Init` |  | Sets the vblank interval (2 = 30 fps) via `sub_82573100`, then subscribes renderer callbacks to numbered events (2, 3, 5, 6, 16). |
| `sub_82092E60` | `Renderer::WaitForFrame` | Wrapped | Tick handler on the main thread. Blocks until the render thread signals the frame is done (event at this+392), swaps, and starts the next frame. With `--subtick`, then paces the coming pass (`src/subtick/renderer_hooks.cpp`, see "Subtick"). |
| `sub_82092050` | `Renderer::ThreadLoop` |  | Render thread loop; started through `sub_825739E0`. Draws one frame each time the main thread asks, after a 2-vblank wait. |
| `sub_82091B60` | `Renderer::Present` |  | Flushes, waits for the vblank interval (`sub_825737E8` → `sub_82573638`), then swaps buffers. Event 5. |
| `sub_82091FB0` | `Renderer::DrawFrame` | Wrapped | Render thread: calls each draw listener in the list at renderer+28. |
| `sub_82091CC0` |  |  | Draw listener between the scene draw and present: resolves the frame into a front buffer (`sub_82573538`) and waits for the GPU to go idle (`sub_8229CB08`). |
| `sub_82098DF0` | `Scene::Update` | Wrapped | Tick handler on the scene object (0x40026FE0 at the menu). Calls the current mode object at +64 (vtable slots 6 and 2). |
| `sub_82098E68` | `Scene::Draw` | Wrapped | Draw listener on the same object. Calls the mode object's draw (vtable slot 5), reading live game state. |
| `sub_8216F0F8` | `Delegate::Invoke` |  | Calls the function at +8 with the argument at +4. Used for tick and draw listeners. |
| `sub_8222E468` | `MemberDelegate::Invoke` |  | Calls the member function at +8 on the object at +4, adjusted by +12. |
| `sub_82573100` |  |  | Receives the frame interval from `sub_82093280` in r6 (2) and stores it at +0x130 (0 and 1 store 1, 2 stores 2, 4 stores 3, 0x80000000 stores 0); `VideoOutput::WaitForVblank` waits that many vblanks before each present. The `--tick_rate_60` switch that set it to 1 was removed 2026-10-25 (see "Tick rate experiments"). |
| `sub_82573638` | `VideoOutput::WaitForVblank` | Overridden | r3 the video output: calls the device (`sub_8229DD58` with [+140]), then waits for the vblank interval at +0x130 (+304). With `--subtick` only the device call runs; the pacer holds the rate (`src/subtick/renderer_hooks.cpp`). |
| `sub_82573920` | `Event::Wait` |  | r3 = event, r4 = pointer to timeout (-1 = forever). |
| `sub_82573908` | `Event::Signal` |  | r3 = event. |
| `sub_822AE7E8` |  |  | Graphics driver present bookkeeping (frame counter at +20024). Library code, not game timing. |
| `sub_8259F788` | `QueryPerformanceCounter` |  | System library. |
| `sub_820BFF60` | `Vehicle::GetSpeedKmh` |  | Returns the float at vehicle+48 (m/s) times 3.6. |
| `sub_820943D8` |  |  | First tick handler: polls input, then walks listener lists that run both game logic and the draw step. |
| `sub_82106478` | `GameMessages::Dispatch` | Overridden | Delivers the game's queued messages to their handlers: 0 = Update (all game logic), 1 = Draw. r3 = the dispatcher: handlers +0 (array), handler count +200, queue of 50 12-byte messages at +204, queued count +804, read index +808. The game has several dispatchers (40 call sites). Replaced with the same steps (`src/subtick/game_messages.cpp`) with `--subtick` (Update skipped on render-only passes); otherwise the original. |
| `sub_82106410` | `StateTable::Dispatch` | Overridden | Picks the handler object a message goes to; its method 0 handles it. r3 = a handler object, r4 = the message: returns the entry for (the object's state at +9604, the message id), `0x82695848` when there is none, and moves +9604 to the entry's next state (`docs/menus.md`). Wrapped by `src/hooks/autosave.cpp` to watch the garage menus (from `src/hooks/title_screen.cpp`'s override), which calls our C++ for it with `--native_game_modes` (`src/game/game_modes.cpp`). |
| `sub_82099700` | `Input::Update` | Wrapped | r3 the input manager: physical pads (+988, 4 x 212, `PhysicalPad::Read`), logical pads (+8, 5 x 196, `LogicalPad::Build`), processors (+1840, 5 x 416, vtable slot 1), listener slots (+3924, 5 x 12; the first node's object, vtable slot 1). Skipped on render-only passes (`src/subtick/input.cpp`). |
| `sub_82159540` | `Race::DrawStep` | Wrapped | The race's draw step (Draw message). Writes a field at +64 of the object its calls return. Noted to tell races from menus (`src/subtick/game_messages.cpp`). |
| `sub_820A3B98` | `CameraPass::Update` | Wrapped | Scales the main camera's projection after the update (`--camera_fov`, `CarTransforms_CameraFov`), then rebuilds the view-projection. Rebuilds a camera pass's view (+0x10) from its eye, target and up vector (+0x190, +0x1A0, +0x1B0), and its view-projection (+0x90 = view * projection at +0x50). Called several times per Draw for pass 0. On pass 0 in a race, built from the camera blended between ticks; also run on pass 0 before `Race::DrawStep` on render-only passes, so the cars are drawn with the frame's view-projection (`src/subtick/car_transforms.cpp`, see "Subtick"). |
| `sub_8209BE70` | `Matrix::Multiply` |  | out (r3) = a (r4) * b (r5), 4x4. |
| `sub_8257ACD0` | `Recorder::RecordObject` | Overridden | r4 the view-projection, r5 the world matrix: a chunk of its own: command 7 (vertex c0-c8, 0x00010009), c0-c3 world x view-projection (Matrix::Multiply), c4-c7 the world (Matrix::Copy), c8 the scene's eye (0x8264EA40), 36. Returns the command's first slot. The subtick wrapper (`src/subtick/car_transforms.cpp`, --log_record_matrix) calls `hooks::NativeSceneryLists_RecordObject` (our C++ with --native_scenery_lists, else the original).|replaces `sub_8257ACD0` |
| `sub_8257AAE0` | `Model::IsShapeVisible` | Overridden | r4 the view-projection, r5 the world matrix, r6 the shape: world x view-projection (Matrix::Multiply into its 192-byte frame at +96, the w's 1 first); the shape's 8 bounding box corners (mesh +96) by the columns (the vector unit's dot products, denormals flushed), / w, flipped when w < 0; 0 when all eight are past x or y +-1.01, or all behind (w < 0) or all z < 0.|replaces `sub_8257AAE0` |
| `sub_825758A0` | `Command::SetShaderConstants` | Wrapped | Render thread, command 7: r3 = payload, r4 = register count, r5 = stage << 24 \| type << 16 \| start << 8 \| count. |
| `sub_82572638` | `CommandList::AllocateSlots` |  | r3 = list, r4 = slot count; returns the first 16-byte slot. |
| `sub_820BFF88` | `Vehicle::GetDistancePerFrame` |  | Speed (m/s) times a hard-coded 1/30 s. |
| `sub_820C2E40` | `Vehicle::Tick` | Original | Once a tick per car: r3 = car, r4 = its index in the control record table at `0x826B5810`, r5 and r6 for `sub_820C2208`. Stores speed x `[0x820029B8]` at car+6160, copies car+5236 (4 bytes) to car+6164, then twice: `Vehicle::ApplyControls`, clears the record's one-shot bits (`0x00010003`), `Vehicle::Step` with `[0x825F17E8]` (1/60), `sub_820C1B98`. Then `sub_820C2208` once. Called by `sub_82163CC8`, `sub_82164188`, `sub_82171950` (race modes) and `sub_8216F398` (every car). See "Vehicle stepping". The game's own (the weighted input is written at `PlayerControls::Build` instead). |
| `sub_820B6330` | `Vehicle::Step` | Original | r3 = car, f1 = step length. `Vehicle::SetStepLength`, then the car's physics parts (car+0x930 read by 9 of them), with an inner loop of car+0x940 (10) iterations (`sub_820B0600`, `sub_820B0720`, `sub_820B3E18`, `sub_820B07F0`, `sub_820B25E0`, `sub_820B4610`). Called by `Vehicle::Tick` and, once each with 1/60 from `0x82609328`, by `sub_8216E590` and `sub_821703B8` (placing cars). |
| `sub_820AF9D0` | `Vehicle::SetStepLength` |  | Stores f1 at car+0x930 and 1/f1 at car+0x934. Nothing under `Vehicle::Step` reads +0x934 or the 60.0 `sub_820B0DD0` stores at +0x938. |
| `sub_820BFB68` | `Vehicle::ApplyControls` | Original | r3 = car, r4 = its control record: steering +8, throttle +4, brake +6, +10, flags +0 (bits pick digital or analog handling of each), bytes +14..+17 (+17 the control type). Rates are scaled by `[0x825F17E8]`. Bits 0x1 and 0x2 (one-shot) and 0x2000 only set request bytes (car+2284, +2285, +2286). |
| `sub_820B1678` |  | Original | Part of `Vehicle::Step` (a leaf): counts car+0x958 up by 1 per step while a slip test holds (to 30, the 30.0 at `0x82000D54`, which is also the test's threshold), down otherwise, and scales car+284 by 1 - counter x 1/30 (car+284 is worked out again later in the step by `sub_820B1EB0`). Counts steps, so it's only right at the game's two steps a tick; the car physics doesn't change its step count. |
| `sub_820C1B98` |  | Overridden | Each step: updates five tracked positions (the car, `sub_82249148` of car+336, into car+5232+16; then 4 wheels, 368 bytes apart) through `sub_820F8350`. |
| `sub_820C2208` |  |  | Once a tick (r3 = car+6160, r4 = car): the car's course position, then the odometer: speed x 1/30 x a constant added to +84 and +88. |
| `sub_8216F398` |  |  | Calls `Vehicle::Tick` for every car in `[0x826C7D9C]`+0xB34 (count +0xB6C). |
| `sub_8215DBF8` | `PlayerControls::Build` | Wrapped | r3 the control record, r5 the pad, r6 a message dispatcher: builds the record from the action mappers through `Input::QueryAbstract` (ids from the table `[0x826B5838]` + pad x 120 + 0x1F7C0; throttle +4, brake +8, steering +0 after abstract id 1, +12, +48), each an integer curve with a dead zone; also posts messages 30 and 29 for one button. Reached through a vtable (`sub_8215E390`). Its record is the tick's last input sample (`src/subtick/input.cpp`). |
| `sub_821024D8` | `Input::Query` |  | r3 the pad, r4 an action id (151-180), r5 the kind (0 held, 1 pressed, 2 fired, 3 a u16 analog value, 4 a float analog value), r6 the result: the action mapper's read slot. |
| `sub_82102610` | `Input::QueryAbstract` |  | r3 the pad, r4 an abstract id (0-30), r5, r6 as `Input::Query`: maps it to one or two actions (e.g. 155 and 156 combined, one negated). |
| `sub_82105790` |  |  | Returns `[0x826B5838 + 4 x r3]`. |
| `sub_82577498` | `PhysicalPad::Read` |  | r3 the pad, r4 a pointer to the user index: XInput state into 40 floats at +8 (cleared each read), flags +4, rumble motors +168 and +180 (ticked down each read). |
| `sub_82577150` | `LogicalPad::Build` |  | Sums its bound pads' 40 values and clamps them. |
| `sub_8210C218` | `ActionMapper::UpdateActions` |  | r3 the mapper + 448: `ActionMapper::UpdateAnalog`, then `ActionMapper::UpdateHysteresis` per action and `ActionMapper::UpdateButtons`. |
| `sub_8210B908` | `ActionMapper::UpdateAnalog` |  | The analog value of each action: u16s at mapper+1072, floats at +1132, through response curve objects (vtable slot 1). |
| `sub_8210B850` | `ActionMapper::UpdateHysteresis` |  | An action's on/off thresholds and state: 20 bytes each at mapper+1852. |
| `sub_8210BCD8` | `ActionMapper::UpdateButtons` |  | Held, held before, pressed, released and fired words at mapper+448..+467; steps each action's repeat object (pointers at +472, vtable slot 1). |
| `sub_8210B6A0` | `Repeat::Step` |  | A repeat object: count +4, delay +8, period +12. |
| `sub_8215C0A0` |  |  | The race overlays' Update handler: `RaceOverlays::Run` on r3 + 9608. |
| `sub_8215C0A8` |  |  | The race overlays' Update handler while paused (ITC's `8215E158`): `RaceOverlays::Record` on r3 + 9608. |
| `sub_8226F9D8` | `Color::HsvToRgb` |  | A leaf: hue (degrees) x 1/60 (`0x820029C0`) gives the sector, then saturation and value. Likely converts the car paint colors (`docs/game_data.md`, car specs): halving its 1/60 swapped them. `sub_821E6F28` is a copy. |
| `sub_8210F260` |  |  | Wind sound; scales with speed. |
| `sub_8213DE38` |  |  | Parent of the speed trail object (switches mode at about 88 km/h). Calls `sub_8213DBE8`, `sub_8213DA48` and `sub_8213DDB8`. |
| `sub_8213BEB8` |  |  | Resets the trail history; runs on a camera switch. |
| `sub_820F1038` | `TextureAnimations::Advance` | Overridden | From the draw step part `sub_820F5F48`: advances the flipbook animation set at r3+1060 (`sub_821453B0`). Skipped on render-only passes (`src/subtick/per_frame_logic.cpp`); otherwise our C++ for it with `--native_course_scenery` (`src/hooks/course_scenery.cpp`). |
| `sub_821453B0` |  | Overridden | Calls `sub_82145350` on every animation in a set: count at +0, then offsets to each animation. |
| `sub_82145350` |  | Overridden | Advances one flipbook animation: frame count +0, current frame +4, tick counter +8 (wraps at the length at +12), frame start ticks from +16. |
| `sub_82145410` | `AnimationSet::CurrentFrame` | Overridden | r4 the animation: its current frame's 8-byte entry (after the animation's table), or 0 past its frames.|replaces `sub_82145410` |
| `sub_821452C8` | `AnimationSet::FindByTexture` | Overridden | Finds the animation in set r3 whose texture (after its key frames and one word) is r4; -1 if none. An animation without keys never matches. |
| `sub_82146F00` |  |  | Draws the light groups: for each group, builds a light list on the stack (count +32, 48-byte lights at +36, from static level data) and picks its texture's current animation frame. Calls `sub_8209CED0` → `sub_8209CD58`. |
| `sub_820F5610` | `Minimap::Draw` | Wrapped | From the draw step part `sub_820F5F48`; steps the minimap blink clock at `0x826B5784`/`0x826B5788` after drawing. Both put back on render-only passes (`src/subtick/per_frame_logic.cpp`). |
| `sub_8257A460` | `ModelAnimations::Apply` | Wrapped | From `Scenery::Draw`, every pass: r3 a scene model; two animation lists at +304 (16-byte entries, time +12) and +364 (32-byte entries, time +16), each applied at its time (`sub_825873B8`, `sub_82586710`), then stepped one frame (`sub_82587498`, `sub_825867D8`: +1.0, wrapping at the clip's end, clips 80 bytes at the list's +8, start +72, end +76). The course's moving textures. On render-only passes applied at the logic pass's time plus the frame's progress and put back (`src/subtick/per_frame_logic.cpp`). The step itself is our C++ with `--native_model_animation` (`hooks::NativeModelAnimation_Animate`, `src/game/model_animation.cpp`), called from the wrapper in place of the recompiled function. |
| `sub_82107A38` |  | Wrapped | From Draw, every pass; adds one to the play time frame counter at `0x826B5870` and runs `ActionMappers::Update` (call at `0x82107B48`). The one is taken back on render-only passes, and every other pass with `--tick_rate_60` (`src/subtick/per_frame_logic.cpp`). |
| `sub_82110928` | `PlayTime::Add` | Overridden | The frames played since the stats' mark (+6040) split into hours, minutes, seconds and frames (30, 1800, 108000 frames) and added to the stats' play time (+6032 to +6038) with the carries; the total into r3. |
| `sub_82107CB8` | `Boot::StartMovie` |  | Message handler: plays the next boot movie, id from the table at `0x826B588C` (index at `0x826B58A8`), via `sub_820A45D0` (name table at `0x8233AF8C`). |
| `sub_82107D80` | `Boot::WaitForMovie` | Wrapped | Message handler, every tick while a boot movie plays; ends the step on a button press if skippable (`0x826B58A4`) or when the movie has finished. |
| `sub_82107D20` | `Boot::EndMovie` |  | Message handler: stops the movie; the front end (Genki logo, title) follows. |
| `sub_820A4678` | `Movie::IsFinished` | Wrapped | 1 when the playing movie has finished. |
| `sub_82292F48` | `CreateFile` | Wrapped | The only caller of `NtCreateFile`; r3 = path. |
| `sub_8221FC48` | `TitleScreen::Update` | Wrapped | Counts idle ticks on the title screen and starts the attract race at 600. |
| `sub_820F3FF8` | `Lights::FindNearest` | Overridden | Collects the lights nearest a point, nearest first. Arguments: r3 the light set owner (list at r3+216: count +0, array +8 of 272-byte entries, each's +12 → +80 a light set), r4 the output, r5 the point, r6 the most lights to keep (at most 32; 0 returned above), r9 a direction vector, f1 a merge radius, f2 the farthest squared distance, f3 the least dot product. For each light (48 bytes: position xyz at +0, 32 bytes copied out): skipped beyond f2, or when the direction to it dotted with r9 is below f3 (a cone); within f1 of a light already kept, it replaces that one if nearer, else is dropped; otherwise insertion-sorted by squared distance into a 33-entry array on its ~2 KB guest stack frame. Writes 32 bytes a light to r4 (position with w = the float at `0x82000A20`, then the light's +16..+31) and returns the count. A light set's count (+0) is little-endian (byte-swapped after `lwz`); `sub_82292170` gives each of its entries, whose +0 count says how many times the entry's one light group (count +60, lights from +76) is scanned: the group pointer isn't advanced, so a count above 1 scans the same lights again (the merge then drops the copies). Only caller: `sub_820C8F10`. |
| `sub_820C8F10` |  |  | Asks `sub_820F3FF8` for up to 10 lights near a point (r1+112) in a cone (cosine-like value from `sub_8258E218` of a setting at `[r27+11844]+5500` times a constant) of the course scenery manager at `[0x826B51FC]` (its visible areas' light sets). Called from `0x820E19B8`, `0x820E66F0` and `0x820DC028`. |
| `sub_82292170` | `DataFile::GetEntry` | Overridden | Reads a container (`docs/game_data.md`): r3 the container, r4 an index; returns r3 plus the r4-th of the little-endian offsets at r3+4 (the count at +0 bounds r4; 0 beyond it); with r5's low byte set, 0 for an entry whose first word is 0. Light sets are containers; `sub_820F3FF8` passes r5 = 0. |
| `sub_8209E348` | `Archive::Init` | Overridden | At boot: allocates the two TOC buffers, reads `DATA\BUILD\TXR\BUILD.TOC` and `BUILD_CRS.TOC` into them, opens `game:\DATA\BUILD\TXR\BUILD.DAT` and `BUILD_CRS.DAT`, allocates the shared load buffer (r4, 0xC80000 from `sub_821066D0`) and the 10 MB staging ring, takes the three semaphores and starts the loader thread (the runner r3's second virtual, with the functor at `0x825F0B30`). The game sizes the TOC buffers from the entry counts built into the XEX (`0x825F0A90`, `0x825F0AA0`); ours (`hooks::NativeArchive_Init`, `src/game/archive.cpp`, `--native_archive`) sizes them from the `.TOC` files and reads them on the host (the same for the game's own files; the game's Init runs if they can't be read). The patch loader's wrapper (`src/hooks/patch_loader.cpp`) calls it, then changes the TOC entries of the ids it replaces. The functor's +20 gets a word of Init's frame the game never writes; ours keeps the frame and copies the same word. With the native Init the shared load buffer is `--archive_shared_buffer_kb` KiB (never less than r4; the game's size if the larger allocation fails). |
| `sub_8209D4D0` | `Archive::Shutdown` | Overridden | Frees the shared load buffer, the staging ring and the TOC buffers, and closes the two `.DAT` handles (a failed close: the read error). Called from `sub_82106800`. |
| `sub_8209D988` | `Archive::GetToc` | Overridden | r3 a file id: returns the `BUILD` TOC buffer for ids below 23009, else the `BUILD_CRS` one. |
| `sub_8209DF80` | `Archive::Load` | Overridden | r3 a file id, r4 the destination, r5-r8 options: reads the id's TOC entry (start sector, stored and decompressed size), checks `Archive::FindLooseFile`, and queues a read for the loader thread (`sub_8209DD38`). Ids at or above 23009 are in `BUILD_CRS`. r8 is a read size that overrides the TOC's stored size; every caller passes 0. A decompressed size of 0 means the entry is stored as is (no unpacking). |
| `sub_8209D9F8` | `Archive::FindLooseFile` | Overridden | r3 a file id (the game's, 0-24367): returns the loose file's path from the table at `0x825F0AB0`, or 0. Paths are relative (`DATA\TXR\MOVIE\OPENING.WMV`); the loose read (`sub_8209E0E0`) opens them under `game:\`. Only caller: `Archive::Load`. Returns a PATCH_DATA override's path first (`src/hooks/patch_loader.cpp`); else, with `--native_archive`, our walk of the table (`game::FindLooseFile`, `src/game/archive.cpp`). |
| `sub_8258C000` | `operator new` |  | r3 a size; returns the block (retries through the new handler when out of memory). |
| `sub_8209DD38` | `Archive::Queue` | Overridden | r3 a 40-byte request (`Archive::Load`'s): copies it into a free slot (`Archive::FindSlot`) at the end of the queue, under the queue's lock; wakes the loader when nothing was queued and no wake is pending. Returns the serial, or -1 when all 960 slots are taken. Serials count up to 0x7FFFFFFF, then start from 0. |
| `sub_8209E250` | `Archive::LoaderThread` | Overridden | The loader thread's body (no direct callers; started by `Archive::Init`; never returns). The game's: takes requests off the queue, reads them from a `.DAT` (`sub_8209DE20`, asynchronously, whole sectors) or a loose file (`sub_8209E0E0`), and unpacks a compressed one (`Ucl::Unpack`) while the next is read, alternating the two 5 MB halves of a staging ring. Ours (`--native_archive`, decided once at boot; the game's loop when the `.DAT` files don't open on the host): queue order. The next request's `.DAT` read runs on a host thread, into host memory, while the current one is unpacked or copied and its callback runs. The next request is marked as started where the game's picker marks it: before a compressed request's callback (except under flag 0x1, or a plain next request into the same destination), after a plain one's. Loose files are read through the game's read (`sub_8209E0E0`). No 5 MB limit. Stored UCL blocks go through the game's memcpy, bounced through the staging ring. Loads into the shared buffer that don't fit are refused (not written; the callback still runs). Callbacks get r3 the serial, r4 the data, r5 the size, r6 the context. Signals the idle semaphore when `Archive::WaitIdle` asked and the queue drained. `--log_archive` logs each load with its read, wait, overlap, unpack and callback times. |
| `sub_8209DED0` | `Archive::PickNext` |  | The loader's picker (r3 the previous request): finishes a plain previous request (`sub_8209D7A8`) before picking the next; a compressed one is chained to the next (`sub_8209DA70`) and finished after the next read. Not picked early when both have flag 0x1, or when the next one is plain and reads into the same destination. Ours: the native loader thread (`sub_8209E250`). |
| `sub_8209DA70` | `Archive::PickAndChain` |  | Picks the request after r3 (state 1, set up by `sub_8209D5B0`) and chains r3 to it (+84, r3 state 2); with none, r3 is chained to itself. |
| `sub_8209D5B0` | `Archive::SetUpRequest` |  | Sets up a picked request for its read. |
| `sub_8209D7A8` | `Archive::FinishRequest` |  | Runs the request's callback and frees its slot; VirtualFrees the data under flag 0x2 without 0x1000. |
| `sub_8209DE20` | `Archive::ReadDat` |  | Reads a request (asynchronously; completion `0x8209D928`) from `BUILD.DAT` or `BUILD_CRS.DAT` (byte offset = start sector << 11); a failed read calls `sub_8209AD48`. |
| `sub_8209E0E0` |  |  | The loose read (r3 a request slot): opens `game:\` + the request's path through the game's file class, seeks to the slot's +64, reads +68 bytes into +76, closes it and signals the read-done semaphore (`0x826AA748`). Our loader calls it for loose files and takes the semaphore back. |
| `sub_82292050` | `Ucl::Unpack` | Overridden | Unpacks a UCL stream (`uclpack` layout: 18-byte header, then blocks of BE decompressed size, stored size, data; a stored block is copied as is). Calls `sub_82292238` per block. Ours (`src/game/ucl.cpp`, host memory only) stops before writing past the expected size; the original checks a compressed block after unpacking it. Checked against 16 `BUILD.DAT` entries and their GUT Archive Tools dumps. |
| `sub_82292238` |  |  | Decompresses one UCL block. |
| `sub_820A0860` | `Archive::GetSize` | Overridden | r3 a file id: the TOC entry's decompressed size, or its stored size when that is 0. |
| `sub_820F2E58` | `Scenery::Create` | Overridden | The 248-byte course scenery manager (`docs/game_data.md`, "Courses"), stored at `0x826B51FC`; its draw settings (+196 the passes' mask 0x38435, +200 to +212 four floats), its texture provider at +228; then `Scenery::Init`. From `sub_82159798` (races) and `sub_82225548` (front end). |
| `sub_820F11F0` | `Scenery::Init` | Overridden | r3 the manager: stores it at `0x826B51F4` and clears its lists and fields (+172 and +176, the object last asked for, -1). |
| `sub_820F1310` | `Scenery::ClearGlobal` | Overridden | Sets `0x826B51F4` to 0. |
| `sub_82159798` |  |  | Race: makes the scenery manager and sets it up for the race's course: the index from `sub_8228C478` (place 1 for race modes 8-10, else place 0 and the race data's +32), the course table at `0x825F2E40`, streaming mode; then `Scenery::Setup` and `Scenery::StartLoad`. |
| `sub_8228C478` |  |  | r3 x 3 + r4: the race's course index (place, variant). |
| `sub_8228C488` |  |  | (r3 + 2) x 3 + r4: the front end's course index (scene, variant). From `sub_82229DD8`. |
| `sub_820F2EF0` | `Scenery::Setup` | Overridden | r3 the manager, r4 the 44-byte configuration: `Scenery::SetupPools`, then one 272-byte visible-area record per area at +224. Returns that block. |
| `sub_820F25B0` | `Scenery::SetupPools` | Overridden | Copies the configuration to +8 (mode 2 with one view takes `[[+40]]`); builds the light key table (+140, count +128) and the model key table (+168, count +156) from the course record at `[+36]`; sets up the five pools (sizes 60, 60, 3824, 88, 560; a pool whose flag is clear keeps the previous one's sizes). Pool 4's buffer is the shared load buffer (+188) when it's large enough; its data size is 0. |
| `sub_820F1EC0` | `Scenery::PoolSizes` | Overridden | The area and model counts and each pool's data size (mode 0: the sum, else the largest; models at most 16). The areas end at the first without lights. |
| `sub_820F13C0` | `SceneryConfig::ModelAsset` | Overridden | r3 the configuration, r4 an area, r5 a model: the model's id (the area's first id + r5), or -1 past its last. |
| `sub_820F1C90` | `Scenery::StartLoad` | Overridden | Loads the course set (pool 2); in mode 0 lists every area and model to load; with pool 0, its object too. |
| `sub_820F1DF8` | `Scenery::IsLoaded` | Overridden | Polled until the course is in: while +192 is set both lists all loaded (`SceneryList::LoadAll`), then the object last asked for loaded (or not in use). |
| `sub_820F23B0` | `CourseSet::Load` | Overridden | Loads set file 1 (`[+36]+12`) into a block of its own and set file 2 (`[+36]+8`) into the object, the last with the callback `CourseSet::OnLoaded`. |
| `sub_820F2148` | `CourseSet::OnLoaded` | Overridden | `Archive::Load` callback (r6 the set): `CourseSet::Parse`, then frees set file 1's block, then marks it loaded. |
| `sub_820F1900` | `CourseSet::Parse` | Overridden | Set file 1: entry 0 an XPR2, 1 a model, 2-7 lists of models. Set file 2: entry 0 the areas, 1 the light groups' textures and animations, 2 the light groups. |
| `sub_820F2188` | `AreaLights::Load` | Overridden | Loads an area's light file (id from the light key table by the object's key at +84) into its data with the callback `AreaLights::OnLoaded` (`sub_820F1AC0`). |
| `sub_820F1050` | `AreaLights::Parse` | Overridden | +80 = the light file's entry 0: the light set `Lights::FindNearest` reads. |
| `sub_820F2250` | `AreaModel::Load` | Overridden | Loads an area model (id from the model key table by the key at +64) into the manager's +188 (the shared load buffer) with the callback `AreaModel::OnLoaded` (`sub_820F1B78`). |
| `sub_820F10E8` | `AreaModel::Parse` | Overridden | Entry 0 into the model at +80, entry 1 into the XPR2 at +528; +56 and +60 point at them when there. |
| `sub_820F4428` | `Scenery::Update` | Overridden | Each update: the enabled camera passes set from their viewports (`sub_820AA460`); both lists cleared and filled from each view (`Scenery::CollectAreas`, then `CollectModels`), sorted and cut to +96 and +104 (`LimitLargeModels` between); loaded all at once (mode 0) or streamed; the visible areas (+216, +224) from the listed areas that are in and seen, with their listed models in and within viewport 0's far distance (each marked seen); streaming, the unseen released. 208-byte frame as the original's. |
| `sub_820F5F48` | `Scenery::Draw` | Overridden | Each frame's passes: the texture provider reset; the texture animations stepped and the visible models' animations applied (`sub_820F1038`, `sub_8257A460`) unless +196 has 0x4000; the set's XPR in slot 0; the sky (+196 0x10: the set's model, world the identity at the camera's eye, range 5,000 to 50,000), the set's model list (+528, 0x20: models within the view's far distance), the visible areas (`DrawArea`, 10 passes then 3), the set lists (`DrawSetLists`), the light groups (0x400) and the minimap after its clear (`sub_8209FB30`, `sub_820F5610`). The pass records are 24-byte tables at 0x825F2FE8 to 0x825F31B0. Our C++ through `hooks::NativeCourseScenery_Draw`, called by the frame state diff's wrapper (`src/debug/frame_state_diff.cpp`). |
| `sub_820FF4B8` | `TrackCollision::Load` |  | Once a race: the same course index as `sub_82159798`; loads the course record's mesh (+0) and route (+4). |
| `sub_820F7680` | `TrackMesh::Load` |  | r3 `0x826B57FC`, r4 the mesh id: into its own block, callback `TrackMesh::OnLoaded` (`sub_820F6890`). |
| `sub_820F9D08` | `TrackRoute::Load` |  | r3 `0x826B57F0`, r4 the route id: into its own block, callback `TrackRoute::OnLoaded`. |
| `sub_820F8F30` | `TrackRoute::OnLoaded` |  | Turns the route file's offsets into pointers (+12 sections, +16 corners, +20 12 lists); for each set bit of the race data's +24 (bits 0-11), narrows the listed slices' floats +24 and +28 to the listed values. |
| `sub_820FD5A8` | `WallQuery::IgnoresWall` |  | Skips collision walls by their kind, the race mode and stage flags. Not read here. |
| `sub_8213D278` | `AiPath::AddCornersAt` |  | The AI path's corners from a route slice's corner list. Not read here. |
| `sub_8213ADD8` | `AiPath::PreferredSections` |  | The sections an AI profile prefers at forks (a table in `.data`). Not read here. |
| `sub_82289118` | `TrackZones::Update` | Overridden | The HUD's 70 route zones (hard-coded ranges): road names and signs. Calls each zone's update in list order. |
| `sub_8209AD48` |  |  | Read error: ends in `sub_82292D50`, which shows the dirty-disc screen (`XamShowDirtyDiscErrorUI`). |
| `sub_82572370` |  |  | Reads a whole opened file into a buffer (r3 the file, r4 the buffer, r5 a pointer to the size); used for the TOCs and `DEBFONT.XPR`. |
| `sub_820CC048` | `Params::Load` | Overridden | The twelve `Archive::Load`s of BUILD 72-83 into their tables, the parameter object as context (callbacks `sub_820CBFE8`, `sub_820CBE48`, `sub_820C9B90` for 74-82, `sub_820CBB30`); r3 the last serial. |
| `sub_821744D8` | `RaceSounds::OnSceneStart` |  | Loads BUILD 22887, the loose `DATA\AUDIO\CAR_SE.XWB`, and starts the music (`Music::PlayRoaming`): the race sounds' state 1, message 4 (next state 38, or 24 in a replay). See `docs/audio.md`. |
| `sub_82094BB8` |  |  | Startup; names the XACT engine `txr_xact` and calls `sub_821066D0` (which calls `Archive::Init`). |
| `sub_8228CEE8` | `SaveGame::Write` | Overridden | One of four functions (with `sub_8228CFA8`, `sub_8228D228`, `sub_8228D370`) that open the save, `TXR360:\TXR_DATA_1`: formats the save's display name with the local time and asks the save interface for the write (`sub_820A0678`, 129,200 bytes). Logged by `src/hooks/autosave.cpp`'s override, which calls our C++ for it with `--native_save_interface` (`src/game/save_interface.cpp`). |
| `sub_8228CDD0` | `SaveGame::DeviceState` | Wrapped | The save device's state. Logged with `--log_autosave` (`src/hooks/autosave.cpp`), as are the four below. |
| `sub_8228CE08` | `SaveGame::Status` | Wrapped | Logged with `--log_autosave`. |
| `sub_8228CE40` | `SaveGame::Pump` | Wrapped | The save's pump. Logged with `--log_autosave`. |
| `sub_8228CD98` | `SaveGame::Finish` | Wrapped | Logged with `--log_autosave`. |
| `sub_8228CFA8` | `SaveGame::Read` | Wrapped | Opens and reads the save. Logged with `--log_autosave`. |
| `sub_82105790` | `GameData::Get` |  | r3 an index: returns the pointer at `0x826B5838` + 4 x r3. Index 0 is the save (`docs/game_data.md`). |
| `sub_82113D28` | `SaveData::Clear` | Overridden | r3 the save: zeroes it and sets its defaults. |
| `sub_82113FB0` | `SaveData::NewGame` | Overridden | r3 the save: clears it (`SaveData::Clear`) and sets a new game's state, including the car unlock bits at +0x1985C (every car but CON_X, ECLI, S30, L86, T86 and BOSS00). Run by the title menu's new game and at boot on the empty saves. Sets the cut cars' bits after it (`src/hooks/unlock_cut_cars.cpp`, `--unlock_cut_cars`), whose override calls our C++ for it with `--native_save_data` (`src/hooks/save_data.cpp`). |
| `sub_82114278` | `SaveData::NewGameKeepCars` | Wrapped | r3 the save: a new game that keeps the garage (+6120), the word at +6116 and the unlock bits at +0x1985C (copies them out, runs `SaveData::NewGame`, copies them back). From the title menu (`sub_8221F568`). Sets the cut cars' bits after it (`src/hooks/unlock_cut_cars.cpp`). |
| `sub_82114D88` | `SaveData::Unlock` |  | r3 the save, r5 what to unlock (a switch of 42 cases), r6 its id. One case sets car r6's bit at +0x1985C if it isn't set yet; new unlocks are queued for the garage notifications. |
| `sub_8217F2A0`, `sub_82272370`, `sub_8224A0E8` |  |  | Build the car lists (the starter cars and the shop): for each of the 72 cars with its bit set at save +0x1985C, copy its specs (`sub_820C9C38`) into list 0 or 1 by spec byte +771 (0 for the road cars, 1 for CON_X and the rival cars). |
| `sub_820C9C38` | `Params::GetCarSpec` | Overridden | r3 a car id: returns its 8,432-byte record in the car specs (BUILD 72). |
| `sub_820C9C50` | `Params::GetWheel` | Overridden | r3 a wheel number: returns record r3 - 1 of BUILD 73 (the wheels), or 0 for 0 (the stock wheel). |
| `sub_820C9BA0` | `Params::GetCarParts` | Overridden | r3 the output (9 pointers), r4 a road car id: points into BUILD 74-82, the aero parts (strides 280, 280, 280, 280, 168, 168, 84, 108, 72). |
| `sub_820CBFE8` |  | Overridden | Load callback for BUILD 72: stores the car count (size / 8432) at params+56 and byte-swaps the records (`sub_820CBF90` → `sub_820CBF08`, one swap per block: `sub_820CA2A0` block 1, `sub_820CBC10` block 2, `sub_820CA780` engine stage, `sub_820CAA48` tire, `sub_820CB1F8` paint, `sub_820CB328` wheel). |
| `sub_820CB1F8` | `Paint::Swap` | Overridden | Swaps a car's paint block, and fills in each slot's second shade (+12) for solid and metallic paints (`docs/game_data.md`). |
| `sub_820C96C8` | `CarSpec::GetTire` | Overridden | r3 the spec, r4 the tire type (0-4), r5 the grade (1-5), r6 1 for the rear: returns tire 0 for type 0, else (type - 1) x 5 + grade. |
| `sub_821ED5D8` | `WheelShop::MaxInchUp` | Wrapped | r3 the tuning menu: how many inch-ups the wheel shop offers for the garage car it shows, by car id: 20 minus the stock rim diameter, at most 4, on the road cars (RX8 2, RX7 3, N180 4); 0 for CON_X, the cut cars and any id over 22. The cut cars get the same rule (ECLI 3, L86/S30/T86 4) with `--cut_car_wheels` (`src/hooks/cut_car_wheels.cpp`). |
| `sub_820C44C8` | `Tire::InchUp` | Overridden | r3 a tire (a copy), r4 inches: bigger rims with the same outer diameter; recomputes width, aspect ratio, rim width and grip (`docs/game_data.md`, "Tires"). |
| `sub_820C4698` | `Car::CopySpec` | Overridden | r3 a car object, r4 its spec: copies block 1 to car+2240, block 2 to +1888, the engine stage at level [car+1652] to +2880, paint to +3364, and the front and rear tire (with inch-up) to +3084 and +3176; +3876, +3880 the widths' change. |
| `sub_820C47A8` | `Car::Refresh` | Overridden | r3 a car object (car id at +12): `Car::CopySpec`, then the aero parts (`Car::ApplyAero`) and the front and rear wheels (+3268, +3316). 50+ callers (garage, shop, race setup). |
| `sub_820C4378` | `Car::ApplyAero` | Overridden | r3 a car object, r4 its `Params::GetCarParts` pointers: copies the aero part records at the levels in car+1740 to +1780 to car+3636 to +3875, through one getter per part (`sub_820CB5E8` ... `sub_820CBA30`; level 0 = no part). |
| `sub_820CB468` | `AeroPart::Swap` | Overridden | Byte-swaps a 28-byte part record after it's copied. |
| `sub_820C4360` | `Car::GetSpecSheet` |  | r3 a car object: returns car+1888 (its copy of block 2), or 0. |
| `sub_820C2608` | `Vehicle::SetupFromCar` |  | r3 a vehicle, r4 a car object: copies the physics values from block 1 (dimensions, mass, wheelbase, tracks ...), the engine stage (rev limit, torque, +156-196) and the tires into the vehicle, then `Vehicle::ApplyTuning`. |
| `sub_820C0B48` | `Vehicle::ApplyTuning` |  | r3 a vehicle, r4 a car object: applies the tuning at car+1644 using tables of multipliers built in the code (7 entries per part, separate NA and turbo tables), the engine type as a cylinder count, and the aero parts. |
| `sub_822B8A90` |  | Overridden | Returns [r3+8] (with r3 = car+1644: the engine stage level). |
| `sub_820A0A88` | `Shaders::LoadAll` | Overridden | At boot: every shared shader pair (BUILD 3-69 as 0-33, 22800-22810 as 1000-1005, BUILD_CRS 24256-24366 as 2000-2055) through LoadPair's C++, then the shared tables set; r3 and r4 left the two tables as SetSharedShaders leaves them. |
| `sub_820A09D8` | `Shaders::LoadPair` | Overridden | r3 the vertex file id, r4 the pair: both files placed back to back in the shaders' buffer (`[0x82694E14]` + the cursor at `0x826B19B8`, moved on), a 12-byte context (the two places, the pair), two `Archive::Load`s, the second with `Shaders::OnPairLoaded`. |
| `sub_821364B0` | `Glow::Record` | Wrapped | The glow (bloom) recorder in races: builds the cross-fade quad (pixel shader `E0D070FB58A4EA03`, the vertex color's alpha is the history weight at +8460) and the glow constants c33 (+8436/+8440, +8452 the glow strength, +8456, +8476).. Its +40 (set by `Glow::Glow` `sub_821370E8`, cleared by Record) makes the weight 0 for a frame: no history; set on camera cut ticks (`--reset_effects_on_camera_cut`, `src/hooks/post_effects.cpp`). |
| `sub_82134EC8` | `View::UploadPreviousViewProjection` | Wrapped | r3 the view. Records vertex c100 x4, the motion blur's previous view-projection: copied from view+8080 (this tick's) when view+48 is set (a pass that ran the game logic) and view+44 is 0, else from view+8144 (the previous tick's). The view moves the pair on after the logic pass, so it's the same matrix all tick. Read by the motion blur's vertex shader (`matOldWVPVS`, BUILD 9/67). (The motion blur's record, vtable `0x820087F8` slot 0: the post effects share a base with +36 the camera pass, +40 a first-frame flag it clears.) On camera cut ticks both matrices are the camera pass's for the call, so there's no camera motion (`--reset_effects_on_camera_cut`, `src/hooks/post_effects.cpp`). |
| `sub_820A3088` | `Matrix::RotateZ` |  | r3 out, r4 in, r5 &angle: out = in * RotZ. |
| `sub_820A0E18` | `Matrix::Copy` |  | r3 destination, r4 source, 4x4. |
| `sub_8257CC90` | `Matrix::LookAt` |  | r3 out, r4 eye, r5 target, r6 up: a row-vector view matrix, forward = target - eye. |
| `sub_8257CBC0` | `Matrix::Perspective` |  | r3 out, f1 field of view, f2 aspect, f3 near, f4 far. |
| `sub_8257CF28` | `Projector::Build` |  | r3 out, r4 eye, r5 target, r6 up, f1-f4 field of view, aspect, near, far: look-at * perspective * a fixed bias (a projected texture's matrix, e.g. the headlights). |
| `sub_820D02A8` | `Model::Draw` |  | Draws a model: a visibility test (`sub_8257AAE0`), then `Recorder::RecordObject`. |
| `sub_820D04B0` | `CarPart::Draw` |  | r3 the part (nothing if its model at +4 is 0), r4 the camera, r5 the world matrix; calls `Model::Draw`. |
| `sub_8215A788` | `Car::TickUpdate` |  | The tick's car update: copies the car's world matrix to car+16 and stores the gauge values (speed, revs, boost) at car+696/700/704. |
| `sub_820DA930` | `Car::DrawUpdate` | Wrapped | Draw's per-car update (r3 = car - 4288), on every pass: takes 1.0 off car+704 (the boost; the tick sets it again), then draws the car's parts. The boost is replayed from the logic pass on render-only passes (`src/subtick/per_frame_logic.cpp`). |
| `sub_820E4DC0` |  |  | r3 the dash: calls `Dash::PoseTachometer` when [dash+20] is set, then `sub_820E35C0` when [dash+2180] is. |
| `sub_820E3EB0` | `Dash::PoseTachometer` | Wrapped | r3 the dash, r4 the car, r5 the camera passes: poses the tachometer needle from `Dial::TachometerAngles`. |
| `sub_820E4178` | `Dash::PoseDials` | Wrapped | r3 the dash, r4 the car, r5 the camera passes: poses up to seven needles, each from `Dial::Angle`. |
| `sub_820D32D0` | `Dial::Angle` |  | A needle's angle from a gauge value. |
| `sub_820D3388` | `Dial::TachometerAngles` |  | The tachometer's two angles from the revs. |
| `sub_8211D4B8` | `UiMovie::Update` | Overridden | r3 the movie, r4 0 = advance a frame and draw, 1 = draw only. Runs the HUD, the race start countdown and the pause menus. Advancing is replayed from the logic pass on render-only passes (`src/subtick/per_frame_logic.cpp`), whose override calls our C++ for it with `--native_ui_movies` (`src/hooks/ui_movies.cpp`): the queued and looping animations applied (`sub_82585FD8`) and their times stepped, the ended ones taken off the queue. |
| `sub_8211CF58` | `UiMovie::DrawQueuedText` | Overridden | r3 the movie, r4 its text list (+0 the 128-byte items, +8 their count), r5 draw only: each queued text drawn and taken off, last first. The item copied into the frame (`sub_8211B0A0`); its matrix's origin is the text's place (`sub_820AB518`); the matrix's scales and the item's clip written into the text node (+28, +32, +40 to +52); the font's style built in the frame from the node (pivot, rotation, color times the item's, alpha, box, spacings, alignments, layer) with the page from the node's size flags (+76: bit 0 page 0, 1 page 1, 2 page 2, 3 page 3, 4 page 2 with glyphs at 0.65, none page 2). A scrolling text's x from `TickerText::Offset`, then `TickerText::Step` unless draw only. The text's page copied (`Strings::CopyPage`) and drawn (`Text::Draw`). |
| `sub_8211B9D0` | `TickerText::Offset` | Wrapped | r3 the movie, r4 the text's draw (+0 x, +8 width, +120 id): the x a scrolling text is drawn at, from its record in the movie's node map (movie+39884: +4 set, +8 frame count, +12 frames before it scrolls, +16 speed). Between ticks taken between the last two frame counts (`src/subtick/per_frame_logic.cpp`). |
| `sub_8211BB10` | `TickerText::Step` | Wrapped | r3 the movie, r4 the text's id: counts a frame on its record. Skipped on render-only passes (`src/subtick/per_frame_logic.cpp`). |
| `sub_821196D0` | `AudioManager::PlayCue` | Wrapped | From the AudioManager's 2D and 3D cue slots (`sub_82119808`, `sub_821197D8`; its vtable is `0x82008318`, so `MenuSound::Play`'s slot 10 is `sub_82119808`). Nothing is started while the menus' drawing is redrawn between ticks (`src/subtick/per_frame_logic.cpp`). |
| `sub_82210730` | `RaceHud::RunCountdown` | Wrapped | r3 the HUD: runs the countdown movie (+612) and stores whether it's still going in +436, which the countdown state's Update (`sub_8216AA40`) waits on. Called from that state's Draw (`sub_8216A1C8`), so on every pass; on render-only passes +436 keeps the logic pass's value (`src/subtick/per_frame_logic.cpp`): a replay after a race stayed in the countdown state, with the race HUD and the race's pause menu. |
| `sub_82225DE0` | `ParkingArea::DrawScene` | Wrapped | r3 the area (`[0x8280DE60]`): its camera from +592 (eye) and +608 (target), then its scenery (`Scenery::Draw` on +552). From the parking area's handler `sub_8222C840` (message 0), which then draws the cars (draw object +5004 = 1, `sub_82225918`, `sub_820C93A8`, `sub_820C5BA0`). Drawn again between ticks with `--subtick_menu_3d` (`src/subtick/per_frame_logic.cpp`). The camera passes it updates are noted for the native renderer's own passes (`CarTransforms_MenuSceneBegin`/`End`, `--xegtao`). |
| `sub_8217DD48` | `GarageStage::Step` | Wrapped | r3 the stage's holder (+0 the stage, 6208 bytes, `VirtualAlloc`), r5 keep the camera's pitch in range: moves the camera around the car (`sub_82179EE8` → `CameraPass::Update`), turns and poses the car (`sub_8217A9D0`) and draws it on its stage (`sub_820C5BA0`, `sub_820C8238`). Called from the garage's and shops' messages, once a tick; drawn again between ticks with `--subtick_menu_3d` (`src/subtick/per_frame_logic.cpp`). The camera passes it updates are noted for the native renderer's own passes (`CarTransforms_MenuSceneBegin`/`End`, `--xegtao`). |
| `sub_8217A620` | `PowerGraph::Draw` | Wrapped | f1 the graph's left x, f2 its base y, r6 a car data (+1280 the rev limit, +1292 32 torque points 400 rpm apart, +1420 their scale; r3 passed on, not read): the torque and power curves as line segments (`Line::RecordFlat`, `sub_82179558`). From `TuningScreen::DrawPowerGraph` (`sub_821DD790`, the tuning screen's update `sub_821E5010`) and the garage's car lists (`sub_8224E050`, `sub_8224E0E0`), once a tick. In the menus with --subtick each call is noted with a copy of the curve and drawn again on render-only passes (`src/subtick/per_frame_logic.cpp`): it flickered. |
| `sub_82585FD8` | `UiMovie::ApplyAnimation` | Overridden | The animation's tracks and sub-animations at time f1 on the node; whether it's still going. |
| `sub_82585C40` | `UiMovie::ApplyTrack` | Overridden | r3 the node, r4 the track (+0 its property id), f1 t: writes one word into the node. Some tracks add to the value instead of setting it. r5 and r6 the keys t is between; a key blends as a step, a line or a curve. Subtick's override (`src/subtick/per_frame_logic.cpp`: the adds once a tick; on render-only passes every word a track writes is put back after the outermost update has drawn, as the nodes are outside the bytes the replay restores) calls our C++ for it with `--native_ui_movies` (`src/hooks/ui_movies.cpp`). |
| `sub_82585560` | `UiMovie::TrackProperty` | Overridden | Property r3's descriptor, the table cleared and filled on first use. |
| `sub_82582D50` | `UiMovie::Child` | Overridden | The child r4 of a group or scene (the entry [+108] + r4). |
| `sub_82119D50` | `UiMovie::RestorePose` | Overridden | The live scene's nodes put back to the rest pose (common fields, then per type), groups and scenes recursed. |
| `sub_8210B140` | `ActionMappers::Update` | Wrapped | r3 the mapper list: updates every action mapper (`ActionMapper::Update` each): held, pressed and fired words, and the menu repeat. Counts calls, so it must run once a tick: skipped on render-only passes, except when `src/subtick/input.cpp` runs it for the analog values. |
| `sub_8210C4A8` | `ActionMapper::Update` | Wrapped | r3 the mapper: raw axes from the logical pad (`[+2880]`) into +388..+446, then `ActionMapper::UpdateActions`. Digital state put back after it when run for the analog values (`src/subtick/input.cpp`). |
| `sub_82209F78` | `MenuList::Update` |  | A menu list's update. |
| `sub_8209A1C0` | `TickHandler::Invoke` | Wrapped | r3 the app, r4 the handler (target at +8). |
| `sub_820DC5C8` | `BlinkTimers::Advance` | Wrapped | Steps the blink timers; once a tick: skipped on render-only passes (`src/subtick/per_frame_logic.cpp`). |
| `sub_8215BE00` | `RaceOverlays::Run` | Overridden | The race overlays (25 objects at r3, e.g. the marker above opponents, the light trails): update (vtable slot 4) and record (slot 5). With `--subtick`, the update here on logic passes, the recording moved to just before `Race::DrawStep` on every pass (`src/subtick/per_frame_logic.cpp`, "Race overlays"); otherwise the original. |
| `sub_8215BE80` | `RaceOverlays::Record` | Overridden | Records the 25 overlays (slot 5) without updating them; from the paused handler `sub_8215C0A8`. With `--subtick` in a race, on logic passes it only saves the list, and the recording happens before `Race::DrawStep` on every pass, as for `RaceOverlays::Run`. |
| `sub_821281B0` | | Wrapped | World-space sprite list recorder (r3 = list, r4 = camera pass, its +144 used): the light trails. Corners blended between ticks. ITC's `sub_82128750`. |
| `sub_82127C08` | | Wrapped | Single world-space sprite recorder (r3 = sprite, r4 = camera pass), reached through `sub_82128CF8`. Corners blended between ticks, except for the marker (blended by its centre in `sub_8212BDA0`). ITC's `sub_821281A8`. |
| `sub_8212BDA0` | | Wrapped | The opponent marker's overlay record (vtable slot 5; r3 = overlay, r4 = camera pass index; ITC's `sub_8212CE40`): builds the sprite on the stack from its centre (overlay +176..+184, set once a tick) and axes facing the camera pass's eye. With `--subtick` the centre is blended between ticks for the call. |
| `sub_82102B30`, `sub_82102BB0` | | | Replay: store the player's control record (r3 = record, r4 = pad), and replace it from the replay while one plays; from `sub_8215E390` right after `PlayerControls::Build`. |
| `sub_821573E0` | `RaceCamera::Update` | Wrapped | The race mode's Update handler (`0x82157948` jumps to it): the race camera's tick (see "Race camera"). Wrapped for the rival intro's logo by `src/subtick/per_frame_logic.cpp`, which calls our C++ for it with `--native_race_camera` (`src/game/race_camera.cpp`). |
| `sub_8220F7B0` | `RivalLogo::Draw` | Overridden | Draws the rival's logo: with its entry's flag 8, its UI movie looked up and run, and flag 16 set when it's over. Kept for render-only passes by `src/subtick/per_frame_logic.cpp`, which calls our C++ for it with `--native_race_hud` (`src/game/race_hud.cpp`). |
| `sub_8246BD88` | `MoviePlayer::OpenFile` |  | The movie player's file open (through a vtable); calls `CreateFile`. |
| `sub_820AE840` |  | Overridden | The render thread's command that starts the boot movie. |
| `sub_82106540` | `Bgm::Load` | Wrapped | r4 a bank name: looks it up in the table at `0x820069B0` (`bgm_tmp_01`-`17`, `BGM_SYSTEM`) and loads its sound bank (BUILD 22970 + i) and wave bank (22988 + i), or `SB_SYSTEM_SE` (22885, 22886) for any other name, through the sound object at `[0x826B5960]` (vtable slot 4). See `docs/audio.md`. Who calls it, by return address: "Music" in `docs/audio.md`. Custom music rolls first, by the caller, and for a blocked song hands it another name from the same list (`src/hooks/custom_music.cpp`). |
| `sub_82173C70` | `Music::PlayRoaming` |  | r3 the scene: by its play mode (+9652, a copy of the race data's +827888), play modes 2, 4, 5, 6 play one of entries 1-5 of `0x82609334` with `sub_8258F558() % 5` (the game's random roll), other modes entry 0 (`bgm_tmp_06`). From `sub_821744D8` and `sub_82174668`. |
| `sub_821741B8` | `Music::PlayRequested` |  | r3 the scene: plays the music each bit at +9644 requests and clears it (0x40, 0x80, 0x100, 0x200-0x2000: "Music" in `docs/audio.md`). |
| `sub_821743B0` | `Music::RequestBattle` |  | r3 the scene: sets the music bit at +9644 for the opponent, by its record's (`sub_821172E0`) class byte (+35) and its id ("Music" in `docs/audio.md`). |
| `sub_82174AD0` | `RaceSounds::TickAfterBattle` |  | The race sounds' tick in state 30 (after a battle): the car sounds' tick, the music change steps (`docs/audio.md`), loading the win or lose music itself (entry 11 or 12 of `0x82609334`). |
| `sub_822624F0` |  |  | Plays `bgm_tmp_14` or `bgm_tmp_08` by comparing +9672 and +9668 of r31 (race end, by the custom music categories; not checked). |
| `sub_821B8580` |  |  | Plays one of the 17 `bgm_tmp_` tracks by a setting (`[[r31+32544]+740]`, 0 = `bgm_tmp_01`): the music select. |
| `sub_82175238` | `RaceSounds::Init` |  | Copies the race data's play mode (+827888) to the scene's +9652, allocates the seven car slots' engine bank buffers, makes the car sounds (`CarSounds::Init`) and registers the race sounds' handlers: (state, message) -> handler, next state, listed in "The race sounds" in `docs/audio.md`. |
| `sub_82118818` |  |  | Builds `data\audio\%s.xgs` and reads the XACT global settings (`TXR_XACT.XGS`). |
| `sub_8244D0A0` | `AudioRenderDriver::Register` | Overridden | r3 the driver object (callback argument +12, handle +16), r4 the mixer callback: `XAudioRegisterRenderDriverClient`. Reached only through a vtable. |
| `sub_8244D128` | `AudioRenderDriver::Submit` | Overridden | r4 the mixed buffer (descriptor from `sub_82448738`): `XAudioSubmitRenderDriverFrame`. |
| `sub_8244D188` | `AudioRenderDriver::Destroy` | Wrapped | Unregisters the client (`XAudioUnregisterRenderDriverClient`). |
| `sub_821199B0` | `AudioManager::AudioManager` |  | r3 the object (0x658 bytes), r4 the app, r5 the engine name, r6 the bank slot count: the XLAudio engine (XAudio, X3DAudio), the listener, the bank slots, the XGS load task (`sub_82118818`), its tick handler `AudioManager::Update` and app event listener `AudioManager::OnAppEvent`. Made by `sub_82119D08` from `sub_82094BB8` (`txr_xact`, 3 slots). Vtable `0x82008318`; its 33 slots are listed in `docs/audio.md`. |
| `sub_82119288` | `AudioManager::Update` |  | A run-loop tick handler: XACT DoWork, every playing sound's update (`Sound::Update`), finished sounds dropped. |
| `sub_821192F8` | `AudioManager::OnAppEvent` |  | An app event listener: on 5 every sound released, on 6 it registers again (suspend and resume, probably). |
| `sub_82118220` | `AudioManager::Load` |  | Vtable slot 4: r4 the slot, r5 the name, r6 a flag, r7 and r8 the wave and sound bank archive ids: a `BankPair` made into the slot and loaded. From `Bgm::Load`. |
| `sub_821182F8` | `AudioManager::Release` |  | Vtable slot 5: r4 the slot: its `BankPair` released. |
| `sub_82118368` | `AudioManager::IsLoading` |  | Vtable slot 6: r4 the slot: 1 while either bank isn't in. |
| `sub_821183D8` | `AudioManager::IsLoaded` |  | Vtable slot 7: r4 the slot: 1 once both banks are in. |
| `sub_82118448` | `AudioManager::IsEmpty` |  | Vtable slot 8: r4 the slot: 1 if it holds no `BankPair`. |
| `sub_82118C60` | `AudioManager::StopCue` |  | Vtable slot 13: r4 the slot, r5 the cue: every playing one-shot of that cue stopped. |
| `sub_82118D28` | `AudioManager::IsCuePlaying` |  | Vtable slot 14: r4 the slot, r5 the cue: 1 if one is playing. |
| `sub_82117FD0` | `AudioManager::GetCategory` |  | Vtable slot 29: r4 the name: the XACT category, -1 for none. |
| `sub_82117FD8` | `AudioManager::SetCategoryVolume` |  | Vtable slot 30: r4 the category, r5 the volume. |
| `sub_82117FE0` | `AudioManager::PauseCategory` |  | Vtable slot 31: r4 the category paused. Slot 32 (`sub_82117FE8`) unpauses it. |
| `sub_82117E70` | `BankPair::BankPair` |  | r3 the pair (0x70 bytes): its fields (`docs/audio.md`), then `BankPair::Load`. |
| `sub_82117A30` | `BankPair::Load` |  | Both banks read from the archive: slot 2 into the fixed buffers `[0x826B5968]` and `[0x826B596C]`, the other slots into new ones; the archive's callbacks make the XACT banks. |
| `sub_821174B0` | `Sound::Sound` |  | r3 the sound (0x18 bytes), r4 the slot, r5 the cue, r6 the bank interface: its cue holder (`Emitter::Emitter`) prepared. |
| `sub_821175A0` | `Sound::Update` |  | Plays the cue once prepared, applies its 3D, and plays a stopped sound again when its +9 is set. |
| `sub_82110180` | `AudioSystem::Init` |  | At boot (`sub_821066D0`): r3 the AudioManager into `[0x826B5960]`, the volume globals to 1.0, the music buffers (`[0x826B5968]` 3 MB, `[0x826B596C]` 512 bytes), `SB_SYSTEM_SE` into slot 0 (`Bgm::Load`), the car sounds' X3DAudio at `0x826B5970`. |
| `sub_821102B0` | `AudioSystem::Shutdown` |  | Slot 0 released and the music buffers freed. From `sub_82106800`. |
| `sub_8210FC00` | `CarSounds::Init` |  | The seven car audio objects (0x4F0 bytes each) into `[0x826B591C]` and the car submix voice into `[0x826B5924]` (6 channels, 48 kHz, its effect from `0x826065C0`). From `RaceSounds::Init`. |
| `sub_8210FE48` | `CarSounds::Release` |  | The seven car audio objects and the submix released. From `sub_82175198` (state 37, message 50). |
| `sub_8210FEC0` | `CarSounds::StopAll` |  | `CarSound::Stop` on each of the seven slots. |
| `sub_82173DE0` | `CarSounds::Tick` |  | The cars' tick, from the race sounds' ticks in states 38, 23 and 30: waits out scene+9656, skips the cars while the state bit 0x4000 is set or 0x20000 isn't, builds each car whose banks just loaded, then `CarSound::Update` for every race car; pauses the cars while the system UI flags `[0x826B7420]`, `[0x826B7424]` are set. |
| `sub_82174048` | `CarSounds::Build` |  | Builds every race car's audio object from its loaded banks and sets the state bit 0x20000. From the race sounds' ticks while scene+9648 is 1, and `RaceSounds::OnRollingStart`. |
| `sub_8210FFA0` | `CarSound::Update` |  | r3 the car audio object (the car at +400): its 3D (`sub_8210E4D8`), engine (`sub_8210E958`) and `car_se` parts; the submix's effect by the car's tunnel flag. See "Car sounds" in `docs/audio.md`. |
| `sub_8210E4D8` | `CarSound::Update3D` |  | X3DAudio for the car from the camera; every output level times the effects level `0x82606BC0` (the cockpit view: unpositioned, 0.4 times it). |
| `sub_8210E958` | `CarSound::UpdateEngine` |  | The engine's two banks of four layers, crossfaded and pitched by rpm and throttle on the set's curve block. |
| `sub_82174668` | `RaceSounds::TickDriving` |  | The race sounds' tick in state 38: builds or ticks the cars, then the music change steps (`docs/audio.md`), with `Music::PlayRoaming`. `sub_82174890` (state 23, a battle), `sub_82174DE8` (17) and `sub_82174FF0` (10) are the same with `Music::PlayRequested`; only 38, 23 and 30 tick the cars. |
| `sub_821737C8` | `RaceSounds::FadeMusic` |  | One fade step: the music level (`0x82606B74`) down by the saved level (`0x82606BBC`) x 0.1, the Music category set; 1 once at 0. |
| `sub_82173888` | `RaceSounds::RestoreMusicLevel` |  | The music level back to the saved level, the Music category set. |
| `sub_821739A8` | `RaceSounds::Pause` |  | Message 29: every car slot paused and the Music category paused. |
| `sub_82173A40` | `RaceSounds::Unpause` |  | Message 29 in the paused states: the cars resumed (slot 0 seven times, as `RaceSounds::ResumeCars`) and the Music category unpaused. |
| `sub_82173AC0` | `RaceSounds::StopCars` |  | Messages 31, 49, 58: every car slot paused; the music plays on. |
| `sub_82174A60` | `RaceSounds::OnBattleOver` |  | Messages 34, 36 and 19: the effects level x 0, the cars' tick, the music level saved, a music change asked for. |
| `sub_82173900` | `RaceSounds::TickLeaving` |  | State 37's tick: once, cue (2, 0) stopped and every car slot paused. |
| `sub_821078D0` | `Modes::Create` |  | r4 the game mode (1-12): its mode object (1 the race, 2 the garage, 4 the title, 5 the parking area). |
| `sub_8214D900` | `SystemUi::Update` |  | The sign-in UI and system message box flags (`[0x826B7420]`, `[0x826B7424]`) around `XamShowSigninUI` and `XamShowMessageBoxUI`. |
| `sub_82582450` | `XLAudio::InitAudio` |  | `XAudioInitialize`: 6 channels, 48 kHz. |
| `sub_825828F0` | `XLAudio::CreateEngine` |  | r4 a game file (the XGS): `XACTCreateEngine` (`sub_825A4B68`) and Initialize with its contents. |
| `sub_8257C880` | `XPR::Create` | Wrapped | r3 the XPR object, r4 the loaded file: checks `XPR2`, copies the header to normal memory and the data to physical memory, relocates each `TX` resource (`docs/game_data.md`, "XPR2 loading"). Object: +0 header, +4 its size, +8 data, +12 its size, +16 records, +20 record count. The wrapper registers the stand-ins of a pack with a sidecar (`.sbpr`) with the renderer, after the original has relocated them (`src/hooks/sidecar.cpp`; `docs/game_data.md`, "Sidecar textures (SBPR)"). |
| `sub_8257CA30` | `XPR::GetTexture` |  | r3 the XPR object, r4 an index: that resource's header; an index past the end gives resource 0. |
| `sub_8257C7A0` | `XPR::FindTexture` | Overridden | r4 a name: the first record whose name matches (`_stricmp`) gives the header + its offset; 0 with no records, no name or no match. |
| `sub_8257C9C0` | `XPR::Release` | Overridden | The header block through FreeLater, the data block through FreePhysicalLater (`0x807F0000`), every word cleared. |
| `sub_82354AC8` | `TextureHeader::Relocate` | Overridden | r4 added to +32 above its low 12 bits, r5 to +48 when the texture has more than one mip level. r3 left the level count. |
| `sub_82573B80` | `Memory::Alloc` |  | r3 a size: page-aligned memory, tagged `XLMM`. |
| `sub_82573CC8` | `Memory::AllocPhysical` | Overridden | r3 bytes, r4 XMemAlloc's attributes: under the memory blocks' lock (`0x8296E198`), `XMemAlloc((r3 + 8191) & ~0xFFF)`, its 4 KB header `XLMM`, kind 2, countdown 1; the block after it, or 0. |
| `sub_8257A150` | `XPRList::XPRList` | Overridden | Vtable `0x82069A94`, the vector cleared, r4 slots (-1: 64) reserved and assigned empty (Reserve's and Assign's C++, the value on the host). |
| `sub_8257A018` | `XPRList::Set` | Overridden | r4 the slot, r5 the texture: stored; a slot past the list's size or room (or no storage) runs the original, which grows it.|replaces `sub_8257A018` |
| `sub_82579FB8` | `XPRList::Get` | Overridden | Vtable slot 3: r4 a slot, its XPR or 0 (no storage, past the size). |
| `sub_825797A8` | `XPRList::GetTexture` |  | r4 the XPR's index, r5 the texture's: `XPR::GetTexture` on that XPR. Vtable +20 (and +24, +28). |
| `sub_82579808` | `XPRList::FindTexture` |  | r4 the XPR's index, r5 a name: `XPR::FindTexture`. Vtable +32. |
| `sub_8257C238` | `Model::Load` | Overridden | r4 the mesh file, r5 an XPR2 file or 0: `Model::Release`, `Mesh::Parse`, the XPR made (`XPR::Create`) and set in the XPR list's slot 0 (slot 4 of its vtable; +48 = the list); then each skinned mesh's weights rounded to thousandths (x 1000 + 0.5 fused, truncated, x 0.001), each vertex's last weight 1 less the others. Returns the shape count. |
| `sub_8257BD58` | `Mesh::Parse` | Overridden | The `00 15 00 00` version (else `Version Error %x`), `Mesh::LoadShaders`, the header (the file up to its vertex data) copied to `Memory::AllocVirtual`'s block (+16) and its offsets made addresses (+28 to +44, the meshes' +396 to +408 and their weights', the materials' +384), the skeleton and track sets from its sections (with header +76), the float4 set always; with shapes, the vertex data into `AllocPhysicalEx(size, 0, -1, 4, 0x404)` (+20) and each mesh's buffers (one when skinned) and materials' buffers relocated. Its memcpys keep the original's return addresses. |
| `sub_8257A620` | `Mesh::LoadShaders` | Overridden | Loads the mesh file's vertex and pixel shaders (`XPhysicalAlloc Error Vertex Shader` / `Pixel  Shader`). |
| `sub_8257AE50` | `Model::RecordShape` | Overridden | r4 a shape, r5 the group, r6 whether to set the shaders, r7 a pixel constant (c76) or 0, r8 a vertex scale (c86) or 0. 0 with no materials; else a chunk: header (10), the vertex declaration, the streams (11; two for types 9 and 10 or a third of 6) and a skinned mesh's indices (22), sampler state 28 of samplers 0-3; per material the shaders (own pair or shared), vertex c9-c13 (the textures' colors from the model's float4 track values, then the material's color, the material animation and the model's slot-1 hook with the blend index's frame address), pixel c63-c67, blend (the table at `0x8264EC14`), render states 96/100 (group 0) and 56, the textures (XPR list slot 5) and their sampler states, the environment textures (slots 6 and 5), c86 and c76 with their defaults put back after the draw (12, 18); command 36. Returns the header slot. A 304-byte frame as the original's; its arguments saved in the caller's frame. |
| `sub_8257BA38` | `Model::RecordShapeDepth` | Overridden | r4 the shape, r5 the group, r7 a mask, r8 the alpha flag: 0 for a group without materials; else one chunk: the depth declaration (10, `sub_8256F4F0` of the mesh's +260), stream 1 (11: +320, or a skinned mesh's +412, stride +264), pixel c43 from 0x8264EA70, a skinned mesh's indices (22, its +408's +20), then each material passing the mask (its +404) and the alpha flag (its +388): render states 100 (+112) and 56 (+344), its indices (12, &+352), the draw (18: +348, +340); 36. Model::MaterialCount called before each material, as the original.|replaces `sub_8257BA38` (`Model::RecordShapeUntextured`) |
| `sub_820CEA60` | `Model::RecordMaterials` | Overridden | A car part's shape (r3 the part model, r4 the shape, r5 the group, r6 whether to set the shaders): 0 with no materials; else a chunk: header (10), the vertex declaration, the streams and skin indices (as RecordShape's); per material the shaders (own pair, or the shared pair: 1005 when the part's +440 is set, 1000 for 1004), blend (the table at `0x825F1854`), vertex c9-c13 (four (0, 0, 0, 1), the textures' colors from the float4 track values, the material's color), pixel c63-c67, render states 96 (group 0), 48 (0 with `[0x826B4DE4]` set), 100 and 56 (2 and 6 swapped with `[0x826B4DE0]`); the textures: none (4 unset) for an untextured own pair; for shared pairs 1000/1004 the vinyl sheet (+444), 1001 the XPR list's, 1002 the XPR list's and the vinyl sheet, 1003 the XPR list's with the part's +460 for the id +436 names, the others none; for own pairs category 3 the part's +452 (one texture), category 5 `CameraPasses::RenderTarget(0)` (one texture), else +456 (with +432) or +460 (first id +436's) for every sampler, else the XPR list's; with their sampler states; sampler 6's environment texture (`[0x8296DD50]` or XPR list slot 6) and states 16, 20, 36 when bit 4 of +104, an own pair or category 1, 2, 7, 9, 10; the draw (12, 18); command 36. Returns the header slot. A 288-byte frame as the original's. |
| `sub_820CE7B8` | `Model::SetVinylTextures` | Overridden | r4, r5 to the part's +444 and +448. |
| `sub_820D5B70` | `CarModel::LoadExterior` | Overridden | The 3,696-byte set-up copied to +32, a new vinyl sheet (`Texture::Create`, A8R8G8B8 at the size at `0x825F2520`, into +6684, +6688), the team emblem into sticker group 7's first record (kind 9) for blocks 18 and up, set a or b by `[[+7256] + 8]`, then `LoadDecals`. |
| `sub_820D5680` | `CarModel::LoadSetA` | Overridden | Node file 892 + 226 x block and body XPR2 890 + 226 x block, each into pages of its own (`CarModel::AllocPages`); a size of 0 isn't loaded (the body's slot then empty). |
| `sub_820D5768` | `CarModel::LoadSetB` | Overridden | Node file 11062 + 197 x block through `LoadSlot::Load`, body XPR2 11060 + 197 x block into the shared buffer (no size check). |
| `sub_820D57F8` | `CarModel::LoadDecals` | Overridden | The six vinyl layers (record layers 0-5; id + 162 for 0-2, + 188 for 3-5) into +4092, then the 48 stickers from the last (groups of six: record layers 18, 12, 24, 30, 48, 42, 36, 6; id + 382, or the id itself for kind 9) into +4380, through `LoadShared`; a negative index leaves the slot empty. +4088 = 2, or 3 when nothing was asked for. Returns 1. |
| `sub_820D53F8` | `CarModel::OnDecalLoaded` | Overridden | The request's slot (a layer's, else a sticker's) marked arrived, `Vinyl::DrawDecals`, the slot reset; once no decal slot (either of a record's two) is loading, `Vinyl::ClearCorner` and `Vinyl::Compress`, +4088 = 3. |
| `sub_820D5C30` | `CarModel::SetVinylTextures` | Overridden | r3 a cockpit body: its six part models (+688) given its car model's sheet (`Model::SetVinylTextures` with +6684, +6692). |
| `sub_820D0620` | `LoadSlot::LoadShared` | Overridden | The same into the car scene's shared load buffer (`[0x826B2B24]` +5008, its size +5012) when the asset is smaller than it, else state 1 (empty). |
| `sub_820D0588` | `LoadSlot::Load` | Overridden | r4 the asset id, r5 the context, r6 the callback: into its own pages (VirtualAlloc of the asset's size); state 2 and 1, or state 1 (empty) and 0 for a size of 0. |
| `sub_820D7660` | `CarInterior::Load` | Overridden | The 120-byte set-up copied to +28; for set b the parts (11060/197/190, or set c's 19925/55/49 under a cockpit) then the accessories' slot 1 with the record's accessory and +116; for set a (890/226/219 or set c's) then slot 1 for 0 to 2, each answer's +808 = 0. |
| `sub_820D73E0` | `CarInterior::LoadParts` | Overridden | Six ids from r4 + block x r5 + r6 into +160 through `LoadShared` (callback `0x820D7228`). |
| `sub_820E6CA8` | `SceneWheels::SetUp` | Overridden | The 156-byte record copied to +32; the defaults row (44 bytes at `0x82004490`, by the block, or 45 + `[[+16776]]` for a traffic record of kind 3) into +112 to +152 (+120, the caliper texture, only when the record's +100 is 0); copied back to r4; `Wheels::BlockDefault` for blocks 18 and up with both wheel numbers -1; +148, +152 = the number isn't 190 when not negative; pool 5 of `[0x826B2B20]` claimed and got, +28 it, its +24 the car model (or +3264 the traffic record), `ScenePools::SetUp`. A record with +100 set and no car model runs the original (it reads the block through 0). |
| `sub_820D3620` | `Wheels::BlockDefault` | Wrapped | r3 a model block (18-44), r4 and r5 the front and rear wheel numbers: sets them to a fixed wheel per block (AD00 for the four cut cars, TA03 for BOSS00, wheel 18 for CON_X). The ECLI keeps -1 (its own stock wheel), and the L86, S30 and T86 get the BOSS00's wheel, or -1 with `--cut_car_steelies` (`src/hooks/cut_car_wheels.cpp`, `--cut_car_wheels`). |
| `sub_820D8DD8` | `Wheels::SetUp` | Overridden | The 156-byte record copied to +28, `Wheels::PickSetA` or `Wheels::PickSetB` by the model set (set a's in the parking area with `--parking_area_garage_wheels`, `src/hooks/parking_area_wheels.cpp`; without `--native_car_models` the original runs with the model set at 0 for the call), then for the front and rear the tire (581, or 583 when the record's +116 is set), the part and XPR2 (by their slots' ids) and the caliper (886), through `LoadShared`. |
| `sub_820D8860` | `Wheels::PickSetA` | Wrapped | Front and rear wheel XPR2 (+2144, +2168) and set a's wheel part (587 + 3i, the detailed rim, nuts and `punch` shapes) at +1168, +1192 from the wheel numbers at +44 and +60 (-1 stock: wheel = model block if <= 19, else AD00). With `--cut_car_steelies`, the L86, S30 and T86's stock wheel becomes the LORRY's after it (`src/hooks/cut_car_wheels.cpp`). |
| `sub_820D8900` | `Wheels::PickSetB` | Wrapped | The same with set b's wheel part (588 + 3i, the race's simple wheel: `B0`-`B2_12/20`, `B_space`) at +1168 and +1192; traffic (+28 = 3) uses wheel 20 + `[[r3+3264]]`. Wrapped like `sub_820D8860`. |
| `sub_82225548` | `ParkingArea::SetUpScene` | Wrapped | The parking area's course scenery (`Scenery::Create`) and car scene: `CarScene::SetUp` with set b (1) and seven car models, as the race's. `--parking_area_garage_wheels` is taken here for the scene (`src/hooks/parking_area_wheels.cpp`). |
| `sub_8215A250` | `Race::SetUpCarScene` | Wrapped | The race's car scene: `CarScene::SetUp` with set b (1). The parking area's garage wheels are turned off for the scene. |
| `sub_8217E9F0` | `GarageStage::SetUpCarScene` | Wrapped | The garage stage's car scene: `CarScene::SetUp` with set a (0). The parking area's garage wheels are turned off for the scene. |
| `sub_820E8250` | `WheelPoints::SetUp` | Wrapped | r3 the points' links: the tires, both calipers and, by the car scene's +4 (the model set), the loaded wheel part linked to `rims_a` (set a's shape names at `0x825F2964`) or `rims_b` (set b's at `0x825F2980`). Run with the car scene's +4 at 0 in the parking area with `--parking_area_garage_wheels`. |
| `sub_820ECA60` | `SceneWheels::Draw` | Wrapped | r3 the scene wheels: `sub_820EC7A0` when its +40 is set, else `sub_820EC668`; both step the calipers, tires and, by the car scene's +4, `rims_a` or `rims_b`. Run with the car scene's +4 at 0 in the parking area with `--parking_area_garage_wheels`. |
| `sub_820D6498` | `CarInterior::LoadSetCBody` | Overridden | The 144-byte set-up copied to +28; node file 19927 + 55 x block (`LoadSlot::Load`, +176), body 19925 + 55 x block into the shared buffer (+400), six parts by the record's choices (body id + choice + 3, 16, 19, 31, 36; the cabin + 35) into +536. The original's message for an empty node file, formatted into its frame and read by nothing, is left out. |
| `sub_820D3D18` | `Vinyl::DrawDecals` | Overridden | Each arrived layer and sticker (state not 0, 1, 2): decoded (`TGA::CreateImage`), its file released, drawn (`DrawDecal`: a 1,024 x 1,024 layer as region 9 at scale 1; layers 0-2 into layer 0's kind's region; 3-5 into regions 1 and 2; stickers into their record's region, mirrored by it), its pixels freed; DrawDecal's arguments in a 368-byte frame as the original's. A decal whose region is past 9 runs the original. Returns 1. |
| `sub_820EE4A8` | `Vinyl::DrawDecal` | Overridden | r3 the region (table `0x825F2A00`; past 9 the original), r4 mirrored, r5 to r7 the TGA header by value, r8, r9 the offset, f1 the angle; the caller's +96 the scale, +116 the pixels, +132 the sheet's data: the decal's corners through `DecalCorner` (center the decal's, offset less the center plus half the region), texels the decal's corners (left and right swapped when mirrored), two triangles into the region (the sheet's pitch `[0x825F2520]` x 4), in a 400-byte frame as the original's. |
| `sub_820D3920` | `Vinyl::ClearCorner` | Overridden | The sheet's bottom-left corner (sizes at `0x825F2520`, read from memory) cleared row by row. |
| `sub_820ECCC8` | `Vinyl::Compress` | Overridden | A DXT5 texture (`0x1A200054`) the sheet's size; both surfaces locked (the sheet's read-only) and described; `XGCompressSurface` (its arguments past r10 in a 272-byte frame as the original's: the source format, no rect, flags 1; f1 0.0); unlocked, released; the old sheet freed (`Texture::Free`) and the new texture in its place. |
| `sub_820ECB40` | `Texture::Create` | Overridden | A 52-byte header (VirtualAlloc, cleared), `XGSetTextureHeader` (1 level, usage 2; its stack arguments in a 160-byte frame as the original's), the data (`XPhysicalAlloc(size, -1, 0, 0x404)`, cleared), `Resource::Relocate`. Returns 1. |
| `sub_820ECC58` | `TGA::CreateImage` | Overridden | A reader in a 208-byte frame (as the original's): constructed, loaded, `TGA::ToARGB`, its header copied out, destroyed with the file kept. Returns 1. |
| `sub_820EE8E8` | `TGAReader::Load` | Overridden | 5 while it holds a file; else the size and file kept, `TGA::ParseHeader`, `TGA::Decode`. |
| `sub_820EEA68` | `TGA::ParseHeader` | Overridden | The file's 18-byte header, its 16-bit fields little-endian. Returns 0. |
| `sub_820EEFD8` | `TGA::Decode` | Overridden | 24 or 32 bits only (else 4); the pixels allocated (w x h x 3 or 4) before the type is checked; type 2 copied from byte 18, type 10 `TGA::DecodeRle`, others `OutputDebugStringA` and 4. |
| `sub_820EEF28` | `TGA::DecodeRle` | Overridden | Type 10: a count byte, bit 7 set one pixel repeated count + 1 times, else count + 1 raw pixels (3 or 4 bytes by the header's depth), until the pixels decoded reach width x height - 1 (a 1-pixel image decodes nothing). Its two packet helpers (`sub_820EEC18`, `sub_820EECC8`, only called from it) are done in line. Returns 0. |
| `sub_820EED78` | `TGA::ToARGB` | Overridden | New pages of top-down ARGB words from the bottom-up rows; 24-bit pixels get alpha 255. |
| `sub_820EDF38` | `SoftRaster::DrawTriangle` | Overridden | The vertices sorted by y (ties keep their order), rows from int(y + 0.5); the long edge's split at the middle row; each row's ends at i / n (fused as the original), their texels at (i + 0.5) / n corrected to the end pixels' centers, then `DrawSpan` with the texel-centers flag. |
| `sub_820EDC70` | `SoftRaster::DrawSpan` | Overridden | One row: the ends in either order (the right one left out), the texels 0.5 less with the flags' top bit (the caller's stack +84), clipped at the left and right with the texels moved along, each pixel's texel at i / n (`BlendPixel`). |
| `sub_820ED0D0` | `SoftRaster::BlendPixel` | Overridden | r3 the image, r4 where the canvas' address is, r5 the region (+8 pitch), r6, r7 the pixel, r8 the texel's two floats: the texel sampled (`SampleTexel`), skipped at alpha 0, else mixed over the canvas pixel by its alpha out of 256 (red and blue each product shifted down first, alpha and green together). Returns the texel. |
| `sub_8218C240` | `PostRace::Check` | Overridden | Post-race check: once loaded (+9608), waits for a counter (global at `0x826C7EC4`) to reach 60 before setting the next state at +9604. The 60 is 1 here (`src/hooks/post_race.cpp`, `--post_race_check_fix`): with 60, the screen stays black after quest races on fast hardware. It's also the garage top's load step, run on every entry to the garage (`docs/menus.md`); r3 is the garage top's handler object, which autosave notes. |

## Font

The text font (BUILD 22656, `docs/game_data.md`, "Font"). The font object
holds its descriptions (one per font, picked by the style's +60, and +108
for the second pass a style can ask for at +112), its style at +16, the
current description at +36, and the shaders: vertex declaration +8368,
vertex shader +8372, a pixel shader per channel at +8376.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_821089D8` | `Font::CreateShaders` | Overridden | The vertex declaration from the elements at 0x825F3280 (+8368); the vertex shader compiled from `[0x825F32C0]` (`main`, `vs_3_0`, flags 0) and made (+8372); the four pixel shaders from the sources at 0x826B58E4 (`ps_2_0`) (+8376..). Each compile's buffer: slot 3 for the code, slot 2 released. D3DXCompileShader's stack arguments at the 160-byte frame's +84 (errors out, +100) and +92 (0); the buffer out +96.|replaces `sub_821089D8` |
| `sub_82109280` | `Font::LoadSheet` | Wrapped | r4 the container, r6 the description it fills: +0 the glyph table's header, +4 its entries, +8 the XPR, +12 the texture, +16/+20 the texture's width and height (`sub_822A3160`, level 0), +24 1. Reached only through a pointer. The wrapper puts 576 x 928 there for a sheet that is an exact multiple of that size, so a larger sheet maps like the original (`src/hooks/font.cpp`). |
| `sub_822A3160` | `Texture::GetLevelDesc` |  | r3 the texture, r4 the level, r5 the description: width at +24, height at +28. |
| `sub_8210A530` | `Font::Draw` | Overridden | r4 the style, r5 the text: Font::SetParams, the page from the style's +60, Font::DrawPass (recompiled) pass 0; with the style's +112, again on the page at +108, pass 1.|replaces `sub_8210A530` (`Font::DrawText`) |
| `sub_8210A020` | `Font::DrawPass` | Overridden | r4 the text, r5 the pass: nothing for no text or no glyphs (Font::CountGlyphs); one chunk on the command list: a first slot (kind 5), SetTexture (the page's), render states 60 1, 72 6, 76 7, 308 0, 56 0, the scissor (31, the clip rectangle as halfwords) or 200 0, sampler 0's states 24 2, 20 1, 16 1, the vertex declaration (10) and shader (8), each channel's vertices inline (21, 144 bytes a glyph, CommandList::AllocateBytes), the glyphs (BuildVertices, or `sub_82109EB0` for pass 1), each channel's pixel shader (9) and DrawPrimitiveUP (19: triangle list, 6 a glyph, stride 24), 308 1, 36; then the render queue's slot 4 with the first slot, the style's +116 + 1 and f1 0.0 (-1000.0 for pass 1).|replaces `sub_8210A020` (`Font::BuildText`) |
| `sub_82108330` | `Font::CountGlyphs` | Overridden | r4 four counts, r5 the text: each character but spaces, `\n` and color codes counted in its glyph's channel (page breaks too). Returns the length.|replaces `sub_82108330` |
| `sub_82109BD0` | `Font::BuildVertices` | Overridden | r4 the text: the lines counted (to a page break), each line from Font::LineX and LineY; every character to the end: a space moves x on, `\n` starts the next line, 0xFFD6 the style's color, 0xFFF6-0xFFFF the font's colors (+8336), others Font::AddGlyph (and the second pass's point at +64 + 8i); x on by (advance + spacing) × the scale. A turned text calls `sub_82109910` first. Returns 0.|replaces `sub_82109BD0` (`Font::LayOutText`) |
| `sub_82109EB0` | `Font::LayOutPositioned` |  | Like `Font::LayOutText` with each glyph's position taken from the font's table at +64. |
| `sub_82109680` | `Font::AddGlyph` | Overridden | r6 the character, r7 the color, f1/f2 the top left: six vertices at its channel's write pointer (0x826B58BC), the cell's place in the sheet (columns = sheet width / cell width) as u, v; clipped (kept: the pointer on 144 bytes; dropped: its count down) or, turned, each vertex through `sub_82109088` (recompiled). A cell width of 0 runs the original.|replaces `sub_82109680` |
| `sub_82108DB0` | `Font::ClipGlyph` | Overridden | r4 a glyph's six vertices: 1 with no clip rectangle; 0 when wholly outside; else each vertex outside moved to the edge, its u or v by the same distance / the scale / the sheet's size.|replaces `sub_82108DB0` |

## Fixed time step

The game never measures elapsed time. Each pass of `App::Run`
(`sub_8209A8F0`) is assumed to be 1/30 s, so running the loop at 60 fps
doubles game speed. Functions that load the step as a float constant from
`.rdata` (`1/30` at `0x820029C4`, `1/60` at `0x820029C0`):

- 1/30: `sub_820B1678`, `sub_820B8BC0`, `sub_820BFF88`, `sub_820C2208`,
  `sub_8210DBE8`, `sub_8210F390`, `sub_8211F6F0`, `sub_82129DE0`
- 1/60: `sub_820B0DD0`, `sub_8226F9D8`, `sub_82153E40`, `sub_821E6F28`

Not all of these are time steps. `sub_8226F9D8` and `sub_821E6F28` are HSV to
RGB conversions: 1/60 turns a hue in degrees into its sector, so halving
`0x820029C0` swapped the car paint colors (green and blue). `sub_820B0DD0`
is the car's creation (its first step length, car+0x930). `sub_820B1678`
uses 1/30 as "per counted step".

Every load of those two floats in `.text` was checked (2026-10-17, a
`lis`/`addi`/`lfs` scan of `default.xex.decrypt`): only these functions read
them, each with one `lfs` (`sub_820B1678` at `0x820B173C`, `sub_820BFF88` at
`0x820BFF90`, `sub_8226F9D8` at `0x8226FA30`, `sub_821E6F28` at
`0x821E6F80`). The neighboring floats (`0x820029B0` to `0x820029CC`) are
separate constants.

Also loaded, and not yet checked:

- 30.0 at `0x82000D54` (22 functions): `sub_820A9DE0`, `sub_820AA040`,
  `sub_820C5BA0`, `sub_820DC8E8`, `sub_820DCD88`, `sub_820DD298`,
  `sub_82110C18`, `sub_821121F0`, `sub_82121920`, `sub_82164500`,
  `sub_8222BE40`, `sub_82450A78`, `sub_82467398`, `sub_824673F8`,
  `sub_8246ACB0`, `sub_82482410`, `sub_82568948`, `sub_8256D8F8`, and leaf
  functions (no `.pdata` entry) with the load at `0x820B16E0`, `0x820DC154`,
  `0x82143D18` and `0x8256B76C`. 30.0 also at `0x82002B80` (`sub_820C0B48`),
  `0x82002BAC` (`sub_8210EAB0`), `0x82007F50` (`sub_8210F390`), `0x82008744`
  (`sub_8212F278`, `sub_8212F5D8`), `0x8200A108` (`sub_8218FF88`) and
  `0x82010028` (`sub_82121920`).
- 60.0 at `0x82000F0C` (9 functions): a leaf ending at `0x82095790`
  (returns a / b * 60), `sub_820A7C08` (compares against 60),
  `sub_82110C18` and `sub_821110B0` (copied into tables on the stack), a
  leaf with the load at `0x821441A0` (compares against 60), `sub_821447D8`
  and `sub_82144958` (a value times a constant, clamped to 60),
  `sub_8217B770`, `sub_8217DFF0` (passed as f1). 60.0 also at
  `0x82002950` (`sub_820B0DD0`, stored into the object at +0x934 and
  +0x938). Some of these are likely not times (60
  degrees, a speed limit), so these constants are shared and can't be
  patched as a whole.
- 1/30 and 1/60 as `.data` variables (writable, so possibly tuning values):
  1/60 at `0x825F17E8` (`sub_820BFB68`, `sub_820C2E40` and a leaf with the
  load at `0x820BFF54`),
  `0x82609328` (`sub_8216E590`, `sub_821703B8`) and `0x82609370`
  (`sub_82151B18`); 1/30 at `0x8260707C` (`sub_82126C10`, `sub_82126D10`,
  `sub_82126EA8`, `sub_82127428`, `sub_821275E0`), `0x825F3204`
  (`sub_820FF1A0`), `0x82607690` (`sub_82144558`), `0x82609304`
  (`sub_82160FE8`) and `0x82609374` (`sub_82151B18`); many more are only
  written by static initializers (`0x825BE8CC` to `0x825BF1F4`).

Integer frame counts (for example `cmpwi r11,60`) aren't included.

## Tick rate experiments (removed 2026-10-25)

A true tick rate upgrade: all game logic at 60 ticks a second. Parked: the
game keeps its 30 Hz tick (see "Subtick"), as the original replays depend
on it. The two switches below (`src/tick_rate/tick_rate.cpp`, both off by default) were removed on 2026-10-25 with the code (`cull_emulated_build.ps1` deletes `src/tick_rate/`); the findings are kept:

- `--tick_rate_60`: the present interval is 1 vblank instead of 2
  (`sub_82573100`), so the main loop and all game logic run 60 times a
  second. Play time counts every other tick (`sub_82107A38`).
- `--tick_rate_half_step`: halves the step lengths once the XEX is loaded:
  1/30 -> 1/60 at `0x820029C4` (`.rdata`, mapped read-only, so the page is
  made writable for the write and put back) and the cars' step length at
  `0x825F17E8` 1/60 -> 1/120. `--tick_rate_half_step_mask` picks which (bit
  0 = 1/30, bit 2 = the cars' step; default 5, both). The 1/60 at
  `0x820029C0` was halved too until 2026-10-18; that swapped the car paint
  colors (it's the HSV conversion's 1/60), so it's left alone.

With `--tick_rate_60` alone (2026-10-17, before the cars' step length was
added) the game ran at twice the speed. So did halving only the two `.rdata`
constants: the cars are stepped by `0x825F17E8`, not by them (see "Vehicle
stepping"), and some of their users aren't step lengths (`sub_820B1678`).

On 2026-10-18: halving only the cars' step length (mask 4) at 30 ticks
halved the car's speed and nothing else. With `--tick_rate_60` too, the
car's speed was right (timed with a stopwatch), but grip and the rest of the
handling felt doubled: what runs once a tick (`sub_820C2208`, the rest of
game logic) ran twice as often.

What a real upgrade would also need: everything that counts ticks
(`sub_820B1678`, the action mappers' repeats, UI movies, the title screen's
600, the 30.0 and 60.0 users above, integer frame counts) scaled.

## Vehicle stepping

The game already steps the cars twice a tick. `Vehicle::Tick`
(`sub_820C2E40`), once a tick per car, runs two steps of 1/60 s
(`[0x825F17E8]`, a `.data` float only ever read): each applies the car's
control record (`Vehicle::ApplyControls`, `sub_820BFB68`), clears the
record's one-shot bits (`0x00010003`: the first step sees them, the second
doesn't), steps the physics (`Vehicle::Step`, `sub_820B6330`) and calls
`sub_820C1B98`. Then it calls `sub_820C2208` (the odometer, which reads
1/30) once.

`Vehicle::Step` stores the step length at car+0x930 and its inverse at
car+0x934 (`sub_820AF9D0`), then runs the car's physics parts and an inner
loop of car+0x940 iterations (10, set when the car is created in
`sub_820B0DD0`). Checked 2026-10-23 in the generated code: nine parts under
it read +0x930, none reads +0x934 or +0x938 (60.0 from creation), so the
physics scales with the step length. The one exception is `sub_820B1678`,
which counts steps (a ramp to 30). `Vehicle::ApplyControls` scales its rates
by `[0x825F17E8]`, not by the step it's given.

The control records: a table of pointers at `0x826B5810`, one per car index.
The race modes (`sub_82163CC8`, `sub_82164188`, `sub_82171950`) build a
car's record on the stack and copy its 5 words into the table's record just
before calling `Vehicle::Tick`. The player's record comes from
`PlayerControls::Build` (`sub_8215DBF8`), which reads the action mappers:
pads (`Input::Update`) -> action mappers (`ActionMappers::Update`, from
`sub_82107A38`) -> `Input::QueryAbstract` / `Input::Query` -> the record.

## Subtick

Frames between the game's 30 Hz ticks, with the cars' input sampled
between ticks and the cars' drawn matrices blended between ticks. Game
logic and car physics keep their 30 Hz tick, so the original replays stay
compatible. `src/subtick/`, all behind `--subtick` (on by default). The only
other switches are `--max_fps`, `--subtick_input` (on with it),
and `--subtick_input_sensitivity`; everything else here is always on with `--subtick`:

- **Passes** (`renderer_hooks.cpp`, `frame_pacer.cpp`,
  `game_messages.cpp`): after `Renderer::WaitForFrame`, the pacer picks the
  coming main loop pass. When the next 30 Hz tick is due it's a logic pass;
  otherwise, in races, it waits for the next frame slot (`--max_fps`, 60 by
  default, 0 = unlimited) and the pass is render-only: `GameMessages::Dispatch`
  consumes Update messages without delivering them (main thread only), so
  the pass draws the same state again. Menus stay at 30 fps (their state
  tables tick on message 1 and draw on message 0; see "Menus" below). A pass is in a race when the last
  logic pass ran `Race::DrawStep`. The present's 2-vblank wait is dropped.
- **Car input** (`input.cpp`, `input_weights.cpp`, `--subtick_input`):
  the player's record gets one input for the tick: the samples taken over
  the last tick's interval, weighted as a whole (all five axes together) by
  `Input_Aggregate`. The weights are (1 - s) x [newest] + s x ([newest] +
  biggest change) / 2, normalized, where the change is a sample's move from
  the one before it over all axes, relative to the largest in the interval.
  The first sample (last tick's) gets no weight. `--subtick_input_sensitivity`
  (s, 0.0 to 1.0): 0 is the tick's own input only, as the game reads it; 1
  is half newest, half the biggest changes (flicks and brake blips count).
  The presence bits come from the newest sample. Physics is unchanged.
  Substepping the physics (N steps a tick) was tried and broke the car's feel
  from 4 steps up (the physics assumes its step count), so it was removed.
- **Input** (`input.cpp`, `--subtick_input`, on with `--subtick`):
  `Input::Update` is skipped and `ActionMappers::Update` runs once a tick,
  so presses, menu repeats and the input
  listeners count ticks. At the start of each render-only pass in a race,
  the pads are read again (`PhysicalPad::Read` with the flags and rumble
  timers put back, `LogicalPad::Build`) and the mappers updated with their
  digital state put back (+448..+471, +1852..+2451 and the 16 bytes of each
  repeat object), so only the analog values move. The record's five axes
  are then worked out as `PlayerControls::Build` does (the same integer
  curves and dead zones) and kept as a sample. The tick's own record is the
  last sample; the last one carries over as the next tick's first.
  The weighted input (see "Car input") is written into the record by the
  `PlayerControls::Build` override, right as it's built, before the race
  mode copies it anywhere. The race mode's caller (`sub_8215E390`) then
  hands the record to the replay recorder (`sub_82102B30`) or, playing a
  replay, replaces it from the replay (`sub_82102BB0`). So a replay stores
  exactly the input the physics used and plays back 1:1. (Before, the
  weighted input was swapped in inside `Vehicle::Tick` only, so replays
  stored the tick's own input and played back "sloppy".) `Vehicle::Tick` is
  the game's own. AI cars and traffic get no samples and run as the game
  does.
- **Cars** (`car_transforms.cpp`): the world matrix at car+16 is set once
  a tick. `Car::DrawUpdate` (`sub_820DA930`, once per view) records it as
  the tick's pose on logic passes, once per car per tick (the pacer's tick
  count). Before `Race::DrawStep` (`sub_82159540`) on every pass, every car
  recorded this tick is blended between its last two poses by the frame's
  progress, and put back after the step if it still holds the blend. On a
  logic pass, cars whose matrix already holds the new pose are recorded
  first; the game sets the rest during the draw, so `Car::DrawUpdate` also
  writes the blend for its own draw when the matrix isn't blended yet.
  Blended: position and rotation rows (each row keeps its length). Only
  consecutive ticks blend; over 8 m in a tick is a cut (a traffic car
  recycled elsewhere). Wheel spin and steering still step per tick.
- **Main camera** (`car_transforms.cpp`): camera pass 0 (array at
  `0x826AA8E0`, 592 bytes a pass, as in ITC) holds the eye (+400), target
  (+416), up (+432) and roll (+448). `CameraPass::Update` (`sub_820A3B98`)
  builds the view (+16) and view-projection (+144 = view x projection +80)
  from them, several times a Draw. On pass 0 in a race each call is matched
  by its order within the pass; the logic pass records what each is given,
  and on every pass a call given exactly the tick's values builds from the
  blend. Calls given other values are left alone. A cut isn't blended:
  - **The game's own switches** (`src/subtick/camera_cuts.cpp`,
    `--subtick_camera_cuts`): `RaceCamera::SetMode` (every call starts the
    mode's camera again: the view button, the race start, a restore, the
    intros' ends), the chase camera's reset (its +380 set when its update
    runs), a TV camera change (the zone +360 or the kind +448 of the TV
    cameras changed by their update), and a scripted camera's shot change
    (its shot +360 or animation +352 changed, or a camera picked with
    `sub_82153BE0`/`sub_82153CA8`; reported at its next update, which is
    when the new shot's first frame is written). Each switch is counted;
    every camera pass's pose takes a new count as a cut at its next record,
    so a pass updated before the switch in its tick is cut on the tick its
    new values show (`CarTransforms_GameCameraCut`).
  - Not reported yet: mode 3 and the garage's presets (blended across
    their switches). A motion test (the eye or the target breaking from its
    motion) was the fallback until 2026-11-18; dropped: it cut where nothing
    switched (unpausing at speed made the car jitter for a tick) and didn't
    catch the garage's switches.
  - `--log_camera_cuts` logs each cut and what made it.

  The blend stays in the pass for the rest of the frame and the
  tick's values go back when the next pass starts
  (`CarTransforms_EndFrame`, before any game logic). The draw reads the eye
  and target besides the view: the camera position the cars' paint
  reflections are worked out against (`0x8264EA40`, copied from the eye per
  view by `sub_820C5BA0` and by `sub_820F3398`, and passed in each part's
  upload, c8, by `sub_8257ACD0`), and the direction the light glows'
  billboards face (`sub_8209C800`). Put back right after each call, those
  got the tick's eye against a blended view: the reflections and the head
  and tail light glows shook while the geometry didn't.
- **`--camera_fov`** (`car_transforms.cpp`, `CarTransforms_CameraFov`, called in the `sub_820A3B98` wrapper after the game's update, with or without `--subtick`): scales pass 0's projection, P00 (+80) and P11 (+100, the row-major diagonal), by one factor, so P11 = cot(fov / 2) and the aspect is kept. Then view-projection (+144) is rebuilt as view (+16) times projection, as `sub_8209BE70` does. Values 10 to 170 degrees; 0 is the game's own. The game's values are kept and put back when the flag is 0, and taken again when the game writes the projection. Before the update, the projection was not used: the update calls a virtual method on the pass (slot 0 of its vtable, `f1` = the constant at `0x82000A24`, `r4`/`r5` = +504/+508) before it builds the view and the view-projection, so a change made earlier can be overwritten. Not yet checked in the game; the recorder sometimes draws from a stack copy of the camera (see "Recorder"), so the race world may not follow pass 0.
- **Cars and the view-projection** (`CarTransforms_RebuildCamera`): the
  cars' parts are drawn with pass 0's +144 as it is when `Car::DrawUpdate`
  runs (the part draw multiplies it with car+16 into the frame's draw
  packets). `Race::DrawStep` draws the cars (`sub_820C5BA0`) before its own
  camera updates (`sub_820F5F48`). On a logic pass the Update message has
  already run `CameraPass::Update` on pass 0 for the frame (5 calls against
  4 on render-only passes), but render-only passes skip Update, so the cars
  were drawn with the previous frame's camera while the world used the new
  one (every car jumped ahead and back, ABAB, with the world smooth). On
  render-only passes in a race, `CameraPass::Update` now runs on pass 0
  just before `Race::DrawStep`.
- **One progress per frame** (`FramePacer::FrameProgress`): the blend's
  progress through the tick is taken once, when the pass starts, and used
  for everything blended in that frame. Read at each use, a logic pass built
  its camera early in Update and drew the cars ~10 ms later at a larger
  progress, so the cars were a few cm out of step with the camera on logic
  frames (plain in the cockpit view).
- **Per-frame logic** (`per_frame_logic.cpp`): what runs from Draw and
  counts passes is kept to once a tick on render-only passes: blink timers,
  texture animations, the minimap blink clock, play time, UI movies and the
  boost value (replayed from the logic pass's state). The course models'
  animations (`ModelAnimations::Apply`, `sub_8257A460`, from
  `Scenery::Draw`: the scrolling textures; the flipbooks are
  `TextureAnimations::Advance`) stepped one frame each pass, so they ran
  faster with the frame rate (John's test, 2026-11-11). On render-only
  passes they're applied at the logic pass's time plus the frame's
  progress, and the times put back: they step once a tick and move smoothly
  between. Works (John's test, 2026-11-11).
- **Race overlays** (`per_frame_logic.cpp`): the marker above opponents,
  the tire smoke and the light trails behind head and tail lights,
  world-space sprites recorded with a camera pass's view-projection (+144)
  as it is when they record. The game records them during Update
  (`RaceOverlays::Run`, `sub_8215BE00`, from handler `sub_8215C0A0`), which
  render-only passes skip; they used to be recorded on render-only passes
  with the logic pass's cameras, so they were drawn with an older camera
  than the world and shook against it. Now the update (vtable slot 4) runs
  in Update on logic passes and the recording (slot 5) on every pass just
  before `Race::DrawStep`, once pass 0 holds the frame's camera
  (`PerFrameLogic_RecordRaceOverlays`; the other passes as the update saw
  them). A logic pass records if the overlays were updated on the latest
  tick or the one before (its update can come after its draw step), the one
  before only if it was an update, not the paused recording (the results
  menu: the logic pass resuming a free run drew the destroyed health gauge
  again for one tick, John's test, 2026-11-11; still there in his next
  test); a
  render-only pass only if they were updated on the latest, so once they
  stop (the race over: the rival's marker and gauge) the frames between
  ticks stop drawing them at once. While paused the overlays aren't updated: the paused Update
  handler `sub_8215C0A8` only records them (`RaceOverlays::Record`,
  `sub_8215BE80`), so they were drawn on logic frames and not between them
  (the marker and the tire smoke flickering every other frame). That
  recording is moved before `Race::DrawStep` on every pass too.
- **World-space sprite corners** (`per_frame_logic.cpp`, ported from
  ITC-Recomp): a sprite's four corners
  (16 floats at +80) are written once a tick, so with the camera blended
  they ran ahead of their car early in each tick. The two recorders,
  `sub_821281B0` (sprite lists, the light trails; count at +0x109F0, sprite
  i at +80 + 64i) and `sub_82127C08` (single sprites, the marker), blend
  the corners between their last two ticks by the frame's progress while
  they record, then put them back. Lists are matched by list and order in
  the pass, single sprites by sprite, caller and order. A corner that moved
  over 10 units in a tick is a new sprite. The list
  follows ITC-Recomp's findings for the same game; more may turn up.
- **Opponent marker** (`per_frame_logic.cpp`, `sub_8212BDA0`): its record
  builds the sprite each call from a centre set once a tick (overlay
  +176..+184) and axes facing the camera's eye. With the camera blended the
  axes, and so the corners, change every frame, so the corner blending
  above never matched it and the centre stepped once a tick against the
  blended car. The centre is blended between its last two ticks instead.

- **Menus** (with `--subtick`; the `--subtick_menus` flag was removed once it
  worked, John's tests 2026-11-11; `renderer_hooks.cpp`, `game_messages.cpp`, `per_frame_logic.cpp`):
  render-only passes outside races too, paced as in races. The dispatcher's
  handlers are all state tables (`StateTable::Dispatch`) keyed by
  (state, message id). Messages 0 and 1 are posted every pass, the others
  are events. The race updates on 0 and draws on 1 (`Race::DrawStep` is
  handler `sub_8215A778`, state 38, message 1; the HUD's `RaceHud::RunMovie`
  callers too). The menus' tables use 1 for their tick: set-up and loading
  (`VirtualAlloc`), timers, sounds (the title's `sub_82221640`), and 0 for
  their UI movies with the logic around them (the garage top: 0
  `sub_8218E400`, 1 `sub_8218E0A0`; the title: 0 `sub_82221C08`, 1
  `sub_822215A0`). The first version skipped 0 and delivered 1 as in races:
  black frames between ticks, the garage's scrolling text too fast, crashes
  on mode changes (John's test, 2026-11-11). Now a render-only pass in the
  menus delivers no message. It consumes the pass's own 0 and 1 (the next
  logic pass posts its own) and leaves any event queued for the next logic
  pass. In their place, the top-level UI movie updates the last logic pass's
  messages made are drawn again, from the state each had then, at the time
  between (UI movie animations, below). Render-only passes start only
  once the scene's mode (`Scene::Draw`'s +64) has been the same for two
  logic passes out of a race, so a mode change sets up on ticks.
  `--log_render_only_draws` (native renderer) logs a render-only frame
  whose draw count differs from the logic frame's: a screen drawing
  something other than UI movies in its messages. John's first run
  (2026-11-11) showed several (2 draws against 86, 112 against 349). The
  garage's 3D (`GarageStage::Step`, `sub_8217DD48`, called from the garage
  top's message 0 `sub_8218E400` and the shops') is one: it's noted with the
  UI movie updates, in order, and drawn again, from the stage as it was before the logic pass's call (6208
  bytes, put back after). Its camera calls are blended as in races, counted
  only within the stage's steps, and the car is blended by
  `Car::DrawUpdate`. John's test (2026-11-11): the garage's 3D is right,
  but the door and hood sounds played on every redraw until the next tick,
  so no sound is started while redrawing (`AudioManager::PlayCue`; the
  first try hooked the wrong vtable slot and didn't stop it). The parking
  area's 3D drew on ticks only: its handler (`sub_8222C840`, found with a
  diagnostic logging where 3D objects were recorded from out of a race,
  since removed) draws it itself, so its scene and cars are noted and drawn
  again too (`ParkingArea::DrawScene`, `sub_82225DE0`). The
  garage's scrolling text isn't moved by its UI movie: the movie's text draw
  steps a per-text frame count (`TickerText::Step`, `sub_8211BB10`) after
  each draw, so it scrolled once a frame with the UI redrawn. The step is
  skipped between ticks, and the x taken between the last two counts
  (`TickerText::Offset`, `sub_8211B9D0`; works, John's test 2026-11-11). A first try, moving queued text that moved steadily, didn't
  change it and is gone. Garage sub-menu openings (to the course select or
  the tuning shop, not mode changes) sometimes hung until `--subtick_menus`
  (then a flag)
  or `--subtick` was toggled (John's test, 2026-11-11, at 90 fps too). The
  redraw calls what the logic pass's messages called, and those can post
  messages (an animation ending, a menu stepping on); posted again, the next
  logic pass took them twice. The queue is now put back after the redraw
  (`--log_message_handlers` logs when the redraw changed it). That wasn't
  it: John's next run (log 008) never logged it, and the hangs came at the
  same place each time: the garage top's close (message 5, `sub_8218C350`)
  then nothing until he toggled a flag, when its "closed" step (state 8,
  message 3, `sub_8218C3D0`) ran at once; the tuning shop's close the same.
  A closing menu waits in a state whose only entry is message 3, and with
  frames between ticks that message sometimes never came (what posts it
  isn't found: something the close starts, likely the UI movie's closing
  animation). Now, while any handler waits in such a state (checked after
  each logic pass from its state table: entries 48 bytes, key state +0 and
  message +4, count +9600, state +9604), the menus run at 30 Hz, as for a
  mode change; over 10 s in one is taken as resting there.
  `--log_message_handlers` logs when a wait starts and ends. Works (John's
  test, 2026-11-11, log 009): every wait ended on the next tick (2 passes),
  so message 3 comes at once without the frames between ticks; the drop to
  30 Hz isn't noticeable (the screen is black then). What on those frames
  held it back isn't known.
  `--log_message_handlers`
  logs each handler with its message id (it logged the local copy's address
  before, so every handler showed as Update).
- **UI movies in races, frames between ticks** (`per_frame_logic.cpp`): a
  render-only pass draws only the UI movie updates the tick's logic pass
  made (by movie and order; draw-only updates too). The HUD was drawn for a
  frame after a race's result took it down (race to free run, John's
  tests 2026-11-11): drawn on a frame between ticks with no logic pass's
  draw behind it. Works (John's test, 2026-11-11).
- **UI movie animations** (`per_frame_logic.cpp`; was `--subtick_ui_blend`,
  removed once it worked, John's tests 2026-11-11): on a render-only pass, a UI movie
  replayed from its state before the logic pass's update has its running
  animations' times moved on by 2 (the update's step a tick, in 60ths of a
  second) times the frame's progress, so menu slides and HUD animations
  move between ticks. Within each animation's length; a looping one isn't
  wrapped early. Anything an animation triggers at a key (a sound, a
  message) would happen twice: sounds and messages started during the
  menus' redraw are dropped (above).

Not done yet: wheel spin and steering change once a tick; the lights on
car models (headlight and tail light sources, not the glow billboards) are
unchecked and may need the same camera-position fix as the paint
reflections; UI movie tracks that add to values
(ITC-Recomp's pause sub-menu arrow fix) aren't ported.

## Native game code

The functions `docs/native-code.md` rewrites as our own C++, by flag. With
a flag off, each override calls the recompiled function. The car specs'
other functions (`Params::GetCarSpec`, `Car::Refresh` and the rest) are in
the first table, by the cars' other functions.

### `native_framework`

The getters are shared by the linker across classes (one function for
every class with a word at that offset), so they're named by offset.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82090840` |  | Overridden | Getter: r3's byte at +133 (the framework's flag `App::Run` reads). |
| `sub_82090848` |  | Overridden | Getter: r3's byte at +134. |
| `sub_82467570` |  | Overridden | Getter: r3's word at +60. |
| `sub_82090850` |  | Overridden | Getter: r3's word at +64. |
| `sub_82090858` |  | Overridden | Getter: r3's word at +80. |
| `sub_82472750` |  | Overridden | Getter: r3's word at +68. |
| `sub_8246CD28` |  | Overridden | Getter: r3's word at +44. |
| `sub_82090908` |  | Overridden | Thunk, the +36 interface's slot 1: the input manager (r3 - 12, its word at +64). |
| `sub_82090900` |  | Overridden | Thunk, the +52 interface's slot 2: the renderer (r3 - 44, +68). |
| `sub_82090910` |  | Overridden | Thunk, the +52 interface's slot 1: the worker pool (r3 - 28, +44). |
| `sub_820908B8` |  | Overridden | Thunk, the +24 interface's slot 4 (r3 - 16, +68). |
| `sub_82090938` |  | Overridden | Thunk, the +52 interface's slot 3: the scene (r3 - 32, +60). |
| `sub_820909B0` |  | Overridden | Thunk, the +36 interface's slot 2: the renderer (r3 - 28, +68). |
| `sub_820909B8` |  | Overridden | Thunk, the +36 interface's slot 4: the worker pool (r3 - 12, +44). |
| `sub_820909D8` |  | Overridden | Thunk, the +36 interface's slot 3: the scene (r3 - 16, +60). |
| `sub_82091120` | `DebugText::SetPosition` | Overridden | Vtable slot 2: r4, r5 the x and y words' addresses; sets the debug font's cursor and line start (`0x8296E140`), and leaves r3, r4 = x, y and r11 as the original does. |
| `sub_82571988` | `DebugFont::SetPosition` | Overridden | r3, r4 = x, y: the debug font's cursor and line start. |

### `native_menu_sounds`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82110318` | `MenuSound::Play` | Overridden | r3 the cue: plays it from bank 0 at volume 1.0 (`Sound::Play`'s three arguments by address, in a 112-byte frame). |
| `sub_82173710` | `Sound::Play` | Overridden | r3 the bank's address, r4 the cue's: the sound manager's 2D play slot at volume 1.0; the slot call returns to `0x8217374C`. |
| `sub_821B80C8` | `SoundOptions::SetEffectsVolume` | Overridden | r3 the setting (0-15): sets the effects category's volume and its two globals. |
| `sub_821B8198` | `SoundOptions::SetMusicVolume` | Overridden | r3 the setting (0-15): sets the music category's volume and its global. |

### `native_car_specs`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820C9B78` | `Params::Exists` | Overridden | 1 when the car parameters are loaded. |
| `sub_820C9C80` | `Params::FirstWheelOfModel` | Overridden | r3 a wheel model: its first record + 1, or 0. |
| `sub_820C9B90` |  | Overridden | Load callback for BUILD 74 to 82 (the aero parts): nothing to swap (their getters swap a copy); counts the load down. |
| `sub_820CBB30` |  | Overridden | Load callback for BUILD 83: swaps its 40-byte records. |
| `sub_820CBE48` |  | Overridden | Load callback for BUILD 73 (the wheels): the wheel count, each wheel swapped, each record's model (+40) up by one, and each model's first record at params +64 + 4 x model. |
| `sub_820CB5E8` |  | Overridden | Aero part getter for BUILD 74: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB670` |  | Overridden | Aero part getter for BUILD 75: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB6F8` |  | Overridden | Aero part getter for BUILD 76: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB780` |  | Overridden | Aero part getter for BUILD 77: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB808` |  | Overridden | Aero part getter for BUILD 78: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB890` |  | Overridden | Aero part getter for BUILD 79: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB918` |  | Overridden | Aero part getter for BUILD 80: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CB9A0` |  | Overridden | Aero part getter for BUILD 81: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CBA30` |  | Overridden | Aero part getter for BUILD 82: r3 the output (left in r3), r4 the parts (9 pointers), r5 the level; copies the record and swaps the copy. Called from `Car::ApplyAero`. |
| `sub_820CBAC0` |  | Overridden | r3 the parts: how many of BUILD 81's first two records (the body kits) have a first float other than 0.0. |
| `sub_820C94F0` | `CarSpec::SwapVector` | Overridden | Three words swapped, the fourth set to 1.0. |
| `sub_820C9700` | `Params::Init` | Overridden | The parameter object (operator new, 468 bytes) into [0x826B2B28], no wheel model yet (-1s), then two passes over BUILD 72-83: their sizes (TOC), the block (operator new[] of the total), each table's place in it; the loads pending counted. |
| `sub_820C9B30` | `Params::Free` | Overridden | The block (operator delete[]) and the object (operator delete) freed, [0x826B2B28] = 0. |
| `sub_820C9CB0` | `CarSpec::SwapMatrix` | Overridden | 16 words swapped. |
| `sub_820C9F38` | `CarSpec::SwapBlock1At368` | Overridden | Block 1 +368: 40 words (the gear ratios among them). |
| `sub_820CA098` | `CarSpec::SwapBlock1At264` | Overridden | Block 1 +264: two runs of three words and 15 words; r3 its +16. |
| `sub_820CA220` | `CarSpec::Swap3` | Overridden | 3 words swapped. |
| `sub_820CA2A0` | `CarSpec::SwapBlock1` | Overridden | Block 1's swap (five vectors with w = 1.0, two 16-word runs, the words from +224); r3 block 1 + 368. |
| `sub_820CA780` | `EngineStage::Swap` | Overridden | A 204-byte engine stage's words from +8 swapped. |
| `sub_820CAA48` | `Tire::Swap` | Overridden | A 92-byte tire's floats from +8 swapped. |
| `sub_820CACC0` | `CarSpec::SwapBlock2At8` | Overridden | Block 2 +8: five words and +56 swapped, the nine floats between set to 0.0. |
| `sub_820CAD90` | `CarSpec::Swap5` | Overridden | 5 words swapped. |
| `sub_820CAE60` | `CarSpec::SwapBlock2At132` | Overridden | Block 2 +132: 42 words swapped. |
| `sub_820CB150` | `CarSpec::Swap4` | Overridden | 4 words swapped. |
| `sub_820CB2D0` | `Wheel::SwapColor` | Overridden | A wheel's 12-byte color record: two halfwords and a word. |
| `sub_820CB328` | `Wheel::Swap` | Overridden | A 48-byte wheel record: its two colors and four words; r3 the second color. |
| `sub_820CB3F8` | `Params::SwapTable83Record` | Overridden | 10 words of a BUILD 83 record swapped. |
| `sub_820CB588` | `AeroPart::Swap36` | Overridden | `AeroPart::Swap`, then the words at +28 and +32 (BUILD 81 and 82). |
| `sub_820CBBB0` | `Params::SwapWheels` | Overridden | `Wheel::Swap` on each wheel record (+60 of them). |
| `sub_820CBC10` | `CarSpec::SwapBlock2` | Overridden | Block 2's swap (its bytes and model name kept); r3 block 2 + 300. |
| `sub_820CBF08` | `CarSpec::Swap` | Overridden | A spec's byte swap: block 1, block 2, 16 engine stages, 21 front and rear tires, the paints, the stock wheel; r3 the stock wheel's second color. |
| `sub_820CBF90` | `Params::SwapCarSpecs` | Overridden | `CarSpec::Swap` on each of the specs (count at +56). |

### `native_menu_helpers`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82184468` | `MenuCursor::Update` | Overridden | Moves the menu cursor from the player's input, with the repeat while a direction is held. The action mapper it reads stays recompiled. |
| `sub_821843C8` | `MenuCursor::Pressed` | Overridden | Whether the player's entry is pressed this tick, from the action mapper's slots. |

### `native_race_hud`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82211F50` | `RaceHud::RunMovie` | Overridden | Runs the UI movie for the race HUD's state: the countdown, the timer, the race-end overlays. The movies stay recompiled. |

### `native_data_tables`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82116F18` | `DataTables::Init` | Overridden | Sets up the item records and the tables' blocks. |
| `sub_821173A8` | `DataTables::Load` | Overridden | Runs the twelve loads into the tables Init set up. |
| `sub_82117240` |  | Overridden | Load callback for asset 94: the item count (400) and the loads pending. |
| `sub_82117228` |  | Overridden | Load callback for asset 84: nothing to swap. |
| `sub_82117258` |  | Overridden | Swap callback for the eight 56-byte tables. |
| `sub_82117158` |  | Overridden | Swap callback for asset 95's 20-byte table. |
| `sub_821171C0` |  | Overridden | Swap callback for asset 96's 40-byte table. |
| `sub_82111A70` |  | Overridden | Byte swap of one data-table entry on its own. |
| `sub_82111AC0` |  | Overridden | Byte swap of one data-table entry on its own. |
| `sub_82112A30` |  | Overridden | Byte swap of one data-table entry on its own. |
| `sub_821172C0` | `DataTables::IsLoading` | Overridden | 1 while loads are pending. |
| `sub_821172E0` | `DataTables::Item` | Overridden | Returns the item's record. |
| `sub_82116F10` | `ItemRecords::Item` | Overridden | Returns the item's record; the item is in r4. |
| `sub_821172F8` | `DataTables::Entry56` | Overridden | An entry of one of the eight 56-byte tables. |
| `sub_82117318` | `DataTables::Find20` | Overridden | The index of the first matching entry in asset 95's table, or -1. |
| `sub_82117378` |  | Overridden | Entry lookup in the 20-byte table (Entry20) or the 40-byte table (Entry40). |
| `sub_82117390` |  | Overridden | Entry lookup in the 20-byte table (Entry20) or the 40-byte table (Entry40). |
| `sub_820C9578` | `SwapWord` | Overridden | r3's word's four bytes reversed in place (the 360's lfs/stfs round trip, signaling NaNs unchanged). |

### `native_file_layer`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82572488` | `File::File` | Overridden | Constructor: sets the object up and returns it in r3. |
| `sub_82572130` | `File::~File` | Overridden | Destructor. |
| `sub_82579070` | `AsyncFile::AsyncFile` | Overridden | Constructor. |
| `sub_825790B0` | `AsyncFile::~AsyncFile` | Overridden | Destructor. |
| `sub_820A69B8` | `GameFile::GameFile` | Overridden | Constructor. |
| `sub_820A69F8` | `GameFile::~GameFile` | Overridden | Destructor. |
| `sub_825791D8` | `Path::Path` | Overridden | Constructor. |
| `sub_825791F8` | `Path::~Path` | Overridden | Destructor; writes the vtable again. |
| `sub_820A6B98` | `GamePath::GamePath` | Overridden | Constructor, also used as its destructor. |
| `sub_82579250` | `Path::Build` | Overridden | Builds a path from the path (r3), the kind (r4) and the name (r5). |
| `sub_820A6BA8` | `GamePath::SetGame` | Overridden | Sets the game's folder name in the path; a name of 0 isn't checked for. |
| `sub_820A6A08` | `GameFile::OpenWrite` | Overridden | By name: the name made a plain path (a Path in its 640-byte frame), File::OpenWrite (`sub_82572140`) on it; 1 when it opened. |
| `sub_820A6AD8` | `GameFile::OpenAsync` | Overridden | As OpenWrite, with AsyncFile's open (`sub_825790C0`). |
| `sub_820A6B40` | `GameFile` deleting destructor | Overridden | The destructor, then operator delete with bit 0 of r4; returns r3. |
| `sub_820A6C20` | `GamePath::Set` | Overridden | As SetGame with the second plain path kind (1). |
| `sub_820A6C98` | `GamePath` deleting destructor | Overridden | As Path's, with GamePath's vtable. |
| `sub_82579160` | `AsyncFile::Detach` | Overridden | Returns the handle (+4) and sets it to -1, without closing it. |
| `sub_82579178` | `AsyncFile` deleting destructor | Overridden | As GameFile's. |
| `sub_825791F0` | `Path::Text` | Overridden | Returns r3 + 4, the text. |
| `sub_82579208` | `Path` deleting destructor | Overridden | The vtable written again, then operator delete with bit 0 of r4. |

### `native_track_zones`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82151288` |  | Overridden | List Link (List16, List64 or List128): the position's low halfword is linked in. |
| `sub_820FE150` |  | Overridden | List Link (List16, List64 or List128). |
| `sub_82288988` |  | Overridden | List Link (List16, List64 or List128). |
| `sub_820FE1B8` |  | Overridden | List Insert (List16, List64 or List128). |
| `sub_82289000` |  | Overridden | List Insert (List16, List64 or List128). |
| `sub_820FDE20` | `RouteRange::Init` | Overridden | Takes the start and the end, empties both lists, and sets the state and +1060 to 1. |
| `sub_820FE750` |  | Overridden | Stores a position in the caller's frame and inserts it into a list. |
| `sub_820FDD60` | `RouteRange::ToNeighbor` | Overridden | Writes the position out for a neighboring section. |
| `sub_820FDEE8` |  | Overridden | A RouteRange test (WithinEnds, OnPath, Test or Update); the position's address is in r4. |
| `sub_820FE220` |  | Overridden | A RouteRange test (WithinEnds, OnPath, Test or Update); the position's address is in r4. |
| `sub_820FE580` |  | Overridden | A RouteRange test (WithinEnds, OnPath, Test or Update); the position's address is in r4. |
| `sub_820FE700` |  | Overridden | A RouteRange member; not yet matched to its name. |
| `sub_82288928` | `Zone::Reset` | Overridden | Empties the zone's range list, then sets the index, the flag (0) and the vehicle. |
| `sub_822888E0` | `Zone::IsInside` | Overridden | Whether the zone holds the position. |
| `sub_82288F90` | `ZoneRanges::Insert` | Overridden | Links a range into the list and copies it into the new node's slot. |
| `sub_82289068` |  | Overridden | A zone's answer: the last range's, or the copy's with no ranges. |
| `sub_820C94B8` | `Racers::Vehicle` | Overridden | The racer's vehicle, or 0 when there's no racer or its flag 0x40 isn't set. |
| `sub_822888E8` | `TrackZones::Destroy` | Overridden | Deletes the owner when there is one, and clears it. |
| `sub_822891B8` | `TrackZones::IsInside` | Overridden | The zone's IsInside, by its place in the list. |

### `native_race_scene`

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8215B628` | `RaceScene::RaceScene` | Overridden | Constructor: sets up the scene's light sets and post-effect values. |
| `sub_8215B698` | `RaceScene::Create` | Overridden | Makes the scene, or 0 when the allocation fails. |
| `sub_8215B4A8` | `RaceScene::DestroyCars` | Overridden | Frees the cars' records from the scene's +16. |
| `sub_8215B6F0` | `RaceScene::Destroy` | Overridden | Frees the cars' records and deletes the scene. |
| `sub_8215A370` | `RaceLights::InitEntry` | Overridden | Sets up one light entry, by its index. |
| `sub_8215AF70` | `RaceLights::SetUp` | Overridden | Sets up the light entries and counts the two kinds. |
| `sub_8228C498` | `Glows::SetParams` | Overridden | Sets the 11 glow floats. |
| `sub_8228C9A8` | `RaceScene::SetEffectParams` | Overridden | Sets the color filters' parameters; split screen uses filters 6 and 10, else filter 2. |
| `sub_8215A758` | `RaceSceneMachine::TogglePause` | Overridden | Toggles pause and the post effects' pause state on the scene. |
| `sub_8215B0D0` | `RaceSceneMachine::Resume` | Overridden | Clears the resume flag (+9608) and runs the post effects' pause toggle when it was set. |

### `native_shader_objects`

The graphics library's shader objects. A file's object is 592 bytes when its magic's low 7 bits are set (a vertex shader), else 56.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_822A5B08` | `ShaderFile::Parse` | Overridden | r3 the file, r4 a 16-byte record: the file, the object's size, the microcode and its size. |
| `sub_822A5CD0` | `VertexShader::Init` | Overridden | r3 the object (cleared, 592 bytes), r4 the microcode: the fetch count at +60, the fetch words at +68, their constants at +196. |
| `sub_822A5B58` | `PixelShader::Init` | Overridden | r3 the object (cleared, 56 bytes), r4 the microcode at +24. |
| `sub_822A7348` | `VertexShader::CopyHeader` | Overridden | The 592 bytes cleared, the file's header copied after them (XMemCpy). |
| `sub_822A7210` | `PixelShader::CopyHeader` | Overridden | The 56 bytes cleared, the file's header copied after them (XMemCpy). |
| `sub_822A7388` | `VertexShader::Create` | Overridden | r3 the file: the object (XMemAlloc 0x24800000) and its microcode (0xA5800000, a 0x0BADF00D word after it, flushed); the object or 0. |
| `sub_822A7250` | `PixelShader::Create` | Overridden | As the vertex shader's, the microcode XMemAlloc'd 0xB5800000, no flush. |
| `sub_8257A620` | `Mesh::LoadShaders` | Overridden | r3 the model, r4 its shader section: the pairs' objects and microcode (AllocPhysicalEx flags 4 vertex, 0x404 pixel; the vertex microcode copied with XMemCpy, the pixel one with memcpy). |
| `sub_820A08B8` | `Shaders::OnPairLoaded` | Overridden | The archive's callback for a shared pair (r6 the context): the tables at 0x826AF998 (vertex) and 0x826AD978 (pixel), then the context deleted. |

### `native_screen_objects`

The race's screen object: +52 its strength, +76096 its UvMap. The UvMap and UvColumn trees and the rings' set-up stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8212E928` | `ScreenObject::SetSpeed` | Overridden | f1 the strength, clamped to [0.0, 1.0], into +52 (+27 = 1); when running (+24), sub_8212E440 with 0.0 first. |
| `sub_8212DDE0` | `ScreenObject::Release` | Overridden | The vertex declaration (+0) and texture (+4) released with the device locked. |
| `sub_82131B98` | `ScreenObject::~ScreenObject` | Overridden | Release, then UvMap::~UvMap on +76096. |

### `native_scene_models`

The scene models' records and links, on the record types in `game/scene_loads.h` and `SceneModel` in `game/scene_models.h`.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820F0CE0` | `Accessories::Get` | Overridden | [0x826B51F0], the accessories' set. |
| `sub_820D9480` | `SceneModel::Accessory` | Overridden | r3 an interior: the set's entry by its +140 (3: none), + 48, or 0. |
| `sub_82173DA0` | `SceneModel::OnSoundLoaded` | Overridden | The archive's callback for SetUp's two sounds: the slot's byte (0x826C7DF8) up, its word (0x826C7E54) = 1 at 2. |
| `sub_820E6F60` | `SceneWheels::SetRecord` | Overridden | +16776 = r4. |
| `sub_820DC710` | `SetCarModel` | Overridden | +24 = the car model. |
| `sub_820DEF70` | `SceneParts::Link` | Overridden | +24 the car model, +28 its skeleton ([model + 24] + 3760). |
| `sub_820DEF88` | `SceneParts::SetRecord` | Overridden | +37248 = r4. |
| `sub_820E3398` | `SceneInterior::Link` | Overridden | +24 the car model, +28 and +32 its skeleton and node block. |
| `sub_820E3220` |  | Overridden | +11452 = r4. |
| `sub_820D3910` | `CarModel::SetRecord` | Overridden | memcpy of the 3,696-byte car record to +32. |
| `sub_820D3780` | `Record12::Reset` | Overridden | +0 = -1, +4 and +8 the float at 0x82001BD8. |
| `sub_820D07D0` | `Layer::Reset` | Overridden | A 64-byte vinyl layer or sticker record's defaults. |
| `sub_820D0848` | `CarRecord::Reset` | Overridden | The 3,696-byte car record's defaults: its vectors, its 54 layers reset, the layers' kinds and the part order. |
| `sub_820D31A0` | `PartsRecord::Reset` | Overridden | The 60-byte parts record cleared, +52 = 3. |
| `sub_820D3488` | `InteriorRecord::Reset` | Overridden | The 120-byte interior record cleared, then its defaults. |
| `sub_820D3100` | `CockpitRecord::Reset` | Overridden | +0 to +16 cleared, the interior record at +24. |
| `sub_820D3548` | `WheelsRecord::Reset` | Overridden | The 156-byte wheels record's defaults. |
| `sub_820D3128` | `Pool4Record::Reset` | Overridden | The 52-byte record cleared, then 1.0 at nine floats. |
| `sub_820D4250` | `CarRecord::SetW` | Overridden | 1.0 into the car record's rows and layers. |
| `sub_820D94D8` | `SceneModelRecord::Reset` | Overridden | The 4,244-byte record: kind 1, then each part's defaults. |
| `sub_820DAF08` | `SceneModelRecord::SceneModelRecord` | Overridden | CarRecord::SetW on +16, Record12::Reset on +4080 and +4092. |
| `sub_820D7B98` | `CarRecord::Copy` | Overridden | r3 to, r4 from: the fields one by one and a 44-byte memcpy of the part order. |
| `sub_820DAF58` | `SceneModelRecord::Copy` | Overridden | +0, the car record, then the other records (memcpy). |
| `sub_820C3AB0` | `CarModel::AllocPages` | Overridden | VirtualAlloc(r5 bytes) into [r4]. |
| `sub_820D9E48` | `SceneModel::Update` | Overridden | Every frame: the deferred build's countdown (+5044, +5040), then the interior's accessory anchored when there is one. |
| `sub_820E0458` | `SceneParts::Apply` | Overridden | Unless its record is kind 1, r4 copied to +108 (60 bytes) and the parts rebuilt (sub_820E0188). |
| `sub_820E3E20` | `SceneInterior::Apply` | Overridden | r4 (120 bytes) copied to +40 and the cockpit's +28, the six lights' +452, then sub_820E3500. |
| `sub_820D9A10` | `SceneModel::ReloadDecals` | Overridden | With +5036 set (not kind 3): the car record set from +48 and the decals rebuilt. |
| `sub_820D9AC8` | `SceneModel::AddSecondWheels` | Overridden | Once (+5032): the scene's pool 5 entry made and linked; 1, or 0 when done before. |
| `sub_820C58E0` | `ScenePools::SetUpB` | Overridden | The car scene's pool r4 object r5 (List::At on +4896 + 12 x pool); 0 unless in use (+16 = 3); set up from r6 by the pool (0 to 7; the wheels' set-up a guest call). Returns the object. |
| `sub_820D6668` | `Pool4Object::OnLoaded` | Overridden | When its slot's request is r3: Model::Load of r4 into +112, the buffer released, state 3. |
| `sub_820D67F8` | `Pool4Object::Load` | Overridden | The record to +28 (52); asset 1115 + 226 x block (set A) or 11256 + 197 x block into the slot at +80 (OnLoaded). |
| `sub_820D68C0` | `CarParts::OnPartLoadedA` | Overridden | Archive::Load's callback (r3 the request, r6 the parts): the matching slot of 216's model loaded, its buffer released, state 3. |
| `sub_820D6938` | `CarParts::OnPartLoadedB` | Overridden | As OnPartLoadedA over 17 slots. |
| `sub_820D6D40` | `CarParts::LoadSetA` | Overridden | Each of 216 slots loads asset 893 + 226 x block + i (LoadSlot::LoadShared, OnPartLoadedA). |
| `sub_820D9538` | `SceneModel::Build` | Overridden | Traffic: no car model (-1), no cockpit, its body (pool 6) and wheels. Others: car model, parts, interior, lights, wheels; a cockpit for kind 2 in a scene with cockpits (its record from the parts words, +5 and +7 adjusted), else +5080 = 1. |
| `sub_820DA420` | `SceneModel::ApplyRecord` | Overridden | Once SceneModel::LoadsDone (`sub_820D9FE0`): block 5's choice; the car record to the car model; scene +4 0: the parts and interior applied in place; else new ones made and the old released. Kind 2 in a cockpit scene: the cockpit made again. |
| `sub_820DB0C8` | `SceneModel::SetUp` | Overridden | +5024 = 1, the record copied, the wheels record's kind; block 5's interior choice; +4916 = 0.3 or 0.1 by the scene's +4; in a race scene outside mode 5 ([0x825F3264]) its slot's (+20) engine sounds cleared and loaded (0x826062A0's ids by block and variant); then built now, or held (+5040) while +5044 counts. |
| `sub_820DB488` | `SceneCarModel::SetUp` | Overridden | +3728 = the car scene; CarRecord::Copy to +32; the first set's pool 0 claimed and set up from it into +24. |
| `sub_820DBAF0` | `SceneCockpit::SetUp` | Overridden | The record to +36 (144); the first set's cockpit body (pool 2) into +32 and set up; its interior (car scene pool 3) into +28, marked the cockpit's (+11656, +11612), linked, its +28 and +32 the body's +208 and +496; SetUpB of it with +60. |
| `sub_820DC718` | `ScenePool4::SetUp` | Overridden | As SceneParts::SetUp with pool 4: the record (52) to +32, the object in +28. |
| `sub_820DEF98` | `SceneParts::SetUp` | Overridden | The record to +108 (memcpy, 60); the first set's pool 1 claimed into +104, given the car model, set up from +108. |
| `sub_820E33B8` | `SceneInterior::SetUp` | Overridden | +11668 = the car scene; the record to +40 (120); the first set's pool 3 into +36 (its +3088 = 1 for a cockpit's), given the car model, set up. |
| `sub_820E5588` | `SceneTraffic::SetUp` | Overridden | +9796 = the car scene; the record to +32 (3,696); the first set's pool 6 into +9760, set up from the caller's record. |
| `sub_820D6DA0` | `CarParts::LoadSetB` | Overridden | Set B's 17 slots from +96: asset 11060 + 197 x block plus, by slot, a parts record word and an offset (slot 1 7 x word 0 + word 1 + 16; 13 and 16 fixed; 7, 9 and 14 none; 4 none when its word is 13), each through LoadSlot::LoadShared with OnPartLoadedB. |
| `sub_820D71C0` | `CarParts::Load` | Overridden | The 60-byte record copied (memcpy) to +28; then by the pools' model set ([+106000] +8): 0 CarParts::LoadSetA, 1 LoadSetB, others nothing. |
| `sub_820D86C0` | `TrafficCar::Load` | Overridden | CarRecord::Copy to +32; +7392 = the first set; LoadSlot::Load of asset 22528 + the car's number into the node slot (+3744, TrafficCar::OnFileLoaded); on failure the error sprintf'd into a buffer on its 1,136-byte frame; then a 16 x 16 A8R8G8B8 Texture::Create into the release words +3728 and +3732. |

### `native_save_data`

The save data's defaults, on `SaveData` and its parts (`game/save_data.h`); a memset's clear is done in place, then the fields that aren't 0 set. SaveData::NewGame (`sub_82113FB0`) keeps `unlock_cut_cars`' override, which calls ours.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82105830` | `Stats::Stats` | Overridden | The stats' records' defaults. |
| `sub_82113828` | `Stats::ResetRecords` | Overridden | The 15 records' defaults (their three indices -1), then +720 = 0. |
| `sub_82113880` | `Stats::Reset` | Overridden | The stats cleared and their defaults. |
| `sub_82113D28` | `SaveData::Clear` | Overridden | Progress::Clear: the 119,908 bytes cleared, its fields, Stats::Reset on +48. |
| `sub_821134F8` | `BitSet400::SetAll` | Overridden | Bits 0 to 399 set. |
| `sub_82105A18` | `ControlMap::SetDefault` | Overridden | The 15 actions' buttons. |
| `sub_82105AB8` | `Options::SetDefaults` | Overridden | The options and their four control maps. |
| `sub_82105E98` | `CourseRecords::Reset` | Overridden | The header cleared, then the 22 records' own reset (sub_82105E20). |
| `sub_82105748` | `SaveData::Delete` | Overridden | The slot's save (0x826B5838 + 4 x slot) deleted and cleared. |
| `sub_82105938` | `SaveData::Create` | Overridden | A save (129,200 bytes) made for the slot and its defaults set. |
| `sub_8209F718` | `Players::ResetControllers` | Overridden | Controller i = i (0x826AA884), then 0x826B73F8 = 1. |
| `sub_82209868` | `MenuList::Init` | Overridden | A menu list: its count, shown, cursor words, and its flags VirtualAlloc'd anew; the 9th and 10th arguments at the caller's r1 + 84 and + 92. |
| `sub_8221F7A8` | `TitleScreen::ResetPlayer` | Overridden | The title's menu list set up, the player flags, save 0 made again, the controllers reset. |
| `sub_820CCA60` | `Defaults::Table50A8` | Overridden | 0x826B4D88. |
| `sub_820CCA70` | `Defaults::Table3044` | Overridden | 0x826B2D24. |
| `sub_820CCA80` | `Defaults::Table3E34` | Overridden | 0x826B3B14. |
| `sub_820CCA90` | `Defaults::Table3C24` | Overridden | 0x826B3904. |
| `sub_820CCAA0` | `Defaults::Table50E4` | Overridden | 0x826B4DC4. |
| `sub_821057E8` | `SaveData::TwoPlayerBlock` | Overridden | Slot 1's word. |
| `sub_821057F8` | `SaveData::Teardown` | Overridden | The input recordings, the replay and the race data freed, save 0 deleted, the data tables freed. Never runs in play. |
| `sub_821058B0` | `SaveData::CreateTwoPlayerBlock` | Overridden | The two-player block (240,100 bytes: two players' save data up to their course records, one set of options) made into slot 1 (0x826B583C), both stats made, Progress::SetDefaults on each half and the options' defaults. |
| `sub_82105D88` | `CourseRecords::Of` | Overridden | r3 + 20 + 408 x (course x 2 + traffic), r4 the pair. |

### `native_accessories`

The interior accessories (`src/game/accessories.cpp`, types in `src/game/accessories.h`): the set at [0x826B51F0] (32 bytes: three entries of 1,104 bytes, each a count, an AccessoryFile at +4 and the accessory at +48, then a scale), the accessory's constructor and destructor, the skeleton's destructor, and the 36-byte AccessoryFile (+4 the asset, +8 the archive's serial, +12 its state: 0 none, 1 loading, 2 loaded, +16 the data, +20 its size, +24 whether the data is its own pages, +28 a callback and +32 its context). The archive, VirtualAlloc and VirtualFree, the entries' own constructors and destructors, the skeleton's construction and the accessory's texture stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820F0AE0` | `AccessoryFile::AccessoryFile` | Overridden | The vtable (0x82006710) and the defaults. |
| `sub_820F0A80` | `AccessoryFile::SetData` | Overridden | r4 the data, r5 whether it's the file's own; loaded. |
| `sub_820F0B20` | `AccessoryFile::Load` | Overridden | r4 the asset, r5 a buffer (0: its own pages), r6 a callback and r7 its context: Archive::Load with OnArchiveDone; the serial. |
| `sub_820F0AB0` | `AccessoryFile::OnArchiveDone` | Overridden | For the file's serial: loaded, then its callback with r6 its context. |
| `sub_820F0760` | `AccessoryFile::FreeData` | Overridden | [r3] VirtualFree'd and cleared. |
| `sub_820F0BC0` | `AccessoryFile::Reset` | Overridden | Its own data cancelled or freed, its defaults. |
| `sub_820F0C48` | `AccessoryFile::~AccessoryFile` | Overridden | Its vtable, `AccessoryFile::Reset`, then the empty base vtable (0x82000A40). |
| `sub_820F0648` | `AccessoriesBase::AccessoriesBase` | Overridden | The vtable (0x82006704), the three entries 0. |
| `sub_820F09E8` | `AccessoriesBase::~AccessoriesBase` | Overridden | Its vtable, `Accessories::ReleaseAll`, then the empty base vtable. |
| `sub_820F0CF0` | `Accessories::Accessories` | Overridden | The base, the vtable (0x82006718), the scale 1.0. |
| `sub_820F0DD8` | `Accessories::New` | Overridden | 32 bytes of pages made a set, or 0. |
| `sub_820F0EC8` | `Accessories::Create` | Overridden | The set made once. |
| `sub_820F0D90` | `Accessories::SetScale` | Overridden | r4 the scale to each entry's accessory (+1024) and the set's +16. |
| `sub_820F0E20` | `Accessories::Free` | Overridden | The set at [r3] destroyed (vtable slot 0) and its pages freed. |
| `sub_820F0F08` | `Accessories::Destroy` | Overridden | The set freed, if there is one. |
| `sub_820F0968` | `Accessories::ReleaseAll` | Overridden | Each held entry released once. |
| `sub_820F0920` | `Accessories::Release` | Overridden | r4 the index (3: none): its count down, destroyed at 0. |
| `sub_820F0870` | `Accessories::AcquireEntry` | Overridden | r4 the index, r5 the texture: the entry made (its file loaded) or its count up; the accessory, or 0. |
| `sub_820F0D48` | `Accessories::Acquire` | Overridden | AcquireEntry, then the set's scale to the accessory. |
| `sub_820EF0C8` | `Accessory::Accessory` | Overridden | `PartModel::PartModel`, vtable 0x820066C0, its two files (+464, +500) and skeleton (+544) made, its position and rotation (+992, +1008) (0, 0, 0, 1), its scale (+1024) 1, +1044 = 1. |
| `sub_820EF180` | `Accessory::~Accessory` | Overridden | The skeleton and files destroyed, then `PartModel::~PartModel`. |
| `sub_820EFE58` | `AccessorySkeleton::~AccessorySkeleton` | Overridden | Its vtables (0x820066D8, +176 0x820066D4) and its members' (+216, +220, +240) put back, its file (+180) destroyed, +176 the empty vtable, then `sub_82580088`. |

### `native_strings_and_font`

The string table at [0x826B5914] (2,256 bytes: +0 the 17 files, +68 their loaded flags, +136 the mask, +2188 the counts). The font's drawing and the formatting stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8210A990` | `Strings::Init` | Overridden | The table made once (VirtualAlloc). |
| `sub_8210A980` | `Strings::OnLoaded` | Overridden | The archive's callback: [r6] = 1. |
| `sub_8210A668` | `Strings::Release` | Overridden | r4 a mask: each loaded file's count down, freed at 0. |
| `sub_821082C0` | `Font::IsLoaded` | Overridden | 1 when a page is asked for and each one asked for is loaded. |
| `sub_8210A898` | `Strings::IsLoaded` | Overridden | 1 when a file is asked for, each one is loaded, and the font is. |
| `sub_8220FA18` | `Text::IsLoaded` | Overridden | The table and the font. |
| `sub_8210A930` | `Strings::Get` | Overridden | r4 the id: its file by the bounds at 0x82007DB0, DataFile::GetEntry of the rest. |
| `sub_8210A6F0` | `Strings::PageCount` | Overridden | r4 the text: its page breaks (0xFFD1). |
| `sub_8210A718` | `Strings::CopyPage` | Overridden | r4 out, r5 the text, r6 the page, r7 the lines to skip: the page appended (wcscat). |
| `sub_821083C0` | `Font::OutCode` | Overridden | r4 a vertex: 1/2/4 left of, right of, across the clip rectangle; 8/16/32 above, below, across.|new |
| `sub_82108488` | `Font::SetParams` | Overridden | r4 the style: kept at +16; the top left (+56/+60) = the position less (scale - 1) × the scale origin, a whole pixel; the spacings (+8256/+8260) from the style's ints.|new |
| `sub_82108578` | `Font::LineWidth` | Overridden | r4 a line: as TextWidth for one line (to `\n` or a page break); justified text the plain sum. f1.|new |
| `sub_82108688` | `Font::LineY` | Overridden | r4 the line count, r5 the line: its y by the vertical alignment in the box (+72); a line is the cell height × the glyph scale (truncated) plus the line spacing; justified text sets the line spacing that fills the box. An unknown alignment leaves f1.|new |
| `sub_82108930` | `Font::LineGlyphs` | Overridden | r4 a line: the glyphs in it (16-bit; colors not counted). Leaves r10.|new |
| `sub_82108980` | `XPR::Release` | Overridden | r3 a slot: its XPR destroyed (`sub_8257CA60`), freed (operator delete) and the slot cleared.|new |
| `sub_82108C50` | `Font::TextWidth` | Overridden | r4 the style, r5 the text: the widest line up to a page break (each glyph's advance × the glyph scale, truncated, plus the letter spacing but for justified text), less the spacing, × the scale; f1. A `\n` counts its own advance.|new |
| `sub_82109100` | `Font::LineX` | Overridden | r4 a line: its x by the horizontal alignment in the box (+68 × the scale), a whole pixel; justified text sets the letter spacing that fills the box (Font::LineGlyphs). An unknown alignment gives the line's width. f1.|new |
| `sub_821095B8` | `Font::Load` | Overridden | r4 a mask of pages: added to +12; each page asked for without a file gets a page (operator new, 28 bytes), its file's memory (VirtualAlloc of the asset's size, assets 22654-22657 at 0x82007BF4) and Archive::Load with Font::OnPageLoaded.|new |
| `sub_82109B28` | `Font::Init` | Overridden | The font (8,400 bytes, VirtualAlloc) into [0x826B58E0], set up by Font::Font (recompiled).|new |
| `sub_8210A7B0` | `Strings::FormatGrouped` | Overridden | r4 an int, r5 the output (appended): its magnitude with thousands separators, a negative one then the whole output after a '-' ("--2147483648" for the smallest int). The original where its 256-unit buffer would overrun or the output has 0xFFFF.|new |
| `sub_8210AAC0` | `Strings::FormatV` | Overridden | r4 the output (appended), r5 a string id, r6 a va_list (8-byte slots): the string with 0xFFD7 (an inner string id), 0xFFEC-0xFFF5 (`%Nd`), 0xFFE2-0xFFEB (`%.Nf` of the slot's double as a float), 0xFFD8-0xFFE1 (`%0Nd`), 0xFFD3 (grouped) and 0xFFD2 (`sub_82108B90`) replaced; the numbers by the game's swprintf in a 2,752-byte guest frame (+96 the format, +608 the text). Returns r4.|new |
| `sub_8210ACF0` | `Strings::Format` | Overridden | As FormatV with the arguments as varargs (r6-r10 stored at the caller's r1 + 40, the va_list), no 0xFFD2; a 2,736-byte frame.|new |
| `sub_8210AEF8` | `Text::Init` | Overridden | No arguments, once at boot: Font::Init, then Strings::Init.|new |
| `sub_825BE990` |  | Overridden | A static initializer: the font's four pixel shader sources at 0x826B58E4 (12 bytes each: the HLSL from `[0x825F32C4]`.., its strlen, 0).|new |
| `sub_82109348` | `Font::Font` | Overridden | +0 the video output (the owner `[0x8265CD68]`'s slot 7, then slot 3), +4 the command list and +8 the render queue (the render manager `[0x826AA880]`'s slot 8, then slots 1 and 2); Device::Lock (its word at the 128-byte frame's +88), Font::CreateShaders, Device::Unlock; the pages (+20) and their files (+40) cleared; the ten colors at +8336 (255 234 0, 44 242 242, 82 240 105, 198 66 14, 0 255 0, 127 127 127, then zeros); +12 = 0. Returns 14.|new |
| `sub_8210A5C0` | `swprintf` | Overridden | r3 the buffer, r4 the format, r5.. the arguments: r5-r10 stored in the caller's parameter area (+32 to +79, the stack arguments after them) and that handed to vswprintf (`sub_8258F420`) as the va_list, from a 96-byte frame (the va_list kept at +80).|new |

### `native_game_modes`

The modes' state machines: up to 200 entries of 48 bytes (a 16-byte key, the handler at +16), the count at +9600. StateMachine::Handle (`sub_82106410`) keeps `title_screen.cpp`'s override, which calls ours.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82106980` | `Modes::Modes` | Overridden | r4 the owner: the vtable and the modes' globals. |
| `sub_821061D0` | `StateKey::Equals` | Overridden | The two keys' four words. |
| `sub_82106230` | `StateMachine::IndexOf` | Overridden | The key's entry, or -1. |
| `sub_821062E8` | `StateMachine::Add` | Overridden | r4 the key, r5 the handler: replaced, or a new entry. |
| `sub_82106380` | `StateMachine::Find` | Overridden | The key's handler, or the default one (0x826B5848, made once). |
| `sub_82107230` | `StateMachine::StateMachine` | Overridden | The 200 entries cleared, their handlers empty (0x82006954). |
| `sub_82106D98` | `Mode::Mode` | Overridden | The vtables. |
| `sub_8215BCB8` | `RaceOverlaysMachine::SetFlags` | Overridden | The 25 overlays' +772 and +776 = 1. |
| `sub_8215BDA0` | `RaceOverlaysMachine::ClearFlags` | Overridden | +772, +776 and +780 = 0. |
| `sub_8215BCF0` | `RaceOverlaysMachine::SetFlag820` | Overridden | +772 = 1, +776 = 0. |
| `sub_8215BD30` | `RaceOverlaysMachine::ResetOverlays` | Overridden | Each overlay's vtable slot 3, then +780 = 1. |

### `native_screen_constructors`

The front end's screen constructors, on the types in `game/screen_constructors.h`: each sets its vtable and defaults (mostly -1 for no node), then its own node numbers, and makes its members in order.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82256928` | `NodeBase::NodeBase` | Overridden | Constructor. |
| `sub_8224C888` | `PanelGroupBase::PanelGroupBase` | Overridden | Constructor. |
| `sub_8224B148` | `PanelCBase::PanelCBase` | Overridden | Constructor. |
| `sub_8224A9F0` | `PanelDBase::PanelDBase` | Overridden | Constructor. |
| `sub_8224C958` | `PanelEBase::PanelEBase` | Overridden | Constructor. |
| `sub_8222E2A0` | `PanelE::PanelE` | Overridden | Constructor. |
| `sub_8224D468` | `PanelFBase::PanelFBase` | Overridden | Constructor. |
| `sub_8222F730` | `Panel6C::Panel6C` | Overridden | Constructor. |
| `sub_8223F270` | `Panel7C::Panel7C` | Overridden | Constructor. |
| `sub_8222F8A0` | `Panel6D::Panel6D` | Overridden | Constructor. |
| `sub_8223F450` | `Panel7D::Panel7D` | Overridden | Constructor. |
| `sub_8222E388` | `Panel6F::Panel6F` | Overridden | Constructor. |
| `sub_8223DE58` | `Panel7F::Panel7F` | Overridden | Constructor. |
| `sub_8222E128` | `PanelGroup::PanelGroup` | Overridden | Constructor. |
| `sub_8222F6C8` | `PanelGroup6B::PanelGroup6B` | Overridden | Constructor. |
| `sub_8223F218` | `PanelGroup7B::PanelGroup7B` | Overridden | Constructor. |
| `sub_82248770` | `PanelSetBase::PanelSetBase` | Overridden | Constructor. |
| `sub_822477C0` | `GroupPair::GroupPair` | Overridden | Constructor. |
| `sub_8224A9A8` | `MachineScreen::MachineScreen` | Overridden | Constructor. |
| `sub_82233FE8` | `Type6Screen0::Type6Screen0` | Overridden | Constructor. |
| `sub_82243B48` | `Type7Screen0::Type7Screen0` | Overridden | Constructor. |
| `sub_822457A0` | `NodeTable::NodeTable` | Overridden | Constructor. |
| `sub_82245818` | `PairedNodeTable::PairedNodeTable` | Overridden | Constructor. |
| `sub_822458D0` | `ColorNodeTable::ColorNodeTable` | Overridden | Constructor. |
| `sub_8224FC08` | `ListScreen::ListScreen` | Overridden | Constructor. |
| `sub_8224FD70` | `ListPartBase::ListPartBase` | Overridden | Constructor. |
| `sub_8224FF58` | `ListPartInnerBase::ListPartInnerBase` | Overridden | Constructor. |
| `sub_82250018` | `ListPartInner::ListPartInner` | Overridden | Constructor. |
| `sub_82250280` | `ListPart::ListPart` | Overridden | Constructor. |
| `sub_822549B0` | `NodeListScreen::NodeListScreen` | Overridden | Constructor. |
| `sub_82254B38` | `TitledListScreen::TitledListScreen` | Overridden | Constructor. |
| `sub_82257D50` | `NodeScreen::NodeScreen` | Overridden | Constructor. |
| `sub_82235090` | `Type6NodeScreen::Type6NodeScreen` | Overridden | Constructor. |
| `sub_82234B50` | `Type6Screens::Type6Screens` | Overridden | Constructor. |
| `sub_822446C8` | `Type7Screens::Type7Screens` | Overridden | Constructor. |

### `native_collision`

The collision shapes: their constructors, getters, bounds and placements. The box-box tests, impacts and pair solver stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820BD000` | `Query::Query` | Overridden | Constructor: the two shapes cleared, the four point sets' pointers set, the vectors' w cleared (`game/collision.h`, `Query`). |
| `sub_820BAE70` | `Shape::Shape` | Overridden | Constructor: the center at 0, radius 0, no rotation, its four defaults (0.5, 0.5, 0.25, 0.05) (`game/collision.h`, `Shape`). |
| `sub_820BAFD0` | `BoxShape::BoxShape` | Overridden | Constructor: Shape::Shape, the corners' and faces' w cleared, BoxShape::SetCorners. |
| `sub_820B9D38` | `BoxShape::Corner` | Overridden | r3 + 176 + 16 x r4. |
| `sub_820B9D48` | `BoxShape::Face` | Overridden | r3 + 304 + 32 x r4. |
| `sub_820BA4C8` | `BoxShape::SetCacheWord` | Overridden | +160 + 4 x r4 = r5. |
| `sub_820BA4D8` | `BoxShape::CacheWord` | Overridden | +160 + 4 x r4. |
| `sub_820BA8A8` | `BoxShape::CopyCache` | Overridden | r4's 16-byte cache into r3's. |
| `sub_820BA998` | `Shape::Radius` | Overridden | f1 = +32. |
| `sub_820BA1C8` | `BoxShape::LowerCorner` | Overridden | r4 the axis: the corner of the axis' face with the lower distance. |
| `sub_820BA210` | `BoxShape::FaceDistance` | Overridden | r4 the face, r5 a point: the dot with its direction less its distance. |
| `sub_820BA150` | `BoxShape::Translate` | Overridden | r4 added to the corners and center, the faces' distances moved. |
| `sub_822B8A90` | `WordAt8` | Overridden | r3's +8 (shared). |
| `sub_820BA978` | `CollisionShape::CurrentCenter` | Overridden | +8's +48. |
| `sub_820BA988` | `CollisionShape::PreviousCenter` | Overridden | +4's +48. |
| `sub_820BAB08` | `CollisionShape::Type` | Overridden | The shape's vtable slot 4. |
| `sub_820BAC48` | `CollisionShape::Previous` | Overridden | The shape's slot 4, then +4. |
| `sub_820BAC88` | `CollisionShape::Current` | Overridden | The shape's slot 4, then +8. |
| `sub_820BAA50` | `CollisionShape::UpdateBounds` | Overridden | Every tick: the box around the previous and current centers, grown by the radius. |
| `sub_820BA8F8` | `CollisionShape::~CollisionShape` | Overridden | The two placements deleted and cleared. |
| `sub_822DF518` | `Query::SetShapes` | Overridden | +0, +4 = r4, r5 (shared). |
| `sub_820BC740` | `Contact::Set` | Overridden | r4 to +0, r5 to +16 and +32, f1 and f2 to +48 and +52. |
| `sub_820BC690` | `Contact::Flip` | Overridden | +16 and +32 scaled by -1. |
| `sub_82468988` | `BoxShape::CornerCount` | Overridden | 8 (a constant getter the linker shares). |
| `sub_82469648` | `BoxShape::FaceCount` | Overridden | 6 (shared). |
| `sub_820B9D58` | `BoxShape::EdgeCount` | Overridden | 12 (shared). |
| `sub_820B9DF8` | `BoxShape::EdgeStart` | Overridden | The edge r4's first corner (0x825F0D04). |
| `sub_820B9E10` | `BoxShape::EdgeEnd` | Overridden | The edge r4's second corner (0x825F0D34). |
| `sub_820B9E28` | `BoxEdges::Start` | Overridden | The edge r3's first corner. |
| `sub_820B9E40` | `BoxEdges::End` | Overridden | The edge r3's second corner. |

### `native_car_physics`

The car physics' set-ups and accessors. Float steps are the originals'. The vehicle's step and the collision world stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820AEBA0` | `CarData::SetDefaults` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820AEE68` | `CarData::CarData` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820B4160` | `Vehicle::Vehicle` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820BC280` | `CollisionSurfaces::Reset` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820BC398` | `CollisionWorld::ResetSurfaces` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820BA8D0` | `CollisionShape::Init` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820B7988` | `CollisionObject::CollisionObject` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820B79E0` | `CollisionObject::~CollisionObject` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820B77C8` | `RigidBody::RigidBody` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820B6180` | `Vehicle::RefreshFromBody` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820B3AB8` | `Vehicle::SetValue2388` | Overridden | +2388 = f1 clamped, +2385 = r4's byte, +2396 = 1.0 - f1 x k clamped (fused). |
| `sub_820B3B28` | `Vehicle::SetGearValues` | Overridden | The gear's ratios into +2304 and +2308, +2312 to +2324. |
| `sub_820B0360` | `Vehicle::RampReverseThrottle` | Overridden | +288 ramped by the car's rate (fused) up to 1.0, or 0.0. |
| `sub_820B03A8` | `Vehicle::RampHandbrake` | Overridden | +296 the same. |
| `sub_820B03F0` | `Vehicle::SurfaceValue` | Overridden | r4 the wheel: its surface record's +0, or 0.0. |
| `sub_820B0938` | `Wheel::Copy` | Overridden | r4 into r3, field by field. |
| `sub_820B0BA8` | `WheelRecord2::Copy` | Overridden | The 120 bytes. |
| `sub_820B7050` | `RigidBody::SetCenterOfMassPosition` | Overridden | sub_82291258 on +224, then UpdateOrigin. |
| `sub_820B7088` | `RigidBody::SetOrientation` | Overridden | r4 to +80 normalized, then UpdateOrientation and UpdateOrigin. |
| `sub_820B70D8` | `RigidBody::SetVelocity` | Overridden | r4 scaled by +4 into +176, then UpdateVelocity. |
| `sub_820B7120` | `RigidBody::SetInertia` | Overridden | +352 and their inverses at +368, then UpdateOrientation. |
| `sub_820BC040` | `CollisionSurface::A` | Overridden | f1 by r3's bits (0x01000000, 0x02000000, 0x04000000, else the default). |
| `sub_820BC088` | `CollisionSurface::B` | Overridden | As A. |
| `sub_820BC0D0` | `CollisionSurface::C` | Overridden | As A. |
| `sub_820BC118` | `CollisionSurface::D` | Overridden | As A. |
| `sub_820BC378` | `CollisionSurface::OfC` | Overridden | C. |
| `sub_820BC380` | `CollisionSurface::OfD` | Overridden | D. |
| `sub_820BC388` | `CollisionSurface::OfA` | Overridden | A. |
| `sub_820BC390` | `CollisionSurface::OfB` | Overridden | B. |
| `sub_820B3CA0` | `Vehicle::SetBrake` | Overridden | f1 clamped to [0.0, 1.0] into +292. |
| `sub_820B3CD0` | `Vehicle::SetHandbrake` | Overridden | f1 clamped into +296. |
| `sub_820B3D00` | `Vehicle::SetThrottle` | Overridden | f1 clamped into +284. |
| `sub_820B0448` | `Vehicle::SurfaceValue20` | Overridden | The record's +20 (0.0), into the wheel's +612. |
| `sub_820B04A0` | `Vehicle::SurfaceValue12` | Overridden | The record's +12 (1.0), into +600. |
| `sub_820B04F8` | `Vehicle::SurfaceValue16` | Overridden | The record's +16 (1.0), into +604. |
| `sub_820BC160` | `CollisionSurface::SetA` | Overridden | r4's bits, f1. |
| `sub_820BC1A8` | `CollisionSurface::SetB` | Overridden | As SetA. |
| `sub_820BC1F0` | `CollisionSurface::SetC` | Overridden | As SetA. |
| `sub_820BC238` | `CollisionSurface::SetD` | Overridden | As SetA; its default slot differs from D's getter, as in the original. |

### `native_vehicle`

The vehicles' construction, refresh, removal and route positions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820BFF50` | `Vehicle::Refresh` | Overridden | Vehicle::RefreshFromBody on its physics part. |
| `sub_820C2000` | `Vehicles::DestroyAll` | Overridden | The 7 vehicles from the last back: each one's wheel route positions destroyed, then its physics part. |
| `sub_820C2EF8` | `Vehicle::Construct` | Overridden | Vehicle::Vehicle on its physics part, CarData::CarData, and the five route positions' vectors given w = 1 (`game/vehicle.h`, `Vehicle`). |
| `sub_820C1F70` | `RoutePosition::Copy` | Overridden | r4 into r3 (`game/vehicle.h`, `RoutePosition`). |
| `sub_820C1B98` | `Vehicle::UpdateTrackPositions` | Overridden | Every tick: the body's and the wheels' points into their route positions, each updated. |
| `sub_820C1F00` | `Vehicle::RemoveFromWorld` | Overridden | The collision object taken out of the world, the other object deleted. |

### `native_race_states`

The race states' simple handlers, the progress' reset and the route helpers, on `RaceStates` and `RaceProgress` (`game/race_states.h`). The race's ticks stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82161218` | `RaceStates::ClearStep` | Overridden | `step` cleared. |
| `sub_82161238` | `RaceStates::OnQuit` | Overridden | +9612 cleared. |
| `sub_82161248` | `RaceStates::FadeInFree` | Overridden | +9612 to +9620 cleared, then the fade in (sub_8209D158, screen 5). |
| `sub_82161278` | `RaceStates::FadeOut` | Overridden | The fade out (sub_8209D1E8, screen 15). |
| `sub_821617F0` | `RaceStates::Unload` | Overridden | The progress at [0x826C7D90] deleted. |
| `sub_82162280` | `RaceStates::OnStandingStart` | Overridden | +9612 to +9640 (but `progress`) and [0x826B65D0] cleared, then sub_82139350. |
| `sub_821627B0` | `RaceProgress::Reset` | Overridden | The race entry reset (`RaceEntry::Reset`), then the four entries, the two halfword tables and the words after them. |
| `sub_82162C90` | `RaceStates::Begin` | Overridden | +9612 to +9620 cleared, the progress reset, then +9628 to +9640 cleared. |
| `sub_821600F8` | `Time::Sub` | Overridden | [r3] - [r4] in milliseconds, a time word again (sub_82101850). |
| `sub_82160170` | `RoutePosition::NotAfter` | Overridden | 1 when r3's route point isn't after r4's. |
| `sub_82160CB0` | `RaceProgress::CountTime` | Overridden | Racer r5's `time_count` up, its `time` from it (sub_821018B0). |
| `sub_82160D18` | `RouteSections::Before` | Overridden | The section (r3 or one beside it) whose link ahead is r4, or -1. |
| `sub_82160E20` | `RouteSections::Beside` | Overridden | 1 when r3 is r4 or beside it. |

### `native_material_passes`

The part models' materials: counts, records, alpha setters, the paint table's lookups.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820D3218` | `TableVector` | Overridden | r3 = the paint table's vector (r4, r5). |
| `sub_820D3248` | `IsBlockSetA` | Overridden | 1 for 1, 26, 28, 33 and 36. |
| `sub_820D3280` | `IsBlockSetB` | Overridden | 1 for 10 and 37. |
| `sub_820D3370` | `RecordAt` | Overridden | The paint record r3 (0x82003660, 48 bytes). |
| `sub_8257A3C0` | `Model::MaterialTotal` | Overridden | r4 the mesh: its material count. |
| `sub_8257A400` | `Model::MaterialAt` | Overridden | r4 the mesh, r5 the material. |
| `sub_820D1848` | `PartModel::SetAlpha` | Overridden | f1 into each material's +12, both passes. |
| `sub_820D1780` | `PartModel::SetPair1002Alpha` | Overridden | The same for the materials of kind 1002. |

### `native_replay_snapshots`

The race snapshots' copies (`src/game/replay_snapshots.cpp`, types in `src/game/replay_snapshots.h`): each save and restore one list of field copies between the live object and its snapshot, run one way or the other; the snapshot-to-snapshot copies copy every field. The originals' memcpys stay calls. The replay's own save and restore stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82104480` | `Record5232::RestoreTrack` | Overridden | The snapshot's words, track block (memcpy) and five route positions into the vehicle r4. |
| `sub_82103320` | `BodyState::Restore` | Overridden | The body state's fields (its layout the same both ways) from r3 to r4. |
| `sub_82102DE8` | `Member1136::Restore` | Overridden | The member's fields (its four wheel blocks among them) from the snapshot r3 to r4. |
| `sub_82102C30` | `Part464::Restore` | Overridden | The part's fields and five points from the snapshot r3 to r4. |
| `sub_821044F0` | `Record5232::SaveTrack` | Overridden | RestoreTrack the other way. |
| `sub_82103B68` | `BodyState::Save` | Overridden | Restore the other way. |
| `sub_82103638` | `Member1136::Save` | Overridden | Restore the other way. |
| `sub_82103480` | `Part464::Save` | Overridden | Restore the other way. |
| `sub_82104560` | `Record5232::CopyTrack` | Overridden | Every field of the snapshot r4 to r3. |
| `sub_82103CC8` | `BodyState::Copy` | Overridden | Every field of r4 to r3. |
| `sub_82104330` | `Part464::Copy` | Overridden | Every field of the snapshot r4 to r3. |
| `sub_821045D0` | `Member4080::CopyShapes` | Overridden | Two parts' snapshots and the 100 bytes after them. |
| `sub_82104708` | `Member4080::Copy` | Overridden | Its word, its shapes, three blocks after them. |

### `native_ui_movies`

The UI movies (the XMD/X2D menu and HUD scenes): the file lookups, the update and track apply (`game/ui_movies.cpp`, types in `game/ui_movies.h`); the drawing side: transforms, pictures, primitives, nodes, the text queue, the key curves, the load fix-ups (`game/ui_movie_draw.cpp`); the nodes, textures, text items, animation lists and times, lookups (`game/ui_movie_nodes.cpp`); the text setters, play/move/loop/finish, the movie table, open, load and unload (`game/ui_movie_text.cpp`). Left recompiled: the ticker's step and offset (`sub_8211BB10`, `sub_8211B9D0`: subtick's own), `sub_8211D3D0` and `sub_8211E538` (std::string temporaries from stack bytes they never write). `sub_8211D4B8` and `sub_82585C40` keep subtick's overrides, which call ours; every call to them goes through the override so subtick's wrappers run. DrawPrimitive with more than 16 vertices runs the original (its frame holds 16).

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8211D378` | `UiMovie::DestroyNamedAnimations` | Overridden | Its 1,024 named animations destroyed (sub_820913F0), last first. |
| `sub_8211E380` | `UiMovie::UiMovie` | Overridden | Its members and named animations made, +48, +37040 and its node map cleared. |
| `sub_8258BCC8` | `Animation::NodeTrack` | Overridden | r3 + 16 x (r4 + 1). |
| `sub_8258BCD8` | `Track::Key` | Overridden | r3 + 32 x r4 + 16. |
| `sub_82582CF8` | `VertexOffset` | Overridden | 16 x (r3 + 2 x (r4 + 4)). |
| `sub_82586358` | `IsXmd` | Overridden | "XMD\0001\0" at r3. |
| `sub_82586538` | `IsMovieFile` | Overridden | "X2D\0005\0" at r3 + 8. |
| `sub_8211B988` | `UiMovie::TextFirstLine` | Overridden | A text's first line (the node r4's +24): r3 the movie, r4 the text's id; 0 when the movie hasn't the text. |
| `sub_8211BB88` | `UiMovie::TextScrolls` | Overridden | Whether a text scrolls (the node r4's +4): r3 the movie, r4 the text's id; 0 when the movie hasn't the text. |
| `sub_8258BCE8` | `MovieFile::NodeTrackList` | Overridden | The file's table entry r4 (+32 its count). |
| `sub_8258BD00` | `MovieFile::KeyList` | Overridden | The table entry r4 (+40). |
| `sub_8258BD18` | `MovieFile::SubAnimation` | Overridden | The table entry r4 (+48). |
| `sub_825865D0` | `MovieFile::Animation` | Overridden | The table entry r4 (+16). |
| `sub_8211A078` | `UiNode::CopyFields` | Overridden | r4 a node given r5's fields by r4's tag: the common ones (+12 to +80), then a picture's (+100 to +136), a primitive's (+100 to +108 and each vertex, `sub_82582D20`) or a text's (all but +108, its id, and +112). |
| `sub_8211A300` | `UiMovie::ResetNodeFields` | Overridden | r4 (a sub-scene or -1) and r5 a node: its fields copied from the file's scene to the live and saved ones (CopyFields), not its subtree. |
| `sub_8211A3B0` | `UiMovie::ResetNode` | Overridden | The node's common fields copied from the file's scene to the live and saved ones, then each one's subtree put back (`sub_82119D50`). |
| `sub_8211A748` | `UiMovie::FindNode` | Overridden | r4 a sub-scene's index in the live scene or -1, r5 a child's index: the node (`UiMovie::Child`, `sub_82582D50`, once or twice). |
| `sub_8211A788` | `UiMovie::SetNodeColor` | Overridden | The node found, its color (+56 to +64) set to entry r6 of the table at 0x820083A0 (r, g, b words). Returns the node. |
| `sub_8211A800` | `UiMovie::SetColor` | Overridden | The same for node r4, color r5. |
| `sub_8211A830` | `UiMovie::SetLayer` | Overridden | Node r4's drawing layer (+72) for layer number r5: (n + 62) x 2 up to 11, (n + 290) x 2 up to 665 for 12 to 43, else 665. |
| `sub_8211A890` | `UiMovie::ClearTextIds` | Overridden | Every `TXT` chunk after the live scene's, up to the `TRM` chunk, loses its text id (+108). |
| `sub_8211A8D8` | `UiMovie::SetNodeTexture` | Overridden | The node found; r6 its texture entry (a primitive's +104, a picture's +108; other nodes left). Returns the node. |
| `sub_8211A958` | `MovieWords::Reserve` | Overridden | r3 a word array, r4 a capacity: when new, a new block (operator new[]), the words that fit copied, the old freed. |
| `sub_8211A9E8` | `TextItem::TextItem` | Overridden | A 128-byte queued text: +0 = 0, an identity transform at +16, +80 to +96 = 0.0. Returns it. |
| `sub_8211AA98` | `TextItem::~TextItem` | Overridden | +80 = 0.0, +0 = 0, the identity at +16. Leaves r3 at +16. |
| `sub_8211AB18` | `Quad::Quad` | Overridden | An identity transform at +0, +64 and +68 (the blend) = 0, +72 = 2. Returns it. |
| `sub_8211ABB8` | `UiMovie::InitAnimationLists` | Overridden | The movie's +0 to +24 and loaded flag (+40912) cleared, its queue and looping lists emptied (`FixedList::Reset`). |
| `sub_8211AC78` | `UiMovie::SetPictureTexture` | Overridden | The picture found; the movie's texture its entry names (`sub_82582D40`, the entry's first word an index into +28's array) replaced with r6. Returns the old one. |
| `sub_8211ACF8` | `UiMovie::GetPictureTexture` | Overridden | The same lookup; returns that texture. |
| `sub_8211AD60` | `UiMovie::SwapTexture` | Overridden | Texture r4 replaced with r5; returns the old one. |
| `sub_8211AD78` | `UiMovie::ResetPose` | Overridden | The live and saved scenes put back to the file's (`sub_82119D50` twice, nodes 0). |
| `sub_8211ADD0` | `UiMovie::ShowNode` | Overridden | The node found, +80 (visible) = 1. Returns it. |
| `sub_8211AE18` | `UiMovie::HideNode` | Overridden | The same, +80 = 0. |
| `sub_8211AE60` | `UiMovie::FreeTexts` | Overridden | Each text record's 2 KB buffer (VirtualFree, MEM_RELEASE) and the record (operator delete) freed; the text map's count = 0. |
| `sub_8211AED0` | `UiMovies::Reset` (probably) | Overridden | A screen's start: 0x826B5990 cleared, the UI movies given the render manager's renderer and command list (its slot 8, then slots 2 and 1; `sub_82582E90`), every movie table entry (0x826B59AC) marked free (key -1, no movie; nothing freed), seven words from 0x826B5984 cleared. |
| `sub_8211AFE8` | `MovieWords::MovieWords` | Overridden | The movie's textures: 512 words (operator new[]), none used. |
| `sub_8211B038` | `MovieWords::Append` | Overridden | The word at r4 appended, the capacity doubled first when full. |
| `sub_8211B0A0` | `TextItem::operator=` | Overridden | r4's +0, transform, +80, +84 to +96 and +100 to +112 copied to r3. Returns r3. |
| `sub_8211B160` | `TextItem::DeletingDestructor` | Overridden | r4 bit 1: an array (its count 16 bytes before), each destroyed last first; else one item. Bit 0: freed (operator delete[] or delete). Returns the block. |
| `sub_8211B1E8` | `TextList::TextList` | Overridden | 512 text items made in one 65,552-byte block (operator new[], the count in its first word); none when it fails. |
| `sub_8211B250` | `AnimationList::RemoveAt` | Overridden | The link at position r4 taken out and put first on the free links, count - 1. |
| `sub_8211B2B8` | `MoviePairs::MoviePairs` | Overridden | The movie table: 64 pairs, cleared (operator new[]; none when it fails). |
| `sub_8211B330` | `MoviePairs::Reserve` | Overridden | As `MovieWords::Reserve` for 8-byte pairs, the new block cleared. |
| `sub_8211B400` | `AnimationList::LinkFree` | Overridden | The first free link put in at position r4 (16-bit), count + 1. No check for a full list. |
| `sub_8211B468` | `AnimationList::Get` | Overridden | The 40-byte slot at position r5 of list r4 copied to r3. Returns r3. |
| `sub_8211B540` | `TextMap::Set` | Overridden | r3 the text map (128 {id, record} pairs, count +1024): the record for id [r4] set to [r5], or appended (the count stored first: a 129th lands on it). |
| `sub_8211B5C0` | `UiMovies::Find` | Overridden | r3 the pairs, r4 a key's address: the value's address, or 0x826B59BC. |
| `sub_8211B628` | `MoviePairs::Append` | Overridden | The pair at r4 appended, the capacity doubled first when full. |
| `sub_8211B6A0` | `AnimationList::Insert` | Overridden | A free link put in at position r5 (`LinkFree`), slot r4 copied to it. Returns the list's slot. |
| `sub_8211B710` | `UiMovie::Unload` | Overridden | The file's three copies freed (VirtualFree), the scenes cleared, the texture entries cleared and freed last first, the queued text items taken off, the texture set released (`sub_82108980`), the loaded flag cleared, the texts freed (`sub_8211AE60`). |
| `sub_8211B830` | `UiMovie::GetText` | Overridden | r4 a text id's address: the id given (the map's count + 1) when 0; its record (`TextMap::Find`), or a new one (operator new, 28 bytes) with a cleared 2 KB buffer (VirtualAlloc). |
| `sub_8211B8F0` | `UiMovie::TextString` | Overridden | Text r4's buffer, or the empty string at 0x820083C4. |
| `sub_8211B940` | `UiMovie::TextPage` | Overridden | Text r4's page (+20), or 0. |
| `sub_8211BBD0` | `UiMovie::AnimationTime` | Overridden | r4 a sub-scene or -1, r5 an animation: f1 its slot's time (+20), looked for in the looping list then the queue; -1.0 when neither has it. |
| `sub_8211BCE8` | `UiMovie::NamedAnimationTime` | Overridden | The same for named animation r4 (the slot's +8). |
| `sub_8211BDE8` | `UiMovie::IsNamedActive` | Overridden | 1 when named animation r4 is looping or queued, else 0. |
| `sub_8211BEE0` | `UiMovie::AssignLayers` | Overridden | r4 the scene, r5 a group (or the scene), r6 a layer, r7 by bucket: the group's children sorted into 64 buckets by their layer offset (+72, at most 63; on the guest stack, a 16,800-byte frame as the original's), then groups and sub-scenes recursed into, pictures, primitives and texts given their layer (`SetLayer`; the bucket's number when r7 is set). From the movie's load. |
| `sub_8211C068` | `UiMovies::IsLoaded` | Overridden | r3 a movie's id: 1 when its movie (the table at 0x826B59AC, `sub_8211B5C0`) has loaded (+40912). An id not in the table reads the movie word the find gives for none. |
| `sub_8211C338` | `UiMovieTable::Set` | Overridden | r3 the movie table, r4 and r5 the key's and value's addresses: the value set when the key is there, else the pair appended (`sub_8211B628`) from its frame. |
| `sub_8211C3C8` | `UiMovie::SetText` | Overridden | r3 the movie, r4 a scene's index or -1, r5 a child's, r6 a string id, r7 on its arguments (stored in the caller's area as a va_list): the text node's text cleared, its page 0, the string formatted into it (`Strings::FormatV`), the text map set. A node that isn't a text runs the original (an endless loop: the game's assert). |
| `sub_8211C498` | `UiMovie::SetCharText` | Overridden | As SetText with r6 an 8-bit string, converted by the font (`sub_82108B90`); the page kept. |
| `sub_8211C640` | `UiMovie::ClearText` | Overridden | The looked-up node's text cleared and its scroll restarted (+8 = 0). No text check. |
| `sub_8211C6C0` | `UiMovie::SetTextScrollSpeed` | Overridden | The looked-up node's text: +12 = r6 (the frames before it scrolls), +16 = f1 (the speed). |
| `sub_8211C740` | `UiMovie::ScrollText` | Overridden | The looked-up node's text: +4 = r6 (scrolling), +12 = 45, +16 = 4.0 (0x820021B8). |
| `sub_8211C7C8` | `UiMovie::RestartTextScroll` | Overridden | The looked-up node's text's +8 (frames scrolled) = 0. |
| `sub_8211C908` | `UiMovie::SetNumberText` | Overridden | The looked-up node's text cleared and a number formatted into it (`Strings::Format`): string 8 for 0, else string 9 with r6 (all 64 bits). |
| `sub_8211C9B0` | `UiMovie::SetNodeText` | Overridden | SetText with the node given (r4), r5 the string id, r6 on its arguments. |
| `sub_8211CA68` | `UiMovie::SetNodeCharText` | Overridden | SetCharText with the node given (r4), r5 the string. |
| `sub_8211CAF8` | `UiMovie::ShowNextPage` | Overridden | r3 the movie, r4 a text node: when the text has a page after the one shown (`Strings::PageCount`), shows it and returns 1, else 0; the text map set either way. |
| `sub_8211CB88` | `UiMovie::HasNextPage` | Overridden | The same test without the step or the map. |
| `sub_8211CBE0` | `UiMovie::SetNodeNumberText` | Overridden | SetNumberText with the node given (r4), r5 the number (64 bits). |
| `sub_8211CC68` | `UiMovie::SetNameText` | Overridden | r3 the movie, r4 a text node, r5 a racer's name record (+4 a kind, +8 an index, +10 a flag; +12, +16, +18 a second part's): kind 5 is one string (6012 + index, 0 to 70); else each part's string (kind 0: 5652 or 5832 + index, 1: 6083 +, 4: 5547 +; second kind 2: 5742 or 5922 +, 3: 6087 +) in string 533, when both have one. Through SetNodeText, the registers after the ones set passed on as they came. |
| `sub_8211CDA0` | `UiMovie::SetNodeTextV` | Overridden | r3 the movie, r4 a node, r5 a string id, r6 a va_list: the text cleared and the string formatted; no text check, the page kept. |
| `sub_8211CE18` | `UiMovie::ClearNodeText` | Overridden | r3 the movie, r4 a node: its text cleared (scroll and page kept). |
| `sub_8211D440` | `UiMovie::Draw` | Overridden | r3 the movie: drawn as its update draws without applying animations: the live scene's pose saved (`sub_82119D50`), laid out (`sub_825850F8`), texts drawn (`sub_8211CF58`), the two draw lists sent (`sub_825831B8` 676, 148). |
| `sub_8211DA40` | `UiMovie::RevealText` | Overridden | ShowNextPage on the looked-up node (r4, r5). |
| `sub_8211DA98` | `UiMovie::HasMoreText` | Overridden | HasNextPage on the looked-up node. |
| `sub_8211DAF0` | `UiMovie::SetChildNameText` | Overridden | SetNameText on the looked-up node, r6 the name record. |
| `sub_8211DC88` | `UiMovie::PlayNamed` | Overridden | r3 the movie, r4 a named animation's index, r5 to r8 the slot's with-next, backwards, remove-at-end and hold: unless it's looping (r3 then its position there) or r4 is negative, queued at the queue's end (`sub_8211B6A0`) at time 0, or its length when backwards (the word as it is), and copied to the movie's last started (+40920). |
| `sub_8211DDA0` | `UiMovie::MoveAnimation` | Overridden | r3 the movie, r4 a scene's index or -1, r5 an animation's, r6 nonzero to move it from the queue to the looping list's end, 0 the other way (each slot copied into its frame to test, `sub_8211B468`; taken off, `sub_8211B250`; appended, `sub_8211B6A0`). |
| `sub_8211DEA8` | `UiMovie::MoveNamedAnimation` | Overridden | The same by a named animation's index (r4), r5 the direction. |
| `sub_8211DF98` | `UiMovie::Loop` | Overridden | MoveAnimation to the looping list. |
| `sub_8211DFA8` | `UiMovie::LoopLast` | Overridden | The last started animation looped: by name when it was started by name, else by its scene and index. |
| `sub_8211DFE8` | `UiMovie::Unloop` | Overridden | MoveAnimation back to the queue. |
| `sub_8211DFF0` | `UiMovie::UnloopNamed` | Overridden | MoveNamedAnimation back to the queue. |
| `sub_8211DFF8` | `UiMovie::Finish` | Overridden | r3 the movie, r4 a scene's index or -1, r5 an animation's: moved to the queue, then, when queued, applied at its end (`sub_82585FD8` at its length, or 0 when backwards) and taken off: the scene keeps its end pose. |
| `sub_8211E1B8` | `UiMovie::FinishAll` | Overridden | r3 the movie: every looping animation moved to the queue's end, then every queued one applied at its end and taken off, first to last. |
| `sub_8211E320` | `UiMovie::~UiMovie` | Overridden | r3 the movie: its named animations destroyed (`sub_8211D378`), its draw context's destructor (`sub_821611F0`), its text draws' array (`sub_8211B160`, when there is one) and its textures' array (`operator delete[]`) freed. |
| `sub_8211E418` | `UiMovies::Delete` | Overridden | r3 a movie's id: when the table has a movie for it, unloaded (`sub_8211B710`), destroyed, freed (`operator delete`) and its table value set to 0. |
| `sub_8211E488` | `UiMovies::DeleteAll` | Overridden | Every movie in the table deleted by its id. |
| `sub_8211E4D8` | `NamedAnimation::Assign` | Overridden | r3 a named animation, r4 another: its name assigned (`std::string::assign`, all of it), its scene and animation copied. From `sub_8211E538`. |
| `sub_8211E530` | `UiMovies::DeleteAllJump` | Overridden | A jump to DeleteAll. |
| `sub_8211E6C0` | `UiMovie::OnLoaded` | Overridden | `Archive::Load`'s callback, r4 the loaded XMD archive, r6 the movie: its X2D file (entry 0, up to its TRM chunk's end) copied into three new buffers (VirtualAlloc), fixed up, their scenes kept (+16 live, +20 saved, +24 the file's); each scene's layers set (`sub_8211BEE0`), the texts' ids cleared and the texts freed, the named animations made (`sub_8211E538`). With more than one entry, entry 1 is a texture set: an XPR made (+12) and each texture appended to +28 with its width and height. The archive freed, +40912 (loaded) = 1. |
| `sub_8211E958` | `UiMovie::Load` | Overridden | r3 the movie, r4 its file's id: a buffer of the file's size (`Archive::GetSize`, VirtualAlloc) and `Archive::Load` with `UiMovie::OnLoaded` as the callback and the movie as its context. |
| `sub_8211E9C8` | `UiMovies::Open` | Overridden | r3 a movie's id, r4 its file's id: unless the table has a movie for the id (0), a movie made (40,976 bytes, `UiMovie::UiMovie`), its lists set up (`sub_8211ABB8`), its id kept (+40916), its file loaded and the movie put in the table; 1. |
| `sub_82582CC0` | `MovieChunks::Find` | Overridden | r3 a chunk (+0 tag, +4 the offset to the next), r4 a tag: the next chunk tagged r4, or 0 at the "TRM" end (the end chunk when r4 is "TRM"). |
| `sub_82582D10` | `Primitive::TextureEntry` | Overridden | r3 + 128 + 16 x r4. |
| `sub_82582D20` | `Primitive::Vertex` | Overridden | Its 32-byte vertex r4, after its [+112] texture entries. |
| `sub_82582D40` | `Picture::TextureEntry` | Overridden | r3 + 144 + 16 x r4. |
| `sub_82582D68` | `Scene::Animation` | Overridden | The animation r4 of a scene (the entry [+116] + r4). |
| `sub_82582D80` | `UiMovie::FixUpScene` | Overridden | r3 the scene, r4 the file: its entries' offsets made pointers (its animations through MovieFile::Animation), its sub-scenes the same. Returns 1. |
| `sub_82582E28` | `Clip::Intersect` | Overridden | r3's clip narrowed by r4's: x0 and y1 the larger, y0 and x1 the smaller. Returns r3. |
| `sub_82582EA8` | `UiMovie::ResetClip` | Overridden | Two chunks keyed r3 handed to the renderer: the clip back to 0, 0, 1280, 720 (at -10000), then the clip off (at -9000). |
| `sub_82583028` | `DrawContext::DrawContext` | Overridden | Identity matrix, no clip, color 1.0, +96..+108 cleared. Returns r3. |
| `sub_825830F0` | `DrawContext::Copy` | Overridden | r3 = r4: the matrix (Matrix::Copy, w words set first), clip, color, textures, text list, +104, the scaled flag. Returns r3. |
| `sub_825831B8` | `UiMovie::ResetClipAfter` | Overridden | `UiMovie::ResetClip` keyed r4 (UiMovie::Update's last draws, keys 676 and 148). |
| `sub_825831C8` | `UiMovie::RecordTexturedQuad` | Overridden | r3 a quad, r4 its key: one chunk on the command list (`[0x8297307C]`): its clip or the clip off, its texture, blend states by +68, sampler states by +112, its 24-byte vertices (position, an a-r-g-b word, u, v), handed to the renderer (`[0x82973078]`, slot 4) at depth 0. |
| `sub_82583790` | `UiMovie::RecordQuad` | Overridden | As `sub_825831C8` without a texture (no texture commands, no u and v). |
| `sub_82583D18` | `TextItem::Copy` | Overridden | r3 = r4: its node, matrix (Matrix::Copy), depth, color and clip. Returns r3. |
| `sub_82583DC0` | `UiMovie::ApplyTransform` | Overridden | r3 the context, r4 the node: its clip (offset by the context's matrix's third row, kept or intersected), its color tinting the context's, its transform in front of the context's matrix (moved, turned, scaled about its pivot; steps doing nothing skipped). |
| `sub_825842E0` | `UiMovie::DrawQuad` | Overridden | r3 a quad, r4 its key: recorded with or without a texture, then the clip reset (key + 1) when clipped. |
| `sub_82584438` | `UiMovie::DrawPicture` | Overridden | r3 the context, r5 the picture, r6 the layer: a 4-vertex quad from (0, 0) to its size, in the context's color, its texture's pixel rectangle (or 0 to 1), flipped, clipped, drawn at the layer's depth. |
| `sub_82584A40` | `UiMovie::DrawPrimitive` | Overridden | As DrawPicture with the primitive's vertices (colors tinted, squared when +100 is set). More than 16 vertices run the original. |
| `sub_82584DF8` | `TextList::Append` | Overridden | r3 the list, r4 a text: copied into the next slot, the list doubled first when full (`sub_82584350`). Returns the slot. |
| `sub_82584E68` | `UiMovie::QueueText` | Overridden | r3 the context, r5 the text node, r6 the layer: its transform applied; with a text list, a text item (node, matrix, depth, color, clip) appended. |
| `sub_82584F40` | `UiMovie::DrawNode` | Overridden | r3 the context, r4 the scene, r5 the node, r6 the layer: its transform, then its children last first with a copy of the context each (hidden or scaled-to-nothing ones skipped): groups and scenes walked, pictures and primitives drawn, texts queued. |
| `sub_825850F8` | `UiMovie::Draw` | Overridden | r3 the context (movie + 64), r4 the scene, r5 the textures, r6 the text list, r7 +104: the scene drawn at layer 0. |
| `sub_82585118` | `Key::EaseInteger` | Overridden | r3, r4 the keys, f1 t: an integer track's eased value (the second key's curve values), truncated. |
| `sub_82585228` | `Key::CubicFloat` | Overridden | A float track's cubic between the keys, tangents from tan of the second key's curve values; in f1. |
| `sub_82585310` | `Key::CubicInteger` | Overridden | The same for an integer track, truncated, in r3. |
| `sub_825863C8` | `XmdFile::Entry` | Overridden | r3 + [r3 + 16 + 4 x r4] while r4 is below [+12], else 0. |
| `sub_825863F0` | `XmdFile::Check` | Overridden | 16-byte aligned and IsXmd. (Its byte swap for files not reading "XMD" never runs after IsXmd and isn't kept.) |
| `sub_825865A8` | `MovieFile::Scene` | Overridden | r3 + [r3 + 4] when tagged "SCE", else 0. |
| `sub_825865E8` | `UiMovie::FixUpFile` | Overridden | A loaded movie file (16-byte aligned, IsMovieFile): its table's offsets made pointers, its scene and animations fixed up. Returns 1, or 0 when it isn't one. |
| `sub_8258BD30` | `UiMovie::FixUpAnimation` | Overridden | r3 the animation, r4 the file: its entries' offsets made pointers, its sub-animations' animations looked up (MovieFile::Animation). Returns 1. |
| `sub_8211B4E0` | `TextMap::Find` | Overridden | r3 a movie's text map (128 {id, record} pairs, count at +1024), r4 an id's address: the record's slot, or 0x826B59B8 when the id isn't there (the id read only when the count isn't 0). Called by subtick's ticker text (`src/subtick/per_frame_logic.cpp`), `ui_movies.cpp`'s node lookups and the text getters. |
| `sub_8211C0B8` | `RaceHud::DrawMirror` | Overridden | r3 a primitive node (the HUD's node 19): drawn as a quad with blend 4, key 597 (`sub_825842E0`), textured with render target 0 (`sub_8209F8A0`): each vertex at its place plus the node's, scaled by 0.8 (`sub_820AB518`), its texture coordinates its pixels over the texture's size, the node's color and alpha (times 255). The rear-view mirror. |
| `sub_8211DB38` | `UiMovie::Play` | Overridden | r3 the movie, r4 a scene's index or -1, r5 an animation's index in it (`sub_82582D68`), r6 to r9 the slot's with-next, backwards, remove-at-end and hold: unless the looping list has it (r3 then the animation), queued at the queue's end at time 0 (its length when backwards, the word as it is) and copied to the movie's last started (+40920). |
| `sub_8211DFA0` | `UiMovie::LoopNamed` | Overridden | MoveNamedAnimation to the looping list. |
| `sub_8211E0E0` | `UiMovie::FinishNamed` | Overridden | r3 the movie, r4 a named animation's index: moved to the queue, then, when queued, applied at its end (`sub_82585FD8` at its length, or 0 when backwards) and taken off. |
| `sub_8211E378` | `UiMovie::Run` | Overridden | A jump to UiMovie::Update (`sub_8211D4B8`, called through subtick's override): r3 the movie, r4 1 to draw only. |
| `sub_82582E90` | `UiMovie::SetRenderer` | Overridden | r3 and r4 stored at `0x82973078` (the renderer) and `0x8297307C` (the command list). |

### `native_data_load`

The boot data's set-up. The records' and recordings' layouts are in `game/replay_snapshots.h`; the constructors set their vectors' w words.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82245750` | `LoadingScreen::Load` | Overridden | Its two words at 0x8280EEB0 and 0x8280EE68 cleared, then movies 0x582E, 0x582D and 0x582F loaded (sub_82245658). |
| `sub_82105618` | `InputRecordings::InputRecordings` | Overridden | Its fourteen records made (two runs of seven). |
| `sub_825BF238` | `Replay826D8520::StaticInit` | Overridden | The static replay's seven records at 0x826FD440 made, its destructor registered. |
| `sub_825BF340` | `Replay82816E50::StaticInit` | Overridden | The same for the records at 0x828452D0. |
| `sub_82104410` | `Part464::Part464` | Overridden | Its vectors' w = 0. |
| `sub_82104800` | `Member4080::Member4080` | Overridden | Its two parts made as Part464s, then its own vectors' w = 0. |
| `sub_82102A10` | `Member1136::Member1136` | Overridden | Its vectors' w = 0. |
| `sub_82104D90` | `Record5232::Record5232` | Overridden | Its own vectors' w (1.0 for the first twenty), its block at +1136 made as a Member1136, then its Member4080. |
| `sub_820CCAB0` | `Bits224::Fill` | Overridden | Bits 0 to 198 set. |
| `sub_820CCB08` | `Bits480::Fill` | Overridden | Bits 0 to 453 set. |
| `sub_82102390` | `RaceData::Create` | Overridden | The race data (827,904 bytes) made into [0x826B580C]. |
| `sub_82105678` | `InputRecordings::Create` | Overridden | Made into [0x826B582C]. |
| `sub_821056D0` | `Replay::Create` | Overridden | Made with its seven records into [0x826B5830]. |
| `sub_821059D8` | `BootData::CreateObjects` | Overridden | The boot's objects, in order. |
| `sub_821067A0` | `BootData::Load` | Overridden | Once: the boot's steps. |
| `sub_820CCB60` | `Defaults::Init` | Overridden | The defaults' bit sets (the unlocked cars', two new game sets, the new game's, 48 group sets of 15 words at 0x826B2D60), each cleared and its bit ranges set; the scratch set at 0x826B3D44 ends cleared. |
| `sub_82106858` | `BootData::Unload` | Overridden | When the boot's load ran: the car parameters, the two movie lists, the save data's teardown (`sub_821057F8`) and the text freed, then `sub_821611F0`. Never runs in play. |
| `sub_8210AF60` | `Text::Unload` | Overridden | `sub_82109B68` (the font's teardown) and `sub_8210A608`. |
| `sub_82245658` | `LoadingScreen::AddMovie` | Overridden | A UI movie (40,976 bytes) made, loaded with r3's asset and appended to the list at 0x8280EE70. |
| `sub_822456E0` | `LoadingScreen::Free` | Overridden | The list's movies destroyed and freed from the last. |
| `sub_8228E658` | `OtherMovies::Add` | Overridden | As LoadingScreen::AddMovie, for the list at 0x8280F618. |
| `sub_8228E6E0` | `OtherMovies::Load` | Overridden | The three movies whose ids are at 0x8262808C loaded into the list at 0x8280F618, their words cleared, then UiMovie::SetRenderer (`sub_82582E90`) with the render manager's renderer and command list. |
| `sub_8228E7C0` | `OtherMovies::Free` | Overridden | As LoadingScreen::Free, for the list at 0x8280F618. |

### `native_race_sounds`

The race sounds' handlers. The sounds' ticks stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82173B18` | `RaceSounds::ResumeCars` | Overridden | `CarSound::ResumeAll` (`sub_8210E440`) seven times, on car slot 0 each time (as the original). |
| `sub_82173D90` | `RaceSounds::OnEffectsBankLoaded` | Overridden | [0x826C7E70] = 1. |
| `sub_82173760` | `RaceSounds::SetListener` | Overridden | The sound manager's slots 26, 25 and 27. |
| `sub_82173BE0` | `RaceSounds::OnStageCountdown` | Overridden | The effects' level back, the cars to be built, no longer held. |
| `sub_82173BB8` | `RaceSounds::OnAbortDone` | Overridden | The same, the level last. |
| `sub_82173C08` | `RaceSounds::OnRollingStartDone` | Overridden | With the cars built and not held, every car's sounds stopped; then the cars to be built again, no longer held. |
| `sub_82174828` | `RaceSounds::OnCountdown` | Overridden | `CarSound::Update` on car slot 0, a music change asked for, the rival music picked. |
| `sub_82174D20` | `RaceSounds::OnLeaveAfterRace` | Overridden | The cars' tick, a music change asked for, the effects' level back. |
| `sub_82174D88` | `RaceSounds::OnNextStage` | Overridden | Every car's sounds stopped, a music change asked for with the cars held, the rival music picked. |
| `sub_82173B60` | `RaceSounds::OnAbort` | Overridden | Every car's sounds stopped, a music change asked for with the cars held. |
| `sub_82174F80` | `RaceSounds::OnRollingStart` | Overridden | The cars built when they're to be, a music change asked for, the rival music picked. |

### `native_race_screens`

The race screens' small parts (`src/game/race_screens.cpp`, types in `src/game/race_screens.h`). The screens' ticks and draws stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82209840` | `ScreenPages::Init` | Overridden | +0, +4 and +8 = r4, +12 and +20 = -1, +24 = 0. |
| `sub_822159C8` | `RaceDialog::Destroy` | Overridden | Its list ([0x826C7F9C]) destroyed (`sub_82209958`), its three blocks freed (VirtualFree), the three globals cleared. |
| `sub_82218140` | `AfterRaceMenu::Close` | Overridden | `sub_82209958` on its list ([0x826C7FBC]). |
| `sub_8220B5C8` | `PauseMenu::Close` | Overridden | `sub_8220B3D8` on r3. |
| `sub_82209810` | `ScreenPages::Go` | Overridden | r4 the page, r5 the next (-1 none), r6 its own. |
| `sub_822097D0` | `ScreenPages::Finish` | Overridden | The pending page taken. |
| `sub_8242B828` | `MenuList::Selection` | Overridden | The word at +16. |
| `sub_822099C8` | `MenuList::SetSelection` | Overridden | +16 = r4, +20 the old one. |
| `sub_822099A8` | `MenuList::SetEnabled` | Overridden | The entry r4's flag = r5. |
| `sub_822099B8` | `MenuList::IsEnabled` | Overridden | The entry r4's flag. |
| `sub_8220A368` | `PauseMenu::AnswerPage` | Overridden | [[0x826C7F7C] + 140]. |
| `sub_822183C8` | `AfterRaceMenu::AnswerPage` | Overridden | [[0x826C7FB4] + 12]. |
| `sub_82215A38` | `RaceDialog::Answer` | Overridden | +32 cleared; 1 for choice 1, else 2. |
| `sub_82216E08` | `CareerResults::Create` | Overridden | 212 bytes of pages into [0x826C7FA8]. |
| `sub_8221CF18` | `RaceResults::Create` | Overridden | 52 bytes into [0x8280DE28]. |
| `sub_8221D5B8` | `SplitResults::Create` | Overridden | 40 bytes into [0x8280DE2C]. |
| `sub_82219508` | `AfterRaceMenu::Start` | Overridden | Its set-up, its pages to page 1. |
| `sub_82216EA0` | `CareerResults::Start` | Overridden | Its pages to page 1. |

### `native_race_hud_object`

The race HUD object. The UI movies and the HUD's set-up stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82210028` | `RaceHud::ClearWarning` | Overridden | +468 (the warning) = 0. |
| `sub_8220FFF8` | `RaceHud::StartCountdown` | Overridden | +436 = 1, then the countdown movie (+612) started (`sub_8211DB38`, r4 = -1, r5 to r9 0). |
| `sub_822104A0` | `RaceHud::StartMovie1` | Overridden | The movie at +628 started (`sub_8211DC88`, then `sub_8211DFA0`). |
| `sub_82210600` | `RaceHud::FinishMovie1` | Overridden | `sub_8211E0E0` on the movie at +628. |
| `sub_82210880` | `RaceHud::RunMovie1` | Overridden | UiMovie::Run on the movie at +628. |
| `sub_8220FA90` | `RaceScreens::DrawPauseMenu` | Overridden | The pause menu's (+1012) draw, `sub_8220D848`. |
| `sub_8220FA98` | `RaceScreens::DrawAfterRaceMenu` | Overridden | The after-race menu's (+1020) draw, `sub_822197F0`. |
| `sub_82210BA0` | `RaceHud::SetBoost` | Overridden | Node 5's +36 = the boost. |
| `sub_82210A90` | `RaceHud::ShowGauge` | Overridden | r5 into nodes 12 + r4 and 9 + r4. |
| `sub_82210B38` | `RaceHud::SetGear` | Overridden | The gear into node 11's frame. |
| `sub_82210418` | `RaceHud::SetPlayer` | Overridden | +16 the player, +440 its vehicle. |
| `sub_82213608` | `RaceHud::Touching` | Overridden | Whether r4's vehicle touches groups 0x07000000. |
| `sub_82210850` | `RaceHud::DrawWarning` | Overridden | With +468 set, the warning movie drawn. |
| `sub_82210630` | `RaceHud::Draw` | Overridden | The movie run, then node 19 drawn with +472 and +12 set. |
| `sub_82211FA0` | `RaceHud::UpdateItem` | Overridden | The race data's item shown when it's changed. |
| `sub_82211F08` | `RaceHud::TearDown` | Overridden | sub_822888E8, then its node list freed. |
| `sub_822119A8` | `PowInt` | Overridden | f1 to the power r4 (doubles). |

### `native_course_scenery`

The course scenery's pool objects and lookups, on the types in `game/course_scenery.h`. `sub_820F1038` keeps subtick's override, which calls ours.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820FB8B8` | `PoolObject::Reset` | Overridden | No flags or key, its data unused. |
| `sub_820F18B0` | `ModelList::Init` | Overridden | Its fixed list reset, its word cleared. |
| `sub_820F1310` | `Scenery::ClearGlobal` | Overridden | [0x826B51F4] cleared. |
| `sub_820F20C0` | `CourseSet::InitParts` | Overridden | Its XPR cleared, its model made, its six lists initialized, its last words cleared. |
| `sub_820F0FC8` | `CourseSet::Reset` | Overridden | Its XPR reset, model released and lists reset, then its pool object's flags, key and data. |
| `sub_820FB798` | `PoolObject::AllocData` | Overridden | r3 the object's data: r4 bytes (to 64) from it. |
| `sub_820FB878` | `PoolObject::ClearLoadFlags` | Overridden | 0x3C cleared. |
| `sub_820FB888` | `PoolObject::MarkLoaded` | Overridden | 0x2 cleared, 0x8 set. |
| `sub_824A7668` | `PoolObject::Key` | Overridden | No key (shared). |
| `sub_820F1AB0` | `AreaLights::Key` | Overridden | 4 bytes at +84. |
| `sub_820F1B68` | `AreaModel::Key` | Overridden | 4 bytes at +64. |
| `sub_820FB9F0` | `PoolObject::PoolObject` | Overridden | Constructor. |
| `sub_820FB8D0` | `PoolObject::SetKey` | Overridden | The key copied where its Key slot says. |
| `sub_820FB9E0` | `ObjectPools::Pool` | Overridden | The pool r4. |
| `sub_820FBFB8` | `ObjectPools::Object` | Overridden | The pool r4's object r5. |
| `sub_820FBEA0` | `ObjectPools::Load` | Overridden | The object, its slot 1. |
| `sub_820F13C0` | `SceneryConfig::ModelAsset` | Overridden | The model r4's r5-th asset, or -1. |
| `sub_820F1418` | `SceneryList::Clear` | Overridden | r4's +4 and +8 cleared. |
| `sub_820F1320` | `Scenery::AllocPhysical` | Overridden | Physical memory, counted in (0x826B51F8). |
| `sub_820F1378` | `Scenery::FreePhysical` | Overridden | Counted out, then freed. |
| `sub_82145350` | `Animation::Advance` | Overridden | Every frame: one step on (frame, key). |
| `sub_821453B0` | `AnimationSet::Advance` | Overridden | Each animation one step on. |
| `sub_820FB7D0` | `PoolObject::IsLoaded` | Overridden | Flags 0x9. |
| `sub_820FB7E8` | `PoolObject::IsVisible` | Overridden | Flags 0x19. |
| `sub_820FB800` | `PoolObject::IsExpiring` | Overridden | Flags 0x29. |
| `sub_820FB818` | `PoolObject::SetVisible` | Overridden | 0x10 cleared, or set once loaded. |
| `sub_820FB848` | `PoolObject::SetExpiring` | Overridden | 0x20 cleared, or set once loaded. |
| `sub_820F1098` | `AreaModel::~Model` | Overridden | The area model's model's destructor; deleted with r4 bit 0. |
| `sub_820F1188` | `AreaModel::Reset` | Overridden | Its model and XPR in use released, then `PoolObject::Reset`. |
| `sub_820F1428` | `SceneryList::LoadAll` | Overridden | Each item's object taken and loaded the first time its key is asked for (queue below 480); 1 once all are in. |
| `sub_820F1550` | `SceneryList::Stream` | Overridden | Each item's object found, or taken and loaded (queue below 960). |
| `sub_820F1650` | `SceneryList::ReleaseUnused` | Overridden | Loaded objects not seen: expiring for two updates, then released. |
| `sub_820F1718` | `SceneryList::Add` | Overridden | The item for r6, r7 gets the nearer distance (f1) and r8's bits, or is added. |
| `sub_820F17C0` | `SceneryList::Sort` | Overridden | Items r5 to r6 nearest first (quicksort, the middle as pivot). |
| `sub_820F1AC0` | `AreaLights::OnLoaded` | Overridden | `Archive::Load` callback (r6): parsed, marked loaded. |
| `sub_820F1AF8` | `AreaModel::AreaModel` | Overridden | Pool 4's constructor: a pool object, its model (vtable 0x8200673C) and XPR made. |
| `sub_820F1B78` | `AreaModel::OnLoaded` | Overridden | `Archive::Load` callback (r6): parsed, marked loaded. |
| `sub_820F1BB0` | `Scenery::UnloadPools` | Overridden | The pools shut down, pool 0's block and the lists' blocks freed, the lists cleared. |
| `sub_820F2308` | `CourseSet::CourseSet` | Overridden | Pool 2's constructor: a pool object, its parts made, its files and lists cleared. |
| `sub_820F24C8` | `Scenery::MakePoolObject` | Overridden | The pools' factory: pools 0 and 1 a pool object and a word, 2 a course set, 3 area lights, 4 an area model. |
| `sub_820F29E0` | `Scenery::Destroy` | Overridden | Both manager pointers cleared, the manager freed. |
| `sub_820F2A20` | `Scenery::Unload` | Overridden | The visible areas freed, then `Scenery::UnloadPools`. |
| `sub_820FB7C0` | `PoolObject::InUse` | Overridden | Flags 0x1. |
| `sub_820FB930` | `PoolObject::KeyEquals` | Overridden | 1 when it has no key or its key's bytes are r4's. |
| `sub_820FBAB8` | `ObjectPools::Init` | Overridden | r4 pools from r5's 24-byte descriptors in one physical block (64-byte aligned); each object made by the factory (or as a plain pool object), all free. |
| `sub_820FBC98` | `ObjectPools::Shutdown` | Overridden | Every object reset (slot 2), the block freed. |
| `sub_820FBD50` | `ObjectPools::Find` | Overridden | The first used object in use whose key is r5's; its index or -1. |
| `sub_820FBDD8` | `ObjectPools::Acquire` | Overridden | The pool's first free object: in use, its key set (r5), first on the used list; its index or -1. |
| `sub_820FBF08` | `ObjectPools::Release` | Overridden | The object reset (slot 2), first on the free list; returns the next used one. |
| `sub_820F2A68` | `Scenery::LimitLargeModels` | Overridden | With more models listed than kept (+104): large models (number from a threshold by area: 24, 11, 18, 9, 54) among those kept past the 6th (11th with two views) swapped with the next listed one that isn't large. Areas are 0 to 4 in every course. |
| `sub_820F2F68` | `Scenery::CollectAreas` | Overridden | r4 the view, r5 the viewport: each area of the course set's area file: 0 with the eye inside its rectangle, else half the far distance when the eye moved that far each way is inside, else the nearest corner; within the far distance + 70, added to the areas' list with the view's passes' bits. |
| `sub_820F31A0` | `Scenery::CollectModels` | Overridden | Each model of the listed areas: its nearest corner (0 with the eye inside), within the far distance + 50 and seen by one of the view's passes, added to the models' list. |
| `sub_820F3A40` | `Scenery::DrawArea` | Overridden | Each pass, a visible area (r5) when the pass (r4 +32), the manager (+196) and the area share a bit: the headlights' projectors, the draw keys and the pass's view; each area model drawn (shadow passes 10, 13, 16: its shapes' depth; else its textures set in the provider at +228, its shapes, then the depth after passes 0 to 2 when their post effect is there); with +196's 0x400 outside the shadow passes, its light groups. 400-byte frame as the original's. |
| `sub_820F3398` | `Scenery::RecordHeadlightProjectors` | Overridden | The pass's camera from its viewport; with a player car, the car scene's first eight car models' headlights into eight slots (0x8296DE40 on: position, beam shape, two colors, direction, extra; 0 and 1 traded in pass 2), the direction normalized as the original's vector code (bit for bit); for the first five a projector built (`sub_8257CF28`) and recorded on the command list (header 5, vertex constants, the matrix, command 36) for the render queue (key 371 in pass 2, else 150); then 0x8264EA60 the scene's +5280. Always the draw keys and 0x8264EA40 the pass's eye. 640-byte frame as the original's. |
| `sub_820F5178` | `Scenery::DrawSetLists` | Overridden | A pass of the course set's lists: the set's XPR in slot 1, the keys and view; every model of +1596, the entries of +2124 and +2652 that are on (`sub_821460E0`, `sub_821461E8`; their draw state reset), each with the identity world and range 10,000 to 50,000; then the point list's (+3180) entries that are on (`sub_821463A8`) drawn (`sub_821468E8`) with the race's current light setting. 384-byte frame as the original's. |

### `native_scene_loaders`

The scene loaders (`src/game/scene_loaders.cpp`, types in `src/game/scene_loads.h`): the loads' constructors and destructors, the wheels' and pool 4 objects' resets and releases, and the load checks. The loads stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820D3A80` | `CarModelLoads::CarModelLoads` | Overridden | Its skeleton (+32), two XPRs (+272) and two XPR lists (+320, reserved size -1) made. |
| `sub_820D3AE0` | `CarModelLoads::~CarModelLoads` | Overridden | The same destroyed, the last first. |
| `sub_820D5CB8` | `CockpitLoads::CockpitLoads` | Overridden | A car model's loads, then six part models at +512. |
| `sub_820D5D38` | `CockpitLoads::~CockpitLoads` | Overridden | The part models destroyed, the last first, then the model loads. |
| `sub_820D78F0` | `TrafficLoads::TrafficLoads` | Overridden | Skeleton at +48, XPRs at +288, lists at +336, six part models at +528. |
| `sub_820D7970` | `TrafficLoads::~TrafficLoads` | Overridden | The same destroyed, the last first. |
| `sub_820D8C28` | `WheelsLoads::WheelsLoads` | Overridden | Part models at +48, +1024 and +2144 (two each), XPRs at +2000, lists at +2048. |
| `sub_820D8CD8` | `WheelsLoads::~WheelsLoads` | Overridden | The same destroyed, the last first. |
| `sub_820D6C38` | `CarPartsLoads::CarPartsLoads` | Overridden | The 216 part models (at +5184, after their 216 load slots) made, w = 1 on the 30 points at +105424. |
| `sub_820D69B0` | `CarParts::Release` | Overridden | For each of the 216 parts: its load slot freed (`LoadSlot::Free`), its model released and cleared. r3 is 96 bytes before the loads. |
| `sub_820D6C98` | `CarPartsLoads::~CarPartsLoads` | Overridden | The 216 part models destroyed, the last first. |
| `sub_820D8810` | `Wheels::Reset` | Overridden | +24 = 0, +3268 = [0x826B2B20] (read live), `WheelsRecord::Reset` on +28, `sub_820D8760` on the loads at +192. |
| `sub_820D65F8` | `Pool4Object::Reset` | Overridden | +24 = 0, `Pool4Record::Reset` on +28, `LoadSlot::Reset` on +80, the part model at +112 released and cleared, +576 = [0x826B2B20] (read live). |
| `sub_820D66D0` | `Pool4Object::Release` | Overridden | Its slot freed, its model released, then `Pool4Object::Reset`. |
| `sub_820D8B30` | `Wheels::Release` | Overridden | The eight load slots of its loads freed, then `Wheels::Reset`. |
| `sub_820D6710` | `Pool4Object::SetRecord` | Overridden | memcpy of the 52-byte record at r4 into +28. |
| `sub_820D38C8` | `CarModel::IsLoaded` | Overridden | 0 while either of the two loads at +3952 is loading (state 2); else whether its own load's (+3728) state isn't 2. |
| `sub_820D66B8` | `Pool4Object::IsLoaded` | Overridden | Whether its load's state (+100) isn't 2. |
| `sub_8257C838` | `XPR::Count` | Overridden | +20, or 0 without a header (+16 = 0). |
| `sub_820C3A50` | `ScenePools::MakeA` | Overridden | The 140-byte first set at [0x826B2B20], +4 = 0. |
| `sub_820C3DE8` | `ScenePools::CreateA` | Overridden | The first set's counts from r4's sizes, the deferred releases' records, then each pool's objects in pages of their own, made on the pool's list with their index. |
| `sub_820C4298` | `ScenePools::ReleaseA` | Overridden | Pool r4's object r5 released as its class does, its +16 = 0. |
| `sub_820CE5C0` | `NameMatches` | Overridden | Same length, same letters ignoring case; '*' on either side matches any. |
| `sub_820D0710` | `AllocPages` | Overridden | [r3] = VirtualAlloc(0, r4, MEM_COMMIT, PAGE_READWRITE). |
| `sub_820D0768` | `XPR::FindByName` | Overridden | The first record whose name matches r4 (`NameMatches`), or -1. |
| `sub_820D1EB8` | `PartModel::WalkMaterials` | Overridden | Each shape's materials looked up; nothing kept. |
| `sub_820D37F8` | `CarModelLoads::Reset` | Overridden | Its slots reset, node values freed, XPRs released, providers reset, the decals' and sheets' words cleared. |
| `sub_820D4228` | `CarModel::SelectedTexture` | Overridden | The body XPR's texture of the record's name choice (+7232 + 4 x [+3708]), or 0. |
| `sub_820D43F0` | `CarModel::~CarModel` | Overridden | Released, its loads destroyed, off its list. |
| `sub_820D5538` | `CarModel::CarModel` | Overridden | Pool 0: node, vtable 0x82004F74, record and loads made, the matrices' and points' w = 1, then `CarModel::Reset`. |
| `sub_820D5630` | `CarModel::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820D5C70` | `Cockpit::ApplyPalettes` | Overridden | On its six arrived parts. |
| `sub_820D5EE0` | `Cockpit::Release` | Overridden | Its buffers freed and slots reset, XPRs and providers released, record and anchor reset. |
| `sub_820D5FD0` | `Cockpit::SetPaints` | Overridden | On its six arrived parts (r4 to r9). |
| `sub_820D6048` | `Cockpit::Cockpit` | Overridden | Pool 2: node, vtable 0x82005000, loads made, its record, anchor and slots reset. |
| `sub_820D6178` | `Cockpit::~Cockpit` | Overridden | Released, its loads destroyed, off its list. |
| `sub_820D6448` | `Cockpit::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820D6720` | `Pool4Object::Pool4Object` | Overridden | Pool 4: node, vtable 0x82005044, its part model made, then `Pool4Object::Reset`. |
| `sub_820D6780` | `Pool4Object::~Pool4Object` | Overridden | Its slot freed, model released, reset, part destroyed, off its list. |
| `sub_820D6870` | `Pool4Object::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820D69F8` | `CarParts::SlotOf` | Overridden | A set a part kind's slot from r5 (and r6 for kinds 1, 8, 9); above 15 r3 as it is. |
| `sub_820D6AC0` | `CarParts::Part` | Overridden | The arrived part model of kind r4, or 0. |
| `sub_820D6B68` | `CarParts::WalkMaterials` | Overridden | `sub_820D1EB8` on each arrived part (216, or 187 outside set a). |
| `sub_820D6BD0` | `CarParts::ApplyPalettes` | Overridden | `PartModel::ApplyPalette` on each arrived part. |
| `sub_820D6CE8` | `CarParts::~CarParts` | Overridden | Released, its loads destroyed, off its list. |
| `sub_820D6F08` | `CarParts::IsLoaded` | Overridden | Set a: none of 216 loading; set b: of the first 17; else 0. |
| `sub_820D7038` | `CarParts::SetPaints` | Overridden | `PartModel::SetPaint` (r4 to r9) on each arrived part. |
| `sub_820D70D8` | `CarParts::CarParts` | Overridden | Pool 1: node, vtable 0x82005074, loads made, the 216 parts reset, the record's defaults, +106000 = [0x826B2B20]. |
| `sub_820D7170` | `CarParts::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820D7228` | `Interior::OnPartLoaded` | Overridden | `Archive::Load` callback (r3 the request, r6 the interior): the matching slot's part loaded, its buffer freed, state 3. |
| `sub_820D72A8` | `Interior::Release` | Overridden | Its parts released; set a gives back the three accessories, set b the record's one. |
| `sub_820D7340` | `Interior::ApplyPalettes` | Overridden | On its six arrived parts. |
| `sub_820D7388` | `Interior::~Interior` | Overridden | Released, its parts destroyed, off its list. |
| `sub_820D7440` | `Interior::IsLoaded` | Overridden | Sets a and b: none of its parts loading; and the accessories all loaded. |
| `sub_820D74F0` | `Interior::SetPaints` | Overridden | On its six arrived parts (r4 to r9). |
| `sub_820D7568` | `Interior::Interior` | Overridden | Pool 3: node, vtable 0x8200507C, six parts made and reset, record's defaults, +3092 = [0x826B2B20]. |
| `sub_820D7610` | `Interior::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820D7760` | `TrafficCar::Reset` | Overridden | +7392 = [0x826B2B20], record reset, slots reset, XPRs and providers released, the eight release ids cleared, parts reset. |
| `sub_820D7840` | `TrafficCar::Release` | Overridden | Its eight deferred releases handed back (`sub_820C3D60`), node file freed, then `TrafficCar::Reset`. |
| `sub_820D78A8` | `TrafficCar::ApplyPalettes` | Overridden | On its six arrived parts. |
| `sub_820D7B40` | `TrafficCar::~TrafficCar` | Overridden | Released, its loads destroyed, off its list. |
| `sub_820D85C0` | `TrafficCar::TrafficCar` | Overridden | Pool 6: node, vtable 0x82005094, record and loads made, w = 1 on its points and matrices, then `TrafficCar::Reset`. |
| `sub_820D8670` | `TrafficCar::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820D8760` | `WheelsLoads::Reset` | Overridden | For both sides: slots reset, models released and cleared, XPR released, provider reset. |
| `sub_820D8D80` | `Wheels::~Wheels` | Overridden | Released, its loads destroyed, off its list. |
| `sub_820D9238` | `Wheels::Wheels` | Overridden | Pool 5: node, vtable 0x820050E0, two records reset, loads made, then `Wheels::Reset`. |
| `sub_820D92B0` | `Wheels::DeletingDestructor` | Overridden | The destructor, then deleted with r4 bit 0. |
| `sub_820F0668` | `Accessories::AllLoaded` | Overridden | Each entry there has its accessory's +1048 set. |
| `sub_82579600` | `PoolNode::PoolNode` | Overridden | r4 the list: vtable 0x82069A90, appended. |
| `sub_82579678` | `PoolNode::~PoolNode` | Overridden | Taken off its list when on one. |
| `sub_8257C858` | `XPR::RecordName` | Overridden | Record r4's name (16-byte records, +12), or 0 past the count. |
| `sub_820D0090` | `ModelPoint::Find` | Overridden | r3 a point record, r4 a part model: the record cleared; with a skeleton (+408), it posed and the node named at the record's +160 found: the record's matrix the node's, its position x 0.1, times the identity turned -pi/2 about y; +144 the position, +80 the inverse, +224 the node. 1 when found. 368-byte frame as the original's. |
| `sub_820D3B38` | `CarModel::Reset` | Overridden | +7256 = [0x826B2B20], the car record and loads reset; the tires' and seats' matrices the identity (memcpy from one built at +96 of its 304-byte frame), their positions (0, 0, 0, 1); the six name lookups -1; +7200 = 0, the anchor (0, 0, 0, 1). |
| `sub_820D61D0` | `Cockpit::OnFileLoaded` | Overridden | `Archive::Load` callback (r3 the request, r4 the buffer, r6 the cockpit): a part built, a body XPR made and set in its provider, or the node file bound and posed, then the last "acce_null" node's matrix (+3472) and the anchor (+3488) from its position x 0.1, turned. 256-byte frame as the original's. |
| `sub_820D80E0` | `TrafficCar::OnFileLoaded` | Overridden | `Archive::Load` callback (r3 the request, r6 the car): "<name>_ex" printed in its frame; entry 0 the body XPR; `sub_820ED308` given the four release words and the part order by value (r7 to r10 and the frame's +80, +88); +7056 the "<name>_ex" record; entry 1 the nodes, the four tires' matrices (+7136); their positions (+7072) x 0.1, turned, w 0; entries 2 to 7 the six parts; the buffer freed, state 3. 1,344-byte frame as the original's. |
| `sub_820D5DB0` | `Cockpit::BindTextures` | Overridden | 0 while any of its files is loading; else the interior texture ("<name>_inte_on", or "<name>_inte_on_0<n>" with the record's +20) for car blocks below 18 into +3504 and each part's +452, each part's provider the cockpit's; 1. Its 1,168-byte frame holds the name. |
| `sub_820D79E8` | `TrafficCar::BindTextures` | Overridden | 0 while its file is loading; else each part's provider the car's and `sub_820CE7B8`; with a variant (record +160 above 0) and a body texture, "<name>_ex_0<n>" into +7060 and each part's +460, +436 the body texture; 1. Its 1,184-byte frame holds the name. |

### `native_scene`

The scene's set-ups, on `SceneAssets` and `CarScene` (`game/scene.h`). Its updates stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820D0AE0` | `SceneSetUp::SceneSetUp` | Overridden | +36 and +40 cleared. |
| `sub_820D0750` | `FreePagesIn` | Overridden | The pages at [r3] freed (VirtualFree). |
| `sub_820C4E00` | `SceneAssets::SceneAssets` | Overridden | Its XPRs and records cleared, its XPR lists and part models made. |
| `sub_820C4F58` | `SceneAssets::~SceneAssets` | Overridden | Its lists, XPRs and part models destroyed, last first. |
| `sub_820C8760` | `CarScene::CarScene` | Overridden | Its assets made, its eight entries cleared, its 22 vectors' w = 1.0. |
| `sub_82579750` | `List::~List` | Overridden | sub_825795A8 on it. |
| `sub_820C4A80` | `SceneAssets::Release` | Overridden | Its load slots freed and pages released, its last part model released, then sub_820C4880. |

### `native_second_set`

The scene parts' point records (`src/game/second_set.cpp`, types in `src/game/scene_points.h`): their constructors and destructors for the car body's parts, the cockpit, the interior, the lights, the traffic cars and the wheels, the scene objects' resets and the finds; the models' stream getters and the morph's run. Each group of 240-byte point records is made with `PointRecord::PointRecord`, then given its defaults (recompiled; they name the shapes, from writable tables in `.data`). The finds look each record up in a model by node (`sub_820D0090`) or shape (`sub_820CE8F0`); some then link it (`sub_820D16B0`, a distance in f1). The defaults, the searches, the models' draws and the morph's blend stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820DF160` | `PartsPoints::InitLeftDoor` | Overridden | 25 records, w = 1 on the four points at +6016, the left door's defaults (`sub_820DE3C8`). |
| `sub_820DF1B8` | `PartsPoints::InitRightDoor` | Overridden | The same with the right door's defaults (`sub_820DE638`). |
| `sub_820E0F48` | `PartsPoints::InitHeadlights` | Overridden | 13 records, w = 1 on the four points after them, the defaults (`sub_820DF2E8`). |
| `sub_820DFBB8` | `PartsPoints::InitMufflers` | Overridden | w = 1 on 16 points, then 5 records, the defaults (`sub_820DEE88`). |
| `sub_820E25D8` | `PartsPoints::PartsPoints` | Overridden | The car body's groups in order (front bumper 11, cooler, rear bumper 7, side skirts, rear spoiler 3, bonnet 2, the doors, both doors 23, three singles, headlights, tail lights 15, fenders 7, cabin 5, engine 2, mufflers, shadows 4), each made and given its defaults. |
| `sub_820DBE00` | `CockpitPoints::CockpitPoints` | Overridden | Its groups (3, 2, 1, 2, 1, 8 records) made and given their defaults. |
| `sub_820E4BC8` | `InteriorPoints::InteriorPoints` | Overridden | A record, then five groups (12, 22, 4, 4, 4 records after a 16-byte header) made and given their defaults. |
| `sub_820DD970` | `LightPoints::LightPoints` | Overridden | Three groups of 18 records (after a 16-byte header) made and given their defaults. |
| `sub_820DC518` | `LightPoints::Reset` | Overridden | The three groups' defaults again, +8..+40 = 0. |
| `sub_820E64D8` | `TrafficPoints::TrafficPoints` | Overridden | Body (6), headlights (7) and tail lights (12) made and given their defaults. |
| `sub_820E5470` | `TrafficPoints::Reset` | Overridden | +0 = 0, the three groups' defaults again. |
| `sub_820E8668` | `WheelPoints::InitTire` | Overridden | 6 records at +368, w = 1 on the two points after them, the tire's defaults (`sub_820E70A0`). |
| `sub_820E95A8` | `WheelPoints::InitRimA` | Overridden | 7 records at +32, two points, rim A's defaults (`sub_820E7500`). |
| `sub_820EA4D8` | `WheelPoints::InitRimB` | Overridden | The same with rim B's defaults (`sub_820E77F0`). |
| `sub_820EAF18` | `WheelPoints::InitBrakes` | Overridden | 12 records at +32, two points, the brakes' defaults (`sub_820E7B78`). |
| `sub_820EC880` | `WheelPoints::WheelPoints` | Overridden | Two tires, two of each rim, two brakes. |
| `sub_820E81B0` | `WheelPoints::Reset` | Overridden | +0..+24 = 0, each part's defaults again, then +2172, +5468, +8956, +13644 = 0. |
| `sub_820E27D8` | `PartsPoints::~PartsPoints` | Overridden | Every record destroyed (the empty destructor `sub_821611F0`), the last first. |
| `sub_820DBEC8` | `CockpitPoints::~CockpitPoints` | Overridden | The same. |
| `sub_820E4CB0` | `InteriorPoints::~InteriorPoints` | Overridden | The same. |
| `sub_820DDA08` | `LightPoints::~LightPoints` | Overridden | The same. |
| `sub_820EC8F0` | `WheelPoints::~WheelPoints` | Overridden | The same. |
| `sub_820E6570` | `TrafficPoints::~TrafficPoints` | Overridden | The same. |
| `sub_820DE2B0` | `PartsPoints::FindRearSpoiler` | Overridden | 3 records by shape; records 2 and 1 linked (5.0). |
| `sub_820DE378` | `PartsPoints::FindBonnet` | Overridden | Record 0 by node, record 1 by shape. |
| `sub_820DE430` | `PartsPoints::FindLeftDoor` | Overridden | 25 records (0 and 1 by node); records 7 to 10 linked (5.0). |
| `sub_820DE6C8` | `PartsPoints::FindRightDoor` | Overridden | 25 records (0 and 1 by node). |
| `sub_820DE7E8` | `PartsPoints::FindDoors` | Overridden | 23 records by shape; records 3 to 10 linked (5.0). |
| `sub_820DEA20` | `PartsPoints::FindTailLights` | Overridden | 15 records (0 to 7 by node); records 8 to 12 linked (5.0). |
| `sub_820DEAE8` | `PartsPoints::FindFenders` | Overridden | 7 records by shape; records 3 and 4 linked (5.0). |
| `sub_820DECA0` | `PartsPoints::FindCabin` | Overridden | 5 records by shape; records 0 and 1 linked (5.0). |
| `sub_820DB778` | `CockpitPoints::FindHeadlights` | Overridden | 8 records (0 to 3 by node); records 5 and 6 linked (1.0). |
| `sub_820E2E60` | `InteriorPoints::FindMeters` | Overridden | The group's 22 records (0 to 6 by node). |
| `sub_820E3070` | `InteriorPoints::FindSeats` | Overridden | The group's 4 records (0 and 1 by node). |
| `sub_820DC468` | `LightPoints::FindGroup` | Overridden | The group's 18 records each by shape, then linked (1.0). |
| `sub_820DC570` | `LightPoints::Find` | Overridden | `LightPoints::FindGroup` on each of the three groups. |
| `sub_820E5128` | `TrafficPoints::FindBody` | Overridden | The 6 records by shape in the models r7, r4, r6, r6, r4, r5. |
| `sub_820E5328` | `TrafficPoints::FindHeadlights` | Overridden | 7 records (0 to 3 by node); record 5 linked (3.0). |
| `sub_820E53F8` | `TrafficPoints::FindTailLights` | Overridden | 12 records (0 to 7 by node); records 9 and 10 linked (3.0). |
| `sub_820E00B8` | `SceneParts::Reset` | Overridden | `sub_820D31A0` on +108, the parts' defaults (`sub_820DFC98`) on +176, its words cleared, +37284 = [0x826B2B24] (read live), memset of 68 at +32. |
| `sub_820DBA90` | `SceneCockpit::Reset` | Overridden | Its words cleared, +4316 = [0x826B2B24] (read live), `sub_820D3100` on +36, the cockpit's defaults (`sub_820DB880`) on +192, +4272 = the address of +36. |
| `sub_820E3310` | `SceneInterior::Reset` | Overridden | Its words cleared, `sub_820D3488` on +40, the interior's defaults (`sub_820E31A0`) on +160, +11668 = [0x826B2B24] (read live). |
| `sub_820DC6B0` | `SceneLights::Reset` | Overridden | Its words cleared, `Pool4Record::Reset` on +32, `LightPoints::Reset` on +96, +13160 = [0x826B2B24] (read live); the points' and each group's first word = the address of +32. |
| `sub_820E5520` | `SceneTraffic::Reset` | Overridden | `sub_820D0848` on +32, `TrafficPoints::Reset` on +3728, +9760..+9792 = 0. |
| `sub_820E8490` | `SceneWheels::Reset` | Overridden | Its words cleared, +16784 = [0x826B2B24] (read live), `WheelsRecord::Reset` on +32, `WheelPoints::Reset` on +192. |
| `sub_820D9DD0` | `SceneModelBlock::SceneModelBlock` | Overridden | w = 1 on its 38 points. |
| `sub_820D0030` | `PointRecord::PointRecord` | Overridden | w = 1 on its nine points, then `PointRecord::Clear` (`sub_820CE7C8`). |
| `sub_8257A610` | `Morph::Run` | Overridden | r4's +8 and +12 = r5 and r6, then Morph::Blend. |
| `sub_820CE698` | `PointRecord::SetNode` | Overridden | r4's name to +160. |
| `sub_820CE6C0` | `PointRecord::SetShape` | Overridden | r4's name to +192. |
| `sub_8257A360` | `Model::StreamData` | Overridden | The mesh r4's +388. |
| `sub_8257A378` | `Model::StreamSize` | Overridden | +268. |
| `sub_8257A390` | `Model::StreamData2` | Overridden | +392. |
| `sub_8257A3A8` | `Model::StreamSize2` | Overridden | +284. |

### `native_load_poll`

The records and paints set as loads finish, the car model's ready check (`src/game/load_poll.cpp`, on the types in `src/game/scene_loads.h` and `src/game/scene_points.h`). The polls and the paints' own functions stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820D8B88` | `Wheels::SetRecord` | Overridden | memcpy of the 156-byte record into +28. |
| `sub_820D8B98` | `Wheels::SetCaliperTexture` | Overridden | The record's caliper texture (+116) on the two calipers (+2336, +2800). |
| `sub_820DC7E8` | `SceneLights::SetRecord` | Overridden | memcpy of the 52-byte record into +32, then `Pool4Object::SetRecord`. |
| `sub_820DC838` | `SceneLights::ApplyRecord` | Overridden | +104..+136 = 0, `SetRecord`, the record's texture on the model at +13152. |
| `sub_820E04B0` | `SceneParts::ApplyPaint` | Overridden | `sub_820D7038([+104], the paints)`. |
| `sub_820E3E18` | `SceneInterior::ApplyPaint` | Overridden | `sub_820D74F0([+36], the paints)`. |
| `sub_820DBDA8` | `SceneCockpit::ApplyPaint` | Overridden | `sub_820D5FD0([+32])`, then `SceneInterior::ApplyPaint([+28])`, the paints passed on. |
| `sub_820E5DC0` | `SceneTraffic::ApplyPaint` | Overridden | `TrafficCar::SetPaint([+9760], the paints)`. |
| `sub_820D32A0` | `GaugeTableEntry` | Overridden | Replaces `sub_820D32A0` (unchanged). |
| `sub_820D3CE8` | `CarModel::IsReady` | Overridden | IsLoaded for set a or b ([+7256] + 8), else 0. |
| `sub_820D6F88` | `CarParts::SetVinylTextures` | Overridden | Each part model (216 in set a, 187 in set b but 7, 9, 14): +48 = the car model's +4048, `Model::SetVinylTextures` with its sheet. |
| `sub_820D7AC8` | `TrafficCar::SetPaint` | Overridden | `PartModel::SetPaint` on each of the six parts (+4256) whose slot (+4124) is 3. |
| `sub_820D8EA8` | `Wheels::ApplyPaint` | Overridden | `PartModel::SetPaintBody` on the tires (+240 with paints 1, 0, 2; +704 with 4, 3, 5) and the rims or brakes (+1216 with 0, 1, 2; +1680 with 3, 4, 5). |
| `sub_820D9830` | `SceneModel::ApplyPaint` | Overridden | Once ready, the record's three colors (+64, +80, +96, in r4 to r9): a traffic body's, or the parts', interior's and (after `CarModel::SetVinylTextures`) cockpit's. |
| `sub_820D9918` | `SceneModel::ApplyWheelPaint` | Overridden | Once ready, not traffic, with the wheels record's +4 set and wheels: the record's six wheel colors (+112 to +207) on the first set's wheels. |
| `sub_820D99D8` | `SceneModel::ApplyLightKit` | Overridden | Once ready, not traffic, with a light kit: `SceneLights::ApplyRecord(+5084, +4224)`. |
| `sub_820D9B68` | `SceneModel::ApplyPalettes` | Overridden | The first set's palettes: a traffic body's, or a car's parts', interior's and cockpit's; then `Wheels::ApplyPalette`. |
| `sub_820D9FE0` | `SceneModel::PollLoad` | Overridden | 0 with +5028 or +5040 set. Traffic (kind 3): the body (+5092, unless +5096) and then the wheels' +16780 = its tire positions; cars: the car model (IsReady and its decals' state 3), parts, interior, cockpit, lights; then the wheels (+5100, unless +5104); 0 at the first not finished. With every part's flag set (+5052, +5064, +5072, +5056, +5080 in a race, +5104, +5088) +5036 = 1; the first time it isn't, every flag set and the palettes, wheel points, parts' vinyl textures, paint, light record and wheel paint applied. 1. |
| `sub_820DBC68` | `SceneCockpit::Finish` | Overridden | Once `Cockpit::BindTextures([+32])`: the interior's +11664 = the body's +3504 and `SceneInterior::Finish`; then +4312 = 1, the body's six part models, `CockpitPoints::Find`. |
| `sub_820DD6D8` | `SceneLights::Finish` | Overridden | Once `Pool4Object::IsLoaded([+28])`: +13156 = 1, +13152 its model, the record's texture, the model's +48 = the car scene's +364, `LightPoints::Find`. |
| `sub_820DF018` | `SceneParts::SetSelectedTexture` | Overridden | Each part model present: +452 = `CarModel::SelectedTexture`. |
| `sub_820E0188` | `SceneParts::Build` | Overridden | +32 + 4k = `CarParts::Part([+104], k, ...)` for the 17 kinds by the record's choices (kind 4 none for choice 13), `PartsPoints::Reset`, the points' links (+37252 to +37268), `PartsPoints::Find`, `CarParts::WalkMaterials`, `SceneParts::SetSelectedTexture`. |
| `sub_820E2A30` | `SceneParts::Finish` | Overridden | 1 once +100; else +100 = `CarParts::IsLoaded([+104])` and, loaded with the scene's +4 0 or 1, `SceneParts::Build`. |
| `sub_820E2C08` | `InteriorDials::SetBlock` | Overridden | +4 the block, +2896 its record (`sub_820D3370`), +8 the car scene's +1132 or +1064 by `sub_820D3280`, `sub_820D3248`. |
| `sub_820E2EB0` | `InteriorMeters::SetGauge` | Overridden | Gauges 0-6 (others the original): the provider at +5296, the texture (3 at most in a race) on its point record's model and shape (records 7, 8, 11, 12, 15, 16, 17), +5328 the table entry. |
| `sub_820E3500` | `SceneInterior::SetGauges` | Overridden | Each of seven gauge choices (record +8, 12 bytes) enabled, but the second on block 5: `InteriorMeters::SetGauge` with the car scene's +4816 + 20 x image. |
| `sub_820E3D08` | `SceneInterior::Finish` | Overridden | Once `Interior::IsLoaded([+36])`: its six part models (+11632) given +32 and the cockpit's texture (+11664) or the selected one, `InteriorDials::SetBlock`, the meters' block and provider, the links (+11600 to +11608), `InteriorPoints::Find`, `SceneInterior::SetGauges`, +11660 = 1. |
| `sub_820E5658` | `SceneTraffic::Finish` | Overridden | Once `TrafficCar::BindTextures([+9760])`: its skeleton, six part models and second texture (+5184, +5188) linked, `TrafficPoints::Find`; +9792 never set. |
| `sub_820E6F10` | `SceneWheels::Finish` | Overridden | Once `Wheels::BindTextures([+28])`: +16768 = 1. |
| `sub_820E84F0` | `SceneWheels::SetUpPoints` | Overridden | Once: the points' links (+192: the car model, or the traffic record and tire positions; the record; the tire, rim and caliper models), `WheelPoints::SetUp`, `Wheels::SetRecord`, `Wheels::SetCaliperTexture`, +16772 = 1. |
| `sub_820E85E8` | `SceneWheels::ApplyPaint` | Overridden | Six paints (r4 to r10, then the caller's stack +88 to +127) on `[+28]`. |
| `sub_820D8F68` | `Wheels::ApplyPalette` | Overridden | `PartModel::ApplyPalette` on the six models, then `SetMaterials` with flags 3 and 4 (material +80, +84 = the record's two floats for the end) on the tires and rims or brakes, categories 1 and 2, front then rear. |
| `sub_820D9300` | `Wheels::BindTextures` | Overridden | 0 while the calipers' second slot is loading; else the six models' texture providers (tires and calipers the car scene's, rims or brakes the wheels' own lists), the record's tire textures on the tires, then `PartModel::SetMaterials` on both tires with only flag 4 (material +84 = 0.0). 1. |

### `native_car_makers`

The garage cars' resets.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82110370` | `GarageCar::Reset` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_825BF2C8` | `GarageCars::StaticInit` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_820D06C0` | `CarRecord::ResetBytes` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_82158AD0` | `CarModels::BlockOf` | Overridden | r3 a car id: its model block (a 71-entry table: ids 1-71), 0 out of range. |
| `sub_82158B04` |  | Overridden | `CarModels::BlockOf`'s case for id 1 (a function of its own): 1. |
| `sub_82158C70` | `SceneModelRecord::SetPaint` | Overridden | r3 a record, r4 a car, r5 the paint pick (-1 the car's, -2 its custom color): the paint's three colors (traffic: the pick only), then the wheels' colors. |
| `sub_821590E0` | `CarRecord::SetBytes` | Overridden | r3 a car record, r4 the car's options (+1568): the part order reset, then set from the option bytes; +3680 the first byte. |
| `sub_821591C8` | `SceneModelRecord::SetChoice` | Overridden | r3 out, r4:r5 a 12-byte {a, b, c} in two doublewords: none for c 0, else 1, c - 1, 3a - 3 + a step. |
| `sub_82159260` | `SceneModelRecord::SetWheels` | Overridden | r3 a record, r4 a car: the wheels' record (wheel models, calipers, each tire's rim, width mm and aspect %); r3 the car's block 2. |
| `sub_82176C90` | `GarageStage::FitCar` | Overridden | r3 the holder, r4 a car: a car data built in the frame, then the stage's car sizes (+6096 to +6204) from it. |
| `sub_82176E48` | `GarageStage::ReapplyRecord` | Overridden | The stage's scene model's paint, wheel paint, record and light kit applied again, its decals reloaded. |
| `sub_82176EB0` | `GarageStage::ReapplyRecordNoDecals` | Overridden | As `sub_82176E48` without the decals. |
| `sub_82177AB0` | `CarData::FromGarageCar` | Overridden | r4 a garage car, r5 a car data: a car built in the frame from it (Init, 1,880 bytes, Refresh), added as a racer, the car data set up from it. |
| `sub_8217D768` | `GarageStage::ReloadModel` | Overridden | r3 the holder, r4 a model block or -1: counts frames; from the fourth, once the model has loaded, releases it and sets up a new one for the block (1), else 0. |
| `sub_8217DB48` | `GarageStage::CopyRecord` | Overridden | r3 the stage's holder, r4 a record: `SceneModelRecord::Copy` onto the stage's scene model's record. |
| `sub_8217DC40` | `GarageStage::SetCar` | Overridden | r3 the holder, r4 a car, r5 the paint pick: the scene model's record from the car, copied, the stage fitted, the record re-applied. |
| `sub_8217DCB0` | `GarageStage::SetCarClass` | Overridden | As `SetCar` without the fit: the record's +3768 = 1 for a pick of 1, 2 or 4; re-applied without decals. |
| `sub_82180B40` | `GarageStage::ShowCarById` | Overridden | r3 the paint pick: the dealer's shown car built in the frame and set on the stage (`SetCar`). |
| `sub_82180BE0` | `GarageStage::ShowCarByIdWithClass` | Overridden | As `sub_82180B40` with `SetCarClass`. |
| `sub_8219D230` | `GarageScreen::ShowBusyMarker` | Overridden | Movie 6's named animations 86, 87 finished, 86 and 85 played, 85 looped. |
| `sub_8219D2D0` | `GarageScreen::HideBusyMarker` | Overridden | Movie 6's 86, 87, 85 finished, 87 played. |
| `sub_8219D350` |  | Overridden | As `sub_8219D2D0`, then 87 finished. |
| `sub_821DD738` | `TuningScreen::SetUpCarData` | Overridden | r3 the tuning screen: the selected garage car (the save's garage list walked) into the screen's car data. |
| `sub_82159880` | `SceneModelRecord::FromCar` | Overridden | r3 a scene model record, r4 a car, r5 the paint pick: the record reset; its kind (2 players, flags 0x81; 1 other cars, 0x2; 3 traffic) and model block; wheels and paint; for players the cockpit (from the aero levels), light kits, five interior choices and accessory; for cars the parts (engine stage and its class, the aero levels, or a body kit's entry from the table at 0x82009318 by model block, with the kit's flags from `AeroParts::Get82`), the 46 decals into the record's layers; the class flag (car +4 of 1, 2 or 4); for cars the pool 4 record from the car's three slots and each tire grade's class; last `CarRecord::SetBytes` (r3). |
| `sub_8219F8A0` | `RivalListScreen::Update` | Overridden | The rival list screen's update (through its vtable; the game names the screen `TXR_MenuRivalList`): the transition machine `[0x826C7EE4]` runs its transition (one of 21, given the machine's +24; done: the queued one next, or the level settles), none running: movie 6 runs unless leaving (level 4). Then with the screen's `[0x826C7EF0]` +0x3265C set: the stand's busy marker shown or hidden by `GarageStage::IsBusy`, `GarageStage::ReloadModel` until it answers, then the selected entry's car (+0x2C784 + 52 x +0x32664, its +48 to `Car::FromBuild`) built in the frame and set on the stand (`SetCar`, `Command` 54), and once shown `GarageStage::Step`; without it, a shown marker hidden (`sub_8219D350`). |

### `native_texture_compression`

The blitter's constructor and the codec's destructor.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8235F798` | `Blitter::Blitter` | Overridden | Its fields set and its calls made, in the original's order. |
| `sub_823D6F20` | `ImageCodec::~ImageCodec` | Overridden | Its fields set and its calls made, in the original's order. |

### `native_save_interface`

The save interface's queries (`src/game/save_interface.cpp`, types in `src/game/save_interface.h`). The content calls stay recompiled. `SaveGame::Write` (`sub_8228CEE8`) is here too, called from autosave's override.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820949C8` | `App::SaveDevices` | Overridden | [+8]'s slot 10. |
| `sub_820949E0` | `App::Users` | Overridden | [+8]'s slot 9. |
| `sub_82099E00` | `Users::SignedIn` | Overridden | The user r4's controller when signed in, else -1. |
| `sub_820A5970` | `SaveSlot::RequestDevice` | Overridden | The slot's +960 = 1. |
| `sub_820A59A0` | `SaveSlot::FreeBytes` | Overridden | The doubleword at +40. |
| `sub_820A59D0` | `SaveSlot::SaveAt` | Overridden | r5's save. |
| `sub_820A5A28` | `SaveSlot::Pending` | Overridden | The six requests' sum. |
| `sub_820A57A0` | `SaveDevices::SetRootName` | Overridden | r4's name to +16. |
| `sub_82112988` | `PlayTime::Update` | Overridden | sub_82110928 with its frame and r3 + 48. |
| `sub_820A07C0` | `SaveInterface::UserCount` | Overridden | The users' slot 1. |
| `sub_820A0808` | `SaveInterface::UserAt` | Overridden | The users' slot 2. |
| `sub_8228CB68` | `SaveSlots::OfController` | Overridden | The first user the controller is, or the count. |
| `sub_820A5988` | `SaveSlot::Device` | Overridden | +36. |
| `sub_820A59B8` | `SaveSlot::SaveCount` | Overridden | +984. |
| `sub_820A5A10` | `SaveSlot::State` | Overridden | +988. |
| `sub_820A03F0` | `SaveInterface::RequestDevice` | Overridden | The save devices' slot 1. |
| `sub_820A04B0` | `SaveInterface::SaveCount` | Overridden | Slot 4. |
| `sub_820A0508` | `SaveInterface::FreeBytes` | Overridden | Slot 3, its low word (the 64-bit answer was passed on whole before). |
| `sub_820A0560` | `SaveInterface::Pending` | Overridden | Slot 8. |
| `sub_820A05B8` | `SaveInterface::Device` | Overridden | Slot 2. |
| `sub_820A0610` | `SaveInterface::SaveAt` | Overridden | Slot 5. |
| `sub_8236FD60` | `word +92` | Overridden | The word at +92. |
| `sub_82473DE8` | `word +216` | Overridden | The word at +216 (a Users' count). |
| `sub_820A01A0` | `SaveInterface::Init` | Overridden | r3 (the app's interface) into 0x826AD970; the save devices' root name set to "TXR360". |
| `sub_820A01F0` | `SaveInterface::Clear` | Overridden | 0x826AD970 = 0. |
| `sub_820A0200` | `SaveInterface::SlotState` | Overridden | Slot 7 (the slot's state); past 9, 0. |
| `sub_820A02E0` | `SaveInterface::Failure` | Overridden | Slot 11 (the slot's failure); past 5, 0. |
| `sub_820A0398` | `SaveInterface::AcknowledgeFailure` | Overridden | Slot 12. |
| `sub_820A0448` | `SaveInterface::SaveFlag` | Overridden | Slot 6 (r4 an enumerated save). |
| `sub_820A0678` | `SaveInterface::Save` | Overridden | Slot 15 (RequestSave), the arguments shifted one register, the thumbnail's size on the stack (+84 of its 160-byte frame). |
| `sub_820A06F8` | `SaveInterface::Load` | Overridden | Slot 16 (RequestLoad). |
| `sub_820A0768` | `SaveInterface::Delete` | Overridden | Slot 17 (RequestDelete). |
| `sub_820A4788` | `SaveSlot::AllocBuffers` | Overridden | The saves' (r4 x 308 bytes) and their flags' arrays, operator new[]. |
| `sub_820A4A28` | `SaveDevices::OnEvent` | Overridden | On event 1, its tick handler and listener removed from the run loop (slots 4 and 3). |
| `sub_820A4A98` | `SaveSlot::WaitForUser` | Overridden | State 0: 3 once the slot's user is 0 to 3. |
| `sub_820A4AB8` | `SaveSlot::Failed` | Overridden | State 2: until the failure is acknowledged. |
| `sub_820A4E28` | `SaveSlot::WaitCloseItem` | Overridden | XGetOverlappedResult on +100: 1 done, 0 pending, else -8. |
| `sub_820A5088` | `SaveSlot::WaitWrite` | Overridden | GetOverlappedResult on the file, then GetLastError: 1, 0 pending, -10 disk full, else -9. |
| `sub_820A5218` | `SaveSlot::WaitRead` | Overridden | As WaitWrite, else -11. |
| `sub_820A5290` | `SaveSlot::WaitClose` | Overridden | The content's close waited on; done or failed, the file's handle and event closed; 1, 0, -8. |
| `sub_820A53B0` | `SaveSlot::WaitThumbnail` | Overridden | The same on +136, else -9. |
| `sub_820A54B0` | `SaveSlot::WaitDelete` | Overridden | The same on +136, else -12. |
| `sub_820A5500` | `SaveSlot::RequestSave` | Overridden | When the slot is ready: the names, data and thumbnail copied in, the save's request raised; 1, else 0. |
| `sub_820A5608` | `SaveSlot::RequestLoad` | Overridden | As RequestSave, with no thumbnail and the load's request. |
| `sub_820A5708` | `SaveSlot::RequestDelete` | Overridden | As RequestSave with the names only and the delete's request. |
| `sub_820A5810` | `SlotArray::Reset` | Overridden | The slots freed (their vector deleting destructor, or operator delete[] of an empty header), the word cleared. |
| `sub_820A5908` | `SaveDevices::Destroy` | Overridden | The listener closed, the slots freed, the vtable stepped down. |
| `sub_820A59F0` | `SaveDevices::SaveFlag` | Overridden | The slot's flag for r5's save (1 when it couldn't be opened). |
| `sub_820A5A70` | `SaveDevices::Failure` | Overridden | The slot's +992. |
| `sub_820A5A88` | `SaveDevices::AcknowledgeFailure` | Overridden | The slot's +980 = 1. |
| `sub_820A5AA0` | `SaveSlot::WaitForDevice` | Overridden | State 1: the device selector on request, else 3 once there's a device. |
| `sub_820A5B00` | `SaveSlot::Ready` | Overridden | State 3: the first request raised starts (selector, enumeration, save, load, delete). |
| `sub_820A5BE8` | `SaveSlot::EnumerateStep` | Overridden | State 5: the enumeration and its wait (`sub_820A4BF0`, `sub_820A4CA8`). |
| `sub_820A5CB0` | `SaveSlot::CheckStep` | Overridden | State 6: each save opened and closed again (`sub_820A4D38`, `sub_820A4DB8`), its flag cleared when it opened. |
| `sub_820A6238` | `SaveSlot::DeleteStep` | Overridden | State 9: the delete (`sub_820A5400`) and its wait. |
| `sub_820A6338` | `SaveDevices` deleting destructor | Overridden | Destroy, then operator delete with bit 0 of r4. |
| `sub_8228CA98` | `SaveBuffers::Create` | Overridden | When none: the 24-byte header and its three buffers (129,200, 1,334,880 and 36 bytes), VirtualAlloc. |
| `sub_8228CBB8` | `Users::Name` | Overridden | XUserGetName of player r3's controller into r4, 16 bytes. |
| `sub_8228CBD0` | `SaveBuffers::FreeReplayList` | Overridden | The replays' records freed (VirtualFree) and cleared. |
| `sub_8228CC28` | `Replays::Sort` | Overridden | The records sorted by number (+28) by swaps through a copy in its frame (memcpy). |
| `sub_8228CCE0` | `SaveBuffers::Destroy` | Overridden | The replays' records, the three buffers and the header freed; 0x8280F1F8 = 0. |
| `sub_8228CE78` | `SaveSlots::Device` | Overridden | r3 a player: SaveInterface::Device on their slot. |
| `sub_8228CEB0` | `SaveSlots::Pending` | Overridden | r3 a player: SaveInterface::Pending on their slot. |
| `sub_8228D138` | `GameData::Find` | Overridden | The game data's save among player r3's saves: 7, 8 when damaged, 3 not there (or no device), 4 busy. |
| `sub_8228D370` | `Replays::Load` | Overridden | Record r3's replay (the entry by its number) loaded into the replay buffer from player 0's slot. |
| `sub_8228D428` | `Replays::CheckRoom` | Overridden | 0 when player r3's device has room for a replay (1,334,880 bytes in blocks, `sub_82293660`), 2 if not, 3 no device, 4 busy. |
| `sub_8228D688` | `Replays::Count` | Overridden | Player 0's saves named `rep...` counted into r3; 7. |
| `sub_8228D760` | `Replays::BuildList` | Overridden | The replays' records and entries made again (with an empty first one when r3 and under 99), each save read (ReadEntry, recompiled), the records sorted; 7. |
| `sub_8228D918` | `Replays::SaveNew` | Overridden | The record being saved numbered with the first free number, then Replays::Save (recompiled). |
| `sub_8228D988` | `Replays::SaveAs` | Overridden | The record being saved numbered r3, then Replays::Save. |
| `sub_8228D9A8` | `Replays::Delete` | Overridden | The replay numbered r3 deleted from player 0's slot; 1, or 0 when not listed. |
| `sub_8228DA28` | `Replays::SaveFlag` | Overridden | SaveInterface::SaveFlag on player 0's slot with record r3's number. |
| `sub_820A6388` | `SaveDevices::SaveDevices` | Overridden | Its update and event handler added to the run loop (member delegates in its 176-byte frame; the run loop clones them), the storage devices' listener (XNotifyCreateListener), the four slots and their buffers, the state handlers, the root name's default. |
| `sub_8228D228` | `Replays::Save` | Overridden | r3 a record, r4 the data: the file name (sprintf, the time capped at 59:59.999) and display name (swprintf) built in its 480-byte frame, then SaveInterface::Save on player 0's slot (1,334,880 bytes, no thumbnail). |
| `sub_8228D4C8` | `Replays::ReadEntry` | Overridden | r3 an enumerated save, r4 a record, r5 a list entry: when player 0's save is a replay (`rep...`), its twelve numbers read from the file name (sscanf into its 240-byte frame, seven pointers on the stack) into the record, the names into the entry; 6, or 5 not a replay, 3 no device, 4 busy. |

### `native_std_list`

The C++ library's list of pointers (a list: +4 its head node, +8 its size; a node: next, previous, value). operator new and delete stay recompiled; the error paths run the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820993A8` | `PtrList::AllocateNodes` | Overridden | r3 the count: count x 12 bytes from operator new (`sub_8258C000`). |
| `sub_82099A40` | `PtrList::IncSize` | Overridden | r3 the list, r4 how many: the size up by that. |
| `sub_82099420` | `PtrList::PtrList` | Overridden | A head node linked to itself, the size 0. Returns the list. |
| `sub_82099AE0` | `PtrList::Insert` | Overridden | r4 the iterator to put it before, r5 the value's address: a node linked in, the size up by one. |
| `sub_82118970` | `PtrList::Erase` | Overridden | r3 where the result iterator goes, r4 the list, r5 the iterator: its node unlinked and freed (`sub_8258C868`), the size down by one; the result is at the next node. |

### `native_std_string`

The C++ library's narrow string (28 bytes: +4 the text in place under a capacity of 16, else its block; +20 the length; +24 the capacity). operator new and delete, memcpy_s (`sub_8258C9F0`), erase (`sub_82091460`), append from a string (`sub_820A6E18`), replace (`sub_82578698`) and the wildcard search (`sub_82577FC0`) stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820913F0` | `std::string::_Tidy` | Overridden | r4 whether built, r5 the length kept: a block's text moved into place and the block freed; the length r5, the capacity 15. |
| `sub_82091378` | `std::allocator<char>::allocate` | Overridden | r3 bytes from operator new. |
| `sub_82091520` | `std::string::_Copy` | Overridden | r4 the capacity wanted, r5 the text kept: a new block (16n - 1, or half as much again as the old), the text copied. |
| `sub_82091608` | `std::string::_Grow` | Overridden | r4 the length wanted, r5 whether to trim: a new block, a tidy into place, or the text emptied. Returns whether the length is above 0. |
| `sub_820917E8` | `std::string::assign` | Overridden | r4 another string, r5 where in it, r6 how much. Returns the string. |
| `sub_820918D0` | `std::string::assign` | Overridden | r4 text, r5 how much. Returns the string. |
| `sub_820A6F18` | `std::string::append` | Overridden | r4 text, r5 how much. Returns the string. |
| `sub_82578268` | `std::string::string` | Overridden | Empty. |
| `sub_82091AD8` | `std::string::string` | Overridden | From r4's text. |
| `sub_82578A50` | `std::string::string` | Overridden | From r4's text, with an allocator (the one above). |
| `sub_825788C0` | `std::string::string` | Overridden | From r4's string, r5 where in it, r6 how much. |
| `sub_825781F0` | `String::Find` | Overridden | r4 the pattern, r5 and r6 where the match's position and length go, r7 where to start: the wildcard search over the text. Returns the match's address or 0. |
| `sub_82578908` | `String::ReplaceAll` | Overridden | r4 the pattern, r5 the text to put in, r6 how many matches at most (0: all). Returns the string's text. |

### `native_c_runtime`

The C runtime's small functions. The heap, the formatters, the parsers, the math kernels and the copies stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82091278` | `vector constructor iterator` | Overridden | r3 the first object, r4 a size, r5 how many, r6 the constructor, called on each in order. |
| `sub_820900C0` | `vector destructor iterator` | Overridden | The same with a destructor, from the last back. |
| `sub_8258F2F8` | `wcslen` | Overridden |  |
| `sub_8258D630` | `wcscmp` | Overridden | -1, 0 or 1. |
| `sub_82592128` | `__ascii_strnicmp` | Overridden | At most r5 bytes, A to Z taken as a to z: the difference of the first that differ. |
| `sub_8258F558` | `rand` | Overridden | The thread's seed (`sub_8258FF70`'s data, +20) x 214013 + 2531011; returns bits 16 to 30. |

### `native_action_mappers`

The action mappers' set-ups, resets, small setters, copies and getters (`src/game/action_mappers.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8210AF88` | `InputClient::InputClient` | Overridden | 4 stores. |
| `sub_8210AFD8` | `InputClientList::InputClientList` | Overridden | 12 stores. |
| `sub_8210B028` | `InputClientList::Add` | Overridden | No stores; calls `sub_82582078`. |
| `sub_8210C1B0` | `ActionMapper::ResetStages` | Overridden | 150 stores; calls `sub_8210BBE8`, `sub_8210BC70`. |
| `sub_82581DE0` | `ClientNode::ClientNode` | Overridden | 3 stores. |
| `sub_82581ED0` | `ClientList::ClientList` | Overridden | 10 stores. |

### `native_ai_drivers`

The ai drivers' set-ups, resets, small setters, copies and getters (`src/game/ai_drivers.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820C9488` | `CarInput::Clear` | Overridden | 10 stores. |
| `sub_82139548` | `AiDriver::Reset` | Overridden | 15 stores; calls `sub_8213CF88`, `sub_82144330`, `sub_82145048`. |
| `sub_821397E8` | `AiDriver::InitPath` | Overridden | 246 stores. |
| `sub_82139838` | `AiDrivers::DestroyRings` | Overridden | No stores; calls `sub_820900C0`. |
| `sub_8213B190` | `AiPath::ClearCornerKind` | Overridden | 1 stores. |
| `sub_8213B1A0` | `AiPath::ClearCorners` | Overridden | 113 stores. |
| `sub_8213CF88` | `AiPath::ResetFor` | Overridden | 6 stores; calls `sub_8213BEB8`. |
| `sub_8213DFA0` | `AiSkill::Reset` | Overridden | 18 stores. |
| `sub_8213E008` | `AiSkill::Clear` | Overridden | 20 stores. |
| `sub_82144298` | `AiDriver::ClearOthers` | Overridden | 103 stores. |
| `sub_82144E18` | `AiDriver::ClearCarState` | Overridden | 126 stores. |

### `native_app_start_up`

The app start up' set-ups, resets, small setters, copies and getters (`src/game/app_start_up.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8209AD00` | `SetAppInterface84` | Overridden | 1 stores. |
| `sub_8209AD20` | `ReadError::SetApp` | Overridden | 1 stores. |
| `sub_8209F6B8` | `SetRenderApp` | Overridden | 1 stores. |
| `sub_820A7568` | `AppCameraEntry::AppCameraEntry` | Overridden | 4 stores. |
| `sub_8225D588` | `ClearWords8110` | Overridden | 2 stores. |
| `sub_82094A30` |  | Overridden | The word at +112. |
| `sub_82094A38` |  | Overridden | The word at +108. |

### `native_app_states`

The app states' set-ups, resets, small setters, copies and getters (`src/game/app_states.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820950D8` | `App::SendMessage` | Overridden | No stores; calls `sub_82094B08`. |
| `sub_820ACFA0` | `StateLayer::StateLayer` | Overridden | 4 stores; calls `sub_82099420`, `sub_820ACF40`. |
| `sub_820AD5E8` | `StateLayer state slot 5` | Overridden | No stores; calls `sub_820AD508`. |
| `sub_820ADA60` | `StateLayer::StateLayer` | Overridden | 4 stores; calls `sub_82099420`, `sub_820ACF40`, `sub_820AD958`. |
| `sub_820ADFA8` | `StateLayer state slot 8` | Overridden | No stores; calls `sub_820ADD88`. |
| `sub_820ADFB0` | `StateLayer state slot 9` | Overridden | No stores; calls `sub_820ADE98`. |
| `sub_820AE040` | `AppStates::OnUpdate` | Overridden | No stores; calls `sub_820ADD88`. |
| `sub_820AE048` | `AppStates::OnUpdateToggled` | Overridden | No stores; calls `sub_820ADE98`. |

### `native_archive`

The archive (`src/game/archive.cpp`, types in `src/game/archive.h`): the TOC lookups, `Archive::Load` and the request queue, the list it's made of, and `Ucl::Unpack` (`src/game/ucl.cpp`). `Archive::GetToc`, `Archive::GetSize`, `Archive::Load`, `Archive::Queue`, `Archive::FindLooseFile` and `Ucl::Unpack` have their rows above. The loader thread, `Archive::Init` and `Archive::Shutdown` stay recompiled (the plan: our own archive service).

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_822925A8` | `ListNode::ListNode` | Overridden | r3 the node, r4 its link. |
| `sub_822925C0` | `List::List` | Overridden | r3 the list, r4 whether it deletes its nodes; +8 the count, +12 the head, +16 the tail. |
| `sub_822925E8` | `List::Append` | Overridden | r3 the list, r4 the node: linked after the tail; 1, or 0 for no node. |
| `sub_82292640` | `List::Remove` | Overridden | r3 the list, r4 the node: unlinked (the node keeps its own link); 1, or 0 when it isn't there. |
| `sub_8209D938` | `Archive::FindSlot` | Overridden | r3 a serial (-1 for a free slot): the first of the 960 92-byte slots at `0x82694E48` holding it, or 0. |
| `sub_8209DC40` | `Archive::Cancel` | Overridden | r3 a serial: under the lock, a request the loader hasn't started (+52 0) is taken off the queue and its slot freed. 1, also when there's no such request; 0 when it's loading. |
| `sub_8209DCB0` | `Archive::CancelQueued` | Overridden | r3 whether to wait: drops every waiting request with flag 0x4000 (every `Archive::Load` sets it); then `Archive::WaitIdle` when asked. |
| `sub_8209D8D0` | `Archive::QueueLength` | Overridden | The queue's count (`0x826AA7E8` +8), under the lock. |
| `sub_8209D850` | `Archive::WaitIdle` | Overridden | With requests queued, sets `0x82694DE8` so the loader signals the idle semaphore (`0x826AA770`) once the queue drains, and waits on it. |

### `native_boot_mode`

The boot mode' set-ups, resets, small setters, copies and getters (`src/game/boot_mode.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82107480` | `ScreenMachine::ScreenMachine` | Overridden | 10 stores; calls `sub_82107230`. |

### `native_c_runtime` (tables)

More of the c runtime (`src/game/c_runtime.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_821611F0` |  | Overridden | No stores. |
| `sub_8258C530` | `_cfltcvt_init` | Overridden | 10 stores. |
| `sub_8258FD88` | `_init_pointers` | Overridden | 6 stores. |
| `sub_82590A78` | `_vsnprintf` | Overridden | No stores; calls `sub_82590980`. |
| `sub_82592BB8` |  | Overridden | 1 stores. |
| `sub_82594890` |  | Overridden | 1 stores. |
| `sub_82595730` |  | Overridden | 1 stores. |
| `sub_82598FB8` |  | Overridden | 1 stores. |
| `sub_82598FD0` |  | Overridden | 1 stores. |

### `native_camera_modes`

The camera modes' set-ups, resets, small setters, copies and getters (`src/game/camera_modes.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8214E6E8` | `CameraAnims::Clear` | Overridden | 1 stores. |
| `sub_8220F5D0` | `RivalLogo::RivalLogo` | Overridden | 5 stores; calls `sub_8220F490`. |

### `native_cameras`

The cameras' set-ups, resets, small setters, copies and getters (`src/game/cameras.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8209FAB0` | `CameraPasses::Init` | Overridden | 15 stores; calls `sub_8209FA58`. |
| `sub_820A0B50` | `CameraPass::Enable` | Overridden | 1 stores. |
| `sub_820AADF8` | `Viewport::Update` | Overridden | No stores; calls `sub_820AA508`. |

### `native_race_camera`

The race camera (`src/game/race_camera.cpp`, types in `src/game/race_camera.h`), written from SBX's code. How a frame's camera is made:

1. `RaceCamera::Update` (the race mode's Update message, r3 the race mode's member `RaceModeMember`, r4 the message dispatcher) reads the followed car's control record (`0x826B5810`, by the member's +77392): the view button (0x40) steps the player 1 viewport's mode (chase → cockpit → hood → chase; with `RaceData::camera_flags` 0x80, likely replays, chase → TV → cockpit → hood; none in split screen), button 0x80 resets the chase camera, and the look stick (+12, a halfword) turns the cockpit camera by up to 60 degrees. A mode that differs from the member's (+9628) goes to `RaceCamera::SetMode`. Then the active camera's update (vtable slot 0; two in split screen) and `Viewport::Update` on each viewport.
2. The camera mode writes its viewport's eye (+1600), target (+1616) and up (+1632) (`[0x826B29C8]`: two viewports of 1,872 bytes, then the split screen flag).
3. `Viewport::Update` (`sub_820AA508`) keeps the main view's last eye, target and up (+1760), copies the new ones into view 0, and builds the rear-view mirror's view (1) and the reflection cube's views (2-7) from them. A viewport's 20 views of 80 bytes go one to a camera pass: eye +16, target +32, up +48, roll +64, field of view +68.
4. `CameraPass::SetFromViewport` copies a view into its camera pass and builds the pass's matrices (`CameraPass::Update`).

The modes, each an object in the race mode's member (`RaceModeMember`, `src/game/game_modes.h`); +68512 and +68516 point at the active one (one a player):

| Mode | Object | Start (snaps the camera) | Update (vtable slot 0) | What |
|---|---|---|---|---|
| 0 | +12976 | `sub_82152620` | `sub_821528E0` (follows with `sub_82151708`) | Chase. Its distance is half the car's length + 2.6 m and its height half the car's height - 0.1 m (block 2's dimensions, `docs/game_data.md`), raised by one of five angles (6, 9, 12, 17, 22 degrees at `0x8200924C`, by its +372) |
| 1 | +11856 | `CockpitCamera::Start` | `CockpitCamera::Update` | Cockpit: the car model's cockpit points (below) |
| 2 | +9648, +10752 | `sub_82152D30` | `sub_82151F30` | Hood: eye (-length/10, height/2, 0) and target (length/2, height/2, 0) in car space, from block 2. Both players' in split screen |
| 3 | +67744 | `sub_82156F08` | `sub_82153E40` | Not identified (one of the 1/60 users) |
| 4 | +13360 | `sub_82153280` | `sub_82157380` (the chase camera inside it at +43136, then `sub_821542E0`, or `sub_82150D70` with +448 set) | TV cameras: the trackside zone the car is in (+452 zones of 1,064 bytes at +456) at +360 |
| 5 | +56880 | `sub_82154C58` | `sub_821541E0` → `sub_82153A10` | Scripted camera: an animation (+352) of shots (608 bytes each, the list at +952); `sub_821537E8` evaluates one, its result's bit 0x20 ends it (+360 steps). The race intros and the rival intro (which draws the rival's logo) |

The cockpit points: a table at `0x82608D20` (writable data, read live), 45 entries of 32 bytes, an eye and a target (x, y, z, 1) in car space (x forward, y up, z to the side), one per car model block (`docs/game_data.md`, "Car models": 0 RX7 ... 44 LEAD06). The block is the +48 of the car model item in the player car's scene (`sub_820C5B28`: kind 3, type 2). The right-hand-drive cars sit at z = -0.34 to -0.42; the left-hand-drive ECLI (19), WON04 (39) and LEAD04 (43) at +0.33; BOSS03 (26) at 0; CON_X (18) and BOSS20 (35) share theirs.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82157188` | `RaceCamera::SetMode` | Overridden | r3 the member, r4 the mode: both viewports' mode (+1860) and the member's (+9628) set, `sub_82290970` on +68528, the viewports' rolls reset, the mode's camera started on the followed racer's car (+9636; mode 2 in split screen: one for each player). Wrapped by `src/subtick/camera_cuts.cpp` (a camera cut), which calls our C++. |
| `sub_82157728` | `RaceCamera::Start` | Overridden | The race start's view from the options (`SaveOptions::camera_view`: 0 hood, 1 cockpit, 2 chase), hood in split screen (play mode 4 sets the split screen flag); with play mode 6, the followed racer from `sub_820FFED8`. Then SetMode, and the view kept at +9632. |
| `sub_82157868` | `RaceCamera::Restore` | Overridden | The followed racer again, then the player 1 viewport's mode back to +9632 (SetMode on the next Update), or hood for both in split screen. |
| `sub_821509F8` | `RaceCamera::ResetRolls` | Overridden | Both viewports' roll 0. |
| `sub_82152A28` | `CockpitCamera::Start` | Overridden | r3 the camera, r4 the viewport, r5 the car: set up, the viewport's car-space defaults (eye (-0.32, 1, -0.3), target (1, 1, -0.3), up (0, 1, 0), +1744 (-0.8, 1.1, 0)), the car's pose taken. |
| `sub_82151B18` | `CockpitCamera::Update` | Overridden | The field of view (55 degrees for the screen, `sub_82150A50`), near clip 0.1, roll 0; the car's cockpit points moved with the car (its rotation then its position) into the eye and target (and +1664), the up (0, 1, 0) turned with the car, the target turned about the up axis by the look angle (+1104), the reflection cube 1 m ahead of the eye. Our C++ falls back to the start's defaults if the car model isn't found (the original doesn't check). |
| `sub_820AA460` | `CameraPass::SetFromViewport` | Overridden | r3 the pass, r4 the player, r5 the view: eye, target and up copied, `CameraPass::SetFov` (`sub_820AA330`) with the view's field of view, +388 and the roll, then `CameraPass::Update`. |
| `sub_820AA288` | `Viewport::SetRoll` | Overridden | f1 brought into -pi..pi, written to every view's roll but view 18's; view 1 (the mirror) the other way. |
| `sub_820AA320` | `Viewport::SetNearClip` | Overridden | f1 to camera pass 0's +384 (likely the near clip; 0.1 in the cockpit). |
| `sub_821528E0` | `ChaseCamera::Update` | Wrapped | A camera cut when its reset (+380) is set (`src/subtick/camera_cuts.cpp`). |
| `sub_82157380` | `TvCameras::Update` | Wrapped | A camera cut when the zone (+360) or kind (+448) changes. |
| `sub_821541E0` | `ScriptedCamera::Update` | Wrapped | A camera cut at the update after its shot (+360) or animation (+352) changes. |
| `sub_82153BE0`, `sub_82153CA8` | `ScriptedCamera::NextCamera`, `PreviousCamera` | Wrapped | From its vtable slot 1 (`sub_82154230`) on pad codes 164 and 163; a camera cut at its next update. |

### `native_car_models`

The car models' set-ups, resets, small setters, copies and getters (`src/game/car_models.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820D04F0` | `LoadSlot::Reset` | Overridden | +0 = 0, +4 (the request) = -1, the size, buffer, shared flag and state 0. |
| `sub_820EE9C8` | `TGAReader::TGAReader` | Overridden | The name emptied (`_Tidy`, assign ""), the header and image cleared, +32 = 0. |
| `sub_820EEA30` | `TGAHeader::Clear` | Overridden | Every field 0 (+3, +9 left). |
| `sub_820EEBA8` | `TGAImage::Clear` | Overridden | +4 to +24 = 0. |
| `sub_820C3AF0` | `FreePagesAt` | Overridden | VirtualFree of `[r4]`. |
| `sub_820C3D60` | `DeferRelease` | Overridden | r3 the first pool set, r4 to r7 a texture's four words: into the first of the set's 4 x (+12 + +40) release records (`[+52]`, 20 bytes) whose state is 0, state 2. |
| `sub_820C4BD0` | `PlateImage` | Overridden | r3 the car scene, r4 a kind, r5 an index: the scene's TGA header (`+3732 + 20j`, j = 0, 4, 7, 11, 15 by kind + r5) copied to r6 and its pixels (`+3656 + 4j`) into `[r7]`. |
| `sub_820D0518` | `LoadSlot::ReleaseBuffer` | Overridden | Its own pages freed (VirtualFree) and +16 = 0; a shared buffer is only let go. |
| `sub_820D3998` | `TeamEmblem` | Overridden | r4 the car's block: the team emblem's sticker id for blocks 18 (178), 24-25 (167, 168), 27-35 (169-177), 36-40 (183-187), 41-44 (179-182), else -1. |
| `sub_820D41A8` | `CarModel::Release` | Overridden | The node file's and bodies' pages released, `Skeleton::FreeNodeValues`, both sheets (+6684, +6700) put off, then `CarModel::Reset`. |
| `sub_820D59C8` | `CarModel::ReloadDecals` | Overridden | The old sheet put off (`DeferRelease`), +4088 = 0, three layer and six sticker slots reset, a new sheet, the team emblem, `LoadDecals`. A record whose +3688 (car model +3720) is 1 (a plate image drawn into the sheet, `sub_820C4C60`) runs the original. |
| `sub_820ECC00` | `Texture::Free` | Overridden | The header freed (VirtualFree) and the data (`XPhysicalFree`), each cleared when set. |
| `sub_820ED268` | `CopyRect` | Overridden | Rows of 32-bit pixels copied (memcpy a row): r3 the target, r4 (x, y), r5 (width, height), r6's high word the target's pitch, r8 the source, r9 (x, y), the caller's stack +96 the source's pitch. |
| `sub_820EE958` | `TGAReader::~TGAReader` | Overridden | The file and the pixels freed (VirtualFree), the name destroyed. |
| `sub_820EEBC8` | `TGAImage::Free` | Overridden | Its pixels (+12) freed and cleared. |
| `sub_820C8B88` | `CarScene::SetUp` | Overridden | r3 the scene, r4 the 48-byte record, r5 kept at +4996: state 3, the record's words and the pools' counts (+8 to +36), +40, +44; `PaintPalette::Init`; +5008, +5012 the archive's shared buffer (`0x82694E14`, `0x82694E10`) or the record's; `CarScene::LoadAssets`; each pool's objects VirtualAlloc'd (3,744, 37,296, 4,320, 11,680, 13,168, 16,800, 9,808, 5,120 bytes) and constructed on the list at +4896 + 12 x pool, their +20 the index; then `ScenePools::CreateA([+4992], r4)`. A failed allocation is skipped (the original stores the index at address 20). |
| `sub_820D4448` | `CarModel::OnFileLoaded` | Overridden | Archive::Load's callback for set a (r3 the request, r6 the car model), in a 1,552-byte frame as the original's: sprintf of "<car>_inte_off" into +384. A body XPR2 (+3956 / +3980 the request): `XPR::Create` over its pages, the texture provider's slot 4, state 3, the pages freed; for the first, `CarModel::MakePlate` (the record's part order, 44 bytes by value: r7 to r10, the frame's +80 and +88) and the interior textures by name (+7232, and "..._0" 1 to 5 into +7236). The node file (+3732): the skeleton bound and posed, state 3, the tires', seats' and `acce_null` node matrices kept (+6720 to +7104, +7200), their positions turned a quarter about y and scaled 0.1 (+6976 to +7024, +7168, +7184 when all seven were found, +7216). A record that draws a plate image (+3720 = 1) runs the original for its first body. |
| `sub_820D4C28` | `CarModel::OnFileLoadedB` | Overridden | The same for set b: "_inte_on", the XPR made over the request's buffer (the shared one), +3968 = 0 then state 3, nothing freed. |
| `sub_820ED308` | `CarModel::MakePlate` | Overridden | r3 to r6 where textures A's and B's header and data go; the record's 11-word part order by value (W0 to W7 in r7 to r10, the first of each pair in the high word; W8, W9 at the caller's +80, W10 at +88; nothing further is read). Five of the car scene's images by W0 (`PlateImage` kinds 0, 2, 4, 3 and the base plate, kind 1, image W0 - 1 from 2) into its 1,168-byte frame; the 11 slots' places (`0x825F2AA0`, writable) sized from the images; both textures the base plate's size (`Texture::Create`), A's level 0 pitch (`GetSurfaceLevel`, `LockRect`, `UnlockRect`, `Resource::Release`); the base plate copied into both; then by slot (`CopyRect`): 0 the region's mark (30 x 33 from its sixth row) into B only; 1 the kana cell for W1 (bounds at `0x8200662C`, 12 a row); 2-4 W3-W5 (12 none); 5 the region cell W2 (6 a row); 6, 7, 9, 10 the digits W6, W7, W9, W10 (48 wide); 8 the separator (25 wide, from x 528) unless W8 is 12. Returns the frame's +192, as the original leaves it. |
| `sub_820ECE20` | `Vinyl::DecalCorner` | Overridden | r3 out, r4 the corner, r5 the center, r6 the scale, r8 the offset, f1 the angle: ((p - c) s) turned by sin and cos (`sub_8258E140`, `sub_8258E218`, each rounded to a float), the cross terms fused (fmsubs, fmadds), then + c + t. |
| `sub_820ECED8` | `SoftRaster::SampleTexel` | Overridden | r3 the image (+0 pixels, +4 width, +8 height, +12 pitch in bytes), f1, f2 u, v: bilinear: the four pixels at int(u), int(v) and the next (a negative index clamps both to 0, one past the end both to the last), mixed by the fractions times 256 (`[0x82001F84]`) along x then y. Conversions saturate as the generated fctiwz/fctidz. Returns the ARGB word; its conversion words below the caller's stack aren't written. |

### `native_car_physics` (tables)

More of the car physics (`src/game/car_physics.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820AFA60` | `Matrix::CopyRows` | Overridden | A copy between its arguments, field by field. |
| `sub_820B6730` | `RigidBody::SetFloat48` | Overridden | A copy between its arguments, field by field. |

### `native_car_sounds`

The car sounds' set-ups, resets, small setters, copies and getters (`src/game/car_sounds.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8210CF40` | `EngineSound::EngineSound` | Overridden | 22 stores. |
| `sub_8210CFB8` | `CarSound::CarSound` | Overridden | 122 stores. |
| `sub_8210D760` | `EngineLayer::Start` | Overridden | 4 stores; calls `sub_8210D1D8`. |
| `sub_8210E260` | `CarSound::Release` | Overridden | 1 stores; calls `sub_8210D670`, `sub_8258C940`. |
| `sub_8210E308` | `CarSound::Stop` | Overridden | 1 stores; calls `sub_8210D670`. |
| `sub_8210E3A8` | `CarSound::PauseAll` | Overridden | No stores; calls `sub_8210D590`. |
| `sub_8210E440` | `CarSound::ResumeAll` | Overridden | No stores; calls `sub_8210D610`. |

### `native_career`

The career' set-ups, resets, small setters, copies and getters (`src/game/career.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82111598` | `PlayTime::Mark` | Overridden | 1 stores. |
| `sub_821104A8` | `DayBlock::WinStreak` | Overridden | The halfword at +4828. |
| `sub_821104B0` | `DayBlock::BestStreak` | Overridden | The halfword at +4830. |
| `sub_821104C8` | `DayBlock::Word4832` | Overridden | The word at +4832. |
| `sub_82110A80` | `Stats::WinStreak` | Overridden | The halfword at +5970. |

### `native_collision` (tables)

More of the collision (`src/game/collision.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820BEE70` | `BoxBox::FindImpact` | Overridden | No stores; calls `sub_820BE2C8`, `sub_820BEAD8`. |

### `native_framework` (tables)

More of the framework (`src/game/framework.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82090408` | `RunFrameworkAt` | Overridden | No stores; calls `sub_8209ACB8`. |
| `sub_82090500` | `Framework::Framework` | Overridden | 46 stores. |
| `sub_8209AAC0` | `RunLoop::RunLoop` | Overridden | 1 stores; calls `sub_8209A5F8`. |

### `native_front_end_car_draw`

The front end car draw' set-ups, resets, small setters, copies and getters (`src/game/front_end_car_draw.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820DEF48` | `Plates::Show` | Overridden | 2 stores. |
| `sub_820DEF58` | `Plates::Hide` | Overridden | 2 stores. |

### `native_game_modes` (tables)

More of the game modes (`src/game/game_modes.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82106928` | `XPRHolder::XPRHolder` | Overridden | 12 stores. |
| `sub_82106B78` | `Buffers28::~Buffers28` | Overridden | No stores; calls `sub_82150C90`. |
| `sub_82106BC8` | `Member56896::Member56896` | Overridden | 21 stores; calls `sub_82106928`. |
| `sub_82106C68` | `RaceModeMember::~RaceModeMember` | Overridden | 7 stores; calls `sub_82106B78`, `sub_8257CA60`. |
| `sub_82107290` | `RaceModeMember::RaceModeMember` | Overridden | 91 stores; calls `sub_82106BC8`, `sub_82107230`, `sub_82153110`. |
| `sub_82108280` | `MachineMember::MachineMember` | Overridden | 2 stores; calls `sub_82107230`. |
| `sub_82150C90` | `Buffer28::~Buffer28` | Overridden | 1 stores; calls `sub_821506B8`. |
| `sub_82150CA0` | `Buffer28::Reset` | Overridden | 4 stores; calls `sub_821506B8`. |
| `sub_82153110` | `Member13360::Member13360` | Overridden | 69 stores; calls `sub_82150CA0`. |
| `sub_82153D78` | `Member67760::Member67760` | Overridden | 62 stores. |

### `native_input`

The input' set-ups, resets, small setters, copies and getters (`src/game/input.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_825729F8` | `InputProcessor::InputProcessor` | Overridden | 1 stores; calls `sub_82577950`. |
| `sub_82577368` | `LogicalPad::Values` | Overridden | No stores. |
| `sub_82577370` | `LogicalPad::LogicalPad` | Overridden | 9 stores. |
| `sub_825773E8` | `PhysicalPad::PhysicalPad` | Overridden | 8 stores. |
| `sub_82577950` | `InputProcessorBase::InputProcessorBase` | Overridden | 16 stores. |
| `sub_82582398` | `PhysicalPad::Values` | Overridden | No stores. |

### `native_location_screen`

The location screen' set-ups, resets, small setters, copies and getters (`src/game/location_screen.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_822254A0` | `LocationScreen::Reset` | Overridden | 6 stores; calls `sub_82225310`. |

### `native_matrix_math`

The matrix math' set-ups, resets, small setters, copies and getters (`src/game/matrix_math.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8209B158` | `SetWsToOne` | Overridden | 4 stores. |
| `sub_820A9B80` | `ObjectInverse` | Overridden | No stores; calls `sub_820A9AD8`. |
| `sub_82291120` | `Matrix::SetIdentity` | Overridden | No stores; calls `sub_82290FE0`. |
| `sub_82291218` | `Matrix::FromQuaternionAndRow` | Overridden | 4 stores; calls `sub_82291130`. |
| `sub_822913C8` | `Matrix::MultiplyReversed` | Overridden | No stores; calls `sub_82290FD8`. |
| `sub_820AB640` | `Vec3::SwapXZ` | Overridden | No stores. |
| `sub_82290FE0` | `Matrix::Set` | Overridden | A copy between its arguments, field by field. |

### `native_mode_tick`

The mode tick' set-ups, resets, small setters, copies and getters (`src/game/mode_tick.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8209CF08` | `ScreenFades::Init` | Overridden | 16 stores. |
| `sub_8209F450` | `ActionMappers::SetBlocked` | Overridden | 4 stores. |

### `native_model_animation`

The model animation' set-ups, resets, small setters, copies and getters (`src/game/model_animation.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8257EEF0` | `Skeleton::Skeleton` | Overridden | The base, +176 = 0, vtable `0x82069C30`. |
| `sub_82580088` | `SkeletonBase::~SkeletonBase` | Overridden | The base's vtable, then Free. |
| `sub_825866D8` | `TrackSet::TrackSet` | Overridden | Cleared, its values 0. |
| `sub_82586FC0` | `RecordClear` | Overridden | A 12-byte record (a physical pad's +168, +180): +0 = 0.0, +4, +8 = 0. |
| `sub_82587638` | `TrackSet::Clear` | Overridden | The section, its count and tracks 0, the start time 0.0. |
| `sub_825877D8` | `SkeletonCursor::Clear` | Overridden | Its words 0, its start time 0.0. |
| `sub_8257EF38` | `Skeleton::FreeNodeValues` | Overridden | +176 freed and cleared. |
| `sub_8257EF98` | `Skeleton::LoadNodeValues` | Overridden | r4 the part file's +0x28 section: with node values, instance 0's cursor bound to it and each entry's node value (x and z its time, y its +16). |
| `sub_8257F038` | `Skeleton::NodeCount` | Overridden | The node file's node count, 0 without node values. |
| `sub_8257F058` | `Skeleton::UpdateMatrices` | Overridden | With node values, `Skeleton::UpdateNodeMatrices`. |
| `sub_8257F070` | `Skeleton::ApplyAnimation` | Overridden | Each entry of instance 0's cursor: its node's channels sampled at the node's time (its value's z) into the node's state. |
| `sub_8257F180` | `Skeleton::~Skeleton` | Overridden | The vtable, +176 freed, the base's destructor. |
| `sub_8257F918` | `SkeletonBase::SkeletonBase` | Overridden | Vtable `0x82069C40`, +16 the identity, the view's first word cleared (`sub_82587918`), +128 to +160 cleared. |
| `sub_8257FA00` | `SkeletonBase::Free` | Overridden | The cursors, node states, bone blocks and own matrices freed and cleared. |
| `sub_82586710` | `TrackSet::Sample32` | Overridden | The same for 32-byte values, +20 the mask of the floats written. |
| `sub_825867D8` | `TrackSet::Advance32` | Overridden | The same for 32-byte values. |
| `sub_82586900` | `TrackSet::Setup32` | Overridden | As SetupFloat4 with 32-byte values (time +16). |
| `sub_82586A60` | `PrimitiveDrawer::PrimitiveDrawer` | Overridden | Vtable `0x8206EFB4`, +4 a 4-byte block (operator new) for its declaration. |
| `sub_82586D98` | `PrimitiveDrawer::SetShaders` | Overridden | Vtable slot 2: r5 its vertex shader (0 or 1), r6 its pixel shader (0 to 4) from the tables at `0x8297338C` and `0x829733A4` set on the device (r4). |
| `sub_82586E88` | `PrimitiveDrawer::Draw` | Overridden | Vtable slot 3: its declaration set, `BeginVertices` for r7 24-byte vertices of type r5, r6 copied in (memcpy, the original's return address), `EndVertices`. |
| `sub_82587120` | `TrackSet::ReleaseArray` | Overridden | Its values (a counted array) freed, +16 = 0. |
| `sub_82587180` | `TrackSet::Setup208` | Overridden | As SetupFloat4 with 208-byte values (eleven rows' last floats 1.0), then each track's time (`sub_8258BDC8`) and set-up (`sub_82587020`), both recompiled. |
| `sub_825872B8` | `TrackSet::Release` | Overridden | Its values (no header) freed, +16 = 0. |
| `sub_82587318` | `TrackSet::SetupFloat` | Overridden | As SetupFloat4 with a float a track (no header). |
| `sub_825873B8` | `TrackSet::SampleFloat4` | Overridden | r4 a track: its value (0, 0, 1), then each channel's curve (`Curve::Sample`) at the value's time into the float it names. |
| `sub_82587498` | `TrackSet::Advance` | Overridden | r4 a track, f1 the step: the time + step, looped between the track's start and end; 1 when it looped. |
| `sub_82587538` | `TrackSet::SetupFloat4` | Overridden | r4 the part file's +0x2C section: the old values freed, the section bound, a 16-byte value a track (counted; -1 bytes past `0x0FFFFFFF`) with +12 1.0, then each one's time the start time. |
| `sub_82587658` | `TrackSet::Count` | Overridden | The track count, 0 with no section. |
| `sub_825876E0` | `TrackSet::View` | Overridden | r4 a section or 0: the section, its track count's address (+16), its tracks (+20), its default start time (+4). |
| `sub_82587800` | `SkeletonCursor::View` | Overridden | As TrackSet::View, +16 = 0. |
| `sub_82587858` | `CursorCurve::Sample` | Overridden | r4 the keys, r5 the channel's key cursor, r6 the key count, f1 the time: before the first key or past the last their value; else from the cursor's pair on (wrapping) to the pair around the time, `Key::Interpolate` (recompiled). |
| `sub_8258BE30` | `TrackSet::StartTime` | Overridden | r4 a track or -1: the track's +72 or the section's +4 in f1; 0.0 with no section. |
| `sub_82586B20` | `PrimitiveDrawer::Init` | Overridden | Vtable slot 0, once at boot. Its vertex declaration from the elements at `0x8265B04C` (`CreateVertexDeclaration`, into its +4 block; `!! CREATE VERTEX DECLARATION ERROR !!` printed when it gives 0); then its two vertex shaders (`vs_3_0`, `main`, the 12-byte entries at `0x8297338C`) and five pixel shaders (`ps_3_0`, at `0x829733A4`): each compiled (`D3DXCompileShader`, answered by SBX's stored shaders, `src/hooks/shader_compiles.cpp`), the compiler's errors printed and released on a failure, the shader made from the buffer's microcode (`VertexShader::Create`, `PixelShader::Create`) into the entry's +8, the buffer released. A 160-byte frame as the original's (+84 where the errors go, +92 no constant table, the buffers at +96 and +100). |

### `native_models`

The models' set-ups, resets, small setters, copies and getters (`src/game/models.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820A0A88` | `Shaders::LoadAll` | Overridden | 1 stores; calls `sub_820A09D8`. |
| `sub_820CE6E8` | `PartModel::PartModel` | Overridden | `Model::Model`, the part's vtable `0x8200336C`, its own words cleared (+436 = -1). |
| `sub_820CE750` | `PartModel::~PartModel` | Overridden | The vtable and its words again, then `Model::~Model`. |
| `sub_820CE788` | `PartModel::Clear` | Overridden | +432 to +460 cleared, +436 = -1. |
| `sub_820CE7B8` | `Model::SetVinylTextures` | Overridden | 2 stores. |
| `sub_8256F240` | `SetSharedShaders` | Overridden | r3, r4 the shared vertex and pixel shader tables, to `0x8296DC14` and `0x8296DC18`. |
| `sub_82574180` | `ScopedLock::ScopedLock` | Overridden | r4 the lock, kept at r3 and taken. |
| `sub_825741B8` | `ScopedLock::~ScopedLock` | Overridden | Its lock let go. |
| `sub_8257A1C0` | `Model::Model` | Overridden | Vtable `0x82069AF8`, its XPR and XPR list (64 slots), skeleton and five track sets made, its words cleared. |
| `sub_8257A858` | `Model::~Model` | Overridden | `Model::Release`, the track sets, the skeleton, the XPR list and the XPR, the last first. |
| `sub_8257C780` | `XPR::XPR` | Overridden | The six words cleared. |
| `sub_8257CA60` | `XPR::~XPR` | Overridden | `XPR::Release`'s C++. |
| `sub_822A3150` | `TextureHeader::MipLevelCount` | Overridden | ((+44 >> 6) & 0xF) + 1. |
| `sub_82354B30` | `Resource::Relocate` | Overridden | r3 a resource header, r4 the data's address: by its type (`sub_822A3788`), 1 adds r4 to +24 above its low 2 bits, 2 and 8 add it, 3 and 17-20 relocate a texture (mips too when +48 has an address); others nothing. |
| `sub_8256F258` | `SharedVertexShader` | Overridden | r3 a pair: its entry of the shared vertex table, 0 before the table is set. |
| `sub_8256F280` | `SharedPixelShader` | Overridden | The same for the pixel table. |
| `sub_8256F2A8` | `VertexDeclaration` | Overridden | r3, r4, r5 a shape's three stream types: the kinds of the first and third (tables at `0x8264E960`, `0x8264E9A8`) pick a declaration from the tables around `0x8296DC54` by the second (9, 10, 5, 11); anything else prints `XLMDL::System::GetVertexDeclaration Error` and gives the default. |
| `sub_82573EF8` | `PendingFrees::IncSize` | Overridden | The size + r4; past `0x3FFFFFFF` the original's `length_error` path. |
| `sub_82573F98` | `PendingFrees::Insert` | Overridden | `std::list::_Insert`: r4 the node to insert before, r5 the value's address; a 12-byte node from operator new. r3 left the list. |
| `sub_82574010` | `Memory::FreeLater` | Overridden | r3 a block: under the lock, when its header carries `XLMM`, the header put at the end of the pending frees (the `std::list` at `0x8296E1B4`), freed some frames later. |
| `sub_82574098` | `Memory::FreePhysicalLater` | Overridden | As FreeLater, r4 (XMemFree's attributes) stored in the header first. |
| `sub_82576800` | `PointerVector::Allocate` | Overridden | operator new(4 × r4); a count whose bytes overflow takes the original's path (never seen). |
| `sub_825768C0` | `PointerVector::Clear` | Overridden | The end set to the start; returns the start. |
| `sub_82579948` | `XPRList::~XPRList` | Overridden | The vtable, the vector cleared, its storage freed (operator delete), the words cleared. |
| `sub_82579B98` | `PointerVector::InsertN` | Overridden | `_Insert_n(r4, r5, *r6)`: new storage of half again the capacity (at least the new size) with the front and back moved in, or the tail moved up in place (`memmove_s`, `copy_backward`), the copies stored. |
| `sub_82579E70` | `PointerVector::Reserve` | Overridden | `reserve(r4)`: past `0x3FFFFFFF` `_Xlen`; below the capacity nothing; else new storage, the elements copied, the old storage freed. |
| `sub_82579F48` | `PointerVector::At` | Overridden | `std::vector<T*>::at`: the element's address; `_Xran` (it throws) out of range. |
| `sub_8257A0D0` | `PointerVector::Assign` | Overridden | `assign(r4, *r5)`: cleared, then InsertN at the start. |
| `sub_8257A270` | `Model::Release` | Overridden | Its header copy, vertex data, shader pairs' four blocks and the pairs' table through FreeLater, its words cleared, its XPR released. |
| `sub_8257A3D8` | `Model::MaterialCount` | Overridden | r4 a shape, r5 the group: the shape's material block's +8 (group 0) or +12. |
| `sub_8257A420` | `Model::Material` | Overridden | r4 a shape, r5 an index, r6 the group: the material (group 1's after group 0's). |
| `sub_8257F1D8` | `Skeleton::Bind` | Overridden | SetUp for one instance, then a Vector a node (+176; x, y, z 0.0). |
| `sub_8257F2A8` | `Skeleton::ResetPose` | Overridden | With node values, each node's state back to the file's translation, angles and scale (and its +64 row with kind bit 0x40), then `Skeleton::ApplyAnimation`. |
| `sub_82580098` | `SkeletonBase::SetUp` | Overridden | r4 a node file, r5 matrices or 0, r6 the instances (1 when not above 0): `SkeletonBase::Free`, a cleared cursor an instance, the file's view, a 144-byte state a node (counted array; the rows' last floats 1.0, the cursors 0), its own matrices unless given, a 64-byte block a bone (+52 = 1), +140 the first bone's state. |
| `sub_82581AA8` | `Skeleton::UpdateNodeMatrices` | Overridden | The root's matrix × the skeleton's; in node order a plain node's (kind 4 or below) × its parent's with the translation row of the node it names, kinds 8 and 16 by `sub_82580E48` and `sub_82581540`; then the bones' (bit 0x80) × their parents'. `Matrix::Multiply` stays recompiled. |
| `sub_82587928` | `NodeFile::View` | Overridden | r3 the view, r4 a node file: its sections' offsets made addresses, +36 = +32 + 16, +44 = the node count less the bone count. |
| `sub_8257F410` | `Skeleton::LocalMatrices` | Overridden | With node values, each plain node's (kind 4 or below) angles wrapped into [-pi, pi] (pi with the angle's sign added, truncated turns of 2 pi taken off, fused) and stored back, its matrix the identity with the scale on the diagonal turned by RotateX, RotateY, RotateZ (each into the frame's matrix at +208 and multiplied in) and the file's matrix, its translation in row 3; then each bone's (bit 0x80) the same from its state without the scale or the wrap. A 384-byte frame as the original's. |

### `native_post_effects`

The post effects' set-ups, resets, small setters, copies and getters (`src/game/post_effect_objects.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8212DAC8` | `GlareDef::GlareDef` | Overridden | 20 stores. |
| `sub_82133C80` | `ColorFilter::~ColorFilter` | Overridden | 1 stores; calls `sub_82131EB0`. |
| `sub_82134E68` | `MotionBlur::~MotionBlur` | Overridden | 1 stores; calls `sub_82131EB0`, `sub_82132738`. |
| `sub_82135908` | `Glow::~Glow` | Overridden | 1 stores; calls `sub_82131EB0`, `sub_82132738`. |

### `native_race_event`

The race event' set-ups, resets, small setters, copies and getters (`src/game/race_event.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_821418A8` | `RaceEvent::RaceEvent` | Overridden | 21 stores. |

### `native_race_loading`

The race loading' set-ups, resets, small setters, copies and getters (`src/game/race_loading.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8214F450` | `CourseSetup::OnCourseReady` | Overridden | 1 stores. |
| `sub_8214F460` | `CourseSetup::Begin` | Overridden | 6 stores. |

### `native_race_overlays`

The race overlays' objects (`src/game/race_overlays.cpp`, types in `src/game/race_overlays.h`): their constructors, resets and destructors, the small setters, and the overlay views' getters. The overlays' updates and recordings stay recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8211EBB0` | `LightSprite::Reset` | Overridden | Its sprite state reset (points to (0, 0, 0, 1), flags 0, 0, 0, 1), +116 = 4, the point at +272 to (0, 0, 0, 1). |
| `sub_8211F220` | `LightSprite::LightSprite` | Overridden | `Overlay::Overlay`, vtable `0x820083D4`, w = 1 on its points. |
| `sub_8211F5F0` | `ModelSprite::Reset` | Overridden | Its sprite state (at +224) reset, then +336 = 9, +340 = 4, +344 = 9, +348 = 12, +352 = 0, +404 and +408 = -1, twelve points to (0, 0, 0, 1). |
| `sub_82122A40` | `ModelSprite::ModelSprite` | Overridden | `Overlay::Overlay`, vtable `0x82008478`, w = 1 on its points. |
| `sub_82123480` | `Overlay::SetTextures104` | Overridden | +104 = r4. |
| `sub_82125F38` | `CarOverlays::ClearViews` | Overridden | The 25 views at +4 = 0. |
| `sub_82126068` | `Overlay::Overlay` | Overridden | The base vtable `0x82008568`; the destructors call it to put it back. |
| `sub_82126278` | `ViewDesc::Clear` | Overridden | +0 and +4 = 0. |
| `sub_82126288` | `OverlayView::OverlayView` | Overridden | Vtable `0x82000A40`, +4 and +8 = 0. |
| `sub_82126478` | `OverlayView::WheelSlip` | Overridden | `sub_820BFFB8` on +4. |
| `sub_82126480` | `OverlayView::HasPoint` | Overridden | `sub_820D9A78` on +8. |
| `sub_8212ACC0` | `SmokeEmitter::SmokeEmitter` | Overridden | w = 1 on its ten points and its ten particles' points (720 bytes; particles at +240). |
| `sub_8212AD18` | `Smoke::~Smoke` | Overridden | Vtable `0x8200868C`, each of the 45 emitters' particles destroyed (`sub_820900C0`, last first), then `Overlay::Overlay`. |
| `sub_8212AE38` | `Particle::Particle` | Overridden | w = 1 on its two points (48 bytes). |
| `sub_8212B9F0` | `ParticlesB::~ParticlesB` | Overridden | Vtable `0x820086B8`, each of the 15 slots' particle destroyed (last first), then `Overlay::Overlay`. |
| `sub_8212BB98` | `Marker::Reset` | Overridden | Its sprite state (at +128) reset, the camera position (+272) to (0, 0, 0, 1), +288 = 1. |
| `sub_8212C580` | `Marker::Marker` | Overridden | `Overlay::Overlay`, vtable `0x820086EC`, w = 1 on its points. |
| `sub_8212D6F8` | `Sparks::Sparks` | Overridden | `Overlay::Overlay`, vtable `0x82008724`, w = 1 on the 200 sparks' points (144 bytes each from +128) and the 32 points after. |
| `sub_8215BDE0` | `RaceOverlays::Clear` | Overridden | The 25 overlays at r3 unset. |
| `sub_821262A8` | `OverlayView::Set` | Overridden | +4..+12 = the three words at r4. |
| `sub_82129438` | `Trail::SetLastPosition` | Overridden | +64144 = the point at r4. |
| `sub_8212A2D0` | `Smoke::SetVelocity` | Overridden | +32544 = the vector at r4. |
| `sub_8212B4D8` | `ParticlesB::SetPosition` | Overridden | +2784 = the point at r4. |
| `sub_8212BD78` | `Marker::SetCameraPosition` | Overridden | +272 = the point at r4. |
| `sub_8212CDC0` | `Sparks::SetPosition` | Overridden | +28944 = the point at r4. |

### `native_race_setup`

The race set-up's records (`src/game/race_setup.cpp`, types in `src/game/race_setup.h`): the racers' car records cleared, the racers' and the race data's entries reset, the stages and course record reset. The race set-up itself stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820CC248` | `Car::ResetDecals40` | Overridden | The 40 decals at r3 (car +288; 32 bytes each): scale 1, 1, image -1, the rest 0. |
| `sub_820CC998` | `Car::ResetDecals6` | Overridden | The same for the 6 decals at r3 (car +96). |
| `sub_820CCA18` | `Car::Reset1568` | Overridden | The car's options (+1568): bytes 0, 0, 5, 0, 0, 0, 1, 2, 1, 2, 0 and +12 = 0. |
| `sub_820CD368` | `Car::SetupVehicle` | Overridden | `Vehicle::SetupFromCar` (`sub_820C2608`) with r3 and r4 swapped. |
| `sub_820CD378` | `Car::Init` | Overridden | `Car::Clear`, then +3888 = 0, +3892 = 1. |
| `sub_820CD500` | `Car::ResetSettings` | Overridden | The car's settings (+16): nine halves at +0 = 0, bytes +56..+78 = 2, 2, 2, 2, 3, 7, fifteen 0s, 10, 0. |
| `sub_820CD590` | `Car::Clear` | Overridden | Its header cleared (+4 = -1), `sub_820CD3B8` on +1644 (r4 = 0), the settings, decals and options reset, the three slots at +1584 cleared. |
| `sub_820FF6A8` | `CourseBlock::Clear` | Overridden | 76 bytes: memset of 64, then +60..+72 = 0. |
| `sub_820FF958` | `RacerRecord::Init` | Overridden | `Car::Init`, then +3904, +3906 (halves), +3912, +3916 = -1, +3920 = 0.0, +3924 = 0. |
| `sub_820FFAC8` | `Racers::Reset` | Overridden | The 4 words after the 113 records (3936 bytes each) = 0, then each `RacerRecord::Init`. |
| `sub_821019B0` | `Stages::Reset` | Overridden | Memset of 1224; each of the 10 stages (80 bytes): +0 = 4, four (-1, 2) pairs at +44; then `CourseRecord::Reset` on +812. |
| `sub_82101EA8` | `RaceEntryPart::RaceEntryPart` | Overridden | A race entry's part (972 bytes): +792 = 0.0, +796 = 0. |
| `sub_821020F0` | `RaceEntry::Reset` | Overridden | A race entry (3936 bytes): +3908 = 1, the rest of its header 0; each of its 4 parts `sub_82102038` then +784, +788 = 12000; `sub_82101B18` on +3892. |
| `sub_82102158` | `RaceEntries::Reset` | Overridden | Memset of 381800, then each of the 97 entries' `RaceEntry::Reset`. |
| `sub_82102330` | `RaceData::ResetGlobal` | Overridden | `sub_82102278` on the race data ([0x826B580C], read live). |
| `sub_82102340` | `RaceData::RaceData` | Overridden | The 97 entries' (at +446056) parts made with `RaceEntryPart::RaceEntryPart` (vector constructor iterator). |
| `sub_82105E20` | `CourseRecord::Reset` | Overridden | `CourseBlock::Clear` on +332; the ten slots a free list (index i, next i + 1, the last -1); +40, +48 = 0, +42, +44, +46 = -1. |

### `native_race_world`

The race world' set-ups, resets, small setters, copies and getters (`src/game/race_world.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82141F90` | `RoutePosition::ToMainRoad` | Overridden | No stores; calls `sub_820FA608`. |
| `sub_821433C0` | `RaceEvent::PlaceStartsCurrent` | Overridden | No stores; calls `sub_82143120`. |
| `sub_8216E408` | `RaceWorld::OnPausedTick` | Overridden | No stores. |
| `sub_8216E930` | `SortedList::Clear` | Overridden | 3 stores. |

### `native_racer_controllers`

The racer controllers' set-ups, resets, small setters, copies and getters (`src/game/racer_controllers.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82102488` | `CarInputs::Destroy` | Overridden | No stores; calls `sub_8258C868`. |
| `sub_8215D538` | `RaceFlow::OnEnd` | Overridden | No stores; calls `sub_82102488`. |
| `sub_8215DAE0` | `RaceFlow::OnHold` | Overridden | No stores; calls `sub_8215D960`. |
| `sub_8215E808` | `RaceFlow::OnState7` | Overridden | No stores; calls `sub_8215E598`. |

### `native_scene_target`

The scene target' set-ups, resets, small setters, copies and getters (`src/game/scene_target.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8209B178` | `SceneTarget::RecordBoundTargets` | Overridden | No stores; calls `sub_8209AF68`. |

### `native_scenery_lists`

The scenery lists' set-ups, resets, small setters, copies and getters (`src/game/scenery_lists.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82145338` | `SceneryModel::ResetDrawState` | Overridden | 3 stores. |
| `sub_82146DD0` | `ModelList::LoadPoints` | Overridden | 24 stores; calls `sub_82146CD0`. |
| `sub_82146EF8` | `PassDescription::SetBillboardPass` | Overridden | 2 stores. |
| `sub_8256F6D8` | `SceneLights::RecordProjectors` | Overridden | 92 stores. |
| `sub_8209BE60` | `PassDescription::Set` | Overridden | +0 = r4, +4 = r6.|new |
| `sub_821460E0` | `ModelList::IsBEntryOn` | Overridden | r4 the entry: its bit in the race data's +24 (entries 0-2: 0x20, 0x40, 0x80; 3-7: 0x1-0x10; 8-11: 0x100-0x800).|new |
| `sub_821461E8` | `ModelList::IsCEntryOn` | Overridden | r4 the entry: play mode 5 none; 3 (and 4 at a stage of type 0) entry 24; else entry 0-23 when the course layout is its (4-11, 14, 15, 12, 13, 0, 1, 3, 2, 16-23), none with camera flag 0x80, and 16-23 only with 0x20 or 0x40.|new |
| `sub_821463A8` | `PointList::IsEntryOn` | Overridden | r4 the entry: none without race data, with camera flag 0x80 or play mode 5; race modes 4 and 7 entry n for course n (0-7); mode 3 entries 8-15 for courses 11-18, 16-19 for 21, 22, 19, 20.|new |
| `sub_82146848` | `ModelList::Release` | Overridden | Each model in list order: Model::Release, then its vtable slot 0 with 1 (the deleting destructor) when not null.|new |
| `sub_82146CD0` | `ModelList::Load` | Overridden | r4 a container: the list reset, then for each of its models (its first word, little-endian) a 448-byte scenery model (operator new, Model::Model, vtable 0x8200673C) loaded from its entry (DataFile::GetEntry, Model::Load) and put at the end; +0 the count.|new |
| `sub_8228C7F0` | `Race::CourseLayout` | Overridden | No arguments: the race's layout, or -1: mode (race +0xCA1F4) 8 by the course (+4: 16-19, 23, 23, 22, 23); modes 3, 4, 7 or a stage of type 1 (`sub_820FFD38`) by the course (0-7: 16-23, 11-22: 4-15); mode 2 by the flags at +20.|new |
| `sub_8256F4F0` | `VertexDeclarations::ForDepth` | Overridden | r3 a stream type: 11 gives `[0x8296DD30]`, others `[0x8296DD2C]`.|new |
| `sub_8257AD90` | `Recorder::RecordSceneLights` | Overridden | A chunk on the command list (`[0x8296DD78]`): command 7 (pixel c0-c62): the view (`0x8264EA00`, Matrix::Copy), the eye (`0x8264EA40`), the directions (`sub_82571500`, recompiled), the projectors; then 36. Returns the command's slot.|new |
| `sub_8257C660` | `Model::DrawShapeDepth` | Overridden | r4 the view-projection, r5 the world, r6 the shape, r7 the group (byte), r8 a mask, r9 a flag: nothing without meshes (+28) or out of view (IsShapeVisible); Recorder::RecordObject; a 2-slot queue entry (its own chunk); when the group has materials (the mesh's materials +8/+12): kinds 3 (the object's constants) and 4 (Model::RecordShapeDepth(model, shape, group 0/1, 0, mask, flag)), to the render queue (`[0x8296DD7C]` slot 4) with the key `[0x8296DD5C]` and f1 = minus the object's c3.z.|new |
| `sub_82146618` | `SceneryModel::DrawShape` | Overridden | r4 the view-projection, r5 the world, r6 the shape, r7 the depth pass, r8 pixel c76, r9 the c86 range, r10 the shared pair offset: nothing without meshes (+28) or out of view; two 3-slot entries (one chunk), Recorder::RecordObject, RecordSceneLights; per group with materials (Model::MaterialCount): kinds 3, 3, 4 (SceneryModel::RecordShape(model, shape, group, 1, r8, r9, r10)) to the render queue: group 0 with `[0x8296DD6C]` at minus the object's depth (its c3.z), group 1 with `[0x8296DD60]` at it; with r7, a 2-slot entry: 3 and 4 (Model::RecordShapeDepth(model, shape, 0, 1, 0, 0)) with `[0x8296DD68]`. 208-byte frame.|new |
| `sub_821468E8` | `PointList::Draw` | Overridden | r4 the entry, r5 the camera pass: its view-projection (+144) copied (memcpy) into a 512-byte frame at +288; model 0 (+436 = the list's +608 animations, +48 = +612 provider, +432 = 1) at each point of groups +616 to 2, model 1 at group 3's (16 bytes a point: x, y, z, the turn in degrees); a point with y below -1000 skipped; else identity, RotateY(w × 0.0174533), Multiply, RotateY(-pi/2), Multiply, MultiplyTranslation (the point, w 1), SceneryModel::DrawShape(model, +288, +80, 0, 0, 0, the range (10000, 50000, 0, 0), 0).|new |
| `sub_8257C430` | `Model::DrawShape` | Overridden | As SceneryModel::DrawShape with Model::RecordShape (r8, r9 only) and the groups tested by the mesh's material counts.|new |
| `sub_82145460` | `SceneryModel::RecordShape` | Overridden | r4 the shape, r5 the group, r6 set the shaders, r7 pixel c76, r8 the c86 range, r9 the shared pair offset: 0 without materials; one chunk: the vertex declaration (`sub_8256F2A8`), 2 or 3 streams, a skinned mesh's indices, samplers 0-3's state 28 (the mesh's +432), 16/20 = 4 and 36 = 3; each material: shaders (own pair, or shared + r9), vertex c9-c13 (texture colors, material animation), the model's slot 1 hook (shape, index, &c13, &blend), pixel c63-c67, the blend (table 0x82008DAC), group 0's 96/100, cull, textures (the provider's slot 5; animated through AnimationSet::CurrentFrame) with sampler states 0, 4 and 24 (the model's +432), environment textures (flags 0x2: slot 6, 0x4: slot 5, stage 6), c86/c76, the draw, c86/c76 put back (0x8264EA80, 0x8264EA90); samplers' 16/20 back to 1; 36. 304-byte frame (+80 blend, +84 header, +96 c67, +112 c64).|new |
| `sub_82571500` | `SceneLights::RecordDirections` | Overridden | r3 the constants (35 vectors): the 3 sun directions at 0x8296DDB0 normalized, 8 + 8 colors (0x8296DE40, 0x8296DEC0) copied, the 8 projector directions at 0x8296E040 normalized, their 8 colors (0x8296E0C0) copied. Normalized: xyz length's reciprocal square root with one Newton step, all four lanes scaled; zero length gives zeros; in the vector unit's own steps (flush mode on), bit for bit.|new |

### `native_sound_cues`

The sound cues' set-ups, resets, small setters, copies and getters (`src/game/sound_cues.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82105B48` | `SoundBankHolder::SoundBankHolder` | Overridden | 3 stores. |
| `sub_82117558` | `Sound::Start` | Overridden | 1 stores. |
| `sub_821178C8` | `BankPair::Release` | Overridden | 1 stores. |
| `sub_82118010` | `AudioManager::GetEngine` | Overridden | No stores. |
| `sub_825820C8` | `Emitter::Emitter` | Overridden | 59 stores. |
| `sub_82582B00` | `WaveBankHolder::WaveBankHolder` | Overridden | 4 stores. |
| `sub_82582B60` | `Listener::Listener` | Overridden | 14 stores. |
| `sub_821178D8` | `BankPair::SoundBank` | Overridden | The word at +76. |
| `sub_821176D0` | `Sound::Set3D` | Overridden | A copy between its arguments, field by field. |
| `sub_82117F70` | `AudioManager::SetListenerPosition` | Overridden | A copy between its arguments, field by field. |
| `sub_82117F78` | `AudioManager::SetListenerOrientation` | Overridden | A copy between its arguments, field by field. |
| `sub_82117F80` | `AudioManager::SetListenerVelocity` | Overridden | A copy between its arguments, field by field. |

### `native_stand_models`

The stand models' set-ups, resets, small setters, copies and getters (`src/game/stand_models.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820D8BE0` | `Wheels::SetTireTextures` | Overridden | PartModel::SetTexture on the front and rear tires with their grades, every pass. |
| `sub_820E7058` | `SceneWheels::SetTireGrades` | Overridden | The front and rear grades (r4, r5) into its record, Wheels::SetRecord, then SetTireTextures. |
| `sub_820DEF68` | `SceneParts::LightValue` | Overridden | f1 = the parts points' float after the headlights (+27664). |
| `sub_820D9C08` | `SceneModel::ApplyInterior` | Overridden | SceneInterior::SetRecord on its interior (+5068) with its interior record (+3804). |
| `sub_820E34A8` | `SceneInterior::SetRecord` | Overridden | The 120-byte record copied (memcpy) to +40 and to the copy at [+36] + 28. |
| `sub_820E6F68` | `SceneWheels::SetSizes` | Overridden | +104 = r4, +108 = r5, the front and rear sizes (12 bytes each in r6:r7 and r8:r9) into +76 and +88, then Wheels::SetRecord. |
| `sub_820E6FF8` | `SceneWheels::SetWord68` | Overridden | The wheels record's +68 = r4, then Wheels::SetRecord. |
| `sub_820E7010` | `SceneWheels::SetCaliperTexture` | Overridden | The calipers' texture (+88 of the record) = r4, Wheels::SetRecord and SetCaliperTexture. |
| `sub_82176BE8` | `GarageStage::FitAxles` | Overridden | Car::SetupVehicle of the car into a car data on the 1,968-byte frame; its two axles' first three floats into the stage's +6176 and +6192 (w 1.0). |
| `sub_82176F10` | `GarageStage::ReloadDecals` | Overridden | SceneModel::ReloadDecals on the stage's model. |
| `sub_82176F50` | `GarageStage::SetPart` | Overridden | The parts record's word for slot r4 (0 to 15; 6 and 7, 8 and 9 share; 13 none) = r5, then ApplyRecord. |
| `sub_82177038` | `GarageStage::SetInteriorClass` | Overridden | The interior's class (+3808): 0 below 3, 1 below 7, else 2. Not re-applied; returns the model. |
| `sub_821770A0` | `GarageStage::SetInteriorWord` | Overridden | Kind r4 1, 4 or 5: +3908, +3900 or +3896 = r5; then ApplyRecord. |
| `sub_82177120` | `GarageStage::SetWheelExtras` | Overridden | +4136 = r4, +4156 = r5, each given to the wheels (SetWord68, SetCaliperTexture). |
| `sub_82177180` | `GarageStage::SetTireGrade` | Overridden | The front (1), rear (2) or both (0) tires' grade (+4080, +4096) = r5; then SceneWheels::SetTireGrades. |
| `sub_82177210` | `GarageStage::SetInteriorLevel` | Overridden | +3908 = 0, 1 or 2 by r4 (below 3, below 6); +3912 = 0 below 1, else 1; then ApplyRecord. |
| `sub_821772A0` | `GarageStage::SetInteriorPair` | Overridden | The interior's part index (+3904) = r4, +3900 = r5; then ApplyRecord. |
| `sub_82177300` | `GarageStage::SetAccessory` | Overridden | The interior's accessory (+3916) = r4 and +3920 = r5, then SceneModel::ApplyInterior. |
| `sub_82177360` | `GarageStage::SetChoice` | Overridden | The interior's choice r4 (+3812 + 12 x r4) = (1, r5, r6); then ApplyRecord. |
| `sub_821773C8` | `GarageStage::ClearChoice` | Overridden | The interior's choice r4 off; then ApplyRecord. |
| `sub_82177420` | `GarageStage::SetDecalWord` | Overridden | +3736 = r4, then SceneModel::ReloadDecals. |
| `sub_82177470` | `GarageStage::SetLightsOn` | Overridden | r4 set: +4976 = 1, +4980 = 0.0; else +4976 = 0, +4980 = the parts' light value. Returns the model. |
| `sub_821774F8` | `GarageStage::SetAero` | Overridden | The car record's +3680 word (+3728) = r4; then ApplyRecord. |
| `sub_82177548` | `GarageStage::SetAero2` | Overridden | +3732 = r4; then ApplyRecord. |
| `sub_821775E8` | `GarageStage::SetWord5016` | Overridden | The model's +5016 = r4. |
| `sub_82177630` | `GarageStage::ClearLightKit` | Overridden | The light kit's group r4 (0 to 2, 3 all) texture word cleared, then SceneModel::ApplyLightKit. |
| `sub_821777F8` | `GarageStage::SetParts` | Overridden | Six parts record words from r4 to r9, the aero set's word 0, then ApplyRecord. |
| `sub_82177868` | `GarageStage::SetDecalA` | Overridden | Decal r4 (0 to 2: layer 2, 1, 0): r5, r6 (the flip), f1 and f2 (position), f3 (angle), f4 (size; its x mirrored by the flip). Returns the model. |
| `sub_82177928` | `GarageStage::SetDecalB` | Overridden | As SetDecalA on layers 5, 4, 3. |
| `sub_821779E8` | `GarageStage::AddSecondWheels` | Overridden | SceneModel::AddSecondWheels on the stage's model. |
| `sub_82177A28` | `GarageStage::ApplyWheelPaint` | Overridden | SceneModel::ApplyWheelPaint on the stage's model. |
| `sub_82177A68` | `GarageStage::SetWheelWord` | Overridden | The wheels record's +8 (+4076) = r4. |
| `sub_82177C98` | `GarageStage::SecondWheelsPending` | Overridden | The model's +5032. |
| `sub_821790C8` | `GarageStage::SetColor0` | Overridden | (f1, f2, f3, 1.0) into the car record's first paint color (+64), then SceneModel::ApplyPaint. |
| `sub_82179158` | `GarageStage::SetColor1` | Overridden | As SetColor0 on the second color (+80). |
| `sub_821791E8` | `GarageStage::SetColor2` | Overridden | As SetColor0 on the third color (+96). |
| `sub_82179278` | `GarageStage::SetColor3` | Overridden | As SetColor0 (+64), then SceneModel::ApplyWheelPaint. Never passed by the game. |
| `sub_82179308` | `GarageStage::SetColor4` | Overridden | As SetColor1 (+80), then ApplyWheelPaint. Never passed. |
| `sub_82179398` | `GarageStage::SetColor5` | Overridden | As SetColor2 (+96), then ApplyWheelPaint. Never passed. |
| `sub_82179428` | `GarageStage::SetSticker` | Overridden | Sticker r5 of area r4 (layer 6 x (area + 1) + slot; other areas the last group): its kind by the area, r6, r7, the position in r8:r9, f1, the size at the caller's r1 + 96. Returns the model. |
| `sub_8217BAA8` | `GarageStage::SetAeroSet` | Overridden | The selected garage car's body kit (Params::GetCarParts, AeroParts::Get82 of r4 + 1, on the 224-byte frame); the aero sets' entry (0x82009990, 24 bytes, by the model's block and r4) into the parts record (-1 kept; the fifth and sixth from r5, r6); +3768 = r4 + 1 when the kit's word +24 has 8. Then ApplyRecord. |
| `sub_8217BC20` | `GarageStage::SetWheel` | Overridden | Wheel r4 (0 the spec's stock wheel, above 178 the last; its model from the 179-entry table) on the front (1), rear (2) or both (0): the models, colors (hue, saturation and value / 255) and two floats into the record; the car's (r6) tire sizes (rim, width x 1000, profile x 100); FitAxles, SetSizes. Runs on the original's 3,120-byte frame. A negative r4 runs the original. |
| `sub_821CADC8` | `TuningScreen::StepSecondWheels` | Overridden | Not pending and step (+25244) 1: AddSecondWheels, step 2; else not pending and step 2: ApplyWheelPaint, step 0. |
| `sub_821CAF70` | `TuningScreen::SetTireGrade` | Overridden | Tire choice r5 (0 to 4) as grade 0, 1, 1, 2, 2 for r4 0 to 2; else nothing. |
| `sub_821DDE78` | `TuningScreen::PreviewWheel` | Overridden | The selected garage car copied into a car on the 4,016-byte frame (Car::Init, 1,880 bytes, flags +3888|= 1, Car::Refresh), then SetWheel. |
| `sub_8217DB98` | `GarageStage::MakeModel` | Overridden | Unless the stage has its model (+16): a record on its 4,384-byte frame (+80; cleared, constructed, reset, its block = r4), pool 7 claimed into the stage's +8 and set up from it; the model's +4296 and the stage's +6056 = r5, +16 = 1, `[0x826C7E88]`'s +20 = 1. The record's bytes its reset leaves are 0 (the original's were stack leftovers; the one the model keeps, the cockpit record's +20, is never read before it's set). |

### `native_strings_and_font` (tables)

More of the strings and font (`src/game/strings_and_font.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_8210A888` | `Text::Draw` | Overridden | r4 the style, r5 the text: Font::Draw's C++ on the font.|moves from `native_strings_and_font (tables)` |
| `sub_8210AF20` | `Text::Load` | Overridden | Font::Load's C++ (pages 0-3), then Strings::Load (`sub_8210AA00`, files 0 and 15).|moves from `native_strings_and_font (tables)` |

### `native_track_collision`

The track collision' set-ups, resets, small setters, copies and getters (`src/game/track_collision.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820F84E0` | `TrackRoute::Prepare` | Overridden | 2 stores. |
| `sub_820F84F8` | `TrackRoute::Free` | Overridden | 1 stores; calls `sub_82292D70`. |
| `sub_820FF100` | `TrackCollision::Unload` | Overridden | No stores; calls `sub_820B8550`, `sub_820F6820`, `sub_820F84F8`, `sub_8258C868`. |

### `native_track_zones` (tables)

More of the track zones (`src/game/track_zones.cpp`): set-ups, resets, small setters, copies and getters, generated from SBX's code and checked against the recompiled functions.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_822889F0` | `TrackZones::TrackZones` | Overridden | 70 stores. |

### `native_tuning_pages`

The tuning pages' set-ups, resets, small setters, copies and getters (`src/game/tuning_pages.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_821F50F8` | `DecalImages::RestoreStickerMovie` | Overridden | No stores; calls `sub_82205FA0`. |
| `sub_821FC6A0` | `TuningScreen::SetUpVinylBusyIcon` | Overridden | 5 stores. |
| `sub_82205FA0` | `DecalImages::RestoreMovieTexture` | Overridden | No stores; calls `sub_821F6CD8`, `sub_82202780`. |

### `native_tuning_screen`

The tuning screen' set-ups, resets, small setters, copies and getters (`src/game/tuning_screen.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_821FC1C0` | `TuningScreen::DebugLayerSet` | Overridden | No stores. |
| `sub_821FC240` | `TuningScreen::DebugEditPosition` | Overridden | No stores. |
| `sub_821FC2B8` | `TuningScreen::DebugEditView` | Overridden | No stores. |

### `native_type67_modes`

The type67 modes' set-ups, resets, small setters, copies and getters (`src/game/type67_modes.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82178328` | `GarageStage::ResetPose` | Overridden | 14 stores. |
| `sub_82179E58` | `CameraPreset::CameraPreset` | Overridden | 15 stores. |
| `sub_8217E808` | `GarageCamera::Init` | Overridden | 3 stores; calls `sub_8217B770`, `sub_8217E1C0`. |
| `sub_82184340` | `Dialog::OpenMovies` | Overridden | No stores; calls `sub_8211E9C8`. |
| `sub_821B9708` | `ControllerOptions::OnLeft` | Overridden | 2 stores; calls `sub_82292D70`. |
| `sub_821BABA8` | `ControllerOptions::Open` | Overridden | 1 stores. |
| `sub_8222FA58` | `Type6Dialog::Shown` | Overridden | 1 stores. |
| `sub_82231C80` | `Type6Screen0::OnLeft` | Overridden | 3 stores; calls `sub_82292D70`. |
| `sub_8223DDA8` | `PanelScreen::InitWords` | Overridden | 2 stores. |
| `sub_8223F558` | `Type7Dialog::Shown` | Overridden | 1 stores. |
| `sub_82241948` | `Type7Screen0::OnLeft` | Overridden | 3 stores; calls `sub_82292D70`. |
| `sub_82274630` | `ScreenMachine::Wait` | Overridden | 1 stores. |

### `native_vehicle_accessors`

The vehicle accessors' set-ups, resets, small setters, copies and getters (`src/game/vehicle_accessors.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82098DE8` | `RigidBody::Force` | Overridden | No stores. |
| `sub_820B65A0` | `Contact::Position` | Overridden | No stores. |
| `sub_820B6710` | `RigidBody::Field80` | Overridden | No stores. |
| `sub_820B6718` | `RigidBody::Velocity` | Overridden | No stores. |
| `sub_820B6720` | `RigidBody::Field144` | Overridden | No stores. |
| `sub_820B6728` | `RigidBody::Field160` | Overridden | No stores. |
| `sub_820B67C8` | `RigidBody::Orientation` | Overridden | No stores. |
| `sub_820B67D0` | `RigidBody::Matrix` | Overridden | No stores. |
| `sub_820B67E8` | `RigidBody::AngularVelocity` | Overridden | No stores. |
| `sub_820B67F8` | `RigidBody::AddForce` | Overridden | No stores; calls `sub_82291DF8`. |
| `sub_820B6800` | `RigidBody::AddTorque` | Overridden | No stores; calls `sub_82291DF8`. |
| `sub_820B7B60` | `CollisionObject::SetWord264` | Overridden | 1 stores. |
| `sub_820B8600` | `Collision::SetGlobal78` | Overridden | 1 stores. |
| `sub_820B8610` | `Collision::SetGlobal7C` | Overridden | 1 stores. |
| `sub_820BFFA0` | `Vehicle::Identity` | Overridden | No stores. |
| `sub_820FFC00` | `Stages::SetWord808` | Overridden | 1 stores. |
| `sub_82249148` | `RigidBody::Position` | Overridden | No stores. |
| `sub_8236FD40` | `RigidBody::Torque` | Overridden | No stores. |
| `sub_82432A68` | `DebugSwitchOn` | Overridden | No stores. |
| `sub_82433058` | `DebugSwitchOff` | Overridden | No stores. |
| `sub_82579560` | `List::Init` | Overridden | 2 stores. |
| `sub_820B64D0` | `RigidBody::IsFixed` | Overridden | The byte at +0. |
| `sub_820B7B58` | `CollisionObject::Word264` | Overridden | The word at +264. |
| `sub_820B8B98` | `Contact::Strength` | Overridden | The float (f1) at +64. |
| `sub_820B94F0` | `Object344::Float28` | Overridden | The float (f1) at +28. |
| `sub_820BFF78` | `Vehicle::Float2224` | Overridden | The float (f1) at +2224. |
| `sub_820BFF80` | `Vehicle::Speed` | Overridden | The float (f1) at +48. |
| `sub_820FFDB8` | `Stages::Word808` | Overridden | The word at +808. |
| `sub_825741C0` | `RigidBody::Mass` | Overridden | The float (f1) at +4. |
| `sub_820B7C08` | `CollisionObject::SetSize` | Overridden | A copy between its arguments, field by field. |
| `sub_820B7C10` | `CollisionRecord::Copy` | Overridden | A copy between its arguments, field by field. |
| `sub_820FFFE8` | `Pair::Copy` | Overridden | A copy between its arguments, field by field. |
| `sub_82291B20` | `Vec4::Copy` | Overridden | A copy between its arguments, field by field. |

### `native_xact`

The xact' set-ups, resets, small setters, copies and getters (`src/game/xact.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82587918` | `XACTWaveEntry::XACTWaveEntry` | Overridden | 1 stores. |
| `sub_825A74F0` | `XACTVariable::XACTVariable` | Overridden | 47 stores. |
| `sub_825A75F0` | `XACTGlobalSettings::XACTGlobalSettings` | Overridden | 16 stores. |
| `sub_825A83A8` | `XACTNodePool::XACTNodePool` | Overridden | 38 stores. |
| `sub_825A88C0` | `XACTGlobalSettingsBase::XACTGlobalSettingsBase` | Overridden | 11 stores. |
| `sub_825AA498` | `XACTCue::DetachSound` | Overridden | 2 stores. |
| `sub_825AB208` | `XACTCue::MakeInstanceVariable` | Overridden | No stores; calls `sub_825AB0F8`. |
| `sub_825AC9E8` | `XACTSoundBankBase::XACTSoundBankBase` | Overridden | 50 stores. |
| `sub_825ACBA0` | `XACTSoundBank::AddListener` | Overridden | No stores; calls `sub_825AEA28`. |
| `sub_825ACD00` | `XACTSoundBank::XACTSoundBank` | Overridden | 8 stores; calls `sub_825AC9E8`. |
| `sub_825AD9B8` | `XACTWaveBank::AddListener` | Overridden | No stores; calls `sub_825AEA28`. |
| `sub_825B16D8` | `XACTFader::XACTFader` | Overridden | 6 stores. |
| `sub_825B1708` | `XACTFader::~XACTFader` | Overridden | 1 stores. |
| `sub_825B2478` | `XACTWaveBankRoot::XACTWaveBankRoot` | Overridden | 51 stores. |
| `sub_825B4318` | `XACTWaveBankBase::XACTWaveBankBase` | Overridden | 3 stores; calls `sub_825B2478`. |
| `sub_825B5E98` | `XACTEvent::~XACTEvent` | Overridden | 1 stores. |
| `sub_825B8E70` | `XACTInMemoryWave::XACTInMemoryWave` | Overridden | 12 stores; calls `sub_825B7A80`. |
| `sub_825BAC80` | `XACTTrackInstance::XACTTrackInstance` | Overridden | 17 stores. |
| `sub_8209A2C8` | `XACTWaveEntry::GetPlayLength` | Overridden | The word at +24. |
| `sub_820A88D0` | `XACTWaveEntry::GetLoopStart` | Overridden | The word at +28. |
| `sub_82370758` | `XACTWaveEntry::GetLoopLength` | Overridden | The word at +32. |
| `sub_8242B820` | `MenuList::GetPreviousSelection` | Overridden | The word at +20. |
| `sub_825BD8F8` | `XACTPlayWaveEvent::GetLoopCount` | Overridden | The byte at +52. |

### `native_xaudio`

The xaudio' set-ups, resets, small setters, copies and getters (`src/game/xaudio.cpp`), generated from SBX's code and checked against the recompiled functions. The rest of the subsystem stays recompiled.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82447AF0` | `XAudioSourceVoice::DeletingDestructor` | Overridden | No stores; calls `sub_82447508`. |
| `sub_82447F90` | `XAudio::Alloc` | Overridden | No stores; calls `sub_82295118`. |
| `sub_82447FA0` | `XAudio::Free` | Overridden | No stores; calls `sub_822951B0`. |
| `sub_824483B0` | `XAudioSubmixVoice::DeletingDestructor` | Overridden | No stores; calls `sub_82448118`. |
| `sub_82448AB0` | `XAudioVoice::SetRootVtable` | Overridden | 1 stores. |
| `sub_82448B90` | `XAudio::MixerCallback` | Overridden | No stores; calls `sub_82444FB8`. |
| `sub_82449498` | `XAudioDefaultAllocator::~XAudioDefaultAllocator` | Overridden | 3 stores; calls `sub_824492B8`. |
| `sub_8244DCE0` | `XAudioMatrix::Destruct` | Overridden | 3 stores. |
| `sub_8244E760` | `XAudioCodec::Destruct` | Overridden | 2 stores; calls `sub_824529C0`. |
| `sub_8244EDC0` | `XAudioVoice::ConstructOutputs` | Overridden | 1 stores; calls `sub_8244FB88`. |
| `sub_82450598` | `ReverbCore::Defaults` | Overridden | 19 stores. |
| `sub_82450A58` | `ReverbCore::SetStereo` | Overridden | 1 stores. |
| `sub_82452980` | `CodecBase::CodecBase` | Overridden | 7 stores. |
| `sub_824529C0` | `CodecBase::~CodecBase` | Overridden | 1 stores; calls `sub_824565E8`. |
| `sub_824529D8` | `CodecBase::Init` | Overridden | 6 stores; calls `sub_82456620`, `sub_8258CB90`. |
| `sub_824554E8` | `PcmCodec::~PcmCodec` | Overridden | 3 stores; calls `sub_8244E760`. |
| `sub_824565E8` | `PacketLists::Clear` | Overridden | No stores; calls `sub_824448F0`. |
| `sub_824566E0` | `PacketLists::Init` | Overridden | 6 stores. |
| `sub_8244F7C8` | `XAudioVoice::SetFormatWords` | Overridden | A copy between its arguments, field by field. |

## Render commands

The game records its drawing into command lists, which the render thread
replays through the handler table at `0x8264EB40` (ids 1-35; 0, 36 and 37
do nothing). Every handler is wrapped (`REPLAY_COUNTED_HANDLER` in
`src/renderer/replay_hooks.cpp`) so the native renderer sees each command
before and after it runs. The names are from what each handler does to
the graphics library's device. Single commands come through a second
table at `0x8264EAA8` with the same ids.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82575740` | `Command::SetRenderState` | Wrapped | Command 1. [state, value]: the device's setter for that render state. |
| `sub_825757A0` | `Command::SetSamplerState` | Wrapped | Command 2. [sampler, state, value]. |
| `sub_82575800` | `Command::SetBlendState` | Wrapped | Command 3. [render target, blend control]: the RB_BLENDCONTROL register for that target. |
| `sub_82575850` | `Command::SetTexture` | Wrapped | Command 4. [sampler, texture]: the texture's fetch constant into the shadow. |
| `sub_82574608` | `Command::SetHelperVertices` | Wrapped | Command 5. Inline vertex data for the helper object (24 bytes a vertex). |
| `sub_82574630` | `Command::SetHelperIndices` | Wrapped | Command 6. Inline index data for the helper object (2 or 4 bytes an index). |
| `sub_82575A58` | `Command::SetVertexShader` | Wrapped | Command 8. [shader]. |
| `sub_82575AA8` | `Command::SetPixelShader` | Wrapped | Command 9. [shader]. |
| `sub_82575AF8` | `Command::SetVertexDeclaration` | Wrapped | Command 10. [declaration]. |
| `sub_82575B48` | `Command::SetStreamSource` | Wrapped | Command 11. [stream, buffer, offset, stride]: the buffer's fetch constant. |
| `sub_82575BA0` | `Command::SetIndices` | Wrapped | Command 12. [index buffer]; once per indexed draw (command 18). |
| `sub_82574668` | `Command::HelperSetUp` | Wrapped | Command 13. Ids 13 and 14 (one handler): stores three words for the helper object and calls its slot 2. |
| `sub_82575BF0` | `Command::HelperDraw` | Wrapped | Command 15. The helper object's draw (slot 3) from its inline data. The native renderer draws those vertices. |
| `sub_82575C98` | `Command::HelperDrawIndexed` | Wrapped | Command 16. The same, slot 4 (indexed). |
| `sub_82575D50` | `Command::DrawVertices` | Wrapped | Command 17. [type, start, count]: `DrawVertices` (`sub_825A3B00`). |
| `sub_82575DC8` | `Command::DrawIndexedVertices` | Wrapped | Command 18. [type, base and start, count]: `DrawIndexedVertices` (`sub_825A3ED0`). |
| `sub_82575E48` | `Command::DrawPrimitiveUP` | Wrapped | Command 19. [type, count, stride, data]: `BeginVertices` (`sub_825A31A0`), a copy, `End` (`sub_825A3AE0`). |
| `sub_82575EC0` |  | Wrapped | Command 20. [object], skipped if 0. Not identified. |
| `sub_825746E0` | `Command::Skip` | Wrapped | Command 21. Skips its payload (inline data or padding). |
| `sub_82577018` |  | Wrapped | Command 22. [object, flag]: asks the object (slot 2); not ready with the flag set, runs its slots 1 and 3. Not identified. |
| `sub_82575F10` | `Command::SetRenderTarget` | Wrapped | Command 23. [index, surface]. |
| `sub_82575F60` | `Command::SetDepthStencilSurface` | Wrapped | Command 24. [surface]. |
| `sub_82575FB0` | `Command::Resolve` | Wrapped | Command 25. EDRAM to a texture: flags, rects, destination, clear values. The native renderer handles it from its items. |
| `sub_82576108` | `Command::Clear` | Wrapped | Command 26. [count, rects, flags, color, Z, stencil]. |
| `sub_82576180` | `Command::DrawIndexedPrimitiveUP` | Wrapped | Command 27. Inline indices and vertices: `BeginIndexedVertices` (`sub_825A35F0`), copies, `End`. |
| `sub_825746F0` | `Command::Callback` | Wrapped | Command 28. [object]: its slot 0 with the device. Runs the boot movies' frame (`sub_820AE840`). |
| `sub_82576250` | `Command::RestoreTargets` | Wrapped | Command 29. Sets render targets 0-3 and depth from the saved set, by bit. |
| `sub_82576348` | `Command::SetViewport` | Wrapped | Command 30. [x, y, width, height, min Z, max Z]. |
| `sub_825763C8` | `Command::SetScissorRect` | Wrapped | Command 31. [left, top, right, bottom]. |
| `sub_82576450` | `Command::BeginTiling` | Wrapped | Command 32. Predicated tiling: [flags, tile count, tile rects]. |
| `sub_825764D0` | `Command::EndTiling` | Wrapped | Command 33. [flags, destination]: resolves each tile (`sub_822AC858`, passed on to the native renderer). |
| `sub_82576550` | `Command::SaveTargets` | Wrapped | Command 34. Saves the viewport, render targets 0-3 and depth. |
| `sub_825765B8` | `Command::RestoreSaved` | Wrapped | Command 35. Restores what 34 saved. |
| `sub_82575148` |  | Wrapped | Single command 15 (the helper object's draw) from the second table: marked as command 15 so the native renderer draws its inline vertices. In a race only 5, 7, 13 and 15 come this way (the minimap's player triangle). |
| `sub_825A31A0` | `Device::BeginVertices` | Wrapped | r3 the device, r4 the primitive type, r5 the vertex count, r6 the stride: where in the command buffer the caller copies the vertices. Noted for `Device::EndVertices`. |
| `sub_825A3AE0` | `Device::EndVertices` | Wrapped | After the copy: the native renderer draws the inline vertices (commands 15 and 19; others are logged once). |
| `sub_825A3B00` | `Device::DrawVertices` | Wrapped | Logged once per caller when called outside command 17. |
| `sub_825A35F0` | `Device::BeginIndexedVertices` | Wrapped | Logged once per caller when called outside command 27. |
| `sub_825A3ED0` | `Device::DrawIndexedVertices` | Overridden | r3 the device, r4 the primitive type, r5 the base vertex, r6 the start index, r7 the index count: writes the dirty state's packets and DRAW_INDX (`native::DrawPackets_IndexedDraw`, which runs the original to check itself with `--native_renderer_check`). |
| `sub_822AB028` | `Device::PatchVertexShader` | Wrapped | r3 the shader, r4 the microcode, r5 the declaration: patches the shader for the declaration. Counted; when the GPU may still be using the shader's microcode the library patches a copy in the command buffer, which is kept for the native renderer. |
| `sub_822AC858` | `Device::Resolve` | Wrapped | r3 the device, r4 flags, r5 the source rect, r6 the destination texture, r7 the destination point, r10 the clear color, f1 the clear Z. In command 33 one per tile (the tile count at device +12772), passed to the native renderer; logged outside 25 and 33. |
| `sub_82573538` | `VideoOutput::ResolveFrontBuffer` | Wrapped | r3 the video output: resolves render target 0 into one of its two front buffer textures (+4 x (68 + [+268])) outside any command; the native renderer shows that target. |
| `sub_8229C8D8` |  | Wrapped | Ends the command segment when the buffer is full (and kicks it); counted, so the draw check can leave those calls out. |
| `sub_822AADE8` |  | Wrapped | The draw's float constant packet writer (r3 the device). Checked against the native renderer with `--native_renderer_check`. |
| `sub_822AABF0` |  | Wrapped | The draw's register block writer: r4 the mask, r5 the first register; the blocks are marked dirty for the native renderer, then written. |
| `sub_822AACB0` |  | Wrapped | The draw's fetch constant writer: r4 the mask; marked dirty for the native renderer, then written. |
| `sub_8229BDA8` | `GpuLink::Kick` | Overridden | r3 the device, r4 pairs of (size in words, address), r5 the pair count. No GPU: the buffers' packets are walked and only their effects outside the GPU are made (memory writes, interrupts, fences into the progress block), so the library sees each kick done at once. Logged with `--native_renderer_log_gpu`. |
| `sub_8229C1F8` | `GpuLink::DeferredKick` | Overridden | r3 the device, r5 the address, r6 the size, r7 the interrupts the buffer raises. The library queues a buffer while interrupts are outstanding; with every kick done at once nothing is deferred, so it kicks at once and leaves the count at 0. |
| `sub_822B1888` |  | Wrapped | EndTiling's (`sub_825A47F0`) insert of the worker callback into the command stream. Logged with `--native_renderer_log_gpu`. |
| `sub_822B1748` |  | Wrapped | The callback that wakes CPU 2's library worker (`sub_822B1678`), which submits deferred kicks. Logged. |
| `sub_822B1450` |  | Wrapped | The worker's job. Logged. |
| `sub_8258CB90` | `memcpy` | Wrapped | The library's memcpy (r3 the destination, r4 the source, r5 the size): copies into the physical views are recorded for the native renderer's buffers once done; with `--native_renderer_log_buffers`, copies into a known buffer header are logged. |
| `sub_822B3678` | `D3DXCompileShader` | Overridden | r3 the source, r4 its length, r8 the profile, r10 where the shader goes: a shader compiled ahead of time (`src/graphics/compiled_shaders.h`) when there's one for the source; otherwise the original compiles it (and it's logged). |

## Audio library

The audio library's XMA layer, over our own decoding (`src/hooks/xma.cpp`,
`src/audio/xma_stream.h`): there are no XMA contexts, registers or
hardware. A source voice's XMA object (voice +180) is a handle for an
object of ours; each stream decodes its sound a little ahead of the
reading and answers how many samples are ready, where and how many were
taken. Custom music reads its track there instead (`docs/custom_music.md`).

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_82445678` |  | Overridden | Reads the XMA registers' context array address at boot: nothing to read. |
| `sub_82445690` | `XmaObject::Create` | Overridden | r3 the stream count, r4 their descriptions (12 bytes: sample rate, output ring samples, channels), r5 flags (bit 0: contexts made later), r6 out: the object's handle. |
| `sub_824458C0` | `XmaObject::Destroy` | Overridden | r3 the object. |
| `sub_82445910` | `XmaObject::ReleaseContexts` | Overridden | Only the contexts' made state is kept. |
| `sub_824460E0` | `XmaObject::MakeContexts` | Overridden | The same. |
| `sub_82445988` | `XmaObject::ContextsMade` | Overridden | 1 when made. |
| `sub_82445998` | `XmaObject::SetInputBuffer` | Overridden | (object, stream, data, size in bytes): starts the sound, or queues it as the second input. |
| `sub_82445A70` | `XmaObject::IsPlaying` | Overridden | (object, stream, address): 1 if the sound at that address is still playing or queued. |
| `sub_82445AD0` | `XmaObject::HasFreeInput` | Overridden | (object, stream): 1 unless both input slots are taken. |
| `sub_82445B28` | `XmaObject::IsIdle` | Overridden | (object, stream): 1 if no sound is playing. |
| `sub_82445B90` | `XmaObject::ReadyCount` | Overridden | (object, stream): samples ready, at most the output ring the 360 would have had. |
| `sub_82445DC0` | `XmaObject::ReadPosition` | Overridden | (object, stream, out): where to read (a staging buffer in guest memory) and how many samples there are readable in one go. |
| `sub_82445C08` | `XmaObject::Take` | Overridden | (object, stream, samples, out): takes up to that many; where they were read from; returns how many. |
| `sub_82445FF8` | `XmaObject::TakenCount` | Overridden | (object, stream): samples taken so far. |
| `sub_82445E68` | `XmaObject::ResetStream` | Overridden | (object, stream): the sound stopped. |
| `sub_82445F30` | `XmaObject::ErrorStatus` | Overridden | (object, stream): none. |
| `sub_82445F80` | `XmaObject::SetLoopData` | Overridden | (object, stream, the loop data: start and end bits, subframes, count). A custom track takes only the loop count. |
| `sub_82446088` | `XmaObject::SetStart` | Overridden | (object, stream, the start's bit offset, subframes to skip). Not for a custom track. |
| `sub_82446018` | `XmaObject::InputPosition` | Overridden | (object, stream, out): the input buffer, the read offset and a word; zeros (logged once). Called by the voice's flush (`sub_824539F0`). |
| `sub_824461B0` | `XmaObject::Lock` | Overridden | Nothing to wait for: 0. |
| `sub_82446310` | `XmaObject::IsLocked` | Overridden | 1. The hardware wait polls it. |
| `sub_82446248` | `XmaObject::WaitForHardware` | Overridden | r3 the object: nothing to wait for, 0. |
| `sub_82446490` | `XmaObject::Unlock` | Overridden | Decodes ahead and moves finished streams on. |
| `sub_825613F8` | `Voice::Worker` | Overridden | The voice chat library's worker (r3 the engine): signals its started event (engine +1304) and returns without running; in play it found no talkers and fed nothing. The engine itself is still made by `Voice::Init`. |

## Movies

The game's movie class on our own FFmpeg player (`src/hooks/movies.cpp`,
`src/movies/movie.h`), behind `--play_movies_with_ffmpeg` (on); with it
off, the recompiled functions run. `Movie::RenderFrame` (`sub_820AE840`)
is in the first table.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820AEAF8` | `Movie::Play` | Overridden | r3 the movie, r4 the file. |
| `sub_820AE3C8` | `Movie::Start` | Overridden | r3 the movie. |
| `sub_820AE710` | `Movie::Stop` | Overridden | r3 the movie. |
| `sub_820AE780` | `Movie::Pause` | Overridden | r3 the movie. |
| `sub_820AE7E0` | `Movie::Resume` | Overridden | r3 the movie. |
| `sub_820AE968` | `Movie::GetState` | Overridden | r3 the movie: its state (the only one whose r3 the callers read). |
| `sub_820AE410` | `Movie::Release` | Overridden | r3 the movie. |

## Debugging: per-frame state

Parts of the draw step wrapped for `--find_per_frame_state`
(`src/debug/frame_state_diff.cpp`), which finds game state that advances on
subtick's render-only passes: each is diffed before and after it runs. Off,
they only call the original. `sub_820F5F48` and `TickHandler::Invoke`
(`sub_8209A1C0`) are in the first table.

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_820A0B40` |  | Wrapped | Per camera pass, in a loop over the passes. |
| `sub_820AA160` |  | Wrapped | A part of the draw step. |
| `sub_8209F998` |  | Wrapped | A part of the draw step. |
| `sub_8209F9F8` |  | Wrapped | A part of the draw step. |
| `sub_820C93A8` |  | Wrapped | A part of the draw step. |
| `sub_820C5BA0` |  | Wrapped | Draws the cars. |
| `sub_82132438` |  | Wrapped | A part of the draw step. |
| `sub_821325C0` |  | Wrapped | A part of the draw step. |
| `sub_8212F5D8` |  | Wrapped | A part of the draw step. |

## Known data

| Address | What | Notes |
|---|---|---|
| vehicle+0x30 | Speed (float, m/s) | Read by `Vehicle::GetSpeedKmh`. |
| vehicle+0xD0 | Vehicle world matrix | 4x4 row-major: rotation rows, then position row (w = 1). The car's forward axis is -row 0. Not yet checked in SBX. |
| vehicle+0xCF0 | Copy of the world matrix | Not yet checked in SBX. |
| `0x826AA8F0` | Camera view matrix | Pass 0's view (+0x10). The camera position is -t·Rᵀ. Draw rebuilds it from the eye and target. |
| `0x826AA8E0` | Camera pass array | 21 objects of 592 bytes: view +0x10, projection +0x50, view-projection +0x90, eye +0x190, look-at target +0x1A0, up +0x1B0. Pass settings at +564 to +588, active flag at +588. Pass 0 is the main 1280x720 view; pass 10 a 1024x1024 reflection and 11 a 512x512 shadow map. |
| `[0x8280DE44]` | Title screen object | +4 state (5 = start the attract race), +36 idle ticks. |
| `0x826C7EC4` | Post-race check counter | Counted up by `sub_8218C240` (`PostRace::Check`). |
| `0x826B5784`, `0x826B5788` | Minimap blink clock | Stepped by `Minimap::Draw`. |
| `0x826B5870` | Play time frame counter | Counted by `sub_82107A38`; `sub_82110928` turns it into hours, minutes and seconds. |
| `0x826B588C` | Boot movie id table | Index at `0x826B58A8`; skippable flag at `0x826B58A4`. |
| `0x826B51FC`, `0x826B51F4` | The course scenery manager (248 bytes) | Both point to it: set by `Scenery::Create` and `Scenery::Init`. +8 the configuration (+8 the mode, +32 the header, +36 the course record), +140 and +168 the key tables, +188 the area models' buffer, +216 to +224 the visible areas (`Lights::FindNearest`'s list). `docs/game_data.md`, "Courses". |
| `0x825F2E40` | The course table | 24 entries of 8 bytes by course index: the header and the course record (`docs/game_data.md`, "Courses"). |
| `0x82626478` | The front end's scenes | 16 bytes an entry: three `BUILD` ids, then the scene number (0-5). |
| `0x826B57FC`, `0x826B57F0` | The course's collision mesh and route | Loaded by `TrackCollision::Load`. |
| `[0x826B580C]` | The race data | +4 the race mode (8-10 use course place 1), +24 the route bits, +32 the course variant. |
| `0x820029C4`, `0x820029C0` | 1/30 and 1/60 | Float step constants (see "Fixed time step"). |
| `0x825F17E8` | The cars' step length | Float, 1/60 (`.data`). Passed to `Vehicle::Step` twice a tick by `Vehicle::Tick`, and read by `Vehicle::ApplyControls` and a leaf at `0x820BFF54`. Not changed by the subtick code. |
| `0x826B5810` | Control record table | One pointer per car index to its control record (5 words; see `Vehicle::ApplyControls`). |
| `0x826B5838` | Input tables | `[+0]` + pad x 120 + 0x1F7C0: the abstract input ids `PlayerControls::Build` queries. |
| `0x820029B8` | Float | `Vehicle::Tick` stores speed times it at car+6160. |
| car+0x930, +0x934 | Step length and its inverse | Written by `Vehicle::SetStepLength` each step. |
| car+0x940 | `Vehicle::Step` inner iterations | 10, set in `sub_820B0DD0`. |
| car+0x958 | `sub_820B1678`'s ramp counter | 0 to 30. |
| `0x82000A20` | 1.0f | w of the light positions `Lights::FindNearest` writes. |
| `0x825F0A84`, `0x825F0A88` | `BUILD.DAT`, `BUILD_CRS.DAT` file handles | Opened by `Archive::Init`. |
| `0x825F0A90`, `0x825F0AA0` | Built-in copies of the `BUILD.TOC` and `BUILD_CRS.TOC` headers | Count (23009, 1359), two unknown words, `.DAT` size. Only the counts are read; adding archive entries needs these patched (`docs/game_data.md`). |
| `0x825F0AB0` | Loose file table | Pairs of file id and path, ended by a null path. |
| `0x82694E00`, `0x82694E08` | `BUILD` and `BUILD_CRS` TOC buffers | Sizes at `0x82694E04`, `0x82694E0C`. |
| `0x82694E14`, `0x82694E10` | The shared load buffer and its size | 0xC80000 bytes, allocated by `Archive::Init` (`docs/game_data.md`). |
| `[0x826B2B24]+5008`, `+5012` | The shared load buffer, as the car code reads it | Set by `sub_820C8B88`. |
| `0x825F2520` | Vinyl sheet size | 1024, 1024, then 16, 16 (the cleared corner). |
| `0x825F2A00` | Vinyl sheet regions | 10 rectangles (x0, y0, x1, y1 as u32), indexed by `Vinyl::DrawDecal`'s r3. |
| `0x82069A94` | `XPRList` vtable | |
| car model+32 | The car's block index | Times the set's stride gives the car's ids. |
| car model+208, +592 | Vinyl layer and sticker records | 64 bytes each: 6 layers; 8 groups of 6 stickers, 384 bytes apart. |
| car model+4092, +4380 | Vinyl and sticker load slots | 48 bytes apart. |
| car model+6684, +6688 | Vinyl sheet texture and pixels | The A8R8G8B8 canvas, then the DXT5 texture. |
| model+48, +80 | The model's XPR list | +80 the `XPRList`, +48 a pointer to it; index 0 is the model's own XPR2. |
| model+444, +448 | Vinyl sheet textures | Set by `Model::SetVinylTextures`. |
| car+16 | Car's world matrix, for drawing | Copied by `Car::TickUpdate` each tick. |
| car+696, +700, +704 | Speed (km/h), revs, boost | Set each tick by `Car::TickUpdate`; read by the dash dials. `Car::DrawUpdate` takes 1.0 off +704 every pass. |
| view+8080, view+8144 | This tick's and the previous tick's view-projection | Copied into vertex c100 by `View::UploadPreviousViewProjection` (the motion blur). |
| `0x820069B0` | Music bank names | 18 strings, 12 bytes apart (`BGM_SYSTEM`, `bgm_tmp_17` down to 01 without 10, then 10), read by `Bgm::Load`. |
| `[0x826B5960]` | The sound object | `Bgm::Load` calls its vtable slot 4 to load a bank pair. |
| `0x82609334` | Music table | 13 name pointers: `bgm_tmp_06`, 07, 01, 15, 05, 10, 03, 09, 17, 11, 16, 14, 08. Read by `Music::PlayRoaming`, `Music::PlayRequested`, `sub_82174AD0`. |
| scene+9644, +9652 | The race sounds' state bits, play mode | The scene the race sounds' handlers run on (`sub_82175238`): +9644 the bits `Music::RequestBattle` sets and `Music::PlayRequested` plays, and the music change and car bits (`docs/audio.md`); +9652 the race data's play mode, which `Music::PlayRoaming` reads. |
| `[0x826B2B28]` | Parameter tables | +8 BUILD 73 (wheels), +12 BUILD 72 (car specs), +16 to +48 BUILD 74-82 (aero parts), +56 the car count. Built by `sub_820C9700`, loaded by `sub_820CC048`. |
| car+1644 | A car object's tuning | +8 engine stage, +16 to +60 the power and drive-train part levels, +64 to +92 tires and wheels, +96 to +136 the aero part levels (`docs/game_data.md`, "How the game uses the car specs"). |

## Known issues

- None recorded yet.


### `native_options_load_save`

Our own C++ behind `--native_options_load_save` (`docs/native-code.md`).

| Address | Name | Status | Notes |
|---|---|---|---|
| `sub_821B8458` | `LoadPage::Close` | Overridden | Sub-page 4: the page left by step 41; +760 = 1. |
| `sub_821B8998` | `SavePage::Close` | Overridden | As the load's, step 42. |
| `sub_821B89E0` | `OptionsSavePages::Cancel` | Overridden | Sub-page 5 of both pages: +760 = 1. |
| `sub_821B9C38` | `SavePage::SetDialog` | Overridden | As the load's for page 10 (steps 47, 46; +692, +696). |
| `sub_821B9D88` | `LoadPage::SetDialog` | Overridden | r3 the kind, r4 the dialog (0 question, 1 note, 2 wait): with a dialog, the page shown again (steps 44, 43) on its sub-page; the kind kept (+676). |
| `sub_821BA160` | `LoadPage::CheckDevice` | Overridden | Sub-page 0: unless the selector is up, the wait with a device, else question 7. |
| `sub_821BA1B8` | `LoadPage::DeviceAnswer` | Overridden | Sub-page 6: by GameData::Find, note 10 (the failure acknowledged) or 11. |
| `sub_821BA210` | `SavePage::CheckDevice` | Overridden | As the load's, question 11 with no device. |
| `sub_821BA268` | `SavePage::Wait` | Overridden | Sub-page 3: after 90 ticks, the check's answer (GameData::Check) or the save's end (a failure's note, acknowledged, or note 9). |
| `sub_821BACF8` | `LoadPage::Answer` | Overridden | Sub-page 1: the question's answer by its kind (load the game data, ask for a device, close). |
| `sub_821BAE88` | `LoadPage::Note` | Overridden | Sub-page 2: a note; kind 3 asks for a device by itself after 45 ticks. |
| `sub_821BAFB8` | `SavePage::Answer` | Overridden | Sub-page 1: as the load's (save the game data, ask for a device, close). |
| `sub_821BB168` | `SavePage::Note` | Overridden | Sub-page 2: as the load's. |
| `sub_821C4A10` | `LoadPage::Leave` | Overridden | Step 41: the options movie's leave (labels 841 and 843 + the cursor, animation 10), then back to page 0 and the save buffers freed. |
| `sub_821C4AF8` | `SavePage::Leave` | Overridden | Step 42: as the load's (labels 857 and 859 +, animation 11). |