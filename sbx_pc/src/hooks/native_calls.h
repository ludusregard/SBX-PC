// Native game code called from other overrides (docs/native-code.md): where
// an address already has SBX's own override (a wrapper that watches or
// reorders the call), the wrapper stays as it is and calls one of these in
// place of the recompiled function. Each runs our own C++ with its
// subsystem's flag on, else the recompiled function, with the original's
// registers in and out.

#pragma once

#include "guest.h"

#include <cstdint>

namespace hooks {

// StateMachine::Handle (sub_82106410; --native_game_modes), from
// title_screen.cpp's override.
void NativeGameModes_Handle(PPCContext& ctx, uint8_t* base);

// RivalLogo::Draw (sub_8220F7B0; --native_race_hud), from
// subtick/per_frame_logic.cpp's override.
void NativeRaceHud_RivalLogoDraw(PPCContext& ctx, uint8_t* base);

// TextureAnimations::Advance (sub_820F1038; --native_course_scenery), from
// subtick/per_frame_logic.cpp's override.
void NativeCourseScenery_AdvanceTextureAnimations(PPCContext& ctx, uint8_t* base);

// SaveData::NewGame (sub_82113FB0; --native_save_data), from
// unlock_cut_cars.cpp's override.
void NativeSaveData_NewGame(PPCContext& ctx, uint8_t* base);

// SaveGame::Write (sub_8228CEE8; --native_save_interface), from
// autosave.cpp's override.
void NativeSaveInterface_Write(PPCContext& ctx, uint8_t* base);

// UiMovie::Update (sub_8211D4B8) and UiMovie::ApplyTrack (sub_82585C40;
// --native_ui_movies), from subtick/per_frame_logic.cpp's overrides.
void NativeUiMovies_Update(PPCContext& ctx, uint8_t* base);
void NativeUiMovies_ApplyTrack(PPCContext& ctx, uint8_t* base);

// RaceCamera::SetMode (sub_82157188) and RaceCamera::Update (sub_821573E0;
// --native_race_camera), from subtick/camera_cuts.cpp's and
// subtick/per_frame_logic.cpp's overrides.
void NativeRaceCamera_SetMode(PPCContext& ctx, uint8_t* base);
void NativeRaceCamera_Update(PPCContext& ctx, uint8_t* base);

// Scenery::Draw (src/game/course_scenery.cpp): our C++ with
// --native_course_scenery, else the recompiled function. Called from the
// frame state diff's wrapper (src/debug/frame_state_diff.cpp).
void NativeCourseScenery_Draw(PPCContext& ctx, uint8_t* base);

// Recorder::RecordObject (src/game/scenery_lists.cpp): our C++ with
// --native_scenery_lists, else the recompiled function. Called from
// subtick's wrapper (src/subtick/car_transforms.cpp).
void NativeSceneryLists_RecordObject(PPCContext& ctx, uint8_t* base);

// Model::Animate (src/game/model_animation.cpp): our C++ with
// --native_model_animation, else the recompiled function.
void NativeModelAnimation_Animate(PPCContext& ctx, uint8_t* base);

// Archive::Init (src/game/archive.cpp): our C++ with --native_archive,
// else the recompiled function. Called from the patch loader's wrapper.
void NativeArchive_Init(PPCContext& ctx, uint8_t* base);

}  // namespace hooks
