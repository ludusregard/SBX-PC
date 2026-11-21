// The scene loaders as our own C++ (--native_scene_loaders, on by
// default): the scene pools' nodes and objects (the car model, car parts,
// cockpit, interior, pool 4 object, wheels and traffic car: their
// constructors, destructors, resets and releases), their loads'
// constructors and destructors, the load checks, the walks over their
// arrived parts (palettes, paints, materials), a part by its kind, and an
// XPR's records by name; the first pool set's creation.
//
// The car model's reset, ModelPoint::Find (a part's point by node) and the
// cockpit's and traffic car's file callbacks build matrices in frames of
// the originals' sizes and layouts for their callees, as do the cockpit's
// and traffic car's texture binds (the names they print).

#include "base/settings.h"
#include "game/scene_loads.h"

#include "game/accessories.h"
#include "game/car_models.h"
#include "game/data_file.h"
#include "game/model_animation.h"
#include "game/scene_models.h"
#include "game/scene_points.h"

#include <fmt/format.h>
#include <rex/logging.h>

#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>

SBX_SETTING_BOOL(native_scene_loaders, true, "Overrides", "Scene",
                 "Our own C++ for the scene pools' objects (constructors, destructors, "
                 "resets, releases, file callbacks), their loads' set-ups, the load checks, "
                 "the parts' walks and a part's point by node instead of the recompiled "
                 "functions; the loads themselves stay recompiled");
SBX_SETTING_BOOL(log_scene_loaders, false, "Hooks", "Other",
                 "Log each call of the scene loaders' functions (natively or not), once "
                 "per distinct call: to compare runs with --native_scene_loaders on and off");

namespace {

using game::Accessories;
using game::AccessoryEntry;
using game::CarBodyLoads;
using game::CarModel;
using game::CarModelLoads;
using game::CarParts;
using game::CarPartsLoads;
using game::CarSceneRecord;
using game::CockpitLoads;
using game::CockpitObject;
using game::DeferredRelease;
using game::InteriorObject;
using game::LoadSlot;
using game::Matrix;
using game::PartModel;
using game::PartPaint;
using game::Pool4Object;
using game::PoolNode;
using game::PoolNodeList;
using game::ScenePoolObjectHeader;
using game::ScenePoolSet;
using game::TexturePair;
using game::TrafficCarObject;
using game::TrafficLoads;
using game::Vector;
using game::Wheels;
using game::WheelsLoads;
using game::XPR;
using game::XPRList;
using guest::At;
using guest::Guest;

bool On() { return SBX_GET(native_scene_loaders); }

// --log_scene_loaders: each call of this file's functions (natively or
// not), once per distinct line: its depth, name, r3 to r6, the r3 it returns
// and a hash of the 4 KB at r3 after it. Two logs, one with
// --native_scene_loaders and one without, from the same steps differ where
// a native function does something the original doesn't.
class CallLog {
 public:
  CallLog(PPCContext& ctx, uint8_t* base, const char* name)
      : ctx_(ctx), base_(base), name_(name), r3_(ctx.r3.u32), r4_(ctx.r4.u32),
        r5_(ctx.r5.u32), r6_(ctx.r6.u32), on_(SBX_GET(log_scene_loaders)) {
    depth_++;
  }
  ~CallLog() {
    depth_--;
    if (!on_) {
      return;
    }
    uint64_t hash = 0xCBF29CE484222325ull;
    if (r3_ >= 0x10000 && r3_ < 0xFFFF0000u - 4096) {
      for (uint32_t i = 0; i < 4096; i++) {
        hash = (hash ^ base_[r3_ + i]) * 0x100000001B3ull;
      }
    }
    std::string line = fmt::format("{} {} r3={:08X} r4={:08X} r5={:08X} r6={:08X} -> {:08X} {:016X}",
                                   depth_, name_, r3_, r4_, r5_, r6_, ctx_.r3.u32, hash);
    static std::mutex mutex;
    static std::unordered_set<std::string> seen;
    std::lock_guard lock(mutex);
    if (seen.size() < 200000 && seen.insert(line).second) {
      REXLOG_INFO("scene_loaders: {}", line);
    }
  }

 private:
  static inline thread_local int depth_ = 0;
  PPCContext& ctx_;
  uint8_t* base_;
  const char* name_;
  uint32_t r3_, r4_, r5_, r6_;
  bool on_;
};

// Writable: copied into the wheels and pool 4 objects when they're reset.
constexpr uint32_t kResetValue = 0x826B2B20;
// The same word: the scene's first pool set, made by ScenePools::MakeA.
constexpr uint32_t kFirstSet = 0x826B2B20;

// The vtables (read-only image data): the pool nodes', the pool objects'
// base (set before a node is destroyed) and each class's.
constexpr uint32_t kPoolNodeVtable = 0x82069A90;
constexpr uint32_t kPoolObjectVtable = 0x82004F70;
constexpr uint32_t kCarModelVtable = 0x82004F74;
constexpr uint32_t kCockpitVtable = 0x82005000;
constexpr uint32_t kPool4ObjectVtable = 0x82005044;
constexpr uint32_t kCarPartsVtable = 0x82005074;
constexpr uint32_t kInteriorVtable = 0x8200507C;
constexpr uint32_t kTrafficCarVtable = 0x82005094;
constexpr uint32_t kWheelsVtable = 0x820050E0;

constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kPageReadWrite = 4;

// A load slot's state once its file has arrived.
constexpr uint32_t kArrived = LoadSlot::kArrived;

// The recompiled (or other files' native) functions these call.
void ConstructSkeleton(Guest& g, uint8_t* skeleton) { g.Call(sub_8257EEF0, skeleton); }
void DestroySkeleton(Guest& g, uint8_t* skeleton) { g.Call(sub_8257F180, skeleton); }
void ConstructXPR(Guest& g, XPR* xpr) { g.Call(sub_8257C780, xpr); }
void DestroyXPR(Guest& g, XPR* xpr) { g.Call(sub_8257CA60, xpr); }
void ConstructList(Guest& g, XPRList* list) { g.Call(sub_8257A150, list, -1); }  // Reserved size: 64.
void DestroyList(Guest& g, XPRList* list) { g.Call(sub_82579948, list); }
void ConstructPart(Guest& g, PartModel* part) { g.Call(sub_820CE6E8, part); }
void DestroyPart(Guest& g, PartModel* part) { g.Call(sub_820CE750, part); }
void ReleaseModel(Guest& g, PartModel* part) { g.Call(sub_8257A270, part); }  // Model::Release
void ClearPart(Guest& g, PartModel* part) { g.Call(sub_820CE788, part); }      // PartModel::Clear
void ResetSlot(LoadSlot* slot) { game::LoadSlotReset(slot); }
void FreeSlot(Guest& g, LoadSlot* slot) { game::LoadSlotReleaseBuffer(g, slot); }

void XprRelease(Guest& g, XPR* xpr) { g.Call(sub_8257C9C0, xpr); }
void FreeNodeValues(Guest& g, uint8_t* skeleton) { g.Call(sub_8257EF38, skeleton); }
// The texture provider's reset (its vtable's slot 2).
void ResetProvider(Guest& g, XPRList* list) { g.CallVirtual(list, 2); }
void OperatorDelete(Guest& g, uint32_t object) { g.Call(sub_8258C868, object); }
uint32_t AllocPages(Guest& g, uint32_t size) {
  return g.Call(sub_82292DD8, 0u, size, kMemCommit, kPageReadWrite);  // VirtualAlloc
}
uint32_t FirstSetWord() { return *At<guest::u32_be>(kFirstSet); }

// A part slot's model released and cleared.
void ReleasePart(Guest& g, LoadSlot* slot, PartModel* part) {
  ResetSlot(slot);
  ReleaseModel(g, part);
  ClearPart(g, part);
}

template <size_t N>
void ConstructParts(Guest& g, PartModel (&parts)[N]) {
  for (PartModel& part : parts) {
    ConstructPart(g, &part);
  }
}

template <size_t N>
void DestroyParts(Guest& g, PartModel (&parts)[N]) {
  for (int i = int(N) - 1; i >= 0; i--) {
    DestroyPart(g, &parts[i]);
  }
}

// The skeleton, two XPRs and two XPR lists every model's loads start with.
void ConstructModelParts(Guest& g, uint8_t* skeleton, XPR (&xprs)[2], XPRList (&lists)[2]) {
  ConstructSkeleton(g, skeleton);
  ConstructXPR(g, &xprs[0]);
  ConstructXPR(g, &xprs[1]);
  ConstructList(g, &lists[0]);
  ConstructList(g, &lists[1]);
}

void DestroyModelParts(Guest& g, uint8_t* skeleton, XPR (&xprs)[2], XPRList (&lists)[2]) {
  DestroyList(g, &lists[1]);
  DestroyList(g, &lists[0]);
  DestroyXPR(g, &xprs[1]);
  DestroyXPR(g, &xprs[0]);
  DestroySkeleton(g, skeleton);
}

}  // namespace

