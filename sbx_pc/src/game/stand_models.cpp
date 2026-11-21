// The garage stand's car as our own C++ (--native_stand_models, on by
// default): the setters the tuning screens change it through (parts,
// interior, paint, decals, stickers, lights, wheels and tires, each then
// re-applied), the second wheels' steps and the wheel models' tire textures
// and grades, and the stage's scene model made.
//
// The re-applies (SceneModel::ApplyRecord, ApplyPaint and the others) are
// the scene models' and load poll's C++, called directly.

#include "game/stand_models.h"

#include "base/settings.h"
#include "game/car_spec.h"
#include "game/physics.h"
#include "game/race_setup.h"
#include "game/save_data.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>

SBX_SETTING_BOOL(native_stand_models, true, "Overrides", "Cars",
                 "Our own C++ for the garage stand's car (the tuning "
                 "screens' setters for parts, interior, paint, decals, stickers, lights, "
                 "wheels and tires, the second wheels' steps) instead of the recompiled "
                 "functions; the re-applies run as their own flags say");

namespace {

using game::CarData;
using game::CarSceneState;
using game::GarageStage;
using game::GarageStageHolder;
using game::GaugeChoice;
using game::InteriorRecord;
using game::PartsPoints;
using game::PolledInterior;
using game::PolledWheels;
using game::RecordLayer;
using game::SceneModel;
using game::SceneModelRecord;
using game::StandScreens;
using game::Vector;
using game::Wheels;
using game::WheelsRecord;
using guest::AddressOf;
using guest::At;
using guest::f32_be;
using guest::Guest;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

bool On() { return SBX_GET(native_stand_models); }

constexpr uint32_t kCarScene = 0x826B2B24;  // Holds the car scene (the second set).
constexpr uint32_t kStageModelPool = 7;

// Read-only image constants.
constexpr float kOne = 1.0f;            // 0x82000A20
constexpr float kLightsOnValue = 0.0f;  // 0x82000A24
constexpr float kFlipScale = -1.0f;     // 0x82001BD8

// The tuning screen, as far as the stand's steps read it: the save data
// whose garage car it shows, and the second wheels' step.
struct TuningScreen {
  uint8_t unknown_0[18904];
  ptr<game::SaveData> save;
  uint8_t unknown_18908[6336];
  i32_be second_wheels_step;  // 1: add the second wheels; 2: paint them.
};
GUEST_OFFSET(TuningScreen, save, 18904);
GUEST_OFFSET(TuningScreen, second_wheels_step, 25244);

GarageStageHolder* CurrentHolder() {
  return At<StandScreens>(*At<u32_be>(game::kStandScreens))->holder;
}

// The stage's scene model: ScenePools::GetB on the car scene's pool 7.
SceneModel* StageModel(Guest& g, GarageStageHolder* holder) {
  const uint32_t scene = *At<u32_be>(kCarScene);
  return At<SceneModel>(g.Call(sub_820C59D8, scene, kStageModelPool,
                               uint32_t(holder->stage->model_index)));
}

// SceneModel::ApplyRecord: the model rebuilt from its record (0 while it's
// still loading).
uint32_t ApplyRecord(Guest& g, SceneModel* model) { return game::SceneModelApplyRecord(g, model); }

InteriorRecord& Interior(SceneModel* model) { return model->record.interior; }

// The interior record's gauge choice `choice`. Not checked, as the original.
GaugeChoice& Choice(SceneModel* model, uint32_t choice) { return Interior(model).Gauge(choice); }

// The car record's layer `index`: the first six are the decals, then eight
// groups of six stickers.
RecordLayer& Layer(SceneModel* model, uint32_t index) {
  // Not checked, as the original (the address wraps in guest memory).
  return *guest::Element<RecordLayer>(AddressOf(model->record.car.layers), index);
}

// --- The stage's model ---

// GarageStage::MakeModel's 4,384-byte frame: the record it makes the model
// from, at the original's +80 (in guest memory: the model's set-up copies
// it with memcpy).
struct MakeModelFrame {
  uint8_t unknown_0[80];
  SceneModelRecord record;
  uint8_t unknown_4324[60];
};
GUEST_OFFSET(MakeModelFrame, record, 80);
GUEST_SIZE(MakeModelFrame, 4384);

// GarageStage::MakeModel: unless the stage has its model, a fresh record
// (its block `block`) made into a scene model in pool 7, its draw word set.
//
// The record's constructor and reset leave a few bytes unwritten; of those,
// SceneModelRecord::Copy carries only the cockpit record's interior variant
// (+20) into the model. The original left whatever the stack held there.
// Nothing reads it before it's set: the model's cockpit record is read only
// by SceneCockpit::SetUp (then Cockpit::BindTextures), which only
// SceneModel::Build and ApplyRecord reach, each after filling the record in
// (the variant from the car record); the tuning screen's backup copy of
// the record never reads it. The record is cleared first, so those bytes
// are 0.
void MakeModel(Guest& g, GarageStageHolder* holder, uint32_t block, uint32_t draw_word) {
  const uint32_t scene = *At<u32_be>(kCarScene);
  if (holder->stage->has_model != 0) {
    return;
  }
  guest::StackFrame frame(g.ctx, uint32_t(sizeof(MakeModelFrame)));
  SceneModelRecord* record = &At<MakeModelFrame>(frame.Local(0))->record;
  *record = SceneModelRecord{};
  game::SceneModelRecordInit(*record);
  game::SceneModelRecordReset(*record);
  record->car.unknown_0 = int32_t(block);
  holder->stage->model_index = g.Call(sub_820C5868, scene, kStageModelPool, 0u);  // ScenePools::ClaimB
  SceneModel* model = At<SceneModel>(game::ScenePoolsSetUpB(
      g, At<CarSceneState>(scene), kStageModelPool, holder->stage->model_index, AddressOf(record)));
  model->unknown_4296 = draw_word;
  holder->stage->draw_word = draw_word;
  holder->stage->has_model = 1;
  At<StandScreens>(*At<u32_be>(game::kStandScreens))->unknown_20 = 1;
}

// --- The parts and the interior (each then re-applied) ---

// GarageStage::SetPart: the parts record's word for a part slot (0 to 15;
// slots 6 and 7, and 8 and 9, share a word; 13 has none).
constexpr int kPartWord[16] = {0, 1, 2, 3, 4, 5, 6, 6, 7, 7, 8, 9, 10, -1, 11, 12};

uint32_t SetPart(Guest& g, GarageStageHolder* holder, uint32_t slot, uint32_t word) {
  SceneModel* model = StageModel(g, holder);
  if (slot < 16 && kPartWord[slot] >= 0) {
    model->record.parts.unknown_0[kPartWord[slot]] = word;
  }
  return ApplyRecord(g, model);
}

// GarageStage::SetParts: six of the parts record's words, and the aero
// set's word cleared.
uint32_t SetParts(Guest& g, GarageStageHolder* holder, const uint32_t (&words)[6]) {
  SceneModel* model = StageModel(g, holder);
  u32_be* parts = model->record.parts.unknown_0;
  parts[0] = words[0];
  parts[2] = words[1];
  parts[3] = words[2];
  parts[10] = words[3];
  parts[5] = words[4];
  parts[4] = words[5];
  parts[6] = 0;
  return ApplyRecord(g, model);
}

// GarageStage::SetInteriorClass: the interior's class from a value: 0 below
// 3, 1 below 7, else 2. Not re-applied.
SceneModel* SetInteriorClass(Guest& g, GarageStageHolder* holder, int32_t value) {
  SceneModel* model = StageModel(g, holder);
  Interior(model).unknown_0[1] = value >= 7 ? 2 : value >= 3 ? 1 : 0;
  return model;
}

// GarageStage::SetInteriorWord: one of three interior words by a kind (1,
// 4 or 5; others none).
uint32_t SetInteriorWord(Guest& g, GarageStageHolder* holder, int32_t kind, uint32_t word) {
  SceneModel* model = StageModel(g, holder);
  InteriorRecord& interior = Interior(model);
  if (kind == 1) {
    interior.unknown_104[0] = word;
  } else if (kind == 4) {
    interior.unknown_0[24] = word;
  } else if (kind == 5) {
    interior.unknown_0[23] = word;
  }
  return ApplyRecord(g, model);
}

// GarageStage::SetInteriorLevel: two interior words from a level: 0 below
// 3, 1 below 6, else 2; and 0 below 1, else 1.
uint32_t SetInteriorLevel(Guest& g, GarageStageHolder* holder, int32_t level) {
  SceneModel* model = StageModel(g, holder);
  InteriorRecord& interior = Interior(model);
  interior.unknown_104[0] = level < 3 ? 0 : level < 6 ? 1 : 2;
  interior.unknown_104[1] = level < 1 ? 0 : 1;
  return ApplyRecord(g, model);
}

// GarageStage::SetInteriorPair: the interior's part index and the word
// before it.
uint32_t SetInteriorPair(Guest& g, GarageStageHolder* holder, uint32_t index, uint32_t word) {
  SceneModel* model = StageModel(g, holder);
  Interior(model).unknown_100 = int32_t(index);
  Interior(model).unknown_0[24] = word;
  return ApplyRecord(g, model);
}

// GarageStage::SetChoice and ClearChoice: one of the interior's gauge
// choices on (with its image and texture) or off.
uint32_t SetChoice(Guest& g, GarageStageHolder* holder, uint32_t choice, uint32_t image,
                   uint32_t texture) {
  SceneModel* model = StageModel(g, holder);
  GaugeChoice& entry = Choice(model, choice);
  entry.enabled = 1;
  entry.image = image;
  entry.texture = int32_t(texture);
  return ApplyRecord(g, model);
}

uint32_t ClearChoice(Guest& g, GarageStageHolder* holder, uint32_t choice) {
  SceneModel* model = StageModel(g, holder);
  Choice(model, choice).enabled = 0;
  return ApplyRecord(g, model);
}

// GarageStage::SetAero and SetAero2: a word of the car record's tail.
uint32_t SetCarWord(Guest& g, GarageStageHolder* holder, int index, uint32_t word) {
  SceneModel* model = StageModel(g, holder);
  model->record.car.unknown_3676[index] = word;
  return ApplyRecord(g, model);
}

// SceneInterior::SetRecord: the record copied into the interior and into
// the first set's interior. Returns the second memcpy's answer.
uint32_t SceneInteriorSetRecord(Guest& g, PolledInterior* interior, const InteriorRecord* record) {
  const uint32_t bytes = uint32_t(sizeof(InteriorRecord));
  g.Call(sub_8258CB90, &interior->record, record, bytes);  // memcpy
  return g.Call(sub_8258CB90, &interior->interior->record, record, bytes);  // memcpy
}

// SceneModel::ApplyInterior: the model's interior record given to its
// interior.
uint32_t ApplyInterior(Guest& g, SceneModel* model) {
  return SceneInteriorSetRecord(g, model->interior, &model->record.interior);
}

// GarageStage::SetAccessory: the interior's accessory (3 none) and the word
// after it, then the interior re-applied.
uint32_t SetAccessory(Guest& g, GarageStageHolder* holder, int32_t accessory, uint32_t word) {
  SceneModel* model = StageModel(g, holder);
  Interior(model).accessory = accessory;
  Interior(model).unknown_116 = word;
  return ApplyInterior(g, model);
}

// --- Paint, decals, stickers, lights ---

// GarageStage::SetColor0 to SetColor5: one of the car record's three paint
// colors (pieces 0 to 2, and again 3 to 5), then the body's paint applied
// (0 to 2) or the wheels' (3 to 5: those read the wheels' own colors, so
// the color set shows only at the next body paint; the game never passes
// 3 to 5).
void SetColor(Guest& g, GarageStageHolder* holder, uint32_t piece, float h, float s, float v) {
  SceneModel* model = StageModel(g, holder);
  Vector& color = model->record.car.unknown_16[piece % 3];
  color.x = h;
  color.y = s;
  color.z = v;
  color.w = kOne;
  if (piece < 3) {
    game::SceneModelApplyPaint(g, model);
  } else {
    game::SceneModelApplyWheelPaint(g, model);
  }
}

// GarageStage::SetDecalA and SetDecalB: one of three decals (entry 0 to 2
// is layer 2, 1, 0 for A; 5, 4, 3 for B): its two words, position, angle and
// size (x mirrored when `flip` is set). Not re-applied.
SceneModel* SetDecal(Guest& g, GarageStageHolder* holder, uint32_t first_layer, uint32_t entry,
                     uint32_t word, uint32_t flip, float x, float y, float angle, float size) {
  SceneModel* model = StageModel(g, holder);
  if (entry > 2) {
    return model;  // The original reads past its three-entry table.
  }
  RecordLayer& layer = Layer(model, first_layer + 2 - entry);
  layer.unknown_16.x = x;
  layer.unknown_0 = int32_t(word);
  layer.unknown_16.y = y;
  layer.unknown_4 = flip;
  layer.unknown_32 = angle;
  layer.unknown_48.y = size;
  layer.unknown_48.x = flip != 0 ? size * kFlipScale : size;
  return model;
}

// GarageStage::SetSticker: sticker `slot` of area 0 to 6 (any other area
// the last group): its layer's kind (the area's), id, flag, position, angle
// and size. Not re-applied (its caller reloads the decals after its five).
constexpr uint32_t kStickerKinds[7] = {8, 2, 1, 3, 4, 7, 6};
constexpr uint32_t kLastStickerKind = 5;

struct Sticker {
  int32_t area;
  uint32_t slot;
  uint32_t id;
  uint32_t flag;
  Vector position;
  float angle;
  Vector size;
};

SceneModel* SetSticker(Guest& g, GarageStageHolder* holder, const Sticker& sticker) {
  SceneModel* model = StageModel(g, holder);
  const bool known = sticker.area >= 0 && sticker.area <= 6;
  const uint32_t group = known ? uint32_t(sticker.area) + 1 : 8;
  RecordLayer& layer = Layer(model, 6 * group + sticker.slot);
  layer.kind = known ? kStickerKinds[sticker.area] : kLastStickerKind;
  layer.unknown_0 = int32_t(sticker.id);
  layer.unknown_4 = sticker.flag;
  layer.unknown_16 = sticker.position;
  layer.unknown_32 = sticker.angle;
  layer.unknown_48 = sticker.size;
  return model;
}

// GarageStage::SetDecalWord: the car record's last tail word, then the
// decals reloaded (the model fetched again).
void SetDecalWord(Guest& g, GarageStageHolder* holder, uint32_t word) {
  StageModel(g, holder)->record.car.unknown_3676[3] = word;
  game::SceneModelReloadDecals(g, StageModel(g, holder));
}

// SceneParts::LightValue: the value kept after the headlights' points.
float LightValue(const PartsPoints& points) {
  return reinterpret_cast<const f32_be&>(points.unknown_27664[0]);
}

// GarageStage::SetLightsOn: on, the model's light value 0.0; off, the
// parts' own.
SceneModel* SetLightsOn(Guest& g, GarageStageHolder* holder, uint32_t on) {
  SceneModel* model = StageModel(g, holder);
  if (on != 0) {
    model->lights_on = 1;
    model->light_value = kLightsOnValue;
  } else {
    model->lights_on = 0;
    model->light_value = LightValue(model->parts->points);
  }
  return model;
}

// GarageStage::ClearLightKit: a light kit group's texture word cleared (0
// to 2, or 3 for all three), then the light kit applied.
uint32_t ClearLightKit(Guest& g, GarageStageHolder* holder, int32_t group) {
  SceneModel* model = StageModel(g, holder);
  auto& colors = model->record.pool4.colors;
  if (group >= 0 && group <= 2) {
    colors[group].unknown_0 = 0;
  } else if (group == 3) {
    colors[1].unknown_0 = 0;
    colors[2].unknown_0 = 0;
    colors[0].unknown_0 = 0;
  }
  return game::SceneModelApplyLightKit(g, model);
}

// --- The wheels ---

// Wheels::SetRecord: the wheel models' record set from a scene wheels'
// (memcpy's answer).
uint32_t WheelsSetRecord(Guest& g, PolledWheels* wheels) {
  return game::WheelsSetRecord(g, wheels->wheels, AddressOf(&wheels->record));
}

// Wheels::SetTireTextures: each tire's texture set from its grade, on every
// pass (PartModel::SetTexture, r5 -1).
uint32_t WheelsSetTireTextures(Guest& g, Wheels* models) {
  g.Call(sub_820D1420, &models->loads.parts_48[0], models->record.unknown_4[2], -1);
  return g.Call(sub_820D1420, &models->loads.parts_48[1], models->record.unknown_28, -1);
}

// SceneWheels::SetTireGrades: the front and rear tire grades, the models'
// record set from them, then their textures.
uint32_t SceneWheelsSetTireGrades(Guest& g, PolledWheels* wheels, uint32_t front,
                                  uint32_t rear) {
  wheels->record.unknown_4[2] = front;
  wheels->record.unknown_28 = rear;
  WheelsSetRecord(g, wheels);
  return WheelsSetTireTextures(g, wheels->wheels);
}

// GarageStage::SetTireGrade: the front (1), rear (2) or both (0) tires'
// grade, then the wheels' grades set from the record.
uint32_t SetTireGrade(Guest& g, GarageStageHolder* holder, int32_t which, uint32_t grade) {
  SceneModel* model = StageModel(g, holder);
  WheelsRecord& record = model->record.wheels;
  if (which == 0 || which == 1) {
    record.unknown_4[2] = grade;
  }
  if (which == 0 || which == 2) {
    record.unknown_28 = grade;
  }
  return SceneWheelsSetTireGrades(g, model->wheels, record.unknown_4[2], record.unknown_28);
}

// TuningScreen::SetTireGrade: a tire choice (0 to 4) as a grade, for the
// front (1), rear (2) or both (0).
constexpr uint32_t kTireGrades[5] = {0, 1, 1, 2, 2};

void TuningScreenSetTireGrade(Guest& g, int32_t which, uint32_t choice) {
  if (which != 0 && which != 1 && which != 2) {
    return;
  }
  if (choice >= 5) {
    return;  // The original reads past its five-entry table.
  }
  SetTireGrade(g, CurrentHolder(), which, kTireGrades[choice]);
}

// SceneWheels::SetWord68 and SetCaliperTexture: a wheels record word, the
// models' record set from it (and for the calipers, their texture).
uint32_t SceneWheelsSetWord68(Guest& g, PolledWheels* wheels, uint32_t word) {
  wheels->record.unknown_68[0] = word;
  return WheelsSetRecord(g, wheels);
}

uint32_t SceneWheelsSetCaliperTexture(Guest& g, PolledWheels* wheels, uint32_t texture) {
  wheels->record.caliper_texture = texture;
  WheelsSetRecord(g, wheels);
  return game::WheelsSetCaliperTexture(g, wheels->wheels);
}

// GarageStage::SetWheelExtras: the record's word and caliper texture, each
// given to the wheels.
uint32_t SetWheelExtras(Guest& g, GarageStageHolder* holder, uint32_t word, uint32_t texture) {
  SceneModel* model = StageModel(g, holder);
  model->record.wheels.unknown_68[0] = word;
  model->record.wheels.caliper_texture = texture;
  SceneWheelsSetWord68(g, model->wheels, word);
  return SceneWheelsSetCaliperTexture(g, model->wheels, model->record.wheels.caliper_texture);
}

// SceneWheels::SetSizes: two words and the front and rear sizes, then the
// models' record set from them.
void SceneWheelsSetSizes(Guest& g, PolledWheels* wheels, uint32_t a, uint32_t b,
                         const game::Record12& front, const game::Record12& rear) {
  wheels->record.unknown_68[2] = b;
  wheels->record.unknown_68[1] = a;
  wheels->record.unknown_44 = front;
  wheels->record.unknown_56 = rear;
  WheelsSetRecord(g, wheels);
}

// GarageStage::FitAxles: a car's vehicle set up into a car data (on the
// original's 1,968-byte frame), its two axles' positions kept by the stage.
void FitAxles(Guest& g, GarageStageHolder* holder, uint32_t car) {
  guest::StackFrame frame(g.ctx, 1968);
  CarData* data = At<CarData>(frame.Local(80));
  g.Call(sub_820AEE68, data);       // CarData::CarData
  g.Call(sub_820CD368, car, data);  // Car::SetupVehicle
  GarageStage* stage = holder->stage;
  Vector& front = stage->axle_positions[0];
  front.x = data->axles[0].values[0];
  front.y = data->axles[0].values[1];
  front.z = data->axles[0].values[2];
  front.w = kOne;
  Vector& rear = stage->axle_positions[1];
  rear.x = data->axles[1].values[0];
  rear.y = data->axles[1].values[1];
  rear.z = data->axles[1].values[2];
  rear.w = kOne;
}

// --- The selected garage car ---

// A save's garage: the list of its cars' slots, the cars (the head of each
// is a Car), and which car in the list is selected.
struct GarageCarEntry {
  uint8_t car[1880];  // A Car's first 1,880 bytes.
  uint8_t unknown_1880[84];
};
GUEST_SIZE(GarageCarEntry, 1964);

struct SaveGarage {
  game::SaveListIndex list;
  uint8_t unknown_210[2];
  GarageCarEntry cars[50];
  u32_be unknown_98412;
  u32_be selected;  // How many steps down the list the selected car is.
};
GUEST_OFFSET(SaveGarage, cars, 212);
GUEST_OFFSET(SaveGarage, selected, 104536 - 6120);

SaveGarage& GarageOf(game::SaveData* save) {
  return *reinterpret_cast<SaveGarage*>(&save->garage);
}

// The selected car: the list walked from its head.
GarageCarEntry& SelectedGarageCar(SaveGarage& garage) {
  using Node = game::SaveListIndex::Node;
  const uint32_t nodes = AddressOf(garage.list.nodes);
  const Node* node = guest::Element<Node>(nodes, uint32_t(int32_t(garage.list.head)));
  for (uint32_t steps = garage.selected; steps != 0; steps--) {
    node = guest::Element<Node>(nodes, uint32_t(int32_t(node->next)));
  }
  return *guest::Element<GarageCarEntry>(AddressOf(garage.cars), uint32_t(int32_t(node->slot)));
}

uint32_t CarIdOf(GarageCarEntry& entry) { return reinterpret_cast<game::Car*>(entry.car)->car_id; }

// --- The aero set ---

// The aero sets' table in the image: two 24-byte entries for each model
// block, each six parts record words (-1 to keep the part).
struct AeroSetEntry {
  i32_be words[6];
};
GUEST_SIZE(AeroSetEntry, 24);
constexpr uint32_t kAeroSets = 0x82009990;
constexpr uint32_t kAeroSetFlag = 8;  // In the body kit's word: the set is kept.

// GarageStage::SetAeroSet: the parts of an aero set (`set` 0 on) for the
// selected garage car: its body kit (AeroParts::Get82 of level set + 1,
// on the original's 224-byte frame), then the set's words into the parts
// record (the fifth and sixth from `fifth` and `sixth` where the set has
// none), the kit's level kept when its flag says so. Then re-applied.
uint32_t SetAeroSet(Guest& g, GarageStageHolder* holder, uint32_t set, uint32_t fifth, uint32_t sixth) {
  SceneModel* model = StageModel(g, holder);
  auto* save = At<game::SaveData>(g.Call(sub_82105790, 0u));  // SaveData::Of
  const uint32_t car_id = CarIdOf(SelectedGarageCar(GarageOf(save)));
  const uint32_t level = set + 1;
  uint32_t kit_flags = 0;
  {
    guest::StackFrame frame(g.ctx, 224);
    const uint32_t parts = frame.Local(80), copy = frame.Local(128);
    const uint32_t found = g.Call(sub_820C9BA0, parts, car_id);  // Params::GetCarParts
    g.Call(sub_8258CB90, copy, found, 36u);                       // memcpy
    const uint32_t kit = g.Call(sub_820CBA30, parts, copy, level);  // AeroParts::Get82
    kit_flags = At<game::AeroPart>(kit)->unknown_24;
  }
  const uint32_t index = 2 * uint32_t(int32_t(model->record.car.unknown_0)) + set;
  const AeroSetEntry& entry = *guest::Element<AeroSetEntry>(kAeroSets, index);
  u32_be* words = model->record.parts.unknown_0;
  constexpr int kKept[4] = {0, 2, 3, 10};  // The parts record words of the first four.
  for (int i = 0; i < 4; i++) {
    if (entry.words[i] != -1) {
      words[kKept[i]] = uint32_t(int32_t(entry.words[i]));
    }
  }
  words[5] = entry.words[4] != -1 ? uint32_t(int32_t(entry.words[4])) : fifth;
  words[4] = entry.words[5] != -1 ? uint32_t(int32_t(entry.words[5])) : sixth;
  words[6] = (kit_flags & kAeroSetFlag) != 0 ? level : 0;
  return ApplyRecord(g, model);
}

// --- The wheel fitting ---

constexpr float kByteScale = 1.0f / 255.0f;  // 0x820021C0
constexpr float kWidthScale = 1000.0f;       // 0x82001BD4
constexpr float kHundred = 100.0f;           // 0x82002A10
constexpr int32_t kLastWheel = 178;

// Each wheel number's model (-1 for 0, the stock wheel): the third word of
// GarageStage::SetWheel's 179-entry table, which it builds on its frame.
constexpr int8_t kWheelModels[kLastWheel + 1] = {
    -1, 0,  0,  0,  0,  1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  4,  4,  4,  4,  5,  5,  5,
    5,  5,  5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  8,  8,  9,  9,  10, 11, 12, 12, 13,
    14, 15, 16, 17, 17, 17, 18, 18, 19, 19, 19, 19, 20, 20, 20, 21, 21, 22, 22, 23, 23, 23, 24,
    24, 24, 24, 25, 25, 26, 26, 26, 26, 27, 27, 27, 27, 27, 28, 28, 28, 29, 29, 29, 29, 30, 30,
    30, 30, 31, 32, 32, 32, 32, 33, 33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38,
    38, 39, 39, 39, 40, 40, 40, 40, 41, 42, 43, 44, 44, 44, 44, 44, 44, 45, 45, 45, 46, 46, 46,
    47, 47, 47, 47, 48, 48, 49, 49, 49, 50, 50, 50, 50, 51, 51, 52, 52, 52, 52, 53, 53, 54, 54,
    54, 55, 56, 56, 57, 58, 59, 59, 60, 61, 61, 61, 62, 62, 62, 63, 63, 63,
};

// fctiwz: toward zero, held at the ends; a NaN gives the smallest.
int32_t TruncateToInt(float value) {
  if (std::isnan(value) || value < -2147483648.0f) {
    return INT32_MIN;
  }
  if (value >= 2147483648.0f) {
    return INT32_MAX;
  }
  return int32_t(value);
}

float FloatBits(uint32_t bits) { return std::bit_cast<float>(bits); }

// A wheel color as the record keeps it: the hue as is, saturation and value
// from bytes to 0 to 1.
void SetWheelColor(Vector& color, uint16_t hue, uint8_t saturation, uint8_t value) {
  color.x = float(hue);
  color.y = float(saturation) * kByteScale;
  color.z = float(value) * kByteScale;
}

// One side's wheel into the record: its model, three colors and two
// values (`colors` the side's three color vectors, `record` its Record12).
void SetSideWheel(const game::SpecWheel& wheel, int32_t model, Vector* colors,
                  game::Record12& record) {
  record.unknown_0 = model;
  SetWheelColor(colors[0], wheel.colors[0].unknown_0, wheel.colors[0].unknown_2[0],
                wheel.colors[0].unknown_2[1]);
  SetWheelColor(colors[1], wheel.colors[1].unknown_0, wheel.colors[1].unknown_2[0],
                wheel.colors[1].unknown_2[1]);
  SetWheelColor(colors[2], wheel.colors[1].unknown_4, wheel.colors[1].unknown_6[0],
                wheel.colors[1].unknown_6[1]);
  record.unknown_8 = FloatBits(wheel.colors[0].unknown_8);
  record.unknown_4 = FloatBits(wheel.colors[1].unknown_8);
}

// One side's tire size from the car's tire: its rim (whole), width (mm)
// and profile (percent), probably.
void SetSideTire(const game::Tire& tire, game::Record12& size) {
  size.unknown_0 = TruncateToInt(float(tire.rim_diameter));
  size.unknown_4 = float(tire.width) * kWidthScale;
  size.unknown_8 = float(tire.aspect_ratio) * kHundred;
}

// GarageStage::SetWheel: wheel number `number` (0 the car spec's stock
// wheel; above 178 the last) on the front (1), rear (anything but 0 or 1)
// or both (0), with the tires of `car`: the record's wheel models, colors
// and sizes. Then the axles refitted to the car and the second set's wheels
// given the sizes. A negative number (never passed) runs the original,
// which reads below its table.
void SetWheel(Guest& g, GarageStageHolder* holder, int32_t number, int32_t which, game::Car* car) {
  if (number < 0) {
    g.Call(__imp__sub_8217BC20, holder, number, which, car);
    return;
  }
  g.ctx.fpscr.disableFlushMode();  // Float math as the recompiled code's.
  SceneModel* model = StageModel(g, holder);
  auto* save = At<game::SaveData>(g.Call(sub_82105790, 0u));  // SaveData::Of
  const uint32_t car_id = CarIdOf(SelectedGarageCar(GarageOf(save)));
  auto* spec = At<game::CarSpec>(g.Call(sub_820C9C38, car_id));  // Params::GetCarSpec
  number = std::min(number, kLastWheel);
  const game::SpecWheel* wheel = &spec->stock_wheel;
  if (number != 0) {
    wheel = At<game::SpecWheel>(g.Call(sub_820C9C50, number));  // Params::GetWheel
  }
  const int32_t wheel_model = kWheelModels[number];
  WheelsRecord& wheels = model->record.wheels;
  Vector* colors = model->record.car.unknown_16;
  const bool front = which == 0 || which == 1;
  const bool rear = which != 1;
  if (front) {
    SetSideWheel(*wheel, wheel_model, &colors[3], wheels.unknown_16);
    SetSideTire(car->setup.front_tire, wheels.unknown_44);
  }
  if (rear) {
    SetSideWheel(*wheel, wheel_model, &colors[6], wheels.unknown_32);
    SetSideTire(car->setup.rear_tire, wheels.unknown_56);
  }
  FitAxles(g, holder, AddressOf(car));
  SceneWheelsSetSizes(g, model->wheels, wheels.unknown_68[1], wheels.unknown_68[2],
                      wheels.unknown_44, wheels.unknown_56);
}

// TuningScreen::PreviewWheel: the selected garage car copied into a car (on
// the original's 4,016-byte frame) and refreshed, then wheel `number` set
// on the stand with its tires.
void PreviewWheel(Guest& g, TuningScreen* screen, int32_t number, int32_t which) {
  GarageCarEntry& entry = SelectedGarageCar(GarageOf(screen->save));
  guest::StackFrame frame(g.ctx, 4016);
  game::Car* car = At<game::Car>(frame.Local(80));
  g.Call(sub_820CD378, car);                                       // Car::Init
  g.Call(sub_8258CB90, car, entry.car, uint32_t(sizeof(entry.car)));  // memcpy
  car->unknown_3888 |= 1;
  g.Call(sub_820C47A8, car);  // Car::Refresh
  SetWheel(g, CurrentHolder(), number, which, car);
}

// --- The second wheels ---

uint32_t SecondWheelsPending(Guest& g, GarageStageHolder* holder) {
  return StageModel(g, holder)->second_wheels_added;
}

// TuningScreen::StepSecondWheels: once they're not pending, the second
// wheels added (step 1, then 2) and painted (step 2, then 0).
void StepSecondWheels(Guest& g, TuningScreen* screen) {
  if (SecondWheelsPending(g, CurrentHolder()) == 0 && screen->second_wheels_step == 1) {
    game::SceneModelAddSecondWheels(g, StageModel(g, CurrentHolder()));
    screen->second_wheels_step = 2;
    return;
  }
  if (SecondWheelsPending(g, CurrentHolder()) == 0 && screen->second_wheels_step == 2) {
    game::SceneModelApplyWheelPaint(g, StageModel(g, CurrentHolder()));
    screen->second_wheels_step = 0;
  }
}

// A 12-byte wheel size passed in two registers: the first's two words and
// the second's high word, kept bit for bit.
game::Record12 SizeOf(uint64_t first, uint64_t second) {
  game::Record12 size;
  size.unknown_0 = int32_t(uint32_t(first >> 32));
  size.unknown_4 = std::bit_cast<float>(uint32_t(first));
  size.unknown_8 = std::bit_cast<float>(uint32_t(second >> 32));
  return size;
}

// The four 64-bit words of two 16-byte values passed in registers, as a
// vector: the high word first.
Vector VectorOf(uint64_t first, uint64_t second) {
  Vector v;
  v.x = std::bit_cast<float>(uint32_t(first >> 32));
  v.y = std::bit_cast<float>(uint32_t(first));
  v.z = std::bit_cast<float>(uint32_t(second >> 32));
  v.w = std::bit_cast<float>(uint32_t(second));
  return v;
}

}  // namespace

