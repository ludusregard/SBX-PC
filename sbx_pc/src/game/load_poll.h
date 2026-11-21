// The car scene ([0x826B2B24]) and its second pool set's objects (the car
// model, parts, interior, cockpit, lights, traffic body and wheels) as the
// scene models' set-ups, load poll and re-applies read them.
//
// The second-set objects are the same objects as scene_points.h's
// SceneParts, SceneInterior, SceneCockpit, SceneLights, SceneTraffic and
// SceneWheels (whose names the game's functions have here), laid out with
// the fields this group's functions read; the car scene the same as
// scene.h's CarScene.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/model.h"
#include "game/scene.h"
#include "game/scene_loads.h"
#include "game/scene_points.h"

namespace game {

using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// The car scene at [0x826B2B24]: its state, the set-up record's words and
// each second-set pool's count (CarScene::SetUp), its assets, the second
// set's pools' lists, the first pool set and the load buffer the car parts
// are loaded into one at a time.
struct CarSceneState {
  u32_be state;      // 3 once set up.
  u32_be model_set;  // 0 the front end's (set a), 1 the race's (set b).
  // The pools' object counts: car models, parts, interiors, cockpits,
  // lights, wheels, scene models, traffic bodies.
  i32_be car_models;
  i32_be parts;
  i32_be interiors;
  i32_be cockpits;
  i32_be lights;
  i32_be wheels;
  i32_be scene_models;
  i32_be traffic;
  u32_be own_buffer;  // The record's load buffer (0: the archive's).
  u32_be own_buffer_size;
  uint8_t unknown_48[16];
  SceneAssets assets;
  PoolNodeList pools[8];  // The second set's pools, by pool.
  ptr<ScenePoolSet> first_set;
  u32_be unknown_4996;  // CarScene::SetUp's r5.
  u32_be unknown_5000;  // Set: SceneModel::Update leaves the accessories alone.
  uint8_t unknown_5004[4];
  u32_be load_buffer;
  u32_be load_buffer_size;
};
GUEST_OFFSET(CarSceneState, own_buffer, 40);
GUEST_OFFSET(CarSceneState, assets, 64);
GUEST_OFFSET(CarSceneState, pools, 4896);
GUEST_OFFSET(CarSceneState, first_set, 4992);
GUEST_OFFSET(CarSceneState, unknown_5000, 5000);
GUEST_OFFSET(CarSceneState, load_buffer, 5008);

struct SceneCarModel;

// What each second-set object starts with: its pool header and the car
// model (pool 0's object) it belongs to (SetCarModel, the parts' and
// interior's Link).
struct CarSceneObjectHead {
  ScenePoolObjectHeader header;
  ptr<SceneCarModel> car_model;
};
GUEST_SIZE(CarSceneObjectHead, 28);

// The car model (pool 0 of the second set; 3,744 bytes): its record and the
// first set's car model made from it, which the other second-set objects
// reach it through.
struct SceneCarModel {
  ScenePoolObjectHeader header;
  ptr<CarModel> model;  // The first set's.
  uint8_t unknown_28[4];
  SceneCarRecord record;
  ptr<CarSceneState> scene;
  uint8_t unknown_3732[12];
};
GUEST_OFFSET(SceneCarModel, model, 24);
GUEST_OFFSET(SceneCarModel, record, 32);
GUEST_OFFSET(SceneCarModel, scene, 3728);
GUEST_SIZE(SceneCarModel, 3744);

// The car parts (pool 1 of the second set; 37,296 bytes).
struct PolledParts {
  static constexpr int kParts = 17;

  ScenePoolObjectHeader header;
  ptr<SceneCarModel> car_model;  // SceneParts::Link's,
  u32_be skeleton;               // and its model's skeleton.
  u32_be part_models[kParts];  // PartModels of the first set's parts, by kind; 0 for none.
  u32_be loaded;
  ptr<CarParts> parts;  // The first set's.
  PartsRecord record;   // The part picked for each kind.
  uint8_t unknown_168[8];
  PartsPoints points;
  uint8_t unknown_37232[16];
  u32_be model_record;  // The scene model's record (SceneParts::SetRecord).
  // What the points are found with (set by SceneParts::Build).
  u32_be car_record;
  u32_be record_address;
  u32_be points_car_model;
  uint8_t unknown_37264[4];
  u32_be points_skeleton;
  uint8_t unknown_37272[12];
  ptr<CarSceneState> scene;  // Its model set 0 or 1 builds the parts.
  uint8_t unknown_37288[8];
};
GUEST_OFFSET(PolledParts, skeleton, 28);
GUEST_OFFSET(PolledParts, part_models, 32);
GUEST_OFFSET(PolledParts, loaded, 100);
GUEST_OFFSET(PolledParts, record, 108);
GUEST_OFFSET(PolledParts, points, 176);
GUEST_OFFSET(PolledParts, model_record, 37248);
GUEST_OFFSET(PolledParts, car_record, 37252);
GUEST_OFFSET(PolledParts, points_skeleton, 37268);
GUEST_OFFSET(PolledParts, scene, 37284);
GUEST_SIZE(PolledParts, 37296);

// The interior's dials' points (scene_points.h's InteriorPoints from +240):
// the car's block and its record and lights.
struct InteriorDials {
  u32_be unknown_0;
  i32_be block;
  u32_be lights;  // One of the car scene's texture providers, by the block's set.
  uint8_t unknown_12[4];
  PointRecord records[12];
  u32_be block_record;  // InteriorDials::BlockRecord's.
  uint8_t unknown_2900[12];
};
GUEST_OFFSET(InteriorDials, records, 16);
GUEST_OFFSET(InteriorDials, block_record, 2896);
GUEST_SIZE(InteriorDials, 2912);

// The interior's meters' points (InteriorPoints from +3152), then the
// gauges' providers and table entries.
struct InteriorMeters {
  static constexpr int kGauges = InteriorRecord::kGauges;