#define SBX_SCENE_LOADERS_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                   \
  CallLog call_log(ctx, base, #name);    \
  if (!On()) {                           \
    __imp__##name(ctx, base);            \
    return;                              \
  }                                      \
  Guest g(ctx, base);

// --- The loads' constructors and destructors ---

namespace {

// CarModelLoads::CarModelLoads.
void ConstructCarModelLoads(Guest& g, CarModelLoads* loads) {
  ConstructModelParts(g, loads->skeleton, loads->xprs, loads->texture_providers);
}

// CarModelLoads::~CarModelLoads.
void DestroyCarModelLoads(Guest& g, CarModelLoads* loads) {
  DestroyModelParts(g, loads->skeleton, loads->xprs, loads->texture_providers);
}

void ConstructCockpitLoads(Guest& g, CockpitLoads* loads) {
  ConstructCarModelLoads(g, &loads->model);
  ConstructParts(g, loads->parts);
}

void DestroyCockpitLoads(Guest& g, CockpitLoads* loads) {
  DestroyParts(g, loads->parts);
  DestroyCarModelLoads(g, &loads->model);
}

void ConstructTrafficLoads(Guest& g, TrafficLoads* loads) {
  ConstructModelParts(g, loads->skeleton, loads->xprs, loads->lists);
  ConstructParts(g, loads->parts);
}

void DestroyTrafficLoads(Guest& g, TrafficLoads* loads) {
  DestroyParts(g, loads->parts);
  DestroyModelParts(g, loads->skeleton, loads->xprs, loads->lists);
}

void ConstructWheelsLoads(Guest& g, WheelsLoads* loads) {
  ConstructParts(g, loads->parts_48);
  ConstructParts(g, loads->parts_1024);
  ConstructXPR(g, &loads->xprs[0]);
  ConstructXPR(g, &loads->xprs[1]);
  ConstructList(g, &loads->lists[0]);
  ConstructList(g, &loads->lists[1]);
  ConstructParts(g, loads->parts_2144);
}

void DestroyWheelsLoads(Guest& g, WheelsLoads* loads) {
  DestroyParts(g, loads->parts_2144);
  DestroyList(g, &loads->lists[1]);
  DestroyList(g, &loads->lists[0]);
  DestroyXPR(g, &loads->xprs[1]);
  DestroyXPR(g, &loads->xprs[0]);
  DestroyParts(g, loads->parts_1024);
  DestroyParts(g, loads->parts_48);
}

void ConstructCarPartsLoads(Guest& g, CarPartsLoads* loads) {
  ConstructParts(g, loads->parts);
  for (Vector& point : loads->points) {
    point.w = 1.0f;
  }
}

void DestroyCarPartsLoads(Guest& g, CarPartsLoads* loads) { DestroyParts(g, loads->parts); }

// --- The loads' resets and releases ---

// CarParts::Release: each part's slot freed and its model released and
// cleared.
void ReleaseCarParts(Guest& g, CarParts* parts) {
  CarPartsLoads& loads = parts->loads;
  for (int i = 0; i < CarPartsLoads::kParts; i++) {
    FreeSlot(g, &loads.slots[i]);
    ReleaseModel(g, &loads.parts[i]);
    ClearPart(g, &loads.parts[i]);
  }
}

void ResetWheelsLoads(Guest& g, WheelsLoads* loads);

// Wheels::Reset: no car model, the first set's word, the record's defaults
// and the loads' slots and models reset.
void ResetWheels(Guest& g, Wheels* wheels) {
  wheels->head.car_model = nullptr;
  wheels->first_set.address = *At<guest::u32_be>(kResetValue);
  game::WheelsRecordReset(wheels->record);
  ResetWheelsLoads(g, &wheels->loads);
}

// Wheels::Release: their slots freed, then Wheels::Reset.
void ReleaseWheels(Guest& g, Wheels* wheels) {
  WheelsLoads& loads = wheels->loads;
  for (int i = 0; i < 2; i++) {
    FreeSlot(g, &loads.slots_0[i]);
    FreeSlot(g, &loads.slots_976[i]);
    FreeSlot(g, &loads.slots_1952[i]);
    FreeSlot(g, &loads.slots_2088[i]);
  }
  ResetWheels(g, wheels);
}

// Pool4Object::Reset.
void ResetPool4Object(Guest& g, Pool4Object* object) {
  object->head.car_model = nullptr;
  game::Pool4RecordReset(object->record);
  ResetSlot(&object->slot);
  ReleaseModel(g, &object->model);
  ClearPart(g, &object->model);
  object->first_set.address = *At<guest::u32_be>(kResetValue);
}

// Pool4Object::Release: its slot freed and model released, then
// Pool4Object::Reset.
void ReleasePool4Object(Guest& g, Pool4Object* object) {
  FreeSlot(g, &object->slot);
  ReleaseModel(g, &object->model);
  ResetPool4Object(g, object);
}

// XPR::Count: its count (word 5), or 0 without a header (word 4).
uint32_t XprCount(const XPR* xpr) { return xpr->words[4] == 0 ? 0 : uint32_t(xpr->words[5]); }

}  // namespace

namespace game {

// Pool4Object::SetRecord: the record (52 bytes) copied in.
void Pool4ObjectSetRecord(Guest& g, Pool4Object* object, uint32_t record) {
  g.Call(sub_8258CB90, &object->record, record, uint32_t(sizeof(Pool4Record)));  // memcpy
}

// CarModel::IsLoaded: false while either of its two bodies is loading;
// otherwise whether its node file isn't.
bool CarModelIsLoaded(const CarModel* model) {
  for (const LoadSlot& slot : model->loads.model.body_slots) {
    if (slot.state == LoadSlot::kLoading) {
      return false;
    }
  }
  return model->loads.model.node_slot.state != LoadSlot::kLoading;
}

// Pool4Object::IsLoaded: whether its load isn't loading.
bool Pool4ObjectIsLoaded(const Pool4Object* object) {
  return object->slot.state != LoadSlot::kLoading;
}

}  // namespace game

// CarModelLoads::CarModelLoads: r3 the loads. Returns them.
extern "C" REX_FUNC(sub_820D3A80) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D3A80)
  CarModelLoads* loads = At<CarModelLoads>(ctx.r3.u32);
  ConstructCarModelLoads(g, loads);
  ctx.r3.u64 = guest::AddressOf(loads);
}

// CarModelLoads::~CarModelLoads: r3 the loads.
extern "C" REX_FUNC(sub_820D3AE0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D3AE0)
  DestroyCarModelLoads(g, At<CarModelLoads>(ctx.r3.u32));
}

// CockpitLoads::CockpitLoads: r3 the loads. Returns them.
extern "C" REX_FUNC(sub_820D5CB8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D5CB8)
  CockpitLoads* loads = At<CockpitLoads>(ctx.r3.u32);
  ConstructCockpitLoads(g, loads);
  ctx.r3.u64 = guest::AddressOf(loads);
}

// CockpitLoads::~CockpitLoads: r3 the loads.
extern "C" REX_FUNC(sub_820D5D38) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D5D38)
  DestroyCockpitLoads(g, At<CockpitLoads>(ctx.r3.u32));
}

// TrafficLoads::TrafficLoads: r3 the loads. Returns them.
extern "C" REX_FUNC(sub_820D78F0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D78F0)
  TrafficLoads* loads = At<TrafficLoads>(ctx.r3.u32);
  ConstructTrafficLoads(g, loads);
  ctx.r3.u64 = guest::AddressOf(loads);
}

// TrafficLoads::~TrafficLoads: r3 the loads.
extern "C" REX_FUNC(sub_820D7970) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7970)
  DestroyTrafficLoads(g, At<TrafficLoads>(ctx.r3.u32));
}

// WheelsLoads::WheelsLoads: r3 the loads. Returns them.
extern "C" REX_FUNC(sub_820D8C28) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D8C28)
  WheelsLoads* loads = At<WheelsLoads>(ctx.r3.u32);
  ConstructWheelsLoads(g, loads);
  ctx.r3.u64 = guest::AddressOf(loads);
}

// WheelsLoads::~WheelsLoads: r3 the loads.
extern "C" REX_FUNC(sub_820D8CD8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D8CD8)
  DestroyWheelsLoads(g, At<WheelsLoads>(ctx.r3.u32));
}

// CarPartsLoads::CarPartsLoads: r3 the loads. Returns them.
extern "C" REX_FUNC(sub_820D6C38) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6C38)
  CarPartsLoads* loads = At<CarPartsLoads>(ctx.r3.u32);
  ConstructCarPartsLoads(g, loads);
  ctx.r3.u64 = guest::AddressOf(loads);
}

// CarPartsLoads::~CarPartsLoads: r3 the loads.
extern "C" REX_FUNC(sub_820D6C98) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6C98)
  DestroyCarPartsLoads(g, At<CarPartsLoads>(ctx.r3.u32));
}

// CarParts::Release: r3 the parts.
extern "C" REX_FUNC(sub_820D69B0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D69B0)
  ReleaseCarParts(g, At<CarParts>(ctx.r3.u32));
}

// Wheels::Reset and Wheels::Release: r3 the wheels.
extern "C" REX_FUNC(sub_820D8810) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D8810)
  ResetWheels(g, At<Wheels>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D8B30) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D8B30)
  ReleaseWheels(g, At<Wheels>(ctx.r3.u32));
}

// Pool4Object::Reset and Pool4Object::Release: r3 the object.
extern "C" REX_FUNC(sub_820D65F8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D65F8)
  ResetPool4Object(g, At<Pool4Object>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D66D0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D66D0)
  ReleasePool4Object(g, At<Pool4Object>(ctx.r3.u32));
}

// Pool4Object::SetRecord: r3 the object, r4 the record.
extern "C" REX_FUNC(sub_820D6710) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6710)
  game::Pool4ObjectSetRecord(g, At<Pool4Object>(ctx.r3.u32), ctx.r4.u32);
}

// CarModel::IsLoaded: r3 the car model; 1 or 0.
extern "C" REX_FUNC(sub_820D38C8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D38C8)
  ctx.r3.u64 = game::CarModelIsLoaded(At<CarModel>(ctx.r3.u32)) ? 1 : 0;
}

// Pool4Object::IsLoaded: r3 the object; 1 or 0.
extern "C" REX_FUNC(sub_820D66B8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D66B8)
  ctx.r3.u64 = game::Pool4ObjectIsLoaded(At<Pool4Object>(ctx.r3.u32)) ? 1 : 0;
}

// XPR::Count: r3 the XPR.
extern "C" REX_FUNC(sub_8257C838) {
  SBX_SCENE_LOADERS_FALLBACK(sub_8257C838)
  ctx.r3.u64 = XprCount(At<XPR>(ctx.r3.u32));
}
// --- The scene pools' nodes and objects ---

