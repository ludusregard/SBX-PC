// The course scenery: its manager, the pools its objects come from, the
// lists of areas and models it streams, the course set and the texture
// animations (`docs/game_data.md`, "Courses").

#pragma once

#include "game/fixed_list.h"
#include "game/guest_types.h"
#include "game/model.h"

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u16_be;
using guest::u32_be;

struct DataFile;  // game/data_file.h

// A pool object's data: where it starts, its size, and how much is used
// (PoolObject::AllocData moves `used`).
struct PoolData {
  u32_be start;
  u32_be size;
  u32_be used;
};
GUEST_SIZE(PoolData, 12);

// An object in one of the scenery's pools. Each pool keeps its objects on
// two lists, free and used, linked through `previous` and `next`.
struct PoolObject {
  enum Flags : uint32_t {
    kInUse = 0x1,       // Set by ObjectPools::Acquire.
    kLoading = 0x2,     // Cleared by MarkLoaded.
    kDataLoaded = 0x8,  // Set by MarkLoaded.
    kLoaded = 0x9,      // Both of 0x1 and 0x8.
    kVisible = 0x10,    // Seen by a camera this update.
    kExpiring = 0x20,   // Not seen; released when `countdown` runs out.
    kLoadFlags = 0x3C,  // Cleared by ClearLoadFlags.
  };

  u32_be vtable;  // Slot 0: Key; slot 1: Load; slot 2: Reset.
  PoolData data;
  union {
    u32_be pool;
    u32_be index;  // Its index in its pool.
  };
  union {
    uint8_t unknown_20[4];
    ptr<PoolObject> previous;
  };
  union {
    u32_be data_end;  // Where the constructor leaves it: the next object.
    ptr<PoolObject> next;
  };
  u32_be unknown_28;
  u32_be flags;
  union {
    i32_be key;        // -1 for none.
    i32_be countdown;  // Updates left before an expiring object is released.
  };
};
GUEST_SIZE(PoolObject, 40);
GUEST_OFFSET(PoolObject, flags, 32);
GUEST_OFFSET(PoolObject, next, 24);

// Pools 0 and 1's objects: a pool object and one word.
struct SceneryPoolObject {
  PoolObject object;
  uint8_t unknown_40[16];
  u32_be unknown_56;
};
GUEST_OFFSET(SceneryPoolObject, unknown_56, 56);

struct AreaLightsParts {
  uint8_t unknown_0[16];
  XPR xpr;  // The area lights' +56.
};
GUEST_SIZE(AreaLightsParts, 40);

// The two pool objects whose keys are their own words.
struct AreaLights {
  PoolObject object;
  union {
    uint8_t unknown_40[40];
    AreaLightsParts parts;
  };
  u32_be light_set;  // The light file's entry 0 (a DataFile of light groups).
  u32_be key;        // The area (high halfword) and 0, as its list's key.
};
GUEST_OFFSET(AreaLights, light_set, 80);
GUEST_OFFSET(AreaLights, key, 84);
GUEST_SIZE(AreaLights, 88);

// A scenery model (448 bytes): a model with its draw state after it. The
// course set's lists' models and the area models' (vtable 0x8200673C: slot
// 0 the deleting destructor, slot 1 an empty hook of
// SceneryModel::RecordShape's).
struct SceneryModel {
  static constexpr uint32_t kVtable = 0x8200673C;

  Model model;
  uint8_t unknown_424[8];
  u32_be sampler_24_value;  // Its textures' sampler state 24 (ResetDrawState: 1).
  u32_be animations;        // The animation set its textures follow (0 for none).
  u32_be unknown_440;
  uint8_t unknown_444[4];
};
GUEST_SIZE(SceneryModel, 448);
GUEST_OFFSET(SceneryModel, sampler_24_value, 432);
GUEST_OFFSET(SceneryModel, animations, 436);

struct AreaModelLinks {
  uint8_t unknown_0[16];
  ptr<SceneryModel> model;  // The area model's +56: `model` once loaded, else 0.
  ptr<XPR> xpr;             // +60: `xpr` once loaded, else 0.
};
GUEST_SIZE(AreaModelLinks, 24);

struct AreaModel {
  PoolObject object;
  union {
    uint8_t unknown_40[24];
    AreaModelLinks links;
  };
  u32_be key;  // The area (high halfword) and the model, as its list's key.
  uint8_t unknown_68[12];
  SceneryModel model;
  XPR xpr;
  uint8_t unknown_552[8];
};
GUEST_OFFSET(AreaModel, key, 64);
GUEST_OFFSET(AreaModel, model, 80);
GUEST_OFFSET(AreaModel, xpr, 528);
GUEST_SIZE(AreaModel, 560);

