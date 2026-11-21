// The course scenery as our own C++ (--native_course_scenery, on by
// default): the scenery manager's life (create, set up, start loading, the
// load check, unload, destroy), its pools (their objects' flags, keys, data
// and constructors; acquire, find, load, release), the area and model lists
// it streams (add, sort, load, stream, release the unused), the course set's
// and the areas' lights' and models' loads and parses, and the texture
// animations' step (every frame).
//
// Scenery::Update (every update): what each view's cameras see, the lists
// streamed and the visible areas filled. The draws (each pass):
// Scenery::Draw (the sky, the set's models, the visible areas, the set's
// lists, the light groups, the minimap), DrawArea, DrawSetLists and
// RecordHeadlightProjectors (the cars' headlights and their projectors).
//
// TextureAnimations::Advance (sub_820F1038) keeps subtick's override, and
// Scenery::Draw the frame state diff's wrapper (debug/frame_state_diff.cpp);
// both call ours in place of the recompiled function. The models, XPRs and
// scenery lists are called directly (model.h, course_scenery.h).

#include "game/course_scenery.h"
#include "base/settings.h"
#include "game/data_file.h"
#include "game/race_camera.h"
#include "game/race_states.h"
#include "game/scene_models.h"
#include "hooks/native_calls.h"

#include <simde/x86/sse4.1.h>

#include <cmath>
#include <cstring>

SBX_SETTING_BOOL(native_course_scenery, true, "Overrides", "Scene",
                 "Our own C++ for the course scenery's manager, pools, streaming lists, the "
                 "course set's and areas' loads, the scenery's update and draws and the "
                 "texture animations' step instead of the recompiled functions");

