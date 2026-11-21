// The scene models' load poll as our own C++ (--native_load_poll, on by
// default): SceneModel::PollLoad and the finishers it runs as each part's
// files arrive (the car model's check, the parts' build, the interior's
// dials and gauges, the cockpit, the lights, the traffic body, the wheels),
// then the steps run once everything is in: the palettes, the wheels'
// points, the vinyl textures, the paints and the light kit's record.
//
// The first set's load checks, part lookups, palettes, paints and texture
// binds are scene_loaders.cpp's C++, called directly; the materials, the
// point searches and the models' own functions are called as they are.

#include "game/load_poll.h"

#include "base/settings.h"
#include "game/car_models.h"
#include "game/scene_models.h"

#include <cstring>

SBX_SETTING_BOOL(native_load_poll, true, "Overrides", "Data and Saves",
                 "Our own C++ for the scene models' load poll (SceneModel::PollLoad, the "
                 "parts', interior's, cockpit's, lights', traffic's and wheels' finishers, "
                 "the palettes, paints and records applied once they're in) instead of the "
                 "recompiled functions");

namespace {

using game::CarModel;
using game::CarParts;
using game::CarSceneState;
using game::CockpitObject;
using game::GaugeChoice;
using game::InteriorDials;
using game::InteriorMeters;
using game::InteriorRecord;
using game::LoadSlot;
using game::PartModel;
using game::PartPaint;
using game::PolledCockpit;
using game::PolledInterior;
using game::PolledLights;
using game::PolledParts;
using game::PolledTraffic;
using game::PolledWheels;
using game::SceneModel;
using game::TrafficCarObject;
using game::Vector;
using game::Wheels;
using game::WheelsLoads;
using guest::AddressOf;
using guest::At;
using guest::be;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

bool On() { return SBX_GET(native_load_poll); }

constexpr uint32_t kCarScene = 0x826B2B24;  // Holds the car scene (writable).
constexpr uint32_t kGauges = 0x820035E0;    // Read-only: 16 bytes an entry, two a gauge.

constexpr uint32_t kAllShapes = uint32_t(-1);

CarSceneState* Scene() { return At<CarSceneState>(*At<u32_be>(kCarScene)); }

// PartModel::SetTexture (sub_820D1420): a texture on a model's shapes (-1
// for all of them).
uint32_t SetTexture(Guest& g, uint32_t model, uint32_t texture, uint32_t shape) {
  return g.Call(sub_820D1420, model, texture, shape);
}

// The first set's car model, through a second-set object's car model.
CarModel* FirstSetModel(const game::SceneCarModel* car_model) { return car_model->model; }

// A paint value (a color: 16 bytes) as the game passes it, in two 64-bit
// registers.
struct PaintWords {
  uint64_t high, low;

