// The scene models as our own C++ (--native_scene_models, on by default):
// the car scene's pools set up (ScenePools::SetUpB and each pool's set-up:
// the car model, parts, cockpit, interior, lights and traffic body), a
// model's set-up (its engine sounds loaded), build and re-apply from its
// record, the records' defaults and copies, the parts' and interior's
// links, the accessories' getter, SceneModel::Update (every frame), the
// first set's part loads (set A, the lights' asset) and their callbacks.
//
// The records' floats are copied as be<float> fields, so their bits are
// kept as the originals' lfs/stfs keep them. memcpy (sub_8258CB90, wrapped
// by the native renderer) and the first set's pools (ScenePools::ClaimA,
// GetA, SetUp) are called as they are; the wheels' set-up, the load poll
// and the load slots are this group's C++ (car_models.cpp, load_poll.cpp),
// called directly.
//
// CarRecord::Copy copies the part order with a 44-byte memcpy and its other
// fields one by one; SceneModel::SetUp picks the engine sounds' variant
// from the parts record's word 11, and the cockpit record takes the car
// record's +3676 word.

#include "game/scene_models.h"

#include "base/settings.h"
#include "game/accessories.h"
#include "game/car_models.h"
#include "game/scene.h"

#include <optional>

SBX_SETTING_BOOL(native_scene_models, true, "Overrides", "Scene",
                 "Our own C++ for the scene models (the car scene's pools' set-ups, a "
                 "model's set-up, build and re-apply, the records' defaults and copies, the "
                 "parts' links, SceneModel::Update, the part loads' callbacks) instead of "
                 "the recompiled functions; memcpy and the first set's pools stay recompiled");