namespace {

using game::Animation;
using game::AnimationSet;
using game::AreaLights;
using game::AreaModel;
using game::CourseArea;
using game::CourseParts;
using game::CourseSceneryRecord;
using game::CourseSet;
using game::DataFile;
using game::DrawRange;
using game::Model;
using game::ModelList;
using game::PointList;
using game::ObjectPools;
using game::PoolData;
using game::PoolDescriptor;
using game::PoolEntry;
using game::PoolObject;
using game::SceneryArea;
using game::SceneryConfiguration;
using game::SceneryItem;
using game::SceneryKey;
using game::SceneryList;
using game::SceneryManager;
using game::SceneryModel;
using game::SceneryPoolObject;
using game::XPR;
using guest::At;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

bool On() { return SBX_GET(native_course_scenery); }

// The vtables (read-only image data).
constexpr uint32_t kPoolObjectVtable = 0x82006820;
constexpr uint32_t kPool0Vtable = 0x82006724;
constexpr uint32_t kPool1Vtable = 0x82006730;
constexpr uint32_t kAreaLightsVtable = 0x82006744;
constexpr uint32_t kAreaModelVtable = 0x82006750;
constexpr uint32_t kCourseSetVtable = 0x8200675C;

// The archive callbacks and the pools' factory (functions in the image).
constexpr uint32_t kAreaLightsOnLoaded = 0x820F1AC0;
constexpr uint32_t kAreaModelOnLoaded = 0x820F1B78;
constexpr uint32_t kCourseSetOnLoaded = 0x820F2148;
constexpr uint32_t kMakePoolObject = 0x820F24C8;

// The manager's two pointers (Scenery::Init's and Scenery::Create's), the
// scenery's physical memory in use, and the archive's shared load buffer
// and its size (writable).
constexpr uint32_t kSceneryGlobal = 0x826B51F4;
constexpr uint32_t kCreatedManager = 0x826B51FC;
constexpr uint32_t kPhysicalBytes = 0x826B51F8;
constexpr uint32_t kSharedBufferSize = 0x82694E10;
constexpr uint32_t kSharedBuffer = 0x82694E14;

constexpr uint32_t kPageReadWrite = 4;
constexpr uint32_t kPoolAlignment = 64;

// The pools: 0 and 1 (by the configuration's flags), the course set, the
// areas' lights, the area models.
constexpr uint32_t kCourseSetPool = 2;
constexpr uint32_t kAreaLightsPool = 3;
constexpr uint32_t kAreaModelPool = 4;
constexpr uint32_t kPoolCount = 5;

// How many archive requests may be queued before the lists stop asking for
// more (all at once, and streaming).
constexpr uint32_t kLoadAllQueueLimit = 480;
constexpr uint32_t kStreamQueueLimit = 960;

// The areas' models a pool holds at most when streaming.
constexpr uint32_t kMaxModelsAtOnce = 16;

SceneryManager* Manager() { return At<SceneryManager>(*At<u32_be>(kSceneryGlobal)); }

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// --- The game's functions these call ---

uint32_t AssetSize(Guest& g, int32_t asset) { return g.Call(sub_820A0860, asset); }
uint32_t QueueLength(Guest& g) { return g.Call(sub_8209D8D0); }
void ArchiveLoad(Guest& g, int32_t asset, uint32_t destination, uint32_t callback,
                 uint32_t context) {
  g.Call(sub_8209DF80, asset, destination, callback, context, 0u, 0u);
}
void Memcpy(Guest& g, uint32_t destination, uint32_t source, uint32_t size) {
  g.Call(sub_8258CB90, destination, source, size);
}
void ClearMemory(Guest& g, uint32_t address, uint32_t size) {
  g.Call(sub_82293CF8, address, 0u, size);  // XMemSet
}

// --- Pool objects ---

bool FlagsSet(const PoolObject* object, uint32_t bits) { return (object->flags & bits) == bits; }

bool InUse(const PoolObject* object) { return (object->flags & PoolObject::kInUse) != 0; }
bool IsLoaded(const PoolObject* object) { return FlagsSet(object, PoolObject::kLoaded); }
bool IsVisible(const PoolObject* object) {
  return FlagsSet(object, PoolObject::kLoaded | PoolObject::kVisible);
}
bool IsExpiring(const PoolObject* object) {
  return FlagsSet(object, PoolObject::kLoaded | PoolObject::kExpiring);
}

// The visible or expiring bit: off clears it; on, once loaded, makes the
// two this one.
void SetFlag(PoolObject* object, uint32_t bit, bool on) {
  uint32_t flags = object->flags;
  if (!on) {
    flags &= ~bit;
  } else {
    if ((flags & PoolObject::kLoaded) != PoolObject::kLoaded) {
      return;
    }
    flags = (flags & ~uint32_t(PoolObject::kVisible | PoolObject::kExpiring)) | bit;
  }
  object->flags = flags;
}
void SetVisible(PoolObject* object, bool on) { SetFlag(object, PoolObject::kVisible, on); }
void SetExpiring(PoolObject* object, bool on) { SetFlag(object, PoolObject::kExpiring, on); }

void ClearLoadFlags(PoolObject* object) { object->flags &= ~uint32_t(PoolObject::kLoadFlags); }

void MarkLoaded(PoolObject* object) {
  object->flags = (object->flags & ~uint32_t(PoolObject::kLoading)) | PoolObject::kDataLoaded;
}

void ResetObject(PoolObject* object) {
  object->flags = 0;
  object->countdown = -1;
  object->data.used = 0;
}

// `size` bytes (rounded up to 64) of the object's data; where they start.
uint32_t AllocData(PoolData* data, uint32_t size) {
  const uint32_t used = data->used;
  data->used = AlignUp(size, kPoolAlignment) + used;
  return used + data->start;
}

// PoolObject::PoolObject: `own_size` bytes of its own, then `data_size` of
// data; linked to the object right after it.
void ConstructPoolObject(PoolObject* object, uint32_t own_size, uint32_t data_size,
                         uint32_t index) {
  const uint32_t start = guest::AddressOf(object) + own_size;
  object->vtable = kPoolObjectVtable;
  object->data.size = data_size;
  object->data.start = start;
  object->unknown_28 = 0;
  object->index = index;
  object->data_end = start + data_size;
  ResetObject(object);
}

// The object's key (its class's Key, vtable slot 0): its address, and its
// size in `size`. The size comes back in a word of a guest frame.
uint32_t KeyOf(Guest& g, PoolObject* object, uint32_t* size) {
  guest::StackFrame frame(g.ctx, 112);
  const uint32_t size_at = frame.Local(80);
  *At<u32_be>(size_at) = 0;
  const uint32_t key = g.CallVirtual(object, 0, size_at);
  *size = *At<u32_be>(size_at);
  return key;
}

// PoolObject::SetKey: the key at `source` copied in (none for a class with
// no key).
void SetKey(Guest& g, PoolObject* object, uint32_t source) {
  uint32_t size;
  const uint32_t key = KeyOf(g, object, &size);
  if (size != 0 && key != 0) {
    Memcpy(g, key, source, size);
  }
}

// PoolObject::KeyEquals: whether the object's key is the one at `source`
// (any for a class with no key).
bool KeyEquals(Guest& g, PoolObject* object, uint32_t source) {
  uint32_t size;
  const uint32_t key = KeyOf(g, object, &size);
  if (size == 0 || key == 0) {
    return true;
  }
  const uint8_t* mine = At<uint8_t>(key);
  const uint8_t* theirs = At<uint8_t>(source);
  for (uint32_t i = 0; i < size; i++) {
    if (mine[i] != theirs[i]) {
      return false;
    }
  }
  return true;
}

// --- The pools ---

// A pool's object `index` (no bounds check).
uint32_t PoolObjectAddress(ObjectPools* pools, uint32_t pool, uint32_t index) {
  const PoolEntry* entry = pools->Pool(pool);
  return (entry->data_size + entry->object_size) * index + entry->objects;
}

PoolObject* ObjectAt(ObjectPools* pools, uint32_t pool, uint32_t index) {
  return At<PoolObject>(PoolObjectAddress(pools, pool, index));
}

// ObjectPools::Acquire: the pool's first free object, marked in use with
// its key set, moved to the front of the used list. Its index, or -1 when
// none is free.
int32_t Acquire(Guest& g, ObjectPools* pools, uint32_t pool, uint32_t key) {
  PoolEntry* entry = pools->Pool(pool);
  PoolObject* object = entry->heads.free_list;
  if (object == nullptr) {
    return -1;
  }
  object->countdown = -1;
  object->flags |= PoolObject::kInUse;
  SetKey(g, object, key);

  PoolObject* next_free = object->next;
  entry->heads.free_list = next_free;
  if (next_free != nullptr) {
    next_free->previous = nullptr;
  }
  PoolObject* first_used = entry->heads.used_list;
  if (first_used == nullptr) {
    object->next = nullptr;
    object->previous = nullptr;
  } else {
    object->next = first_used;
    object->previous = first_used->previous;
    first_used->previous = object;
  }
  entry->heads.used_list = object;
  return int32_t(uint32_t(object->index));
}

// ObjectPools::Find: the index of the first used object in use whose key
// is the one at `key`, or -1.
int32_t Find(Guest& g, ObjectPools* pools, uint32_t pool, uint32_t key) {
  for (PoolObject* object = pools->Pool(pool)->heads.used_list; object != nullptr;
       object = object->next) {
    if (InUse(object) && KeyEquals(g, object, key)) {
      return int32_t(uint32_t(object->index));
    }
  }
  return -1;
}

// ObjectPools::Load: the object loaded through its class's Load (slot 1);
// returns it.
uint32_t LoadObject(Guest& g, ObjectPools* pools, uint32_t pool, uint32_t index) {
  const uint32_t object = PoolObjectAddress(pools, pool, index);
  if (object != 0) {
    g.CallVirtual(At<PoolObject>(object), 1);
  }
  return object;
}

// ObjectPools::Release: the object reset (its class's Reset, slot 2) and
// moved from the used list to the front of the free list. Returns the
// object that followed it on the used list.
uint32_t Release(Guest& g, ObjectPools* pools, uint32_t pool, uint32_t index) {
  PoolEntry* entry = pools->Pool(pool);
  PoolObject* object = ObjectAt(pools, pool, index);
  const uint32_t following = object->next.address;
  g.CallVirtual(object, 2);

  PoolObject* previous = object->previous;
  PoolObject* next = object->next;
  if (previous != nullptr) {
    previous->next = next;
  } else {
    entry->heads.used_list = next;
  }
  if (next != nullptr) {
    next->previous = previous;
  }
  PoolObject* first_free = entry->heads.free_list;
  object->previous = nullptr;
  object->next = first_free;
  if (first_free != nullptr) {
    first_free->previous = object;
  }
  entry->heads.free_list = object;
  return following;
}

// ObjectPools::Init: `count` pools in one block of physical memory: their
// records, then each pool's objects and data. Each object is made by the
// pool's factory, or as a plain pool object, and all start on the free
// list.
void InitPools(Guest& g, ObjectPools* pools, int32_t count, const PoolDescriptor* descriptors) {
  const uint32_t records_size = AlignUp(uint32_t(count) * uint32_t(sizeof(PoolEntry)),
                                        kPoolAlignment);
  uint32_t total = records_size;
  for (int32_t i = 0; i < count; i++) {
    const PoolDescriptor& d = descriptors[i];
    total += AlignUp(d.object_size, kPoolAlignment) * d.object_count +
             AlignUp(d.data_size, kPoolAlignment) * d.data_count;
  }
  const uint32_t block = g.Call(sub_82293F10, total, -1, kPoolAlignment, kPageReadWrite);
  pools->count = count;
  pools->pools = block;

  uint32_t next_address = block + records_size;
  for (int32_t i = 0; i < count; i++) {
    const PoolDescriptor& d = descriptors[i];
    const uint32_t object_size = AlignUp(d.object_size, kPoolAlignment);
    const uint32_t data_size = AlignUp(d.data_size, kPoolAlignment);
    PoolEntry* entry = pools->Pool(uint32_t(i));
    entry->objects = next_address;
    entry->object_count = int32_t(uint32_t(d.object_count));
    entry->object_size = object_size;
    entry->data_size = data_size;

    PoolObject* previous = nullptr;
    uint32_t address = next_address;
    for (int32_t j = 0; j < entry->object_count; j++) {
      const uint32_t factory = d.factory;
      if (factory != 0) {
        address = g.CallAddress(factory, uint32_t(i), address, object_size, data_size, j);
      } else {
        ConstructPoolObject(At<PoolObject>(address), object_size, data_size, uint32_t(j));
      }
      PoolObject* object = At<PoolObject>(address);
      object->previous = previous;
      previous = object;
      address = object->next.address;
    }
    if (previous != nullptr) {
      previous->next = nullptr;
    }
    entry->heads.used_list = nullptr;
    entry->heads.free_list = At<PoolObject>(entry->objects);
    next_address += d.data_count * data_size + d.object_count * object_size;
  }
}

// ObjectPools::Shutdown: every object reset (its class's Reset), the block
// freed and the pools cleared.
void ShutdownPools(Guest& g, ObjectPools* pools) {
  for (int32_t i = 0; i < pools->count; i++) {
    for (int32_t j = 0; j < pools->Pool(uint32_t(i))->object_count; j++) {
      const uint32_t object = PoolObjectAddress(pools, uint32_t(i), uint32_t(j));
      if (object != 0) {
        g.CallVirtual(At<PoolObject>(object), 2);
      }
    }
  }
  const uint32_t block = pools->pools;
  if (block != 0) {
    g.Call(sub_82294010, block);  // XPhysicalFree
    pools->count = 0;
    pools->pools = 0;
  }
}

// --- The scenery's physical memory ---

// Scenery::AllocPhysical: physical memory, its size counted in.
uint32_t AllocPhysical(Guest& g, uint32_t size) {
  const uint32_t block = g.Call(sub_82293F10, size, -1, 0u, kPageReadWrite);
  *At<u32_be>(kPhysicalBytes) += g.Call(sub_82294008, block);  // its allocated size
  return block;
}

// Scenery::FreePhysical: the block's size counted out, then freed.
void FreePhysical(Guest& g, uint32_t block) {
  *At<u32_be>(kPhysicalBytes) -= g.Call(sub_82294008, block);
  g.Call(sub_82294010, block);
}

// --- The lists ---

// SceneryList::Clear.
void ListClear(SceneryList* list) {
  list->done() = 0;
  list->count() = 0;
}

// SceneryList::Add: the item for this area and model gets the nearer
// distance and the mask's bits; a new one is added when there's none (no
// check for a full list).
void ListAdd(SceneryList* list, double distance, uint32_t area, uint32_t model, uint32_t mask) {
  const uint32_t count = list->count();
  for (uint32_t i = count; i != 0; i--) {
    SceneryItem* item = list->ItemAt(i - 1);
    if (area == item->area && model == item->model) {
      const float nearest = item->distance;
      item->distance = nearest > distance ? float(distance) : nearest;
      item->mask |= mask;
      return;
    }
  }
  SceneryItem* item = list->Item(count);
  *list->ItemSlot(count) = guest::AddressOf(item);
  item->distance = float(distance);
  item->mask = mask;
  item->area = uint16_t(area);
  item->model = uint16_t(model);
  list->count() = count + 1;
}

// SceneryList::Sort: the items from `first` to `last` nearest first (the
// game's quicksort, the middle item's distance as the pivot).
void ListSort(SceneryList* list, int32_t first, int32_t last) {
  for (;;) {
    const float pivot = list->ItemAt(uint32_t((first + last) / 2))->distance;
    int32_t low = first, high = last;
    for (;;) {
      while (list->ItemAt(uint32_t(low))->distance < pivot) {
        low++;
      }
      while (list->ItemAt(uint32_t(high))->distance > pivot) {
        high--;
      }
      if (!(low < high)) {
        break;
      }
      const uint32_t swap = *list->ItemSlot(uint32_t(high));
      *list->ItemSlot(uint32_t(high)) = uint32_t(*list->ItemSlot(uint32_t(low)));
      *list->ItemSlot(uint32_t(low)) = swap;
      low++;
      high--;
    }
    if (first < low - 1) {
      ListSort(list, first, low - 1);
    }
    if (!(last > high + 1)) {
      return;
    }
    first = high + 1;
  }
}

// The item's object, now there: kept, and neither seen nor expiring.
void ItemArrived(SceneryItem* item, PoolObject* object) {
  item->object = object;
  SetExpiring(object, false);
  SetVisible(item->object, false);
}

// SceneryList::LoadAll: from the first not yet done, each item's object
// taken from the list's pool and loaded (while the archive's queue has
// room) the first time its key is asked for; an item whose object is there
// is done. Whether all are.
bool ListLoadAll(Guest& g, SceneryManager* manager, SceneryList* list) {
  const uint32_t pool = list->pool;
  const uint32_t queued = QueueLength(g);
  for (uint32_t i = list->done(); i < list->count(); i++) {
    SceneryItem* item = list->ItemAt(i);
    item->object = nullptr;
    for (uint32_t k = 0; k < list->key_count; k++) {
      SceneryKey* key = list->Key(k);
      if (key->area != item->area || key->model != item->model) {
        continue;
      }
      if (key->object == nullptr && queued < kLoadAllQueueLimit) {
        const int32_t index = Acquire(g, &manager->pools, pool, guest::AddressOf(&item->area));
        key->object = At<PoolObject>(LoadObject(g, &manager->pools, pool, uint32_t(index)));
        manager->last_pool = int32_t(pool);
        manager->last_index = index;
      }
      if (key->object != nullptr) {
        ItemArrived(item, key->object);
        list->done() += 1;
      }
      break;
    }
  }
  return list->done() == list->count();
}

// SceneryList::Stream: each item's object found in the list's pool by its
// key, or taken and loaded (while the archive's queue has room); 0 for
// none. Whether all are done.
bool ListStream(Guest& g, SceneryManager* manager, SceneryList* list) {
  const uint32_t pool = list->pool;
  const uint32_t queued = QueueLength(g);
  for (uint32_t i = 0; i < list->count(); i++) {
    SceneryItem* item = list->ItemAt(i);
    const uint32_t key = guest::AddressOf(&item->area);
    int32_t index = Find(g, &manager->pools, pool, key);
    if (index < 0 && queued < kStreamQueueLimit) {
      index = Acquire(g, &manager->pools, pool, key);
      if (index >= 0) {
        LoadObject(g, &manager->pools, pool, uint32_t(index));
        manager->last_pool = int32_t(pool);
        manager->last_index = index;
      }
    }
    if (index < 0) {
      item->object = nullptr;
      continue;
    }
    ItemArrived(item, ObjectAt(&manager->pools, pool, uint32_t(index)));
  }
  return list->done() == list->count();
}

// SceneryList::ReleaseUnused: the list's pool's loaded objects not seen this
// update are marked expiring for two more updates, then released; a seen
// one's seen bit is cleared for the next update.
void ListReleaseUnused(Guest& g, SceneryManager* manager, SceneryList* list) {
  const uint32_t pool = list->pool;
  PoolObject* object = manager->pools.Pool(pool)->heads.used_list;
  while (object != nullptr) {
    if (!IsLoaded(object) || IsVisible(object)) {
      SetVisible(object, false);
    } else if (!IsExpiring(object)) {
      SetExpiring(object, true);
      object->countdown = 2;
    } else if (object->countdown > 0) {
      object->countdown -= 1;
    } else {
      object = At<PoolObject>(Release(g, &manager->pools, pool, object->index));
      continue;
    }
    object = object->next;
  }
}

// --- The configuration ---

// SceneryConfig::ModelAsset: area `area`'s model `model`'s asset, or -1
// past its last (or for no such area).
int32_t ModelAsset(const SceneryConfiguration* config, uint32_t area, uint32_t model) {
  CourseSceneryRecord* course = config->course;
  if (area >= course->area_count) {
    return -1;
  }
  const CourseArea* entry = course->Area(area);
  const uint32_t first = uint32_t(entry->first_model), last = uint32_t(entry->last_model);
  if (first <= last && model <= last - first) {
    return int32_t(first + model);
  }
  return -1;
}

// An area's lights' asset, or -1 past the last area.
int32_t AreaLightsAsset(const SceneryConfiguration* config, uint32_t area) {
  CourseSceneryRecord* course = config->course;
  return area < course->area_count ? int32_t(course->Area(area)->lights) : -1;
}

// An area's model count (its last model - its first + 1), or -1 past the
// last area.
int32_t AreaModelCount(const SceneryConfiguration* config, uint32_t area) {
  CourseSceneryRecord* course = config->course;
  if (area >= course->area_count) {
    return -1;
  }
  const CourseArea* entry = course->Area(area);
  return entry->last_model - entry->first_model + 1;
}

// The sum of the sizes (mode 0, everything at once) or the largest.
void AddSize(const SceneryConfiguration* config, int32_t& total, uint32_t size) {
  if (config->mode == 0) {
    total += int32_t(size);
  } else if (total < int32_t(size)) {
    total = int32_t(size);
  }
}

// Scenery::PoolSizes: each pool's data size; the areas (lists up to the first
// without lights) and their models counted.
void PoolSizes(Guest& g, u32_be area_counts[2], u32_be model_counts[2], u32_be sizes[kPoolCount],
               const SceneryConfiguration* config) {
  // Pool 0: what the configuration names; pool 1 none.
  sizes[0] = AssetSize(g, *config->pool0_asset);
  sizes[1] = 0;

  // Pool 2, the course set: set file 2.
  int32_t course_set = 0;
  const int32_t set_file = config->course->set_file_2;
  if (set_file >= 0) {
    course_set += int32_t(AssetSize(g, set_file));
  }
  sizes[2] = uint32_t(course_set);

  // Pool 3, the areas' lights.
  int32_t lights = 0;
  uint32_t areas = 0;
  for (int32_t asset; (asset = AreaLightsAsset(config, areas)) >= 0; areas++) {
    AddSize(config, lights, AssetSize(g, asset));
  }
  area_counts[0] = areas;
  area_counts[1] = areas;
  sizes[3] = uint32_t(lights);

  // Pool 4, the areas' models.
  int32_t models = 0;
  uint32_t model_count = 0;
  for (uint32_t area = 0; AreaLightsAsset(config, area) >= 0; area++) {
    const int32_t count = AreaModelCount(config, area);
    for (int32_t model = 0; model < count; model++) {
      const int32_t asset = ModelAsset(config, area, uint32_t(model));
      if (asset >= 0) {
        AddSize(config, models, AssetSize(g, asset));
        model_count++;
      }
    }
  }
  model_counts[0] = model_count;
  model_counts[1] = model_count < kMaxModelsAtOnce ? model_count : kMaxModelsAtOnce;
  sizes[4] = uint32_t(models);
}

// --- The scenery manager ---

// Scenery::Init: the manager's global set, its fields and lists cleared.
void InitManager(SceneryManager* manager) {
  *At<u32_be>(kSceneryGlobal) = guest::AddressOf(manager);
  manager->unknown_52 = 0;
  manager->unknown_56 = 0;
  for (uint8_t& byte : manager->unknown_60) {
    byte = 0;
  }
  for (int i = 0; i < 2; i++) {
    manager->area_counts[i] = 0;
    manager->model_counts[i] = 0;
  }
  manager->pool0_object = nullptr;
  manager->course_set = nullptr;
  manager->pool0_block = 0;
  manager->model_buffer = 0;
  std::memset(&manager->areas, 0, sizeof(SceneryList));
  std::memset(&manager->models, 0, sizeof(SceneryList));
  manager->last_pool = -1;
  manager->last_index = -1;
  manager->loading = 0;
}

// Scenery::Create: the 248-byte manager from physical memory, its draw
// settings, its texture provider made, then Scenery::Init.
void CreateManager(Guest& g) {
  const uint32_t address = AllocPhysical(g, uint32_t(sizeof(SceneryManager)));
  SceneryManager* manager = At<SceneryManager>(address);
  *At<u32_be>(kCreatedManager) = address;
  manager->unknown_200[0] = 192.0f;
  manager->pass_mask = 0x38435;
  manager->unknown_200[1] = 72.0f;
  manager->unknown_200[2] = 256.0f;
  manager->unknown_200[3] = 64.0f;
  manager->visible_areas = 0;
  game::XprListConstruct(g, &manager->textures, -1);
  InitManager(manager);
}

// A list over `count` keys: its item pointers, items and keys from
// physical memory, the last two cleared.
void SetupList(Guest& g, SceneryList* list, uint32_t pool, uint32_t count) {
  list->done() = 0;
  list->count() = 0;
  list->key_count = count;
  list->pool = pool;
  list->by_distance = AllocPhysical(g, count * uint32_t(sizeof(u32_be)));
  list->items = AllocPhysical(g, count * uint32_t(sizeof(SceneryItem)));
  list->keys = AllocPhysical(g, count * uint32_t(sizeof(SceneryKey)));
  ClearMemory(g, list->items, count * uint32_t(sizeof(SceneryItem)));
  ClearMemory(g, list->keys, count * uint32_t(sizeof(SceneryKey)));
}

// Scenery::SetupPools: the configuration copied in; the lists and their
// keys (each area with lights, each of its models with an asset); the five
// pools. Pool 4's data is the archive's shared load buffer when that's big
// enough for the largest model, else none.
void SetupPools(Guest& g, SceneryManager* manager, uint32_t configuration) {
  *At<u32_be>(kPhysicalBytes) = 0;
  manager->pool0_block_size = 0;
  manager->pool0_block = 0;
  SceneryConfiguration* config = &manager->config;
  Memcpy(g, guest::AddressOf(config), configuration, uint32_t(sizeof(SceneryConfiguration)));
  if (config->mode == 2 && config->last_view == 0) {
    config->mode = *config->mode_2_with_one_view;
  }

  u32_be sizes[kPoolCount];
  PoolSizes(g, manager->area_counts, manager->model_counts, sizes, config);

  SetupList(g, &manager->areas, kAreaLightsPool, manager->area_counts[0]);
  SetupList(g, &manager->models, kAreaModelPool, manager->model_counts[0]);

  uint32_t area_keys = 0, model_keys = 0;
  for (uint32_t area = 0; area_keys < manager->areas.key_count; area++) {
    const int32_t lights = AreaLightsAsset(config, area);
    if (lights < 0) {
      continue;
    }
    SceneryKey* key = manager->areas.Key(area_keys++);
    key->model = 0;
    key->asset = lights;
    key->area = uint16_t(area);
    const int32_t count = AreaModelCount(config, area);
    for (int32_t model = 0; model < count; model++) {
      const int32_t asset = ModelAsset(config, area, uint32_t(model));
      if (asset >= 0) {
        SceneryKey* model_key = manager->models.Key(model_keys++);
        model_key->area = uint16_t(area);
        model_key->model = uint16_t(model);
        model_key->asset = asset;
      }
    }
  }

  // The pools. A pool the configuration leaves out keeps the previous
  // pool's sizes and counts (none for pool 0).
  PoolDescriptor descriptors[kPoolCount];
  uint32_t object_size = 0, object_count = 0, data_count = 0;
  const bool all_at_once = config->mode == 0;
  for (uint32_t pool = 0; pool < kPoolCount; pool++) {
    switch (pool) {
      case 0:
        if (config->has_pool0 != 0) {
          manager->pool0_block_size = sizes[0];
          object_size = 60;
          object_count = 1;
          data_count = 0;
          manager->pool0_block = AllocPhysical(g, sizes[0]);
          sizes[0] = 0;
        }
        break;
      case 1:
        if (config->has_pool1 != 0) {
          object_size = 60;
          object_count = 0;
          data_count = 0;
        }
        break;
      case kCourseSetPool:
        object_size = 3824;
        object_count = 1;
        data_count = 1;
        break;
      case kAreaLightsPool:
        object_size = 88;
        object_count = all_at_once ? manager->area_counts[0] : manager->area_counts[1];
        data_count = all_at_once ? 1 : object_count;
        break;
      case kAreaModelPool: {
        object_size = 560;
        object_count = all_at_once ? manager->model_counts[0] : manager->model_counts[1];
        data_count = all_at_once ? 1 : object_count;
        const bool fits = !(*At<u32_be>(kSharedBufferSize) < sizes[4]);
        sizes[4] = 0;
        manager->model_buffer = fits ? uint32_t(*At<u32_be>(kSharedBuffer)) : 0u;
        break;
      }
    }
    PoolDescriptor& d = descriptors[pool];
    d.index = pool;
    d.object_size = object_size;
    d.data_size = sizes[pool];
    d.object_count = object_count;
    d.data_count = data_count;
    d.factory = kMakePoolObject;
  }
  InitPools(g, &manager->pools, int32_t(kPoolCount), descriptors);
  manager->loading = 0;
}

// Scenery::Setup: the pools, then one visible-area record per area. Returns
// the records' block.
uint32_t Setup(Guest& g, SceneryManager* manager, uint32_t configuration) {
  SetupPools(g, manager, configuration);
  const uint32_t areas = manager->area_counts[0];
  const uint32_t models = manager->model_counts[0];
  manager->visible_area_count = 0;
  manager->visible_area_capacity = areas;
  const uint32_t block = AllocPhysical(g, areas * uint32_t(sizeof(SceneryArea)));
  manager->visible_areas = block;
  for (uint32_t i = 0; i < manager->visible_area_capacity; i++) {
    SceneryArea* area = manager->VisibleArea(i);
    area->model_count = 0;
    area->model_capacity = models;
  }
  return block;
}

// An object taken from `pool` (no key) and loaded; its index.
int32_t AcquireAndLoad(Guest& g, SceneryManager* manager, uint32_t pool) {
  const int32_t index = Acquire(g, &manager->pools, pool, 0);
  LoadObject(g, &manager->pools, pool, uint32_t(index));
  return index;
}

// Scenery::StartLoad: the course set loaded; all at once (mode 0), both
// lists filled with every key, for IsLoaded to load them all; pool 0's
// object loaded when there is one.
void StartLoad(Guest& g, SceneryManager* manager) {
  int32_t index = AcquireAndLoad(g, manager, kCourseSetPool);
  manager->course_set =
      At<CourseSet>(PoolObjectAddress(&manager->pools, kCourseSetPool, uint32_t(index)));
  manager->last_index = index;
  manager->last_pool = int32_t(kCourseSetPool);
  if (manager->config.mode == 0) {
    ListClear(&manager->areas);
    ListClear(&manager->models);
    for (SceneryList* list : {&manager->areas, &manager->models}) {
      for (uint32_t i = 0; i < list->key_count; i++) {
        const SceneryKey* key = list->Key(i);
        ListAdd(list, 0.0, key->area, key->model, 0);
      }
    }
  }
  if (manager->config.has_pool0 != 0) {
    index = AcquireAndLoad(g, manager, 0);
    manager->pool0_object = ObjectAt(&manager->pools, 0, uint32_t(index));
    manager->last_pool = 0;
    manager->last_index = index;
  }
  manager->loading = 1;
}

// Scenery::IsLoaded: while loading, both lists all in; then the object last
// asked for loaded (or no longer in use).
bool SceneryIsLoaded(Guest& g, SceneryManager* manager) {
  if (manager->loading != 0) {
    if (!ListLoadAll(g, manager, &manager->areas) || !ListLoadAll(g, manager, &manager->models)) {
      return false;
    }
  }
  if (manager->last_pool < 0 || manager->last_index < 0) {
    return false;
  }
  PoolObject* object = ObjectAt(&manager->pools, uint32_t(int32_t(manager->last_pool)),
                                uint32_t(int32_t(manager->last_index)));
  if (object == nullptr || (InUse(object) && !IsLoaded(object))) {
    return false;
  }
  manager->loading = 0;
  return true;
}

// A block freed when there is one.
void FreeIfSet(Guest& g, uint32_t block) {
  if (block != 0) {
    FreePhysical(g, block);
  }
}

// The list's blocks freed and the list cleared.
void ReleaseList(Guest& g, SceneryList* list) {
  FreeIfSet(g, list->keys);
  FreeIfSet(g, list->items);
  FreeIfSet(g, list->by_distance);
  ClearMemory(g, guest::AddressOf(list), uint32_t(sizeof(SceneryList)));
}

// Scenery::UnloadPools: the pools shut down, pool 0's block freed, the
// lists' blocks freed.
void UnloadPools(Guest& g, SceneryManager* manager) {
  ShutdownPools(g, &manager->pools);
  manager->model_buffer = 0;
  manager->pool0_block_size = 0;
  if (manager->pool0_block != 0) {
    FreePhysical(g, manager->pool0_block);
    manager->pool0_block = 0;
  }
  ReleaseList(g, &manager->models);
  ReleaseList(g, &manager->areas);
}

// Scenery::Unload: the visible areas freed, then the pools.
void Unload(Guest& g, SceneryManager* manager) {
  if (manager->visible_areas != 0) {
    FreePhysical(g, manager->visible_areas);
    manager->visible_areas = 0;
  }
  UnloadPools(g, manager);
}

// Scenery::Destroy: both globals cleared, the manager freed.
void Destroy(Guest& g) {
  *At<u32_be>(kSceneryGlobal) = 0;
  FreePhysical(g, *At<u32_be>(kCreatedManager));
  *At<u32_be>(kCreatedManager) = 0;
}

// --- The course set ---

void InitModelList(ModelList* list) {
  list->models.Reset();
  list->count = 0;
}

// The course set's model lists made empty, the point list's draw state
// cleared.
void InitLists(CourseParts* parts) {
  InitModelList(&parts->list_464);
  InitModelList(&parts->list_1000);
  for (ModelList& list : parts->lists_1532) {
    InitModelList(&list);
  }
  InitModelList(&parts->point_list.list);
  parts->point_list.animations = 0;
  parts->point_list.texture_provider = 0;
  parts->point_list.first_group = 0;
  parts->point_list.point_buffer_count = 0;
}

// CourseSet::InitParts: the XPR cleared, the model made, the lists made
// empty.
void InitParts(Guest& g, CourseParts* parts) {
  game::XprConstruct(&parts->xpr);
  game::ModelConstruct(g, &parts->model);
  InitLists(parts);
}

// CourseSet::CourseSet (pool 2's objects).
void ConstructCourseSet(Guest& g, CourseSet* set, uint32_t own_size, uint32_t data_size,
                        uint32_t index) {
  ConstructPoolObject(&set->object, own_size, data_size, index);
  set->object.vtable = kCourseSetVtable;
  InitParts(g, &set->parts);
  set->set_files[0] = 0;
  set->set_files[1] = 0;
  set->parts.area_file = 0;
  set->parts.animations = nullptr;
  set->parts.light_groups = 0;
  InitLists(&set->parts);
}

// CourseSet::Load: set file 1 into a block of its own, set file 2 into the
// object's data; the last one there calls back CourseSet::OnLoaded.
void LoadCourseSet(Guest& g, CourseSet* set) {
  const CourseSceneryRecord* course = Manager()->config.course;
  ClearLoadFlags(&set->object);
  set->parts.area_file = 0;
  set->parts.animations = nullptr;

  const int32_t assets[2] = {course->set_file_1, course->set_file_2};
  int last = 0;
  for (int i = 0; i < 2; i++) {
    if (assets[i] < 0) {
      set->set_files[i] = 0;
      continue;
    }
    last = i;
    const uint32_t size = AssetSize(g, assets[i]);
    set->set_files[i] = i == 0 ? AllocPhysical(g, size) : AllocData(&set->object.data, size);
  }
  for (int i = 0; i < 2; i++) {
    if (assets[i] >= 0) {
      ArchiveLoad(g, assets[i], set->set_files[i], i == last ? kCourseSetOnLoaded : 0u,
                  guest::AddressOf(set));
    }
  }
}

// A container's entry, or 0 (also for no container).
uint32_t EntryOf(uint32_t file, uint32_t index, bool skip_empty) {
  if (file == 0) {
    return 0;
  }
  return guest::AddressOf(At<DataFile>(file)->Entry(index, skip_empty));
}

// CourseSet::Parse: set file 1 (0 the XPR, 1 the model, 2 to 6 model lists,
// 7 a point list) and set file 2 (0 the areas, 1 the texture animations, 2
// the light groups).
void ParseCourseSet(Guest& g, CourseSet* set) {
  const uint32_t file_1 = set->set_files[0];
  if (const uint32_t xpr = EntryOf(file_1, 0, true); xpr != 0) {
    game::XprConstruct(&set->parts.xpr);
    g.Call(sub_8257C880, &set->parts.xpr, xpr);  // XPR::Create (wrapped)
  }
  if (const uint32_t model = EntryOf(file_1, 1, true); model != 0) {
    game::ModelConstruct(g, &set->parts.model);
    game::ModelLoad(g, &set->parts.model, EntryOf(model, 0, false), 0);
  }
  ModelList* lists[] = {&set->parts.list_464, &set->parts.list_1000, &set->parts.lists_1532[0],
                        &set->parts.lists_1532[1], &set->parts.lists_1532[2]};
  for (uint32_t i = 0; i < 5; i++) {
    if (const uint32_t entry = EntryOf(file_1, 2 + i, true); entry != 0) {
      game::ModelListLoad(g, lists[i], At<DataFile>(entry));
    }
  }
  if (const uint32_t entry = EntryOf(file_1, 7, true); entry != 0) {
    game::PointListLoad(g, &set->parts.point_list, At<DataFile>(entry));
  }

  const uint32_t file_2 = set->set_files[1];
  if (const uint32_t areas = EntryOf(file_2, 0, true); areas != 0) {
    set->parts.area_file = areas;
  }
  if (const uint32_t animations = EntryOf(file_2, 1, true); animations != 0) {
    set->parts.animations = At<AnimationSet>(animations);
  }
  if (const uint32_t groups = EntryOf(file_2, 2, true); groups != 0) {
    set->parts.light_groups = groups;
  }
}

// CourseSet::OnLoaded: parsed, set file 1's block freed, marked loaded.
void CourseSetLoaded(Guest& g, CourseSet* set) {
  ParseCourseSet(g, set);
  FreePhysical(g, set->set_files[0]);
  MarkLoaded(&set->object);
}

// CourseSet::Reset: its XPR reset, model released and lists reset, then the
// pool object's own reset.
void ResetCourseSet(Guest& g, CourseSet* set) {
  game::XprFree(g, &set->parts.xpr);
  game::ModelRelease(g, &set->parts.model);
  game::ModelListRelease(g, &set->parts.list_464);
  game::ModelListRelease(g, &set->parts.list_1000);
  for (ModelList& list : set->parts.lists_1532) {
    game::ModelListRelease(g, &list);
  }
  game::ModelListRelease(g, &set->parts.point_list.list);
  ResetObject(&set->object);
}

// --- The areas' lights and models ---

// The asset of the key with this area and model in the list's keys, or -1.
int32_t KeyAsset(SceneryList* list, uint16_t area, uint16_t model) {
  for (uint32_t i = 0; i < list->key_count; i++) {
    const SceneryKey* key = list->Key(i);
    if (key->area == area && key->model == model) {
      return key->asset;
    }
  }
  return -1;
}

// The area and model halfwords of an object's 4-byte key.
uint16_t KeyArea(const u32_be& key) { return uint16_t(uint32_t(key) >> 16); }
uint16_t KeyModel(const u32_be& key) { return uint16_t(uint32_t(key)); }

// AreaLights::Load: the area's light file into the object's data, with
// AreaLights::OnLoaded.
void LoadAreaLights(Guest& g, AreaLights* lights) {
  SceneryManager* manager = Manager();
  ClearLoadFlags(&lights->object);
  lights->light_set = 0;
  const int32_t asset = KeyAsset(&manager->areas, KeyArea(lights->key), KeyModel(lights->key));
  const uint32_t data = AllocData(&lights->object.data, AssetSize(g, asset));
  ArchiveLoad(g, asset, data, kAreaLightsOnLoaded, guest::AddressOf(lights));
}

// AreaLights::Parse: the light file's entry 0, the light set.
void ParseAreaLights(AreaLights* lights) {
  if (const uint32_t set = EntryOf(lights->object.data.start, 0, true); set != 0) {
    lights->light_set = set;
  }
}

// AreaModel::AreaModel (pool 4's objects).
void ConstructAreaModel(Guest& g, AreaModel* area, uint32_t own_size, uint32_t data_size,
                        uint32_t index) {
  ConstructPoolObject(&area->object, own_size, data_size, index);
  area->object.vtable = kAreaModelVtable;
  game::ModelConstruct(g, &area->model.model);
  area->model.model.vtable = SceneryModel::kVtable;
  game::XprConstruct(&area->xpr);
  area->links.model = nullptr;
  area->links.xpr = nullptr;
}

// AreaModel::Load: the area model into the manager's model buffer, with
// AreaModel::OnLoaded.
void LoadAreaModel(Guest& g, AreaModel* area) {
  SceneryManager* manager = Manager();
  ClearLoadFlags(&area->object);
  area->links.model = nullptr;
  area->links.xpr = nullptr;
  const int32_t asset = KeyAsset(&manager->models, KeyArea(area->key), KeyModel(area->key));
  ArchiveLoad(g, asset, manager->model_buffer, kAreaModelOnLoaded, guest::AddressOf(area));
}

// AreaModel::Parse: the model buffer's entry 0 into the model, entry 1 into
// the XPR; each linked when there.
void ParseAreaModel(Guest& g, AreaModel* area) {
  const uint32_t buffer = Manager()->model_buffer;
  const uint32_t model = EntryOf(buffer, 0, true);
  const uint32_t xpr = EntryOf(buffer, 1, true);
  if (model != 0) {
    game::ModelLoad(g, &area->model.model, model, 0);
    area->links.model = &area->model;
  } else {
    area->links.model = nullptr;
  }
  if (xpr != 0) {
    g.Call(sub_8257C880, &area->xpr, xpr);  // XPR::Create (wrapped)
    area->links.xpr = &area->xpr;
  } else {
    area->links.xpr = nullptr;
  }
}

// AreaModel::Reset: the model and XPR in use released, then the pool
// object's own reset.
void ResetAreaModel(Guest& g, AreaModel* area) {
  if (area->links.model != nullptr) {
    game::ModelRelease(g, &area->links.model->model);
    area->links.model = nullptr;
  }
  if (area->links.xpr != nullptr) {
    game::XprFree(g, area->links.xpr);
    area->links.xpr = nullptr;
  }
  ResetObject(&area->object);
}

// Scenery::MakePoolObject: the pools' factory. Each pool's class made in
// place; returns the object.
void MakePoolObject(Guest& g, uint32_t pool, uint32_t address, uint32_t own_size,
                    uint32_t data_size, uint32_t index) {
  if (address == 0) {
    return;
  }
  switch (pool) {
    case 0:
    case 1: {
      SceneryPoolObject* object = At<SceneryPoolObject>(address);
      ConstructPoolObject(&object->object, own_size, data_size, index);
      object->object.vtable = pool == 0 ? kPool0Vtable : kPool1Vtable;
      object->unknown_56 = 0;
      break;
    }
    case kCourseSetPool:
      ConstructCourseSet(g, At<CourseSet>(address), own_size, data_size, index);
      break;
    case kAreaLightsPool: {
      AreaLights* lights = At<AreaLights>(address);
      ConstructPoolObject(&lights->object, own_size, data_size, index);
      lights->object.vtable = kAreaLightsVtable;
      game::XprConstruct(&lights->parts.xpr);
      break;
    }
    case kAreaModelPool:
      ConstructAreaModel(g, At<AreaModel>(address), own_size, data_size, index);
      break;
  }
}

// --- Texture animations ---

// Animation::Advance: the frame back to 0 (and the key) once past the last;
// the key moved on when there's a next one and the frame has reached its
// first frame; then the frame counted on.
void AdvanceAnimation(Animation* a) {
  if (a->frame > a->last_frame) {
    a->frame = 0;
    a->key = 0;
  }
  const int32_t next = a->key + 1;
  if (next < a->key_count && !(a->frame < *a->KeyFrame(next))) {
    a->key = next;
  }
  a->frame += 1;
}

// AnimationSet::Advance: each animation one step on.
void AdvanceAnimations(AnimationSet* set) {
  for (int32_t i = 0; i < int32_t(uint32_t(set->count)); i++) {
    AdvanceAnimation(set->Entry(uint32_t(i)));
  }
}

// --- What the cameras see (Scenery::Update, every update) ---

// The camera passes' records (writable).
constexpr uint32_t kPassRecords = 0x825F2F08;
constexpr int kPassRecordCount = 14;

// The constants the collects read (read-only data): how far past the far
// distance an area (0x820029F8) or a model (0x82002534) may be, half the
// far distance (0x82001BDC), and the "none yet" distance (0x82002A70).
constexpr float kAreaMargin = 70.0f;
constexpr float kModelMargin = 50.0f;
constexpr float kHalf = 0.5f;
constexpr float kFarthest = 3.40282347e+38f;

game::Viewport* ViewportAt(uint32_t view) {
  return At<game::Viewports>(*At<u32_be>(game::kViewports))->players[view];
}

// The bits of the passes for `view`'s player whose camera pass is on.
uint32_t PassMask(int32_t player) {
  uint32_t mask = 0;
  for (int i = 0; i < kPassRecordCount; i++) {
    const game::ScenePassRecord* record = guest::Element<game::ScenePassRecord>(kPassRecords, uint32_t(i));
    if (player == record->player &&
        guest::Element<game::CameraPass>(game::kCameraPasses, uint32_t(int32_t(record->pass)))->enabled != 0) {
      mask |= record->mask;
    }
  }
  return mask;
}

// Inside the rectangle of corners 0 (least x and z) and 2 (greatest).
bool Inside(float x, float z, const game::Vector* corners) {
  return !(x < corners[0].x) && !(x > corners[2].x) && !(z < corners[0].z) && !(z > corners[2].z);
}

// The distance in x and z from (x, z) to a corner.
float CornerDistance(const game::Vector& corner, float x, float z) {
  const float dx = corner.x - x;
  const float dz = corner.z - z;
  return std::sqrt(float(std::fma(dz, dz, dx * dx)));
}

// The collects' frames: the corners copied in at +80 (memcpy), each
// corner's w set to 1.0 first (the copy overwrites them).
struct CornersFrame {
  uint8_t unknown_0[80];
  game::Vector corners[4];
};
GUEST_OFFSET(CornersFrame, corners, 80);

// The game's float math runs without flush-to-zero (the recompiled code
// turns it off before its float instructions; vector code turns it on).
void ScalarFloats(Guest& g) { g.ctx.fpscr.disableFlushMode(); }

game::Vector* CopyCorners(Guest& g, CornersFrame* f, const game::Vector* corners) {
  for (game::Vector& corner : f->corners) {
    corner.w = 1.0f;
  }
  Memcpy(g, guest::AddressOf(f->corners), guest::AddressOf(corners), uint32_t(sizeof(f->corners)));
  ScalarFloats(g);
  return f->corners;
}

game::SceneryAreaEntry* AreaEntry(SceneryManager* manager, uint32_t area) {
  const uint32_t file = manager->course_set->parts.area_file;
  return At<game::SceneryAreaEntry>(guest::AddressOf(At<DataFile>(file)->Entry(area, false)));
}

// Scenery::CollectAreas: each area of the course near `view`'s viewport: 0
// with the eye inside it; else half the far distance when the eye moved
// that far each way (x, then z; four ways) is inside; else the nearest
// corner. Those within the far distance + 70 are added to the areas' list
// with the view's passes.
void CollectAreas(Guest& g, SceneryManager* manager, uint32_t view, game::Viewport* viewport) {
  guest::StackFrame frame(g.ctx, 288);
  CornersFrame* f = At<CornersFrame>(frame.Local(0));
  ScalarFloats(g);
  const float far = viewport->views[0].unknown_72;
  const float limit = far + kAreaMargin;
  const uint32_t file = manager->course_set->parts.area_file;
  const uint32_t count = At<DataFile>(file)->count;
  for (uint32_t area = 0; area < count; area++) {
    game::SceneryAreaEntry* entry = AreaEntry(manager, area);
    float nearest = kFarthest;
    const game::Vector* corners = CopyCorners(g, f, entry->corners);
    const float x = viewport->eye.x;
    if (Inside(x, viewport->eye.z, corners)) {
      nearest = 0.0f;
    } else {
      const float z = viewport->eye.z;
      const float half = far * kHalf;
      for (int k = 0; k < 4; k++) {
        const float moved_x = x + ((k & 1) != 0 ? half : -half);
        const float moved_z = z + ((k & 2) != 0 ? half : -half);
        if (Inside(moved_x, moved_z, corners)) {
          nearest = half;
          break;
        }
        const float d = CornerDistance(corners[k], x, z);
        if (!(d > nearest)) {
          nearest = d;
        }
      }
    }
    if (limit < nearest) {
      continue;
    }
    ListAdd(&manager->areas, nearest, area, 0, PassMask(int32_t(view)));
  }
}

// Scenery::CollectModels: each model of the listed areas: its nearest
// corner (0 with the eye inside its rectangle), within the far distance +
// 50 and seen by a pass of `view`'s, added to the models' list.
void CollectModels(Guest& g, SceneryManager* manager, uint32_t view, game::Viewport* viewport) {
  guest::StackFrame frame(g.ctx, 320);
  CornersFrame* f = At<CornersFrame>(frame.Local(0));
  ScalarFloats(g);
  const float limit = viewport->views[0].unknown_72 + kModelMargin;
  SceneryList* areas = &manager->areas;
  for (uint32_t i = 0; i < areas->count(); i++) {
    const uint16_t area = areas->ItemAt(i)->area;
    game::SceneryAreaEntry* entry = AreaEntry(manager, area);
    if (entry == nullptr) {
      continue;  // (The original reads guest address 0 as the count.)
    }
    for (uint32_t m = 0, n = entry->model_count; m < n; m++) {
      game::SceneryModelBounds* bounds = entry->Model(m);
      const uint32_t model = bounds->model;
      float nearest = kFarthest;
      const game::Vector* corners = CopyCorners(g, f, bounds->corners);
      const float x = viewport->eye.x, z = viewport->eye.z;
      for (int k = 0; k < 4; k++) {
        const float d = CornerDistance(corners[k], x, z);
        if (!(d > nearest)) {
          nearest = d;
        }
      }
      if (limit < nearest) {
        continue;
      }
      if (Inside(x, z, corners)) {
        nearest = 0.0f;
      }
      const uint32_t mask = PassMask(int32_t(view));
      if (mask != 0) {
        ListAdd(&manager->models, nearest, area, model, mask);
      }
    }
  }
}

// Scenery::LimitLargeModels: when more models are listed than are kept,
// the large ones (model number from a threshold by area) among those kept
// past the 6th (the 11th with two views) are swapped with the next listed
// one that isn't large. Areas are 0 to 4 (at most five in every course).
void LimitLargeModels(SceneryManager* manager) {
  static constexpr uint32_t kLargeFrom[5] = {24, 11, 18, 9, 54};
  SceneryList* list = &manager->models;
  const uint32_t count = list->count();
  const uint32_t allowed = manager->config.last_view == 1 ? 10 : 5;
  auto large = [](const SceneryItem* item) {
    return !(kLargeFrom[uint32_t(item->area)] > uint32_t(item->model));
  };
  if (!(count > manager->model_counts[1]) || manager->model_counts[1] == 0) {
    return;
  }
  uint32_t larges = 0;
  for (uint32_t i = 0; i < manager->model_counts[1]; i++) {
    if (!large(list->ItemAt(i))) {
      continue;
    }
    if (!(allowed < larges)) {
      larges++;
      continue;
    }
    for (uint32_t j = i + 1; j < count - 1; j++) {
      if (!large(list->ItemAt(j))) {
        const uint32_t swap = *list->ItemSlot(j);
        *list->ItemSlot(j) = uint32_t(*list->ItemSlot(i));
        *list->ItemSlot(i) = swap;
        break;
      }
    }
  }
}

// A list sorted nearest first and cut to `keep` when it holds more than one.
void SortAndCut(SceneryList* list, uint32_t keep, SceneryManager* manager, bool limit_large) {
  const uint32_t count = list->count();
  if (!(count > 1)) {
    return;
  }
  ListSort(list, 0, int32_t(count - 1));
  if (limit_large) {
    LimitLargeModels(manager);
  }
  if (list->count() > keep) {
    list->count() = keep;
  }
}

// Scenery::Update: the enabled camera passes set from their viewports; both
// lists filled from each view (CollectAreas, then CollectModels), sorted
// and cut; loaded (all at once) or streamed; then the visible areas: each
// listed area that's in and seen, with its listed models that are in, of
// it and within viewport 0's far distance (each marked seen); streaming,
// the unseen released.
void Update(Guest& g, SceneryManager* manager) {
  const float far = ViewportAt(0)->views[0].unknown_72;
  for (int i = 0; i < kPassRecordCount; i++) {
    const game::ScenePassRecord* record = guest::Element<game::ScenePassRecord>(kPassRecords, uint32_t(i));
    game::CameraPass* pass = guest::Element<game::CameraPass>(game::kCameraPasses, uint32_t(int32_t(record->pass)));
    if (pass->enabled != 0) {
      g.Call(sub_820AA460, pass, record->player, record->view);  // CameraPass::FromViewport
    }
  }
  ListClear(&manager->areas);
  ListClear(&manager->models);
  for (int32_t view = 0; view < manager->config.last_view + 1; view++) {
    CollectAreas(g, manager, uint32_t(view), ViewportAt(uint32_t(view)));
  }
  SortAndCut(&manager->areas, manager->area_counts[1], manager, false);
  for (int32_t view = 0; view < manager->config.last_view + 1; view++) {
    CollectModels(g, manager, uint32_t(view), ViewportAt(uint32_t(view)));
  }
  SortAndCut(&manager->models, manager->model_counts[1], manager, true);

  const bool all_at_once = manager->config.mode == 0;
  if (all_at_once) {
    ListLoadAll(g, manager, &manager->areas);
    ListLoadAll(g, manager, &manager->models);
  } else {
    ListStream(g, manager, &manager->areas);
    ListStream(g, manager, &manager->models);
  }

  ScalarFloats(g);
  manager->visible_area_count = 0;
  for (uint32_t i = 0; i < manager->areas.count(); i++) {
    SceneryArea* visible = manager->VisibleArea(manager->visible_area_count);
    SceneryItem* area = manager->areas.ItemAt(i);
    PoolObject* lights = area->object;
    if (lights == nullptr || !IsLoaded(lights) || area->mask == 0) {
      continue;
    }
    visible->mask = area->mask;
    visible->model_count = 0;
    visible->lights = At<AreaLights>(guest::AddressOf(area->object.get()));
    for (uint32_t j = 0, models = manager->models.count(); j < models; j++) {
      SceneryItem* model = manager->models.ItemAt(j);
      if (model->mask == 0) {
        continue;
      }
      if (far < model->distance) {
        break;
      }
      if (model->area != area->area || model->object == nullptr || !IsLoaded(model->object)) {
        continue;
      }
      SetVisible(model->object, true);
      visible->models[visible->model_count] = model->object.address;
      visible->model_count += 1;
    }
    if (visible->model_count != 0) {
      SetVisible(area->object, true);
      manager->visible_area_count += 1;
    }
  }
  if (!all_at_once) {
    ListReleaseUnused(g, manager, &manager->areas);
    ListReleaseUnused(g, manager, &manager->models);
  }
}

// --- Scenery::DrawArea (each pass, each visible area) ---

void RecordHeadlightProjectors(Guest& g, game::SceneryDrawPass* pass);

// The draw keys the model draws read (writable).
constexpr uint32_t kDrawKind = 0x8296DD68;    // 163 for the scenery.
constexpr uint32_t kDrawKey = 0x8296DD6C;
constexpr uint32_t kDrawKey2 = 0x8296DD60;
constexpr uint32_t kDepthKey2 = 0x8296DD58;
constexpr uint32_t kDepthKey = 0x8296DD5C;
constexpr uint32_t kSceneView = 0x8264EA00;   // A Matrix: the pass's view.
constexpr uint32_t kSceneryDrawKind = 163;
constexpr uint32_t kLightsBit = 0x400;  // The manager's pass mask: draw the areas' lights.

// DrawArea's 400-byte frame: the identity (built at +80), the pass's
// view-projection (+144) and the world matrix (+208, the identity).
struct DrawAreaFrame {
  uint8_t unknown_0[80];
  game::Matrix scratch;
  game::Matrix view_projection;
  game::Matrix world;
  uint8_t unknown_272[128];
};
GUEST_OFFSET(DrawAreaFrame, view_projection, 144);
GUEST_OFFSET(DrawAreaFrame, world, 208);
GUEST_SIZE(DrawAreaFrame, 400);

bool IsShadowPass(int32_t kind) { return kind == 10 || kind == 13 || kind == 16; }

// The depth pass after a main pass's colors: for kinds 0, 1 and 2, when
// post effect 3, 7 or 11 is there and its +52 is 0, every shape's depth
// with both keys 116 (119 for kind 2).
// The start of a post effect (PostEffects::Get's result): a pass is skipped
// while its +52 is set.
struct PostEffectHead {
  uint8_t unknown_0[52];
  u32_be unknown_52;
};

void DrawAfterDepth(Guest& g, game::Model* model, DrawAreaFrame* f, int32_t kind) {
  static constexpr struct {
    int32_t kind;
    uint32_t effect, key;
  } kDepths[] = {{0, 3, 116}, {1, 7, 116}, {2, 11, 119}};
  for (const auto& depth : kDepths) {
    if (kind != depth.kind) {
      continue;
    }
    const PostEffectHead* effect =
        At<PostEffectHead>(g.Call(sub_821325C0, depth.effect));  // PostEffects::Get
    if (effect == nullptr || effect->unknown_52 != 0) {
      continue;
    }
    *At<u32_be>(kDepthKey) = depth.key;
    *At<u32_be>(kDepthKey2) = depth.key;
    for (int32_t shape = 0; shape < int32_t(uint32_t(model->shape_count)); shape++) {
      game::ModelDrawShapeDepth(g, model, &f->view_projection, &f->world, uint32_t(shape), 0, 0, 1);
    }
  }
}

// Scenery::DrawArea: a visible area in one pass, when the pass, the
// manager and the area share a bit. The headlights' projectors recorded;
// the draw keys and the pass's view set; each of the area's models drawn
// (shadow passes: its shapes' depth; else its textures (the course set's
// XPR in slot 1 while it has animations, the model's own XPR next), its
// shapes, then the depth after some main passes); then, with the lights
// bit outside the shadow passes, the area's light groups.
void DrawArea(Guest& g, SceneryManager* manager, game::SceneryDrawPass* pass, SceneryArea* area) {
  guest::StackFrame frame(g.ctx, 400);
  DrawAreaFrame* f = At<DrawAreaFrame>(frame.Local(0));
  CourseSet* set = manager->course_set;
  if ((pass->mask & manager->pass_mask & area->mask) == 0) {
    return;
  }
  RecordHeadlightProjectors(g, pass);
  game::CameraPass* camera = guest::Element<game::CameraPass>(game::kCameraPasses, uint32_t(int32_t(pass->camera_pass)));
  game::CameraPassKind* kind = At<game::CameraPassKind>(guest::AddressOf(camera));
  f->view_projection.right.w = 1.0f;
  f->view_projection.up.w = 1.0f;
  f->view_projection.forward.w = 1.0f;
  f->view_projection.position.w = 1.0f;
  f->scratch.right = {1.0f, 0.0f, 0.0f, 0.0f};
  f->scratch.up = {0.0f, 1.0f, 0.0f, 0.0f};
  f->scratch.forward = {0.0f, 0.0f, 1.0f, 0.0f};
  f->scratch.position = {0.0f, 0.0f, 0.0f, 1.0f};
  f->world = f->scratch;
  g.Call(sub_820A0E18, &f->view_projection, &camera->view_projection);  // Matrix::Copy
  *At<u32_be>(kDrawKind) = kSceneryDrawKind;
  *At<u32_be>(kDrawKey) = pass->key;
  *At<u32_be>(kDrawKey2) = pass->key_2;
  g.Call(sub_820A0E18, kSceneView, &camera->view);  // Matrix::Copy

  for (uint32_t i = 0, count = area->model_count; i < count; i++) {
    AreaModel* area_model = At<AreaModel>(area->models[i]);
    if (IsShadowPass(kind->kind)) {
      *At<u32_be>(kDepthKey2) = pass->key_2;
      *At<u32_be>(kDepthKey) = pass->key;
      SceneryModel* model = area_model->links.model;
      if (model == nullptr) {
        continue;
      }
      for (int32_t shape = 0; shape < int32_t(uint32_t(model->model.shape_count)); shape++) {
        game::ModelDrawShapeDepth(g, &model->model, &f->view_projection, &f->world, uint32_t(shape),
                                  0, 1, 1);
      }
      continue;
    }
    const uint32_t animations = set->parts.animations.address;
    SceneryModel* model = area_model->links.model;
    if (model == nullptr) {
      continue;  // (The original draws through guest address 0.)
    }
    uint32_t slot = 1;
    game::SceneryModelResetDrawState(model);
    if (animations != 0) {
      g.CallVirtual(&manager->textures, 4, 1u, &set->parts.xpr);  // TextureProvider::Set
      slot = 2;
    }
    model->animations = animations;
    if (area_model->links.xpr != nullptr) {
      g.CallVirtual(&manager->textures, 4, slot, area_model->links.xpr);
    }
    model->model.texture_provider = &manager->textures;
    const int32_t pass_kind = kind->kind;
    const uint32_t main_view = pass_kind == 0 ? 1 : 0;
    const uint32_t flags = (pass_kind == 0 || pass_kind == 1 || pass_kind == 2) ? 0 : 28;
    for (int32_t shape = 0; shape < int32_t(uint32_t(model->model.shape_count)); shape++) {
      game::SceneryModelDrawShape(g, model, &f->view_projection, &f->world, uint32_t(shape),
                                  main_view, nullptr, nullptr, flags);
    }
    DrawAfterDepth(g, &model->model, f, kind->kind);
  }

  if ((manager->pass_mask & kLightsBit) == 0 || IsShadowPass(kind->kind)) {
    return;
  }
  const uint32_t light_set = area->lights->light_set;
  if (light_set == 0) {
    return;
  }
  game::SetBillboardPass(pass, pass->camera, pass->key_2);
  DataFile* groups = At<DataFile>(light_set);
  for (uint32_t i = 0; i < groups->count; i++) {
    const uint32_t group = guest::AddressOf(groups->Entry(i, false));
    g.Call(sub_82146F00, pass, group, &set->parts.xpr, set->parts.animations);  // LightGroups::Draw
  }
}

// --- Scenery::DrawSetLists (each pass) ---

// The race progress (writable), for the point lists' light setting.
constexpr uint32_t kRaceProgressHolder = 0x826C7D90;

// DrawSetLists' 384-byte frame: the world matrix (the identity), the three
// lists' ranges, the pass's view-projection.
struct DrawSetListsFrame {
  uint8_t unknown_0[80];
  game::Matrix world;
  DrawRange range_c;
  DrawRange range_b;
  DrawRange range_a;
  game::Matrix view_projection;
  uint8_t unknown_256[128];
};
GUEST_OFFSET(DrawSetListsFrame, world, 80);
GUEST_OFFSET(DrawSetListsFrame, range_c, 144);
GUEST_OFFSET(DrawSetListsFrame, range_b, 160);
GUEST_OFFSET(DrawSetListsFrame, range_a, 176);
GUEST_OFFSET(DrawSetListsFrame, view_projection, 192);
GUEST_SIZE(DrawSetListsFrame, 384);

void SetIdentity(game::Matrix& m) {
  m.right = {1.0f, 0.0f, 0.0f, 0.0f};
  m.up = {0.0f, 1.0f, 0.0f, 0.0f};
  m.forward = {0.0f, 0.0f, 1.0f, 0.0f};
  m.position = {0.0f, 0.0f, 0.0f, 1.0f};
}

// The world the identity again and the view-projection copied from the
// pass (memcpy), before each list.
void ResetSetListsMatrices(Guest& g, DrawSetListsFrame* f, game::CameraPass* camera) {
  SetIdentity(f->world);
  Memcpy(g, guest::AddressOf(&f->view_projection), guest::AddressOf(&camera->view_projection),
         uint32_t(sizeof(game::Matrix)));
}

// The model of a model list's entry `index` (its node that many steps from
// the head).
SceneryModel* ListModel(ModelList* list, uint32_t index) {
  return At<SceneryModel>(*list->models.ItemAtPosition(index));
}

// One of the set lists' models drawn: the scenery's textures and the
// course set's animations, the range (10,000 to 50,000, from 0x820027CC
// and 0x820067DC), then its shape 0.
void DrawListModel(Guest& g, SceneryManager* manager, uint32_t animations, SceneryModel* model,
                   DrawSetListsFrame* f, DrawRange* range, bool reset) {
  if (reset) {
    game::SceneryModelResetDrawState(model);
  }
  model->model.texture_provider = &manager->textures;
  model->animations = animations;
  range->near = 10000.0f;
  range->far = 50000.0f;
  range->unknown_8 = 0.0f;
  range->unknown_12 = 0.0f;
  game::SceneryModelDrawShape(g, model, &f->view_projection, &f->world, 0, 0, nullptr, range, 0);
}

// Scenery::DrawSetLists: the course set's own lists in one pass. The
// set's XPR in the provider's slot 1, the draw keys and the pass's view
// set; then every model of the first list, the second and third lists'
// entries that are on (their draw state reset first), each with the
// identity world and the range 10,000 to 50,000; then the point list's
// entries that are on, with the race's current light setting.
void DrawSetLists(Guest& g, SceneryManager* manager, game::SceneryDrawPass* pass) {
  guest::StackFrame frame(g.ctx, 384);
  DrawSetListsFrame* f = At<DrawSetListsFrame>(frame.Local(0));
  CourseSet* set = manager->course_set;
  CourseParts& parts = set->parts;
  game::CameraPass* camera =
      guest::Element<game::CameraPass>(game::kCameraPasses, uint32_t(int32_t(pass->camera_pass)));
  for (game::Matrix* m : {&f->world, &f->view_projection}) {
    m->right.w = 1.0f;
    m->up.w = 1.0f;
    m->forward.w = 1.0f;
    m->position.w = 1.0f;
  }
  const uint32_t animations = parts.animations.address;
  g.CallVirtual(&manager->textures, 4, 1u, &parts.xpr);  // TextureProvider::Set
  *At<u32_be>(kDrawKey) = pass->key;
  *At<u32_be>(kDrawKey2) = pass->key_2;
  g.Call(sub_820A0E18, kSceneView, &camera->view);  // Matrix::Copy

  ModelList* all = &parts.lists_1532[0];
  ResetSetListsMatrices(g, f, camera);
  for (int32_t i = 0; i < all->count; i++) {
    DrawListModel(g, manager, animations, ListModel(all, uint32_t(i)), f, &f->range_a, false);
  }

  ModelList* b = &parts.lists_1532[1];
  ResetSetListsMatrices(g, f, camera);
  for (int32_t i = 0; i < b->count; i++) {
    if (game::ModelListIsBEntryOn(uint32_t(i))) {
      DrawListModel(g, manager, animations, ListModel(b, uint32_t(i)), f, &f->range_b, true);
    }
  }

  ModelList* c = &parts.lists_1532[2];
  if (c->count > 0) {
    ResetSetListsMatrices(g, f, camera);
    for (int32_t i = 0; i < c->count; i++) {
      if (game::ModelListIsCEntryOn(g, uint32_t(i))) {
        DrawListModel(g, manager, animations, ListModel(c, uint32_t(i)), f, &f->range_c, true);
      }
    }
  }

  // The point list's entries, its first point group the race's current
  // light setting.
  PointList* points = &parts.point_list;
  if (points->point_buffer_count > 0) {
    ResetSetListsMatrices(g, f, camera);
    for (int32_t i = 0; i < points->point_buffer_count; i++) {
      if (!game::PointListIsEntryOn(i)) {
        continue;
      }
      game::RaceProgress* progress = At<game::RaceProgress>(*At<u32_be>(kRaceProgressHolder));
      game::RaceLightSettings* lights =
          At<game::RaceLightSettings>(guest::AddressOf(&progress->entry));
      points->texture_provider = guest::AddressOf(&manager->textures);
      points->animations = animations;
      points->first_group =
          guest::Element<game::RaceLightSetting>(guest::AddressOf(lights->settings),
                                                 uint32_t(int32_t(lights->current)))->value;
      game::PointListDraw(g, points, uint32_t(i), guest::AddressOf(camera));
    }
  }
}

// --- Scenery::RecordHeadlightProjectors (each area drawn, each pass) ---

constexpr uint32_t kCarScene = 0x826B2B24;      // Holds the car scene (writable).
constexpr uint32_t kRenderManager = 0x826AA880;  // Holds the render manager.
// The headlights' eight slots, six arrays (writable): positions, beam
// shapes, the two colors, directions, the scene's extra.
constexpr uint32_t kLightPositions = 0x8296DE40;
constexpr uint32_t kLightBeams = 0x8296DEC0;
constexpr uint32_t kLightColors = 0x8296DF40;
constexpr uint32_t kLightColors2 = 0x8296DFC0;
constexpr uint32_t kLightDirections = 0x8296E040;
constexpr uint32_t kLightExtras = 0x8296E0C0;
constexpr int kLightSlots = 8;
constexpr int kProjectorSlots = 5;
constexpr uint32_t kSceneViewCenter = 0x8264EA60;  // A Vector.
constexpr uint32_t kSceneEye = 0x8264EA40;         // A Vector.
constexpr int32_t kMirrorPass = 2;  // Its headlights 0 and 1 trade slots.
// The projector's near distance and the reach (0x82001F94, 0x82002F88).
constexpr float kProjectorNear = 0.1f;
constexpr float kProjectorFar = 300.0f;
// The render queue's keys for the projectors.
constexpr uint32_t kProjectorKeyMirror = 371;
constexpr uint32_t kProjectorKey = 150;

// RecordHeadlightProjectors' 640-byte frame, at the original's offsets.
struct HeadlightsFrame {
  uint8_t unknown_0[80];
  game::Vector away;       // The beam's direction, negated.
  game::Vector direction;  // `away` normalized.
  game::Vector position;   // The lamp in the world.
  game::Vector up;         // (0, 1, 0, 0), turned by each car's matrix in turn.
  game::Vector beam;       // The car's beam direction in the world.
  game::Vector color;
  game::Vector color_2;
  game::Vector target;
  game::Vector lamp_offset;
  game::Vector beam_shape;
  game::Vector extra;
  game::Matrix transform;  // The car's (its position row cleared after the lamp's).
  game::Vector car_beam;  // Scene model's beam (sub_820DA790).
  game::Matrix projector;
  game::Vector out_400;
  game::Vector out_416;
  game::Vector out_432;
  uint8_t unknown_448[192];
};
GUEST_OFFSET(HeadlightsFrame, away, 80);
GUEST_OFFSET(HeadlightsFrame, beam, 144);
GUEST_OFFSET(HeadlightsFrame, target, 192);
GUEST_OFFSET(HeadlightsFrame, transform, 256);
GUEST_OFFSET(HeadlightsFrame, car_beam, 320);
GUEST_OFFSET(HeadlightsFrame, projector, 336);
GUEST_OFFSET(HeadlightsFrame, out_400, 400);
GUEST_SIZE(HeadlightsFrame, 640);

game::Vector* Slot(uint32_t array, int slot) { return guest::Element<game::Vector>(array, uint32_t(slot)); }

void SetVector(game::Vector& v, float x, float y, float z, float w) {
  v.x = x;
  v.y = y;
  v.z = z;
  v.w = w;
}

// The normalize the original does in vector registers (vmsum3fp,
// vrsqrtefp, one Newton step; 0 stays 0), with the generated code's
// operations, flush-to-zero on as it has it.
game::Vector NormalizeVector(Guest& g, const game::Vector& v) {
  g.ctx.fpscr.enableFlushModeUnconditional();
  // Lanes as the recompiled code keeps a vector register: element 0 last.
  alignas(16) float in[4] = {v.w, v.z, v.y, v.x};
  const simde__m128 a = simde_mm_load_ps(in);
  const simde__m128 dot = simde_mm_dp_ps(a, a, 0xEF);
  const simde__m128 half = simde_mm_set1_ps(0.5f);
  simde__m128 estimate = simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(dot));
  const simde__m128 half_dot = simde_mm_mul_ps(dot, half);
  const simde__m128 zero = simde_mm_cmpeq_ps(dot, simde_mm_setzero_ps());
  const simde__m128 squared = simde_mm_mul_ps(estimate, estimate);
  const simde__m128 step = simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(half_dot, squared), half),
                                           simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
  estimate = simde_mm_add_ps(simde_mm_mul_ps(estimate, step), estimate);
  const simde__m128 scaled = simde_mm_mul_ps(a, estimate);
  const simde__m128i picked =
      simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_castps_si128(zero), simde_mm_castps_si128(scaled)),
                        simde_mm_and_si128(simde_mm_castps_si128(zero), simde_mm_castps_si128(dot)));
  alignas(16) float out[4];
  simde_mm_store_ps(out, simde_mm_castsi128_ps(picked));
  game::Vector r;
  SetVector(r, out[3], out[2], out[1], out[0]);
  return r;
}

