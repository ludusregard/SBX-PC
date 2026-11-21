// The car models' loads as our own C++ (--native_car_models, on by
// default): the load slots, the car model's sets, decals and release, the
// vinyl sheet's textures, the interior's, cockpit's and wheels' loads, and
// the TGA reader the decals are decoded with.
//
// Stays recompiled: the vinyl drawing (Vinyl::DrawDecals, DrawDecal, the
// software rasterizer, Vinyl::Compress). The archive, the heap and the
// models' own functions are called as they are.

#include "game/car_models.h"

#include "base/settings.h"
#include "game/archive.h"
#include "game/load_poll.h"
#include "game/model_animation.h"
#include "game/scene.h"
#include "game/scene_models.h"
#include "hooks/parking_area_wheels.h"

#include <cmath>
#include <climits>
#include <cstring>
#include <utility>

SBX_SETTING_BOOL(native_car_models, true, "Overrides", "Cars",
                 "Our own C++ for the car models' loads (the load slots, the car model's "
                 "sets, decals and release, the vinyl sheet's textures, the interior's, "
                 "cockpit's and wheels' loads, the TGA reader) instead of the recompiled "
                 "functions; the drawing of the decals and plate stays recompiled");

namespace {

using game::AssetRecord;
using game::CarBodyLoads;
using game::CarModel;
using game::CarSceneRecord;
using game::CarSceneState;
using game::CockpitObject;
using game::DecalSlot;
using game::DeferredRelease;
using game::InteriorAccessoryLoad;
using game::InteriorObject;
using game::LoadSlot;
using game::PlateWords;
using game::RecordLayer;
using game::ScenePoolSet;
using game::TexturePair;
using game::TGAHeader;
using game::TGAImage;
using game::TGAReader;
using game::Wheels;
using game::XPR;
using guest::AddressOf;
using guest::be;
using guest::At;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

bool On() { return SBX_GET(native_car_models); }

// Writable globals.
constexpr uint32_t kCarScene = 0x826B2B24;  // Holds the car scene.

// The vinyl sheet's size and the corner cleared before it's compressed
// (writable).
struct SheetSize {
  u32_be width;
  u32_be height;
  u32_be corner_width;
  u32_be corner_height;
};
constexpr uint32_t kSheetSize = 0x825F2520;

SheetSize& Sheet() { return *At<SheetSize>(kSheetSize); }

constexpr uint32_t kEmptyText = 0x820021B0;  // "".
constexpr uint32_t kTgaTypeError = 0x8200668C;  // The debug text for a type it can't read.

// The vinyl sheet's format: A8R8G8B8.
constexpr uint32_t kSheetFormat = 0x18280086;

// The callbacks the loads hand Archive::Load.
constexpr uint32_t kOnFileLoaded = 0x820D4448;       // CarModel::OnFileLoaded (set a).
constexpr uint32_t kOnFileLoadedB = 0x820D4C28;      // CarModel::OnFileLoadedB (set b).
constexpr uint32_t kOnDecalLoaded = 0x820D53F8;      // CarModel::OnDecalLoaded.
constexpr uint32_t kOnCockpitLoaded = 0x820D61D0;    // The cockpit's loads.
constexpr uint32_t kOnInteriorPartLoaded = 0x820D7228;
constexpr uint32_t kOnWheelPartLoaded = 0x820D89E8;

// The asset ids: each set's files are a block of ids per car.
constexpr uint32_t kSetANodeFile = 892, kSetABody = 890, kSetAStride = 226;
constexpr uint32_t kSetBNodeFile = 11062, kSetBBody = 11060, kSetBStride = 197;
constexpr uint32_t kSetCNodeFile = 19927, kSetCBody = 19925, kSetCStride = 55;
constexpr uint32_t kVinylLayers = 162;       // + the layer's index, layers 0-2;
constexpr uint32_t kVinylLayersHigh = 188;   // layers 3-5.
constexpr uint32_t kStickers = 382;          // + the sticker's index, unless a team emblem.
constexpr uint32_t kTire = 581, kAftermarketTire = 583, kCaliper = 886;

// A sticker record whose kind is this names its asset id itself.
constexpr uint32_t kEmblemKind = 9;

// --- The game's functions these call ---

uint32_t Memcpy(Guest& g, uint32_t to, uint32_t from, uint32_t bytes) {
  return g.Call(sub_8258CB90, to, from, bytes);
}
void Memset(Guest& g, uint32_t to, uint32_t value, uint32_t bytes) {
  g.Call(sub_8258C950, to, value, bytes);
}

constexpr uint32_t kMemCommit = 0x1000, kPageReadWrite = 4, kMemRelease = 0x8000;
uint32_t VirtualAlloc(Guest& g, uint32_t bytes) {
  return g.Call(sub_82292DD8, 0u, bytes, kMemCommit, kPageReadWrite);
}
uint32_t VirtualFree(Guest& g, uint32_t address) {
  return g.Call(sub_82292D70, address, 0u, kMemRelease);
}

// FreePagesAt: the pages whose address is at `where` freed. VirtualFree's
// answer.
uint32_t FreePagesAt(Guest& g, const u32_be* where) { return VirtualFree(g, *where); }

uint32_t AssetSize(Guest& g, uint32_t id) { return g.Call(sub_820A0860, id); }

// Archive::Load: the request's serial.
int32_t ArchiveLoad(Guest& g, uint32_t id, uint32_t destination, uint32_t callback,
                    uint32_t context) {
  return int32_t(g.Call(sub_8209DF80, id, destination, callback, context, 0u, 0u));
}

CarSceneState& SceneLoads() { return *At<CarSceneState>(*At<u32_be>(kCarScene)); }

// --- Load slots ---

// LoadSlot::Reset.
void ResetSlot(LoadSlot* slot) {
  slot->unknown_0 = 0;   // The asset id.
  slot->unknown_4 = -1;  // The request.
  slot->unknown_8 = 0;   // The size.
  slot->block = 0;
  slot->keeps_block = 0;
  slot->state = 0;
}

constexpr uint32_t kSlotEmpty = 1, kSlotLoading = 2;

// LoadSlot::LoadShared: the asset into the car scene's load buffer when it
// fits (smaller than the buffer), else the slot is marked empty. 1 when
// it's loading.
uint32_t LoadShared(Guest& g, LoadSlot* slot, uint32_t id, uint32_t context,
                    uint32_t callback) {
  const uint32_t buffer = SceneLoads().load_buffer;
  const uint32_t buffer_size = SceneLoads().load_buffer_size;
  slot->unknown_0 = id;
  const uint32_t size = AssetSize(g, id);
  slot->unknown_8 = size;
  if (size == 0 || buffer_size <= size) {
    slot->state = kSlotEmpty;
    return 0;
  }
  slot->block = buffer;
  slot->unknown_4 = ArchiveLoad(g, slot->unknown_0, buffer, callback, context);
  slot->state = kSlotLoading;
  slot->keeps_block = 1;
  return 1;
}

// LoadSlot::Load: the same into pages allocated for it. 1 when it's
// loading.
uint32_t LoadAllocated(Guest& g, LoadSlot* slot, uint32_t id, uint32_t context,
                       uint32_t callback) {
  slot->unknown_0 = id;
  const uint32_t size = AssetSize(g, id);
  slot->unknown_8 = size;
  if (size == 0) {
    slot->state = kSlotEmpty;
    return 0;
  }
  slot->block = VirtualAlloc(g, size);
  if (slot->block == 0) {
    g.Call(sub_82293960);  // GetLastError (its answer isn't used)
  }
  slot->unknown_4 = ArchiveLoad(g, slot->unknown_0, slot->block, callback, context);
  slot->state = kSlotLoading;
  return 1;
}

// LoadSlot::ReleaseBuffer: the slot's own pages freed (a shared buffer is
// only let go). Returns the original's r3: the slot when shared, else 0
// without a buffer or VirtualFree's answer.
uint32_t ReleaseBuffer(Guest& g, LoadSlot* slot) {
  uint32_t result = AddressOf(slot);
  if (slot->keeps_block == 0) {
    result = slot->block;
    if (slot->block != 0) {
      result = VirtualFree(g, slot->block);
      slot->block = 0;
    }
  }
  if (slot->keeps_block != 0) {
    slot->block = 0;
  }
  return result;
}

// DeferRelease (sub_820C3D60): the four words put in the set's first free
// release record (state 2: to be released); nothing when none is free.
void QueueRelease(ScenePoolSet* set, const TexturePair& textures) {
  const int32_t count = int32_t(uint32_t(set->traffic_cars + set->car_models) << 2);
  for (int32_t i = 0; i < count; i++) {
    DeferredRelease& record = set->releases.get()[i];
    if (record.state != 0) {
      continue;
    }
    record.words[0] = textures.header;
    record.words[1] = textures.data;
    record.words[2] = textures.header_b;
    record.words[3] = textures.data_b;
    record.state = 2;
    return;
  }
}

// --- Textures ---

// Texture::Create: a texture's 52-byte header (VirtualAlloc'd, cleared) and
// its data (physical memory, cleared), one level of `format` at the size
// given; the addresses into [header_out] and [data_out].
void CreateTexture(Guest& g, uint32_t format, uint32_t width, uint32_t height, u32_be* header_out,
                   u32_be* data_out) {
  constexpr uint32_t kHeaderBytes = 52;
  guest::StackFrame frame(g.ctx, 160);
  *header_out = VirtualAlloc(g, kHeaderBytes);
  Memset(g, *header_out, 0, kHeaderBytes);
  // XGSetTextureHeader's arguments past r10 go on the stack: no base or mip
  // addresses, the header, no pitch.
  struct StackArguments {
    u32_be unknown_80;
    u32_be base_address;
    u32_be unknown_88;
    u32_be header;
    u32_be unknown_96;
    u32_be mip_address;
    u32_be unknown_104;
    u32_be pitch;
  };
  StackArguments* arguments = At<StackArguments>(frame.Local(80));
  *arguments = {};
  arguments->header = *header_out;
  constexpr uint32_t kLevels = 1, kUsage = 2;
  const uint32_t size =
      g.Call(sub_82354A58, width, height, kLevels, kUsage, format, 0u, 0u, 0u);  // XGSetTextureHeader
  constexpr uint32_t kWriteCombine = 0x404;  // PAGE_READWRITE | PAGE_WRITECOMBINE.
  *data_out = g.Call(sub_82293F10, size, -1, 0u, kWriteCombine);  // XPhysicalAlloc
  Memset(g, *data_out, 0, size);
  g.Call(sub_82354B30, uint32_t(*header_out), uint32_t(*data_out));  // Resource::Relocate
}

// Texture::Free: a texture's header and data (where their addresses are)
// freed and cleared when set. XPhysicalFree's answer, or 0 without data.
uint32_t FreeTexture(Guest& g, u32_be* header, u32_be* data) {
  if (*header != 0) {
    VirtualFree(g, *header);
    *header = 0;
  }
  uint32_t result = 0;
  if (*data != 0) {
    result = g.Call(sub_82294010, uint32_t(*data));  // XPhysicalFree
    *data = 0;
  }
  return result;
}

void CreateSheet(Guest& g, CarModel* model) {
  CreateTexture(g, kSheetFormat, Sheet().width, Sheet().height, &model->loads.sheet.header,
                &model->loads.sheet.data);
}

// --- The car model ---

// The team emblem's asset id for the car's block (18 to 44), or -1.
int32_t TeamEmblem(int32_t block) {
  static constexpr int32_t kEmblems[27] = {
      178, -1,  -1,  -1,  -1,  -1,  167, 168, -1,  169, 170, 171, 172, 173,
      174, 175, 176, 177, 183, 184, 185, 186, 187, 179, 180, 181, 182};
  const uint32_t index = uint32_t(block - 18);
  return index < 27 ? kEmblems[index] : -1;
}

// Sticker group 7's first record becomes the team emblem, for the blocks
// that have one.
void SetTeamEmblem(CarModel* model) {
  const int32_t block = model->record.unknown_0;
  if (block < 18) {
    return;
  }
  const int32_t emblem = TeamEmblem(block);
  if (emblem == -1) {
    return;
  }
  RecordLayer& first = model->record.layers[6];
  first.unknown_0 = emblem;
  first.kind = kEmblemKind;
}

// The sticker groups, by the order the stickers' slots are in: each group
// is six of the record's layers (group n at layer 6n).
constexpr int kStickerGroups[8] = {3, 2, 4, 5, 8, 7, 6, 1};

// CarModel::LoadDecals: the six vinyl layers, then the 48 stickers (the
// last first), each into its slot through the shared buffer; a negative
// index leaves the slot empty. The decals' state is 2, or 3 when nothing
// was asked for.
void LoadDecals(Guest& g, CarModel* model) {
  const uint32_t context = AddressOf(model);
  bool asked = false;
  for (int i = 0; i < CarBodyLoads::kLayers; i++) {
    LoadSlot* slot = &model->loads.layers[i].load;
    const int32_t index = model->record.layers[i].unknown_0;
    if (index < 0) {
      slot->state = kSlotEmpty;
      continue;
    }
    asked = true;
    const uint32_t first = i < 3 ? kVinylLayers : kVinylLayersHigh;
    LoadShared(g, slot, uint32_t(index) + first, context, kOnDecalLoaded);
  }
  for (int j = CarBodyLoads::kStickers - 1; j >= 0; j--) {
    const RecordLayer& record = model->record.layers[6 * kStickerGroups[j / 6] + j % 6];
    LoadSlot* slot = &model->loads.stickers[j].load;
    const int32_t index = record.unknown_0;
    if (index < 0) {
      slot->state = kSlotEmpty;
      continue;
    }
    asked = true;
    const uint32_t id = record.kind == kEmblemKind ? uint32_t(index) : uint32_t(index) + kStickers;
    LoadShared(g, slot, id, context, kOnDecalLoaded);
  }
  model->loads.decals_state = 2;
  if (!asked) {
    model->loads.decals_state = 3;
  }
}

// CarModel::LoadSetA: the node file and the body XPR2, each into pages of
// its own (CarModel::AllocPages); a file of size 0 isn't loaded.
void LoadSetA(Guest& g, CarModel* model) {
  const uint32_t context = AddressOf(model);
  LoadSlot& node = model->loads.model.node_slot;
  node.unknown_0 = uint32_t(model->record.unknown_0) * kSetAStride + kSetANodeFile;
  node.unknown_8 = AssetSize(g, node.unknown_0);
  game::CarModelAllocPages(g, &node.block, uint32_t(node.unknown_8));
  if (node.unknown_8 > 0) {
    const int32_t serial = ArchiveLoad(g, node.unknown_0, node.block, kOnFileLoaded, context);
    node.state = kSlotLoading;
    node.unknown_4 = serial;
  }
  LoadSlot& body = model->loads.model.body_slots[0];
  body.unknown_0 = uint32_t(model->record.unknown_0) * kSetAStride + kSetABody;
  body.unknown_8 = AssetSize(g, body.unknown_0);
  game::CarModelAllocPages(g, &body.block, uint32_t(body.unknown_8));
  if (body.unknown_8 != 0) {
    const int32_t serial = ArchiveLoad(g, body.unknown_0, body.block, kOnFileLoaded, context);
    body.state = kSlotLoading;
    body.unknown_4 = serial;
  } else {
    body.state = kSlotEmpty;
  }
}

// CarModel::LoadSetB: the node file into pages of its own, the body XPR2
// into the shared buffer (its size not checked).
void LoadSetB(Guest& g, CarModel* model) {
  const uint32_t context = AddressOf(model);
  LoadAllocated(g, &model->loads.model.node_slot,
                uint32_t(model->record.unknown_0) * kSetBStride + kSetBNodeFile, context,
                kOnFileLoadedB);
  const uint32_t buffer = SceneLoads().load_buffer;
  const uint32_t id = uint32_t(model->record.unknown_0) * kSetBStride + kSetBBody;
  LoadSlot& body = model->loads.model.body_slots[0];
  body.unknown_0 = id;
  body.unknown_8 = AssetSize(g, id);
  body.unknown_4 = ArchiveLoad(g, id, buffer, kOnFileLoadedB, context);
  body.state = kSlotLoading;
}

// CarModel::LoadExterior: the set-up copied in, a new vinyl sheet, the team
// emblem, the model set's node file and body, then the decals.
void LoadExterior(Guest& g, CarModel* model, uint32_t setup) {
  Memcpy(g, AddressOf(&model->record), setup, uint32_t(sizeof(model->record)));
  CreateSheet(g, model);
  SetTeamEmblem(model);
  const uint32_t set = model->first_set->model_set;
  if (set == 0) {
    LoadSetA(g, model);
  } else if (set == 1) {
    LoadSetB(g, model);
  }
  LoadDecals(g, model);
}

// CarModel::ReloadDecals: the old sheet put off for release, the first
// three layers' and six stickers' slots reset, a new sheet, the team
// emblem, then the decals. (The record's plate image, drawn into the new
// sheet, is the original's: the adapter's check.)
void ClearHeader(TGAHeader* header);

// The record's word that draws the car scene's plate image into the sheet
// (CarModel::ReloadDecals, and OnFileLoaded for the first body).
bool DrawsPlateImage(const CarModel* model) { return model->record.unknown_3676[3] == 1; }

// The locals the plate image's draw leaves in its caller's frame (at the
// same offsets in ReloadDecals' and OnFileLoaded's), where Vinyl::DrawDecal
// reads its arguments past the registers (+96 the scale, +116 the pixels,
// +132 the sheet's data).
struct PlateImageLocals {
  uint8_t unknown_0[96];
  game::Vector scale;
  uint8_t unknown_112[4];
  u32_be pixels;
  uint8_t unknown_120[4];
  u32_be unknown_124;
  uint8_t unknown_128[4];
  u32_be sheet_data;
  uint8_t unknown_136[4];
  u32_be sheet_data_b;
  u32_be image_pixels;
  uint8_t unknown_148[12];
  game::Vector scale_source;
  game::Vector offsets;
  TGAHeader image;
};
GUEST_OFFSET(PlateImageLocals, pixels, 116);
GUEST_OFFSET(PlateImageLocals, sheet_data, 132);
GUEST_OFFSET(PlateImageLocals, image_pixels, 144);
GUEST_OFFSET(PlateImageLocals, offsets, 176);
GUEST_OFFSET(PlateImageLocals, image, 192);

// Vinyl::DrawDecal for the plate image: region 8 of the sheet, the image
// at the offsets, no turn; its arguments past the registers in the frame at
// `frame` (defined with the rasterizer, below).
void DrawSheetDecal(Guest& g, const TGAHeader& image, const game::Vector& offsets,
                    uint32_t frame);

// The car scene's plate image (sub_820C4C60) drawn over the whole sheet
// (Vinyl::DrawDecal, region 8, scale 1, no offset or turn), from the
// caller's frame at `frame` (`second_header` another header it clears).
void DrawPlateImage(Guest& g, CarModel* model, uint32_t frame, uint32_t second_header) {
  PlateImageLocals* locals = At<PlateImageLocals>(frame);
  locals->image_pixels = 0;
  ClearHeader(&locals->image);
  ClearHeader(At<TGAHeader>(second_header));
  g.Call(sub_820C4C60, *At<u32_be>(kCarScene), &locals->image, &locals->image_pixels);  // CarScene::PlateImage
  const float one = *At<guest::f32_be>(0x82000A20);   // 1.0 (read-only)
  const float zero = *At<guest::f32_be>(0x82000A24);  // 0.0 (read-only)
  locals->scale_source = {one, one, one, one};
  locals->unknown_124 = 0;
  locals->sheet_data_b = model->loads.sheet.data_b;
  locals->sheet_data = model->loads.sheet.data;
  locals->pixels = locals->image_pixels;
  locals->offsets = {zero, zero, zero, one};
  Memcpy(g, AddressOf(&locals->scale), AddressOf(&locals->scale_source), 16);
  DrawSheetDecal(g, locals->image, locals->offsets, frame);
  // (The two headers' destructors do nothing.)
}

void ReloadDecals(Guest& g, CarModel* model) {
  constexpr uint32_t kFrameBytes = 320;  // The original's: the plate image's locals.
  guest::StackFrame frame(g.ctx, kFrameBytes);
  QueueRelease(model->first_set, model->loads.sheet);
  model->loads.decals_state = 0;
  for (int k = 0; k < 3; k++) {
    ResetSlot(&model->loads.layers[k].load);
  }
  for (int k = 0; k < 6; k++) {
    ResetSlot(&model->loads.stickers[k].load);
  }
  CreateSheet(g, model);
  if (DrawsPlateImage(model)) {
    DrawPlateImage(g, model, frame.Local(0), frame.Local(224));
  }
  SetTeamEmblem(model);
  LoadDecals(g, model);
}

// CarModel::Release: the node file's and bodies' pages, the skeleton's node
// values, both sheets put off for release, then CarModel::Reset.
void Release(Guest& g, CarModel* model) {
  ReleaseBuffer(g, &model->loads.model.node_slot);
  g.Call(sub_8257EF38, model->loads.model.skeleton);  // Skeleton::FreeNodeValues
  for (LoadSlot& body : model->loads.model.body_slots) {
    ReleaseBuffer(g, &body);
  }
  QueueRelease(model->first_set, model->loads.sheet);
  QueueRelease(model->first_set, model->loads.plate);
  game::CarModelReset(g, model);
}

// Vinyl::ClearCorner: the sheet's bottom-left corner cleared.
void ClearCorner(Guest& g, CarModel* model) {
  const SheetSize& size = Sheet();
  uint32_t row = model->loads.sheet.data + (size.height - size.corner_height) * size.width * 4;
  for (int32_t y = 0; y < int32_t(uint32_t(size.corner_height)); y++) {
    Memset(g, row, 0, size.corner_width * 4);
    row += size.width * 4;
  }
}

bool AnyDecalLoading(const CarModel* model) {
  for (const DecalSlot& decal : model->loads.layers) {
    if (decal.load.state == kSlotLoading || decal.unknown_24.state == kSlotLoading) {
      return true;
    }
  }
  for (const DecalSlot& decal : model->loads.stickers) {
    if (decal.load.state == kSlotLoading || decal.unknown_24.state == kSlotLoading) {
      return true;
    }
  }
  return false;
}

// CarModel::OnDecalLoaded: the request's slot (a layer's, else a
// sticker's) marked arrived, the arrived decals drawn into the sheet
// (Vinyl::DrawDecals) and the slot reset; once none is loading, the
// sheet's corner cleared and the sheet compressed. Returns the original's
// r3: Vinyl::Compress's, else the slot, or the request when no slot has it.
void VinylDrawDecals(Guest& g, CarModel* model);
uint32_t CompressSheet(Guest& g, uint32_t width, uint32_t height, u32_be* header_at,
                       u32_be* data_at);

uint32_t OnDecalLoaded(Guest& g, CarModel* model, uint32_t request) {
  constexpr uint32_t kArrived = 3;
  uint32_t result = request;
  auto arrive = [&](DecalSlot& decal) {
    if (uint32_t(int32_t(decal.load.unknown_4)) != request) {
      return false;
    }
    decal.load.state = kArrived;
    VinylDrawDecals(g, model);
    ResetSlot(&decal.load);
    result = AddressOf(&decal.load);
    return true;
  };
  bool found = false;
  for (DecalSlot& decal : model->loads.layers) {
    if ((found = arrive(decal))) {
      break;
    }
  }
  if (!found) {
    for (DecalSlot& decal : model->loads.stickers) {
      if (arrive(decal)) {
        break;
      }
    }
  }
  if (AnyDecalLoading(model)) {
    return result;
  }
  ClearCorner(g, model);
  result = CompressSheet(g, Sheet().width, Sheet().height, &model->loads.sheet.header,
                         &model->loads.sheet.data);  // Vinyl::Compress
  model->loads.decals_state = 3;
  return result;
}

// --- The interior, cockpit and wheels ---

// Interior::LoadParts: six consecutive ids from base + block x stride +
// offset into the part slots, through the shared buffer. The last load's
// answer.
uint32_t LoadInteriorParts(Guest& g, InteriorObject* interior, int32_t base, int32_t stride,
                           int32_t offset) {
  const int32_t first = interior->head.car_model->record.unknown_0 * stride + base + offset;
  uint32_t loading = 0;
  for (int i = 0; i < 6; i++) {
    loading = LoadShared(g, &interior->part_slots[i], uint32_t(first + i), AddressOf(interior),
                         kOnInteriorPartLoaded);
  }
  return loading;
}

// Interior::Load: the record copied in; the parts by the model set (a
// cockpit's from set c), then the accessories' set loads its own (its slot
// 1: once with the record's accessory for set b, three times for set a,
// each answer's +808 cleared).
void LoadInterior(Guest& g, InteriorObject* interior, uint32_t setup) {
  const uint32_t accessories = game::AccessoriesGet();
  Memcpy(g, AddressOf(&interior->record), setup, uint32_t(sizeof(interior->record)));
  const uint32_t set = interior->first_set->model_set;
  const bool cockpit = interior->in_cockpit != 0;
  constexpr uint32_t kLoadSlot = 1;
  if (set == 1) {
    if (cockpit) {
      LoadInteriorParts(g, interior, kSetCBody, kSetCStride, 49);
    } else {
      LoadInteriorParts(g, interior, kSetBBody, kSetBStride, 190);
    }
    g.CallVirtual(At<u32_be>(accessories), kLoadSlot,
                  uint32_t(int32_t(interior->record.accessory)),
                  uint32_t(interior->record.unknown_116));
  } else if (set == 0) {
    if (cockpit) {
      LoadInteriorParts(g, interior, kSetCBody, kSetCStride, 49);
    } else {
      LoadInteriorParts(g, interior, kSetABody, kSetAStride, 219);
    }
    for (uint32_t i = 0; i < 3; i++) {
      const uint32_t entry = g.CallVirtual(At<u32_be>(accessories), kLoadSlot, i, 0u);
      At<InteriorAccessoryLoad>(entry)->unknown_808 = 0;
    }
  }
}

// Cockpit::LoadSetCBody: the record copied in; set c's node file into
// pages of its own and its body into the shared buffer (no size check);
// then six exterior parts by the record's tuning choices. The last load's
// answer.
uint32_t LoadCockpit(Guest& g, CockpitObject* cockpit, uint32_t setup) {
  Memcpy(g, AddressOf(&cockpit->record), setup, uint32_t(sizeof(cockpit->record)));
  const uint32_t context = AddressOf(cockpit);
  const uint32_t block = uint32_t(cockpit->head.car_model->record.unknown_0);
  // (When the node file is empty the original formats a message into its
  // frame that nothing reads.)
  LoadAllocated(g, &cockpit->loads.model.node_slot, block * kSetCStride + kSetCNodeFile, context,
                kOnCockpitLoaded);
  const uint32_t body_id = block * kSetCStride + kSetCBody;
  const uint32_t buffer = SceneLoads().load_buffer;
  LoadSlot& body = cockpit->loads.model.body_slots[0];
  body.unknown_0 = body_id;
  body.unknown_8 = AssetSize(g, body_id);
  body.unknown_4 = ArchiveLoad(g, body_id, buffer, kOnCockpitLoaded, context);
  body.state = kSlotLoading;
  // Each part: the body's id + its offset + the record's choice (the cabin
  // has none).
  const u32_be* choices = cockpit->record.unknown_0;
  const uint32_t parts[6] = {
      body_id + choices[0] + 3,  body_id + choices[1] + 16, body_id + choices[2] + 19,
      body_id + choices[3] + 31, body_id + choices[4] + 36, body_id + 35,
  };
  uint32_t loading = 0;
  for (int i = 0; i < 6; i++) {
    loading = LoadShared(g, &cockpit->loads.part_slots[i], parts[i], context, kOnCockpitLoaded);
  }
  return loading;
}

// Wheels::SetUp: the record copied in, the wheel parts picked for the model
// set (set a's, 587 + 3i, or set b's, 588 + 3i; set a's in the parking area
// with --parking_area_garage_wheels), then for the front and rear the tire,
// the part, its XPR2 and the caliper, through the shared buffer. The last
// load's answer.
uint32_t SetUpWheels(Guest& g, Wheels* wheels, uint32_t setup) {
  Memcpy(g, AddressOf(&wheels->record), setup, uint32_t(sizeof(wheels->record)));
  if (wheels->first_set->model_set == 0 || hooks::ParkingArea_GarageWheels()) {
    g.Call(sub_820D8860, wheels);  // Wheels::PickSetA
  } else {
    g.Call(sub_820D8900, wheels);  // Wheels::PickSetB
  }
  const uint32_t context = AddressOf(wheels);
  uint32_t loading = 0;
  for (int end = 0; end < 2; end++) {
    const bool aftermarket = wheels->record.unknown_116[end] != 0;
    LoadShared(g, &wheels->loads.slots_0[end], aftermarket ? kAftermarketTire : kTire, context,
               kOnWheelPartLoaded);
    LoadSlot* part = &wheels->loads.slots_976[end];
    LoadShared(g, part, part->unknown_0, context, kOnWheelPartLoaded);
    LoadSlot* xpr = &wheels->loads.slots_1952[end];
    LoadShared(g, xpr, xpr->unknown_0, context, kOnWheelPartLoaded);
    loading =
        LoadShared(g, &wheels->loads.slots_2088[end], kCaliper, context, kOnWheelPartLoaded);
  }
  return loading;
}

// --- The node file's and bodies' arrival ---

constexpr uint32_t kCarNames = 0x825F1878;   // A name's address a block (writable).
constexpr uint32_t kNameFormat = 0x82004FC8;  // "%s%s"
constexpr uint32_t kNumberedFormat = 0x82004F84;  // "%s%s%d"

// The texture names' suffixes: set a's "_inte_off", set b's "_inte_on",
// and the numbered ones' ("..._0" then 1 to 5).
struct InteriorSuffixes {
  uint32_t name, numbered;
};
constexpr InteriorSuffixes kSetASuffixes = {0x82004FD0, 0x82004F78};
constexpr InteriorSuffixes kSetBSuffixes = {0x82004FE8, 0x82004FDC};

// The node names whose matrices the car model keeps (read-only strings).
constexpr uint32_t kTireFL = 0x82004F8C, kTireFR = 0x82004F94, kTireRL = 0x82004F9C,
                   kTireRR = 0x82004FA4, kSeatL = 0x82004FAC, kSeatR = 0x82004FB4,
                   kAccessoryNull = 0x82004FBC;

// Read-only constants.
constexpr uint32_t kOne = 0x82000A20;          // 1.0
constexpr uint32_t kZero = 0x82000A24;         // 0.0
constexpr uint32_t kNodeScale = 0x82001F94;    // 0.1
constexpr uint32_t kQuarterTurn = 0x82002A2C;  // -pi/2

float Constant(uint32_t address) { return *At<guest::f32_be>(address); }

// CarModel::OnFileLoaded's frame (1,552 bytes): the matrices the node
// points are turned by and the texture name the callees are given.
struct FileLoadedFrame {
  uint8_t unknown_0[224];
  game::Matrix turn;
  game::Matrix transform;
  TGAHeader plate_header;  // Cleared with the plate image's (the other at +192).
  uint8_t unknown_372[12];
  char name[1168];
};
GUEST_OFFSET(FileLoadedFrame, turn, 224);
GUEST_OFFSET(FileLoadedFrame, name, 384);
GUEST_SIZE(FileLoadedFrame, 1552);

// A node point: a matrix's position with w 1.0, scaled by 0.1 and turned
// by the transform (Vec4::Transform in place).
void NodePointFrom(Guest& g, game::Vector* point, const game::Vector& position,
                   const game::Matrix& transform) {
  *point = position;
  point->w = Constant(kOne);
  const float scale = Constant(kNodeScale);
  point->x = point->x * scale;
  point->y = point->y * scale;
  point->z = point->z * scale;
  g.Call(sub_820AB518, point, &transform, point);  // Vec4::Transform
}

// The node file arrived: the skeleton bound to it and posed, the tires',
// seats' and accessory's node matrices kept, and their points turned a
// quarter about y into the car model's frame.
void NodeFileLoaded(Guest& g, CarModel* model, FileLoadedFrame* frame) {
  game::Skeleton* skeleton = At<game::Skeleton>(AddressOf(model->loads.model.skeleton));
  g.Call(sub_8257F1D8, skeleton, uint32_t(model->loads.model.node_slot.block), 0u);  // Skeleton::Bind
  g.Call(sub_8257F2A8, skeleton);  // Skeleton::ResetPose
  g.Call(sub_8257F410, skeleton);  // Skeleton::LocalMatrices
  g.Call(sub_8257F058, skeleton);  // Skeleton::UpdateMatrices
  model->loads.model.node_slot.state = 3;
  const int32_t count = int32_t(g.Call(sub_8257F038, skeleton));  // Skeleton::NodeCount
  struct Kept {
    uint32_t name;
    game::Matrix* matrix;
  };
  const Kept kept[6] = {{kTireFL, &model->tire_matrices[0]}, {kTireFR, &model->tire_matrices[1]},
                        {kTireRL, &model->tire_matrices[2]}, {kTireRR, &model->tire_matrices[3]},
                        {kSeatL, &model->seat_matrices[0]},  {kSeatR, &model->seat_matrices[1]}};
  int found = 0;
  for (int32_t k = 0; k < count; k++) {
    // A node's name is its record's first bytes.
    const char* name = reinterpret_cast<const char*>(skeleton->Node(uint32_t(k))->unknown_0);
    for (const Kept& keep : kept) {
      if (strcmp(At<char>(keep.name), name) == 0) {
        g.Call(sub_820A0E18, keep.matrix, skeleton->MatrixAt(uint32_t(k)));  // Matrix::Copy
        found++;
      }
    }
    if (strcmp(At<char>(kAccessoryNull), name) == 0) {
      model->accessory_matrix = AddressOf(skeleton->MatrixAt(uint32_t(k)));
      found++;
    }
  }

  // The transform: the identity turned a quarter about y.
  game::Matrix identity;
  identity.right = {1.0f, 0.0f, 0.0f, 0.0f};
  identity.up = {0.0f, 1.0f, 0.0f, 0.0f};
  identity.forward = {0.0f, 0.0f, 1.0f, 0.0f};
  identity.position = {0.0f, 0.0f, 0.0f, 1.0f};
  frame->turn = identity;
  frame->transform = frame->turn;
  g.ctx.f1.f64 = double(Constant(kQuarterTurn));
  g.Call(sub_820A68E8, &frame->turn);  // Matrix::RotationY
  g.Call(sub_8209BE70, &frame->transform, &frame->transform, &frame->turn);  // Matrix::Multiply

  for (int i = 0; i < 4; i++) {
    NodePointFrom(g, &model->tire_positions[i], model->tire_matrices[i].position, frame->transform);
  }
  for (game::Vector& point : model->tire_positions) {
    point.w = Constant(kZero);
  }
  if (found == 7) {
    for (int i = 0; i < 2; i++) {
      NodePointFrom(g, &model->seat_positions[i], model->seat_matrices[i].position,
                    frame->transform);
    }
    for (game::Vector& point : model->seat_positions) {
      point.w = Constant(kZero);
    }
  }
  if (model->accessory_matrix != 0) {
    NodePointFrom(g, &model->accessory_node, At<game::Matrix>(model->accessory_matrix)->position,
                  frame->transform);
  }
}

void MakePlate(Guest& g, u32_be* header_a, u32_be* data_a, u32_be* header_b, u32_be* data_b,
               const PlateWords& words);

// A body XPR2 arrived (texture set i): its XPR made over the file (set a's
// own pages, freed after; set b's the shared buffer) and given to the
// texture provider. For the first, the license plate made and the
// interior's textures looked up by name.
void BodyLoaded(Guest& g, CarModel* model, FileLoadedFrame* frame, int i, bool set_b,
                uint32_t buffer, const InteriorSuffixes& suffixes) {
  LoadSlot& body = model->loads.model.body_slots[i];
  g.Call(sub_8257C880, &model->loads.model.xprs[i], set_b ? buffer : uint32_t(body.block));  // XPR::Create
  constexpr uint32_t kSetXpr = 4;
  g.CallVirtual(&model->loads.model.texture_providers[i], kSetXpr, 0u, &model->loads.model.xprs[i]);
  if (set_b) {
    body.block = 0;
    body.state = 3;
  } else {
    body.state = 3;
    FreePagesAt(g, &body.block);
    body.block = 0;
  }
  if (i != 0) {
    return;
  }
  if (DrawsPlateImage(model)) {
    DrawPlateImage(g, model, AddressOf(frame), AddressOf(&frame->plate_header));
  }

  // The plates' textures, from the record's part order.
  TexturePair& plate = model->loads.plate;
  MakePlate(g, &plate.header, &plate.data, &plate.header_b, &plate.data_b,
            PlateWords::Of(model->record.part_order));

  // The interior's textures in the body XPR: "<car><suffix>", then the
  // numbered ones.
  XPR* xpr = &model->loads.model.xprs[0];
  model->interior_textures[0] = game::XprFindByName(xpr, frame->name);
  for (uint32_t n = 1; n < 6; n++) {
    const uint32_t car = *guest::Element<u32_be>(kCarNames, uint32_t(model->record.unknown_0));
    g.Call(sub_8258ED70, frame->name, kNumberedFormat, car, suffixes.numbered, n);  // sprintf
    model->interior_textures[n] = game::XprFindByName(xpr, frame->name);
  }
}

// CarModel::OnFileLoaded and OnFileLoadedB: Archive::Load's callback for
// set a's and set b's files. The texture name "<car><suffix>" made in the
// frame; then the body whose request it is, or the node file.
void FileLoaded(Guest& g, CarModel* model, uint32_t request, uint32_t buffer, bool set_b) {
  const InteriorSuffixes& suffixes = set_b ? kSetBSuffixes : kSetASuffixes;
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(FileLoadedFrame)));
  FileLoadedFrame* frame = At<FileLoadedFrame>(stack.Local(0));
  const uint32_t car = *guest::Element<u32_be>(kCarNames, uint32_t(model->record.unknown_0));
  g.Call(sub_8258ED70, frame->name, kNameFormat, car, suffixes.name);  // sprintf
  for (int i = 0; i < 2; i++) {
    if (uint32_t(int32_t(model->loads.model.body_slots[i].unknown_4)) == request) {
      BodyLoaded(g, model, frame, i, set_b, buffer, suffixes);
      return;
    }
  }
  if (uint32_t(int32_t(model->loads.model.node_slot.unknown_4)) == request) {
    NodeFileLoaded(g, model, frame);
  }
}