namespace {

using game::Accessories;
using game::AccessoryEntry;
using game::CarModel;
using game::CarParts;
using game::CarSceneObjectHead;
using game::CarSceneState;
using game::CarSoundIds;
using game::CockpitObject;
using game::CockpitRecord;
using game::InteriorObject;
using game::InteriorRecord;
using game::Matrix;
using game::PartsRecord;
using game::PolledCockpit;
using game::PolledInterior;
using game::PolledLights;
using game::PolledParts;
using game::PolledTraffic;
using game::PolledWheels;
using game::Pool4Object;
using game::Pool4Record;
using game::Record12;
using game::RecordLayer;
using game::SceneCarModel;
using game::SceneCarRecord;
using game::SceneModel;
using game::SceneModelRecord;
using game::ScenePoolObjectHead;
using game::ScenePoolObjectHeader;
using game::ScenePoolSet;
using game::Vector;
using game::WheelsRecord;
using guest::AddressOf;
using guest::At;
using guest::Guest;
using guest::f32_be;
using guest::u32_be;

bool On() { return SBX_GET(native_scene_models); }

constexpr uint32_t kAccessories = 0x826B51F0;     // Holds the accessories' set.
constexpr uint32_t kSoundLoadsDone = 0x826C7DF8;  // A byte a slot.
constexpr uint32_t kSoundLoaded = 0x826C7E54;     // A word a slot.
constexpr int32_t kNoAccessory = 3;
constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kPageReadWrite = 4;

void Memcpy(Guest& g, const void* to, const void* from, uint32_t bytes) {
  g.Call(sub_8258CB90, to, from, bytes);
}

template <typename T>
void Zero(T& object) {
  for (u32_be& word : reinterpret_cast<u32_be(&)[sizeof(T) / 4]>(object)) {
    word = 0;
  }
}

void Set(Vector& v, float x, float y, float z, float w) {
  v.x = x;
  v.y = y;
  v.z = z;
  v.w = w;
}

// Record12::Reset (sub_820D3780): no index, both floats -1.0.
void ResetRecord12(Record12& record) {
  record.unknown_4 = -1.0f;
  record.unknown_0 = -1;
  record.unknown_8 = -1.0f;
}

// Layer::Reset (sub_820D07D0).
void ResetLayer(RecordLayer& layer) {
  layer.unknown_4 = 0;
  layer.kind = 8;
  layer.unknown_0 = -1;
  Set(layer.unknown_16, 0.0f, 0.0f, 0.0f, 1.0f);
  layer.unknown_32 = 0.0f;
  Set(layer.unknown_48, 1.0f, 1.0f, 1.0f, 1.0f);
}

// CarRecord::Reset (sub_820D0848): nine vectors in three groups of three,
// its 54 layers reset, then the layers' kinds in nine groups of six, and
// the part order.
void ResetCarRecord(SceneCarRecord& record) {
  record.unknown_0 = -1;
  constexpr float kZ[9] = {1.0f, 1.0f, 0.4f, 1.0f, 1.0f, 0.125f, 1.0f, 1.0f, 0.125f};
  for (int i = 0; i < 9; i++) {
    Set(record.unknown_16[i], 0.0f, 0.0f, kZ[i], 1.0f);
  }
  record.unknown_160[0] = 0;
  record.unknown_160[1] = 0;
  for (RecordLayer& layer : record.layers) {
    ResetLayer(layer);
  }
  // Each group of six layers' kind; the first group's last three are 0.
  constexpr uint32_t kGroupKinds[9] = {8, 8, 2, 1, 3, 4, 7, 6, 5};
  for (int group = 0; group < 9; group++) {
    for (int i = 0; i < 6; i++) {
      RecordLayer& layer = record.layers[6 * group + i];
      layer.kind = (group == 0 && i >= 3) ? 0 : kGroupKinds[group];
      if (kGroupKinds[group] == 1 || kGroupKinds[group] == 5) {
        layer.unknown_4 = 1;
      }
    }
  }
  constexpr uint32_t kPartOrder[11] = {0, 0, 0, 1, 2, 3, 4, 5, 0, 6, 7};
  for (int i = 0; i < 11; i++) {
    record.part_order[i] = kPartOrder[i];
  }
  for (u32_be& word : record.unknown_3676) {
    word = 0;
  }
}

// CarRecord::SetW (sub_820D4250): 1.0 into the w of its vectors and its
// layers'.
void SetCarRecordW(SceneCarRecord& record) {
  for (Vector& v : record.unknown_16) {
    v.w = 1.0f;
  }
  for (RecordLayer& layer : record.layers) {
    layer.unknown_16.w = 1.0f;
    layer.unknown_48.w = 1.0f;
  }
}

// PartsRecord::Reset (sub_820D31A0).
void ResetPartsRecord(PartsRecord& record) {
  Zero(record);
  record.unknown_52 = 3;
}

// InteriorRecord::Reset (sub_820D3488).
void ResetInteriorRecord(InteriorRecord& record) {
  Zero(record);
  u32_be* w = record.unknown_0;
  w[7] = 1;   // +28
  w[10] = 2;  // +40
  w[13] = 3;  // +52
  w[16] = 4;  // +64
  w[18] = 1;  // +72
  w[21] = 1;  // +84
  w[22] = 1;  // +88
  record.accessory = kNoAccessory;
}

// CockpitRecord::Reset (sub_820D3100).
void ResetCockpitRecord(CockpitRecord& record) {
  for (u32_be& word : record.unknown_0) {
    word = 0;
  }
  ResetInteriorRecord(record.interior);
}

// WheelsRecord::Reset (sub_820D3548).
void ResetWheelsRecord(WheelsRecord& record) {
  record.unknown_0 = 1;
  for (u32_be& word : record.unknown_4) {
    word = 0;
  }
  for (Record12* r : {&record.unknown_16, &record.unknown_32}) {
    r->unknown_0 = -1;
    r->unknown_4 = 20.0f;
    r->unknown_8 = 0.5f;
  }
  record.unknown_28 = 0;
  ResetRecord12(record.unknown_44);
  ResetRecord12(record.unknown_56);
  for (u32_be& word : record.unknown_68) {
    word = 0;
  }
  record.caliper_texture = 0;
  record.unknown_92[0] = 0;
  record.unknown_92[1] = 0;
  record.unknown_100[0] = 0.0f;
  record.unknown_100[1] = 0.0f;
  record.unknown_108[0] = 11;
  record.unknown_108[1] = 11;
  record.unknown_116[0] = 0;
  record.unknown_116[1] = 0;
  record.unknown_124 = -1;
  for (f32_be& f : record.unknown_128) {
    f = 0.0f;
  }
}

// Pool4Record::Reset (sub_820D3128).
void ResetPool4Record(Pool4Record& record) {
  Zero(record);
  for (Pool4Record::Color& color : record.colors) {
    for (f32_be& f : color.rgb) {
      f = 1.0f;
    }
  }
}

// SceneModelRecord::Reset (sub_820D94D8).
void ResetSceneModelRecord(SceneModelRecord& record) {
  record.kind = 1;
  ResetCarRecord(record.car);
  ResetPartsRecord(record.parts);
  ResetInteriorRecord(record.interior);
  ResetCockpitRecord(record.cockpit);
  ResetWheelsRecord(record.wheels);
  ResetPool4Record(record.pool4);
}

// A layer's copied fields: its first three words, its first vector and the
// float after it, and its last vector.
void CopyLayer(RecordLayer& to, const RecordLayer& from) {
  to.unknown_0 = from.unknown_0;
  to.unknown_4 = from.unknown_4;
  to.kind = from.kind;
  to.unknown_16 = from.unknown_16;
  to.unknown_32 = from.unknown_32;
  to.unknown_48 = from.unknown_48;
}

// CarRecord::Copy (sub_820D7B98): `from` into `to`, field by field, the
// part order with memcpy.
void CopyCarRecord(Guest& g, SceneCarRecord& to, const SceneCarRecord& from) {
  to.unknown_0 = from.unknown_0;
  for (int i = 0; i < 9; i++) {
    to.unknown_16[i] = from.unknown_16[i];
  }
  to.unknown_160[0] = from.unknown_160[0];
  to.unknown_160[1] = from.unknown_160[1];
  for (int i = 0; i < SceneCarRecord::kLayers; i++) {
    CopyLayer(to.layers[i], from.layers[i]);
  }
  Memcpy(g, to.part_order, from.part_order, uint32_t(sizeof(to.part_order)));
  for (int i = 0; i < 4; i++) {
    to.unknown_3676[i] = from.unknown_3676[i];
  }
}

// SceneModelRecord::Copy (sub_820DAF58): the kind, the car record, then the
// other records (memcpy).
void CopySceneModelRecord(Guest& g, SceneModelRecord& to, const SceneModelRecord& from) {
  to.kind = from.kind;
  CopyCarRecord(g, to.car, from.car);
  Memcpy(g, &to.parts, &from.parts, uint32_t(sizeof(PartsRecord)));
  Memcpy(g, &to.interior, &from.interior, uint32_t(sizeof(InteriorRecord)));
  Memcpy(g, &to.cockpit, &from.cockpit, uint32_t(sizeof(CockpitRecord)));
  Memcpy(g, &to.wheels, &from.wheels, uint32_t(sizeof(WheelsRecord)));
  Memcpy(g, &to.pool4, &from.pool4, uint32_t(sizeof(Pool4Record)));
}

// SceneModel::Accessory (sub_820D9480): the accessory the first set's
// interior's record picks from the set, or 0 (none, or no entry).
uint32_t Accessory(const InteriorObject* interior) {
  Accessories* set = At<Accessories>(*At<u32_be>(kAccessories));
  const int32_t index = interior->record.accessory;
  if (index == kNoAccessory) {
    return 0;
  }
  AccessoryEntry* entry =
      *guest::Element<guest::ptr<AccessoryEntry>>(AddressOf(set->entries), uint32_t(index));
  return entry != nullptr ? AddressOf(&entry->accessory) : 0;
}

void Build(Guest& g, SceneModel* model);

// SceneModel::Update (sub_820D9E48): the deferred build counted down (and
// run, SceneModel::Build, on reaching 0 when pending). Then, when the scene
// allows it and the interior is ready, the interior's accessory anchored:
// the identity with row 3 the car model's accessory node, times the
// model's transform (Matrix::Multiply), set on the accessory
// (Accessory::SetAnchor) and stepped (Accessory::Step, r4 = 0), in the
// original's 240-byte frame.
void Update(Guest& g, SceneModel* model) {
  int32_t countdown = model->build_countdown;
  if (countdown > 0) {
    countdown--;
    model->build_countdown = countdown;
    if (model->build_pending != 0 && countdown == 0) {
      Build(g, model);
      model->build_pending = 0;
    }
  }
  if (model->scene->unknown_5000 != 0) {
    return;
  }
  if (model->interior_ready == 0 || model->car_model == nullptr) {
    return;
  }
  // In the front end (set a) its +4296; in a race its cockpit.
  const uint32_t ready =
      model->scene->model_set == 0 ? model->unknown_4296 : model->cockpit.address;
  if (ready == 0) {
    return;
  }
  const uint32_t accessory = Accessory(model->interior->interior);
  if (accessory == 0) {
    return;
  }
  guest::StackFrame frame(g.ctx, 240);
  Matrix* anchor = At<Matrix>(frame.Local(144));
  Set(anchor->right, 1.0f, 0.0f, 0.0f, 0.0f);
  Set(anchor->up, 0.0f, 1.0f, 0.0f, 0.0f);
  Set(anchor->forward, 0.0f, 0.0f, 1.0f, 0.0f);
  anchor->position = model->car_model->model->accessory_node;
  g.Call(sub_8209BE70, anchor, anchor, &model->transform);  // Matrix::Multiply
  g.Call(sub_820EF288, accessory, anchor);                  // Accessory::SetAnchor
  g.Call(sub_820EF0C0, accessory, 0u);                      // Accessory::Step
}

// --- The car scene's pools ---

constexpr uint32_t kFirstSet = 0x826B2B20;  // Holds the first set of pools (the loads).
constexpr uint32_t kCarScene = 0x826B2B24;  // Holds the car scene (the second set).

enum ScenePool : uint32_t {
  kCarModelPool = 0,
  kPartsPool = 1,
  kCockpitPool = 2,
  kInteriorPool = 3,
  kPool4 = 4,
  kWheelsPool = 5,
  kTrafficPool = 6,
  kSceneModelPool = 7,
};

constexpr int32_t kTrafficKind = 3;
constexpr int32_t kCockpitKind = 2;
constexpr int32_t kBlock5 = 5;  // The model block whose interior has choice 1 on.

uint32_t FirstSet() { return *At<u32_be>(kFirstSet); }

// ScenePools::ClaimA, GetA and SetUp: the first set's pools (the loads).
uint32_t ClaimA(Guest& g, uint32_t set, uint32_t pool) { return g.Call(sub_820C40D8, set, pool, 0u); }
uint32_t GetA(Guest& g, uint32_t set, uint32_t pool, uint32_t index) {
  return g.Call(sub_820C4230, set, pool, index);
}
uint32_t SetUpA(Guest& g, uint32_t set, uint32_t pool, uint32_t index, const void* record) {
  return g.Call(sub_820C4148, set, pool, index, record);
}

// ScenePools::ClaimB, GetB and ReleaseB: the car scene's pools.
uint32_t ClaimB(Guest& g, CarSceneState* scene, uint32_t pool) {
  return g.Call(sub_820C5868, scene, pool, 0u);
}
uint32_t GetB(Guest& g, CarSceneState* scene, uint32_t pool, uint32_t index) {
  return g.Call(sub_820C59D8, scene, pool, index);
}
void ReleaseB(Guest& g, CarSceneState* scene, uint32_t pool, uint32_t object) {
  g.Call(sub_820C5A40, scene, pool, uint32_t(At<ScenePoolObjectHeader>(object)->index));
}

uint32_t SetUpB(Guest& g, CarSceneState* scene, uint32_t pool, uint32_t index, const void* record);

// A first-set object's car model set from the second set's car model's.
void ShareCarModel(uint32_t object, const SceneCarModel* owner) {
  At<ScenePoolObjectHead>(object)->car_model = owner->model;
}

// SetCarModel: a second-set object's car model.
void SetCarModel(CarSceneObjectHead* object, uint32_t car_model) {
  object->car_model.address = car_model;
}

// SceneParts::Link: the car model and its model's skeleton.
void LinkParts(PolledParts* parts, uint32_t car_model) {
  parts->car_model.address = car_model;
  parts->skeleton = AddressOf(parts->car_model->model->loads.model.skeleton);
}

// SceneInterior::Link: the car model and its model's skeleton and texture
// providers.
void LinkInterior(PolledInterior* interior, uint32_t car_model) {
  interior->car_model.address = car_model;
  CarModel* model = interior->car_model->model;
  interior->skeleton = AddressOf(model->loads.model.skeleton);
  interior->texture_provider = AddressOf(model->loads.model.texture_providers);
}

// SceneCarModel::SetUp (pool 0): the record copied in and the first set's
// car model made from it.
uint32_t SetUpCarModel(Guest& g, SceneCarModel* object, const SceneCarRecord* record) {
  const uint32_t set = FirstSet();
  object->scene.address = *At<u32_be>(kCarScene);
  CopyCarRecord(g, object->record, *record);
  const uint32_t index = ClaimA(g, set, kCarModelPool);
  object->model.address = SetUpA(g, set, kCarModelPool, index, &object->record);
  return object->model.address;
}

// SceneParts::SetUp (pool 1): the record copied in, the first set's parts
// claimed, given the car model and set up from it.
uint32_t SetUpParts(Guest& g, PolledParts* parts, const PartsRecord* record) {
  const uint32_t set = FirstSet();
  Memcpy(g, &parts->record, record, uint32_t(sizeof(PartsRecord)));
  const uint32_t index = ClaimA(g, set, kPartsPool);
  const uint32_t loads = GetA(g, set, kPartsPool, index);
  parts->parts.address = loads;
  ShareCarModel(loads, parts->car_model);
  parts->parts.address = SetUpA(g, set, kPartsPool, index, &parts->record);
  return parts->parts.address;
}

// SceneInterior::SetUp (pool 3): as the parts, the first set's interior
// told when it's a cockpit's.
uint32_t SetUpInterior(Guest& g, PolledInterior* interior, const InteriorRecord* record) {
  const uint32_t set = FirstSet();
  interior->scene.address = *At<u32_be>(kCarScene);
  Memcpy(g, &interior->record, record, uint32_t(sizeof(InteriorRecord)));
  const uint32_t index = ClaimA(g, set, kInteriorPool);
  const uint32_t loads = GetA(g, set, kInteriorPool, index);
  interior->interior.address = loads;
  if (interior->in_cockpit != 0) {
    At<InteriorObject>(loads)->in_cockpit = 1;
  }
  ShareCarModel(interior->interior.address, interior->car_model);
  interior->interior.address = SetUpA(g, set, kInteriorPool, index, &interior->record);
  return interior->interior.address;
}

// ScenePool4::SetUp (pool 4, the lights): as the parts.
uint32_t SetUpPool4(Guest& g, PolledLights* lights, const Pool4Record* record) {
  const uint32_t set = FirstSet();
  Memcpy(g, &lights->record, record, uint32_t(sizeof(Pool4Record)));
  const uint32_t index = ClaimA(g, set, kPool4);
  const uint32_t loads = GetA(g, set, kPool4, index);
  lights->object.address = loads;
  ShareCarModel(loads, lights->car_model);
  lights->object.address = SetUpA(g, set, kPool4, index, &lights->record);
  return lights->object.address;
}

// SceneTraffic::SetUp (pool 6): the record copied in, the first set's
// traffic car claimed and set up from the caller's record.
uint32_t SetUpTraffic(Guest& g, PolledTraffic* traffic, const SceneCarRecord* record) {
  const uint32_t set = FirstSet();
  traffic->scene.address = *At<u32_be>(kCarScene);
  Memcpy(g, &traffic->record, record, uint32_t(sizeof(SceneCarRecord)));
  const uint32_t index = ClaimA(g, set, kTrafficPool);
  traffic->body.address = GetA(g, set, kTrafficPool, index);
  return SetUpA(g, set, kTrafficPool, index, record);
}

// SceneCockpit::SetUp (pool 2): the record copied in, the first set's
// cockpit body set up, then the cockpit's own interior (pool 3) claimed,
// marked as the cockpit's, linked to the car model but to the body's
// skeleton and texture providers in place of the model's, and set up.
uint32_t SetUpCockpit(Guest& g, PolledCockpit* cockpit, const CockpitRecord* record) {
  const uint32_t set = FirstSet();
  cockpit->scene.address = *At<u32_be>(kCarScene);
  Memcpy(g, &cockpit->record, record, uint32_t(sizeof(CockpitRecord)));
  const uint32_t index = ClaimA(g, set, kCockpitPool);
  const uint32_t body = GetA(g, set, kCockpitPool, index);
  cockpit->body.address = body;
  ShareCarModel(body, cockpit->car_model);
  SetUpA(g, set, kCockpitPool, index, &cockpit->record);

  const uint32_t interior_index = ClaimB(g, cockpit->scene, kInteriorPool);
  PolledInterior* interior =
      At<PolledInterior>(GetB(g, cockpit->scene, kInteriorPool, interior_index));
  cockpit->interior = interior;
  interior->in_cockpit = 1;
  interior->cockpit_points = 1;  // InteriorPoints::SetCockpit
  LinkInterior(cockpit->interior, cockpit->car_model.address);
  cockpit->interior->skeleton = AddressOf(cockpit->body->loads.model.skeleton);
  cockpit->interior->texture_provider = AddressOf(cockpit->body->loads.model.texture_providers);
  return SetUpB(g, cockpit->scene, kInteriorPool, interior_index, &cockpit->record.interior);
}

void SetUp(Guest& g, SceneModel* model, const SceneModelRecord* from);

// ScenePools::SetUpB: object `index` of a pool of the car scene set up from
// a record, by the pool. Returns the object, or 0 when it isn't in use.
uint32_t SetUpB(Guest& g, CarSceneState* scene, uint32_t pool, uint32_t index, const void* record) {
  game::PoolNodeList* list = guest::Element<game::PoolNodeList>(AddressOf(scene->pools), pool);
  const uint32_t object = g.Call(sub_82579570, list, index);  // List::At
  if (object == 0 ||
      At<ScenePoolObjectHeader>(object)->state != ScenePoolObjectHeader::kInUse) {
    return 0;
  }
  const uint32_t from = AddressOf(record);
  switch (pool) {
    case kCarModelPool:
      SetUpCarModel(g, At<SceneCarModel>(object), At<SceneCarRecord>(from));
      break;
    case kPartsPool:
      SetUpParts(g, At<PolledParts>(object), At<PartsRecord>(from));
      break;
    case kInteriorPool:
      SetUpInterior(g, At<PolledInterior>(object), At<InteriorRecord>(from));
      break;
    case kCockpitPool:
      SetUpCockpit(g, At<PolledCockpit>(object), At<CockpitRecord>(from));
      break;
    case kPool4:
      SetUpPool4(g, At<PolledLights>(object), At<Pool4Record>(from));
      break;
    case kWheelsPool:
      game::SceneWheelsSetUp(g, At<PolledWheels>(object), from);
      break;
    case kTrafficPool:
      SetUpTraffic(g, At<PolledTraffic>(object), At<SceneCarRecord>(from));
      break;
    case kSceneModelPool:
      SetUp(g, At<SceneModel>(object), At<SceneModelRecord>(from));
      break;
    default:
      break;
  }
  return object;
}

// The cockpit's record from the model's parts record and interior: its five
// words (Build adjusts the first and fifth), the car record's word, the
// interior copied (memcpy).
void FillCockpitRecord(Guest& g, SceneModel* model, bool adjust) {
  SceneModelRecord& record = model->record;
  const u32_be* parts = record.parts.unknown_0;
  u32_be* cockpit = record.cockpit.unknown_0;
  int32_t first = int32_t(uint32_t(parts[5]));
  int32_t fifth = int32_t(uint32_t(parts[10]));
  if (adjust && first >= 6) {
    first += 5;
  }
  if (adjust && fifth >= 4) {
    fifth += 7;
  }
  cockpit[0] = uint32_t(first);
  cockpit[1] = parts[6];
  cockpit[2] = parts[7];
  cockpit[4] = uint32_t(fifth);
  cockpit[3] = parts[8];
  reinterpret_cast<u32_be&>(record.cockpit.unknown_20) = record.car.unknown_3676[0];
  Memcpy(g, &record.cockpit.interior, &record.interior, uint32_t(sizeof(InteriorRecord)));
}

// The model's cockpit claimed in pool 2, given the car model and set up
// from the cockpit record.
void MakeCockpit(Guest& g, SceneModel* model, CarSceneState* scene) {
  const uint32_t index = ClaimB(g, scene, kCockpitPool);
  model->cockpit.address = GetB(g, model->scene, kCockpitPool, index);
  model->cockpit->car_model = model->car_model;
  model->cockpit.address = SetUpB(g, model->scene, kCockpitPool, index, &model->record.cockpit);
}

// A part claimed in its pool, linked to the model's car model (`link`: its
// Link, or SetCarModel) and set up from `record`. Returns the object.
template <typename Link>
uint32_t MakePart(Guest& g, SceneModel* model, uint32_t pool, Link link, const void* record,
                  u32_be& field) {
  const uint32_t index = ClaimB(g, model->scene, pool);
  field = GetB(g, model->scene, pool, index);
  link(uint32_t(field), model->car_model.address);
  field = SetUpB(g, model->scene, pool, index, record);
  return field;
}

void LinkPartsAt(uint32_t parts, uint32_t car_model) { LinkParts(At<PolledParts>(parts), car_model); }
void LinkInteriorAt(uint32_t interior, uint32_t car_model) {
  LinkInterior(At<PolledInterior>(interior), car_model);
}
void SetCarModelAt(uint32_t object, uint32_t car_model) {
  SetCarModel(At<CarSceneObjectHead>(object), car_model);
}

// SceneModel::Build: the model's parts claimed in the car scene and set up
// from its record. Traffic: its body (pool 6) and wheels; no car model or
// cockpit. Others: the car model, parts, interior, lights and wheels, and
// for a cockpit kind in a scene that shows cockpits, the cockpit.
void Build(Guest& g, SceneModel* model) {
  SceneModelRecord& record = model->record;
  if (int32_t(uint32_t(record.kind)) == kTrafficKind) {
    model->car_model.address = 0xFFFFFFFFu;
    model->cockpit_ready = 1;
    uint32_t index = ClaimB(g, model->scene, kTrafficPool);
    model->traffic.address = GetB(g, model->scene, kTrafficPool, index);
    model->traffic.address = SetUpB(g, model->scene, kTrafficPool, index, &record.car);
    index = ClaimB(g, model->scene, kWheelsPool);
    model->wheels.address = GetB(g, model->scene, kWheelsPool, index);
    model->wheels->traffic_record = AddressOf(&record.car);  // SceneWheels::SetRecord
    model->wheels.address = SetUpB(g, model->scene, kWheelsPool, index, &record.wheels);
    return;
  }
  const uint32_t index = ClaimB(g, model->scene, kCarModelPool);
  model->car_model.address = SetUpB(g, model->scene, kCarModelPool, index, &record.car);
  MakePart(g, model, kPartsPool, LinkPartsAt, &record.parts, model->parts.address);
  model->parts->model_record = AddressOf(&record);  // SceneParts::SetRecord
  MakePart(g, model, kInteriorPool, LinkInteriorAt, &record.interior, model->interior.address);
  MakePart(g, model, kPool4, SetCarModelAt, &record.pool4, model->lights.address);
  MakePart(g, model, kWheelsPool, SetCarModelAt, &record.wheels, model->wheels.address);
  CarSceneState* scene = model->scene;
  if (int32_t(uint32_t(record.kind)) == kCockpitKind && model->scene->model_set == 1) {
    FillCockpitRecord(g, model, true);
    MakeCockpit(g, model, scene);
  } else {
    model->cockpit_ready = 1;
  }
}

// CarModel::SetRecord: the car record copied into the first set's car
// model (memcpy).
void SetCarModelRecord(Guest& g, CarModel* model, const SceneCarRecord* record) {
  Memcpy(g, &model->record, record, uint32_t(sizeof(SceneCarRecord)));
}

// SceneParts::Apply: unless the car scene is the race's (set b), the record
// (60 bytes) copied to the parts' and the parts rebuilt (SceneParts::Build).
void ApplyParts(Guest& g, PolledParts* parts, const PartsRecord* record) {
  if (parts->scene->model_set == 1) {
    return;
  }
  Memcpy(g, &parts->record, record, uint32_t(sizeof(PartsRecord)));
  game::ScenePartsBuild(g, parts);
}

// SceneInterior::Apply: the record (120 bytes) copied to the interior's
// and to the first set's interior's; with an index, its group's +8 the car
// scene's part model of that index; each of the six part models given the
// car model's selected texture; then the gauges set
// (SceneInterior::SetGauges).
void ApplyInterior(Guest& g, PolledInterior* interior, const InteriorRecord* record) {
  Memcpy(g, &interior->record, record, uint32_t(sizeof(InteriorRecord)));
  Memcpy(g, &interior->interior->record, record, uint32_t(sizeof(InteriorRecord)));
  const int32_t index = interior->record.unknown_100;
  if (index != 0) {
    // The car scene's part model `index` (1 to 4).
    const CarSceneState* scene = interior->scene;
    interior->group_9488.unknown_8 = AddressOf(guest::Element<game::PartModel>(
        AddressOf(scene->assets.parts_1184), uint32_t(index - 1)));
  }
  for (const u32_be& part : interior->part_models) {
    CarModel* model = interior->car_model->model;
    At<game::PartModel>(part)->unknown_452[0] = game::CarModelSelectedTexture(g, model);
  }
  game::SceneInteriorSetGauges(g, interior);
}

// SceneModel::ApplyRecord: once its loads are done (the poll), the model
// rebuilt from its record. In a scene without cockpits (the stand's) the
// car model, parts and interior take the record in place; otherwise new
// parts and interior are made and the old ones released, and a cockpit
// kind's cockpit is made again. Returns 0 while still loading.
uint32_t ApplyRecord(Guest& g, SceneModel* model) {
  if (game::SceneModelPollLoad(g, model) == 0) {
    return 0;
  }
  SceneModelRecord& record = model->record;
  if (int32_t(record.car.unknown_0) == kBlock5) {
    record.interior.unknown_0[6] = 0;
    record.interior.unknown_0[5] = 1;
  }
  const bool cockpits = model->scene->model_set != 0;
  SetCarModelRecord(g, model->car_model->model, &record.car);
  if (!cockpits) {
    ApplyParts(g, model->parts, &record.parts);
    ApplyInterior(g, model->interior, &record.interior);
  } else {
    const uint32_t old_parts = model->parts.address;
    MakePart(g, model, kPartsPool, LinkPartsAt, &record.parts, model->parts.address);
    model->parts->model_record = AddressOf(&record);  // SceneParts::SetRecord
    const uint32_t old_interior = model->interior.address;
    MakePart(g, model, kInteriorPool, LinkInteriorAt, &record.interior, model->interior.address);
    if (old_parts != 0) {
      ReleaseB(g, model->scene, kPartsPool, old_parts);
    }
    if (old_interior != 0) {
      ReleaseB(g, model->scene, kInteriorPool, old_interior);
    }
    model->ready = 0;
    model->parts_ready = 0;
    model->interior_ready = 0;
  }
  if (int32_t(uint32_t(record.kind)) != kCockpitKind || model->scene->model_set != 1) {
    return 0;
  }
  CarSceneState* scene = model->scene;
  if (model->cockpit != nullptr) {
    ReleaseB(g, scene, kCockpitPool, model->cockpit.address);
  }
  model->ready = 0;
  model->cockpit = nullptr;
  model->cockpit_ready = 0;
  FillCockpitRecord(g, model, false);
  MakeCockpit(g, model, scene);
  return model->cockpit.address;
}

// --- The model's set-up and its engine sounds ---

constexpr uint32_t kGameMode = 0x825F3264;
constexpr int32_t kNoSoundsMode = 5;
constexpr uint32_t kCarSounds = 0x826062A0;      // CarSoundIds by block.
constexpr uint32_t kSoundLoading = 0x826C7E38;   // A word a slot.
constexpr uint32_t kSoundBuffersA = 0x826C7E1C;  // A buffer a slot, the first load's.
constexpr uint32_t kSoundBuffersB = 0x826C7E00;  // The second load's.
constexpr uint32_t kSoundContexts = 0x826C7DDC;  // The loads' callback contexts.
constexpr uint32_t kSoundBlocks = 0x826B5928;    // The block each slot's sounds are.
constexpr uint32_t kSoundVariants = 0x826B5944;
constexpr uint32_t kSoundBufferABytes = 0x6B080;
constexpr uint32_t kSoundBufferBBytes = 0x5D2D0;
constexpr uint32_t kOnSoundLoaded = 0x82173DA0;  // SceneModel::OnSoundLoaded
constexpr int32_t kTrafficSoundBlock = 47;
constexpr int32_t kSoundBlocksWithVariants = 18;

// Read-only image constants: the model's +4916 by the scene's kind.
constexpr float kStandValue4916 = 0.3f;  // 0x82002944
constexpr float kSceneValue4916 = 0.1f;  // 0x82001F94

// The engine sounds' variant, by the parts record's word 11 (0 to 9).
constexpr int32_t kSoundVariantOf[10] = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4};