// A pool's free and used lists' first objects.
struct PoolHeads {
  ptr<PoolObject> free_list;
  ptr<PoolObject> used_list;
};
GUEST_SIZE(PoolHeads, 8);

// One pool: its objects are `unknown_4 + unknown_8` bytes each from
// `objects`.
struct PoolEntry {
  union {
    uint8_t unknown_0[4];
    i32_be object_count;
  };
  union {
    u32_be unknown_4;
    u32_be object_size;  // Rounded up to 64.
  };
  union {
    u32_be unknown_8;
    u32_be data_size;  // Rounded up to 64.
  };
  u32_be objects;
  union {
    uint8_t unknown_16[8];
    PoolHeads heads;
  };
};
GUEST_SIZE(PoolEntry, 24);

struct ObjectPools {
  union {
    uint8_t unknown_0[4];
    i32_be count;
  };
  u32_be pools;  // The address of its PoolEntry array, in one block with the objects.

  PoolEntry* Pool(uint32_t index) { return guest::Element<PoolEntry>(pools, index); }
};
GUEST_SIZE(ObjectPools, 8);

// How ObjectPools::Init makes one pool.
struct PoolDescriptor {
  u32_be index;
  u32_be object_size;
  u32_be data_size;
  u32_be object_count;
  u32_be data_count;  // How many data blocks the pool's memory has room for.
  u32_be factory;     // Constructs each object (Scenery::MakePoolObject), or 0.
};
GUEST_SIZE(PoolDescriptor, 24);

// One area of a course: its lights' asset (-1 ends the areas) and its
// models' first and last assets.
struct CourseArea {
  i32_be lights;
  i32_be first_model;
  i32_be last_model;
};
GUEST_SIZE(CourseArea, 12);

// A course's record in the course table: its two set files and its areas.
struct CourseSceneryRecord {
  uint8_t unknown_0[8];
  i32_be set_file_2;  // The areas, light groups and animations.
  i32_be set_file_1;  // The XPR, the model and the model lists.
  u32_be area_count;
  CourseArea areas[1];  // The first of `area_count`.

  CourseArea* Area(uint32_t index) {
    return guest::Element<CourseArea>(guest::AddressOf(areas), index);
  }
};
GUEST_OFFSET(CourseSceneryRecord, area_count, 16);
GUEST_OFFSET(CourseSceneryRecord, areas, 20);

// What Scenery::Setup is given (44 bytes; the manager keeps a copy).
struct SceneryConfiguration {
  i32_be mode;       // 0 loads everything at once; else streams by the cameras.
  i32_be last_view;  // The view count - 1.
  uint8_t unknown_8[16];
  ptr<i32_be> pool0_asset;  // Pool 0's data is this asset's size.
  ptr<CourseSceneryRecord> course;
  ptr<i32_be> mode_2_with_one_view;  // The mode used in place of 2 with one view.
  u32_be has_pool0;
  u32_be has_pool1;
};
GUEST_SIZE(SceneryConfiguration, 44);
GUEST_OFFSET(SceneryConfiguration, course, 28);

// An area or model a list holds: how near a camera has it, the cameras'
// pass bits, which one, and its pool object once there.
struct SceneryItem {
  f32_be distance;
  u32_be mask;
  u16_be area;
  u16_be model;  // 0 for an area.
  ptr<PoolObject> object;
};
GUEST_SIZE(SceneryItem, 16);
GUEST_OFFSET(SceneryItem, area, 8);

// An entry of a list's key table: an area or model, its asset, and its
// object while it's loaded (SceneryList::LoadAll).
struct SceneryKey {
  u16_be area;
  u16_be model;
  i32_be asset;
  ptr<PoolObject> object;
};
GUEST_SIZE(SceneryKey, 12);

// The scenery's lists: the areas (pool 3) and the models (pool 4) to have
// loaded, `count` items at `items`, ordered by distance through
// `by_distance` (pointers to the items); `done` counts the ones in.
struct SceneryList {
  union {
    uint8_t unknown_0[4];
    u32_be pool;
  };
  u32_be unknown_4[2];  // done(), count().
  u32_be key_count;
  u32_be by_distance;  // The address of `key_count` item pointers.
  u32_be items;        // The address of `key_count` SceneryItems.
  u32_be keys;         // The address of `key_count` SceneryKeys.

