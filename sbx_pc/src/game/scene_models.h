// A scene model: a car (or traffic) model in the car scene (pool 7 of its
// second set), its record, its parts' objects with the flags that say
// they're done, and its deferred build.

#pragma once

#include "game/guest_types.h"
#include "game/load_poll.h"
#include "game/math.h"
#include "game/scene_loads.h"

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// The scene model (pool 7 of the second set; 5,120 bytes).
struct SceneModel {
  static constexpr uint32_t kTraffic = 3;  // The record's kind for a traffic car.

  uint8_t unknown_0[16];
  u32_be state;  // 3 while in use (ScenePoolObjectHeader).
  u32_be slot;   // Its index in pool 7; also its sound slot.
  uint8_t unknown_24[8];
  SceneModelRecord record;
  uint8_t unknown_4276[20];
  // GarageStage::MakeModel's flag (0 or 1; also kept by the stage). In the
  // front end it stands for the interior being ready (SceneModel::Update).
  u32_be unknown_4296;
  uint8_t unknown_4300[4];
  Matrix transform;  // The accessory's anchor is multiplied by it.
  uint8_t unknown_4368[548];
  f32_be unknown_4916;  // Set by SceneModel::SetUp (the scene's kind picks it).
  uint8_t unknown_4920[56];
  u32_be lights_on;    // GarageStage::SetLightsOn.
  f32_be light_value;  // 0.0 with the lights on, else the parts' light value.
  uint8_t unknown_4984[32];
  u32_be unknown_5016;
  uint8_t unknown_5020[4];
  u32_be unknown_5024;  // Set by SceneModel::SetUp.
  u32_be release_pending;  // Probably.
  u32_be second_wheels_added;
  u32_be ready;  // Everything finished and applied (SceneModel::PollLoad); cleared to load again.
  u32_be build_pending;
  i32_be build_countdown;
  ptr<SceneCarModel> car_model;
  u32_be car_model_ready;
  u32_be decals_ready;
  ptr<PolledParts> parts;
  u32_be parts_ready;
  ptr<PolledInterior> interior;
  u32_be interior_ready;
  ptr<PolledCockpit> cockpit;  // Or 0.
  u32_be cockpit_ready;        // Set at once when there's no cockpit.
  ptr<PolledLights> lights;
  u32_be lights_ready;
  ptr<PolledTraffic> traffic;  // A traffic model's body.
  u32_be traffic_ready;
  ptr<PolledWheels> wheels;
  u32_be wheels_ready;
  ptr<PolledWheels> second_wheels;
  u32_be unknown_5112;
  ptr<CarSceneState> scene;
};
GUEST_OFFSET(SceneModel, slot, 20);
GUEST_OFFSET(SceneModel, record, 32);
GUEST_OFFSET(SceneModel, unknown_4296, 4296);
GUEST_OFFSET(SceneModel, transform, 4304);
GUEST_OFFSET(SceneModel, unknown_4916, 4916);
GUEST_OFFSET(SceneModel, lights_on, 4976);
GUEST_OFFSET(SceneModel, unknown_5016, 5016);
GUEST_OFFSET(SceneModel, unknown_5024, 5024);
GUEST_OFFSET(SceneModel, release_pending, 5028);
GUEST_OFFSET(SceneModel, second_wheels_added, 5032);
GUEST_OFFSET(SceneModel, ready, 5036);
GUEST_OFFSET(SceneModel, build_countdown, 5044);
GUEST_OFFSET(SceneModel, car_model, 5048);
GUEST_OFFSET(SceneModel, decals_ready, 5056);
GUEST_OFFSET(SceneModel, parts, 5060);
GUEST_OFFSET(SceneModel, interior, 5068);
GUEST_OFFSET(SceneModel, cockpit, 5076);
GUEST_OFFSET(SceneModel, lights, 5084);
GUEST_OFFSET(SceneModel, traffic, 5092);
GUEST_OFFSET(SceneModel, wheels, 5100);
GUEST_OFFSET(SceneModel, second_wheels, 5108);
GUEST_OFFSET(SceneModel, scene, 5116);
GUEST_SIZE(SceneModel, 5120);

// The car sounds' table (writable data): two arrays of asset ids for each
// car block (traffic is block 47).
struct CarSoundIds {
  uint8_t unknown_0[8];
  u32_be second;  // The second load's ids, by variant.
  u32_be first;   // The first load's ids, by variant.
};
GUEST_SIZE(CarSoundIds, 16);

// --- The scene models' functions (scene_models.cpp) ---

// The records' defaults (each record's Reset) and SceneModelRecord's
// constructor.
void Record12Reset(Record12& record);
void CarRecordReset(SceneCarRecord& record);
void CarRecordSetW(SceneCarRecord& record);  // CarRecord::CarRecord.
void PartsRecordReset(PartsRecord& record);
void InteriorRecordReset(InteriorRecord& record);
void CockpitRecordReset(CockpitRecord& record);
void WheelsRecordReset(WheelsRecord& record);
void Pool4RecordReset(Pool4Record& record);
void SceneModelRecordReset(SceneModelRecord& record);
void SceneModelRecordInit(SceneModelRecord& record);

uint32_t AccessoriesGet();
uint32_t CarModelAllocPages(Guest& g, u32_be* out, uint32_t bytes);
uint32_t ScenePoolsSetUpB(Guest& g, CarSceneState* scene, uint32_t pool, uint32_t index,
                          uint32_t record);
uint32_t SceneModelApplyRecord(Guest& g, SceneModel* model);
void SceneModelReloadDecals(Guest& g, SceneModel* model);
uint32_t SceneModelAddSecondWheels(Guest& g, SceneModel* model);

}  // namespace game