u32_be& SlotWord(uint32_t table, uint32_t slot) { return *guest::Element<u32_be>(table, slot); }

// The model's two engine sound assets loaded into its slot's buffers (each
// cleared first), by its block and variant.
void LoadEngineSounds(Guest& g, SceneModel* model) {
  const SceneModelRecord& record = model->record;
  const int32_t kind = int32_t(uint32_t(record.kind));
  int32_t block = 0;
  int32_t variant = 0;
  if (kind == 1 || kind == 2) {
    block = record.car.unknown_0;
    const uint32_t word = record.parts.unknown_0[11];
    if (block < kSoundBlocksWithVariants && word <= 9) {
      variant = kSoundVariantOf[word];
    }
  } else if (kind == kTrafficKind) {
    block = kTrafficSoundBlock;
  }
  SlotWord(kSoundLoaded, model->slot) = 0;
  SlotWord(kSoundLoading, model->slot) = 1;
  *guest::Element<uint8_t>(kSoundLoadsDone, model->slot) = 0;
  g.Call(sub_8258C950, SlotWord(kSoundBuffersA, model->slot), 0u, kSoundBufferABytes);  // memset
  g.Call(sub_8258C950, SlotWord(kSoundBuffersB, model->slot), 0u, kSoundBufferBBytes);  // memset
  const CarSoundIds* ids = guest::Element<CarSoundIds>(kCarSounds, uint32_t(block));
  const uint32_t second = SlotWord(ids->second, uint32_t(variant));
  const uint32_t first = int32_t(record.car.unknown_0) < kSoundBlocksWithVariants
                             ? uint32_t(SlotWord(ids->first, uint32_t(variant)))
                             : uint32_t(SlotWord(ids->first, 0));
  SlotWord(kSoundBlocks, model->slot) = uint32_t(block);
  SlotWord(kSoundVariants, model->slot) = uint32_t(variant);
  const uint32_t context = AddressOf(&SlotWord(kSoundContexts, model->slot));
  g.Call(sub_8209DF80, first, SlotWord(kSoundBuffersA, model->slot), kOnSoundLoaded, context, 0u,
         0u);  // Archive::Load
  g.Call(sub_8209DF80, second, SlotWord(kSoundBuffersB, model->slot), kOnSoundLoaded, context,
         0u, 0u);  // Archive::Load
}

