// The scene's loads: the slots a file is loaded into, the first pool set
// ([0x826B2B20]) and its objects (the car model, car parts, cockpit,
// interior, pool 4 object, wheels and traffic car) with their loads, and
// the records the scene models are built from.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/model.h"

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// A file being loaded: its asset id, the archive's request, its size, its
// block (freed by LoadSlot::ReleaseBuffer unless the slot only borrows the
// shared buffer) and its state.
struct LoadSlot {
  static constexpr uint32_t kEmpty = 1;    // Nothing to load (size 0, or too big).
  static constexpr uint32_t kLoading = 2;
  static constexpr uint32_t kArrived = 3;  // Its file is in (or used).

  u32_be unknown_0;  // The asset id.
  i32_be unknown_4;  // The archive's request, -1 for none.
  u32_be unknown_8;  // The asset's size.
  u32_be keeps_block;  // Set: the block is the shared load buffer (only let go).
  u32_be block;
  u32_be state;
};
GUEST_SIZE(LoadSlot, 24);
GUEST_OFFSET(LoadSlot, state, 20);

struct PoolNode;

// A list of pool nodes: the scene's pools keep their objects on them.
struct PoolNodeList {
  ptr<PoolNode> first;
  ptr<PoolNode> last;
  u32_be count;
};
GUEST_SIZE(PoolNodeList, 12);

// The base of every scene pool object (PoolNode::PoolNode): on its pool's
// list once made.
struct PoolNode {
  u32_be vtable;
  ptr<PoolNodeList> list;
  ptr<PoolNode> next;
  ptr<PoolNode> previous;
};
GUEST_SIZE(PoolNode, 16);

// What each pool object (of both pool sets) starts with: its node, its
// state (3 while it's in use; 0 once made, and after ScenePools::ReleaseA)
// and its index in its pool.
struct ScenePoolObjectHeader {
  static constexpr uint32_t kInUse = 3;

  PoolNode node;
  u32_be state;
  i32_be index;  // -1 until the pools give it one.
};
GUEST_SIZE(ScenePoolObjectHeader, 24);
GUEST_OFFSET(ScenePoolObjectHeader, index, 20);

struct CarModel;

// The header and the car model a first-set object belongs to.
struct ScenePoolObjectHead {
  ScenePoolObjectHeader header;
  ptr<CarModel> car_model;
};
GUEST_SIZE(ScenePoolObjectHead, 28);

// The car scene's set-up record (48 bytes; CarScene::SetUp's, then
// ScenePools::CreateA's): the model set, how many of each object to make
// (the interiors, wheels and scene models are sums of these) and the load
// buffer to use (0 for the archive's).
struct CarSceneRecord {
  u32_be model_set;
  i32_be car_models;
  i32_be car_parts;
  i32_be interiors;  // Besides the cockpits' and traffic cars'.
  i32_be cockpits;
  i32_be pool4_objects;
  i32_be wheels;  // Besides the traffic cars' and the other models'.
  i32_be unknown_28;  // Scene models that aren't traffic cars, probably.
  i32_be traffic_cars;
  u32_be load_buffer;
  u32_be load_buffer_size;
  u32_be unknown_44;
};
GUEST_SIZE(CarSceneRecord, 48);
GUEST_OFFSET(CarSceneRecord, traffic_cars, 32);
GUEST_OFFSET(CarSceneRecord, load_buffer, 36);

// A texture put off for release (DeferRelease): its four words (the
// texture's header and data, twice) and its state (2 once queued, 0
// free).
struct DeferredRelease {
  u32_be words[4];
  u32_be state;
};
GUEST_SIZE(DeferredRelease, 20);

// The scene's first pool set (140 bytes at [0x826B2B20]): which model set
// it's for, its pools' object counts, the deferred releases' records (four
// of 20 bytes for each car model and traffic car) and the seven pools'
// lists.
struct ScenePoolSet {
  static constexpr uint32_t kSetA = 0;  // The front end's models.
  static constexpr uint32_t kSetB = 1;  // The race's.