  static PaintWords Of(const Vector& v) {
    const auto* words = reinterpret_cast<const be<uint64_t>*>(&v);
    return {words[0], words[1]};
  }
};

// The three body colors as a SetPaint walk hands them on: six 64-bit words
// (PartPaint), each kept whole.
PartPaint BodyPaintOf(const Vector (&colors)[3]) {
  PartPaint paint;
  for (int i = 0; i < 3; i++) {
    const PaintWords words = PaintWords::Of(colors[i]);
    paint.words[2 * i] = words.high;
    paint.words[2 * i + 1] = words.low;
  }
  return paint;
}

// The six wheel colors (PartModel::SetPaintBody's three at a time).
struct WheelPaints {
  PaintWords p[6];
};

// --- The car model, parts and interior ---

// CarModel::IsReady: CarModel::IsLoaded for set a or b, else 0.
uint32_t CarModelReady(CarModel* model) {
  if (model->first_set->model_set > 1) {
    return 0;
  }
  return game::CarModelIsLoaded(model) ? 1 : 0;
}

// SceneParts::SetSelectedTexture: each part model present gets the car
// model's selected texture.
void SetSelectedTexture(Guest& g, PolledParts* parts) {
  for (const u32_be& part : parts->part_models) {
    if (part != 0) {
      At<PartModel>(part)->unknown_452[0] = game::CarModelSelectedTexture(g, FirstSetModel(parts->car_model));
    }
  }
}

// SceneParts::Build: the first set's part model for each kind
// (CarParts::Part, by the record's choices), the points found on them, the
// parts' materials walked and the selected texture set.
void BuildParts(Guest& g, PolledParts* parts) {
  const u32_be* choice = parts->record.unknown_0;
  // Each kind's two words for CarParts::Part (the second 0 for most).
  const uint32_t words[PolledParts::kParts][2] = {
      {choice[0], 0},         {choice[1], choice[0]}, {choice[2], 0},  {choice[3], 0},
      {choice[4], 0},         {choice[5], 0},         {choice[6], 0},  {choice[6], 0},
      {choice[7], choice[6]}, {choice[7], choice[6]}, {choice[8], 0},  {choice[9], 0},
      {choice[10], 0},        {0, 0},                 {choice[11], 0}, {choice[12], 0},
      {0, 0},
  };
  constexpr uint32_t kNoRearSpoiler = 13;  // Kind 4's choice for none.
  for (uint32_t kind = 0; kind < PolledParts::kParts; kind++) {
    if (kind == 4 && words[kind][0] == kNoRearSpoiler) {
      parts->part_models[kind] = 0;
      continue;
    }
    parts->part_models[kind] =
        game::CarPartsPart(parts->parts, int32_t(kind), words[kind][0], words[kind][1]);
  }
  g.Call(sub_820DFC98, &parts->points);  // PartsPoints::Reset
  CarModel* model = FirstSetModel(parts->car_model);
  parts->points_car_model = parts->car_model.address;
  parts->record_address = AddressOf(&parts->record);
  parts->car_record = AddressOf(&model->record);
  parts->points_skeleton = AddressOf(model->loads.model.skeleton);
  g.Call(sub_820DFE00, &parts->points, parts->part_models);  // PartsPoints::Find
  game::CarPartsWalkMaterials(g, parts->parts);
  SetSelectedTexture(g, parts);
}

// SceneParts::Finish: 1 once the first set's parts are loaded; then (for a
// scene of kind 0 or 1) the parts built.
uint32_t FinishParts(Guest& g, PolledParts* parts) {
  if (parts->loaded != 0) {
    return 1;
  }
  const uint32_t loaded = game::CarPartsIsLoaded(parts->parts) ? 1 : 0;
  parts->loaded = loaded;
  if (loaded != 0 && parts->scene->model_set <= 1) {
    BuildParts(g, parts);
  }
  return loaded;
}

// InteriorDials::SetBlock: the car's block, its record, and the dials'
// lights by which of two lists of blocks it's in (else left as they
// were).
void SetDialsBlock(Guest& g, InteriorDials* dials, int32_t block) {
  dials->block = block;
  dials->block_record = g.Call(sub_820D3370, uint32_t(block));  // InteriorDials::BlockRecord
  if (g.Call(sub_820D3280, uint32_t(int32_t(dials->block))) != 0) {
    dials->lights = AddressOf(&Scene()->assets.loads_952[1].list);
  } else if (g.Call(sub_820D3248, uint32_t(int32_t(dials->block))) != 0) {
    dials->lights = AddressOf(&Scene()->assets.loads_952[0].list);
  }
}

// GaugeTableEntry: the gauge's first entry, or its second for levels 3 and
// up.
uint32_t GaugeTableEntry(uint32_t gauge, int32_t level) {
  constexpr uint32_t kEntryBytes = 16;
  const uint32_t second = level >= 3 ? 1 : 0;
  return (gauge * 2 + second) * kEntryBytes + kGauges;
}

// InteriorMeters::SetGauge: gauge 0 to 6 given its shared image's provider,
// its point record's model and shape the choice's texture (3 at most in a
// race), and its table entry (returned).
uint32_t SetGauge(Guest& g, InteriorMeters* meters, uint32_t gauge, uint32_t provider) {
  static constexpr int kGaugeRecords[InteriorMeters::kGauges] = {7, 8, 11, 12, 15, 16, 17};
  meters->gauge_providers[gauge] = provider;
  const game::PointRecord& point = meters->records[kGaugeRecords[gauge]];
  const GaugeChoice& choice = At<InteriorRecord>(meters->record)->Gauge(gauge);
  int32_t texture = choice.texture;
  if (Scene()->model_set != 0 && texture > 2) {
    texture = 3;
  }
  SetTexture(g, point.unknown_4, uint32_t(texture), uint32_t(int32_t(point.unknown_228)));
  meters->gauge_entries[gauge] = GaugeTableEntry(choice.image, choice.texture);
  return meters->gauge_entries[gauge];
}

// SceneInterior::SetGauges: each enabled gauge set from its choice (the
// second skipped on block 5). Returns the original's r3: the last gauge's
// table entry, or the interior.
uint32_t SetGauges(Guest& g, PolledInterior* interior) {
  uint32_t result = AddressOf(interior);
  for (uint32_t gauge = 0; gauge < InteriorMeters::kGauges; gauge++) {
    if (uint32_t(FirstSetModel(interior->car_model)->record.unknown_0) == 5 && gauge == 1) {
      continue;
    }
    const GaugeChoice& choice = interior->record.Gauge(gauge);
    if (choice.enabled == 0) {
      continue;
    }
    const uint32_t provider = AddressOf(guest::Element<game::XPRList>(
        AddressOf(interior->scene->assets.lists_4752), choice.image));
    result = SetGauge(g, &interior->meters, gauge, provider);
  }
  return result;
}

// SceneInterior::Finish: once the first set's interior is loaded, its six
// part models given the texture provider and the cockpit's texture (or the
// car model's selected one), the dials and meters set for the car's block,
// the points found and the gauges set. Answers Interior::IsLoaded's.
uint32_t FinishInterior(Guest& g, PolledInterior* interior) {
  if (interior->finished != 0) {
    return 1;
  }
  const uint32_t loaded = game::InteriorIsLoaded(g, interior->interior) ? 1 : 0;
  if (loaded == 0) {
    return 0;
  }
  for (int i = 0; i < 6; i++) {
    PartModel* part = &interior->interior->parts[i];
    interior->part_models[i] = AddressOf(part);
    part->model.unknown_48 = interior->texture_provider;
    if (interior->in_cockpit != 0) {
      part->unknown_452[0] = interior->cockpit_texture;
    } else {
      part->unknown_452[0] = game::CarModelSelectedTexture(g, FirstSetModel(interior->car_model));
    }
  }
  CarModel* model = FirstSetModel(interior->car_model);
  const int32_t block = model->record.unknown_0;
  SetDialsBlock(g, &interior->dials, block);
  interior->meters.block = block;
  interior->meters.texture_provider = interior->texture_provider;
  interior->record_address = AddressOf(&interior->record);
  interior->seat = AddressOf(&FirstSetModel(interior->car_model)->seat_positions[0]);
  interior->skeleton_points = AddressOf(FirstSetModel(interior->car_model)->loads.model.skeleton);
  g.Call(sub_820E3228, &interior->points, interior->part_models);  // InteriorPoints::Find
  SetGauges(g, interior);
  interior->finished = 1;
  return loaded;
}

// --- The cockpit, lights, traffic and wheels ---

// SceneCockpit::Finish: once the cockpit's body has its textures, the
// interior given the body's texture and finished, then the body's six part
// models and the points found.
uint32_t FinishCockpit(Guest& g, PolledCockpit* cockpit) {
  if (cockpit->finished != 0) {
    return 1;
  }
  if (!game::CockpitBindTextures(g, cockpit->body)) {
    return 0;
  }
  cockpit->interior->cockpit_texture = cockpit->body->texture;
  if (FinishInterior(g, cockpit->interior) == 0) {
    return 0;
  }
  cockpit->finished = 1;
  for (int i = 0; i < 6; i++) {
    cockpit->part_models[i] = AddressOf(&cockpit->body->loads.parts[i]);
  }
  g.Call(sub_820DB8F8, &cockpit->points, cockpit->part_models);  // CockpitPoints::Find
  return 1;
}

// SceneLights::Finish: once the pool 4 object is loaded, its model given
// the record's texture and the car scene's light provider, and the points
// found.
uint32_t FinishLights(Guest& g, PolledLights* lights) {
  if (lights->finished != 0) {
    return 1;
  }
  if (!game::Pool4ObjectIsLoaded(lights->object)) {
    return 0;
  }
  lights->finished = 1;
  lights->model = &lights->object->model;
  SetTexture(g, lights->model.address, lights->record.texture, kAllShapes);
  lights->model->model.unknown_48 = AddressOf(&lights->scene->assets.loads_184[1].list);
  g.Call(sub_820DC570, &lights->points, lights->model);  // LightPoints::Find
  return 1;
}

// SceneLights::SetRecord: the record copied in, then given to the pool 4
// object.
void SetLightsRecord(Guest& g, PolledLights* lights, uint32_t record) {
  g.Call(sub_8258CB90, &lights->record, record, uint32_t(sizeof(lights->record)));  // memcpy
  game::Pool4ObjectSetRecord(g, lights->object, record);
}

// SceneLights::ApplyRecord: the points' words cleared, the record set,
// then its texture set on the lights' model (PartModel::SetTexture's
// answer).
uint32_t ApplyLightsRecord(Guest& g, PolledLights* lights, uint32_t record) {
  for (u32_be& v : lights->points.unknown_8) {
    v = 0;
  }
  SetLightsRecord(g, lights, record);
  return SetTexture(g, lights->model.address, lights->record.texture, kAllShapes);
}

// SceneTraffic::Finish: once the traffic body has its textures, its
// skeleton, part models and plate textures linked and the points found.
// (Its finished flag is never set.)
uint32_t FinishTraffic(Guest& g, PolledTraffic* traffic) {
  if (traffic->finished != 0) {
    return 1;
  }
  if (!game::TrafficCarBindTextures(g, traffic->body)) {
    return 0;
  }
  TrafficCarObject* body = traffic->body;
  traffic->skeleton = AddressOf(body->loads.skeleton);
  for (int i = 0; i < 6; i++) {
    traffic->part_models[i] = AddressOf(&traffic->body->loads.parts[i]);
  }
  traffic->plate_header_b = traffic->body->plate.header_b;
  traffic->plate_header = traffic->body->plate.header;
  g.Call(sub_820E54B8, traffic->points, traffic->part_models);  // TrafficPoints::Find
  return 1;
}

// --- The wheels' textures and palettes ---

// What PartModel::SetMaterials (sub_820D2F60) changes in each material:
// six flags, then what each flag sets (three vectors at the material's +0,
// +32, +16 and three floats at +80, +84, +88). Passed by value: its first
// 24 bytes and the vectors in r4 to r10 and the caller's stack.
struct MaterialChange {
  u32_be flags[6];
  uint8_t unknown_24[8];  // Not passed.
  game::Vector vectors[3];
  guest::f32_be floats[3];
  uint8_t unknown_92[4];
};
GUEST_SIZE(MaterialChange, 96);

// The frame the wheels' SetMaterials calls are made from (Wheels::
// BindTextures' 352 bytes, ApplyPalette's 384): the change's tail copied
// to +96 (SetMaterials' stack arguments), the material selectors at +132,
// +140 and +148, the change at +160.
struct MaterialsFrame {
  uint8_t unknown_0[96];
  uint8_t stack_tail[32];  // +96: the change's +64 to +95.
  uint8_t unknown_128[4];
  i32_be unknown_132;  // -1.
  uint8_t unknown_136[4];
  i32_be category;  // +140: the materials picked (-1 any).
  uint8_t unknown_144[4];
  i32_be unknown_148;  // -1.
  uint8_t unknown_152[8];
  MaterialChange change;  // +160
};
GUEST_OFFSET(MaterialsFrame, unknown_132, 132);
GUEST_OFFSET(MaterialsFrame, category, 140);
GUEST_OFFSET(MaterialsFrame, change, 160);

// PartModel::SetMaterials on a part model with the frame's change, for
// the materials of `category`. The change's words the originals never
// write (they're passed on, and SetMaterials uses only what the flags
// select) are 0 here.
void SetMaterials(Guest& g, MaterialsFrame* frame, PartModel* part, int32_t category) {
  frame->unknown_132 = -1;
  frame->category = category;
  frame->unknown_148 = -1;
  memcpy(frame->stack_tail, reinterpret_cast<const uint8_t*>(&frame->change) + 64, 32);
  const auto* words = reinterpret_cast<const be<uint64_t>*>(&frame->change);
  const auto* vectors = reinterpret_cast<const be<uint64_t>*>(frame->change.vectors);
  g.Call(sub_820D2F60, part, uint64_t(words[0]), uint64_t(words[1]), uint64_t(words[2]),
         uint64_t(vectors[0]), uint64_t(vectors[1]), uint64_t(vectors[2]),
         uint64_t(vectors[3]));  // PartModel::SetMaterials
}

// The change both make: every flag clear, every vector's w 1.0.
void ClearMaterialChange(MaterialChange& change) {
  change = {};
  const float one = *At<guest::f32_be>(0x82000A20);  // 1.0 (read-only)
  for (game::Vector& vector : change.vectors) {
    vector.w = one;
  }
}

// Wheels::BindTextures: 0 while the calipers' second load is loading.
// Else the models' texture providers (the tires and calipers the car
// scene's, the rims or brakes the wheels' own), the record's tire
// textures on the tires, and the tires' materials' +84 set to 0.0. 1.
uint32_t BindWheelTextures(Guest& g, Wheels* wheels) {
  if (wheels->loads.slots_2088[1].state == LoadSlot::kLoading) {
    return 0;
  }
  CarSceneState* scene = Scene();
  WheelsLoads& loads = wheels->loads;
  loads.parts_48[0].model.unknown_48 = AddressOf(&scene->assets.lists_144[0]);
  loads.parts_1024[0].model.unknown_48 = AddressOf(&loads.lists[0]);
  loads.parts_1024[1].model.unknown_48 = AddressOf(&loads.lists[1]);
  loads.parts_48[1].model.unknown_48 = AddressOf(&scene->assets.lists_144[0]);
  loads.parts_2144[0].model.unknown_48 = AddressOf(&scene->assets.loads_184[0].list);
  loads.parts_2144[1].model.unknown_48 = AddressOf(&scene->assets.loads_184[0].list);
  SetTexture(g, AddressOf(&loads.parts_48[0]), wheels->record.unknown_4[2], kAllShapes);
  SetTexture(g, AddressOf(&loads.parts_48[1]), wheels->record.unknown_28, kAllShapes);

  guest::StackFrame stack(g.ctx, 352);
  MaterialsFrame* frame = At<MaterialsFrame>(stack.Local(0));
  ClearMaterialChange(frame->change);
  frame->change.flags[4] = 1;  // Set the materials' +84
  frame->change.floats[1] = *At<guest::f32_be>(0x82000A24);  // to 0.0 (read-only).
  SetMaterials(g, frame, &loads.parts_48[0], -1);
  SetMaterials(g, frame, &loads.parts_48[1], -1);
  return 1;
}

// Wheels::ApplyPalette: the paint palette on the six models
// (PartModel::ApplyPalette), then the tires' and rims' (or brakes')
// materials of categories 1 and 2 given the record's two floats for their
// end (+80, +84 of each material), front then rear.
void ApplyWheelPalette(Guest& g, Wheels* wheels) {
  WheelsLoads& loads = wheels->loads;
  guest::StackFrame stack(g.ctx, 384);
  MaterialsFrame* frame = At<MaterialsFrame>(stack.Local(0));
  ClearMaterialChange(frame->change);
  PartModel* models[6] = {&loads.parts_48[0],   &loads.parts_48[1],   &loads.parts_1024[0],
                          &loads.parts_1024[1], &loads.parts_2144[0], &loads.parts_2144[1]};
  for (PartModel* part : models) {
    g.Call(sub_820D2AF0, part);  // PartModel::ApplyPalette
  }
  frame->change.flags[3] = 1;
  frame->change.flags[4] = 1;
  const game::Record12* ends[2] = {&wheels->record.unknown_16, &wheels->record.unknown_32};
  for (int end = 0; end < 2; end++) {
    frame->change.floats[0] = ends[end]->unknown_4;
    frame->change.floats[1] = ends[end]->unknown_8;
    for (PartModel* part : {&loads.parts_48[end], &loads.parts_1024[end]}) {
      SetMaterials(g, frame, part, 1);
      SetMaterials(g, frame, part, 2);
    }
  }
}

// SceneWheels::Finish: 1 once the first set's wheels have their textures
// (Wheels::BindTextures).
uint32_t FinishWheels(Guest& g, PolledWheels* wheels) {
  if (wheels->finished != 0) {
    return 1;
  }
  if (BindWheelTextures(g, wheels->wheels) == 0) {
    return 0;
  }
  wheels->finished = 1;
  return 1;
}

// Wheels::SetRecord: the record copied in (memcpy's answer).
uint32_t SetWheelsRecord(Guest& g, Wheels* wheels, uint32_t record) {
  return g.Call(sub_8258CB90, &wheels->record, record, uint32_t(sizeof(wheels->record)));  // memcpy
}

// Wheels::SetCaliperTexture: the record's caliper texture on both calipers
// (the second's PartModel::SetTexture answer).
uint32_t SetCaliperTexture(Guest& g, Wheels* wheels) {
  uint32_t result = 0;
  for (PartModel& caliper : wheels->loads.parts_2144) {
    result = SetTexture(g, AddressOf(&caliper), wheels->record.caliper_texture, kAllShapes);
  }
  return result;
}

// SceneWheels::SetUpPoints: once, the points' links (the car model, or a
// traffic car's record and tire positions; the record; the first set's
// tires, rims or brakes and calipers), the points set up, the record given
// to the first set's wheels and the calipers' texture set.
void SetUpWheelPoints(Guest& g, PolledWheels* wheels) {
  if (wheels->points_set != 0) {
    return;
  }
  if (wheels->car_model != nullptr) {
    wheels->links.car_model = wheels->car_model.address;
  } else {
    wheels->links.traffic_record = wheels->traffic_record;
    wheels->links.tire_positions = wheels->tire_positions;
  }
  Wheels* first = wheels->wheels;
  wheels->links.record = AddressOf(&wheels->record);
  wheels->links.tires = AddressOf(&first->loads.parts_48[0]);
  wheels->links.rims = AddressOf(&first->loads.parts_1024[0]);
  wheels->links.calipers = AddressOf(&first->loads.parts_2144[0]);
  g.Call(sub_820E8250, &wheels->links);  // WheelPoints::SetUp
  SetWheelsRecord(g, wheels->wheels, AddressOf(&wheels->record));
  SetCaliperTexture(g, wheels->wheels);
  wheels->points_set = 1;
}

// --- The palettes and paints ---

// SceneModel::ApplyPalettes: the paint palette on each part's models (the
// first set's functions): a traffic car's body, or a car's parts, interior
// and cockpit; then the wheels.
void ApplyPalettes(Guest& g, SceneModel* model) {
  if (model->record.kind == SceneModel::kTraffic) {
    if (model->traffic != nullptr) {
      game::TrafficCarApplyPalette(g, model->traffic->body);
    }
  } else {
    if (model->parts != nullptr) {
      game::CarPartsApplyPalette(g, model->parts->parts);
    }
    if (model->interior != nullptr) {
      game::InteriorApplyPalette(g, model->interior->interior);
    }
    if (model->cockpit != nullptr) {
      game::CockpitApplyPalette(g, model->cockpit->body);
    }
  }
  if (model->wheels != nullptr) {
    ApplyWheelPalette(g, model->wheels->wheels);
  }
}

// CarParts::SetVinylTextures: each of the first set's part models (216 in
// set a, 187 in set b but parts 7, 9 and 14) given the car model's texture
// provider and vinyl sheet.
void SetPartsVinylTextures(Guest& g, CarParts* parts) {
  int count = 0;
  if (parts->first_set->model_set == 0) {
    count = 216;
  } else if (parts->first_set->model_set == 1) {
    count = 187;
  }
  for (int i = 0; i < count; i++) {
    if (int32_t(uint32_t(parts->first_set->model_set)) == 1 && (i == 7 || i == 9 || i == 14)) {
      continue;
    }
    PartModel* part = &parts->loads.parts[i];
    CarModel* model = parts->head.car_model;
    part->model.unknown_48 = AddressOf(&model->loads.model.texture_providers[0]);
    g.Call(sub_820CE7B8, part, uint32_t(model->loads.sheet.header),
           uint32_t(model->loads.sheet.header_b));  // Model::SetVinylTextures
  }
}

// The second-set objects' paints, each handed to its first-set object's
// (PartModel::SetPaint on the arrived parts).
void ApplyPartsPaint(Guest& g, PolledParts* parts, const PartPaint& paint) {
  game::CarPartsSetPaint(g, parts->parts, paint);
}

void ApplyInteriorPaint(Guest& g, PolledInterior* interior, const PartPaint& paint) {
  game::InteriorSetPaint(g, interior->interior, paint);
}

void ApplyCockpitPaint(Guest& g, PolledCockpit* cockpit, const PartPaint& paint) {
  game::CockpitSetPaint(g, cockpit->body, paint);
  ApplyInteriorPaint(g, cockpit->interior, paint);
}

// TrafficCar::SetPaint: PartModel::SetPaint on each of the body's arrived
// parts, the paint's six words handed on whole.
void SetTrafficPaint(Guest& g, TrafficCarObject* body, const PartPaint& paint) {
  for (int i = 0; i < 6; i++) {
    if (body->loads.part_slots[i].state == LoadSlot::kArrived) {
      const uint64_t* w = paint.words;
      g.Call(sub_820D0AF0, &body->loads.parts[i], w[0], w[1], w[2], w[3], w[4],
             w[5]);  // PartModel::SetPaint
    }
  }
}

void ApplyTrafficPaint(Guest& g, PolledTraffic* traffic, const PartPaint& paint) {
  SetTrafficPaint(g, traffic->body, paint);
}

// SceneModel::ApplyPaint: once ready, the record's three body colors on a
// traffic car's body, or on a car's parts, interior and cockpit (the
// cockpit's body first given the vinyl sheet).
void ApplyPaint(Guest& g, SceneModel* model) {
  if (model->ready == 0) {
    return;
  }
  const PartPaint paint = BodyPaintOf(
      reinterpret_cast<const Vector(&)[3]>(model->record.car.unknown_16[0]));
  // (The original hands the traffic body, parts and interior on without
  // checking them; a car in play has its parts and interior.)
  if (model->record.kind == SceneModel::kTraffic) {
    if (model->traffic != nullptr) {
      ApplyTrafficPaint(g, model->traffic, paint);
    }
    return;
  }
  if (model->parts != nullptr) {
    ApplyPartsPaint(g, model->parts, paint);
  }
  if (model->interior != nullptr) {
    ApplyInteriorPaint(g, model->interior, paint);
  }
  if (model->cockpit != nullptr) {
    game::CockpitSetVinylTextures(g, model->cockpit->body);
    ApplyCockpitPaint(g, model->cockpit, paint);
  }
}

// Wheels::ApplyPaint: PartModel::SetPaintBody on the tires and the rims or
// brakes, front with paints 0 to 2, rear with 3 to 5 (the tires with the
// first two swapped).
void ApplyWheelsPaint(Guest& g, Wheels* wheels, const WheelPaints& paints) {
  auto set = [&](PartModel& part, int a, int b, int c) {
    g.Call(sub_820D0FC0, &part, paints.p[a].high, paints.p[a].low, paints.p[b].high,
           paints.p[b].low, paints.p[c].high, paints.p[c].low);  // PartModel::SetPaintBody
  };
  set(wheels->loads.parts_48[0], 1, 0, 2);
  set(wheels->loads.parts_48[1], 4, 3, 5);
  set(wheels->loads.parts_1024[0], 0, 1, 2);
  set(wheels->loads.parts_1024[1], 3, 4, 5);
}

// SceneModel::ApplyWheelPaint: once ready, for a car whose wheels record
// asks for it, the record's six wheel colors on the wheels.
void ApplyWheelPaint(Guest& g, SceneModel* model) {
  if (model->ready == 0 || model->record.kind == SceneModel::kTraffic ||
      model->record.wheels.unknown_4[0] == 0 || model->wheels == nullptr) {
    return;
  }
  WheelPaints paints;
  for (int i = 0; i < 6; i++) {
    paints.p[i] = PaintWords::Of(model->record.car.unknown_16[3 + i]);
  }
  ApplyWheelsPaint(g, model->wheels->wheels, paints);
}

// SceneModel::ApplyLightKit: once ready, a car's light kit given its
// record (SceneLights::ApplyRecord). Returns the original's r3 (GarageStage::
// ClearLightKit's callers get it): the model when it isn't ready or is a
// traffic car, 0 without lights, else the light texture's SetTexture's.
uint32_t ApplyLightKit(Guest& g, SceneModel* model) {
  if (model->ready == 0 || model->record.kind == SceneModel::kTraffic) {
    return AddressOf(model);
  }
  if (model->lights == nullptr) {
    return 0;
  }
  return ApplyLightsRecord(g, model->lights, AddressOf(&model->record.pool4));
}

// The six wheel paints as SceneWheels::ApplyPaint and Wheels::ApplyPaint
// get them: r4 to r10, then the caller's stack from +88.
WheelPaints WheelPaintArguments(PPCContext& ctx) {
  const auto* stack = At<be<uint64_t>>(ctx.r1.u32 + 88);
  return {{
      {ctx.r4.u64, ctx.r5.u64},
      {ctx.r6.u64, ctx.r7.u64},
      {ctx.r8.u64, ctx.r9.u64},
      {ctx.r10.u64, stack[0]},
      {stack[1], stack[2]},
      {stack[3], stack[4]},
  }};
}

// The three body paints in r4 to r9, each register kept whole.
PartPaint BodyPaintArguments(PPCContext& ctx) {
  return {{ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64}};
}

// --- The poll ---

// SceneModel::PollLoad: 0 while a release or build is pending or any part
// isn't finished, else 1. A traffic car's body and wheels are finished in
// turn; a car's car model (with its decals drawn), parts, interior,
// cockpit, lights and wheels. Once every part's flag is set the model is
// ready; the first time it isn't, everything is marked ready and the
// palettes, wheel points, vinyl textures, paints and light record are
// applied.
uint32_t PollLoad(Guest& g, SceneModel* model) {
  if (model->release_pending != 0 || model->build_pending != 0) {
    return 0;
  }
  if (model->record.kind == SceneModel::kTraffic) {
    if (model->traffic != nullptr && model->traffic_ready == 0) {
      if (FinishTraffic(g, model->traffic) == 0) {
        return 0;
      }
      if (model->wheels != nullptr) {
        model->wheels->tire_positions = AddressOf(model->traffic->body->tire_positions);
      }
    }
  } else {
    if (model->car_model != nullptr) {
      CarModel* car_model = FirstSetModel(model->car_model);
      if (CarModelReady(car_model) == 0 || car_model->loads.decals_state != 3) {
        return 0;
      }
    }
    if (model->parts != nullptr && FinishParts(g, model->parts) == 0) {
      return 0;
    }
    if (model->interior != nullptr && FinishInterior(g, model->interior) == 0) {
      return 0;
    }
    if (model->cockpit != nullptr && FinishCockpit(g, model->cockpit) == 0) {
      return 0;
    }
    if (model->lights != nullptr && FinishLights(g, model->lights) == 0) {
      return 0;
    }
  }
  if (model->wheels != nullptr && model->wheels_ready == 0) {
    if (FinishWheels(g, model->wheels) == 0) {
      return 0;
    }
  }
  if (model->record.kind == SceneModel::kTraffic) {
    return 1;  // (A traffic car's ready flag isn't set here.)
  }
  if (model->car_model == nullptr && model->parts == nullptr && model->interior == nullptr &&
      model->wheels == nullptr) {
    return 1;
  }

  // In the front end (the scene's set a) the cockpit's flag isn't needed.
  const bool cockpit_needed = model->scene->model_set != 0;
  if (model->car_model_ready != 0 && model->parts_ready != 0 && model->interior_ready != 0 &&
      model->decals_ready != 0 && (!cockpit_needed || model->cockpit_ready != 0) &&
      model->wheels_ready != 0 && model->lights_ready != 0) {
    model->ready = 1;
  }
  if (model->ready != 0) {
    return 1;
  }
  model->ready = 1;
  model->lights_ready = 1;
  model->cockpit_ready = 1;
  model->interior_ready = 1;
  model->parts_ready = 1;
  model->decals_ready = 1;
  model->car_model_ready = 1;
  model->wheels_ready = 1;
  ApplyPalettes(g, model);
  if (model->wheels_ready != 0 && model->wheels != nullptr) {
    SetUpWheelPoints(g, model->wheels);
  }
  if (model->parts != nullptr) {
    SetPartsVinylTextures(g, model->parts->parts);
  }
  ApplyPaint(g, model);
  if (model->ready != 0 && model->record.kind != SceneModel::kTraffic &&
      model->lights != nullptr) {
    ApplyLightsRecord(g, model->lights, AddressOf(&model->record.pool4));
  }
  ApplyWheelPaint(g, model);
  return 1;
}

}  // namespace