// SceneWheels::SetUp: r4's 156-byte record copied in, the defaults for
// the car's block (or a traffic car's) filled in, copied back to r4; for
// blocks 18 and up with both wheel numbers -1 their fixed wheels
// (Wheels::BlockDefault); aftermarket tires for wheel numbers other than
// 190; then pool 5 of the first set claimed, linked to the car model (or
// the traffic record) and set up with the record. Returns
// ScenePools::SetUp's r3.
void SetUpSceneWheels(Guest& g, game::PolledWheels* wheels, uint32_t setup) {
  constexpr uint32_t kFirstSet = 0x826B2B20;  // Holds the first pool set (writable).
  constexpr uint32_t kWheelDefaults = 0x82004490;
  constexpr uint32_t kTrafficRows = 45;
  constexpr int32_t kStockWheel = 190;
  constexpr uint32_t kWheelsPool = 5;
  const uint32_t first = *At<u32_be>(kFirstSet);
  game::WheelsRecord& record = wheels->record;
  Memcpy(g, AddressOf(&record), setup, uint32_t(sizeof(record)));
  auto block = [&] { return uint32_t(wheels->car_model->model->record.unknown_0); };
  if (record.unknown_68[0] == 0) {
    const uint32_t row = record.unknown_0 == 3 ? *At<u32_be>(wheels->traffic_record) + kTrafficRows
                                               : block();
    const game::WheelDefaults& d = *guest::Element<game::WheelDefaults>(kWheelDefaults, row);
    record.unknown_68[3] = d.unknown_4[0];
    record.unknown_68[4] = d.unknown_4[1];
    record.caliper_texture = d.caliper_texture;
    record.unknown_92[0] = d.unknown_16[0];
    record.unknown_92[1] = d.unknown_16[1];
    record.unknown_100[0] = d.unknown_24[0];
    record.unknown_100[1] = d.unknown_24[1];
    record.unknown_108[0] = d.unknown_32[0];
    record.unknown_108[1] = d.unknown_32[1];
    record.unknown_116[0] = d.aftermarket_tires;
    record.unknown_116[1] = d.aftermarket_tires;
  } else {
    // The record's own caliper texture is kept.
    const game::WheelDefaults& d = *guest::Element<game::WheelDefaults>(kWheelDefaults, block());
    record.unknown_68[3] = d.unknown_4[0];
    record.unknown_68[4] = d.unknown_4[1];
    record.unknown_92[0] = d.unknown_16[0];
    record.unknown_92[1] = d.unknown_16[1];
    record.unknown_100[0] = d.unknown_24[0];
    record.unknown_100[1] = d.unknown_24[1];
    record.unknown_108[0] = d.unknown_32[0];
    record.unknown_108[1] = d.unknown_32[1];
    record.unknown_116[0] = d.aftermarket_tires;
    record.unknown_116[1] = d.aftermarket_tires;
  }
  Memcpy(g, setup, AddressOf(&record), uint32_t(sizeof(record)));
  i32_be& front = record.unknown_16.unknown_0;
  i32_be& rear = record.unknown_32.unknown_0;
  if (front == -1 && rear == -1 && wheels->car_model != nullptr &&
      int32_t(block()) >= 18) {
    g.Call(sub_820D3620, block(), &front, &rear);  // Wheels::BlockDefault
  }
  if (front >= 0) {
    record.unknown_116[0] = front != kStockWheel ? 1 : 0;
  }
  if (rear >= 0) {
    record.unknown_116[1] = rear != kStockWheel ? 1 : 0;
  }
  const uint32_t entry = g.Call(sub_820C40D8, first, kWheelsPool, 0u);  // ScenePools::ClaimA
  Wheels* object = At<Wheels>(g.Call(sub_820C4230, first, kWheelsPool, entry));  // ScenePools::GetA
  wheels->wheels = object;
  if (wheels->car_model != nullptr) {
    object->head.car_model = wheels->car_model->model;
  } else {
    object->traffic_record = wheels->traffic_record;
  }
  g.Call(sub_820C4148, first, kWheelsPool, entry, &record);  // ScenePools::SetUp
}