  uint8_t unknown_0[4];
  u32_be unknown_4;  // 3 once created.
  u32_be model_set;
  i32_be car_models;     // Pool 0.
  i32_be car_parts;      // Pool 1.
  i32_be interiors;      // Pool 3.
  i32_be cockpits;       // Pool 2.
  i32_be pool4_objects;  // Pool 4.
  i32_be wheels;         // Pool 5.
  i32_be unknown_36;
  i32_be traffic_cars;  // Pool 6.
  uint8_t unknown_44[8];
  ptr<DeferredRelease> releases;
  PoolNodeList lists[7];
};
GUEST_OFFSET(ScenePoolSet, model_set, 8);
GUEST_OFFSET(ScenePoolSet, car_models, 12);
GUEST_OFFSET(ScenePoolSet, traffic_cars, 40);
GUEST_OFFSET(ScenePoolSet, releases, 52);
GUEST_OFFSET(ScenePoolSet, lists, 56);
GUEST_SIZE(ScenePoolSet, 140);

// Two textures' header and data addresses: the vinyl sheet (the drawn one,
// then a second the parts are also given), or the two plate textures.
struct TexturePair {
  u32_be header;
  u32_be data;
  u32_be header_b;
  u32_be data_b;
};
GUEST_SIZE(TexturePair, 16);

// A vinyl layer's or sticker's load: the TGA's slot, and a second slot
// whose state the decals' wait also checks.
struct DecalSlot {
  LoadSlot load;
  LoadSlot unknown_24;
};
GUEST_SIZE(DecalSlot, 48);

// A part model takes 464 bytes in the loads' arrays.
static_assert(sizeof(PartModel) == 464);

// The loads every car model's and cockpit's start with: the node file,
// the skeleton bound to it, the two body XPR2s and their texture
// providers.
struct CarModelLoads {
  LoadSlot node_slot;
  uint8_t unknown_24[8];
  uint8_t skeleton[192];  // A Skeleton.
  LoadSlot body_slots[2];
  XPR xprs[2];
  XPRList texture_providers[2];
};
GUEST_OFFSET(CarModelLoads, skeleton, 32);
GUEST_OFFSET(CarModelLoads, body_slots, 224);
GUEST_OFFSET(CarModelLoads, xprs, 272);
GUEST_OFFSET(CarModelLoads, texture_providers, 320);
GUEST_SIZE(CarModelLoads, 360);

// A car model's loads in full (CarModelLoads::Reset): the common part,
// then the decals' state and loads, the vinyl sheet and the plate.
struct CarBodyLoads {
  static constexpr int kLayers = 6;     // Vinyl layers: the record's layers 0-5.
  static constexpr int kStickers = 48;  // Eight groups of six.

  CarModelLoads model;
  u32_be decals_state;  // 2 loading, 3 drawn and compressed.
  DecalSlot layers[kLayers];
  DecalSlot stickers[kStickers];
  TexturePair sheet;  // The vinyl sheet.
  TexturePair plate;
};
GUEST_OFFSET(CarBodyLoads, layers, 364);
GUEST_OFFSET(CarBodyLoads, stickers, 652);
GUEST_OFFSET(CarBodyLoads, sheet, 2956);
GUEST_OFFSET(CarBodyLoads, plate, 2972);
GUEST_SIZE(CarBodyLoads, 2988);

// The cockpit's loads: set c's node file and body, and its six exterior
// parts.
struct CockpitLoads {
  CarModelLoads model;
  LoadSlot part_slots[6];
  uint8_t unknown_504[8];
  PartModel parts[6];
};
GUEST_OFFSET(CockpitLoads, part_slots, 360);
GUEST_OFFSET(CockpitLoads, parts, 512);

// The traffic car loads' first words: its vinyl sheet (a blank 16 x 16
// texture; TrafficCar::Release hands it back) and the file's slot.
struct TrafficLoadsHead {
  TexturePair sheet;
  LoadSlot node_slot;
  uint8_t unknown_40[8];
};
GUEST_SIZE(TrafficLoadsHead, 48);

struct TrafficLoads {
  TrafficLoadsHead head;
  uint8_t skeleton[192];  // A Skeleton.
  LoadSlot xpr_slots[2];
  XPR xprs[2];
  XPRList lists[2];
  LoadSlot part_slots[6];
  uint8_t unknown_520[8];
  PartModel parts[6];
};
GUEST_OFFSET(TrafficLoads, skeleton, 48);
GUEST_OFFSET(TrafficLoads, xprs, 288);
GUEST_OFFSET(TrafficLoads, lists, 336);
GUEST_OFFSET(TrafficLoads, part_slots, 376);
GUEST_OFFSET(TrafficLoads, parts, 528);
GUEST_SIZE(TrafficLoads, 3312);