  u32_be record;  // The interior's record.
  i32_be block;
  uint8_t unknown_8[8];
  PointRecord records[22];
  u32_be gauge_providers[kGauges];
  u32_be texture_provider;
  u32_be gauge_entries[kGauges];  // GaugeTableEntry's.
  uint8_t unknown_5356[4];
};
GUEST_OFFSET(InteriorMeters, records, 16);
GUEST_OFFSET(InteriorMeters, gauge_providers, 5296);
GUEST_OFFSET(InteriorMeters, texture_provider, 5324);
GUEST_OFFSET(InteriorMeters, gauge_entries, 5328);
GUEST_SIZE(InteriorMeters, 5360);

// The interior (pool 3 of the second set; 11,680 bytes).
struct PolledInterior {
  ScenePoolObjectHeader header;
  // SceneInterior::Link's car model and its model's skeleton and texture
  // providers (a cockpit's interior: the cockpit body's), then the first
  // set's interior (which keeps a copy of the record).
  ptr<SceneCarModel> car_model;
  u32_be skeleton;
  u32_be texture_provider;
  ptr<InteriorObject> interior;
  InteriorRecord record;
  // The points (scene_points.h's InteriorPoints), split where the poll
  // reads them.
  PointRecord points;
  InteriorDials dials;
  InteriorMeters meters;
  PointGroup<4> group_8512;
  PointGroup<4> group_9488;  // +8: the car scene's part model the record's index picks.
  PointGroup<4> group_10464;
  u32_be seat;
  u32_be record_address;
  u32_be skeleton_points;
  u32_be cockpit_points;  // 1 for a cockpit's interior (InteriorPoints::SetCockpit).
  uint8_t unknown_11616[16];
  u32_be part_models[6];  // The first set's interior's parts.
  u32_be in_cockpit;
  u32_be finished;
  u32_be cockpit_texture;  // Set by SceneCockpit::Finish.
  ptr<CarSceneState> scene;
  uint8_t unknown_11672[8];
};
GUEST_OFFSET(PolledInterior, texture_provider, 32);
GUEST_OFFSET(PolledInterior, interior, 36);
GUEST_OFFSET(PolledInterior, record, 40);
GUEST_OFFSET(PolledInterior, points, 160);
GUEST_OFFSET(PolledInterior, dials, 400);
GUEST_OFFSET(PolledInterior, meters, 3312);
GUEST_OFFSET(PolledInterior, group_9488, 160 + 9488);
GUEST_OFFSET(PolledInterior, seat, 11600);
GUEST_OFFSET(PolledInterior, cockpit_points, 160 + 11452);
GUEST_OFFSET(PolledInterior, part_models, 11632);
GUEST_OFFSET(PolledInterior, in_cockpit, 11656);
GUEST_OFFSET(PolledInterior, finished, 11660);
GUEST_OFFSET(PolledInterior, scene, 11668);
GUEST_SIZE(PolledInterior, 11680);

// The cockpit (pool 2 of the second set; 4,320 bytes): its own interior
// and the first set's cockpit body.
struct PolledCockpit {
  ScenePoolObjectHeader header;
  ptr<SceneCarModel> car_model;
  ptr<PolledInterior> interior;
  ptr<CockpitObject> body;  // The first set's.
  CockpitRecord record;
  uint8_t unknown_180[12];
  CockpitPoints points;
  u32_be record_address;
  uint8_t unknown_4276[12];
  u32_be part_models[6];
  u32_be finished;
  ptr<CarSceneState> scene;
};
GUEST_OFFSET(PolledCockpit, interior, 28);
GUEST_OFFSET(PolledCockpit, body, 32);
GUEST_OFFSET(PolledCockpit, record, 36);
GUEST_OFFSET(PolledCockpit, points, 192);
GUEST_OFFSET(PolledCockpit, record_address, 4272);
GUEST_OFFSET(PolledCockpit, part_models, 4288);
GUEST_OFFSET(PolledCockpit, finished, 4312);
GUEST_OFFSET(PolledCockpit, scene, 4316);
GUEST_SIZE(PolledCockpit, 4320);

// The lights (pool 4 of the second set; 13,168 bytes).
struct PolledLights {
  ScenePoolObjectHeader header;
  ptr<SceneCarModel> car_model;
  ptr<Pool4Object> object;  // The first set's.
  Pool4Record record;
  uint8_t unknown_84[12];
  LightPoints points;
  ptr<PartModel> model;  // The object's model, once finished.
  u32_be finished;
  ptr<CarSceneState> scene;
  uint8_t unknown_13164[4];
};
GUEST_OFFSET(PolledLights, object, 28);
GUEST_OFFSET(PolledLights, record, 32);
GUEST_OFFSET(PolledLights, points, 96);
GUEST_OFFSET(PolledLights, model, 13152);
GUEST_OFFSET(PolledLights, scene, 13160);
GUEST_SIZE(PolledLights, 13168);

// A traffic car's body (pool 6 of the second set; 9,808 bytes).
struct PolledTraffic {
  ScenePoolObjectHeader header;
  uint8_t unknown_24[8];
  SceneCarRecord record;
  // The points (scene_points.h's TrafficPoints), split where the poll
  // writes them.
  uint8_t points[1456];
  u32_be plate_header;    // The traffic car's two plate textures' headers.
  u32_be plate_header_b;
  uint8_t points_1464[4568];
  ptr<TrafficCarObject> body;  // The first set's.
  u32_be skeleton;
  u32_be part_models[6];
  u32_be finished;  // Never set: the poll asks again until the model stops asking.
  ptr<CarSceneState> scene;
  uint8_t unknown_9800[8];
};
GUEST_OFFSET(PolledTraffic, record, 32);
GUEST_OFFSET(PolledTraffic, points, 3728);
GUEST_OFFSET(PolledTraffic, plate_header, 5184);
GUEST_OFFSET(PolledTraffic, body, 9760);
GUEST_OFFSET(PolledTraffic, part_models, 9768);
GUEST_OFFSET(PolledTraffic, finished, 9792);
GUEST_OFFSET(PolledTraffic, scene, 9796);
GUEST_SIZE(PolledTraffic, 9808);

// What the wheels' points are found with.
struct WheelPointLinks {
  u32_be car_model;
  u32_be traffic_record;
  u32_be tire_positions;
  u32_be record;
  u32_be tires;
  u32_be rims;
  u32_be calipers;
};
GUEST_SIZE(WheelPointLinks, 28);

// The wheels (pool 5 of the second set; 16,800 bytes).
struct PolledWheels {
  ScenePoolObjectHeader header;
  ptr<SceneCarModel> car_model;  // 0 for traffic.
  ptr<Wheels> wheels;            // The first set's.
  WheelsRecord record;
  uint8_t unknown_188[4];
  // The points (scene_points.h's WheelPoints): what they're found with,
  // then the records.
  WheelPointLinks links;
  uint8_t points_28[16548];
  u32_be finished;
  u32_be points_set;
  u32_be traffic_record;  // A traffic car's record (SceneWheels::SetRecord).
  u32_be tire_positions;  // The traffic body's, set by the poll.
  ptr<CarSceneState> scene;
  uint8_t unknown_16788[12];
};
GUEST_OFFSET(PolledWheels, wheels, 28);
GUEST_OFFSET(PolledWheels, record, 32);
GUEST_OFFSET(PolledWheels, links, 192);
GUEST_OFFSET(PolledWheels, finished, 16768);
GUEST_OFFSET(PolledWheels, traffic_record, 16776);
GUEST_OFFSET(PolledWheels, tire_positions, 16780);
GUEST_OFFSET(PolledWheels, scene, 16784);
GUEST_SIZE(PolledWheels, 16800);

// --- The load poll's functions (load_poll.cpp) ---

using guest::Guest;

struct SceneModel;

uint32_t SceneModelPollLoad(Guest& g, SceneModel* model);
void SceneModelApplyPaint(Guest& g, SceneModel* model);
void SceneModelApplyWheelPaint(Guest& g, SceneModel* model);
uint32_t SceneModelApplyLightKit(Guest& g, SceneModel* model);
void ScenePartsBuild(Guest& g, PolledParts* parts);
uint32_t SceneInteriorSetGauges(Guest& g, PolledInterior* interior);
uint32_t WheelsSetRecord(Guest& g, Wheels* wheels, uint32_t record);
uint32_t WheelsSetCaliperTexture(Guest& g, Wheels* wheels);

}  // namespace game