// CarScene::SetUp: the scene's state 3: the record's words and the pools'
// counts kept, the paint palette made, the load buffer picked (the
// archive's shared one unless the record has its own), the scene's assets
// loaded, then each second-set pool's objects allocated and constructed on
// the scene's list for it (each object's +20 its index), and the first
// set's pools made (ScenePools::CreateA).
void SetUpCarScene(Guest& g, CarSceneState* scene, const CarSceneRecord* record, uint32_t word) {
  constexpr uint32_t kArchiveState = 0x82694DE4;  // Writable.
  scene->unknown_4996 = word;
  scene->state = 3;
  scene->model_set = record->model_set;
  scene->car_models = record->car_models;
  scene->parts = record->car_parts;
  scene->interiors = record->interiors + record->cockpits + record->traffic_cars;
  scene->cockpits = record->cockpits;
  scene->lights = record->pool4_objects;
  scene->wheels = record->wheels + record->traffic_cars + record->unknown_28;
  scene->scene_models = record->traffic_cars + record->unknown_28;
  scene->traffic = record->traffic_cars;
  scene->own_buffer = record->load_buffer;
  scene->own_buffer_size = record->load_buffer_size;
  g.Call(sub_820D1F78);  // PaintPalette::Init
  if (record->load_buffer == 0) {
    const game::ArchiveState& archive = *At<game::ArchiveState>(kArchiveState);
    scene->load_buffer = archive.shared_buffer;
    scene->load_buffer_size = archive.shared_buffer_size;
  } else {
    scene->load_buffer = scene->own_buffer;
    scene->load_buffer_size = scene->own_buffer_size;
  }
  g.Call(sub_820C88F8, scene);  // CarScene::LoadAssets

  // The second set's pools: count, object size and constructor.
  struct Pool {
    i32_be CarSceneState::*count;
    uint32_t bytes;
    Guest::Function* construct;
  };
  const Pool pools[8] = {
      {&CarSceneState::car_models, 3744, sub_820DB3E0},     // SceneCarModel
      {&CarSceneState::parts, 37296, sub_820E2AE0},         // SceneParts
      {&CarSceneState::cockpits, 4320, sub_820DC058},       // SceneCockpit
      {&CarSceneState::interiors, 11680, sub_820E4F30},     // SceneInterior
      {&CarSceneState::lights, 13168, sub_820DDCA8},        // SceneLights
      {&CarSceneState::wheels, 16800, sub_820ECA78},        // SceneWheels
      {&CarSceneState::traffic, 9808, sub_820E6A60},        // SceneTraffic
      {&CarSceneState::scene_models, 5120, sub_820DAFF8},   // SceneModel
  };
  for (int p = 0; p < 8; p++) {
    const int32_t count = scene->*pools[p].count;
    for (int32_t i = 0; i < count; i++) {
      const uint32_t object = VirtualAlloc(g, pools[p].bytes);
      if (object == 0) {
        continue;  // (The original then stores the index at address 20.)
      }
      g.Call(pools[p].construct, object, &scene->pools[p]);
      At<game::ScenePoolObjectHeader>(object)->index = i;
    }
  }
  game::ScenePoolsCreateA(g, scene->first_set, record);
}