struct WheelsLoads {
  LoadSlot slots_0[2];
  PartModel parts_48[2];
  LoadSlot slots_976[2];
  PartModel parts_1024[2];
  LoadSlot slots_1952[2];
  XPR xprs[2];
  XPRList lists[2];
  LoadSlot slots_2088[2];
  uint8_t unknown_2136[8];
  PartModel parts_2144[2];
};
GUEST_OFFSET(WheelsLoads, parts_48, 48);
GUEST_OFFSET(WheelsLoads, slots_976, 976);
GUEST_OFFSET(WheelsLoads, parts_1024, 1024);
GUEST_OFFSET(WheelsLoads, slots_1952, 1952);
GUEST_OFFSET(WheelsLoads, xprs, 2000);
GUEST_OFFSET(WheelsLoads, lists, 2048);
GUEST_OFFSET(WheelsLoads, slots_2088, 2088);
GUEST_OFFSET(WheelsLoads, parts_2144, 2144);
GUEST_SIZE(WheelsLoads, 3072);

// Three words a record keeps: an index (-1 for none) and two floats
// (Record12::Reset: both -1.0).
struct Record12 {
  i32_be unknown_0;
  f32_be unknown_4;
  f32_be unknown_8;
};
GUEST_SIZE(Record12, 12);

// The wheels' record (156 bytes; WheelsRecord::Reset).
struct WheelsRecord {
  u32_be unknown_0;
  u32_be unknown_4[3];
  Record12 unknown_16;  // -1, 20.0, 0.5 once reset.
  u32_be unknown_28;
  Record12 unknown_32;  // The same.
  Record12 unknown_44;
  Record12 unknown_56;
  u32_be unknown_68[5];
  u32_be caliper_texture;
  u32_be unknown_92[2];
  f32_be unknown_100[2];
  u32_be unknown_108[2];  // 11 once reset.
  u32_be unknown_116[2];
  i32_be unknown_124;
  f32_be unknown_128[7];
};
GUEST_SIZE(WheelsRecord, 156);
GUEST_OFFSET(WheelsRecord, unknown_44, 44);
GUEST_OFFSET(WheelsRecord, caliper_texture, 88);
GUEST_OFFSET(WheelsRecord, unknown_128, 128);

// The wheels (pool 5 of the first set; 3,280 bytes): their record and
// models, and the car model (or a traffic car's record) they're for.
struct Wheels {
  ScenePoolObjectHead head;
  WheelsRecord record;
  uint8_t unknown_184[8];
  WheelsLoads loads;
  u32_be traffic_record;  // A traffic car's record, in place of the car model.
  ptr<ScenePoolSet> first_set;  // [0x826B2B20] when reset.
  uint8_t unknown_3272[8];
};
GUEST_OFFSET(Wheels, record, 28);
GUEST_OFFSET(Wheels, loads, 192);
GUEST_OFFSET(Wheels, traffic_record, 3264);
GUEST_OFFSET(Wheels, first_set, 3268);
GUEST_SIZE(Wheels, 3280);

// The car parts' loads: a slot and a model for each of 216 parts.
struct CarPartsLoads {
  static constexpr int kParts = 216;

  LoadSlot slots[kParts];
  PartModel parts[kParts];
  uint8_t unknown_105408[16];
  Vector points[30];
};
GUEST_OFFSET(CarPartsLoads, parts, 5184);
GUEST_OFFSET(CarPartsLoads, points, 105424);

// A car's choice of parts (60 bytes; PartsRecord::Reset): a word for each
// kind of part.
struct PartsRecord {
  u32_be unknown_0[13];
  u32_be unknown_52;  // 3 once reset.
  u32_be unknown_56;
};
GUEST_SIZE(PartsRecord, 60);

// The car parts (pool 1 of the first set; 106,016 bytes): 216 slots and
// models (187 in set b).
struct CarParts {
  ScenePoolObjectHead head;
  PartsRecord record;
  uint8_t unknown_88[8];
  CarPartsLoads loads;
  ptr<ScenePoolSet> first_set;  // [0x826B2B20] when made.
  uint8_t unknown_106004[12];