void Transform(Guest& g, game::Vector* out, game::Matrix* m, game::Vector* v) {
  g.Call(sub_820AB518, out, m, v);  // Vector::Transform
}

// The render manager's command list (its slot 8, then that's slot 1), as
// the original asks for it before every command list call.
uint32_t CommandList(Guest& g) {
  const uint32_t manager = *At<u32_be>(kRenderManager);
  const uint32_t device = g.CallVirtual(At<uint8_t>(manager), 8);
  return g.CallVirtual(At<uint8_t>(device), 1);
}

// The command words a projector is recorded with.
struct CommandHeader5 {
  uint8_t unknown_0[2];
  guest::u16_be kind;
  u32_be unknown_4;
};
struct VertexConstantCommand {
  u32_be command;  // 7
  u32_be count;    // 4
  u32_be target;   // The four vertex constants the matrix goes to.
};
struct EndCommand {
  u32_be command;  // 36
  u32_be unknown_4;
  u32_be unknown_8;
};

// A projector for headlight `slot` (Projector::Build from the lamp, its
// target and up, the scene's field of view) recorded on the command list
// and handed to the render queue.
void RecordProjector(Guest& g, HeadlightsFrame* f, game::CarSceneHeadlights* scene, int slot,
                     int32_t camera_pass) {
  ScalarFloats(g);
  f->projector.right.w = 1.0f;
  f->projector.up.w = 1.0f;
  f->projector.forward.w = 1.0f;
  f->projector.position.w = 1.0f;
  f->target.x = f->position.x - f->direction.x;
  f->target.y = f->position.y - f->direction.y;
  f->target.z = f->position.z - f->direction.z;
  f->target.w = 1.0f;
  g.ctx.f1.f64 = double(float(scene->extra.w));
  g.ctx.f2.f64 = 1.0;
  g.ctx.f3.f64 = double(kProjectorNear);
  g.ctx.f4.f64 = double(kProjectorFar);
  g.Call(sub_8257CF28, &f->projector, &f->position, &f->target, &f->up);  // Projector::Build

  g.Call(sub_82572568, CommandList(g));  // CommandList::OpenChunk
  CommandHeader5* header = At<CommandHeader5>(g.Call(sub_82572638, CommandList(g), 1u));  // CommandList::AllocateSlots
  header->unknown_4 = 0;
  header->kind = 5;
  VertexConstantCommand* constants =
      At<VertexConstantCommand>(g.Call(sub_82572638, CommandList(g), 1u));
  constants->command = 7;
  constants->count = 4;
  constants->target = ((uint32_t(slot) << 10) + 0x7700) | 0x10000 | 4;
  const uint32_t matrix = g.Call(sub_82572730, CommandList(g), 64u, 0u);  // CommandList::Allocate
  g.Call(sub_820A0E18, matrix, &f->projector);                          // Matrix::Copy
  EndCommand* end = At<EndCommand>(g.Call(sub_82572638, CommandList(g), 1u));
  end->unknown_4 = 0;
  end->unknown_8 = 0;
  end->command = 36;
  g.Call(sub_825725E8, CommandList(g));  // CommandList::CloseChunk

  const uint32_t manager = *At<u32_be>(kRenderManager);
  const uint32_t device = g.CallVirtual(At<uint8_t>(manager), 8);
  const uint32_t queue = g.CallVirtual(At<uint8_t>(device), 2);
  g.ctx.f1.f64 = 0.0;
  g.CallVirtual(At<uint8_t>(queue), 4, header,
                camera_pass == kMirrorPass ? kProjectorKeyMirror : kProjectorKey);
}