namespace {

// PoolNode::PoolNode: the node appended to its list.
void ConstructNode(PoolNode* node, PoolNodeList* list) {
  node->list = list;
  node->next = nullptr;
  node->vtable = kPoolNodeVtable;
  node->previous = nullptr;
  if (list->first == nullptr) {
    list->first = node;
    list->last = node;
  } else {
    list->last->next = node;
    node->previous = list->last;
    list->last = node;
  }
  list->count += 1;
}

// PoolNode::~PoolNode: the node taken off its list, if it's on one.
void DestroyNode(PoolNode* node) {
  node->vtable = kPoolNodeVtable;
  PoolNodeList* list = node->list;
  if (list == nullptr) {
    return;
  }
  PoolNode* previous = node->previous;
  PoolNode* next = node->next;
  if (previous == nullptr) {
    list->first = next;
    if (next == nullptr) {
      list->last = nullptr;
    } else {
      next->previous = nullptr;
    }
  } else {
    if (next == nullptr) {
      list->last = previous;
    } else {
      next->previous = previous;
    }
    previous->next = next;
  }
  list->count -= 1;
}

// A pool object's start: on the pool's list, its class's vtable, not
// taken, no index yet.
void ConstructHeader(ScenePoolObjectHeader* header, PoolNodeList* list, uint32_t vtable) {
  ConstructNode(&header->node, list);
  header->state = 0;
  header->node.vtable = vtable;
  header->index = -1;
}

// A pool object's end: the base class's vtable, then off its list.
void DestroyHeader(ScenePoolObjectHeader* header) {
  header->node.vtable = kPoolObjectVtable;
  DestroyNode(&header->node);
}

// A matrix's rows' w and a point's w (the rest is set by the resets).
void SetRowsW(Matrix& m, float w) {
  m.right.w = w;
  m.up.w = w;
  m.forward.w = w;
  m.position.w = w;
}

// The accessory anchor's reset: (0, 0, 0, 1).
void ResetAnchor(Vector& v) {
  v.x = 0.0f;
  v.y = 0.0f;
  v.z = 0.0f;
  v.w = 1.0f;
}

// --- Car models (pool 0) ---

void ResetCarModel(Guest& g, CarModel* model);
void ResetCarBodyLoads(Guest& g, game::CarBodyLoads* loads);

void ConstructCarModel(Guest& g, CarModel* model, PoolNodeList* list) {
  ConstructHeader(&model->header, list, kCarModelVtable);
  game::CarRecordSetW(model->record);  // CarRecord::CarRecord
  ConstructCarModelLoads(g, &model->loads.model);
  for (Matrix& m : model->tire_matrices) {
    SetRowsW(m, 1.0f);
  }
  for (Vector& v : model->tire_positions) {
    v.w = 1.0f;
  }
  for (Matrix& m : model->seat_matrices) {
    SetRowsW(m, 1.0f);
  }
  for (Vector& v : model->seat_positions) {
    v.w = 1.0f;
  }
  model->accessory_node.w = 1.0f;
  ResetCarModel(g, model);
}

// CarModel::Reset: the first set's word, the car record and the loads
// reset; the tires' and seats' matrices the identity (copied with memcpy
// from one built in the original's 304-byte frame, at +96) and their
// positions (0, 0, 0, 1); no name lookups; no accessory matrix, its anchor
// (0, 0, 0, 1).
void ResetCarModel(Guest& g, CarModel* model) {
  guest::StackFrame frame(g.ctx, 304);
  Matrix* identity = At<Matrix>(frame.Local(96));
  auto copy_identity = [&](Matrix& m) {
    identity->right = {1.0f, 0.0f, 0.0f, 0.0f};
    identity->up = {0.0f, 1.0f, 0.0f, 0.0f};
    identity->forward = {0.0f, 0.0f, 1.0f, 0.0f};
    identity->position = {0.0f, 0.0f, 0.0f, 1.0f};
    g.Call(sub_8258CB90, &m, identity, uint32_t(sizeof(Matrix)));  // memcpy
  };
  model->first_set.address = FirstSetWord();
  game::CarRecordReset(model->record);
  ResetCarBodyLoads(g, &model->loads);
  for (Matrix& m : model->tire_matrices) {
    copy_identity(m);
  }
  for (Vector& v : model->tire_positions) {
    ResetAnchor(v);
  }
  for (int i = 0; i < 2; i++) {
    copy_identity(model->seat_matrices[i]);
    ResetAnchor(model->seat_positions[i]);
  }
  for (guest::i32_be& name : model->interior_textures) {
    name = -1;
  }
  model->accessory_matrix = 0;
  ResetAnchor(model->accessory_node);
}

void DestroyCarModel(Guest& g, CarModel* model) {
  model->header.node.vtable = kCarModelVtable;
  game::CarModelRelease(g, model);
  DestroyCarModelLoads(g, &model->loads.model);
  DestroyHeader(&model->header);
}

// CarModel::SelectedTexture: the body XPR's interior texture the car
// record's choice picks, or 0 for none.
uint32_t SelectedTexture(Guest& g, CarModel* model) {
  const int32_t choice = int32_t(uint32_t(model->record.unknown_3676[0]));
  const int32_t name = *guest::Element<guest::i32_be>(guest::AddressOf(model->interior_textures),
                                                      uint32_t(choice));
  if (name == -1) {
    return 0;
  }
  return g.Call(sub_8257CA30, &model->loads.model.xprs[0], name);  // XPR::GetTexture
}

// --- Car parts (pool 1) ---

void ConstructCarParts(Guest& g, CarParts* parts, PoolNodeList* list) {
  ConstructHeader(&parts->head.header, list, kCarPartsVtable);
  ConstructCarPartsLoads(g, &parts->loads);
  parts->head.car_model = nullptr;
  for (int i = 0; i < CarPartsLoads::kParts; i++) {
    ReleasePart(g, &parts->loads.slots[i], &parts->loads.parts[i]);
  }
  game::PartsRecordReset(parts->record);
  parts->first_set.address = FirstSetWord();
}

void DestroyCarParts(Guest& g, CarParts* parts) {
  parts->head.header.node.vtable = kCarPartsVtable;
  ReleaseCarParts(g, parts);
  DestroyCarPartsLoads(g, &parts->loads);
  DestroyHeader(&parts->head.header);
}

bool Arrived(const LoadSlot& slot) { return slot.state == kArrived; }

// CarParts::IsLoaded: none of its parts loading (set a all 216, set b the
// first 17; other sets never).
bool CarPartsLoaded(CarParts* parts) {
  const uint32_t set = parts->first_set->model_set;
  if (set > 1) {
    return false;
  }
  const int count = set == 0 ? CarPartsLoads::kParts : 17;
  for (int i = 0; i < count; i++) {
    if (parts->loads.slots[i].state == LoadSlot::kLoading) {
      return false;
    }
  }
  return true;
}

// CarParts::SlotOf: which part slot a kind (and for kinds 1, 8 and 9 a
// choice) of set a's parts is, counted from `base`.
uint32_t PartSlot(uint32_t kind, uint32_t base, uint32_t choice) {
  switch (kind) {
    case 0: return base;
    case 1: return base + 13 + 7 * choice;
    case 2: return base + 104;
    case 3: return base + 117;
    case 4: return base + 130;
    case 5: return base + 143;
    case 6: return base + 151;
    case 7: return base + 154;
    case 8: return base + 157 + 4 * choice;
    case 9: return base + 169 + 4 * choice;
    case 10: return base + 181;
    case 11: return base + 185;
    case 12: return base + 190;
    case 13: return 189;
    case 14: return base + 196;
    default: return base + 211;  // 15
  }
}

// CarParts::Part: the part model of a kind once it has arrived, else 0. Set
// a has no kind 16 (and none past it here); set b's slots are the kinds,
// without 7, 9 and 14.
uint32_t CarPart(CarParts* parts, int32_t kind, uint32_t base, uint32_t choice) {
  uint32_t slot;
  switch (uint32_t(parts->first_set->model_set)) {
    case 0:
      if (kind == 16 || uint32_t(kind) > 15) {
        return 0;
      }
      slot = PartSlot(uint32_t(kind), base, choice);
      break;
    case 1:
      if (kind == 7 || kind == 9 || kind == 14) {
        return 0;
      }
      slot = uint32_t(kind);
      break;
    default:
      return 0;
  }
  if (!Arrived(*guest::Element<LoadSlot>(guest::AddressOf(parts->loads.slots), slot))) {
    return 0;
  }
  return guest::AddressOf(guest::Element<PartModel>(guest::AddressOf(parts->loads.parts), slot));
}

// The materials of each of a part model's shapes looked up (the answers
// aren't used).
void PartWalkMaterials(Guest& g, PartModel* part) {
  if (part == nullptr) {
    return;
  }
  const int32_t shapes = int32_t(uint32_t(part->model.unknown_404[2]));
  for (int32_t shape = 0; shape < shapes; shape++) {
    const int32_t first = int32_t(g.Call(sub_8257A3D8, part, shape, 0));  // Model::MaterialCount
    const int32_t second = int32_t(g.Call(sub_8257A3D8, part, shape, 1));
    for (int32_t m = 0; m < first; m++) {
      g.Call(sub_8257A420, part, shape, m, 0);  // Model::Material
    }
    for (int32_t m = 0; m < second; m++) {
      g.Call(sub_8257A420, part, shape, m, 1);
    }
  }
}

// PartModel::ApplyPalette on each arrived part.
void ApplyPalettes(Guest& g, LoadSlot* slots, PartModel* parts, int count) {
  for (int i = 0; i < count; i++) {
    if (Arrived(slots[i])) {
      g.Call(sub_820D2AF0, &parts[i]);
    }
  }
}

// The paint (PartPaint: six 64-bit words, kept whole) as a SetPaint walk
// gets it, in r4 to r9.
PartPaint PaintArguments(PPCContext& ctx) {
  return {{ctx.r4.u64, ctx.r5.u64, ctx.r6.u64, ctx.r7.u64, ctx.r8.u64, ctx.r9.u64}};
}

// PartModel::SetPaint on each arrived part, the paint's six words handed
// on whole.
void SetPaints(Guest& g, LoadSlot* slots, PartModel* parts, int count, const PartPaint& p) {
  for (int i = 0; i < count; i++) {
    if (Arrived(slots[i])) {
      g.Call(sub_820D0AF0, &parts[i], p.words[0], p.words[1], p.words[2], p.words[3], p.words[4],
             p.words[5]);
    }
  }
}

// --- Cockpits (pool 2) ---

// The cockpit's state from its record down, as made and as released.
void ResetCockpitState(CockpitObject* cockpit) {
  game::CockpitRecordReset(cockpit->record);
  cockpit->accessory_matrix = 0;
  ResetAnchor(cockpit->accessory_node);
  cockpit->texture = 0;
}

void ConstructCockpit(Guest& g, CockpitObject* cockpit, PoolNodeList* list) {
  CarModelLoads& model = cockpit->loads.model;
  ConstructHeader(&cockpit->head.header, list, kCockpitVtable);
  ConstructCockpitLoads(g, &cockpit->loads);
  cockpit->accessory_node.w = 1.0f;
  cockpit->head.car_model = nullptr;
  cockpit->first_set.address = FirstSetWord();
  FreeNodeValues(g, model.skeleton);
  ResetCockpitState(cockpit);
  ResetSlot(&model.node_slot);
  for (int k = 0; k < 2; k++) {
    ResetSlot(&model.body_slots[k]);
    XprRelease(g, &model.xprs[k]);
    ResetProvider(g, &model.texture_providers[k]);
  }
  for (int i = 0; i < 6; i++) {
    ReleasePart(g, &cockpit->loads.part_slots[i], &cockpit->loads.parts[i]);
  }
}

// Cockpit::Release: its buffers freed and slots reset, its XPRs and
// providers released, its record back to its defaults. Its part models
// aren't released here.
void ReleaseCockpit(Guest& g, CockpitObject* cockpit) {
  CarModelLoads& model = cockpit->loads.model;
  FreeSlot(g, &model.node_slot);
  ResetSlot(&model.node_slot);
  FreeNodeValues(g, model.skeleton);
  for (int k = 0; k < 2; k++) {
    FreeSlot(g, &model.body_slots[k]);
    ResetSlot(&model.body_slots[k]);
    XprRelease(g, &model.xprs[k]);
    ResetProvider(g, &model.texture_providers[k]);
  }
  cockpit->first_set = nullptr;
  cockpit->head.car_model = nullptr;
  ResetCockpitState(cockpit);
  for (LoadSlot& slot : cockpit->loads.part_slots) {
    FreeSlot(g, &slot);
    ResetSlot(&slot);
  }
}

void DestroyCockpit(Guest& g, CockpitObject* cockpit) {
  cockpit->head.header.node.vtable = kCockpitVtable;
  ReleaseCockpit(g, cockpit);
  DestroyCockpitLoads(g, &cockpit->loads);
  DestroyHeader(&cockpit->head.header);
}

// --- Interiors (pool 3) ---

void ConstructInterior(Guest& g, InteriorObject* interior, PoolNodeList* list) {
  ConstructHeader(&interior->head.header, list, kInteriorVtable);
  ConstructParts(g, interior->parts);
  interior->head.car_model = nullptr;
  for (int i = 0; i < 6; i++) {
    ReleasePart(g, &interior->part_slots[i], &interior->parts[i]);
  }
  game::InteriorRecordReset(interior->record);
  interior->in_cockpit = 0;
  interior->first_set.address = FirstSetWord();
}

uint32_t AccessoriesSet() { return game::AccessoriesGet(); }

// Interior::Release: its parts' buffers freed and models released; set a
// gives back all three accessories, set b the one its record picked.
void ReleaseInterior(Guest& g, InteriorObject* interior) {
  for (int i = 0; i < 6; i++) {
    FreeSlot(g, &interior->part_slots[i]);
    ReleaseModel(g, &interior->parts[i]);
    ClearPart(g, &interior->parts[i]);
  }
  const uint32_t set = interior->first_set->model_set;
  if (set == 0) {
    for (uint32_t i = 0; i < 3; i++) {
      g.Call(sub_820F0920, AccessoriesSet(), i);  // Accessories::Release
    }
  } else if (set == 1) {
    const int32_t accessory = interior->record.accessory;
    g.Call(sub_820F0920, AccessoriesSet(), accessory);
  }
  interior->in_cockpit = 0;
}

void DestroyInterior(Guest& g, InteriorObject* interior) {
  interior->head.header.node.vtable = kInteriorVtable;
  ReleaseInterior(g, interior);
  DestroyParts(g, interior->parts);
  DestroyHeader(&interior->head.header);
}

// Accessories::AllLoaded: each of the set's entries there has its
// accessory loaded (its +1048).
bool AccessoriesLoaded(Accessories* set) {
  for (const auto& entry : set->entries) {
    const AccessoryEntry* e = entry;
    if (e != nullptr && e->accessory.unknown_1048 == 0) {
      return false;
    }
  }
  return true;
}

// Interior::IsLoaded: sets a and b, none of its parts loading; and the
// accessories all loaded.
bool InteriorLoaded(Guest& g, InteriorObject* interior) {
  bool loaded = interior->first_set->model_set <= 1;
  if (loaded) {
    for (const LoadSlot& slot : interior->part_slots) {
      if (slot.state == LoadSlot::kLoading) {
        loaded = false;
        break;
      }
    }
  }
  if (!AccessoriesLoaded(At<Accessories>(AccessoriesSet()))) {
    loaded = false;
  }
  return loaded;
}

// --- Pool 4 objects ---

void ConstructPool4Object(Guest& g, Pool4Object* object, PoolNodeList* list) {
  ConstructHeader(&object->head.header, list, kPool4ObjectVtable);
  ConstructPart(g, &object->model);
  ResetPool4Object(g, object);
}

void DestroyPool4Object(Guest& g, Pool4Object* object) {
  object->head.header.node.vtable = kPool4ObjectVtable;
  FreeSlot(g, &object->slot);
  ReleaseModel(g, &object->model);
  ResetPool4Object(g, object);
  DestroyPart(g, &object->model);
  DestroyHeader(&object->head.header);
}

// --- Wheels (pool 5) ---

void ConstructWheels(Guest& g, Wheels* wheels, PoolNodeList* list) {
  ConstructHeader(&wheels->head.header, list, kWheelsVtable);
  game::Record12Reset(wheels->record.unknown_44);
  game::Record12Reset(wheels->record.unknown_56);
  ConstructWheelsLoads(g, &wheels->loads);
  ResetWheels(g, wheels);
}

void DestroyWheels(Guest& g, Wheels* wheels) {
  wheels->head.header.node.vtable = kWheelsVtable;
  ReleaseWheels(g, wheels);
  DestroyWheelsLoads(g, &wheels->loads);
  DestroyHeader(&wheels->head.header);
}

// WheelsLoads::Reset: for each of the two sides, its slots reset, its
// models released and cleared, its XPR released and its provider reset.
void ResetWheelsLoads(Guest& g, WheelsLoads* loads) {
  for (int k = 0; k < 2; k++) {
    ResetSlot(&loads->slots_0[k]);
    ReleaseModel(g, &loads->parts_48[k]);
    ClearPart(g, &loads->parts_48[k]);
    ReleasePart(g, &loads->slots_976[k], &loads->parts_1024[k]);
    ResetSlot(&loads->slots_1952[k]);
    XprRelease(g, &loads->xprs[k]);
    ResetProvider(g, &loads->lists[k]);
    ResetSlot(&loads->slots_2088[k]);
    ReleaseModel(g, &loads->parts_2144[k]);
    ClearPart(g, &loads->parts_2144[k]);
  }
}

// --- Traffic cars (pool 6) ---

// TrafficCar::Reset.
void ResetTrafficCar(Guest& g, TrafficCarObject* car) {
  TrafficLoads& loads = car->loads;
  car->first_set.address = FirstSetWord();
  game::CarRecordReset(car->record);
  car->body_texture = -1;
  ResetSlot(&loads.head.node_slot);
  FreeNodeValues(g, loads.skeleton);
  for (int k = 0; k < 2; k++) {
    ResetSlot(&loads.xpr_slots[k]);
    XprRelease(g, &loads.xprs[k]);
    ResetProvider(g, &loads.lists[k]);
  }
  loads.head.sheet = {};
  car->plate = {};
  for (int i = 0; i < 6; i++) {
    ReleasePart(g, &loads.part_slots[i], &loads.parts[i]);
  }
}

void ConstructTrafficCar(Guest& g, TrafficCarObject* car, PoolNodeList* list) {
  ConstructHeader(&car->head.header, list, kTrafficCarVtable);
  game::CarRecordSetW(car->record);  // CarRecord::CarRecord
  ConstructTrafficLoads(g, &car->loads);
  for (Vector& v : car->tire_positions) {
    v.w = 1.0f;
  }
  for (Matrix& m : car->tire_matrices) {
    SetRowsW(m, 1.0f);
  }
  ResetTrafficCar(g, car);
}

// TrafficCar::Release: its sheet's and plate's textures put off for
// release (DeferRelease), its node file's buffer freed, then
// TrafficCar::Reset.
void ReleaseTrafficCar(Guest& g, TrafficCarObject* car) {
  game::DeferRelease(car->first_set, car->loads.head.sheet);
  game::DeferRelease(car->first_set, car->plate);
  FreeSlot(g, &car->loads.head.node_slot);
  ResetTrafficCar(g, car);
}

void DestroyTrafficCar(Guest& g, TrafficCarObject* car) {
  car->head.header.node.vtable = kTrafficCarVtable;
  ReleaseTrafficCar(g, car);
  DestroyTrafficLoads(g, &car->loads);
  DestroyHeader(&car->head.header);
}

// --- The car model's loads and the interior's parts ---

// CarModelLoads::Reset: the slots reset, the skeleton's node values freed,
// the XPRs released and providers reset, the decals' state cleared and the
// first three layers' and six stickers' slots reset, the sheet's first
// texture and both plate textures forgotten. Buffers aren't freed here.
void ResetCarBodyLoads(Guest& g, game::CarBodyLoads* loads) {
  CarModelLoads& model = loads->model;
  ResetSlot(&model.node_slot);
  FreeNodeValues(g, model.skeleton);
  for (int k = 0; k < 2; k++) {
    ResetSlot(&model.body_slots[k]);
    XprRelease(g, &model.xprs[k]);
    ResetProvider(g, &model.texture_providers[k]);
  }
  loads->decals_state = 0;
  for (int k = 0; k < 3; k++) {
    ResetSlot(&loads->layers[k].load);
    ResetSlot(&loads->layers[k].unknown_24);
  }
  for (int k = 0; k < 6; k++) {
    ResetSlot(&loads->stickers[k].load);
    ResetSlot(&loads->stickers[k].unknown_24);
  }
  loads->sheet.header = 0;
  loads->sheet.data = 0;
  loads->plate = {};
}

// Interior::OnPartLoaded: the part whose slot asked for this request built
// from its buffer (Model::Load), the buffer freed, the slot arrived.
void InteriorPartLoaded(Guest& g, InteriorObject* interior, uint32_t request) {
  for (int i = 0; i < 6; i++) {
    LoadSlot& slot = interior->part_slots[i];
    if (uint32_t(int32_t(slot.unknown_4)) != request) {
      continue;
    }
    g.Call(sub_8257C238, &interior->parts[i], slot.block, 0u);  // Model::Load
    FreeSlot(g, &slot);
    slot.state = kArrived;
    return;
  }
}

// --- The first pool set ---

// ScenePools::CreateA: the set's counts from the sizes, the deferred
// releases' records (four for each car model and traffic car), then each
// pool's objects in pages of their own, each made on its pool's list with
// its index.
void CreatePools(Guest& g, ScenePoolSet* set, const CarSceneRecord* sizes) {
  set->unknown_4 = 3;
  set->model_set = sizes->model_set;
  set->car_models = sizes->car_models;
  set->car_parts = sizes->car_parts;
  set->interiors = sizes->interiors + sizes->cockpits + sizes->traffic_cars;
  set->cockpits = sizes->cockpits;
  set->pool4_objects = sizes->pool4_objects;
  set->wheels = sizes->wheels + sizes->traffic_cars + sizes->unknown_28;
  set->unknown_36 = sizes->traffic_cars + sizes->unknown_28;
  const int32_t car_models = set->car_models;
  set->traffic_cars = sizes->traffic_cars;
  set->releases.address = AllocPages(g, uint32_t(car_models + sizes->traffic_cars) * 80);
  for (int32_t i = 0; i < (set->car_models + set->traffic_cars) * 4; i++) {
    guest::Element<DeferredRelease>(set->releases.address, uint32_t(i))->state = 0;
  }

  struct Pool {
    int32_t count;
    uint32_t size;
  };
  // Each pool's count and object size.
  const Pool pools[7] = {
      {set->car_models, 7264},     {set->car_parts, 106016}, {set->cockpits, 3520},
      {set->interiors, 3104},      {set->pool4_objects, 592}, {set->wheels, 3280},
      {set->traffic_cars, 7408},
  };
  for (uint32_t p = 0; p < 7; p++) {
    PoolNodeList* list = &set->lists[p];
    for (int32_t j = 0; j < pools[p].count; j++) {
      const uint32_t object = AllocPages(g, pools[p].size);
      if (object == 0) {
        continue;  // (The original would write the index at guest address 20.)
      }
      switch (p) {
        case 0: ConstructCarModel(g, At<CarModel>(object), list); break;
        case 1: ConstructCarParts(g, At<CarParts>(object), list); break;
        case 2: ConstructCockpit(g, At<CockpitObject>(object), list); break;
        case 3: ConstructInterior(g, At<InteriorObject>(object), list); break;
        case 4: ConstructPool4Object(g, At<Pool4Object>(object), list); break;
        case 5: ConstructWheels(g, At<Wheels>(object), list); break;
        default: ConstructTrafficCar(g, At<TrafficCarObject>(object), list); break;
      }
      At<ScenePoolObjectHeader>(object)->index = j;
    }
  }
}

// --- A model's point by node ---

// ModelPoint::Find's 368-byte frame: a scratch matrix, the identity turned
// a quarter turn about y, and the turn.
struct FindPointFrame {
  uint8_t unknown_0[80];
  Matrix scratch;
  Matrix turned;
  Matrix turn;
  uint8_t unknown_272[96];
};
GUEST_OFFSET(FindPointFrame, scratch, 80);
GUEST_OFFSET(FindPointFrame, turned, 144);
GUEST_OFFSET(FindPointFrame, turn, 208);
GUEST_SIZE(FindPointFrame, 368);

void SetIdentity(Matrix& m) {
  m.right = {1.0f, 0.0f, 0.0f, 0.0f};
  m.up = {0.0f, 1.0f, 0.0f, 0.0f};
  m.forward = {0.0f, 0.0f, 1.0f, 0.0f};
  m.position = {0.0f, 0.0f, 0.0f, 1.0f};
}

bool NameMatches(const char* a, const char* b);
int32_t FindByName(XPR* xpr, const char* name);

// ModelPoint::Find: the record cleared (its matrix the identity); then, for
// a model with a skeleton, the skeleton posed and the node named by the
// record looked for. Found: the record keeps the model, the skeleton and
// the node; its matrix is the node's with its position scaled by 0.1,
// turned a quarter turn about y; its +144 the position, its +80 the
// inverse. Whether found.
bool FindPointByNode(Guest& g, game::PointRecord* record, game::PartModel* model) {
  guest::StackFrame frame(g.ctx, 368);
  FindPointFrame* f = At<FindPointFrame>(frame.Local(0));
  Matrix* matrix = At<Matrix>(guest::AddressOf(&record->points[0]));
  record->unknown_228 = -1;
  record->unknown_0 = 0;
  record->unknown_8 = 0;
  record->unknown_4 = 0;
  SetIdentity(f->scratch);
  g.Call(sub_8258CB90, matrix, &f->scratch, uint32_t(sizeof(Matrix)));  // memcpy
  if (model->model.unknown_404[1] == 0) {
    return false;  // No skeleton.
  }
  SetIdentity(f->scratch);
  f->turned = f->scratch;
  g.ctx.f1.f64 = double(-1.57079637f);  // The float at 0x82002A2C: -pi/2.
  g.Call(sub_820A68E8, &f->turn);                         // Matrix::RotateY
  g.Call(sub_8209BE70, &f->turned, &f->turned, &f->turn);  // Matrix::Multiply

  game::Skeleton* skeleton = At<game::Skeleton>(guest::AddressOf(model->model.skeleton));
  g.Call(sub_8257F2A8, skeleton);
  g.Call(sub_8257F410, skeleton);
  g.Call(sub_8257F058, skeleton);  // Skeleton::UpdateMatrices
  const int32_t count = int32_t(g.Call(sub_8257F038, skeleton));  // Skeleton::NodeCount
  int32_t node = 0;
  for (; node < count; node++) {
    const char* name = reinterpret_cast<const char*>(skeleton->Node(uint32_t(node)));
    if (NameMatches(name, record->node)) {
      break;
    }
  }
  if (node >= count) {
    return false;
  }

  record->unknown_4 = guest::AddressOf(model);
  record->unknown_0 = guest::AddressOf(skeleton);
  g.Call(sub_820A0E18, matrix, skeleton->MatrixAt(uint32_t(node)));  // Matrix::Copy
  f->scratch.right.w = 1.0f;
  f->scratch.up.w = 1.0f;
  f->scratch.forward.w = 1.0f;
  f->scratch.position.w = 1.0f;
  g.ctx.fpscr.disableFlushMode();  // Float math as the recompiled code's.
  const float scale = 0.1f;  // The float at 0x82001F94.
  matrix->position.x = matrix->position.x * scale;
  matrix->position.y = matrix->position.y * scale;
  matrix->position.z = matrix->position.z * scale;
  g.Call(sub_8209BE70, &f->scratch, matrix, &f->turned);  // Matrix::Multiply
  g.Call(sub_820A0E18, matrix, &f->scratch);              // Matrix::Copy
  record->points[8] = record->points[3];
  g.Call(sub_820A9AD8, &record->points[4], matrix);  // Matrix::Inverse
  record->unknown_224 = node;
  return true;
}

// --- The cockpit's and traffic car's file callbacks ---

// The frame the anchors are turned in (Cockpit::OnFileLoaded's 256 bytes;
// TrafficCar::OnFileLoaded's is larger, the same at its +80 and +144).
struct AnchorFrame {
  uint8_t unknown_0[80];
  Matrix turn;
  Matrix turned;
};
GUEST_OFFSET(AnchorFrame, turn, 80);
GUEST_OFFSET(AnchorFrame, turned, 144);

constexpr char kAccessoryNode[] = "acce_null";  // The string at 0x82004FBC.

// A part's file arrived: the part model built from the slot's buffer
// (Model::Load), the buffer freed, the slot arrived.
void PartArrived(Guest& g, LoadSlot* slot, PartModel* part) {
  g.Call(sub_8257C238, part, slot->block, 0u);  // Model::Load
  FreeSlot(g, slot);
  slot->state = kArrived;
}

// An XPR's file arrived (in `buffer`): the XPR made from it and set in its
// provider's slot 0 (its vtable's slot 4).
void XprArrived(Guest& g, LoadSlot* slot, XPR* xpr, XPRList* provider, uint32_t buffer) {
  g.Call(sub_8257C880, xpr, buffer);  // XPR::Create
  g.CallVirtual(provider, 4, 0u, xpr);
  slot->state = kArrived;
}

// The node file arrived: the skeleton bound to it and posed; the matrix of
// its last node named "acce_null", or 0. Returns that matrix.
uint32_t NodesArrived(Guest& g, LoadSlot* slot, uint8_t* skeleton_bytes) {
  game::Skeleton* skeleton = At<game::Skeleton>(guest::AddressOf(skeleton_bytes));
  g.Call(sub_8257F1D8, skeleton, slot->block, 0u);  // Skeleton::Bind
  g.Call(sub_8257F2A8, skeleton);
  g.Call(sub_8257F410, skeleton);
  g.Call(sub_8257F058, skeleton);  // Skeleton::UpdateMatrices
  slot->state = kArrived;
  return guest::AddressOf(skeleton);
}

uint32_t AccessoryMatrix(Guest& g, game::Skeleton* skeleton, uint32_t found) {
  const int32_t count = int32_t(g.Call(sub_8257F038, skeleton));  // Skeleton::NodeCount
  for (int32_t i = 0; i < count; i++) {
    const char* name = reinterpret_cast<const char*>(skeleton->Node(uint32_t(i)));
    if (std::strcmp(kAccessoryNode, name) == 0) {
      found = guest::AddressOf(skeleton->MatrixAt(uint32_t(i)));
    }
  }
  return found;
}

// The accessory's anchor: the node matrix's position scaled by 0.1 (w 1),
// turned a quarter turn about y (the frame's identity times RotateY).
void PlaceAnchor(Guest& g, AnchorFrame* f, uint32_t node_matrix, Vector* anchor) {
  SetIdentity(f->turn);
  f->turned = f->turn;
  g.ctx.f1.f64 = double(-1.57079637f);                     // The float at 0x82002A2C.
  g.Call(sub_820A68E8, &f->turn);                          // Matrix::RotateY
  g.Call(sub_8209BE70, &f->turned, &f->turned, &f->turn);  // Matrix::Multiply
  g.ctx.fpscr.disableFlushMode();  // Float math as the recompiled code's.
  *anchor = At<Matrix>(node_matrix)->position;
  anchor->w = 1.0f;
  const float scale = 0.1f;  // The float at 0x82001F94.
  anchor->x = anchor->x * scale;
  anchor->y = anchor->y * scale;
  anchor->z = anchor->z * scale;
  g.Call(sub_820AB518, anchor, &f->turned, anchor);  // Vector::Transform
}

// Cockpit::OnFileLoaded: the archive's callback for the cockpit's files
// (r3 the request, r4 its buffer): a part, a body XPR, or the node file
// (then the accessory's node and anchor).
void CockpitFileLoaded(Guest& g, CockpitObject* cockpit, uint32_t request, uint32_t buffer) {
  guest::StackFrame frame(g.ctx, 256);
  CockpitLoads& loads = cockpit->loads;
  for (int i = 0; i < 6; i++) {
    if (uint32_t(int32_t(loads.part_slots[i].unknown_4)) == request) {
      PartArrived(g, &loads.part_slots[i], &loads.parts[i]);
      return;
    }
  }
  for (int k = 0; k < 2; k++) {
    if (uint32_t(int32_t(loads.model.body_slots[k].unknown_4)) == request) {
      XprArrived(g, &loads.model.body_slots[k], &loads.model.xprs[k],
                 &loads.model.texture_providers[k], buffer);
      return;
    }
  }
  if (uint32_t(int32_t(loads.model.node_slot.unknown_4)) != request) {
    return;
  }
  game::Skeleton* skeleton =
      At<game::Skeleton>(NodesArrived(g, &loads.model.node_slot, loads.model.skeleton));
  cockpit->accessory_matrix = AccessoryMatrix(g, skeleton, cockpit->accessory_matrix);
  if (cockpit->accessory_matrix != 0) {
    PlaceAnchor(g, At<AnchorFrame>(frame.Local(0)), cockpit->accessory_matrix,
                &cockpit->accessory_node);
  }
}

// TrafficCar::OnFileLoaded's frame (1,344 bytes): the turn at +80 and
// +144 as AnchorFrame's but 16 bytes on, and the body texture's name.
struct TrafficFileFrame {
  uint8_t unknown_0[96];
  Matrix turn;
  Matrix turned;
  char texture_name[1120];
};
GUEST_OFFSET(TrafficFileFrame, turn, 96);
GUEST_OFFSET(TrafficFileFrame, turned, 160);
GUEST_OFFSET(TrafficFileFrame, texture_name, 224);
GUEST_SIZE(TrafficFileFrame, 1344);

constexpr const char* kTireNodes[4] = {"tire_fl", "tire_fr", "tire_rl", "tire_rr"};  // 0x82004F8C on
constexpr uint32_t kTextureSuffix = 0x82005098;  // "_ex"
constexpr uint32_t kTextureFormat = 0x82004FC8;  // "%s%s"
constexpr uint32_t kCarNames = 0x825F1930;       // A name a car (writable).

uint32_t EntryOf(uint32_t file, uint32_t index) {
  return guest::AddressOf(At<game::DataFile>(file)->Entry(index, true));
}

// TrafficCar::OnFileLoaded: the archive's callback for the traffic car's
// one file (r3 the request): entry 0 its body XPR, set in the provider;
// its plate made from its part order (CarModel::MakePlate); the
// body texture "<name>_ex" looked up; entry 1 its nodes (the skeleton bound
// and posed, the four tires' matrices copied); the tires' positions from
// their matrices, scaled by 0.1 and turned a quarter turn about y (w 0);
// entries 2 to 7 its six parts; the buffer freed, the slot arrived.
void TrafficFileLoaded(Guest& g, TrafficCarObject* car, uint32_t request) {
  guest::StackFrame frame(g.ctx, 1344);
  TrafficFileFrame* f = At<TrafficFileFrame>(frame.Local(0));
  TrafficLoads& loads = car->loads;
  const uint32_t name = *guest::Element<guest::u32_be>(kCarNames, uint32_t(car->record.unknown_0));
  g.Call(sub_8258ED70, f->texture_name, kTextureFormat, name, kTextureSuffix);  // sprintf
  if (uint32_t(int32_t(loads.head.node_slot.unknown_4)) != request) {
    return;
  }
  const uint32_t file = loads.head.node_slot.block;
  g.Call(sub_8257C880, &loads.xprs[0], EntryOf(file, 0));  // XPR::Create
  g.CallVirtual(&loads.lists[0], 4, 0u, &loads.xprs[0]);
  // The plate's two textures, by the part order.
  game::CarModelMakePlate(g, &car->plate.header, &car->plate.data, &car->plate.header_b,
                          &car->plate.data_b, game::PlateWords::Of(car->record.part_order));
  car->body_texture = FindByName(&loads.xprs[0], f->texture_name);

  game::Skeleton* skeleton = At<game::Skeleton>(guest::AddressOf(loads.skeleton));
  g.Call(sub_8257F1D8, skeleton, EntryOf(file, 1), 0u);  // Skeleton::Bind
  g.Call(sub_8257F2A8, skeleton);
  g.Call(sub_8257F410, skeleton);
  g.Call(sub_8257F058, skeleton);  // Skeleton::UpdateMatrices
  const int32_t count = int32_t(g.Call(sub_8257F038, skeleton));  // Skeleton::NodeCount
  for (int32_t i = 0; i < count; i++) {
    const char* node = reinterpret_cast<const char*>(skeleton->Node(uint32_t(i)));
    for (int t = 0; t < 4; t++) {
      if (std::strcmp(kTireNodes[t], node) == 0) {
        g.Call(sub_820A0E18, &car->tire_matrices[t], skeleton->MatrixAt(uint32_t(i)));  // Matrix::Copy
      }
    }
  }

  SetIdentity(f->turn);
  f->turned = f->turn;
  g.ctx.f1.f64 = double(-1.57079637f);                     // The float at 0x82002A2C.
  g.Call(sub_820A68E8, &f->turn);                          // Matrix::RotateY
  g.Call(sub_8209BE70, &f->turned, &f->turned, &f->turn);  // Matrix::Multiply
  g.ctx.fpscr.disableFlushMode();  // Float math as the recompiled code's.
  const float scale = 0.1f;  // The float at 0x82001F94.
  for (int t = 0; t < 4; t++) {
    Vector& v = car->tire_positions[t];
    v = car->tire_matrices[t].position;
    v.w = 1.0f;
    v.x = v.x * scale;
    v.y = v.y * scale;
    v.z = v.z * scale;
  }
  for (Vector& v : car->tire_positions) {
    g.Call(sub_820AB518, &v, &f->turned, &v);  // Vector::Transform
  }
  for (Vector& v : car->tire_positions) {
    v.w = 0.0f;
  }
  for (int i = 0; i < 6; i++) {
    g.Call(sub_8257C238, &loads.parts[i], EntryOf(file, 2 + uint32_t(i)), 0u);  // Model::Load
  }
  FreeSlot(g, &loads.head.node_slot);
  loads.head.node_slot.state = kArrived;
}

// --- The cockpit's and traffic car's texture binds ---

// The binds' frames (1,168 and 1,184 bytes): the texture's name at +80.
struct NameFrame {
  uint8_t unknown_0[80];
  char name[1088];
};
GUEST_OFFSET(NameFrame, name, 80);

constexpr uint32_t kCockpitNames = 0x825F1878;   // A name a car block (writable).
constexpr uint32_t kNumberedFormat = 0x82004F84;  // "%s%s%d"
constexpr uint32_t kInteriorOn = 0x82004FE8;      // "_inte_on"
constexpr uint32_t kInteriorOnNumbered = 0x82004FDC;  // "_inte_on_0"
constexpr uint32_t kBodyNumbered = 0x8200508C;    // "_ex_0"
constexpr int32_t kBlocksWithInteriorTexture = 18;

uint32_t GetTexture(Guest& g, XPR* xpr, int32_t record) {
  return g.Call(sub_8257CA30, xpr, record);  // XPR::GetTexture
}

// Cockpit::BindTextures: 0 while any of its files is loading. Else its
// interior texture ("<name>_inte_on", or "<name>_inte_on_0<variant>") for
// car blocks below 18 (+3504 and each part's +452), and each part's texture
// provider the cockpit's. Returns 1.
bool BindCockpitTextures(Guest& g, CockpitObject* cockpit) {
  guest::StackFrame frame(g.ctx, 1168);
  NameFrame* f = At<NameFrame>(frame.Local(0));
  CockpitLoads& loads = cockpit->loads;
  for (const LoadSlot& slot : loads.part_slots) {
    if (slot.state == LoadSlot::kLoading) {
      return false;
    }
  }
  for (const LoadSlot& slot : loads.model.body_slots) {
    if (slot.state == LoadSlot::kLoading) {
      return false;
    }
  }
  if (loads.model.node_slot.state == LoadSlot::kLoading) {
    return false;
  }
  int32_t record = -1;
  const int32_t variant = cockpit->record.interior_variant;
  const int32_t block = cockpit->head.car_model->record.unknown_0;
  const uint32_t name = *guest::Element<guest::u32_be>(kCockpitNames, uint32_t(block));
  if (variant == 0) {
    g.Call(sub_8258ED70, f->name, kTextureFormat, name, kInteriorOn, variant);  // sprintf
  } else {
    g.Call(sub_8258ED70, f->name, kNumberedFormat, name, kInteriorOnNumbered, variant);
  }
  if (block < kBlocksWithInteriorTexture) {
    record = FindByName(&loads.model.xprs[0], f->name);
    cockpit->texture = GetTexture(g, &loads.model.xprs[0], record);
  }
  for (PartModel& part : loads.parts) {
    part.model.unknown_48 = guest::AddressOf(&loads.model.texture_providers[0]);
    if (block < kBlocksWithInteriorTexture) {
      part.unknown_452[0] = GetTexture(g, &loads.model.xprs[0], record);
    }
  }
  return true;
}

// TrafficCar::BindTextures: 0 while its file is loading. Else each part's
// provider the car's and its vinyl sheet set (Model::SetVinylTextures, the
// blank sheet); with a variant
// (record +160 above 0) and a body texture, each part's +460 the
// "<name>_ex_0<variant>" texture and +436 the body texture. Returns 1.
bool BindTrafficTextures(Guest& g, TrafficCarObject* car) {
  guest::StackFrame frame(g.ctx, 1184);
  NameFrame* f = At<NameFrame>(frame.Local(0));
  TrafficLoads& loads = car->loads;
  if (loads.head.node_slot.state == LoadSlot::kLoading) {
    return false;
  }
  for (PartModel& part : loads.parts) {
    part.model.unknown_48 = guest::AddressOf(&loads.lists[0]);
    g.Call(sub_820CE7B8, &part, loads.head.sheet.header, 0u);  // Model::SetVinylTextures
    const int32_t variant = int32_t(uint32_t(car->record.unknown_160[0]));
    if (!(variant > 0) || car->body_texture == -1) {
      continue;
    }
    const uint32_t name =
        *guest::Element<guest::u32_be>(kCarNames, uint32_t(car->record.unknown_0));
    g.Call(sub_8258ED70, f->name, kNumberedFormat, name, kBodyNumbered, variant);  // sprintf
    const int32_t record = FindByName(&loads.xprs[0], f->name);
    car->variant_texture = record;
    if (record == -1) {
      continue;
    }
    part.unknown_452[2] = GetTexture(g, &loads.xprs[0], record);
    part.unknown_436 = car->body_texture;
  }
  return true;
}

// --- XPR records by name ---

// An ASCII letter in upper case.
char Upper(char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; }

// NameMatches: the same length and the same letters, case ignored; a '*'
// on either side matches any character.
bool NameMatches(const char* a, const char* b) {
  const int32_t length = int32_t(std::strlen(a));
  if (length != int32_t(std::strlen(b))) {
    return false;
  }
  for (int32_t i = 0; i < length; i++) {
    const char x = Upper(a[i]), y = Upper(b[i]);
    if (x != '*' && y != '*' && x != y) {
      return false;
    }
  }
  return true;
}

// XPR::RecordName: resource `index`'s name, or 0 past the count.
uint32_t RecordName(XPR* xpr, uint32_t index) {
  if (index >= xpr->words[5]) {
    return 0;
  }
  return guest::Element<game::XprResource>(xpr->words[4], index)->name.address;
}

uint32_t RecordCount(XPR* xpr) { return xpr->words[4] == 0 ? 0 : uint32_t(xpr->words[5]); }

// XPR::FindByName: the first record whose name matches, or -1.
int32_t FindByName(XPR* xpr, const char* name) {
  const uint32_t count = RecordCount(xpr);
  for (uint32_t i = 0; i < count; i++) {
    if (NameMatches(name, At<char>(RecordName(xpr, i)))) {
      return int32_t(i);
    }
  }
  return -1;
}

}  // namespace