  u32_be& done() { return unknown_4[0]; }
  u32_be& count() { return unknown_4[1]; }
  SceneryItem* ItemAt(uint32_t position) {
    return guest::At<SceneryItem>(*guest::Element<u32_be>(by_distance, position));
  }
  u32_be* ItemSlot(uint32_t position) { return guest::Element<u32_be>(by_distance, position); }
  SceneryItem* Item(uint32_t index) { return guest::Element<SceneryItem>(items, index); }
  SceneryKey* Key(uint32_t index) { return guest::Element<SceneryKey>(keys, index); }
};
GUEST_SIZE(SceneryList, 28);

// An entry of the course set's area file: the area's models, its
// rectangle (corners 0 and 2 are its least and greatest x and z) and each
// model's corners (96-byte records).
struct SceneryModelBounds {
  u32_be model;
  uint8_t unknown_4[28];
  Vector corners[4];
};
GUEST_SIZE(SceneryModelBounds, 96);
GUEST_OFFSET(SceneryModelBounds, corners, 32);

struct SceneryAreaEntry {
  u32_be model_count;
  uint8_t unknown_4[28];
  Vector corners[4];
  SceneryModelBounds models[1];  // The first of `model_count`.

  SceneryModelBounds* Model(uint32_t index) {
    return guest::Element<SceneryModelBounds>(guest::AddressOf(models), index);
  }
};
GUEST_OFFSET(SceneryAreaEntry, corners, 32);
GUEST_OFFSET(SceneryAreaEntry, models, 96);

// One of the 14 records (at 0x825F2F08) that tie a player's viewport view
// to a camera pass, with the pass's bits for the scenery's masks.
struct ScenePassRecord {
  i32_be player;
  i32_be view;
  i32_be pass;
  u32_be mask;
};
GUEST_SIZE(ScenePassRecord, 16);

// What the scenery's draws are given for one pass (the race's draw step
// fills it): the billboard words, the camera pass, the render queue's two
// keys and the pass's bits.
struct SceneryDrawPass {
  u32_be billboard[2];  // The camera pass's address and key_2 (the billboard set-up's).
  u32_be camera;        // The camera pass's address (RecordHeadlightProjectors sets it).
  i32_be player;
  i32_be viewport_view;
  i32_be camera_pass;
  u32_be key;
  u32_be key_2;
  u32_be mask;
};
GUEST_SIZE(SceneryDrawPass, 36);
GUEST_OFFSET(SceneryDrawPass, camera, 8);
GUEST_OFFSET(SceneryDrawPass, camera_pass, 20);
GUEST_OFFSET(SceneryDrawPass, mask, 32);

// A camera pass (game::CameraPass, race_camera.h) as far as its kind goes:
// 0 to 2 the main passes, 10, 13 and 16 the shadow passes. (CameraPass
// keeps +584 unnamed; the kind belongs there.)
struct CameraPassKind {
  uint8_t unknown_0[584];
  i32_be kind;
  u32_be enabled;
};
GUEST_SIZE(CameraPassKind, 592);

// What a camera sees of one area (272 bytes; Scenery::Update fills them).
struct SceneryArea {
  u32_be mask;
  u32_be model_count;
  u32_be model_capacity;
  ptr<AreaLights> lights;
  u32_be models[64];
};
GUEST_SIZE(SceneryArea, 272);

// A list of scenery models (a course set's lists: CourseSet::Parse loads
// them from its set file's entries 2 to 7).
struct ModelList {
  i32_be count;
  FixedList<64, u32_be> models;  // SceneryModel addresses.
};
GUEST_SIZE(ModelList, 528);

// The course set's point list: its two models drawn at the points of its
// buffers (PointList::Draw): model 0 at the groups from `first_group` to 2,
// model 1 at group 3's. Its 20 point buffers are 256 bytes each (four
// groups of four 16-byte points) in one static block.
struct PointList {
  static constexpr int kPointBufferCount = 20;

  ModelList list;
  u32_be point_buffers[kPointBufferCount];
  u32_be animations;        // The models' animation set (set before a draw).
  u32_be texture_provider;  // The models' texture provider (set before a draw).
  u32_be first_group;       // Set from the race's current light setting before a draw.
  i32_be point_buffer_count;
};
GUEST_SIZE(PointList, 624);
GUEST_OFFSET(PointList, point_buffers, 528);
GUEST_OFFSET(PointList, animations, 608);
GUEST_OFFSET(PointList, first_group, 616);
GUEST_OFFSET(PointList, point_buffer_count, 620);