// Scenery::RecordHeadlightProjectors: the pass's camera set from its
// viewport; with a player car, the headlights of the scene's first eight
// car models with a car (kinds 1 and 2) into the eight slots (0 and 1
// traded in the mirror pass): position, beam shape, colors, direction and
// the scene's extra, and for the first five a projector recorded; then the
// scene's view center copied. Always the draw keys and the pass's eye.
void RecordHeadlightProjectors(Guest& g, game::SceneryDrawPass* pass) {
  guest::StackFrame frame(g.ctx, 640);
  HeadlightsFrame* f = At<HeadlightsFrame>(frame.Local(0));
  game::CameraPass* camera =
      guest::Element<game::CameraPass>(game::kCameraPasses, uint32_t(int32_t(pass->camera_pass)));
  pass->camera = guest::AddressOf(camera);
  g.Call(sub_820AA460, camera, pass->player, pass->viewport_view);  // CameraPass::FromViewport
  if (g.Call(sub_820C0288, 0) != 0) {  // Vehicle::Get
    game::CarSceneHeadlights* scene = At<game::CarSceneHeadlights>(*At<u32_be>(kCarScene));
    ScalarFloats(g);
    SetVector(f->direction, 0.0f, 0.0f, 0.0f, 0.0f);
    SetVector(f->away, 0.0f, 0.0f, 0.0f, 0.0f);
    for (int slot = 0; slot < kLightSlots; slot++) {
      *Slot(kLightColors, slot) = f->direction;
      *Slot(kLightColors2, slot) = f->away;
    }
    f->lamp_offset = scene->lamp_offset;
    SetVector(f->up, 0.0f, 1.0f, 0.0f, 0.0f);
    f->position.w = 1.0f;
    f->beam.w = 1.0f;
    const int32_t count = scene->model_count < kLightSlots ? int32_t(scene->model_count) : kLightSlots;
    int next_slot = 0;
    for (uint32_t i = 0; i < uint32_t(count); i++) {
      const uint32_t list = guest::AddressOf(&At<game::CarSceneHeadlights>(*At<u32_be>(kCarScene))->models);
      game::SceneModel* model = At<game::SceneModel>(g.Call(sub_82579570, list, i));  // List::At
      if (model == nullptr || model->state != 3 || model->car_model == nullptr) {
        continue;
      }
      const uint32_t kind = model->record.kind;
      if (kind != 2 && kind != 1) {
        continue;
      }
      g.Call(sub_820DA790, &f->car_beam, model);  // SceneModel::BeamDirection
      int slot = next_slot++;
      if (slot >= kLightSlots) {
        break;
      }
      if (uint32_t(int32_t(pass->camera_pass)) == uint32_t(kMirrorPass)) {
        slot = slot == 0 ? 1 : slot == 1 ? 0 : slot;
      }
      Memcpy(g, guest::AddressOf(&f->transform), guest::AddressOf(&model->transform), 64);
      Transform(g, &f->position, &f->transform, &f->lamp_offset);
      SetVector(f->transform.position, 0.0f, 0.0f, 0.0f, 0.0f);  // Directions: no translation.
      Transform(g, &f->beam, &f->transform, &f->car_beam);
      Transform(g, &f->up, &f->transform, &f->up);
      ScalarFloats(g);
      SetVector(f->away, -f->beam.x, -f->beam.y, -f->beam.z, -f->beam.w);
      f->extra = scene->extra;
      f->direction = NormalizeVector(g, f->away);
      f->beam_shape = *At<game::Vector>(g.Call(sub_820DA7D8, &f->out_400, model));  // the beam shape
      const game::Vector* color = At<game::Vector>(g.Call(sub_820DA700, &f->out_416, model));
      ScalarFloats(g);
      f->color.w = 1.0f;
      f->color.x = color->x * scene->color_scale.x;
      f->color.y = scene->color_scale.y * color->y;
      f->color.z = scene->color_scale.z * color->z;
      const game::Vector* color_2 = At<game::Vector>(g.Call(sub_820DA700, &f->out_432, model));
      ScalarFloats(g);
      f->color_2.x = scene->color_scale_2.x * color_2->x;
      f->color_2.y = scene->color_scale_2.y * color_2->y;
      f->color_2.z = scene->color_scale_2.z * color_2->z;
      f->color_2.w = 1.0f;
      *Slot(kLightPositions, slot) = f->position;
      *Slot(kLightColors, slot) = f->color;
      *Slot(kLightDirections, slot) = f->direction;
      *Slot(kLightColors2, slot) = f->color_2;
      *Slot(kLightExtras, slot) = f->extra;
      *Slot(kLightBeams, slot) = f->beam_shape;
      if (slot < kProjectorSlots) {
        RecordProjector(g, f, scene, slot, pass->camera_pass);
      }
    }
    *At<game::Vector>(kSceneViewCenter) = scene->view_center;
  }
  *At<u32_be>(kDrawKey) = pass->key;
  *At<u32_be>(kDrawKey2) = pass->key_2;
  *At<game::Vector>(kSceneEye) = At<game::CameraPass>(pass->camera)->eye;
}