// --- What the group's other files call ---

namespace game {

uint32_t CarModelSelectedTexture(Guest& g, CarModel* model) { return SelectedTexture(g, model); }

void CarModelReset(Guest& g, CarModel* model) { ResetCarModel(g, model); }

uint32_t CarPartsPart(CarParts* parts, int32_t kind, uint32_t base, uint32_t choice) {
  return CarPart(parts, kind, base, choice);
}

// CarParts::WalkMaterials: each arrived part's materials looked up.
void CarPartsWalkMaterials(Guest& g, CarParts* parts) {
  for (int i = 0; i < parts->PartCount(); i++) {
    if (Arrived(parts->loads.slots[i])) {
      PartWalkMaterials(g, &parts->loads.parts[i]);
    }
  }
}

bool CarPartsIsLoaded(CarParts* parts) { return CarPartsLoaded(parts); }

// The palettes (PartModel::ApplyPalette) on the arrived parts.
void CarPartsApplyPalette(Guest& g, CarParts* parts) {
  ApplyPalettes(g, parts->loads.slots, parts->loads.parts, parts->PartCount());
}

void CockpitApplyPalette(Guest& g, CockpitObject* cockpit) {
  ApplyPalettes(g, cockpit->loads.part_slots, cockpit->loads.parts, 6);
}

void InteriorApplyPalette(Guest& g, InteriorObject* interior) {
  ApplyPalettes(g, interior->part_slots, interior->parts, 6);
}

void TrafficCarApplyPalette(Guest& g, TrafficCarObject* car) {
  ApplyPalettes(g, car->loads.part_slots, car->loads.parts, 6);
}

// The paints (PartModel::SetPaint) on the arrived parts.
void CarPartsSetPaint(Guest& g, CarParts* parts, const PartPaint& paint) {
  SetPaints(g, parts->loads.slots, parts->loads.parts, parts->PartCount(), paint);
}

void CockpitSetPaint(Guest& g, CockpitObject* cockpit, const PartPaint& paint) {
  SetPaints(g, cockpit->loads.part_slots, cockpit->loads.parts, 6, paint);
}

void InteriorSetPaint(Guest& g, InteriorObject* interior, const PartPaint& paint) {
  SetPaints(g, interior->part_slots, interior->parts, 6, paint);
}

bool InteriorIsLoaded(Guest& g, InteriorObject* interior) { return InteriorLoaded(g, interior); }

bool CockpitBindTextures(Guest& g, CockpitObject* cockpit) {
  return BindCockpitTextures(g, cockpit);
}

bool TrafficCarBindTextures(Guest& g, TrafficCarObject* car) {
  return BindTrafficTextures(g, car);
}

int32_t XprFindByName(XPR* xpr, const char* name) { return FindByName(xpr, name); }

void ScenePoolsCreateA(Guest& g, ScenePoolSet* set, const CarSceneRecord* record) {
  CreatePools(g, set, record);
}

}  // namespace game