  // Set a has all 216 parts; the others the first 187.
  int PartCount() const { return first_set->model_set == 0 ? CarPartsLoads::kParts : 187; }
};
GUEST_OFFSET(CarParts, record, 28);
GUEST_OFFSET(CarParts, loads, 96);
GUEST_OFFSET(CarParts, first_set, 106000);
GUEST_SIZE(CarParts, 106016);

// The record a pool 4 object is loaded from (52 bytes; Pool4Record::Reset):
// a texture and three colors (the light kit's).
struct Pool4Record {
  struct Color {
    u32_be unknown_0;
    f32_be rgb[3];  // 1.0 once reset.
  };

  u32_be texture;
  Color colors[3];
};
GUEST_SIZE(Pool4Record, 52);

// Pool 4 of the first set (592 bytes): the lights' model.
struct Pool4Object {
  ScenePoolObjectHead head;
  Pool4Record record;
  LoadSlot slot;
  uint8_t unknown_104[8];
  PartModel model;
  ptr<ScenePoolSet> first_set;  // [0x826B2B20] when reset.
  uint8_t unknown_580[12];
};
GUEST_OFFSET(Pool4Object, record, 28);
GUEST_OFFSET(Pool4Object, slot, 80);
GUEST_OFFSET(Pool4Object, model, 112);
GUEST_OFFSET(Pool4Object, first_set, 576);
GUEST_SIZE(Pool4Object, 592);

// A car record's vinyl layer or sticker (64 bytes; Layer::Reset).
struct RecordLayer {
  i32_be unknown_0;
  u32_be unknown_4;
  u32_be kind;  // 8 once reset.
  uint8_t unknown_12[4];
  Vector unknown_16;
  f32_be unknown_32;
  uint8_t unknown_36[12];
  Vector unknown_48;
};
GUEST_SIZE(RecordLayer, 64);
GUEST_OFFSET(RecordLayer, unknown_48, 48);

// A car's record (3,696 bytes; CarRecord::Reset): nine vectors, 54
// layers, the part order.
struct SceneCarRecord {
  static constexpr int kLayers = 54;

  i32_be unknown_0;
  uint8_t unknown_4[12];
  Vector unknown_16[9];
  u32_be unknown_160[2];
  uint8_t unknown_168[8];
  RecordLayer layers[kLayers];
  u32_be part_order[11];  // Copied with memcpy.
  u32_be unknown_3676[4];
  uint8_t unknown_3692[4];
};
GUEST_SIZE(SceneCarRecord, 3696);
GUEST_OFFSET(SceneCarRecord, layers, 176);
GUEST_OFFSET(SceneCarRecord, part_order, 3632);
GUEST_OFFSET(SceneCarRecord, unknown_3676, 3676);

// One of the interior record's gauge choices (seven, from its +8): on or
// off, which of the car scene's shared images the gauge shows, and its
// texture. (The garage stand's tuning screens set them: GarageStage::
// SetChoice and ClearChoice.)
struct GaugeChoice {
  u32_be enabled;
  u32_be image;
  i32_be texture;
};
GUEST_SIZE(GaugeChoice, 12);

struct InteriorRecord {
  static constexpr int kGauges = 7;

  u32_be unknown_0[25];  // See InteriorRecord::Reset for the defaults.
  i32_be unknown_100;    // SceneInterior::Apply's index.
  u32_be unknown_104[2];
  i32_be accessory;  // 3 for none.
  u32_be unknown_116;

  // Gauge choice `index` (its words +8 on). Not checked, as the game.
  GaugeChoice& Gauge(uint32_t index) {
    return *guest::Element<GaugeChoice>(guest::AddressOf(&unknown_0[2]), index);
  }
};
GUEST_OFFSET(InteriorRecord, accessory, 112);
GUEST_SIZE(InteriorRecord, 120);

struct CockpitRecord {
  u32_be unknown_0[5];  // Bonnet, door, mirrors, headlights, fenders.
  union {
    uint8_t unknown_20[4];
    i32_be interior_variant;  // 0 for the plain "_inte_on" texture, else its number.
  };
  InteriorRecord interior;
};
GUEST_SIZE(CockpitRecord, 144);
GUEST_OFFSET(CockpitRecord, interior, 24);