// --- Scenery::Draw (each pass) ---

// The draw's pass records (writable; 24 bytes each: the pass description's
// player, view, camera pass, keys and mask): the sky (3), the set's own
// models (3), the areas (10), the set lists' passes (3, also the areas'
// second round and the light groups'), the minimap (1).
struct ScenePassRecord24 {
  i32_be player;
  i32_be viewport_view;
  i32_be camera_pass;
  u32_be key;
  u32_be key_2;
  u32_be mask;
};
GUEST_SIZE(ScenePassRecord24, 24);
constexpr uint32_t kSkyPasses = 0x825F2FE8;
constexpr uint32_t kSetModelPasses = 0x825F3030;
constexpr uint32_t kAreaPasses = 0x825F3078;
constexpr uint32_t kSetPasses = 0x825F3168;
constexpr uint32_t kMinimapPass = 0x825F31B0;
constexpr uint32_t kNoTextureAnimations = 0x4000;  // The manager's pass mask bits.
constexpr uint32_t kDrawSky = 0x10;
constexpr uint32_t kDrawSetModels = 0x20;
// RecordClear's arguments for the minimap's pass, and where its two words
// are (camera pass 19's +564 and +572).
constexpr uint32_t kClearCommand = 19;
constexpr uint32_t kClearKey = 103;
constexpr uint32_t kClearFlags = 49;
constexpr uint32_t kClearColor = game::kCameraPasses + 11812;
constexpr uint32_t kClearDepth = game::kCameraPasses + 11820;