// PoolNode::PoolNode: r3 the node, r4 its list. Returns the node.
extern "C" REX_FUNC(sub_82579600) {
  SBX_SCENE_LOADERS_FALLBACK(sub_82579600)
  ConstructNode(At<PoolNode>(ctx.r3.u32), At<PoolNodeList>(ctx.r4.u32));
}

// PoolNode::~PoolNode: r3 the node, returned.
extern "C" REX_FUNC(sub_82579678) {
  SBX_SCENE_LOADERS_FALLBACK(sub_82579678)
  DestroyNode(At<PoolNode>(ctx.r3.u32));
}

// Scene pages: r3 where to keep them, r4 their size (VirtualAlloc).
extern "C" REX_FUNC(sub_820D0710) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D0710)
  const uint32_t where = ctx.r3.u32, size = ctx.r4.u32;
  *At<guest::u32_be>(where) = AllocPages(g, size);
}

// ScenePools::MakeA: the first pool set's 140 bytes, kept at [0x826B2B20].
extern "C" REX_FUNC(sub_820C3A50) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820C3A50)
  const uint32_t set = AllocPages(g, uint32_t(sizeof(ScenePoolSet)));
  *At<guest::u32_be>(kFirstSet) = set;
  At<ScenePoolSet>(set)->unknown_4 = 0;
}