// --- The TGA reader ---

void ClearHeader(TGAHeader* header) {
  header->id_length = 0;
  header->color_map_type = 0;
  header->image_type = 0;
  header->color_map_first = 0;
  header->color_map_length = 0;
  header->color_map_depth = 0;
  header->x_origin = 0;
  header->y_origin = 0;
  header->width = 0;
  header->height = 0;
  header->bits_per_pixel = 0;
  header->descriptor = 0;
}

void ClearImage(TGAImage* image) {
  image->file = 0;
  image->header = nullptr;
  image->pixels = 0;
  image->size = 0;
  image->width = 0;
  image->height = 0;
}

// TGAReader::TGAReader: an empty name and a clear header and image.
void InitReader(Guest& g, TGAReader* reader) {
  g.Call(sub_820913F0, &reader->name, 0u, 0u);  // string::_Tidy
  ClearHeader(&reader->header);
  ClearImage(&reader->image);
  g.Call(sub_820918D0, &reader->name, kEmptyText, 0u);  // string::assign
  reader->file = 0;
}

// TGA::ParseHeader: the file's 18-byte header, its 16-bit fields
// little-endian.
void ParseHeader(TGAHeader* header, const uint8_t* file) {
  auto word = [file](int at) { return uint16_t(file[at] | file[at + 1] << 8); };
  header->id_length = file[0];
  header->color_map_type = file[1];
  header->image_type = file[2];
  header->color_map_first = word(3);
  header->color_map_length = word(5);
  header->color_map_depth = file[7];
  header->x_origin = word(8);
  header->y_origin = word(10);
  header->width = word(12);
  header->height = word(14);
  header->bits_per_pixel = file[16];
  header->descriptor = file[17];
}

constexpr uint32_t kTgaHeaderBytes = 18;
constexpr uint32_t kTgaError = 4;

// TGA::DecodeRle: run-length packets (a count byte, bit 7 set: one pixel
// repeated count + 1 times; else count + 1 raw pixels) into the image's
// pixels, until the pixels decoded reach width x height - 1 (as the game
// counts them: a 1-pixel image decodes nothing).
void DecodeRle(TGAImage* image, uint32_t source) {
  const uint32_t pixel_bytes = image->header->bits_per_pixel == 24 ? 3 : 4;
  uint32_t to = image->pixels;
  uint32_t from = source;
  uint32_t decoded = 0;
  if (image->height * image->width - 1 == 0) {
    return;
  }
  do {
    const uint8_t packet = *At<uint8_t>(from);
    const uint32_t count = (packet & 0x7F) + 1;
    from += 1;
    decoded += count;
    if (packet & 0x80) {
      const uint8_t* pixel = At<uint8_t>(from);
      for (uint32_t i = 0; i < count; i++) {
        memcpy(At<uint8_t>(to + i * pixel_bytes), pixel, pixel_bytes);
      }
      from += pixel_bytes;
    } else {
      memcpy(At<uint8_t>(to), At<uint8_t>(from), count * pixel_bytes);
      from += count * pixel_bytes;
    }
    to += count * pixel_bytes;
  } while (decoded < image->height * image->width - 1);
}

// TGA::Decode: 24- or 32-bit images only; the pixels allocated, then
// copied from the file (type 2) or run-length decoded (type 10). 0, or 4
// for an image it can't read.
uint32_t Decode(Guest& g, TGAImage* image, uint32_t file, TGAHeader* header) {
  image->header = header;
  image->file = file;
  image->width = uint16_t(header->width);
  image->height = uint16_t(header->height);
  const uint8_t bits = header->bits_per_pixel;
  if (bits != 24 && bits != 32) {
    return kTgaError;
  }
  const uint32_t pixels = image->height * image->width;
  image->size = bits == 32 ? pixels * 4 : pixels * 3;
  image->pixels = VirtualAlloc(g, image->size);
  switch (image->header->image_type) {
    case 2:
      Memcpy(g, image->pixels, file + kTgaHeaderBytes, image->size);
      return 0;
    case 10:
      DecodeRle(image, file + kTgaHeaderBytes);
      return 0;
    default:
      g.Call(sub_82293CB8, kTgaTypeError);  // OutputDebugStringA
      return kTgaError;
  }
}

// TGAReader::Load: 5 while it holds a file; else the header parsed and the
// image decoded (Decode's answer).
uint32_t LoadReader(Guest& g, TGAReader* reader, uint32_t size, uint32_t file) {
  if (reader->file != 0) {
    return 5;
  }
  reader->file_size = size;
  reader->file = file;
  ParseHeader(&reader->header, At<uint8_t>(file));
  return Decode(g, &reader->image, reader->file, &reader->header);
}

// TGA::ToARGB: the pixels as top-down 32-bit ARGB (the file's rows are
// bottom-up), into new pages at [out]; 24-bit pixels get alpha 255.
void ToARGB(Guest& g, const TGAImage* image, u32_be* out) {
  const bool rgb = image->header->bits_per_pixel == 24;
  const uint32_t width = image->width, height = image->height;
  *out = VirtualAlloc(g, rgb ? width * height * 4 : uint32_t(image->size));
  u32_be* to = At<u32_be>(*out);
  const uint8_t* from = At<uint8_t>(image->pixels);
  for (uint32_t y = 0; y < height; y++) {
    u32_be* row = to + width * y;
    const uint32_t source_row = (height - y - 1) * width;
    for (uint32_t x = 0; x < width; x++) {
      if (rgb) {
        const uint8_t* p = from + (source_row + x) * 3;  // B, G, R.
        row[x] = 0xFF000000u | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
      } else {
        const uint8_t* p = from + (source_row + x) * 4;  // B, G, R, A.
        row[x] = uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
      }
    }
  }
}

// TGAImage::Free: its pixels freed. Returns the original's r3.
uint32_t FreeImage(Guest& g, TGAImage* image) {
  if (image->pixels == 0) {
    return 0;
  }
  const uint32_t result = VirtualFree(g, image->pixels);
  image->pixels = 0;
  return result;
}

// TGAReader::~TGAReader: the file and the pixels freed, the name destroyed
// (the header's destructor does nothing).
void FreeReader(Guest& g, TGAReader* reader) {
  if (reader->file != 0) {
    VirtualFree(g, reader->file);
    reader->file = 0;
  }
  FreeImage(g, &reader->image);
  g.Call(sub_820913F0, &reader->name, 1u, 0u);  // string::_Tidy
}

// TGA::CreateImage: a TGA file decoded into new ARGB pixels at [pixels_out]
// and its header copied out; the file is kept.
void CreateImage(Guest& g, TGAHeader* header_out, uint32_t size, uint32_t file,
                 u32_be* pixels_out) {
  guest::StackFrame frame(g.ctx, 208);  // The reader is the original's local.
  TGAReader* reader = At<TGAReader>(frame.Local(80));
  InitReader(g, reader);
  LoadReader(g, reader, size, file);
  ToARGB(g, &reader->image, pixels_out);
  Memcpy(g, AddressOf(header_out), AddressOf(&reader->header), uint32_t(sizeof(TGAHeader)));
  reader->file = 0;  // Not the reader's to free.
  FreeReader(g, reader);
}

// --- The plate's images ---

// PlateImage: the car scene's shared image `index` of a kind (its TGA
// header copied out, its pixels' address into [pixels_out]).
uint32_t PlateImage(Guest& g, CarSceneState* scene, uint32_t kind, uint32_t index, uint32_t header_out,
                    u32_be* pixels_out) {
  static constexpr uint32_t kFirstOfKind[5] = {0, 4, 7, 11, 15};
  const uint32_t image = (kind < 5 ? kFirstOfKind[kind] : 0) + index;
  const AssetRecord* header = guest::Element<AssetRecord>(
      AddressOf(scene->assets.records_3668), image);
  const uint32_t result = Memcpy(g, header_out, AddressOf(header), uint32_t(sizeof(AssetRecord)));
  *pixels_out = *guest::Element<u32_be>(AddressOf(scene->assets.pages_3592), image);
  return result;
}

// A rectangle in an image of 32-bit pixels.
struct PixelRect {
  uint32_t image;  // The first pixel's address.
  uint32_t x, y;
  uint32_t pitch;  // In pixels.
};

// CopyRect: `height` rows of `width` pixels copied, row by row.
void CopyRect(Guest& g, const PixelRect& to, const PixelRect& from, uint32_t width,
              int32_t height) {
  uint32_t target = to.image + (to.pitch * to.y + to.x) * 4;
  uint32_t source = from.image + (from.pitch * from.y + from.x) * 4;
  for (int32_t row = 0; row < height; row++) {
    Memcpy(g, target, source, width * 4);
    target += to.pitch * 4;
    source += from.pitch * 4;
  }
}

// --- The vinyl sheet's software rasterizer ---

// The PowerPC's float-to-integer conversions as the recompiled code does
// them (fctiwz, fctidz: truncation; NaN and out-of-range values saturate
// as the generated helpers saturate them).
int32_t TruncateToInt32(double value) {
  if (std::isnan(value) || !(value > -2147483649.0)) {
    return INT32_MIN;
  }
  if (value >= double(INT32_MAX)) {
    return INT32_MAX;
  }
  return int32_t(value);
}

int64_t TruncateToInt64(double value) {
  constexpr double kTwo63 = 9223372036854775808.0;
  if (std::isnan(value)) {
    return INT64_MIN;
  }
  if (value > kTwo63) {
    return INT64_MAX;
  }
  if (value >= kTwo63 || value < -kTwo63) {
    return INT64_MIN;
  }
  return int64_t(value);
}

// An image the rasterizer reads: its pixels (32-bit ARGB) and size; the
// pitch in bytes.
struct RasterImage {
  u32_be pixels;
  i32_be width;
  i32_be height;
  i32_be pitch;
};
GUEST_SIZE(RasterImage, 16);

// Where the rasterizer draws: the canvas' width, height and pitch (bytes).
struct RasterRegion {
  i32_be width;
  i32_be height;
  i32_be pitch;
};

// Two ARGB pixels mixed by `weight` out of 256 (a's share 256 - weight),
// two channels at a time: red and blue each product shifted down first,
// alpha and green together (so each can come out one lower).
uint32_t MixPixels(uint32_t a, uint32_t b, uint32_t weight) {
  constexpr uint32_t kRedBlue = 0x00FF00FF;
  const uint32_t keep = 256 - weight;
  const uint32_t red_blue = (((a & kRedBlue) * keep) >> 8 & 0x00FFFFFF) +
                            (((b & kRedBlue) * weight) >> 8 & 0x00FFFFFF);
  const uint32_t alpha_green = ((a >> 8) & kRedBlue) * keep + ((b >> 8) & kRedBlue) * weight;
  return (alpha_green & 0xFF00FF00) | (red_blue & kRedBlue);
}

// SoftRaster::SampleTexel: the image's texel at (u, v), bilinear: the
// four pixels around it (a negative index clamps both to 0, one past the
// end both to the last) mixed by the fractions in 256ths, along x then y.
uint32_t SampleTexel(const RasterImage& image, double u, double v) {
  constexpr float kWeights = 256.0f;  // 0x82001F84 (read-only)
  int32_t x0 = TruncateToInt32(u), y0 = TruncateToInt32(v);
  const float fx = float(u - double(float(double(x0))));
  const float fy = float(v - double(float(double(y0))));
  int32_t x1 = int32_t(uint32_t(x0) + 1), y1 = int32_t(uint32_t(y0) + 1);
  if (x0 < 0) {
    x0 = x1 = 0;
  } else if (!(image.width > x1)) {
    x0 = x1 = image.width - 1;
  }
  if (y0 < 0) {
    y0 = y1 = 0;
  } else if (!(image.height > y1)) {
    y0 = y1 = image.height - 1;
  }
  const uint32_t wx = uint32_t(TruncateToInt64(double(fx * kWeights)));
  const uint32_t wy = uint32_t(TruncateToInt64(double(fy * kWeights)));
  const uint32_t row0 = uint32_t(image.pitch) * uint32_t(y0) + image.pixels;
  const uint32_t row1 = uint32_t(image.pitch) * uint32_t(y1) + image.pixels;
  auto pixel = [](uint32_t row, int32_t x) { return uint32_t(*At<u32_be>(row + uint32_t(x) * 4)); };
  const uint32_t top = MixPixels(pixel(row0, x0), pixel(row0, x1), wx);
  const uint32_t bottom = MixPixels(pixel(row1, x0), pixel(row1, x1), wx);
  return MixPixels(top, bottom, wy);
}