// Scenery::Draw's 640-byte frame: the pass description its callees get
// (+80), the identity (+128), the sky's range (+192), the worlds and
// view-projections of the sky (+208, +336) and the set's models (+272,
// +400).
struct SceneryDrawFrame {
  uint8_t unknown_0[80];
  game::SceneryDrawPass pass;
  uint8_t unknown_116[12];
  game::Matrix identity;
  DrawRange sky_range;
  game::Matrix sky_world;
  game::Matrix set_world;
  game::Matrix sky_view_projection;
  game::Matrix set_view_projection;
  uint8_t unknown_464[176];
};
GUEST_OFFSET(SceneryDrawFrame, pass, 80);
GUEST_OFFSET(SceneryDrawFrame, identity, 128);
GUEST_OFFSET(SceneryDrawFrame, sky_range, 192);
GUEST_OFFSET(SceneryDrawFrame, sky_world, 208);
GUEST_OFFSET(SceneryDrawFrame, set_world, 272);
GUEST_OFFSET(SceneryDrawFrame, sky_view_projection, 336);
GUEST_OFFSET(SceneryDrawFrame, set_view_projection, 400);
GUEST_SIZE(SceneryDrawFrame, 640);

const ScenePassRecord24* PassRecord(uint32_t table, int index) {
  return guest::Element<ScenePassRecord24>(table, uint32_t(index));
}