// ScenePools::CreateA: r3 the first set, r4 the car scene's record.
extern "C" REX_FUNC(sub_820C3DE8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820C3DE8)
  game::ScenePoolsCreateA(g, At<ScenePoolSet>(ctx.r3.u32), At<CarSceneRecord>(ctx.r4.u32));
}

// ScenePools::ReleaseA: r3 the first set, r4 a pool, r5 an index: the
// object released as its class does, then free again.
extern "C" REX_FUNC(sub_820C4298) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820C4298)
  ScenePoolSet* set = At<ScenePoolSet>(ctx.r3.u32);
  const uint32_t pool = ctx.r4.u32, index = ctx.r5.u32;
  PoolNodeList* list = guest::Element<PoolNodeList>(guest::AddressOf(set->lists), pool);
  const uint32_t object = g.Call(sub_82579570, list, index);  // List::At
  static PPCFunc* const releases[] = {sub_820D41A8, sub_820D69B0, sub_820D5EE0, sub_820D72A8,
                                      sub_820D66D0, sub_820D8B30, sub_820D7840};
  ctx.r3.u64 = object;
  if (object == 0) {
    return;  // No such object (the original would write at guest address 16).
  }
  if (pool < std::size(releases)) {
    g.Call(releases[pool], object);
  }
  At<ScenePoolObjectHeader>(object)->state = 0;
}