// --- What the group's other files call ---

namespace game {

uint32_t SceneModelPollLoad(Guest& g, SceneModel* model) { return PollLoad(g, model); }
void SceneModelApplyPaint(Guest& g, SceneModel* model) { ApplyPaint(g, model); }
void SceneModelApplyWheelPaint(Guest& g, SceneModel* model) { ApplyWheelPaint(g, model); }
uint32_t SceneModelApplyLightKit(Guest& g, SceneModel* model) { return ApplyLightKit(g, model); }
void ScenePartsBuild(Guest& g, PolledParts* parts) { BuildParts(g, parts); }
uint32_t SceneInteriorSetGauges(Guest& g, PolledInterior* interior) {
  return SetGauges(g, interior);
}
uint32_t WheelsSetRecord(Guest& g, Wheels* wheels, uint32_t record) {
  return SetWheelsRecord(g, wheels, record);
}
uint32_t WheelsSetCaliperTexture(Guest& g, Wheels* wheels) { return SetCaliperTexture(g, wheels); }

}  // namespace game

#define SBX_LOAD_POLL_FALLBACK(name) \
  REX_FUNC_PROLOGUE();               \
  if (!On()) {                       \
    __imp__##name(ctx, base);        \
    return;                          \
  }                                  \
  Guest g(ctx, base);