// SceneModel::SetUp (pool 7): the record copied in, the scene's settings,
// in a race scene (outside mode 5) its engine sounds loaded, then built
// now or, while the build is held back, by SceneModel::Update later.
void SetUp(Guest& g, SceneModel* model, const SceneModelRecord* from) {
  model->unknown_5024 = 1;
  CopySceneModelRecord(g, model->record, *from);
  SceneModelRecord& record = model->record;
  record.wheels.unknown_0 = record.kind;
  if (int32_t(record.car.unknown_0) == kBlock5) {
    record.interior.unknown_0[5] = 1;
    record.interior.unknown_0[6] = 0;
  }
  const CarSceneState* scene = model->scene;
  model->unknown_4916 = scene->model_set == 0 ? kStandValue4916 : kSceneValue4916;
  if (scene->model_set == 1 && int32_t(*At<u32_be>(kGameMode)) != kNoSoundsMode) {
    LoadEngineSounds(g, model);
  }
  if (model->build_countdown != 0) {
    model->build_pending = 1;
  } else {
    Build(g, model);
  }
}

// --- The first set's part loads ---

constexpr int kPartSlotsA = game::CarPartsLoads::kParts;  // CarParts::LoadSetA's slots.
constexpr int kPartSlotsB = 17;                           // CarParts::LoadSetB's.