// A scene model's record (4,244 bytes): its kind, then each part's.
struct SceneModelRecord {
  u32_be kind;
  uint8_t unknown_4[12];
  SceneCarRecord car;
  PartsRecord parts;
  InteriorRecord interior;
  CockpitRecord cockpit;
  WheelsRecord wheels;
  Pool4Record pool4;
};
GUEST_OFFSET(SceneModelRecord, car, 16);
GUEST_OFFSET(SceneModelRecord, parts, 3712);
GUEST_OFFSET(SceneModelRecord, interior, 3772);
GUEST_OFFSET(SceneModelRecord, cockpit, 3892);
GUEST_OFFSET(SceneModelRecord, wheels, 4036);
GUEST_OFFSET(SceneModelRecord, pool4, 4192);
GUEST_SIZE(SceneModelRecord, 4244);

// The car model (pool 0 of the first set; 7,264 bytes): its record and
// loads (body, node file, decals, vinyl sheet, plate), the matrices and
// points of the nodes the car's parts are placed by, and the interior's
// textures.
struct CarModel {
  ScenePoolObjectHeader header;
  uint8_t unknown_24[8];
  SceneCarRecord record;  // Set by CarModel::SetRecord: +0 the car's block.
  CarBodyLoads loads;     // From its node file's slot.
  uint8_t unknown_6716[4];
  Matrix tire_matrices[4];   // The nodes "tire_fl", "tire_fr", "tire_rl", "tire_rr".
  Vector tire_positions[4];  // Their positions turned a quarter about y, scaled 0.1.
  Matrix seat_matrices[2];   // "seat_l", "seat_r".
  Vector seat_positions[2];
  u32_be accessory_matrix;  // The "acce_null" node's matrix; 0 for none.
  uint8_t unknown_7204[12];
  Vector accessory_node;  // The interior accessory's anchor (the node's point).
  // The body XPR's records of the interior's textures: "<car>_inte_off"
  // (set b "_inte_on"), then "..._0" 1 to 5; -1 for none. The car record's
  // +3676 picks one.
  i32_be interior_textures[6];
  ptr<ScenePoolSet> first_set;
  uint8_t unknown_7260[4];
};
GUEST_OFFSET(CarModel, record, 32);
GUEST_OFFSET(CarModel, loads, 3728);
GUEST_OFFSET(CarModel, tire_matrices, 6720);
GUEST_OFFSET(CarModel, seat_matrices, 7040);
GUEST_OFFSET(CarModel, seat_positions, 7168);
GUEST_OFFSET(CarModel, accessory_matrix, 7200);
GUEST_OFFSET(CarModel, accessory_node, 7216);
GUEST_OFFSET(CarModel, interior_textures, 7232);
GUEST_OFFSET(CarModel, first_set, 7256);
GUEST_SIZE(CarModel, 7264);

// An entry of an XPR's resource table (its word 4; word 5 the count).
struct XprResource {
  uint8_t unknown_0[12];
  ptr<char> name;
};
GUEST_SIZE(XprResource, 16);

// The cockpit (pool 2 of the first set; 3,520 bytes): set c's node file
// and body, six exterior parts picked by the record's tuning choices, the
// accessory's node and the interior's texture.
struct CockpitObject {
  ScenePoolObjectHead head;
  CockpitRecord record;
  uint8_t unknown_172[4];
  CockpitLoads loads;
  u32_be accessory_matrix;  // The "acce_null" node's matrix; 0 for none.
  uint8_t unknown_3476[12];
  Vector accessory_node;
  u32_be texture;  // The interior's texture, given to the interior's parts.
  ptr<ScenePoolSet> first_set;
  uint8_t unknown_3512[8];
};
GUEST_OFFSET(CockpitObject, record, 28);
GUEST_OFFSET(CockpitObject, loads, 176);
GUEST_OFFSET(CockpitObject, accessory_matrix, 3472);
GUEST_OFFSET(CockpitObject, accessory_node, 3488);
GUEST_OFFSET(CockpitObject, texture, 3504);
GUEST_OFFSET(CockpitObject, first_set, 3508);
GUEST_SIZE(CockpitObject, 3520);