// The objects' constructors: r3 the object, r4 its pool's list; each
// returns the object.
#define SBX_SCENE_OBJECT_CONSTRUCTOR(name, Type, construct)                     \
  extern "C" REX_FUNC(name) {                                                  \
    SBX_SCENE_LOADERS_FALLBACK(name)                                           \
    const uint32_t object = ctx.r3.u32;                                        \
    construct(g, At<Type>(object), At<PoolNodeList>(ctx.r4.u32));             \
    ctx.r3.u64 = object;                                                       \
  }

// Their destructors: r3 the object, returned.
#define SBX_SCENE_OBJECT_DESTRUCTOR(name, Type, destroy) \
  extern "C" REX_FUNC(name) {                            \
    SBX_SCENE_LOADERS_FALLBACK(name)                     \
    const uint32_t object = ctx.r3.u32;                  \
    destroy(g, At<Type>(object));                        \
    ctx.r3.u64 = object;                                 \
  }

// Their deleting destructors (the vtables' slot): r4 the flags, bit 0 to
// free it too; r3 the object, returned.
#define SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(name, Type, destroy) \
  extern "C" REX_FUNC(name) {                                     \
    SBX_SCENE_LOADERS_FALLBACK(name)                              \
    const uint32_t object = ctx.r3.u32, flags = ctx.r4.u32;       \
    destroy(g, At<Type>(object));                                 \
    if ((flags & 1) != 0) {                                       \
      OperatorDelete(g, object);                                  \
    }                                                             \
    ctx.r3.u64 = object;                                          \
  }