// SceneModel::PollLoad: r3 the scene model. 1 when it's loaded.
extern "C" REX_FUNC(sub_820D9FE0) {
  SBX_LOAD_POLL_FALLBACK(sub_820D9FE0)
  ctx.r3.u64 = game::SceneModelPollLoad(g, At<SceneModel>(ctx.r3.u32));
}

// CarModel::IsReady: r3 the car model.
extern "C" REX_FUNC(sub_820D3CE8) {
  SBX_LOAD_POLL_FALLBACK(sub_820D3CE8)
  ctx.r3.u64 = CarModelReady(At<CarModel>(ctx.r3.u32));
}

// The finishers: r3 the second-set object. 1 when it's finished.
extern "C" REX_FUNC(sub_820E2A30) {  // SceneParts::Finish
  SBX_LOAD_POLL_FALLBACK(sub_820E2A30)
  ctx.r3.u64 = FinishParts(g, At<PolledParts>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820E3D08) {  // SceneInterior::Finish
  SBX_LOAD_POLL_FALLBACK(sub_820E3D08)
  ctx.r3.u64 = FinishInterior(g, At<PolledInterior>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820DBC68) {  // SceneCockpit::Finish
  SBX_LOAD_POLL_FALLBACK(sub_820DBC68)
  ctx.r3.u64 = FinishCockpit(g, At<PolledCockpit>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820DD6D8) {  // SceneLights::Finish
  SBX_LOAD_POLL_FALLBACK(sub_820DD6D8)
  ctx.r3.u64 = FinishLights(g, At<PolledLights>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820E5658) {  // SceneTraffic::Finish
  SBX_LOAD_POLL_FALLBACK(sub_820E5658)
  ctx.r3.u64 = FinishTraffic(g, At<PolledTraffic>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820E6F10) {  // SceneWheels::Finish
  SBX_LOAD_POLL_FALLBACK(sub_820E6F10)
  ctx.r3.u64 = FinishWheels(g, At<PolledWheels>(ctx.r3.u32));
}

// SceneParts::Build and SetSelectedTexture: r3 the parts.
extern "C" REX_FUNC(sub_820E0188) {
  SBX_LOAD_POLL_FALLBACK(sub_820E0188)
  game::ScenePartsBuild(g, At<PolledParts>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820DF018) {
  SBX_LOAD_POLL_FALLBACK(sub_820DF018)
  SetSelectedTexture(g, At<PolledParts>(ctx.r3.u32));
}

// InteriorDials::SetBlock: r3 the dials, r4 the car's block.
extern "C" REX_FUNC(sub_820E2C08) {
  SBX_LOAD_POLL_FALLBACK(sub_820E2C08)
  SetDialsBlock(g, At<InteriorDials>(ctx.r3.u32), ctx.r4.s32);
}

// SceneInterior::SetGauges: r3 the interior.
extern "C" REX_FUNC(sub_820E3500) {
  SBX_LOAD_POLL_FALLBACK(sub_820E3500)
  ctx.r3.u64 = game::SceneInteriorSetGauges(g, At<PolledInterior>(ctx.r3.u32));
}

// InteriorMeters::SetGauge: r3 the meters, r4 the gauge, r5 its provider.
// Gauges past 6 (never asked) run the original.
extern "C" REX_FUNC(sub_820E2EB0) {
  REX_FUNC_PROLOGUE();
  if (!On() || ctx.r4.u32 >= uint32_t(InteriorMeters::kGauges)) {
    __imp__sub_820E2EB0(ctx, base);
    return;
  }
  Guest g(ctx, base);
  ctx.r3.u64 = SetGauge(g, At<InteriorMeters>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// GaugeTableEntry: r3 the gauge, r4 the level.
extern "C" REX_FUNC(sub_820D32A0) {
  SBX_LOAD_POLL_FALLBACK(sub_820D32A0)
  ctx.r3.u64 = GaugeTableEntry(ctx.r3.u32, ctx.r4.s32);
}

// SceneLights::SetRecord and ApplyRecord: r3 the lights, r4 the record (52
// bytes).
extern "C" REX_FUNC(sub_820DC7E8) {
  SBX_LOAD_POLL_FALLBACK(sub_820DC7E8)
  SetLightsRecord(g, At<PolledLights>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_820DC838) {
  SBX_LOAD_POLL_FALLBACK(sub_820DC838)
  ApplyLightsRecord(g, At<PolledLights>(ctx.r3.u32), ctx.r4.u32);
}

// SceneModel::ApplyLightKit: r3 the scene model.
extern "C" REX_FUNC(sub_820D99D8) {
  SBX_LOAD_POLL_FALLBACK(sub_820D99D8)
  ctx.r3.u64 = game::SceneModelApplyLightKit(g, At<SceneModel>(ctx.r3.u32));
}

// Wheels::BindTextures: r3 the first set's wheels. 1 when bound.
extern "C" REX_FUNC(sub_820D9300) {
  SBX_LOAD_POLL_FALLBACK(sub_820D9300)
  ctx.r3.u64 = BindWheelTextures(g, At<Wheels>(ctx.r3.u32));
}

// Wheels::ApplyPalette: r3 the first set's wheels.
extern "C" REX_FUNC(sub_820D8F68) {
  SBX_LOAD_POLL_FALLBACK(sub_820D8F68)
  ApplyWheelPalette(g, At<Wheels>(ctx.r3.u32));
}

// SceneWheels::SetUpPoints: r3 the wheels.
extern "C" REX_FUNC(sub_820E84F0) {
  SBX_LOAD_POLL_FALLBACK(sub_820E84F0)
  SetUpWheelPoints(g, At<PolledWheels>(ctx.r3.u32));
}

// Wheels::SetRecord: r3 the wheels, r4 the record (156 bytes) copied in.
extern "C" REX_FUNC(sub_820D8B88) {
  SBX_LOAD_POLL_FALLBACK(sub_820D8B88)
  game::WheelsSetRecord(g, At<Wheels>(ctx.r3.u32), ctx.r4.u32);
}

// Wheels::SetCaliperTexture: r3 the wheels.
extern "C" REX_FUNC(sub_820D8B98) {
  SBX_LOAD_POLL_FALLBACK(sub_820D8B98)
  game::WheelsSetCaliperTexture(g, At<Wheels>(ctx.r3.u32));
}

// SceneModel::ApplyPalettes: r3 the scene model.
extern "C" REX_FUNC(sub_820D9B68) {
  SBX_LOAD_POLL_FALLBACK(sub_820D9B68)
  ApplyPalettes(g, At<SceneModel>(ctx.r3.u32));
}

// CarParts::SetVinylTextures: r3 the first set's car parts.
extern "C" REX_FUNC(sub_820D6F88) {
  SBX_LOAD_POLL_FALLBACK(sub_820D6F88)
  SetPartsVinylTextures(g, At<CarParts>(ctx.r3.u32));
}

// SceneModel::ApplyPaint and ApplyWheelPaint: r3 the scene model.
extern "C" REX_FUNC(sub_820D9830) {
  SBX_LOAD_POLL_FALLBACK(sub_820D9830)
  game::SceneModelApplyPaint(g, At<SceneModel>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D9918) {
  SBX_LOAD_POLL_FALLBACK(sub_820D9918)
  game::SceneModelApplyWheelPaint(g, At<SceneModel>(ctx.r3.u32));
}

// The objects' paints: r3 the object, r4 to r9 the three paints (each
// register whole).
extern "C" REX_FUNC(sub_820E04B0) {  // SceneParts::ApplyPaint
  SBX_LOAD_POLL_FALLBACK(sub_820E04B0)
  ApplyPartsPaint(g, At<PolledParts>(ctx.r3.u32), BodyPaintArguments(ctx));
}

extern "C" REX_FUNC(sub_820E3E18) {  // SceneInterior::ApplyPaint
  SBX_LOAD_POLL_FALLBACK(sub_820E3E18)
  ApplyInteriorPaint(g, At<PolledInterior>(ctx.r3.u32), BodyPaintArguments(ctx));
}

extern "C" REX_FUNC(sub_820DBDA8) {  // SceneCockpit::ApplyPaint
  SBX_LOAD_POLL_FALLBACK(sub_820DBDA8)
  ApplyCockpitPaint(g, At<PolledCockpit>(ctx.r3.u32), BodyPaintArguments(ctx));
}

extern "C" REX_FUNC(sub_820E5DC0) {  // SceneTraffic::ApplyPaint
  SBX_LOAD_POLL_FALLBACK(sub_820E5DC0)
  PolledTraffic* traffic = At<PolledTraffic>(ctx.r3.u32);
  const PartPaint paint = BodyPaintArguments(ctx);
  ctx.r3.u64 = traffic->body.address;  // As the original leaves it when nothing is painted.
  ApplyTrafficPaint(g, traffic, paint);
}

extern "C" REX_FUNC(sub_820D7AC8) {  // TrafficCar::SetPaint
  SBX_LOAD_POLL_FALLBACK(sub_820D7AC8)
  SetTrafficPaint(g, At<TrafficCarObject>(ctx.r3.u32), BodyPaintArguments(ctx));
}

// SceneWheels::ApplyPaint and Wheels::ApplyPaint: r3 the wheels (the
// second set's or the first's), six paints in r4 to r10 and the caller's
// stack.
extern "C" REX_FUNC(sub_820E85E8) {
  SBX_LOAD_POLL_FALLBACK(sub_820E85E8)
  ApplyWheelsPaint(g, At<PolledWheels>(ctx.r3.u32)->wheels, WheelPaintArguments(ctx));
}

extern "C" REX_FUNC(sub_820D8EA8) {
  SBX_LOAD_POLL_FALLBACK(sub_820D8EA8)
  ApplyWheelsPaint(g, At<Wheels>(ctx.r3.u32), WheelPaintArguments(ctx));
}