constexpr uint32_t kOnPartLoadedA = 0x820D68C0;    // CarParts::OnPartLoadedA
constexpr uint32_t kOnPool4Loaded = 0x820D6668;    // Pool4Object::OnLoaded
constexpr uint32_t kPartsSetAFirst = 893;          // Set A: 226 assets a block.
constexpr uint32_t kPartsSetABlock = 226;
constexpr uint32_t kPool4SetAFirst = 1115;
constexpr uint32_t kPool4SetBFirst = 11256;        // Set B: 197 assets a block.
constexpr uint32_t kPartsSetBBlock = 197;

// The block of a first-set object's car model.
uint32_t CarBlockOf(const ScenePoolObjectHead& head) {
  return uint32_t(head.car_model->record.unknown_0);
}

// LoadSlot::LoadShared: asset `id` into a slot, `callback` with `context`
// when it's loaded.
void LoadShared(Guest& g, game::LoadSlot* slot, uint32_t id, uint32_t context,
                uint32_t callback) {
  game::LoadSlotLoadShared(g, slot, id, context, callback);
}

// CarParts::LoadSetA: each of the 216 part slots loads its asset of set A
// (the car block's run of 226).
void LoadPartsSetA(Guest& g, CarParts* parts) {
  const uint32_t first = CarBlockOf(parts->head) * kPartsSetABlock + kPartsSetAFirst;
  for (uint32_t i = 0; i < uint32_t(game::CarPartsLoads::kParts); i++) {
    game::LoadSlot& slot = parts->loads.slots[i];
    slot.unknown_0 = first + i;
    LoadShared(g, &slot, first + i, AddressOf(parts), kOnPartLoadedA);
  }
}