// SoftRaster::BlendPixel: the texel at the given texel coordinates
// blended over the canvas pixel (x, y) by its alpha (skipped at alpha 0).
// Returns the texel.
uint32_t BlendPixel(const RasterImage& image, uint32_t canvas, const RasterRegion& region,
                    int32_t x, int32_t y, const guest::f32_be* texel) {
  u32_be& target = *At<u32_be>(uint32_t(region.pitch) * uint32_t(y) + canvas + uint32_t(x) * 4);
  const uint32_t below = target;
  const uint32_t source = SampleTexel(image, double(float(texel[0])), double(float(texel[1])));
  const uint32_t alpha = source >> 24;
  if (alpha != 0) {
    target = MixPixels(below, source, alpha);
  }
  return source;
}

// A point on the decal or sheet (x, y floats).
struct DecalPoint {
  guest::f32_be x, y;
};

// Vinyl::DecalCorner: a decal's corner moved onto the sheet: p' = R(a)
// ((p - c) s) + c + t, with c the decal's center, s its scale and t the
// offset; sin and cos through the game's math library, each step rounded
// to a float, the two products and sums fused as the original's.
void DecalCorner(Guest& g, DecalPoint* out, const DecalPoint& p, const DecalPoint& center,
                 const DecalPoint& scale, const DecalPoint& offset, double angle) {
  const double sx = double(float(double(float(double(float(p.x)) - double(float(center.x)))) *
                                 double(float(scale.x))));
  const double sy = double(float(double(float(double(float(p.y)) - double(float(center.y)))) *
                                 double(float(scale.y))));
  g.ctx.f1.f64 = angle;
  g.Call(sub_8258E140);  // sin
  const double sine = double(float(g.ctx.f1.f64));
  g.ctx.f1.f64 = angle;
  g.Call(sub_8258E218);  // cos
  const double cosine = double(float(g.ctx.f1.f64));
  const double sin_y = double(float(sine * sy));
  const double sin_x = double(float(sine * sx));
  const double turned_x = double(float(std::fma(cosine, sx, -sin_y)));
  const double turned_y = double(float(std::fma(cosine, sy, sin_x)));
  out->x = float(double(float(turned_x + double(float(center.x)))) + double(float(offset.x)));
  out->y = float(double(float(turned_y + double(float(offset.y)))) + double(float(center.y)));
}

// a + d t in two rounded steps (kept as two statements so the compiler
// can't fuse them into one multiply-add, as the original doesn't).
float Along(float a, float d, float t) {
  const float step = d * t;
  return step + a;
}

// A texel position the rasterizer carries (u, v; then two words it copies
// along but doesn't read).
struct RasterTexel {
  guest::f32_be u, v;
  u32_be unknown_8[2];
};

// The rasterizer's flags: the top bit moves the texels by -0.5 (to the
// texels' centers).
constexpr uint32_t kTexelCenters = 0x80000000;

// SoftRaster::DrawSpan: one row of a triangle, from x = xa to xb (either
// order; the right end left out), the texels interpolated between the
// ends' and clipped to the region, each pixel blended in (BlendPixel).
void DrawSpan(uint32_t canvas, const RasterRegion& region, int32_t xa, int32_t xb, int32_t row,
              const RasterTexel& texel_a, const RasterTexel& texel_b, const RasterImage& image,
              uint32_t flags) {
  if (row < 0 || !(region.height > row) || xa == xb) {
    return;
  }
  int32_t left = xa, right = xb;
  float u0 = texel_a.u, v0 = texel_a.v, u1 = texel_b.u, v1 = texel_b.v;
  if (!(xa < xb)) {
    left = xb;
    right = xa;
    u0 = texel_b.u, v0 = texel_b.v, u1 = texel_a.u, v1 = texel_a.v;
  }
  if (flags & kTexelCenters) {
    constexpr float kHalf = 0.5f;  // 0x82001BDC (read-only)
    u0 = u0 - kHalf;
    v0 = v0 - kHalf;
    u1 = u1 - kHalf;
    v1 = v1 - kHalf;
  }
  if (left < 0) {  // Clipped at the left: the texels moved along.
    const float t = float(-left) / float(right - left);
    u0 = Along(u0, u1 - u0, t);
    v0 = Along(v0, v1 - v0, t);
    left = 0;
  }
  if (!(region.width > right)) {  // And at the right.
    const float t = float(right - region.width) / float(right - left);
    u1 = Along(u1, u0 - u1, t);
    v1 = Along(v1, v0 - v1, t);
    right = region.width;
  }
  if (!(left < right)) {
    return;
  }
  const float du = u1 - u0, dv = v1 - v0;
  const float n = float(right - left);
  for (int32_t x = left, i = 0; x < right; x++, i++) {
    const float t = float(i) / n;
    guest::f32_be texel[2];
    texel[0] = Along(u0, du, t);
    texel[1] = Along(v0, dv, t);
    BlendPixel(image, canvas, region, x, row, texel);
  }
}

// a x + b in one rounding (the original's fmadds).
float FusedAlong(float a, float x, float b) {
  return float(std::fma(double(a), double(x), double(b)));
}

// A triangle's vertex position on the sheet (x, y; then two words not read).
struct RasterVertex {
  guest::f32_be x, y;
  u32_be unknown_8[2];
};

// SoftRaster::DrawTriangle: a textured triangle filled row by row. The
// vertices sorted by y (ties keep their order); rows from int(y + 0.5)
// of the top to the bottom; the long edge's point at the middle row; the
// top half's rows between the edges top-middle and top-split, the bottom
// half's between middle-bottom and split-bottom. Each row's ends are the
// edges at the row's top (i / n), their texels at the row's middle
// ((i + 0.5) / n) corrected to the end pixels' centers; then a span
// (DrawSpan, with the texel-centers flag) into the canvas.
void DrawTriangle(uint32_t canvas, const RasterRegion& region, const RasterVertex* positions,
                  const RasterTexel* texels, const RasterImage& image, uint32_t flags) {
  constexpr float kHalf = 0.5f;  // 0x82001BDC (read-only)
  int top = 0, mid = 1, bottom = 2;
  if (!(positions[0].y <= positions[1].y)) {
    top = 1;
    mid = 0;
  }
  if (!(positions[top].y <= positions[2].y)) {
    bottom = top;
    top = 2;
  }
  if (!(positions[mid].y <= positions[bottom].y)) {
    std::swap(mid, bottom);
  }
  const RasterVertex& p_top = positions[top];
  const RasterVertex& p_mid = positions[mid];
  const RasterVertex& p_bottom = positions[bottom];
  const RasterTexel& t_top = texels[top];
  const RasterTexel& t_mid = texels[mid];
  const RasterTexel& t_bottom = texels[bottom];
  const int32_t row_top = TruncateToInt32(double(float(p_top.y) + kHalf));
  const int32_t row_mid = TruncateToInt32(double(float(p_mid.y) + kHalf));
  const int32_t row_bottom = TruncateToInt32(double(float(p_bottom.y) + kHalf));
  if (row_top == row_bottom) {
    return;
  }
  const uint32_t span_flags = (flags & 0x1FFFFFFF) | kTexelCenters;

  // The long edge's point at the middle row.
  const float x_top = p_top.x, x_mid = p_mid.x, x_bottom = p_bottom.x;
  const float u_top = t_top.u, v_top = t_top.v;
  const float u_mid = t_mid.u, v_mid = t_mid.v;
  const float u_bottom = t_bottom.u, v_bottom = t_bottom.v;
  const float upper_rows = float(row_mid - row_top);
  const float split = upper_rows / float(row_bottom - row_top);
  const float x_split = Along(x_top, x_bottom - x_top, split);
  const float u_split = Along(u_top, u_bottom - u_top, split);
  const float v_split = Along(v_top, v_bottom - v_top, split);

  // One row: the edges' x at the row's top (a, b), their x and texels at
  // its middle, the texels moved to the end pixels' centers.
  struct Edge {
    float x0, dx;  // x = x0 + dx t
    float u0, du, v0, dv;
  };
  auto draw_row = [&](int32_t row, float i, float rows, const Edge& a, const Edge& b) {
    const float t = i / rows;
    const float t_mid_row = (i + kHalf) / rows;
    const int32_t xa = TruncateToInt32(double(FusedAlong(a.dx, t, a.x0)));
    const int32_t xb = TruncateToInt32(double(FusedAlong(b.dx, t, b.x0)));
    const float xa_mid = FusedAlong(a.dx, t_mid_row, a.x0);
    const float xb_mid = FusedAlong(b.dx, t_mid_row, b.x0);
    // The texels along each edge.
    const float ua = Along(a.u0, a.du, t_mid_row), va = Along(a.v0, a.dv, t_mid_row);
    const float ub = Along(b.u0, b.du, t_mid_row), vb = Along(b.v0, b.dv, t_mid_row);
    const float a_to_b = xb_mid - xa_mid, b_to_a = xa_mid - xb_mid;
    const float ka = ((float(xa) - xa_mid) + kHalf) / a_to_b;
    const float kb = ((float(xb) - xb_mid) + kHalf) / b_to_a;
    RasterTexel texel_a{}, texel_b{};
    texel_a.u = Along(ua, ub - ua, ka);
    texel_a.v = Along(va, vb - va, ka);
    texel_b.u = Along(ub, ua - ub, kb);
    texel_b.v = Along(vb, va - vb, kb);
    DrawSpan(canvas, region, xa, xb, row, texel_a, texel_b, image, span_flags);
  };

  if (row_top < row_mid) {
    const Edge a{x_top, x_mid - x_top, u_top, u_mid - u_top, v_top, v_mid - v_top};
    const Edge b{x_top, x_split - x_top, u_top, u_split - u_top, v_top, v_split - v_top};
    for (int32_t row = row_top, i = 0; row < row_mid; row++, i++) {
      draw_row(row, float(i), upper_rows, a, b);
    }
  }
  if (row_mid < row_bottom) {
    const float lower_rows = float(row_bottom - row_mid);
    const Edge a{x_mid, x_bottom - x_mid, u_mid, u_bottom - u_mid, v_mid, v_bottom - v_mid};
    const Edge b{x_split, x_bottom - x_split, u_split, u_bottom - u_split, v_split,
                 v_bottom - v_split};
    for (int32_t row = row_mid, i = 0; row < row_bottom; row++, i++) {
      draw_row(row, float(i), lower_rows, a, b);
    }
  }
}

// The sheet's regions (writable, 0x825F2A00): left, top, right, bottom;
// region 9 is the whole sheet.
struct DecalRegion {
  i32_be left, top, right, bottom;
};
GUEST_SIZE(DecalRegion, 16);
constexpr uint32_t kDecalRegions = 0x825F2A00;
constexpr uint32_t kDecalRegionCount = 10;

// A decal's arguments past the registers, in its caller's frame.
struct DrawDecalArguments {
  uint8_t unknown_0[96];
  DecalPoint scale;  // +96
  uint8_t unknown_104[12];
  u32_be pixels;  // +116
  uint8_t unknown_120[12];
  u32_be sheet;  // +132: the sheet's data.
};
GUEST_OFFSET(DrawDecalArguments, pixels, 116);
GUEST_OFFSET(DrawDecalArguments, sheet, 132);

// Vinyl::DrawDecal: a decal (its TGA's size of ARGB pixels) drawn into one
// of the sheet's regions: a quad the decal's size, each corner scaled,
// turned by `angle` and moved (DecalCorner: about the decal's center, to
// `offset` less the center plus half the region), its texels the decal's
// corners (left and right swapped when mirrored); drawn as two triangles
// clipped to the region.
void DrawDecal(Guest& g, uint32_t region_index, bool mirrored, const TGAHeader& header,
               const DecalPoint& place, double angle, const DrawDecalArguments& arguments) {
  constexpr float kHalf = 0.5f;  // 0x82001BDC (read-only)
  const float zero = *At<guest::f32_be>(0x82000A24);  // 0.0 (read-only)
  const DecalRegion region = *guest::Element<DecalRegion>(kDecalRegions, region_index);
  const int32_t width = uint16_t(header.width), height = uint16_t(header.height);
  const int32_t region_width = region.right - region.left;
  const int32_t region_height = region.bottom - region.top;
  const float w = float(width), h = float(height);

  // The quad's corners, turned and moved about the decal's center.
  const DecalPoint scale = arguments.scale;
  RasterVertex corners[4] = {};
  corners[0].x = zero;
  corners[0].y = zero;
  corners[1].x = w;
  corners[1].y = zero;
  corners[2].x = zero;
  corners[2].y = h;
  corners[3].x = w;
  corners[3].y = h;
  DecalPoint center, offset;
  center.x = w * kHalf;
  center.y = h * kHalf;
  offset.x = FusedAlong(float(region_width), kHalf, float(place.x) - float(center.x));
  offset.y = FusedAlong(float(region_height), kHalf, float(place.y) - float(center.y));
  for (RasterVertex& corner : corners) {
    DecalPoint* point = reinterpret_cast<DecalPoint*>(&corner);
    DecalCorner(g, point, *point, center, scale, offset, angle);
  }

  // The texels: the decal's corners, left and right swapped when mirrored.
  const float u[4] = {zero, w, zero, w};
  const float v[4] = {zero, zero, h, h};
  RasterTexel texels[4] = {};
  for (int k = 0; k < 4; k++) {
    const int from = mirrored ? (k ^ 1) : k;
    texels[k].u = u[from];
    texels[k].v = v[from];
  }

  // The region of the sheet drawn into, and the decal's pixels.
  RasterRegion target;
  target.width = region_width;
  target.height = region_height;
  const uint32_t sheet_pitch = Sheet().width * 4;
  target.pitch = int32_t(sheet_pitch);
  RasterImage image;
  image.width = width;
  image.height = height;
  image.pitch = width * 4;
  image.pixels = arguments.pixels;
  const uint32_t canvas =
      sheet_pitch * uint32_t(region.top) + uint32_t(region.left) * 4 + arguments.sheet;
  for (int k = 0; k < 2; k++) {
    DrawTriangle(canvas, target, &corners[k], &texels[k], image, 0);
  }
  // (The header's destructor does nothing.)
}