// A texture animation: the key it's on and the frame, with each key's
// first frame.
struct Animation {
  i32_be key_count;
  i32_be key;
  i32_be frame;
  i32_be last_frame;
  i32_be key_frames;  // The first of `key_count`.

  i32_be* KeyFrame(int32_t key) {
    return guest::Element<i32_be>(guest::AddressOf(&key_frames), uint32_t(key));
  }
  // The texture it animates: the word after its key frames and one more.
  u32_be* Texture() {
    return guest::Element<u32_be>(guest::AddressOf(&key_frames), uint32_t(key_count) + 1);
  }
};
GUEST_OFFSET(Animation, key_frames, 16);

// The animations: a count, then each one's offset from the end of the
// offsets.
struct AnimationSet {
  u32_be count;
  u32_be offsets;  // The first of `count`.

  Animation* Entry(uint32_t index) {
    const uint32_t table_end = guest::AddressOf(this) + ((uint32_t(count) + 1) << 2);
    return guest::At<Animation>(table_end + *guest::Element<u32_be>(guest::AddressOf(&offsets),
                                                                    index));
  }
};

// A draw's distance range for vertex c86 (the shape recorders read its
// first two floats).
struct DrawRange {
  f32_be near;
  f32_be far;
  f32_be unknown_8;
  f32_be unknown_12;
};
GUEST_SIZE(DrawRange, 16);

// The race progress's entry (game::RaceEntry at the progress's +2184) as
// the scenery's point lists read it: nine 76-byte light settings and which
// one is current.
struct RaceLightSetting {
  u32_be value;
  uint8_t unknown_4[72];
};
GUEST_SIZE(RaceLightSetting, 76);

struct RaceLightSettings {
  uint8_t unknown_0[84];
  RaceLightSetting settings[9];
  uint8_t unknown_768[68];
  i32_be current;
};
GUEST_OFFSET(RaceLightSettings, settings, 84);
GUEST_OFFSET(RaceLightSettings, current, 836);

// The car scene ([0x826B2B24], scene.h's CarScene) as the headlights read
// it: its scene models (a node list), and the lights' settings.
struct CarSceneHeadlights {
  uint8_t unknown_0[4980];
  u32_be models;  // The list's first node (List::At walks it).
  u32_be models_last;
  i32_be model_count;
  uint8_t unknown_4992[256];
  Vector color_scale;    // The headlights' first color, times each car's.
  Vector color_scale_2;  // The second.
  Vector view_center;    // Copied to 0x8264EA60 after the cars.
  uint8_t unknown_5296[32];
  Vector lamp_offset;  // Where the lamps sit in a car's space.
  uint8_t unknown_5344[32];
  Vector extra;  // Copied to each slot's sixth array; w the projectors' field of view.
};
GUEST_OFFSET(CarSceneHeadlights, models, 4980);
GUEST_OFFSET(CarSceneHeadlights, model_count, 4988);
GUEST_OFFSET(CarSceneHeadlights, color_scale, 5248);
GUEST_OFFSET(CarSceneHeadlights, view_center, 5280);
GUEST_OFFSET(CarSceneHeadlights, lamp_offset, 5328);
GUEST_OFFSET(CarSceneHeadlights, extra, 5376);

// The course set's parts (CourseSet::InitParts makes them).
struct CourseParts {
  XPR xpr;
  uint8_t unknown_24[8];
  Model model;  // The sky.
  uint8_t unknown_456[8];
  ModelList list_464;  // The set's own models (each pass within the far distance).
  u32_be area_file;    // Set file 2's entry 0: the areas.
  ptr<AnimationSet> animations;
  ModelList list_1000;
  u32_be light_groups;  // Set file 2's entry 2.
  ModelList lists_1532[3];  // The set lists: all, the B entries and the C entries.
  PointList point_list;
};
GUEST_OFFSET(CourseParts, model, 32);
GUEST_OFFSET(CourseParts, list_464, 464);
GUEST_OFFSET(CourseParts, area_file, 992);
GUEST_OFFSET(CourseParts, animations, 996);
GUEST_OFFSET(CourseParts, light_groups, 1528);
GUEST_OFFSET(CourseParts, lists_1532, 1532);
GUEST_OFFSET(CourseParts, point_list, 3116);
GUEST_SIZE(CourseParts, 3740);

// The course set: a pool object, then its parts and its two set files.
struct CourseSet {
  PoolObject object;
  uint8_t unknown_40[24];
  CourseParts parts;
  uint8_t unknown_3804[4];
  u32_be set_files[2];  // Set file 1's block of its own; set file 2 in the object's data.
  uint8_t unknown_3816[8];
};
GUEST_OFFSET(CourseSet, parts, 64);
GUEST_OFFSET(CourseSet, set_files, 3808);
GUEST_SIZE(CourseSet, 3824);