// Pool4Object::Load: the record copied in, then its one asset loaded (by
// the asset set and the car's block).
void LoadPool4(Guest& g, Pool4Object* object, const Pool4Record* record) {
  Memcpy(g, &object->record, record, uint32_t(sizeof(Pool4Record)));
  const uint32_t set = object->first_set->model_set;
  const uint32_t block = CarBlockOf(object->head);
  const uint32_t id = set == ScenePoolSet::kSetA ? block * kPartsSetABlock + kPool4SetAFirst
                                                     : block * kPartsSetBBlock + kPool4SetBFirst;
  LoadShared(g, &object->slot, id, AddressOf(object), kOnPool4Loaded);
}

// CarParts::LoadSetB: set B's 17 part slots, each its asset by the parts
// record (the car block's run of 197 from 11060); slots 7, 9 and 14 have
// none, and slot 4 none when its word is 13.
constexpr uint32_t kOnPartLoadedB = 0x820D6938;  // CarParts::OnPartLoadedB
constexpr uint32_t kPartsSetBFirst = 11060;
constexpr int kNoPart4 = 13;

std::optional<uint32_t> SetBAsset(const PartsRecord& record, uint32_t first, uint32_t slot) {
  const u32_be* words = record.unknown_0;
  switch (slot) {
    case 0: return first + words[0] + 3;
    case 1: return first + 7 * words[0] + words[1] + 16;
    case 2: return first + words[2] + 107;
    case 3: return first + words[3] + 120;
    case 4:
      if (int32_t(uint32_t(words[4])) == kNoPart4) {
        return std::nullopt;
      }
      return first + words[4] + 133;
    case 5: return first + words[5] + 146;
    case 6: return first + words[6] + 154;
    case 8: return first + words[7] + 157;
    case 10: return first + words[8] + 169;
    case 11: return first + words[9] + 173;
    case 12: return first + words[10] + 178;
    case 13: return first + 177;
    case 15: return first + words[12] + 184;
    case 16: return first + 189;
    default: return std::nullopt;  // 7, 9 and 14.
  }
}

void LoadPartsSetB(Guest& g, CarParts* parts) {
  const uint32_t first = CarBlockOf(parts->head) * kPartsSetBBlock + kPartsSetBFirst;
  for (uint32_t i = 0; i < uint32_t(kPartSlotsB); i++) {
    if (const std::optional<uint32_t> id = SetBAsset(parts->record, first, i)) {
      LoadShared(g, &parts->loads.slots[i], *id, AddressOf(parts), kOnPartLoadedB);
    }
  }
}

// CarParts::Load: the record copied in, then the parts of the pools' model
// set (a or b; any other none).
void LoadParts(Guest& g, CarParts* parts, const PartsRecord* record) {
  Memcpy(g, &parts->record, record, uint32_t(sizeof(PartsRecord)));
  const uint32_t set = parts->first_set->model_set;
  if (set == 0) {
    LoadPartsSetA(g, parts);
  } else if (set == 1) {
    LoadPartsSetB(g, parts);
  }
}

// TrafficCar::Load: the record copied in, the car's file (asset 22528 + its
// number) loaded into its node slot (TrafficCar::OnFileLoaded when done;
// a failed request prints an error into a buffer on the original's
// 1,136-byte frame that nothing reads), then a blank 16 x 16 texture made
// for its vinyl sheet.
constexpr uint32_t kTrafficFilesFirst = 22528;
constexpr uint32_t kOnTrafficFileLoaded = 0x820D80E0;  // TrafficCar::OnFileLoaded
constexpr uint32_t kTrafficLoadError = 0x8200509C;     // "... TSP_OthData::Load( %d )"
constexpr uint32_t kBlankTextureFormat = 0x18280086;    // A8R8G8B8
constexpr uint32_t kBlankTextureSize = 16;

struct TrafficLoadFrame {
  uint8_t unknown_0[80];
  char message[1056];
};
GUEST_SIZE(TrafficLoadFrame, 1136);

void LoadTrafficCar(Guest& g, game::TrafficCarObject* car, const SceneCarRecord* record) {
  guest::StackFrame frame(g.ctx, uint32_t(sizeof(TrafficLoadFrame)));
  CopyCarRecord(g, car->record, *record);
  const uint32_t number = uint32_t(int32_t(car->record.unknown_0));
  car->first_set.address = *At<u32_be>(kFirstSet);
  game::LoadSlot* slot = &car->loads.head.node_slot;
  if (game::LoadSlotLoad(g, slot, number + kTrafficFilesFirst, AddressOf(car),
                         kOnTrafficFileLoaded) == 0) {
    TrafficLoadFrame* locals = At<TrafficLoadFrame>(frame.Local(0));
    g.Call(sub_8258ED70, locals->message, kTrafficLoadError, number);  // sprintf
  }
  game::TexturePair& sheet = car->loads.head.sheet;
  game::TextureCreate(g, kBlankTextureFormat, kBlankTextureSize, kBlankTextureSize, &sheet.header,
                      &sheet.data);
}

// --- The first set's part loads' callbacks ---

constexpr uint32_t kSlotLoaded = 3;                       // A LoadSlot's state once loaded.

// Archive::Load's callback for a set of part slots: the slot whose request
// finished gets its model loaded from its buffer (Model::Load), the buffer
// released and the slot marked loaded (3).
void OnPartLoaded(Guest& g, CarParts* parts, uint32_t request, int count) {
  for (int i = 0; i < count; i++) {
    game::LoadSlot& slot = parts->loads.slots[i];
    if (uint32_t(slot.unknown_4) != request) {
      continue;
    }
    g.Call(sub_8257C238, &parts->loads.parts[i], slot.block, 0u);  // Model::Load
    game::LoadSlotReleaseBuffer(g, &slot);
    slot.state = kSlotLoaded;
    return;
  }
}

// Pool4Object::OnLoaded: the object's model loaded from the data, the
// buffer released and the slot marked loaded, when the request is its.
void OnPool4Loaded(Guest& g, Pool4Object* object, uint32_t request, uint32_t data) {
  if (uint32_t(object->slot.unknown_4) != request) {
    return;
  }
  g.Call(sub_8257C238, &object->model, data, 0u);  // Model::Load
  game::LoadSlotReleaseBuffer(g, &object->slot);
  object->slot.state = kSlotLoaded;
}

// SceneModel::OnSoundLoaded: Archive::Load's callback for SceneModel::
// SetUp's two sound assets (the context's +0 the slot): the slot's byte up
// by one, and when it's then 2 (the slot read again) its word = 1.
void OnSoundLoaded(const u32_be* slot) {
  uint8_t* done = guest::Element<uint8_t>(kSoundLoadsDone, *slot);
  *done = uint8_t(*done + 1);
  const uint32_t again = *slot;
  if (*guest::Element<uint8_t>(kSoundLoadsDone, again) == 2) {
    *guest::Element<u32_be>(kSoundLoaded, again) = 1;
  }
}