// Vinyl::DrawDecal for the plate image (see above): region 8, not
// mirrored, no turn, the image placed at the offsets.
void DrawSheetDecal(Guest& g, const TGAHeader& image, const game::Vector& offsets,
                    uint32_t frame) {
  constexpr uint32_t kSheetRegion = 8;
  const float zero = *At<guest::f32_be>(0x82000A24);  // 0.0 (read-only)
  DecalPoint place;
  place.x = offsets.x;
  place.y = offsets.y;
  g.ctx.fpscr.disableFlushMode();  // Float math as the recompiled code's.
  DrawDecal(g, kSheetRegion, false, image, place, double(zero), *At<DrawDecalArguments>(frame));
}

// Whether every arrived decal's region is one of the sheet's ten (a
// region past 9 leaves DrawDecal's rectangle unset in the original).
bool DecalRegionsKnown(const CarModel* model) {
  for (int k = 0; k < CarBodyLoads::kLayers; k++) {
    if (model->loads.layers[k].load.state > 2 && k < 3 &&
        uint32_t(model->record.layers[0].kind) >= kDecalRegionCount) {
      return false;
    }
  }
  for (int j = 0; j < CarBodyLoads::kStickers; j++) {
    const game::RecordLayer& record = model->record.layers[6 * kStickerGroups[j / 6] + j % 6];
    if (model->loads.stickers[j].load.state > 2 && uint32_t(record.kind) >= kDecalRegionCount) {
      return false;
    }
  }
  return true;
}

// Vinyl::DrawDecals' frame (368 bytes): Vinyl::DrawDecal's arguments past
// the registers (+96 the scale, +116 the pixels, +124, +132 and +140 the
// sheet's words), the decoded decal's pixels and TGA header.
struct DrawDecalsFrame {
  uint8_t unknown_0[96];
  game::Vector scale;
  uint8_t unknown_112[4];
  u32_be pixels;  // +116
  uint8_t unknown_120[4];
  u32_be unknown_124;
  uint8_t unknown_128[4];
  u32_be sheet_data;  // +132
  uint8_t unknown_136[4];
  u32_be sheet_data_b;  // +140
  u32_be decal_pixels;  // +144: TGA::CreateImage's.
  uint8_t unknown_148[44];
  TGAHeader header;   // +192
  uint8_t unknown_212[44];
  TGAHeader unknown_256;  // Cleared with the header, not used.
  uint8_t unknown_276[92];
};
GUEST_OFFSET(DrawDecalsFrame, scale, 96);
GUEST_OFFSET(DrawDecalsFrame, decal_pixels, 144);
GUEST_OFFSET(DrawDecalsFrame, header, 192);
GUEST_OFFSET(DrawDecalsFrame, unknown_256, 256);
GUEST_SIZE(DrawDecalsFrame, 368);

// Vinyl::DrawDecals: each arrived decal (state not 0, 1 or 2) decoded
// (TGA::CreateImage), its file released and drawn into the sheet
// (Vinyl::DrawDecal), then its pixels freed. A 1,024 x 1,024 layer is the
// whole sheet (region 9, scale 1); layers 0-2 go into the region of layer
// 0's kind, layers 3-5 into regions 1 and 2 both; a sticker into its
// record's region, mirrored by its record. Returns 1.
void DrawDecals(Guest& g, CarModel* model) {
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(DrawDecalsFrame)));
  DrawDecalsFrame* frame = At<DrawDecalsFrame>(stack.Local(0));
  const float zero = *At<guest::f32_be>(0x82000A24);  // 0.0 (read-only)
  const float one = *At<guest::f32_be>(0x82000A20);   // 1.0 (read-only)
  const DrawDecalArguments& arguments = *At<DrawDecalArguments>(stack.Local(0));
  auto arrived = [](const DecalSlot& decal) { return decal.load.state > 2; };

  // The decal decoded and its file released; then DrawDecal's arguments.
  auto decode = [&](DecalSlot& decal) {
    CreateImage(g, &frame->header, decal.load.unknown_8, decal.load.block, &frame->decal_pixels);
    ReleaseBuffer(g, &decal.load);
  };
  auto set_arguments = [&](const game::Vector& scale) {
    frame->unknown_124 = 0;
    frame->sheet_data_b = model->loads.sheet.data_b;
    frame->sheet_data = model->loads.sheet.data;
    frame->pixels = frame->decal_pixels;
    frame->scale = scale;
  };
  auto place_of = [](const game::Vector& offset) {
    DecalPoint place;
    place.x = offset.x;
    place.y = offset.y;
    return place;
  };

  for (int k = 0; k < CarBodyLoads::kLayers; k++) {
    frame->decal_pixels = 0;
    ClearHeader(&frame->header);
    ClearHeader(&frame->unknown_256);
    DecalSlot& decal = model->loads.layers[k];
    const game::RecordLayer& record = model->record.layers[k];
    if (!arrived(decal)) {
      continue;
    }
    decode(decal);
    const DecalPoint place = place_of(record.unknown_16);
    const double angle = double(float(record.unknown_32));
    if (uint16_t(frame->header.width) == 1024 && uint16_t(frame->header.height) == 1024) {
      constexpr uint32_t kWholeSheet = 9;
      set_arguments(game::Vector{one, one, one, one});
      DecalPoint origin;
      origin.x = zero;
      origin.y = zero;
      DrawDecal(g, kWholeSheet, false, frame->header, origin, double(zero), arguments);
    } else if (k < 3) {
      set_arguments(record.unknown_48);
      DrawDecal(g, uint32_t(model->record.layers[0].kind), false, frame->header, place, angle,
                arguments);
    } else {
      for (uint32_t region : {1u, 2u}) {
        set_arguments(record.unknown_48);
        DrawDecal(g, region, false, frame->header, place, angle, arguments);
      }
    }
    VirtualFree(g, frame->decal_pixels);
  }

  for (int j = 0; j < CarBodyLoads::kStickers; j++) {
    frame->decal_pixels = 0;
    ClearHeader(&frame->header);
    ClearHeader(&frame->unknown_256);
    DecalSlot& decal = model->loads.stickers[j];
    if (!arrived(decal)) {
      continue;
    }
    decode(decal);
    const game::RecordLayer& record = model->record.layers[6 * kStickerGroups[j / 6] + j % 6];
    set_arguments(record.unknown_48);
    DrawDecal(g, record.kind, record.unknown_4 != 0, frame->header, place_of(record.unknown_16),
              double(float(record.unknown_32)), arguments);
    VirtualFree(g, frame->decal_pixels);
  }
}

// Vinyl::DrawDecals as its callers get it: a decal whose region (a
// record's kind, or layer 0's) is past 9 runs the original.
void VinylDrawDecals(Guest& g, CarModel* model) {
  if (!DecalRegionsKnown(model)) {
    g.Call(__imp__sub_820D3D18, model);
    return;
  }
  g.ctx.fpscr.disableFlushMode();  // Float math as the recompiled code's.
  DrawDecals(g, model);
}

// Vinyl::Compress's frame (272 bytes): XGCompressSurface's arguments past
// r10, the new texture's words, the two surfaces' locked rectangles and
// descriptions.
struct CompressFrame {
  struct LockedRect {
    u32_be pitch;
    u32_be bits;
  };
  // D3DSURFACE_DESC (32 bytes; its width and height at +24, +28).
  struct SurfaceDesc {
    u32_be unknown_0[6];
    u32_be width;
    u32_be height;
  };
  uint8_t unknown_0[80];
  u32_be unknown_80;
  u32_be source_format;  // +84
  u32_be unknown_88;
  u32_be source_rect;  // +92: none.
  u32_be unknown_96;
  u32_be flags;  // +100
  uint8_t unknown_104[8];
  u32_be texture;  // +112
  u32_be data;
  LockedRect sheet;    // +120
  LockedRect locked;  // +128
  uint8_t unknown_136[8];
  SurfaceDesc desc;        // +144
  SurfaceDesc sheet_desc;  // +176
  uint8_t unknown_208[64];
};
GUEST_OFFSET(CompressFrame, source_format, 84);
GUEST_OFFSET(CompressFrame, texture, 112);
GUEST_OFFSET(CompressFrame, desc, 144);
GUEST_SIZE(CompressFrame, 272);

// Vinyl::Compress: a DXT5 texture the sheet's size made, the sheet
// (A8R8G8B8) compressed into it (XGCompressSurface: no rect, flags 1,
// alpha reference 0.0), both surfaces unlocked and released, the old
// sheet freed (Texture::Free) and the new texture put in its place.
// Returns Texture::Free's r3.
uint32_t CompressSheet(Guest& g, uint32_t width, uint32_t height, u32_be* header_at,
                       u32_be* data_at) {
  constexpr uint32_t kDxt5 = 0x1A200054;
  constexpr uint32_t kLockReadOnly = 0x10;
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(CompressFrame)));
  CompressFrame* frame = At<CompressFrame>(stack.Local(0));
  frame->texture = 0;
  frame->data = 0;
  frame->sheet = {};
  frame->locked = {};
  CreateTexture(g, kDxt5, width, height, &frame->texture, &frame->data);
  const uint32_t texture = frame->texture;
  const uint32_t surface = g.Call(sub_822A3168, texture, 0u);  // Texture::GetSurfaceLevel
  g.Call(sub_822A35D0, surface, &frame->locked, 0u, 0u);   // Surface::LockRect
  const uint32_t sheet = g.Call(sub_822A3168, uint32_t(*header_at), 0u);
  g.Call(sub_822A35D0, sheet, &frame->sheet, 0u, kLockReadOnly);
  frame->desc = {};
  g.Call(sub_822A3538, surface, &frame->desc);  // Surface::GetDesc
  frame->sheet_desc = {};
  g.Call(sub_822A3538, sheet, &frame->sheet_desc);
  frame->unknown_80 = 0;
  frame->source_format = kSheetFormat;
  frame->unknown_88 = 0;
  frame->source_rect = 0;
  frame->unknown_96 = 0;
  frame->flags = 1;
  g.ctx.f1.f64 = double(*At<guest::f32_be>(0x82000A24));  // 0.0 (read-only)
  g.Call(sub_82354BF8, uint32_t(frame->locked.bits), uint32_t(frame->locked.pitch),
         uint32_t(frame->desc.width), uint32_t(frame->desc.height), kDxt5, 0u,
         uint32_t(frame->sheet.bits), uint32_t(frame->sheet.pitch));  // XGCompressSurface
  g.Call(sub_822A19E0, surface);  // Surface::UnlockRect
  g.Call(sub_822A19E0, sheet);
  g.Call(sub_822A3DB8, surface);  // Resource::Release
  g.Call(sub_822A3DB8, sheet);
  const uint32_t result = FreeTexture(g, header_at, data_at);
  *header_at = texture;
  *data_at = frame->data;
  return result;
}

// --- The license plate ---

// The plate's slots: where each goes on the plate (x, y; the width and
// height are worked out from the images). Writable: 0x825F2AA0.
struct PlateSlot {
  i32_be x, y, width, height;
};
GUEST_SIZE(PlateSlot, 16);
constexpr uint32_t kPlateSlots = 0x825F2AA0;
constexpr int kPlateSlotCount = 11;

// Slot 1's character to its cell in the image: six (bound, offset) pairs
// (read-only, 0x8200662C).
constexpr uint32_t kKanaCells = 0x8200662C;

// The images' kinds (PlateImage's): the background, the separator and
// digits, the small characters, the kana, the plate itself.
enum PlateKind : uint32_t {
  kPlateRegion = 0, kPlateDigits = 2, kPlateSmall = 4, kPlateKana = 3, kPlateBase = 1,
};

// CarModel::MakePlate's frame: the five images' TGA headers and the
// surface's locked rectangle, where the callees write them.
struct MakePlateFrame {
  uint8_t unknown_0[128];
  TGAHeader base;     // +128
  uint8_t unknown_148[12];
  TGAHeader digits;   // +160
  uint8_t unknown_180[12];
  TGAHeader region;   // +192
  uint8_t unknown_212[12];
  TGAHeader small;    // +224
  uint8_t unknown_244[12];
  TGAHeader kana;     // +256
  uint8_t unknown_276[188];
  u32_be locked_pitch;  // +464 (D3DLOCKED_RECT)
  u32_be locked_bits;
  uint8_t unknown_472[696];
};
GUEST_OFFSET(MakePlateFrame, base, 128);
GUEST_OFFSET(MakePlateFrame, region, 192);
GUEST_OFFSET(MakePlateFrame, kana, 256);
GUEST_OFFSET(MakePlateFrame, locked_pitch, 464);
GUEST_SIZE(MakePlateFrame, 1168);

// Slot 1's cell for a character: by the first bound at or above it, the
// previous pair's offset plus it (itself below the first); past them all,
// it plus 15.
int32_t KanaCell(int32_t kana) {
  struct Pair {
    i32_be bound, offset;
  };
  const Pair* pairs = At<Pair>(kKanaCells);
  for (int k = 0; k < 6; k++) {
    if (pairs[k].bound >= kana) {
      return k == 0 ? kana : pairs[k - 1].offset + kana;
    }
  }
  return kana + 15;
}