// The interior (pool 3 of the first set; 3,104 bytes): its record and six
// parts (meters, roll cage, interior, gauges, seats, steering wheel).
struct InteriorObject {
  ScenePoolObjectHead head;
  InteriorRecord record;
  uint8_t unknown_148[12];
  LoadSlot part_slots[6];
  PartModel parts[6];
  u32_be in_cockpit;  // A cockpit's interior: its parts come from set c.
  ptr<ScenePoolSet> first_set;
  uint8_t unknown_3096[8];
};
GUEST_OFFSET(InteriorObject, record, 28);
GUEST_OFFSET(InteriorObject, part_slots, 160);
GUEST_OFFSET(InteriorObject, parts, 304);
GUEST_OFFSET(InteriorObject, in_cockpit, 3088);
GUEST_OFFSET(InteriorObject, first_set, 3092);
GUEST_SIZE(InteriorObject, 3104);

// A traffic car (pool 6 of the first set; 7,408 bytes): its record, loads,
// plate, textures and tires.
struct TrafficCarObject {
  ScenePoolObjectHead head;
  uint8_t unknown_28[4];
  SceneCarRecord record;
  TrafficLoads loads;
  TexturePair plate;  // CarModel::MakePlate's two textures.
  i32_be body_texture;     // The body XPR's "<name>_ex" record, -1 for none.
  i32_be variant_texture;  // The body XPR's "<name>_ex_0<variant>" record, -1 for none.
  uint8_t unknown_7064[8];
  Vector tire_positions[4];  // Front left, front right, rear left, rear right.
  Matrix tire_matrices[4];   // The nodes "tire_fl", "tire_fr", "tire_rl", "tire_rr".
  ptr<ScenePoolSet> first_set;
  uint8_t unknown_7396[12];
};
GUEST_OFFSET(TrafficCarObject, record, 32);
GUEST_OFFSET(TrafficCarObject, loads, 3728);
GUEST_OFFSET(TrafficCarObject, plate, 7040);
GUEST_OFFSET(TrafficCarObject, body_texture, 7056);
GUEST_OFFSET(TrafficCarObject, tire_positions, 7072);
GUEST_OFFSET(TrafficCarObject, tire_matrices, 7136);
GUEST_OFFSET(TrafficCarObject, first_set, 7392);
GUEST_SIZE(TrafficCarObject, 7408);

// --- The scene loaders' functions (scene_loaders.cpp) ---

// The paint a SetPaint walk hands on to PartModel::SetPaint: three colors
// (twelve floats) passed by value in r4 to r9, two floats to a register.
// SetPaint stores the registers whole and reads both halves, so all 64
// bits of each word are kept (with only the low words, half the paint's
// values were 0 and the cars drew black: John's test, 2026-11-20).
struct PartPaint {
  uint64_t words[6];
};

using guest::Guest;

uint32_t CarModelSelectedTexture(Guest& g, CarModel* model);
bool CarModelIsLoaded(const CarModel* model);
void CarModelReset(Guest& g, CarModel* model);
uint32_t CarPartsPart(CarParts* parts, int32_t kind, uint32_t base, uint32_t choice);
void CarPartsWalkMaterials(Guest& g, CarParts* parts);
bool CarPartsIsLoaded(CarParts* parts);
void CarPartsApplyPalette(Guest& g, CarParts* parts);
void CarPartsSetPaint(Guest& g, CarParts* parts, const PartPaint& paint);
bool CockpitBindTextures(Guest& g, CockpitObject* cockpit);
void CockpitApplyPalette(Guest& g, CockpitObject* cockpit);
void CockpitSetPaint(Guest& g, CockpitObject* cockpit, const PartPaint& paint);
bool InteriorIsLoaded(Guest& g, InteriorObject* interior);
void InteriorApplyPalette(Guest& g, InteriorObject* interior);
void InteriorSetPaint(Guest& g, InteriorObject* interior, const PartPaint& paint);
bool Pool4ObjectIsLoaded(const Pool4Object* object);
void Pool4ObjectSetRecord(Guest& g, Pool4Object* object, uint32_t record);
bool TrafficCarBindTextures(Guest& g, TrafficCarObject* car);
void TrafficCarApplyPalette(Guest& g, TrafficCarObject* car);
int32_t XprFindByName(XPR* xpr, const char* name);
void ScenePoolsCreateA(Guest& g, ScenePoolSet* set, const CarSceneRecord* record);

}  // namespace game