// SceneModel::ReloadDecals: when marked (and not kind 3), the car model's
// record set from the model's (CarModel::SetRecord) and its decals rebuilt
// (CarModel::ReloadDecals).
void ReloadDecals(Guest& g, SceneModel* model) {
  if (model->ready == 0 || int32_t(uint32_t(model->record.kind)) == kTrafficKind) {
    return;
  }
  model->ready = 0;
  model->decals_ready = 0;
  SetCarModelRecord(g, model->car_model->model, &model->record.car);
  game::CarModelReloadDecals(g, model->car_model->model);
}

// SceneModel::AddSecondWheels: once, the scene's pool 5 entry made, linked
// to the car model (SetCarModel) and set up from the model's wheels record.
// Whether it was made now.
bool AddSecondWheels(Guest& g, SceneModel* model) {
  if (model->second_wheels_added != 0) {
    return false;
  }
  model->second_wheels_added = 1;
  model->unknown_5112 = 0;
  const uint32_t entry = ClaimB(g, model->scene, kWheelsPool);
  model->second_wheels.address = GetB(g, model->scene, kWheelsPool, entry);
  SetCarModel(At<CarSceneObjectHead>(model->second_wheels.address), model->car_model.address);
  SetUpB(g, model->scene, kWheelsPool, entry, &model->record.wheels);
  return true;
}

// The interior whose points (at its +160) a function is given.
PolledInterior* InteriorOfPoints(uint32_t points) {
  return At<PolledInterior>(points - uint32_t(offsetof(PolledInterior, points)));
}

}  // namespace

// --- What the group's other files call ---

namespace game {

void Record12Reset(Record12& record) { ResetRecord12(record); }
void CarRecordReset(SceneCarRecord& record) { ResetCarRecord(record); }
void CarRecordSetW(SceneCarRecord& record) { SetCarRecordW(record); }
void PartsRecordReset(PartsRecord& record) { ResetPartsRecord(record); }
void InteriorRecordReset(InteriorRecord& record) { ResetInteriorRecord(record); }
void CockpitRecordReset(CockpitRecord& record) { ResetCockpitRecord(record); }
void WheelsRecordReset(WheelsRecord& record) { ResetWheelsRecord(record); }
void Pool4RecordReset(Pool4Record& record) { ResetPool4Record(record); }
void SceneModelRecordReset(SceneModelRecord& record) { ResetSceneModelRecord(record); }

// SceneModelRecord::SceneModelRecord: the car record's w set, the two
// Record12s in its wheels record reset.
void SceneModelRecordInit(SceneModelRecord& record) {
  SetCarRecordW(record.car);
  ResetRecord12(record.wheels.unknown_44);
  ResetRecord12(record.wheels.unknown_56);
}

// Accessories::Get: the accessories' set.
uint32_t AccessoriesGet() { return *At<u32_be>(kAccessories); }

// CarModel::AllocPages: `bytes` of pages (VirtualAlloc) into [out]; the
// pages.
uint32_t CarModelAllocPages(Guest& g, u32_be* out, uint32_t bytes) {
  *out = g.Call(sub_82292DD8, 0u, bytes, kMemCommit, kPageReadWrite);
  return *out;
}

uint32_t ScenePoolsSetUpB(Guest& g, CarSceneState* scene, uint32_t pool, uint32_t index,
                          uint32_t record) {
  return SetUpB(g, scene, pool, index, At<void>(record));
}

uint32_t SceneModelApplyRecord(Guest& g, SceneModel* model) { return ApplyRecord(g, model); }

void SceneModelReloadDecals(Guest& g, SceneModel* model) { ReloadDecals(g, model); }

uint32_t SceneModelAddSecondWheels(Guest& g, SceneModel* model) {
  return AddSecondWheels(g, model) ? 1 : 0;
}

}  // namespace game

#define SBX_SCENE_MODELS_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                  \
  if (!On()) {                          \
    __imp__##name(ctx, base);           \
    return;                             \
  }                                     \
  Guest g(ctx, base);

// Accessories::Get: the accessories' set.
extern "C" REX_FUNC(sub_820F0CE0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820F0CE0)
  ctx.r3.u64 = game::AccessoriesGet();
}

// SceneModel::Accessory: r3 the first set's interior.
extern "C" REX_FUNC(sub_820D9480) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D9480)
  ctx.r3.u64 = Accessory(At<InteriorObject>(ctx.r3.u32));
}

// SceneModel::OnSoundLoaded: Archive::Load's callback, r6 the context.
extern "C" REX_FUNC(sub_82173DA0) {
  SBX_SCENE_MODELS_FALLBACK(sub_82173DA0)
  OnSoundLoaded(At<u32_be>(ctx.r6.u32));
}

// SceneWheels::SetRecord: r3 the wheels, r4 a traffic car's record.
extern "C" REX_FUNC(sub_820E6F60) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E6F60)
  At<PolledWheels>(ctx.r3.u32)->traffic_record = ctx.r4.u32;
}

// SetCarModel: r3 a second-set object, r4 the car model.
extern "C" REX_FUNC(sub_820DC710) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DC710)
  SetCarModel(At<CarSceneObjectHead>(ctx.r3.u32), ctx.r4.u32);
}

// SceneParts::Link: r3 the parts, r4 the car model.
extern "C" REX_FUNC(sub_820DEF70) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DEF70)
  LinkParts(At<PolledParts>(ctx.r3.u32), ctx.r4.u32);
}

// SceneParts::SetRecord: r3 the parts, r4 the scene model's record.
extern "C" REX_FUNC(sub_820DEF88) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DEF88)
  At<PolledParts>(ctx.r3.u32)->model_record = ctx.r4.u32;
}

// SceneInterior::Link: r3 the interior, r4 the car model.
extern "C" REX_FUNC(sub_820E3398) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E3398)
  LinkInterior(At<PolledInterior>(ctx.r3.u32), ctx.r4.u32);
}

// InteriorPoints::SetCockpit: r3 an interior's points, r4 the flag.
extern "C" REX_FUNC(sub_820E3220) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E3220)
  InteriorOfPoints(ctx.r3.u32)->cockpit_points = ctx.r4.u32;
}

// CarModel::SetRecord: r3 the first set's car model, r4 the car record
// (memcpy's r3 left).
extern "C" REX_FUNC(sub_820D3910) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D3910)
  SetCarModelRecord(g, At<CarModel>(ctx.r3.u32), At<SceneCarRecord>(ctx.r4.u32));
}

// The records' defaults: r3 the record. Each leaves r3 as the original
// does where a recompiled caller reads it.
extern "C" REX_FUNC(sub_820D3780) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D3780)
  game::Record12Reset(*At<Record12>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D07D0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D07D0)
  ResetLayer(*At<RecordLayer>(ctx.r3.u32));
}

// CarRecord::Reset: r3 left at its last layer (CarModel::Release's
// recompiled callers hand it on).
extern "C" REX_FUNC(sub_820D0848) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D0848)
  SceneCarRecord* record = At<SceneCarRecord>(ctx.r3.u32);
  game::CarRecordReset(*record);
  ctx.r3.u64 = guest::AddressOf(&record->layers[SceneCarRecord::kLayers - 1]);
}