// CarModel::MakePlate: two 32-bit textures the base plate's size (A and B,
// headers and data into the four outs), the base plate copied into both,
// then each slot's cell of its image copied in (slot 0, the region's mark,
// into B only).
void MakePlate(Guest& g, u32_be* header_a, u32_be* data_a, u32_be* header_b, u32_be* data_b,
               const PlateWords& words) {
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(MakePlateFrame)));
  MakePlateFrame* frame = At<MakePlateFrame>(stack.Local(0));
  CarSceneState* scene = &SceneLoads();
  ClearHeader(&frame->region);
  ClearHeader(&frame->base);
  ClearHeader(&frame->digits);
  ClearHeader(&frame->small);
  ClearHeader(&frame->kana);
  const uint32_t set = uint32_t(words.image_set);
  u32_be region_pixels, digits_pixels, small_pixels, kana_pixels, base_pixels;
  PlateImage(g, scene, kPlateRegion, set, AddressOf(&frame->region), &region_pixels);
  PlateImage(g, scene, kPlateDigits, set, AddressOf(&frame->digits), &digits_pixels);
  PlateImage(g, scene, kPlateSmall, set, AddressOf(&frame->small), &small_pixels);
  PlateImage(g, scene, kPlateKana, set, AddressOf(&frame->kana), &kana_pixels);
  PlateImage(g, scene, kPlateBase, words.image_set >= 2 ? set - 1 : 0, AddressOf(&frame->base),
             &base_pixels);

  // The slots' sizes, from the images.
  struct Rect {
    int32_t x, y, width, height;
  };
  Rect slots[kPlateSlotCount];
  const PlateSlot* table = At<PlateSlot>(kPlateSlots);
  for (int i = 0; i < kPlateSlotCount; i++) {
    slots[i] = {table[i].x, table[i].y, table[i].width, table[i].height};
    if (i == 0 || i == 5) {
      slots[i].width = int32_t(uint16_t(frame->region.width)) / 6;
      slots[i].height = int32_t(uint16_t(frame->region.height)) / 6;
    } else if (i == 1) {
      slots[i].width = int32_t(uint16_t(frame->kana.width)) / 12;
      slots[i].height = int32_t(uint16_t(frame->kana.height)) / 10;
    } else if (i < 5) {
      slots[i].width = int32_t(uint16_t(frame->small.width)) / 10;
      slots[i].height = uint16_t(frame->small.height);
    } else {
      slots[i].width = 48;
      slots[i].height = uint16_t(frame->digits.height);
    }
  }

  // The two textures, and the pitch of A's top level.
  const uint32_t width = uint16_t(frame->base.width), height = uint16_t(frame->base.height);
  CreateTexture(g, kSheetFormat, width, height, header_a, data_a);
  CreateTexture(g, kSheetFormat, width, height, header_b, data_b);
  const uint32_t surface = g.Call(sub_822A3168, uint32_t(*header_a), 0u);  // Texture::GetSurfaceLevel
  g.Call(sub_822A35D0, surface, &frame->locked_pitch, 0u, 0u);  // Surface::LockRect
  const uint32_t pitch = frame->locked_pitch;
  g.Call(sub_822A19E0, surface);  // Surface::UnlockRect
  g.Call(sub_822A3DB8, surface);  // Resource::Release

  // The base plate into both.
  uint32_t row_a = *data_a, row_b = *data_b, source = base_pixels;
  for (int32_t y = 0; y < int32_t(height); y++) {
    Memcpy(g, row_a, source, width * 4);
    Memcpy(g, row_b, source, width * 4);
    row_a += pitch & ~3u;
    row_b += pitch & ~3u;
    source += width * 4;
  }

  // A cell of an image into a slot of A and B (or B only).
  const uint32_t plate_pitch = pitch / 4;
  auto copy = [&](const Rect& slot, int32_t w, int32_t h, uint32_t pixels,
                  const TGAHeader& image, int32_t source_x, int32_t source_y, bool both) {
    const PixelRect from{pixels, uint32_t(source_x), uint32_t(source_y),
                         uint32_t(uint16_t(image.width))};
    if (both) {
      CopyRect(g, PixelRect{*data_a, uint32_t(slot.x), uint32_t(slot.y), plate_pitch}, from,
               uint32_t(w), h);
    }
    CopyRect(g, PixelRect{*data_b, uint32_t(slot.x), uint32_t(slot.y), plate_pitch}, from,
             uint32_t(w), h);
  };
  constexpr int32_t kNone = 12;
  for (int i = 0; i < kPlateSlotCount; i++) {
    const Rect& slot = slots[i];
    switch (i) {
      case 0:  // The region's mark, its sixth row.
        copy(slot, 30, 33, region_pixels, frame->region, 0, 5 * slot.height, false);
        break;
      case 1: {
        const int32_t cell = KanaCell(words.kana);
        copy(slot, slot.width, slot.height, kana_pixels, frame->kana, cell % 12 * slot.width,
             cell / 12 * slot.height, true);
        break;
      }
      case 2:
      case 3:
      case 4: {
        const int32_t pick = words.small[i - 2];
        if (pick != kNone) {
          copy(slot, slot.width, slot.height, small_pixels, frame->small, pick * slot.width, 0,
               true);
        }
        break;
      }
      case 5:
        copy(slot, slot.width, slot.height, region_pixels, frame->region,
             words.region % 6 * slot.width, words.region / 6 * slot.height, true);
        break;
      case 8:
        if (words.separator != kNone) {
          copy(slot, 25, slot.height, digits_pixels, frame->digits, 528, 0, true);
        }
        break;
      default: {
        const int32_t digit = i < 8 ? words.digits_a[i - 6] : words.digits_b[i - 9];
        copy(slot, 48, slot.height, digits_pixels, frame->digits, digit * 48, 0, true);
        break;
      }
    }
  }
  // (The headers' destructors do nothing.)
}

// CarModel::SetVinylTextures: a cockpit's six part models given its car
// model's sheet (Model::SetVinylTextures), the car model read again for
// each.
void SetCockpitVinylTextures(Guest& g, CockpitObject* cockpit) {
  for (game::PartModel& part : cockpit->loads.parts) {
    const CarModel* model = cockpit->head.car_model;
    g.Call(sub_820CE7B8, &part, uint32_t(model->loads.sheet.header),
           uint32_t(model->loads.sheet.header_b));
  }
}

}  // namespace

// --- What the group's other files call ---

namespace game {

void LoadSlotReset(LoadSlot* slot) { ResetSlot(slot); }

uint32_t LoadSlotLoad(Guest& g, LoadSlot* slot, uint32_t id, uint32_t context, uint32_t callback) {
  return LoadAllocated(g, slot, id, context, callback);
}

uint32_t LoadSlotLoadShared(Guest& g, LoadSlot* slot, uint32_t id, uint32_t context,
                            uint32_t callback) {
  return LoadShared(g, slot, id, context, callback);
}

uint32_t LoadSlotReleaseBuffer(Guest& g, LoadSlot* slot) { return ReleaseBuffer(g, slot); }

void DeferRelease(ScenePoolSet* set, const TexturePair& textures) { QueueRelease(set, textures); }

void TextureCreate(Guest& g, uint32_t format, uint32_t width, uint32_t height, u32_be* header_out,
                   u32_be* data_out) {
  CreateTexture(g, format, width, height, header_out, data_out);
}

void CarModelRelease(Guest& g, CarModel* model) { Release(g, model); }

void CarModelReloadDecals(Guest& g, CarModel* model) { ReloadDecals(g, model); }

void CarModelMakePlate(Guest& g, u32_be* header_a, u32_be* data_a, u32_be* header_b,
                       u32_be* data_b, const PlateWords& words) {
  MakePlate(g, header_a, data_a, header_b, data_b, words);
}

void CockpitSetVinylTextures(Guest& g, CockpitObject* cockpit) {
  SetCockpitVinylTextures(g, cockpit);
}

// SceneWheels::SetUp: a record with its own caliper texture (+68) and no
// car model (where the original reads the block through address 0) runs
// the original.
void SceneWheelsSetUp(Guest& g, PolledWheels* wheels, uint32_t setup) {
  if (At<WheelsRecord>(setup)->unknown_68[0] != 0 && wheels->car_model == nullptr) {
    g.Call(__imp__sub_820E6CA8, wheels, setup);
    return;
  }
  SetUpSceneWheels(g, wheels, setup);
}

}  // namespace game

#define SBX_CAR_MODELS_FALLBACK(name)   \
  REX_FUNC_PROLOGUE();                  \
  if (!On()) {                          \
    __imp__##name(ctx, base);           \
    return;                             \
  }                                     \
  Guest g(ctx, base);

// --- Load slots ---

// LoadSlot::Reset: r3 the slot. Returns it.
extern "C" REX_FUNC(sub_820D04F0) {
  SBX_CAR_MODELS_FALLBACK(sub_820D04F0)
  ResetSlot(At<LoadSlot>(ctx.r3.u32));
}