// The course scenery manager (248 bytes, at [0x826B51F4] and
// [0x826B51FC]). It is its own pool set.
struct SceneryManager {
  ObjectPools pools;
  SceneryConfiguration config;
  u32_be unknown_52;
  u32_be unknown_56;
  uint8_t unknown_60[32];
  u32_be area_counts[2];   // The areas, and how many are kept at once.
  u32_be model_counts[2];  // The models, and how many are kept at once (at most 16).
  ptr<PoolObject> pool0_object;
  ptr<CourseSet> course_set;
  SceneryList areas;
  SceneryList models;
  i32_be last_pool;  // The object last asked to load, -1 for none.
  i32_be last_index;
  u32_be pool0_block_size;
  u32_be pool0_block;
  u32_be model_buffer;  // The area models' load buffer (the archive's shared one).
  u32_be loading;       // Set by StartLoad until IsLoaded answers 1.
  u32_be pass_mask;     // The camera passes the areas draw in.
  f32_be unknown_200[4];
  u32_be visible_area_count;
  u32_be visible_area_capacity;
  u32_be visible_areas;  // The address of its SceneryArea array.
  XPRList textures;      // The texture provider the scenery draws with.

  SceneryArea* VisibleArea(uint32_t index) {
    return guest::Element<SceneryArea>(visible_areas, index);
  }
};
GUEST_OFFSET(SceneryManager, config, 8);
GUEST_OFFSET(SceneryManager, area_counts, 92);
GUEST_OFFSET(SceneryManager, pool0_object, 108);
GUEST_OFFSET(SceneryManager, areas, 116);
GUEST_OFFSET(SceneryManager, models, 144);
GUEST_OFFSET(SceneryManager, last_pool, 172);
GUEST_OFFSET(SceneryManager, model_buffer, 188);
GUEST_OFFSET(SceneryManager, pass_mask, 196);
GUEST_OFFSET(SceneryManager, visible_areas, 224);
GUEST_SIZE(SceneryManager, 248);

// --- The functions other files of the group call (scenery_lists.cpp) ---

// The model lists: a container's models loaded (a point list's with its
// point buffers set) and released; which set list entries are drawn.
void ModelListLoad(guest::Guest& g, ModelList* list, DataFile* container);  // sub_82146CD0
void PointListLoad(guest::Guest& g, PointList* list, DataFile* container);  // sub_82146DD0
void ModelListRelease(guest::Guest& g, ModelList* list);                    // sub_82146848
bool ModelListIsBEntryOn(uint32_t entry);                                   // sub_821460E0
bool ModelListIsCEntryOn(guest::Guest& g, uint32_t entry);                  // sub_821461E8
bool PointListIsEntryOn(int32_t entry);                                     // sub_821463A8

// A scenery model's draw state reset (its animations off, sampler state
// 24's value 1).
void SceneryModelResetDrawState(SceneryModel* model);  // sub_82145338

// A pass's billboard words set (the light groups' billboard set-up).
void SetBillboardPass(SceneryDrawPass* pass, uint32_t word_0, uint32_t word_4);  // sub_82146EF8

// The shape draws: a scenery model's shape (SceneryModel::DrawShape, `range`
// the optional vertex c86 range, `pixel_constant` the optional c76,
// `pair_offset` added to the shared shader pairs) and a plain model's
// (Model::DrawShape), each with the depth pass too when asked; a shape's
// depth draw; the point list's draw for a camera pass.
void SceneryModelDrawShape(guest::Guest& g, SceneryModel* model, Matrix* view_projection,
                           Matrix* world, uint32_t shape, uint32_t depth_pass,
                           const Vector* pixel_constant, const DrawRange* range,
                           uint32_t pair_offset);  // sub_82146618
void ModelDrawShape(guest::Guest& g, Model* model, Matrix* view_projection, Matrix* world,
                    uint32_t shape, uint32_t depth_pass, const Vector* pixel_constant,
                    const DrawRange* range);  // sub_8257C430
void ModelDrawShapeDepth(guest::Guest& g, Model* model, Matrix* view_projection, Matrix* world,
                         uint32_t shape, uint8_t group, uint32_t mask,
                         uint32_t with_alpha);  // sub_8257C660
void PointListDraw(guest::Guest& g, PointList* list, uint32_t entry,
                   uint32_t camera_pass);  // sub_821468E8

}  // namespace game