game::CameraPass* CameraPassOf(int32_t index) {
  return guest::Element<game::CameraPass>(game::kCameraPasses, uint32_t(index));
}

// The record's words into the frame's pass description.
void SetPass(game::SceneryDrawPass& pass, const ScenePassRecord24* record) {
  pass.player = record->player;
  pass.viewport_view = record->viewport_view;
  pass.camera_pass = record->camera_pass;
  pass.key = record->key;
  pass.key_2 = record->key_2;
  pass.mask = record->mask;
}

void SetRowsW(game::Matrix& m, float w) {
  m.right.w = w;
  m.up.w = w;
  m.forward.w = w;
  m.position.w = w;
}

// The pass's view for the model draws: the keys and the scene view.
void SetPassView(Guest& g, SceneryManager* manager, game::SceneryDrawPass& pass, game::CameraPass* camera) {
  g.CallVirtual(&manager->textures, 4, 1u, &manager->course_set->parts.xpr);  // TextureProvider::Set
  *At<u32_be>(kDrawKey) = pass.key;
  *At<u32_be>(kDrawKey2) = pass.key_2;
  g.Call(sub_820A0E18, kSceneView, &camera->view);  // Matrix::Copy
}

// The sky (the set's model): each enabled sky pass, its world the identity
// at the camera's eye, every shape with the range 5,000 to 50,000.
void DrawSky(Guest& g, SceneryManager* manager, SceneryDrawFrame* f) {
  CourseSet* set = manager->course_set;
  for (int i = 0; i < 3; i++) {
    const ScenePassRecord24* record = PassRecord(kSkyPasses, i);
    game::CameraPass* camera = CameraPassOf(record->camera_pass);
    if (camera->enabled == 0) {
      continue;
    }
    SetRowsW(f->sky_world, 1.0f);
    SetRowsW(f->sky_view_projection, 1.0f);
    game::Model* model = &set->parts.model;
    SetPass(f->pass, record);
    RecordHeadlightProjectors(g, &f->pass);
    SetIdentity(f->identity);
    f->sky_world = f->identity;
    f->sky_world.position.x = camera->eye.x;
    f->sky_world.position.y = camera->eye.y;
    f->sky_world.position.z = camera->eye.z;
    g.Call(sub_820A0E18, &f->sky_view_projection, &camera->view_projection);  // Matrix::Copy
    SetPassView(g, manager, f->pass, camera);
    model->texture_provider = &manager->textures;
    for (int32_t shape = 0; shape < int32_t(uint32_t(model->shape_count)); shape++) {
      f->sky_range.near = 5000.0f;  // The float at 0x82001FA4.
      f->sky_range.far = 50000.0f;
      f->sky_range.unknown_8 = 0.0f;
      f->sky_range.unknown_12 = 0.0f;
      game::ModelDrawShape(g, model, &f->sky_view_projection, &f->sky_world, uint32_t(shape), 0,
                           nullptr, &f->sky_range);
    }
  }
}

// The set's own model list (+464): each enabled pass, every model within
// the view's far distance (its first mesh's +236), with the identity
// world.
void DrawSetModels(Guest& g, SceneryManager* manager, SceneryDrawFrame* f) {
  CourseSet* set = manager->course_set;
  ModelList* list = &set->parts.list_464;
  for (int i = 0; i < 3; i++) {
    const ScenePassRecord24* record = PassRecord(kSetModelPasses, i);
    game::CameraPass* camera = CameraPassOf(record->camera_pass);
    if (camera->enabled == 0) {
      continue;
    }
    SetRowsW(f->set_world, 1.0f);
    SetRowsW(f->set_view_projection, 1.0f);
    SetPass(f->pass, record);
    RecordHeadlightProjectors(g, &f->pass);
    SetIdentity(f->identity);
    f->set_world = f->identity;
    g.Call(sub_820A0E18, &f->set_view_projection, &camera->view_projection);  // Matrix::Copy
    SetPassView(g, manager, f->pass, camera);
    for (int32_t m = 0; m < list->count; m++) {
      SceneryModel* model = ListModel(list, uint32_t(m));
      game::SceneryModelResetDrawState(model);
      game::Viewport* viewport = ViewportAt(uint32_t(int32_t(record->player)));
      const float far = viewport->views[uint32_t(int32_t(record->viewport_view))].unknown_72;
      ScalarFloats(g);
      const float reach = model->model.MeshAt(0)->unknown_236;
      if (reach > far) {
        continue;
      }
      model->model.texture_provider = &manager->textures;
      game::SceneryModelDrawShape(g, model, &f->set_view_projection, &f->set_world, 0, 0, nullptr,
                                  nullptr, 0);
    }
  }
}

// Each visible area with models in each enabled pass of a table.
void DrawAreas(Guest& g, SceneryManager* manager, SceneryDrawFrame* f, uint32_t table, int passes) {
  for (uint32_t a = 0; a < manager->visible_area_count; a++) {
    SceneryArea* area = manager->VisibleArea(a);
    if (area->model_count == 0) {
      continue;
    }
    for (int i = 0; i < passes; i++) {
      const ScenePassRecord24* record = PassRecord(table, i);
      if (CameraPassOf(record->camera_pass)->enabled == 0) {
        continue;
      }
      SetPass(f->pass, record);
      DrawArea(g, manager, &f->pass, area);
    }
  }
}

// Scenery::Draw: the course for one frame's passes. The texture provider
// reset; the texture animations stepped and the visible models' animations
// applied (unless the manager's 0x4000); the set's XPR in slot 0; then the
// sky (0x10), the set's models (0x20), the visible areas (twice, two pass
// tables), the set lists, the light groups (0x400) and, when its pass is
// on, the minimap after its clear.
void DrawScenery(Guest& g, SceneryManager* manager) {
  guest::StackFrame frame(g.ctx, 640);
  SceneryDrawFrame* f = At<SceneryDrawFrame>(frame.Local(0));
  CourseSet* set = manager->course_set;
  g.CallVirtual(&manager->textures, 2);  // TextureProvider::Reset
  if ((~uint32_t(manager->pass_mask) & kNoTextureAnimations) != 0) {
    g.Call(sub_820F1038, set);  // TextureAnimations::Advance
    for (uint32_t a = 0; a < manager->visible_area_count; a++) {
      SceneryArea* area = manager->VisibleArea(a);
      for (uint32_t m = 0; m < area->model_count; m++) {
        g.Call(sub_8257A460, At<AreaModel>(area->models[m])->links.model);  // Model::Animate (wrapped)
      }
    }
  }
  g.CallVirtual(&manager->textures, 4, 0u, &set->parts.xpr);  // TextureProvider::Set
  if ((manager->pass_mask & kDrawSky) != 0) {
    DrawSky(g, manager, f);
  }
  if ((manager->pass_mask & kDrawSetModels) != 0) {
    DrawSetModels(g, manager, f);
  }
  DrawAreas(g, manager, f, kAreaPasses, 10);
  DrawAreas(g, manager, f, kSetPasses, 3);
  for (int i = 0; i < 3; i++) {
    const ScenePassRecord24* record = PassRecord(kSetPasses, i);
    if (CameraPassOf(record->camera_pass)->enabled == 0) {
      continue;
    }
    SetPass(f->pass, record);
    DrawSetLists(g, manager, &f->pass);
  }
  if ((manager->pass_mask & kLightsBit) != 0) {
    for (int i = 0; i < 3; i++) {
      const ScenePassRecord24* record = PassRecord(kSetPasses, i);
      game::CameraPass* camera = CameraPassOf(record->camera_pass);
      if (camera->enabled == 0 ||
          IsShadowPass(At<game::CameraPassKind>(guest::AddressOf(camera))->kind)) {
        continue;
      }
      const uint32_t groups = set->parts.light_groups;
      if (groups == 0) {
        continue;
      }
      SetPass(f->pass, record);
      RecordHeadlightProjectors(g, &f->pass);
      game::SetBillboardPass(&f->pass, f->pass.camera, f->pass.key_2);
      DataFile* file = At<DataFile>(groups);
      for (uint32_t k = 0; k < file->count; k++) {
        const uint32_t group = guest::AddressOf(file->Entry(k, false));
        g.Call(sub_82146F00, &f->pass, group, &set->parts.xpr, set->parts.animations);  // LightGroups::Draw
      }
    }
  }
  const ScenePassRecord24* minimap = PassRecord(kMinimapPass, 0);
  if (CameraPassOf(minimap->camera_pass)->enabled != 0) {
    g.Call(sub_8209FB30, kClearCommand, kClearKey, kClearFlags, *At<u32_be>(kClearColor),
           *At<u32_be>(kClearDepth));  // RecordClear
    SetPass(f->pass, minimap);
    RecordHeadlightProjectors(g, &f->pass);
    g.Call(sub_820F5610, manager, &f->pass);  // Minimap::Draw
  }
}

// AnimationSet::FindByTexture: the animation of this texture, or -1. (An
// animation without keys has none; the original reads guest address 4.)
int32_t FindAnimationByTexture(AnimationSet* set, uint32_t texture) {
  const int32_t count = int32_t(uint32_t(set->count));
  for (int32_t i = 0; i < count; i++) {
    Animation* animation = set->Entry(uint32_t(i));
    if (animation->key_count > 0 && *animation->Texture() == texture) {
      return i;
    }
  }
  return -1;
}

}  // namespace

#define SBX_COURSE_SCENERY_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                    \
  if (!On()) {                            \
    __imp__##name(ctx, base);             \
    return;                               \
  }                                       \
  Guest g(ctx, base);

// --- Pool objects ---

// PoolObject::Reset: r3 the object, returned. No flags or countdown.
extern "C" REX_FUNC(sub_820FB8B8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB8B8)
  ResetObject(At<PoolObject>(ctx.r3.u32));
}

// PoolObject::AllocData: r3 the object's data, r4 bytes (rounded up to
// 64) from it; returns where they start.
extern "C" REX_FUNC(sub_820FB798) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB798)
  ctx.r3.u64 = AllocData(At<PoolData>(ctx.r3.u32), ctx.r4.u32);
}

// PoolObject::InUse, IsLoaded, IsVisible, IsExpiring: r3 the object.
extern "C" REX_FUNC(sub_820FB7C0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB7C0)
  ctx.r3.u64 = InUse(At<PoolObject>(ctx.r3.u32)) ? 1 : 0;
}

extern "C" REX_FUNC(sub_820FB7D0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB7D0)
  ctx.r3.u64 = IsLoaded(At<PoolObject>(ctx.r3.u32)) ? 1 : 0;
}

extern "C" REX_FUNC(sub_820FB7E8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB7E8)
  ctx.r3.u64 = IsVisible(At<PoolObject>(ctx.r3.u32)) ? 1 : 0;
}

extern "C" REX_FUNC(sub_820FB800) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB800)
  ctx.r3.u64 = IsExpiring(At<PoolObject>(ctx.r3.u32)) ? 1 : 0;
}

// PoolObject::SetVisible and SetExpiring: r3 the object, r4 on (its low
// word not 0) or off.
extern "C" REX_FUNC(sub_820FB818) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB818)
  SetVisible(At<PoolObject>(ctx.r3.u32), ctx.r4.u32 != 0);
}

extern "C" REX_FUNC(sub_820FB848) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB848)
  SetExpiring(At<PoolObject>(ctx.r3.u32), ctx.r4.u32 != 0);
}

// PoolObject::ClearLoadFlags and MarkLoaded.
extern "C" REX_FUNC(sub_820FB878) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB878)
  ClearLoadFlags(At<PoolObject>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820FB888) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB888)
  MarkLoaded(At<PoolObject>(ctx.r3.u32));
}

// The keys (each class's vtable slot 0): the key's size into [r4], its
// address in r3. PoolObject::Key (shared): none.
extern "C" REX_FUNC(sub_824A7668) {
  SBX_COURSE_SCENERY_FALLBACK(sub_824A7668)
  *At<u32_be>(ctx.r4.u32) = 0;
  ctx.r3.u64 = 0;
}

// AreaLights::Key: its 4-byte key.
extern "C" REX_FUNC(sub_820F1AB0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1AB0)
  *At<u32_be>(ctx.r4.u32) = uint32_t(sizeof(u32_be));
  ctx.r3.u64 = guest::AddressOf(&At<AreaLights>(ctx.r3.u32)->key);
}

// AreaModel::Key: its 4-byte key.
extern "C" REX_FUNC(sub_820F1B68) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1B68)
  *At<u32_be>(ctx.r4.u32) = uint32_t(sizeof(u32_be));
  ctx.r3.u64 = guest::AddressOf(&At<AreaModel>(ctx.r3.u32)->key);
}

// PoolObject::PoolObject: r3 the object, r4 its own size (where its data
// starts), r5 the data's size, r6 its index.
extern "C" REX_FUNC(sub_820FB9F0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB9F0)
  ConstructPoolObject(At<PoolObject>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// PoolObject::SetKey: r4 the key's source.
extern "C" REX_FUNC(sub_820FB8D0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB8D0)
  SetKey(g, At<PoolObject>(ctx.r3.u32), ctx.r4.u32);
}

// PoolObject::KeyEquals: r4 the key to compare with; 1 or 0.
extern "C" REX_FUNC(sub_820FB930) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB930)
  const uint32_t source = ctx.r4.u32;
  ctx.r3.u64 = KeyEquals(g, At<PoolObject>(ctx.r3.u32), source) ? 1 : 0;
}

// --- The pools: r3 the pool set, r4 a pool ---

// ObjectPools::Pool: r4's record.
extern "C" REX_FUNC(sub_820FB9E0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FB9E0)
  ctx.r3.u64 = guest::AddressOf(At<ObjectPools>(ctx.r3.u32)->Pool(ctx.r4.u32));
}

// ObjectPools::Object: pool r4's object r5.
extern "C" REX_FUNC(sub_820FBFB8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBFB8)
  ctx.r3.u64 = PoolObjectAddress(At<ObjectPools>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// ObjectPools::Load: pool r4's object r5 loaded; returns it.
extern "C" REX_FUNC(sub_820FBEA0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBEA0)
  ctx.r3.u64 = LoadObject(g, At<ObjectPools>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// ObjectPools::Acquire: r5 the key's source; the object's index, or -1.
extern "C" REX_FUNC(sub_820FBDD8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBDD8)
  const uint32_t pool = ctx.r4.u32, key = ctx.r5.u32;
  ctx.r3.s64 = Acquire(g, At<ObjectPools>(ctx.r3.u32), pool, key);
}

// ObjectPools::Find: r5 the key; the object's index, or -1.
extern "C" REX_FUNC(sub_820FBD50) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBD50)
  const uint32_t pool = ctx.r4.u32, key = ctx.r5.u32;
  ctx.r3.s64 = Find(g, At<ObjectPools>(ctx.r3.u32), pool, key);
}