// LoadSlot::Load: r3 the slot, r4 the asset id, r5 the context, r6 the
// callback. 1 when it's loading.
extern "C" REX_FUNC(sub_820D0588) {
  SBX_CAR_MODELS_FALLBACK(sub_820D0588)
  ctx.r3.u64 = LoadAllocated(g, At<LoadSlot>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// LoadSlot::LoadShared: as LoadSlot::Load, into the shared buffer.
extern "C" REX_FUNC(sub_820D0620) {
  SBX_CAR_MODELS_FALLBACK(sub_820D0620)
  ctx.r3.u64 = LoadShared(g, At<LoadSlot>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// LoadSlot::ReleaseBuffer: r3 the slot.
extern "C" REX_FUNC(sub_820D0518) {
  SBX_CAR_MODELS_FALLBACK(sub_820D0518)
  ctx.r3.u64 = ReleaseBuffer(g, At<LoadSlot>(ctx.r3.u32));
}

// DeferRelease: r3 the first pool set, r4 to r7 the four words. Returns r3.
extern "C" REX_FUNC(sub_820C3D60) {
  SBX_CAR_MODELS_FALLBACK(sub_820C3D60)
  TexturePair words;
  words.header = ctx.r4.u32;
  words.data = ctx.r5.u32;
  words.header_b = ctx.r6.u32;
  words.data_b = ctx.r7.u32;
  QueueRelease(At<ScenePoolSet>(ctx.r3.u32), words);
}

// FreePagesAt: r4 where a buffer's address is; the buffer freed. Returns
// VirtualFree's answer.
extern "C" REX_FUNC(sub_820C3AF0) {
  SBX_CAR_MODELS_FALLBACK(sub_820C3AF0)
  ctx.r3.u64 = FreePagesAt(g, At<u32_be>(ctx.r4.u32));
}

// --- The car model ---

// TeamEmblem: r4 the car's block (r3, r5 not read). The emblem's asset id
// or -1.
extern "C" REX_FUNC(sub_820D3998) {
  SBX_CAR_MODELS_FALLBACK(sub_820D3998)
  ctx.r3.s64 = TeamEmblem(ctx.r4.s32);
}

// CarModel::LoadSetA and LoadSetB: r3 the car model. Return the last
// callee's r3 (not read).
extern "C" REX_FUNC(sub_820D5680) {
  SBX_CAR_MODELS_FALLBACK(sub_820D5680)
  LoadSetA(g, At<CarModel>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820D5768) {
  SBX_CAR_MODELS_FALLBACK(sub_820D5768)
  LoadSetB(g, At<CarModel>(ctx.r3.u32));
}

// CarModel::LoadDecals: r3 the car model. Returns 1.
extern "C" REX_FUNC(sub_820D57F8) {
  SBX_CAR_MODELS_FALLBACK(sub_820D57F8)
  LoadDecals(g, At<CarModel>(ctx.r3.u32));
  ctx.r3.u64 = 1;
}

// CarModel::LoadExterior: r3 the car model, r4 its set-up (3,696 bytes).
// Returns 1.
extern "C" REX_FUNC(sub_820D5B70) {
  SBX_CAR_MODELS_FALLBACK(sub_820D5B70)
  LoadExterior(g, At<CarModel>(ctx.r3.u32), ctx.r4.u32);
  ctx.r3.u64 = 1;
}

// CarModel::ReloadDecals: r3 the car model. Returns 1.
extern "C" REX_FUNC(sub_820D59C8) {
  SBX_CAR_MODELS_FALLBACK(sub_820D59C8)
  ReloadDecals(g, At<CarModel>(ctx.r3.u32));
  ctx.r3.u64 = 1;
}

// CarModel::Release: r3 the car model.
extern "C" REX_FUNC(sub_820D41A8) {
  SBX_CAR_MODELS_FALLBACK(sub_820D41A8)
  Release(g, At<CarModel>(ctx.r3.u32));
}

// CarModel::SetVinylTextures: r3 a cockpit body (its car model at +24):
// its six part models given the car model's sheet (Model::SetVinylTextures),
// the car model read again for each.
extern "C" REX_FUNC(sub_820D5C30) {
  SBX_CAR_MODELS_FALLBACK(sub_820D5C30)
  game::CockpitSetVinylTextures(g, At<CockpitObject>(ctx.r3.u32));
}

// CarModel::OnDecalLoaded: Archive::Load's callback, r3 the request, r6 the
// car model.
extern "C" REX_FUNC(sub_820D53F8) {
  SBX_CAR_MODELS_FALLBACK(sub_820D53F8)
  ctx.r3.u64 = OnDecalLoaded(g, At<CarModel>(ctx.r6.u32), ctx.r3.u32);
}

// Vinyl::ClearCorner: r3 the car model.
extern "C" REX_FUNC(sub_820D3920) {
  SBX_CAR_MODELS_FALLBACK(sub_820D3920)
  ClearCorner(g, At<CarModel>(ctx.r3.u32));
}

// --- Textures ---

// Texture::Create: r3 the format, r4 and r5 the size, r6 and r7 where the
// header's and data's addresses go. Returns 1.
extern "C" REX_FUNC(sub_820ECB40) {
  SBX_CAR_MODELS_FALLBACK(sub_820ECB40)
  CreateTexture(g, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, At<u32_be>(ctx.r6.u32),
                At<u32_be>(ctx.r7.u32));
  ctx.r3.u64 = 1;
}

// Texture::Free: r3 and r4 where a texture's header and data addresses
// are; each freed and cleared when set. Returns XPhysicalFree's r3, or 0
// without data.
extern "C" REX_FUNC(sub_820ECC00) {
  SBX_CAR_MODELS_FALLBACK(sub_820ECC00)
  ctx.r3.u64 = FreeTexture(g, At<u32_be>(ctx.r3.u32), At<u32_be>(ctx.r4.u32));
}

// --- The interior, cockpit and wheels ---

// Interior::LoadParts: r3 the interior, r4 the base id, r5 the stride, r6
// the offset.
extern "C" REX_FUNC(sub_820D73E0) {
  SBX_CAR_MODELS_FALLBACK(sub_820D73E0)
  ctx.r3.u64 =
      LoadInteriorParts(g, At<InteriorObject>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32, ctx.r6.s32);
}

// Interior::Load: r3 the interior, r4 its set-up (120 bytes).
extern "C" REX_FUNC(sub_820D7660) {
  SBX_CAR_MODELS_FALLBACK(sub_820D7660)
  LoadInterior(g, At<InteriorObject>(ctx.r3.u32), ctx.r4.u32);
}

// Cockpit::LoadSetCBody: r3 the cockpit, r4 its set-up (144 bytes).
extern "C" REX_FUNC(sub_820D6498) {
  SBX_CAR_MODELS_FALLBACK(sub_820D6498)
  ctx.r3.u64 = LoadCockpit(g, At<CockpitObject>(ctx.r3.u32), ctx.r4.u32);
}

// Wheels::SetUp: r3 the wheels, r4 its set-up (156 bytes). Without
// --native_car_models the original runs, with the wheels' pool set's model
// set at 0 for the call when the parking area wants garage wheels.
extern "C" REX_FUNC(sub_820D8DD8) {
  REX_FUNC_PROLOGUE();
  if (!On()) {
    ScenePoolSet* pool_set = At<Wheels>(ctx.r3.u32)->first_set;
    if (!hooks::ParkingArea_GarageWheels() || pool_set == nullptr) {
      __imp__sub_820D8DD8(ctx, base);
      return;
    }
    const uint32_t model_set = pool_set->model_set;
    pool_set->model_set = 0;
    __imp__sub_820D8DD8(ctx, base);
    pool_set->model_set = model_set;
    return;
  }
  Guest g(ctx, base);
  ctx.r3.u64 = SetUpWheels(g, At<Wheels>(ctx.r3.u32), ctx.r4.u32);
}

// CarModel::OnFileLoaded and OnFileLoadedB: Archive::Load's callbacks, r3
// the request, r4 its buffer, r6 the car model.
extern "C" REX_FUNC(sub_820D4448) {
  SBX_CAR_MODELS_FALLBACK(sub_820D4448)
  FileLoaded(g, At<CarModel>(ctx.r6.u32), ctx.r3.u32, ctx.r4.u32, false);
}

extern "C" REX_FUNC(sub_820D4C28) {
  SBX_CAR_MODELS_FALLBACK(sub_820D4C28)
  FileLoaded(g, At<CarModel>(ctx.r6.u32), ctx.r3.u32, ctx.r4.u32, true);
}

// SceneWheels::SetUp: r3 the second set's wheels, r4 the record.
extern "C" REX_FUNC(sub_820E6CA8) {
  REX_FUNC_PROLOGUE();
  Guest g(ctx, base);  // (First: it sets where guest memory is.)
  if (!On()) {
    __imp__sub_820E6CA8(ctx, base);
    return;
  }
  game::SceneWheelsSetUp(g, At<game::PolledWheels>(ctx.r3.u32), ctx.r4.u32);
}

// CarScene::SetUp: r3 the car scene, r4 its set-up record, r5 a word kept
// at +4996.
extern "C" REX_FUNC(sub_820C8B88) {
  SBX_CAR_MODELS_FALLBACK(sub_820C8B88)
  SetUpCarScene(g, At<CarSceneState>(ctx.r3.u32), At<CarSceneRecord>(ctx.r4.u32), ctx.r5.u32);
}

// --- The TGA reader ---

// TGA::CreateImage: r3 the header out, r4 the file's size, r5 where the
// file's address is, r6 where the pixels' address goes. Returns 1.
extern "C" REX_FUNC(sub_820ECC58) {
  SBX_CAR_MODELS_FALLBACK(sub_820ECC58)
  CreateImage(g, At<TGAHeader>(ctx.r3.u32), ctx.r4.u32, *At<u32_be>(ctx.r5.u32),
              At<u32_be>(ctx.r6.u32));
  ctx.r3.u64 = 1;
}

// TGAReader::TGAReader: r3 the reader. Returns it.
extern "C" REX_FUNC(sub_820EE9C8) {
  SBX_CAR_MODELS_FALLBACK(sub_820EE9C8)
  TGAReader* reader = At<TGAReader>(ctx.r3.u32);
  InitReader(g, reader);
  ctx.r3.u64 = AddressOf(reader);
}

// TGAReader::Load: r3 the reader, r4 the file's size, r5 the file.
extern "C" REX_FUNC(sub_820EE8E8) {
  SBX_CAR_MODELS_FALLBACK(sub_820EE8E8)
  ctx.r3.u64 = LoadReader(g, At<TGAReader>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// TGAReader::~TGAReader: r3 the reader.
extern "C" REX_FUNC(sub_820EE958) {
  SBX_CAR_MODELS_FALLBACK(sub_820EE958)
  FreeReader(g, At<TGAReader>(ctx.r3.u32));
}

// TGAImage::Free: r3 the image.
extern "C" REX_FUNC(sub_820EEBC8) {
  SBX_CAR_MODELS_FALLBACK(sub_820EEBC8)
  ctx.r3.u64 = FreeImage(g, At<TGAImage>(ctx.r3.u32));
}

// TGAHeader::Clear and TGAImage::Clear: r3 the header or image. Return r3.
extern "C" REX_FUNC(sub_820EEA30) {
  SBX_CAR_MODELS_FALLBACK(sub_820EEA30)
  ClearHeader(At<TGAHeader>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_820EEBA8) {
  SBX_CAR_MODELS_FALLBACK(sub_820EEBA8)
  ClearImage(At<TGAImage>(ctx.r3.u32));
}

// TGA::ParseHeader: r3 the header, r4 the file. Returns 0.
extern "C" REX_FUNC(sub_820EEA68) {
  SBX_CAR_MODELS_FALLBACK(sub_820EEA68)
  ParseHeader(At<TGAHeader>(ctx.r3.u32), At<uint8_t>(ctx.r4.u32));
  ctx.r3.u64 = 0;
}

// TGA::Decode: r3 the image, r4 the file, r5 its parsed header.
extern "C" REX_FUNC(sub_820EEFD8) {
  SBX_CAR_MODELS_FALLBACK(sub_820EEFD8)
  ctx.r3.u64 = Decode(g, At<TGAImage>(ctx.r3.u32), ctx.r4.u32, At<TGAHeader>(ctx.r5.u32));
}

// TGA::DecodeRle: r3 the image, r4 the packets. Returns 0.
extern "C" REX_FUNC(sub_820EEF28) {
  SBX_CAR_MODELS_FALLBACK(sub_820EEF28)
  DecodeRle(At<TGAImage>(ctx.r3.u32), ctx.r4.u32);
  ctx.r3.u64 = 0;
}

// TGA::ToARGB: r3 the image, r4 where the pixels' address goes.
extern "C" REX_FUNC(sub_820EED78) {
  SBX_CAR_MODELS_FALLBACK(sub_820EED78)
  ToARGB(g, At<TGAImage>(ctx.r3.u32), At<u32_be>(ctx.r4.u32));
}

// --- The vinyl sheet's software rasterizer ---

// SoftRaster::SampleTexel: r3 the image, f1 and f2 the texel coordinates.
extern "C" REX_FUNC(sub_820ECED8) {
  SBX_CAR_MODELS_FALLBACK(sub_820ECED8)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = SampleTexel(*At<RasterImage>(ctx.r3.u32), ctx.f1.f64, ctx.f2.f64);
}

// SoftRaster::BlendPixel: r3 the image, r4 where the canvas' address is,
// r5 the region, r6 and r7 the pixel, r8 the texel's two floats.
extern "C" REX_FUNC(sub_820ED0D0) {
  SBX_CAR_MODELS_FALLBACK(sub_820ED0D0)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = BlendPixel(*At<RasterImage>(ctx.r3.u32), *At<u32_be>(ctx.r4.u32),
                          *At<RasterRegion>(ctx.r5.u32), ctx.r6.s32, ctx.r7.s32,
                          At<guest::f32_be>(ctx.r8.u32));
}

// Vinyl::DecalCorner: r3 the corner out, r4 the corner, r5 the decal's
// center, r6 its scale, r8 the offset, f1 the angle.
extern "C" REX_FUNC(sub_820ECE20) {
  SBX_CAR_MODELS_FALLBACK(sub_820ECE20)
  ctx.fpscr.disableFlushMode();
  DecalCorner(g, At<DecalPoint>(ctx.r3.u32), *At<DecalPoint>(ctx.r4.u32),
              *At<DecalPoint>(ctx.r5.u32), *At<DecalPoint>(ctx.r6.u32),
              *At<DecalPoint>(ctx.r8.u32), ctx.f1.f64);
}

// SoftRaster::DrawSpan: r3 where the canvas' address is, r4 the region,
// r5, r6 the ends, r7 the row, r8, r9 the ends' texels, r10 the image, the
// caller's stack +84 the flags.
extern "C" REX_FUNC(sub_820EDC70) {
  SBX_CAR_MODELS_FALLBACK(sub_820EDC70)
  ctx.fpscr.disableFlushMode();
  DrawSpan(*At<u32_be>(ctx.r3.u32), *At<RasterRegion>(ctx.r4.u32), ctx.r5.s32, ctx.r6.s32,
           ctx.r7.s32, *At<RasterTexel>(ctx.r8.u32), *At<RasterTexel>(ctx.r9.u32),
           *At<RasterImage>(ctx.r10.u32), *At<u32_be>(ctx.r1.u32 + 84));
}

// SoftRaster::DrawTriangle: r3 where the canvas' address is, r4 the
// region, r5 the three positions, r6 their texels (16 bytes apart), r7 the
// image, r8 the flags.
extern "C" REX_FUNC(sub_820EDF38) {
  SBX_CAR_MODELS_FALLBACK(sub_820EDF38)
  ctx.fpscr.disableFlushMode();
  DrawTriangle(*At<u32_be>(ctx.r3.u32), *At<RasterRegion>(ctx.r4.u32),
               At<RasterVertex>(ctx.r5.u32), At<RasterTexel>(ctx.r6.u32),
               *At<RasterImage>(ctx.r7.u32), ctx.r8.u32);
}

// Vinyl::DrawDecal: r3 the region (0-9; others run the original), r4
// mirrored, r5 to r7 the TGA header by value, r8 and r9 the record's offset
// (x, y first), f1 the angle; the caller's stack +96 the scale, +116 the
// pixels, +132 the sheet's data.
extern "C" REX_FUNC(sub_820EE4A8) {
  REX_FUNC_PROLOGUE();
  if (!On() || ctx.r3.u32 >= kDecalRegionCount) {
    __imp__sub_820EE4A8(ctx, base);
    return;
  }
  Guest g(ctx, base);
  ctx.fpscr.disableFlushMode();
  TGAHeader header;
  auto* words = reinterpret_cast<be<uint64_t>*>(&header);
  words[0] = ctx.r5.u64;
  words[1] = ctx.r6.u64;
  *reinterpret_cast<u32_be*>(&words[2]) = uint32_t(ctx.r7.u64 >> 32);
  DecalPoint place;
  *reinterpret_cast<be<uint64_t>*>(&place) = ctx.r8.u64;
  const uint32_t caller = ctx.r1.u32;
  DrawDecal(g, ctx.r3.u32, ctx.r4.u32 != 0, header, place, ctx.f1.f64,
            *At<DrawDecalArguments>(caller));
}

// Vinyl::DrawDecals: r3 the car model. Returns 1. A decal whose region
// (a record's kind, or layer 0's) is past 9 runs the original.
extern "C" REX_FUNC(sub_820D3D18) {
  REX_FUNC_PROLOGUE();
  Guest g(ctx, base);  // (First: it sets where guest memory is.)
  CarModel* model = At<CarModel>(ctx.r3.u32);
  if (!On() || !DecalRegionsKnown(model)) {
    __imp__sub_820D3D18(ctx, base);
    return;
  }
  VinylDrawDecals(g, model);
  ctx.r3.u64 = 1;
}

// Vinyl::Compress: r3, r4 the size, r5, r6 where the sheet's header and
// data addresses are (r7, r8 not read).
extern "C" REX_FUNC(sub_820ECCC8) {
  SBX_CAR_MODELS_FALLBACK(sub_820ECCC8)
  ctx.r3.u64 = CompressSheet(g, ctx.r3.u32, ctx.r4.u32, At<u32_be>(ctx.r5.u32),
                             At<u32_be>(ctx.r6.u32));
}

// --- The license plate ---

// CarModel::MakePlate: r3 to r6 where texture A's header and data and B's
// go; the 11-word choices by value, W0 to W7 in r7 to r10 (two a
// register, the first in the high word), W8 and W9 at the caller's stack
// +80, W10 at +88.
extern "C" REX_FUNC(sub_820ED308) {
  SBX_CAR_MODELS_FALLBACK(sub_820ED308)
  const u32_be* stack = At<u32_be>(ctx.r1.u32 + 80);
  const uint64_t pairs[4] = {ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64};
  int32_t w[11];
  for (int i = 0; i < 4; i++) {
    w[2 * i] = int32_t(uint32_t(pairs[i] >> 32));
    w[2 * i + 1] = int32_t(uint32_t(pairs[i]));
  }
  w[8] = int32_t(uint32_t(stack[0]));
  w[9] = int32_t(uint32_t(stack[1]));
  w[10] = int32_t(uint32_t(stack[2]));
  const PlateWords words{w[0], w[1], w[2], {w[3], w[4], w[5]}, {w[6], w[7]},
                         w[8], {w[9], w[10]}};
  MakePlate(g, At<u32_be>(ctx.r3.u32), At<u32_be>(ctx.r4.u32), At<u32_be>(ctx.r5.u32),
            At<u32_be>(ctx.r6.u32), words);
}

// --- The plate's images (CarModel::MakePlate's) ---

// PlateImage: r3 the car scene, r4 a kind, r5 an index, r6 where the
// header goes, r7 where the pixels' address goes. Returns r6.
extern "C" REX_FUNC(sub_820C4BD0) {
  SBX_CAR_MODELS_FALLBACK(sub_820C4BD0)
  ctx.r3.u64 = PlateImage(g, At<CarSceneState>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                          At<u32_be>(ctx.r7.u32));
}

// CopyRect: r3 the target image, r4 its (x, y), r5 the (width, height),
// r6's high word the target's pitch, r8 the source image, r9 its (x, y),
// the caller's stack +96 the source's pitch (pairs in 64-bit registers, the
// first in the high word).
extern "C" REX_FUNC(sub_820ED268) {
  SBX_CAR_MODELS_FALLBACK(sub_820ED268)
  auto high = [](const PPCRegister& r) { return uint32_t(r.u64 >> 32); };
  PixelRect to{ctx.r3.u32, high(ctx.r4), ctx.r4.u32, high(ctx.r6)};
  PixelRect from{ctx.r8.u32, high(ctx.r9), ctx.r9.u32, *At<u32_be>(ctx.r1.u32 + 96)};
  CopyRect(g, to, from, high(ctx.r5), ctx.r5.s32);
}