extern "C" REX_FUNC(sub_820D31A0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D31A0)
  game::PartsRecordReset(*At<PartsRecord>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D3488) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D3488)
  game::InteriorRecordReset(*At<InteriorRecord>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D3100) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D3100)
  game::CockpitRecordReset(*At<CockpitRecord>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D3548) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D3548)
  game::WheelsRecordReset(*At<WheelsRecord>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D3128) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D3128)
  game::Pool4RecordReset(*At<Pool4Record>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D4250) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D4250)
  game::CarRecordSetW(*At<SceneCarRecord>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D94D8) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D94D8)
  game::SceneModelRecordReset(*At<SceneModelRecord>(ctx.r3.u32));
}

// SceneModelRecord::SceneModelRecord: r3 the record, returned.
extern "C" REX_FUNC(sub_820DAF08) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DAF08)
  const uint32_t address = ctx.r3.u32;
  game::SceneModelRecordInit(*At<SceneModelRecord>(address));
  ctx.r3.u64 = address;
}

// The copies: r3 to, r4 from; each returns `to`.
extern "C" REX_FUNC(sub_820D7B98) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D7B98)
  const uint32_t to = ctx.r3.u32;
  CopyCarRecord(g, *At<SceneCarRecord>(to), *At<SceneCarRecord>(ctx.r4.u32));
  ctx.r3.u64 = to;
}

extern "C" REX_FUNC(sub_820DAF58) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DAF58)
  const uint32_t to = ctx.r3.u32;
  CopySceneModelRecord(g, *At<SceneModelRecord>(to), *At<SceneModelRecord>(ctx.r4.u32));
  ctx.r3.u64 = to;
}

// CarModel::AllocPages: r4 where the pages' address goes, r5 the bytes.
// Returns the pages.
extern "C" REX_FUNC(sub_820C3AB0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820C3AB0)
  ctx.r3.u64 = game::CarModelAllocPages(g, At<u32_be>(ctx.r4.u32), ctx.r5.u32);
}

// SceneModel::Update: r3 the scene model (every frame).
extern "C" REX_FUNC(sub_820D9E48) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D9E48)
  Update(g, At<SceneModel>(ctx.r3.u32));
}

// SceneParts::Apply: r3 the parts, r4 the parts record.
extern "C" REX_FUNC(sub_820E0458) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E0458)
  ApplyParts(g, At<PolledParts>(ctx.r3.u32), At<PartsRecord>(ctx.r4.u32));
}

// SceneInterior::Apply: r3 the interior, r4 the interior record.
extern "C" REX_FUNC(sub_820E3E20) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E3E20)
  ApplyInterior(g, At<PolledInterior>(ctx.r3.u32), At<InteriorRecord>(ctx.r4.u32));
}

// SceneModel::ReloadDecals: r3 the scene model.
extern "C" REX_FUNC(sub_820D9A10) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D9A10)
  game::SceneModelReloadDecals(g, At<SceneModel>(ctx.r3.u32));
}

// SceneModel::AddSecondWheels: r3 the scene model. 1, or 0 when done
// before.
extern "C" REX_FUNC(sub_820D9AC8) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D9AC8)
  ctx.r3.u64 = game::SceneModelAddSecondWheels(g, At<SceneModel>(ctx.r3.u32));
}

// ScenePools::SetUpB: r3 the car scene, r4 the pool, r5 the index, r6 the
// record. Returns the object, or 0 when it isn't in use.
extern "C" REX_FUNC(sub_820C58E0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820C58E0)
  ctx.r3.u64 =
      game::ScenePoolsSetUpB(g, At<CarSceneState>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// The car scene's pools' set-ups: r3 the object, r4 the record.
extern "C" REX_FUNC(sub_820DB488) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DB488)
  ctx.r3.u64 = SetUpCarModel(g, At<SceneCarModel>(ctx.r3.u32), At<SceneCarRecord>(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_820DEF98) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DEF98)
  ctx.r3.u64 = SetUpParts(g, At<PolledParts>(ctx.r3.u32), At<PartsRecord>(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_820DBAF0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DBAF0)
  ctx.r3.u64 = SetUpCockpit(g, At<PolledCockpit>(ctx.r3.u32), At<CockpitRecord>(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_820E33B8) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E33B8)
  ctx.r3.u64 = SetUpInterior(g, At<PolledInterior>(ctx.r3.u32), At<InteriorRecord>(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_820DC718) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DC718)
  ctx.r3.u64 = SetUpPool4(g, At<PolledLights>(ctx.r3.u32), At<Pool4Record>(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_820E5588) {
  SBX_SCENE_MODELS_FALLBACK(sub_820E5588)
  ctx.r3.u64 = SetUpTraffic(g, At<PolledTraffic>(ctx.r3.u32), At<SceneCarRecord>(ctx.r4.u32));
}

// SceneModel::SetUp: r3 the scene model, r4 its record.
extern "C" REX_FUNC(sub_820DB0C8) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DB0C8)
  SetUp(g, At<SceneModel>(ctx.r3.u32), At<SceneModelRecord>(ctx.r4.u32));
}

// SceneModel::Build: r3 the scene model.
extern "C" REX_FUNC(sub_820D9538) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D9538)
  Build(g, At<SceneModel>(ctx.r3.u32));
}

// SceneModel::ApplyRecord: r3 the scene model. 0 while its loads aren't
// done.
extern "C" REX_FUNC(sub_820DA420) {
  SBX_SCENE_MODELS_FALLBACK(sub_820DA420)
  ctx.r3.u64 = game::SceneModelApplyRecord(g, At<SceneModel>(ctx.r3.u32));
}

// CarParts::OnPartLoadedA and B: Archive::Load's callbacks for the two sets
// of part loads (216 and 17 slots): r3 the finished request, r6 the parts.
extern "C" REX_FUNC(sub_820D68C0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D68C0)
  OnPartLoaded(g, At<CarParts>(ctx.r6.u32), ctx.r3.u32, kPartSlotsA);
}

extern "C" REX_FUNC(sub_820D6938) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D6938)
  OnPartLoaded(g, At<CarParts>(ctx.r6.u32), ctx.r3.u32, kPartSlotsB);
}

// Pool4Object::OnLoaded: Archive::Load's callback for a pool 4 object's
// load: r3 the finished request, r4 its data, r6 the object.
extern "C" REX_FUNC(sub_820D6668) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D6668)
  OnPool4Loaded(g, At<Pool4Object>(ctx.r6.u32), ctx.r3.u32, ctx.r4.u32);
}

// CarParts::LoadSetA: r3 the parts.
extern "C" REX_FUNC(sub_820D6D40) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D6D40)
  LoadPartsSetA(g, At<CarParts>(ctx.r3.u32));
}

// Pool4Object::Load: r3 the object, r4 its record.
extern "C" REX_FUNC(sub_820D67F8) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D67F8)
  LoadPool4(g, At<Pool4Object>(ctx.r3.u32), At<Pool4Record>(ctx.r4.u32));
}

// CarParts::Load: r3 the parts, r4 their record.
extern "C" REX_FUNC(sub_820D71C0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D71C0)
  LoadParts(g, At<CarParts>(ctx.r3.u32), At<PartsRecord>(ctx.r4.u32));
}

// CarParts::LoadSetB: r3 the parts.
extern "C" REX_FUNC(sub_820D6DA0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D6DA0)
  LoadPartsSetB(g, At<CarParts>(ctx.r3.u32));
}

// TrafficCar::Load: r3 the traffic car, r4 its record.
extern "C" REX_FUNC(sub_820D86C0) {
  SBX_SCENE_MODELS_FALLBACK(sub_820D86C0)
  LoadTrafficCar(g, At<game::TrafficCarObject>(ctx.r3.u32), At<SceneCarRecord>(ctx.r4.u32));
}
