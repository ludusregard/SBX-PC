// The garage stand: the stage the front end's screens show a car on (its
// scene model and the shown car's sizes), the holder they reach it
// through, and the screens' shared state.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/scene_models.h"

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// The garage stage: its scene model (pool 7 of the car scene), the model
// block to load next, and the shown car's sizes, which the stand's camera,
// floor and wheels use.
struct GarageStage {
  uint8_t unknown_0[4];
  i32_be next_model_block;  // The model block to load (CarModels::BlockOf), or -1.
  u32_be model_index;       // The scene model's index in pool 7.
  uint8_t unknown_12[4];
  u32_be has_model;  // Set once the model was made; released before the next.
  uint8_t unknown_20[6036];
  u32_be draw_word;  // GarageStage::MakeModel's flag (0 or 1), also the model's +4296.
  uint8_t unknown_6060[36];
  // From the car data: its +1736 and +1756, then each axle record's +52,
  // the differences, and each axle record's +208 and +212 (front, rear).
  f32_be heights[2];
  f32_be axle_heights[2];
  f32_be axle_angles[2];
  f32_be axle_values_212[2];
  f32_be angle_offsets[2];       // sin(axle angle in degrees) x axle height.
  f32_be height_differences[2];  // axle_heights - heights.
  uint8_t unknown_6144[8];
  f32_be dimensions[3];  // The car data's +4, +8, +12 (probably the wheelbase and tracks).
  uint8_t unknown_6164[12];
  // Each axle record's first three floats, w 1.0 (front, rear; refitted to
  // a car by GarageStage::FitAxles and FitCar).
  Vector axle_positions[2];
};
GUEST_OFFSET(GarageStage, next_model_block, 4);
GUEST_OFFSET(GarageStage, model_index, 8);
GUEST_OFFSET(GarageStage, has_model, 16);
GUEST_OFFSET(GarageStage, draw_word, 6056);
GUEST_OFFSET(GarageStage, heights, 6096);
GUEST_OFFSET(GarageStage, axle_angles, 6112);
GUEST_OFFSET(GarageStage, angle_offsets, 6128);
GUEST_OFFSET(GarageStage, height_differences, 6136);
GUEST_OFFSET(GarageStage, dimensions, 6152);
GUEST_OFFSET(GarageStage, axle_positions, 6176);

// What the screens call the stage's functions on (the r3 of every
// GarageStage function).
struct GarageStageHolder {
  ptr<GarageStage> stage;
};

// The stand's screens' shared state at [0x826C7E88]: their holder.
struct StandScreens {
  ptr<GarageStageHolder> holder;
  uint8_t unknown_4[16];
  u32_be unknown_20;  // Set to 1 when the stage's model is made.
};
GUEST_OFFSET(StandScreens, unknown_20, 20);

constexpr uint32_t kStandScreens = 0x826C7E88;  // Holds the StandScreens' address.

}  // namespace game