#define SBX_STAND_MODELS_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                  \
  if (!On()) {                          \
    __imp__##name(ctx, base);           \
    return;                             \
  }                                     \
  Guest g(ctx, base);

namespace {

GarageStageHolder* HolderArg(PPCContext& ctx) { return At<GarageStageHolder>(ctx.r3.u32); }

}  // namespace

// GarageStage::MakeModel: r3 the holder, r4 the model's block, r5 its draw
// word.
extern "C" REX_FUNC(sub_8217DB98) {
  SBX_STAND_MODELS_FALLBACK(sub_8217DB98)
  MakeModel(g, HolderArg(ctx), ctx.r4.u32, ctx.r5.u32);
}

// GarageStage::SetPart: r4 the slot, r5 the word.
extern "C" REX_FUNC(sub_82176F50) {
  SBX_STAND_MODELS_FALLBACK(sub_82176F50)
  ctx.r3.u64 = SetPart(g, HolderArg(ctx), ctx.r4.u32, ctx.r5.u32);
}

// GarageStage::SetParts: r4 to r9 the six words.
extern "C" REX_FUNC(sub_821777F8) {
  SBX_STAND_MODELS_FALLBACK(sub_821777F8)
  const uint32_t words[6] = {ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                             ctx.r7.u32, ctx.r8.u32, ctx.r9.u32};
  ctx.r3.u64 = SetParts(g, HolderArg(ctx), words);
}