// CarModel::CarModel, CarParts::CarParts, Cockpit::Cockpit,
// Interior::Interior, Pool4Object::Pool4Object, Wheels::Wheels,
// TrafficCar::TrafficCar.
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D5538, CarModel, ConstructCarModel)
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D70D8, CarParts, ConstructCarParts)
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D6048, CockpitObject, ConstructCockpit)
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D7568, InteriorObject, ConstructInterior)
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D6720, Pool4Object, ConstructPool4Object)
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D9238, Wheels, ConstructWheels)
SBX_SCENE_OBJECT_CONSTRUCTOR(sub_820D85C0, TrafficCarObject, ConstructTrafficCar)

// Their destructors and deleting destructors.
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D43F0, CarModel, DestroyCarModel)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D5630, CarModel, DestroyCarModel)
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D6CE8, CarParts, DestroyCarParts)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D7170, CarParts, DestroyCarParts)
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D6178, CockpitObject, DestroyCockpit)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D6448, CockpitObject, DestroyCockpit)
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D7388, InteriorObject, DestroyInterior)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D7610, InteriorObject, DestroyInterior)
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D6780, Pool4Object, DestroyPool4Object)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D6870, Pool4Object, DestroyPool4Object)
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D8D80, Wheels, DestroyWheels)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D92B0, Wheels, DestroyWheels)
SBX_SCENE_OBJECT_DESTRUCTOR(sub_820D7B40, TrafficCarObject, DestroyTrafficCar)
SBX_SCENE_OBJECT_DELETING_DESTRUCTOR(sub_820D8670, TrafficCarObject, DestroyTrafficCar)

// Cockpit::Release, Interior::Release, TrafficCar::Release and
// TrafficCar::Reset: r3 the object.
extern "C" REX_FUNC(sub_820D5EE0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D5EE0)
  ReleaseCockpit(g, At<CockpitObject>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D72A8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D72A8)
  ReleaseInterior(g, At<InteriorObject>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D7840) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7840)
  ReleaseTrafficCar(g, At<TrafficCarObject>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D7760) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7760)
  ResetTrafficCar(g, At<TrafficCarObject>(ctx.r3.u32));
}

// WheelsLoads::Reset: r3 the loads.
extern "C" REX_FUNC(sub_820D8760) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D8760)
  ResetWheelsLoads(g, At<WheelsLoads>(ctx.r3.u32));
}

// CarModel::Reset: r3 the car model.
extern "C" REX_FUNC(sub_820D3B38) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D3B38)
  game::CarModelReset(g, At<CarModel>(ctx.r3.u32));
}

// CarModelLoads::Reset: r3 the loads (a car model's +3728).
extern "C" REX_FUNC(sub_820D37F8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D37F8)
  ResetCarBodyLoads(g, At<game::CarBodyLoads>(ctx.r3.u32));
}

// Interior::OnPartLoaded: the archive's callback, r3 the request, r6 the
// interior.
extern "C" REX_FUNC(sub_820D7228) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7228)
  InteriorPartLoaded(g, At<InteriorObject>(ctx.r6.u32), ctx.r3.u32);
}

// CarModel::SelectedTexture: r3 the car model; the texture or 0.
extern "C" REX_FUNC(sub_820D4228) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D4228)
  ctx.r3.u64 = game::CarModelSelectedTexture(g, At<CarModel>(ctx.r3.u32));
}

// CarParts::IsLoaded: r3 the parts; 1 or 0.
extern "C" REX_FUNC(sub_820D6F08) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6F08)
  ctx.r3.u64 = game::CarPartsIsLoaded(At<CarParts>(ctx.r3.u32)) ? 1 : 0;
}

// CarParts::SlotOf: r4 the kind (above 15: r3 as it is), r5 the base, r6
// the choice.
extern "C" REX_FUNC(sub_820D69F8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D69F8)
  const uint32_t kind = ctx.r4.u32;
  if (kind > 15) {
    return;
  }
  ctx.r3.u64 = PartSlot(kind, ctx.r5.u32, ctx.r6.u32);
}

// CarParts::Part: r3 the parts, r4 the kind, r5 and r6 as SlotOf's; the
// part model or 0.
extern "C" REX_FUNC(sub_820D6AC0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6AC0)
  CarParts* parts = At<CarParts>(ctx.r3.u32);
  ctx.r3.u64 = game::CarPartsPart(parts, int32_t(ctx.r4.u32), ctx.r5.u32, ctx.r6.u32);
}

// CarParts::WalkMaterials and its part step: r3 the parts (or a part).
extern "C" REX_FUNC(sub_820D6B68) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6B68)
  game::CarPartsWalkMaterials(g, At<CarParts>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D1EB8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D1EB8)
  PartWalkMaterials(g, At<PartModel>(ctx.r3.u32));
}

// The palettes (PartModel::ApplyPalette) on the arrived parts: r3 the car
// parts, cockpit, interior or traffic car.
extern "C" REX_FUNC(sub_820D6BD0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D6BD0)
  game::CarPartsApplyPalette(g, At<CarParts>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D5C70) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D5C70)
  game::CockpitApplyPalette(g, At<CockpitObject>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D7340) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7340)
  game::InteriorApplyPalette(g, At<InteriorObject>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D78A8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D78A8)
  game::TrafficCarApplyPalette(g, At<TrafficCarObject>(ctx.r3.u32));
}

// The paints (PartModel::SetPaint, r4 to r9 handed on whole) on the
// arrived parts: r3 the car parts, cockpit or interior.
extern "C" REX_FUNC(sub_820D7038) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7038)
  game::CarPartsSetPaint(g, At<CarParts>(ctx.r3.u32), PaintArguments(ctx));
}

extern "C" REX_FUNC(sub_820D5FD0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D5FD0)
  game::CockpitSetPaint(g, At<CockpitObject>(ctx.r3.u32), PaintArguments(ctx));
}

extern "C" REX_FUNC(sub_820D74F0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D74F0)
  game::InteriorSetPaint(g, At<InteriorObject>(ctx.r3.u32), PaintArguments(ctx));
}

// Interior::IsLoaded: r3 the interior; 1 or 0.
extern "C" REX_FUNC(sub_820D7440) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D7440)
  ctx.r3.u64 = game::InteriorIsLoaded(g, At<InteriorObject>(ctx.r3.u32)) ? 1 : 0;
}

// Accessories::AllLoaded: r3 the accessories' set; 1 or 0.
extern "C" REX_FUNC(sub_820F0668) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820F0668)
  ctx.r3.u64 = AccessoriesLoaded(At<Accessories>(ctx.r3.u32)) ? 1 : 0;
}

// XPR::RecordName: r3 the XPR, r4 the record; its name or 0.
extern "C" REX_FUNC(sub_8257C858) {
  SBX_SCENE_LOADERS_FALLBACK(sub_8257C858)
  ctx.r3.u64 = RecordName(At<XPR>(ctx.r3.u32), ctx.r4.u32);
}

// XPR::FindByName: r3 the XPR, r4 the name; the record or -1.
extern "C" REX_FUNC(sub_820D0768) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D0768)
  const char* name = At<char>(ctx.r4.u32);
  ctx.r3.s64 = game::XprFindByName(At<XPR>(ctx.r3.u32), name);
}

// ModelPoint::Find: r3 the point record, r4 the part model; 1 when found.
extern "C" REX_FUNC(sub_820D0090) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D0090)
  game::PointRecord* record = At<game::PointRecord>(ctx.r3.u32);
  game::PartModel* model = At<game::PartModel>(ctx.r4.u32);
  ctx.r3.u64 = FindPointByNode(g, record, model) ? 1 : 0;
}

// Cockpit::OnFileLoaded: r3 the request, r4 its buffer, r6 the cockpit.
extern "C" REX_FUNC(sub_820D61D0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D61D0)
  const uint32_t request = ctx.r3.u32, buffer = ctx.r4.u32;
  CockpitFileLoaded(g, At<CockpitObject>(ctx.r6.u32), request, buffer);
}

// TrafficCar::OnFileLoaded: r3 the request, r6 the traffic car.
extern "C" REX_FUNC(sub_820D80E0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D80E0)
  const uint32_t request = ctx.r3.u32;
  TrafficFileLoaded(g, At<TrafficCarObject>(ctx.r6.u32), request);
}

// Cockpit::BindTextures and TrafficCar::BindTextures: r3 the object; 0
// while it's loading, else 1.
extern "C" REX_FUNC(sub_820D5DB0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D5DB0)
  ctx.r3.u64 = game::CockpitBindTextures(g, At<CockpitObject>(ctx.r3.u32)) ? 1 : 0;
}

extern "C" REX_FUNC(sub_820D79E8) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820D79E8)
  ctx.r3.u64 = game::TrafficCarBindTextures(g, At<TrafficCarObject>(ctx.r3.u32)) ? 1 : 0;
}

// NameMatches: r3 and r4 the names; 1 or 0.
extern "C" REX_FUNC(sub_820CE5C0) {
  SBX_SCENE_LOADERS_FALLBACK(sub_820CE5C0)
  ctx.r3.u64 = NameMatches(At<char>(ctx.r3.u32), At<char>(ctx.r4.u32)) ? 1 : 0;
}