// ObjectPools::Release: pool r4's object r5; returns the next used object.
extern "C" REX_FUNC(sub_820FBF08) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBF08)
  const uint32_t pool = ctx.r4.u32, index = ctx.r5.u32;
  ctx.r3.u64 = Release(g, At<ObjectPools>(ctx.r3.u32), pool, index);
}

// ObjectPools::Init: r4 the pool count, r5 their descriptors.
extern "C" REX_FUNC(sub_820FBAB8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBAB8)
  ObjectPools* pools = At<ObjectPools>(ctx.r3.u32);
  const int32_t count = int32_t(ctx.r4.u32);
  const PoolDescriptor* descriptors = At<PoolDescriptor>(ctx.r5.u32);
  InitPools(g, pools, count, descriptors);
  ctx.r3.u64 = pools->pools;
}

// ObjectPools::Shutdown.
extern "C" REX_FUNC(sub_820FBC98) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820FBC98)
  ShutdownPools(g, At<ObjectPools>(ctx.r3.u32));
}

// Scenery::MakePoolObject: r3 the pool, r4 the object, r5 its own size, r6
// its data's, r7 its index; returns the object.
extern "C" REX_FUNC(sub_820F24C8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F24C8)
  const uint32_t pool = ctx.r3.u32, object = ctx.r4.u32;
  MakePoolObject(g, pool, object, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  ctx.r3.u64 = object;
}

// --- The lists: r3 the manager, r4 the list ---

// SceneryList::Clear.
extern "C" REX_FUNC(sub_820F1418) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1418)
  ListClear(At<SceneryList>(ctx.r4.u32));
}

// SceneryList::Add: f1 the distance, r6 the area, r7 the model, r8 the
// mask.
extern "C" REX_FUNC(sub_820F1718) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1718)
  ctx.fpscr.disableFlushMode();
  ListAdd(At<SceneryList>(ctx.r4.u32), ctx.f1.f64, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}

// SceneryList::Sort: r5 the first item, r6 the last.
extern "C" REX_FUNC(sub_820F17C0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F17C0)
  ctx.fpscr.disableFlushMode();
  ListSort(At<SceneryList>(ctx.r4.u32), int32_t(ctx.r5.u32), int32_t(ctx.r6.u32));
}

// SceneryList::LoadAll and Stream: 1 when all are in.
extern "C" REX_FUNC(sub_820F1428) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1428)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  SceneryList* list = At<SceneryList>(ctx.r4.u32);
  ctx.r3.u64 = ListLoadAll(g, manager, list) ? 1 : 0;
}

extern "C" REX_FUNC(sub_820F1550) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1550)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  SceneryList* list = At<SceneryList>(ctx.r4.u32);
  ctx.r3.u64 = ListStream(g, manager, list) ? 1 : 0;
}

// Scenery::DrawArea: r3 the manager, r4 the pass, r5 the visible area.
extern "C" REX_FUNC(sub_820F3A40) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F3A40)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  game::SceneryDrawPass* pass = At<game::SceneryDrawPass>(ctx.r4.u32);
  DrawArea(g, manager, pass, At<SceneryArea>(ctx.r5.u32));
}

// Scenery::DrawSetLists: r3 the manager, r4 the pass.
extern "C" REX_FUNC(sub_820F5178) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F5178)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  DrawSetLists(g, manager, At<game::SceneryDrawPass>(ctx.r4.u32));
}

// Scenery::RecordHeadlightProjectors: r4 the pass.
extern "C" REX_FUNC(sub_820F3398) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F3398)
  RecordHeadlightProjectors(g, At<game::SceneryDrawPass>(ctx.r4.u32));
}

// Scenery::Update: r3 the manager.
extern "C" REX_FUNC(sub_820F4428) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F4428)
  Update(g, At<SceneryManager>(ctx.r3.u32));
}

// Scenery::CollectAreas and CollectModels: r3 the manager, r4 the view (its
// player), r5 the viewport.
extern "C" REX_FUNC(sub_820F2F68) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2F68)
  const uint32_t view = ctx.r4.u32;
  CollectAreas(g, At<SceneryManager>(ctx.r3.u32), view, At<game::Viewport>(ctx.r5.u32));
}

extern "C" REX_FUNC(sub_820F31A0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F31A0)
  const uint32_t view = ctx.r4.u32;
  CollectModels(g, At<SceneryManager>(ctx.r3.u32), view, At<game::Viewport>(ctx.r5.u32));
}

// Scenery::LimitLargeModels: r3 the manager.
extern "C" REX_FUNC(sub_820F2A68) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2A68)
  LimitLargeModels(At<SceneryManager>(ctx.r3.u32));
}

// SceneryList::ReleaseUnused.
extern "C" REX_FUNC(sub_820F1650) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1650)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  ListReleaseUnused(g, manager, At<SceneryList>(ctx.r4.u32));
}

// --- The scenery manager ---

// SceneryConfig::ModelAsset: r3 the configuration, r4 the area, r5 the
// model: the model's asset, or -1.
extern "C" REX_FUNC(sub_820F13C0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F13C0)
  const int32_t asset =
      ModelAsset(At<SceneryConfiguration>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
  ctx.r3.u64 = asset < 0 ? ~uint64_t(0) : uint64_t(uint32_t(asset));
}

// Scenery::PoolSizes: r3 the area counts, r4 the model counts, r5 the five
// sizes, r6 the configuration.
extern "C" REX_FUNC(sub_820F1EC0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1EC0)
  PoolSizes(g, At<u32_be>(ctx.r3.u32), At<u32_be>(ctx.r4.u32), At<u32_be>(ctx.r5.u32),
            At<SceneryConfiguration>(ctx.r6.u32));
}

// Scenery::AllocPhysical: r3 bytes; returns the block.
extern "C" REX_FUNC(sub_820F1320) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1320)
  ctx.r3.u64 = AllocPhysical(g, ctx.r3.u32);
}

// Scenery::FreePhysical: r3 the block.
extern "C" REX_FUNC(sub_820F1378) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1378)
  FreePhysical(g, ctx.r3.u32);
}

// Scenery::ClearGlobal: [0x826B51F4] cleared.
extern "C" REX_FUNC(sub_820F1310) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1310)
  *At<u32_be>(kSceneryGlobal) = 0;
}

// Scenery::Init: r3 the manager.
extern "C" REX_FUNC(sub_820F11F0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F11F0)
  InitManager(At<SceneryManager>(ctx.r3.u32));
}

// Scenery::Create. Its callers read the manager from [0x826B51FC].
extern "C" REX_FUNC(sub_820F2E58) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2E58)
  CreateManager(g);
}

// Scenery::Setup: r3 the manager, r4 the configuration; returns the
// visible areas' block.
extern "C" REX_FUNC(sub_820F2EF0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2EF0)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  const uint32_t configuration = ctx.r4.u32;
  ctx.r3.u64 = Setup(g, manager, configuration);
}

// Scenery::SetupPools: r3 the manager, r4 the configuration.
extern "C" REX_FUNC(sub_820F25B0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F25B0)
  SceneryManager* manager = At<SceneryManager>(ctx.r3.u32);
  SetupPools(g, manager, ctx.r4.u32);
}

// Scenery::StartLoad: r3 the manager.
extern "C" REX_FUNC(sub_820F1C90) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1C90)
  StartLoad(g, At<SceneryManager>(ctx.r3.u32));
}

// Scenery::IsLoaded: r3 the manager; 1 once the course is in.
extern "C" REX_FUNC(sub_820F1DF8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1DF8)
  ctx.r3.u64 = SceneryIsLoaded(g, At<SceneryManager>(ctx.r3.u32)) ? 1 : 0;
}

// Scenery::Unload and UnloadPools: r3 the manager.
extern "C" REX_FUNC(sub_820F2A20) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2A20)
  Unload(g, At<SceneryManager>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820F1BB0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1BB0)
  UnloadPools(g, At<SceneryManager>(ctx.r3.u32));
}

// Scenery::Destroy.
extern "C" REX_FUNC(sub_820F29E0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F29E0)
  Destroy(g);
}

// --- The course set: r3 the set ---

// ModelList::Init: r3 the list, returned.
extern "C" REX_FUNC(sub_820F18B0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F18B0)
  InitModelList(At<ModelList>(ctx.r3.u32));
}

// CourseSet::InitParts: r3 the parts, returned.
extern "C" REX_FUNC(sub_820F20C0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F20C0)
  const uint32_t parts = ctx.r3.u32;
  InitParts(g, At<CourseParts>(parts));
  ctx.r3.u64 = parts;
}

// CourseSet::CourseSet: r4 its own size, r5 its data's, r6 its index;
// returns it.
extern "C" REX_FUNC(sub_820F2308) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2308)
  const uint32_t set = ctx.r3.u32;
  ConstructCourseSet(g, At<CourseSet>(set), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  ctx.r3.u64 = set;
}

// CourseSet::Load (its class's Load).
extern "C" REX_FUNC(sub_820F23B0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F23B0)
  LoadCourseSet(g, At<CourseSet>(ctx.r3.u32));
}

// CourseSet::OnLoaded: the archive's callback, r6 the set.
extern "C" REX_FUNC(sub_820F2148) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2148)
  CourseSetLoaded(g, At<CourseSet>(ctx.r6.u32));
}

// CourseSet::Parse.
extern "C" REX_FUNC(sub_820F1900) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1900)
  ParseCourseSet(g, At<CourseSet>(ctx.r3.u32));
}

// CourseSet::Reset (its class's Reset): returns the set.
extern "C" REX_FUNC(sub_820F0FC8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F0FC8)
  const uint32_t set = ctx.r3.u32;
  ResetCourseSet(g, At<CourseSet>(set));
  ctx.r3.u64 = set;
}

// --- The areas' lights and models ---

// AreaLights::Load (its class's Load): r3 the lights.
extern "C" REX_FUNC(sub_820F2188) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2188)
  LoadAreaLights(g, At<AreaLights>(ctx.r3.u32));
}

// AreaLights::OnLoaded: the archive's callback, r6 the lights.
extern "C" REX_FUNC(sub_820F1AC0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1AC0)
  AreaLights* lights = At<AreaLights>(ctx.r6.u32);
  ParseAreaLights(lights);
  MarkLoaded(&lights->object);
}

// AreaLights::Parse: r3 the lights.
extern "C" REX_FUNC(sub_820F1050) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1050)
  ParseAreaLights(At<AreaLights>(ctx.r3.u32));
}

// AreaModel::AreaModel: r4 its own size, r5 its data's, r6 its index;
// returns it.
extern "C" REX_FUNC(sub_820F1AF8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1AF8)
  const uint32_t area = ctx.r3.u32;
  ConstructAreaModel(g, At<AreaModel>(area), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  ctx.r3.u64 = area;
}

// AreaModel::Load (its class's Load): r3 the area model.
extern "C" REX_FUNC(sub_820F2250) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F2250)
  LoadAreaModel(g, At<AreaModel>(ctx.r3.u32));
}

// AreaModel::OnLoaded: the archive's callback, r6 the area model.
extern "C" REX_FUNC(sub_820F1B78) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1B78)
  AreaModel* area = At<AreaModel>(ctx.r6.u32);
  ParseAreaModel(g, area);
  MarkLoaded(&area->object);
}

// AreaModel::Parse: r3 the area model.
extern "C" REX_FUNC(sub_820F10E8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F10E8)
  ParseAreaModel(g, At<AreaModel>(ctx.r3.u32));
}

// AreaModel::Reset (its class's Reset): returns the area model.
extern "C" REX_FUNC(sub_820F1188) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1188)
  const uint32_t area = ctx.r3.u32;
  ResetAreaModel(g, At<AreaModel>(area));
  ctx.r3.u64 = area;
}

// A scenery model's destructor (its vtable's slot 0): r3 the model, r4 the
// flags (bit 0: delete it); returns it.
extern "C" REX_FUNC(sub_820F1098) {
  SBX_COURSE_SCENERY_FALLBACK(sub_820F1098)
  const uint32_t model = ctx.r3.u32, flags = ctx.r4.u32;
  game::ModelDestruct(g, At<Model>(model));
  if ((flags & 1) != 0) {
    g.Call(sub_8258C868, model);  // operator delete
  }
  ctx.r3.u64 = model;
}

// --- Texture animations ---

// Animation::Advance.
extern "C" REX_FUNC(sub_82145350) {
  SBX_COURSE_SCENERY_FALLBACK(sub_82145350)
  AdvanceAnimation(At<Animation>(ctx.r3.u32));
}

// AnimationSet::Advance: each animation one step on.
extern "C" REX_FUNC(sub_821453B0) {
  SBX_COURSE_SCENERY_FALLBACK(sub_821453B0)
  AdvanceAnimations(At<AnimationSet>(ctx.r3.u32));
}

// AnimationSet::FindByTexture: r4 the texture; the animation's index, or -1.
extern "C" REX_FUNC(sub_821452C8) {
  SBX_COURSE_SCENERY_FALLBACK(sub_821452C8)
  ctx.r3.s64 = FindAnimationByTexture(At<AnimationSet>(ctx.r3.u32), ctx.r4.u32);
}

// Scenery::Draw (sub_820F5F48): r3 the manager. Called from the frame
// state diff's wrapper (debug/frame_state_diff.cpp) in place of the
// recompiled function.
void hooks::NativeCourseScenery_Draw(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_820F5F48(ctx, base);
    return;
  }
  Guest g(ctx, base);
  DrawScenery(g, At<SceneryManager>(ctx.r3.u32));
}

// TextureAnimations::Advance: r3 the course set; its animation set
// advanced (AnimationSet::Advance, sub_821453B0) when it has one. Called
// from subtick's override (subtick/per_frame_logic.cpp), which skips it on
// render-only passes.
void hooks::NativeCourseScenery_AdvanceTextureAnimations(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_820F1038(ctx, base);
    return;
  }
  Guest g(ctx, base);
  AnimationSet* animations = At<CourseSet>(ctx.r3.u32)->parts.animations;
  if (animations != nullptr) {
    AdvanceAnimations(animations);
  }
}