// GarageStage::SetInteriorClass: r4 the value. Returns the model.
extern "C" REX_FUNC(sub_82177038) {
  SBX_STAND_MODELS_FALLBACK(sub_82177038)
  ctx.r3.u64 = AddressOf(SetInteriorClass(g, HolderArg(ctx), ctx.r4.s32));
}

// GarageStage::SetInteriorWord: r4 the kind, r5 the word.
extern "C" REX_FUNC(sub_821770A0) {
  SBX_STAND_MODELS_FALLBACK(sub_821770A0)
  ctx.r3.u64 = SetInteriorWord(g, HolderArg(ctx), ctx.r4.s32, ctx.r5.u32);
}

// GarageStage::SetInteriorLevel: r4 the level.
extern "C" REX_FUNC(sub_82177210) {
  SBX_STAND_MODELS_FALLBACK(sub_82177210)
  ctx.r3.u64 = SetInteriorLevel(g, HolderArg(ctx), ctx.r4.s32);
}

// GarageStage::SetInteriorPair: r4 the part index, r5 the word.
extern "C" REX_FUNC(sub_821772A0) {
  SBX_STAND_MODELS_FALLBACK(sub_821772A0)
  ctx.r3.u64 = SetInteriorPair(g, HolderArg(ctx), ctx.r4.u32, ctx.r5.u32);
}

// GarageStage::SetChoice: r4 the choice, r5 and r6 its words.
extern "C" REX_FUNC(sub_82177360) {
  SBX_STAND_MODELS_FALLBACK(sub_82177360)
  ctx.r3.u64 = SetChoice(g, HolderArg(ctx), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// GarageStage::ClearChoice: r4 the choice.
extern "C" REX_FUNC(sub_821773C8) {
  SBX_STAND_MODELS_FALLBACK(sub_821773C8)
  ctx.r3.u64 = ClearChoice(g, HolderArg(ctx), ctx.r4.u32);
}

// GarageStage::SetAero: r4 the word.
extern "C" REX_FUNC(sub_821774F8) {
  SBX_STAND_MODELS_FALLBACK(sub_821774F8)
  ctx.r3.u64 = SetCarWord(g, HolderArg(ctx), 1, ctx.r4.u32);
}

// GarageStage::SetAero2: r4 the word.
extern "C" REX_FUNC(sub_82177548) {
  SBX_STAND_MODELS_FALLBACK(sub_82177548)
  ctx.r3.u64 = SetCarWord(g, HolderArg(ctx), 2, ctx.r4.u32);
}

// GarageStage::SetWord5016: r4 the word. Returns the model.
extern "C" REX_FUNC(sub_821775E8) {
  SBX_STAND_MODELS_FALLBACK(sub_821775E8)
  const uint32_t word = ctx.r4.u32;
  SceneModel* model = StageModel(g, HolderArg(ctx));
  model->unknown_5016 = word;
  ctx.r3.u64 = AddressOf(model);
}

// GarageStage::SetWheelWord: r4 the word. Returns the model.
extern "C" REX_FUNC(sub_82177A68) {
  SBX_STAND_MODELS_FALLBACK(sub_82177A68)
  const uint32_t word = ctx.r4.u32;
  SceneModel* model = StageModel(g, HolderArg(ctx));
  model->record.wheels.unknown_4[1] = word;
  ctx.r3.u64 = AddressOf(model);
}

// SceneInterior::SetRecord: r3 the interior, r4 the record.
extern "C" REX_FUNC(sub_820E34A8) {
  SBX_STAND_MODELS_FALLBACK(sub_820E34A8)
  ctx.r3.u64 = SceneInteriorSetRecord(g, At<PolledInterior>(ctx.r3.u32),
                                      At<InteriorRecord>(ctx.r4.u32));
}

// SceneModel::ApplyInterior: r3 the scene model.
extern "C" REX_FUNC(sub_820D9C08) {
  SBX_STAND_MODELS_FALLBACK(sub_820D9C08)
  ctx.r3.u64 = ApplyInterior(g, At<SceneModel>(ctx.r3.u32));
}

// GarageStage::SetAccessory: r4 the accessory, r5 the word.
extern "C" REX_FUNC(sub_82177300) {
  SBX_STAND_MODELS_FALLBACK(sub_82177300)
  ctx.r3.u64 = SetAccessory(g, HolderArg(ctx), ctx.r4.s32, ctx.r5.u32);
}

// GarageStage::SetColor0 to SetColor5: f1 to f3 the color.
#define SBX_STAND_SET_COLOR(name, piece)                                                   \
  extern "C" REX_FUNC(name) {                                                              \
    SBX_STAND_MODELS_FALLBACK(name)                                                        \
    ctx.fpscr.disableFlushMode();                                                          \
    SetColor(g, HolderArg(ctx), piece, float(ctx.f1.f64), float(ctx.f2.f64), float(ctx.f3.f64)); \
  }

SBX_STAND_SET_COLOR(sub_821790C8, 0)
SBX_STAND_SET_COLOR(sub_82179158, 1)
SBX_STAND_SET_COLOR(sub_821791E8, 2)
SBX_STAND_SET_COLOR(sub_82179278, 3)
SBX_STAND_SET_COLOR(sub_82179308, 4)
SBX_STAND_SET_COLOR(sub_82179398, 5)

// GarageStage::SetDecalA and SetDecalB: r4 the entry, r5 and r6 the words,
// f1 to f4 the position, angle and size. Return the model.
extern "C" REX_FUNC(sub_82177868) {
  SBX_STAND_MODELS_FALLBACK(sub_82177868)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = AddressOf(SetDecal(g, HolderArg(ctx), 0, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                  float(ctx.f1.f64), float(ctx.f2.f64), float(ctx.f3.f64),
                                  float(ctx.f4.f64)));
}

extern "C" REX_FUNC(sub_82177928) {
  SBX_STAND_MODELS_FALLBACK(sub_82177928)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = AddressOf(SetDecal(g, HolderArg(ctx), 3, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                  float(ctx.f1.f64), float(ctx.f2.f64), float(ctx.f3.f64),
                                  float(ctx.f4.f64)));
}

namespace {

// The caller's frame as SetSticker reads it: the size is passed on the
// stack, in the caller's parameter area.
struct StickerStackArguments {
  uint8_t unknown_0[96];
  Vector size;
};
GUEST_OFFSET(StickerStackArguments, size, 96);

}  // namespace

// GarageStage::SetSticker: r4 the area, r5 the slot, r6 the id, r7 the
// flag, r8 and r9 the position, f1 the angle, the size (16 bytes) on the
// caller's stack at +96. Returns the model.
extern "C" REX_FUNC(sub_82179428) {
  SBX_STAND_MODELS_FALLBACK(sub_82179428)
  ctx.fpscr.disableFlushMode();
  Sticker sticker;
  sticker.area = ctx.r4.s32;
  sticker.slot = ctx.r5.u32;
  sticker.id = ctx.r6.u32;
  sticker.flag = ctx.r7.u32;
  sticker.position = VectorOf(ctx.r8.u64, ctx.r9.u64);
  sticker.angle = float(ctx.f1.f64);
  sticker.size = At<StickerStackArguments>(ctx.r1.u32)->size;
  ctx.r3.u64 = AddressOf(SetSticker(g, HolderArg(ctx), sticker));
}

// GarageStage::SetDecalWord: r4 the word.
extern "C" REX_FUNC(sub_82177420) {
  SBX_STAND_MODELS_FALLBACK(sub_82177420)
  SetDecalWord(g, HolderArg(ctx), ctx.r4.u32);
}

// GarageStage::ReloadDecals: the stage's model's decals reloaded.
extern "C" REX_FUNC(sub_82176F10) {
  SBX_STAND_MODELS_FALLBACK(sub_82176F10)
  game::SceneModelReloadDecals(g, StageModel(g, HolderArg(ctx)));
}

// GarageStage::SetLightsOn: r4 the flag. Returns the model.
extern "C" REX_FUNC(sub_82177470) {
  SBX_STAND_MODELS_FALLBACK(sub_82177470)
  ctx.r3.u64 = AddressOf(SetLightsOn(g, HolderArg(ctx), ctx.r4.u32));
}

// SceneParts::LightValue: r3 the parts' points; the value in f1.
extern "C" REX_FUNC(sub_820DEF68) {
  SBX_STAND_MODELS_FALLBACK(sub_820DEF68)
  ctx.fpscr.disableFlushMode();
  ctx.f1.f64 = double(LightValue(*At<PartsPoints>(ctx.r3.u32)));
}

// GarageStage::ClearLightKit: r4 the group.
extern "C" REX_FUNC(sub_82177630) {
  SBX_STAND_MODELS_FALLBACK(sub_82177630)
  ctx.r3.u64 = ClearLightKit(g, HolderArg(ctx), ctx.r4.s32);
}

// Wheels::SetTireTextures: r3 the wheel models.
extern "C" REX_FUNC(sub_820D8BE0) {
  SBX_STAND_MODELS_FALLBACK(sub_820D8BE0)
  ctx.r3.u64 = WheelsSetTireTextures(g, At<Wheels>(ctx.r3.u32));
}

// SceneWheels::SetTireGrades: r3 the scene's wheels, r4 and r5 the front
// and rear grades.
extern "C" REX_FUNC(sub_820E7058) {
  SBX_STAND_MODELS_FALLBACK(sub_820E7058)
  ctx.r3.u64 = SceneWheelsSetTireGrades(g, At<PolledWheels>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// GarageStage::SetTireGrade: r4 which tires, r5 the grade.
extern "C" REX_FUNC(sub_82177180) {
  SBX_STAND_MODELS_FALLBACK(sub_82177180)
  ctx.r3.u64 = SetTireGrade(g, HolderArg(ctx), ctx.r4.s32, ctx.r5.u32);
}

// TuningScreen::SetTireGrade: r4 which tires, r5 the tire choice (r3 not
// read).
extern "C" REX_FUNC(sub_821CAF70) {
  SBX_STAND_MODELS_FALLBACK(sub_821CAF70)
  TuningScreenSetTireGrade(g, ctx.r4.s32, ctx.r5.u32);
}

// SceneWheels::SetWord68: r4 the word.
extern "C" REX_FUNC(sub_820E6FF8) {
  SBX_STAND_MODELS_FALLBACK(sub_820E6FF8)
  ctx.r3.u64 = SceneWheelsSetWord68(g, At<PolledWheels>(ctx.r3.u32), ctx.r4.u32);
}

// SceneWheels::SetCaliperTexture: r4 the texture.
extern "C" REX_FUNC(sub_820E7010) {
  SBX_STAND_MODELS_FALLBACK(sub_820E7010)
  ctx.r3.u64 = SceneWheelsSetCaliperTexture(g, At<PolledWheels>(ctx.r3.u32), ctx.r4.u32);
}

// GarageStage::SetWheelExtras: r4 the word, r5 the caliper texture.
extern "C" REX_FUNC(sub_82177120) {
  SBX_STAND_MODELS_FALLBACK(sub_82177120)
  ctx.r3.u64 = SetWheelExtras(g, HolderArg(ctx), ctx.r4.u32, ctx.r5.u32);
}

// SceneWheels::SetSizes: r4 and r5 two words; r6, r7 the front size and r8,
// r9 the rear (each 12 bytes: the first register's two words and the
// second's high word).
extern "C" REX_FUNC(sub_820E6F68) {
  SBX_STAND_MODELS_FALLBACK(sub_820E6F68)
  const game::Record12 front = SizeOf(ctx.r6.u64, ctx.r7.u64);
  const game::Record12 rear = SizeOf(ctx.r8.u64, ctx.r9.u64);
  SceneWheelsSetSizes(g, At<PolledWheels>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, front, rear);
}

// GarageStage::FitAxles: r4 the car.
extern "C" REX_FUNC(sub_82176BE8) {
  SBX_STAND_MODELS_FALLBACK(sub_82176BE8)
  FitAxles(g, HolderArg(ctx), ctx.r4.u32);
}

// GarageStage::SecondWheelsPending: whether the second wheels are still
// to be added (the stage's model's flag).
extern "C" REX_FUNC(sub_82177C98) {
  SBX_STAND_MODELS_FALLBACK(sub_82177C98)
  ctx.r3.u64 = SecondWheelsPending(g, HolderArg(ctx));
}

// GarageStage::AddSecondWheels: SceneModel::AddSecondWheels on the stage's
// model.
extern "C" REX_FUNC(sub_821779E8) {
  SBX_STAND_MODELS_FALLBACK(sub_821779E8)
  ctx.r3.u64 = game::SceneModelAddSecondWheels(g, StageModel(g, HolderArg(ctx)));
}

// GarageStage::ApplyWheelPaint: SceneModel::ApplyWheelPaint on the stage's
// model.
extern "C" REX_FUNC(sub_82177A28) {
  SBX_STAND_MODELS_FALLBACK(sub_82177A28)
  game::SceneModelApplyWheelPaint(g, StageModel(g, HolderArg(ctx)));
}

// TuningScreen::StepSecondWheels: r3 the tuning screen.
extern "C" REX_FUNC(sub_821CADC8) {
  SBX_STAND_MODELS_FALLBACK(sub_821CADC8)
  StepSecondWheels(g, At<TuningScreen>(ctx.r3.u32));
}

// GarageStage::SetAeroSet: r4 the set, r5 and r6 the fifth and sixth words
// where the set has none.
extern "C" REX_FUNC(sub_8217BAA8) {
  SBX_STAND_MODELS_FALLBACK(sub_8217BAA8)
  ctx.r3.u64 = SetAeroSet(g, HolderArg(ctx), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// GarageStage::SetWheel: r4 the wheel number, r5 which wheels, r6 the car.
extern "C" REX_FUNC(sub_8217BC20) {
  SBX_STAND_MODELS_FALLBACK(sub_8217BC20)
  SetWheel(g, HolderArg(ctx), ctx.r4.s32, ctx.r5.s32, At<game::Car>(ctx.r6.u32));
}

// TuningScreen::PreviewWheel: r3 the tuning screen, r4 the wheel number, r5
// which wheels.
extern "C" REX_FUNC(sub_821DDE78) {
  SBX_STAND_MODELS_FALLBACK(sub_821DDE78)
  PreviewWheel(g, At<TuningScreen>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
}
