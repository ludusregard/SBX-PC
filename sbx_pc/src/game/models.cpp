// The models as our own C++ (--native_models, on by default): a model's
// load (its mesh file parsed, its XPR set, its weights rounded), the models'
// and part models' constructors, destructors, clears and releases, their
// XPR lists (a std::vector of XPR objects by slot) and materials, an XPR's
// release and texture lookup, the graphics resources' relocation, the
// vertex declarations and shared shaders, the shaders' loads, the skeleton's
// set-up over a node file (its pose reset, local and node matrices), the
// shapes' draw recording (Model::RecordShape, and a car part's
// Model::RecordMaterials), the memory blocks' physical allocation and
// put-off frees, and the scoped lock.
//
// The functions other files of the group call are declared in model.h;
// this file calls model_animation.cpp's (the track sets, the skeleton's
// construction, frees and animation) directly.

#include "base/settings.h"
#include "game/model.h"

#include "game/model_animation.h"
#include "game/shader_objects.h"
#include "game/std_types.h"

#include <bit>
#include <climits>
#include <cmath>

SBX_SETTING_BOOL(native_models, true, "Overrides", "Scene",
                 "Our own C++ for the models (their loads, set-ups and releases, XPR lists, "
                 "materials, XPRs, resource relocation, vertex declarations, skeleton "
                 "set-ups and node matrices, memory blocks) and the shaders' loads, instead "
                 "of the recompiled functions");

namespace {

using game::Material;
using game::MaterialList;
using game::Mesh;
using game::Matrix;
using game::Model;
using game::NodeFile;
using game::NodeFileView;
using game::NodeRecord;
using game::NodeState;
using game::PartModel;
using game::PointerVector;
using game::PtrList;
using game::PtrListNode;
using game::ShaderPairEntry;
using game::Skeleton;
using game::TrackSet;
using game::VertexWeights;
using game::Vector;
using game::XPR;
using game::XPRList;
using game::XPRRecord;
using game::MaterialCount;
using game::ModelRelease;
using game::SkeletonApplyAnimation;
using game::SkeletonBaseFree;
using game::SkeletonCursorClear;
using game::SkeletonLoadNodeValues;
using game::SharedPixelShader;
using game::SharedVertexShader;
using game::SkeletonNodeCount;
using game::VertexDeclaration;
using game::XprListConstruct;
using game::XprFree;
using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;
using guest::f32_be;
using guest::i32_be;
using guest::u32_be;

constexpr uint32_t kModelVtable = 0x82069AF8;
constexpr uint32_t kPartModelVtable = 0x8200336C;

// The shaders: the global cleared before they load (the shared pairs'
// cursor into their buffer), the buffer, and the shared ones set after.
constexpr uint32_t kShadersLoaded = 0x826B19B8;
constexpr uint32_t kShaderBuffer = 0x82694E14;
constexpr uint32_t kSharedShaders = 0x8296DC14;  // Two words.
constexpr uint32_t kSharedVertexShaders = 0x826AF998;
constexpr uint32_t kSharedPixelShaders = 0x826AD978;
constexpr uint32_t kOnPairLoaded = 0x820A08B8;  // Shaders::OnPairLoaded

// The shader pairs loaded: each run's first file id and first slot, and
// how many (the file ids two apart).
struct ShaderRun {
  uint32_t first_file;
  uint32_t first_slot;
  uint32_t count;
};
constexpr ShaderRun kShaderRuns[] = {
    {3, 0, 34},
    {22800, 1000, 6},
    {24256, 2000, 56},
};

// The shared pair the context of a shared shader load carries.
struct ShaderPairLoad {
  u32_be vertex_file;
  u32_be pixel_file;
  u32_be pair;
};
GUEST_SIZE(ShaderPairLoad, 12);

// The vertex declarations: the stream kinds' tables (by the first and third
// stream types) and the declarations by kind (writable data).
constexpr uint32_t kFirstStreamKinds = 0x8264E960;
constexpr uint32_t kThirdStreamKinds = 0x8264E9A8;
constexpr uint32_t kDeclarations = 0x8296DC54;
constexpr uint32_t kDeclarationError = 0x82068D70;  // "...GetVertexDeclaration Error\n"

// The memory blocks: each has a 4 KB header before it. A block whose header
// carries the magic is put on the pending frees, freed some frames later.
constexpr uint32_t kMemoryBlocksLock = 0x8296E198;
constexpr uint32_t kPendingFrees = 0x8296E1B4;  // A PtrList of headers.
constexpr uint32_t kBlockHeaderBytes = 4096;
constexpr uint32_t kBlockMagic = 0x584C4D4D;  // "XLMM"
constexpr uint32_t kPhysicalBlock = 2;
constexpr uint32_t kMaxListSize = 0x3FFFFFFF;

struct MemoryBlockHeader {
  u32_be magic;
  u32_be kind;       // 0 virtual, 2 physical.
  u32_be countdown;  // Frames to wait before the free.
  u32_be attributes;  // XMemFree's, for a physical block.
};
GUEST_SIZE(MemoryBlockHeader, 16);

// A graphics resource's header, as far as relocating it reads it.
struct GpuResource {
  u32_be common;
  uint8_t unknown_4[20];
  u32_be address;  // Vertex and index buffers' data.
  uint8_t unknown_28[4];
  u32_be base_address;  // A texture's data (above its low 12 bits).
  uint8_t unknown_36[8];
  u32_be mip_info;     // Bits 6-9: the mip level count less 1.
  u32_be mip_address;  // A texture's mips (above its low 12 bits).
};
GUEST_OFFSET(GpuResource, address, 24);
GUEST_OFFSET(GpuResource, base_address, 32);
GUEST_OFFSET(GpuResource, mip_info, 44);
GUEST_OFFSET(GpuResource, mip_address, 48);

// The resource types (GpuResource::Type, sub_822A3788) relocation tells
// apart.
constexpr int32_t kResourceVertexBuffer = 1;
constexpr int32_t kResourceIndexBuffer = 2;
constexpr int32_t kResourceTexture = 3;
constexpr int32_t kResourceType8 = 8;
constexpr int32_t kResourceFirstTexture2 = 17;  // 17 to 20: textures too.
constexpr int32_t kResourceLastTexture2 = 20;

constexpr uint32_t kMaxVectorSize = 0x3FFFFFFF;

struct ScopedLock {
  u32_be lock;
};

bool On() { return SBX_GET(native_models); }

void ClearPartModel(PartModel* part) {
  for (u32_be& word : part->vinyl_textures) {
    word = 0;
  }
  for (u32_be& word : part->unknown_452) {
    word = 0;
  }
  part->unknown_432 = 0;
  part->unknown_436 = -1;
  part->unknown_440 = 0;
}

void OperatorDelete(Guest& g, uint32_t block) { g.Call(sub_8258C868, block); }
uint32_t NewArray(Guest& g, uint32_t bytes) { return g.Call(sub_8258C0C0, bytes); }  // operator new[]
void MemoryMove(Guest& g, uint32_t to, uint32_t from, uint32_t bytes) {
  g.Call(sub_8258CAC8, to, bytes, from, bytes);  // memmove_s
}

// --- The memory blocks ---

// The memory blocks' lock, held while one lives.
class MemoryBlocksLock {
 public:
  explicit MemoryBlocksLock(Guest& g) : g_(g) { g_.Call(sub_82574178, kMemoryBlocksLock); }  // Lock
  ~MemoryBlocksLock() { g_.Call(sub_8258C4A8, kMemoryBlocksLock); }                        // Unlock
  MemoryBlocksLock(const MemoryBlocksLock&) = delete;
  MemoryBlocksLock& operator=(const MemoryBlocksLock&) = delete;

 private:
  Guest& g_;
};

// The list's size raised by `count`; past its limit std::list throws
// length_error (the original's, never seen).
void PendingFreesIncSize(Guest& g, PtrList* list, uint32_t count) {
  if (kMaxListSize - list->size < count) {
    g.Call(__imp__sub_82573EF8, list, count);
    return;
  }
  list->size += count;
}

// A node holding `value` linked in before `where`.
void PendingFreesInsert(Guest& g, PtrList* list, PtrListNode* where, uint32_t value) {
  PtrListNode* before = where->previous;
  PtrListNode* node = At<PtrListNode>(g.Call(sub_8258C000, 12u));  // operator new
  if (node == nullptr) {
    return;
  }
  node->next = where;
  node->previous = before;
  node->value = value;
  PendingFreesIncSize(g, list, 1);
  where->previous = node;
  node->previous->next = node;
}

// A block's header when it carries the magic, else nullptr.
MemoryBlockHeader* BlockHeader(uint32_t block) {
  if (block == 0) {
    return nullptr;
  }
  MemoryBlockHeader* header = At<MemoryBlockHeader>(block - kBlockHeaderBytes);
  return header->magic == kBlockMagic ? header : nullptr;
}

// The block's header put at the end of the pending frees.
void QueueFree(Guest& g, MemoryBlockHeader* header) {
  PtrList* list = At<PtrList>(kPendingFrees);
  PendingFreesInsert(g, list, list->head, AddressOf(header));
}

void FreeLater(Guest& g, uint32_t block) {
  MemoryBlocksLock lock(g);
  if (MemoryBlockHeader* header = BlockHeader(block)) {
    QueueFree(g, header);
  }
}

void FreePhysicalLater(Guest& g, uint32_t block, uint32_t attributes) {
  MemoryBlocksLock lock(g);
  if (MemoryBlockHeader* header = BlockHeader(block)) {
    header->attributes = attributes;
    QueueFree(g, header);
  }
}

// `bytes` of physical memory with XMemAlloc's attributes, after a header.
uint32_t AllocPhysical(Guest& g, uint32_t bytes, uint32_t attributes) {
  MemoryBlocksLock lock(g);
  const uint32_t pages = (bytes + kBlockHeaderBytes * 2 - 1) & ~(kBlockHeaderBytes - 1);
  MemoryBlockHeader* header = At<MemoryBlockHeader>(g.Call(sub_82295118, pages, attributes));  // XMemAlloc
  if (header == nullptr) {
    return 0;
  }
  header->kind = kPhysicalBlock;
  header->magic = kBlockMagic;
  header->countdown = 1;
  header->attributes = 0;
  return AddressOf(header) + kBlockHeaderBytes;
}

// --- XPRs ---

// The resource whose record has the name (any case); 0 when none has.
uint32_t XprFindTexture(Guest& g, XPR* xpr, uint32_t name) {
  if (xpr->records == 0 || name == 0) {
    return 0;
  }
  for (uint32_t i = 0; i < xpr->record_count; i++) {
    if (g.Call(sub_8258FAF0, name, Element<XPRRecord>(xpr->records, i)->name) == 0) {  // _stricmp
      return xpr->header + Element<XPRRecord>(xpr->records, i)->offset;
    }
  }
  return 0;
}

// --- The graphics resources' relocation ---

// An address field moved by `base` above its low bits, which stay.
uint32_t Moved(uint32_t field, uint32_t base, uint32_t low_mask) {
  return (((field & ~low_mask) + base) & ~low_mask) | (field & low_mask);
}

uint32_t MipLevelCount(const GpuResource* texture) { return ((texture->mip_info >> 6) & 0xF) + 1; }

void RelocateTexture(GpuResource* texture, uint32_t base, uint32_t mip_base) {
  texture->base_address = Moved(texture->base_address, base, 0xFFF);
  if (MipLevelCount(texture) > 1) {
    texture->mip_address = Moved(texture->mip_address, mip_base, 0xFFF);
  }
}

void RelocateResource(Guest& g, GpuResource* resource, uint32_t base) {
  const int32_t type = int32_t(g.Call(sub_822A3788, resource));  // GpuResource::Type
  if (type == kResourceVertexBuffer) {
    resource->address = Moved(resource->address, base, 3);
  } else if (type == kResourceIndexBuffer || type == kResourceType8) {
    resource->address = resource->address + base;
  } else if (type == kResourceTexture ||
             (type >= kResourceFirstTexture2 && type <= kResourceLastTexture2)) {
    const uint32_t mip_base = (resource->mip_address & ~0xFFFu) != 0 ? base : 0;
    RelocateTexture(resource, base, mip_base);
  }
}

// --- Vertex declarations and shared shaders ---

uint32_t SharedShader(uint32_t table_address, uint32_t pair) {
  const uint32_t table = *At<u32_be>(table_address);
  return table != 0 ? uint32_t(*Element<u32_be>(table, pair)) : 0;
}

// --- The shaders' loads ---

// A shared pair: the vertex file and the pixel file after it placed back
// to back in the shaders' buffer, and both loaded; the pixel file's load
// makes the pair (Shaders::OnPairLoaded).
void ShadersLoadPair(Guest& g, uint32_t vertex_file, uint32_t pair) {
  const uint32_t pixel_file = vertex_file + 1;
  const uint32_t vertex_size = g.Call(sub_820A0860, vertex_file);  // Archive::GetSize
  const uint32_t pixel_size = g.Call(sub_820A0860, pixel_file);
  u32_be& loaded = *At<u32_be>(kShadersLoaded);
  const uint32_t buffer = *At<u32_be>(kShaderBuffer);
  const uint32_t vertex_to = buffer + loaded;
  const uint32_t pixel_to = buffer + (loaded + vertex_size);
  loaded = loaded + vertex_size + pixel_size;
  ShaderPairLoad* load = At<ShaderPairLoad>(g.Call(sub_8258C000, 12u));  // operator new
  if (load != nullptr) {
    load->vertex_file = vertex_to;
    load->pixel_file = pixel_to;
    load->pair = pair;
  }
  g.Call(sub_8209DF80, vertex_file, vertex_to, 0u, 0u, 0u, 0u);  // Archive::Load
  g.Call(sub_8209DF80, pixel_file, pixel_to, kOnPairLoaded, load, 0u, 0u);
}

// --- XPR lists (std::vector of XPR pointers) ---

// operator new for `count` pointers; a count whose bytes overflow takes the
// original's path (std::bad_alloc made first; never seen).
uint32_t PointersAllocate(Guest& g, PointerVector* vector, uint32_t count) {
  if (count != 0 && 0xFFFFFFFFu / count < 4) {
    return g.Call(__imp__sub_82576800, vector, count);
  }
  return g.Call(sub_8258C000, count * 4);  // operator new
}

// Room for `count` elements, the elements kept.
void PointersReserve(Guest& g, PointerVector* vector, uint32_t count) {
  if (count > kMaxVectorSize) {
    g.Call(sub_82579A18);  // std::_Xlen (throws)
    return;
  }
  if (vector->Capacity() >= count) {
    return;
  }
  const uint32_t storage = PointersAllocate(g, vector, count);
  g.Call(sub_825798E8, vector->first, vector->last, storage);  // std::_Uninitialized_copy
  uint32_t size = 0;
  if (vector->first != 0) {
    size = vector->Size();
    OperatorDelete(g, vector->first);
  }
  vector->first = storage;
  vector->end = storage + count * 4;
  vector->last = storage + size * 4;
}

void FillPointers(uint32_t from, uint32_t to, uint32_t value) {
  for (uint32_t at = from; at != to; at += 4) {
    *At<u32_be>(at) = value;
  }
}

// `count` copies of `value` inserted at `where`.
void PointersInsertN(Guest& g, PointerVector* vector, uint32_t where, uint32_t count,
                     uint32_t value) {
  if (count == 0) {
    return;
  }
  const uint32_t capacity = vector->Capacity();
  if (kMaxVectorSize - vector->Size() < count) {
    g.Call(sub_82579A18);  // std::_Xlen (throws)
    return;
  }

  if (capacity < vector->Size() + count) {
    // New storage: half again the capacity (none when that would pass the
    // limit), at least the new size; the front, the copies, then the back.
    const uint32_t growth = capacity >> 1;
    uint32_t new_capacity = kMaxVectorSize - growth >= capacity ? growth + capacity : 0;
    if (new_capacity < vector->Size() + count) {
      new_capacity = vector->Size() + count;
    }
    const uint32_t storage = PointersAllocate(g, vector, new_capacity);
    const uint32_t front = uint32_t(int32_t(where - vector->first) >> 2);
    const uint32_t copies = storage + front * 4;
    if (front != 0) {
      MemoryMove(g, storage, vector->first, front * 4);
    }
    FillPointers(copies, copies + count * 4, value);
    const uint32_t back = uint32_t(int32_t(vector->last - where) >> 2);
    if (back != 0) {
      MemoryMove(g, copies + count * 4, where, back * 4);
    }
    const uint32_t new_size = vector->Size() + count;
    if (vector->first != 0) {
      OperatorDelete(g, vector->first);
    }
    vector->first = storage;
    vector->end = storage + new_capacity * 4;
    vector->last = storage + new_size * 4;
    return;
  }

  const uint32_t last = vector->last;
  const uint32_t tail = uint32_t(int32_t(last - where) >> 2);
  if (tail < count) {
    // The tail moved past the copies; the copies fill the gap and its old
    // place.
    if (tail != 0) {
      MemoryMove(g, where + count * 4, where, tail * 4);
    }
    const uint32_t beyond = count - uint32_t(int32_t(vector->last - where) >> 2);
    FillPointers(vector->last, vector->last + beyond * 4, value);
    const uint32_t old_last = vector->last;
    vector->last = old_last + count * 4;
    FillPointers(where, old_last, value);
    return;
  }

  // The last `count` moved to the end, the rest of the tail shifted up
  // (std::copy_backward), the copies at `where`.
  const uint32_t bytes = count * 4;
  MemoryMove(g, last, last - bytes, bytes);
  vector->last = last + bytes;
  g.Call(sub_82576870, where, last - bytes, last);  // std::copy_backward
  FillPointers(where, where + bytes, value);
}

// The elements dropped; the storage kept. Returns the first.
uint32_t PointersClear(PointerVector* vector) {
  if (vector->first != vector->last) {
    vector->last = vector->first;
  }
  return vector->first;
}

void PointersAssign(Guest& g, PointerVector* vector, uint32_t count, uint32_t value) {
  PointersClear(vector);
  PointersInsertN(g, vector, vector->first, count, value);
}

void XprListDestruct(Guest& g, XPRList* list) {
  list->vtable = XPRList::kVtable;
  PointersClear(&list->items);
  if (list->items.first != 0) {
    OperatorDelete(g, list->items.first);
  }
  list->items.first = 0;
  list->items.last = 0;
  list->items.end = 0;
}

// --- Models ---

// Where Mesh::Parse's copies return to (the memcpy wrapper logs its
// callers).
constexpr uint32_t kParseHeaderCopyReturn = 0x8257BDB0;
constexpr uint32_t kParseVertexCopyReturn = 0x8257C080;
constexpr uint32_t kVersionError = 0x82069BC8;     // "Version Error %x\n"
constexpr uint32_t kPhysicalAllocError = 0x82069BA8;  // "XPhysicalAllocEx Error 0x%x\n"

void SkeletonBind(Guest& g, Skeleton* skeleton, uint32_t file, uint32_t matrices);

void RelocateBuffer(Guest& g, game::GpuBuffer* buffer, uint32_t base) {
  RelocateResource(g, At<GpuResource>(AddressOf(buffer)), base);
}

// Mesh::Parse: the file's shaders loaded, its header copied to normal
// memory and its offsets made addresses, the skeleton and track sets set up
// from its sections, then its vertex data copied to physical memory and the
// meshes' buffers relocated to it.
void MeshParse(Guest& g, Model* model, uint32_t file_address) {
  game::MeshFile* file = At<game::MeshFile>(file_address);
  if (file->version != game::MeshFile::kVersion) {
    g.Call(sub_82591FE0, kVersionError, uint32_t(file->version));  // DebugPrint
  }
  g.Call(sub_8257A620, model, file_address + file->shaders);  // Mesh::LoadShaders

  model->header_copy = g.Call(sub_82573B80, uint32_t(file->header_bytes));  // Memory::AllocVirtual
  {
    guest::ReturnAddress from(g, kParseHeaderCopyReturn);
    g.Call(sub_8258CB90, model->header_copy, file_address, uint32_t(file->header_bytes));  // memcpy
  }
  const uint32_t header_address = model->header_copy;
  game::MeshFileHeader* header = At<game::MeshFileHeader>(header_address);
  model->header = header_address;
  model->header_sections[0] = header_address + header->section_offsets[0];
  model->header_sections[1] = header_address + header->section_offsets[1];
  model->meshes = header_address + header->section_offsets[2];
  model->header_section_44 = header_address + header->section_offsets[3];
  model->header_count_0 = header->counts[0];
  model->header_count_1 = header->counts[1];
  model->shape_count = header->counts[2];
  model->header_count_3 = header->counts[3];
  auto shape_count = [&] { return int32_t(uint32_t(model->shape_count)); };

  // The shapes' and their weights' offsets made addresses in the copy.
  for (int32_t i = 0; i < shape_count(); i++) {
    Mesh* mesh = model->MeshAt(i);
    mesh->materials.address = mesh->materials.address + model->header_copy;
    mesh->weights.address = mesh->weights.address + model->header_copy;
    mesh->unknown_400 = mesh->unknown_400 + model->header_copy;
    mesh->unknown_404 = mesh->unknown_404 + model->header_copy;
    VertexWeights* weights = model->MeshAt(i)->weights;
    const uint32_t counts = weights->counts;
    const uint32_t values = weights->weights;
    const uint32_t unknown_16 = weights->unknown_16;
    weights->counts = counts + model->header_copy;
    weights->weights = values + model->header_copy;
    weights->unknown_16 = unknown_16 + model->header_copy;
  }
  // The materials' +384.
  for (int32_t i = 0; i < shape_count(); i++) {
    for (int32_t m = 0; m < int32_t(uint32_t(model->MeshAt(i)->materials->count)); m++) {
      u32_be& word = model->MeshAt(i)->materials->materials[m].unknown_384;
      word = model->header_copy + word;
    }
  }

  // The skeleton and the track sets.
  auto section = [&](uint32_t offset) { return model->header_copy + offset; };
  if (At<game::MeshFileHeader>(model->header)->has_skeleton != 0) {
    if (file->skeleton != 0) {
      SkeletonBind(g, model->SkeletonObject(), section(file->skeleton), 0);
    }
    if (file->node_values != 0) {
      SkeletonLoadNodeValues(model->SkeletonObject(), section(file->node_values));
    }
    if (file->float_tracks[0] != 0) {
      game::TrackSetSetupFloat(g, &model->tracks[1], section(file->float_tracks[0]));
    }
    if (file->float_tracks[1] != 0) {
      game::TrackSetSetupFloat(g, &model->tracks[2], section(file->float_tracks[1]));
    }
    if (file->tracks_32 != 0) {
      game::TrackSetSetup32(g, &model->tracks[3], section(file->tracks_32));
    }
    if (file->tracks_208 != 0) {
      game::TrackSetSetup208(g, &model->tracks[4], section(file->tracks_208));
    }
  }
  game::TrackSetSetupFloat4(g, &model->tracks[0], section(file->float4_tracks));

  if (model->shape_count == 0) {
    return;
  }
  // The vertex data, after the header in the file, into physical memory.
  const uint32_t data_in_file = file_address + file->header_bytes;
  model->vertex_data = g.Call(sub_82573C28, uint32_t(file->vertex_data_bytes), 0u, -1, 4u,
                                     0x404u);  // Memory::AllocPhysicalEx
  if (model->vertex_data == 0) {
    g.Call(sub_82586A00, kPhysicalAllocError, 0u);  // DebugPrint
  }
  {
    guest::ReturnAddress from(g, kParseVertexCopyReturn);
    g.Call(sub_8258CB90, model->vertex_data, data_in_file,
           uint32_t(file->vertex_data_bytes));  // memcpy
  }
  // Each buffer's data moved from its file offset to the copy, and its
  // header relocated; a skinned mesh has only the first buffer.
  auto move_data = [&](u32_be& data) {
    data = data - file->header_bytes;
    data = model->vertex_data + data;
  };
  for (int32_t i = 0; i < shape_count(); i++) {
    move_data(model->MeshAt(i)->vertex_data);
    RelocateBuffer(g, &model->MeshAt(i)->streams[0], model->vertex_data);
    if (model->MeshAt(i)->skinned != 1) {
      move_data(model->MeshAt(i)->stream_data);
      RelocateBuffer(g, &model->MeshAt(i)->streams[1], model->vertex_data);
      move_data(model->MeshAt(i)->stream2_data);
      RelocateBuffer(g, &model->MeshAt(i)->streams[2], model->vertex_data);
    }
    for (int32_t m = 0; m < int32_t(uint32_t(model->MeshAt(i)->materials->count)); m++) {
      RelocateBuffer(g, &model->MeshAt(i)->materials->materials[m].index_buffer,
                     model->vertex_data);
    }
  }
}

// A weight quantized to thousandths, as the game rounds it: x 1000 + 0.5
// in one fused step, truncated to an integer (its low word, unsigned), back
// to a float and x 0.001.
float QuantizeWeight(float weight) {
  const double scaled = double(float(std::fma(double(weight), 1000.0, 0.5)));
  // As the recompiled fctidz: NaN and anything out of range below 2^63
  // give INT64_MIN, above it INT64_MAX.
  int64_t truncated;
  if (std::isnan(scaled) || scaled == 0x1p63 || scaled < -0x1p63) {
    truncated = INT64_MIN;
  } else if (scaled > 0x1p63) {
    truncated = INT64_MAX;
  } else {
    truncated = int64_t(scaled);
  }
  const float whole = float(double(uint32_t(truncated)));
  return float(double(whole) * double(0.001f));
}

// Each skinned mesh's weights rounded to thousandths, a vertex's last
// weight made 1 less the others' sums (x, y and z each).
void NormalizeWeights(Model* model) {
  for (uint32_t shape = 0; shape < model->shape_count; shape++) {
    Mesh* mesh = model->MeshAt(shape);
    if (mesh->skinned != 1) {
      continue;
    }
    VertexWeights* block = mesh->weights;
    uint32_t weight = block->weights;
    for (uint32_t vertex = 0; vertex < block->vertex_count; vertex++) {
      const u32_be& count = *Element<u32_be>(block->counts, vertex);
      float sum_x = 0.0f;
      float sum_y = 0.0f;
      float sum_z = 0.0f;
      if (count != 1) {
        for (uint32_t i = 0; i < count - 1; i++, weight += sizeof(Vector)) {
          Vector* w = At<Vector>(weight);
          const float x = QuantizeWeight(w->x);
          const float y = QuantizeWeight(w->y);
          const float z = QuantizeWeight(w->z);
          w->x = x;
          sum_x = float(double(x) + double(sum_x));
          w->y = y;
          sum_y = float(double(y) + double(sum_y));
          w->z = z;
          sum_z = float(double(z) + double(sum_z));
        }
      }
      Vector* last = At<Vector>(weight);
      last->x = float(1.0 - double(sum_x));
      last->y = float(1.0 - double(sum_y));
      last->z = float(1.0 - double(sum_z));
      weight += sizeof(Vector);
    }
  }
}

// A shape's material block (the mesh's +396).
MaterialList* Materials(Model* model, uint32_t shape) { return model->MeshAt(shape)->materials; }

// --- Skeletons over a node file ---

// The node file's sections as addresses.
void NodeFileViewOf(NodeFileView* view, uint32_t file_address) {
  NodeFile* file = At<NodeFile>(file_address);
  view->file = file;
  view->nodes = At<NodeRecord>(file_address + file->offsets[0]);
  view->translations = file_address + file->offsets[1];
  view->angles = file_address + file->offsets[2];
  view->scales = file_address + file->offsets[3];
  view->unknown_20 = file_address + file->offsets[4];
  view->matrices = file_address + file->offsets[5];
  view->unknown_28 = file_address + file->offsets[6];
  const uint32_t unknown_32 = file_address + file->offsets[7];
  view->unknown_32 = unknown_32;
  view->unknown_36 = unknown_32 + 16;
  view->unknown_40 = file_address + file->offsets[8];
  view->node_minus_bone_count = file->node_count - file->bone_count;
}

// operator new[]'s size for a counted array (a 16-byte header), as the
// game asks: 0xFFFFFFFF past `max` or when the header doesn't fit.
uint32_t CountedArraySize(uint32_t count, uint32_t bytes, uint32_t max) {
  if (count > max) {
    return 0xFFFFFFFF;
  }
  const uint32_t values = count * bytes;
  return values <= 0xFFFFFFEFu ? values + 16 : 0xFFFFFFFF;
}

// A counted array: the header's count set; the elements' address, or 0.
uint32_t NewCountedArray(Guest& g, uint32_t count, uint32_t bytes, uint32_t max) {
  const uint32_t block = NewArray(g, CountedArraySize(count, bytes, max));
  if (block == 0) {
    return 0;
  }
  *At<u32_be>(block) = count;
  return block + 16;
}

// Matrices whose rows' last floats are 1.0 (the rest as operator new[]
// leaves them).
void SetRowEnds(Matrix* matrix) {
  matrix->right.w = 1.0f;
  matrix->up.w = 1.0f;
  matrix->forward.w = 1.0f;
  matrix->position.w = 1.0f;
}

// The skeleton made for a node file: the old arrays freed, a cursor for
// each instance, the file's view, a state for each node (and its own
// matrices, unless `matrices` are given), a block for each bone.
void SkeletonSetUp(Guest& g, Skeleton* skeleton, uint32_t file, uint32_t matrices,
                   int32_t instances) {
  SkeletonBaseFree(g, skeleton);
  skeleton->instance_count = instances;
  if (instances <= 0) {
    skeleton->instance_count = 1;
  }
  const uint32_t cursor_count = uint32_t(int32_t(skeleton->instance_count));
  const uint32_t cursors =
      NewArray(g, cursor_count <= 0x0CCCCCCC ? cursor_count * 20 : 0xFFFFFFFF);
  if (cursors != 0) {
    for (int32_t i = 0; i < int32_t(cursor_count); i++) {
      SkeletonCursorClear(Element<game::SkeletonCursor>(cursors, i));
    }
  }
  skeleton->cursors = At<game::SkeletonCursor>(cursors);
  NodeFileViewOf(&skeleton->node_file, file);

  const uint32_t nodes = uint32_t(int32_t(skeleton->node_file.file->node_count));
  if (nodes != 0) {
    const uint32_t states = NewCountedArray(g, nodes, sizeof(NodeState), 0x01C71C71);
    if (states != 0) {
      for (int32_t i = 0; i < int32_t(nodes); i++) {
        NodeState* state = Element<NodeState>(states, i);
        state->translation.w = 1.0f;
        state->angles.w = 1.0f;
        state->scale.w = 1.0f;
        state->extra.w = 1.0f;
        state->unknown_132 = 0;
        for (guest::i32_be& cursor : state->channel_cursors) {
          cursor = 0;
        }
      }
    }
    skeleton->node_states = At<NodeState>(states);
    if (matrices == 0) {
      const uint32_t count = uint32_t(int32_t(skeleton->node_file.file->node_count));
      const uint32_t own = NewCountedArray(g, count, sizeof(Matrix), 0x03FFFFFF);
      if (own != 0) {
        for (int32_t i = 0; i < int32_t(count); i++) {
          SetRowEnds(Element<Matrix>(own, i));
        }
      }
      skeleton->own_matrices = own;
      skeleton->matrices = own;
    } else {
      skeleton->matrices = matrices;
    }
  }

  const uint32_t bones = uint32_t(int32_t(skeleton->node_file.file->bone_count));
  if (bones != 0) {
    const uint32_t blocks = NewCountedArray(g, bones, sizeof(game::BoneMatrix), 0x03FFFFFF);
    if (blocks != 0) {
      for (int32_t i = 0; i < int32_t(bones); i++) {
        game::BoneMatrix* block = Element<game::BoneMatrix>(blocks, i);
        block->one_12 = 1.0f;
        block->one_28 = 1.0f;
        block->one_44 = 1.0f;
      }
    }
    skeleton->bone_matrices = At<game::BoneMatrix>(blocks);
    for (int32_t i = 0; i < skeleton->node_file.file->bone_count && blocks != 0; i++) {
      Element<game::BoneMatrix>(skeleton->bone_matrices.address, i)->unknown_52 = 1;
    }
  }
  skeleton->bone_states =
      At<NodeState>(skeleton->node_states.address +
                    uint32_t(sizeof(NodeState)) * skeleton->node_file.file->first_bone);
}

// Skeleton::Bind: the set-up for one instance, then a value for each node
// (x, y, z 0.0).
void SkeletonBind(Guest& g, Skeleton* skeleton, uint32_t file, uint32_t matrices) {
  SkeletonSetUp(g, skeleton, file, matrices, 1);
  if (skeleton->node_values.address != 0) {
    g.Call(sub_8258C940, skeleton->node_values.address);  // operator delete[]
  }
  skeleton->node_values = nullptr;
  const uint32_t nodes = uint32_t(int32_t(skeleton->node_file.file->node_count));
  if (nodes != 0) {
    skeleton->node_values = At<Vector>(NewArray(g, nodes <= 0x0FFFFFFF ? nodes * 16 : 0xFFFFFFFF));
  }
  for (int32_t i = 0; i < int32_t(SkeletonNodeCount(skeleton)); i++) {
    Vector& value = *skeleton->Value(i);
    value.x = 0.0f;
    value.y = 0.0f;
    value.z = 0.0f;
  }
}

// Each node's state back to the file's rest pose (and its extra row, for
// the nodes that have one), then the node channels applied.
void SkeletonResetPose(Guest& g, Skeleton* skeleton) {
  if (skeleton->node_values.address == 0) {
    return;
  }
  const NodeFileView& view = skeleton->node_file;
  const uint32_t nodes = view.nodes.address;
  const uint32_t states = skeleton->node_states.address;
  const uint32_t translations = view.translations;
  const uint32_t angles = view.angles;
  const uint32_t scales = view.scales;
  for (int32_t i = 0; i < int32_t(SkeletonNodeCount(skeleton)); i++) {
    NodeState* state = Element<NodeState>(states, i);
    const NodeRecord* node = Element<NodeRecord>(nodes, i);
    state->translation = *Element<Vector>(translations, i);
    state->angles = *Element<Vector>(angles, i);
    state->scale = *Element<Vector>(scales, i);
    if (node->kind & NodeRecord::kRestExtra) {
      state->extra = node->extra;
    }
  }
  SkeletonApplyAnimation(g, skeleton);
}

void MultiplyInto(Guest& g, Matrix* matrix, Matrix* by) {
  g.Call(sub_8209BE70, matrix, matrix, by);  // Matrix::Multiply
}

// --- Skeleton::LocalMatrices ---

// Its frame (384 bytes): the rotation builders write their matrix at +208,
// which the multiplies then read.
constexpr uint32_t kLocalMatricesFrame = 384;
constexpr uint32_t kRotationOffset = 208;

constexpr float kInverseTwoPi = std::bit_cast<float>(0x3E22F983u);  // 0x82001F7C
constexpr float kTwoPi = std::bit_cast<float>(0x40C90FDBu);         // 0x82001F80
constexpr uint32_t kPiBits = 0x40490FDB;

// fctiwz as the recompiled code does it: NaN and values below the range
// give INT_MIN, values from INT_MAX up INT_MAX, else truncation.
int32_t TruncateToInt32(double value) {
  if (std::isnan(value)) {
    return INT32_MIN;
  }
  if (value >= double(INT32_MAX)) {
    return INT32_MAX;
  }
  if (value <= double(INT32_MIN) - 1.0) {
    return INT32_MIN;
  }
  return int32_t(value);
}

// An angle brought into [-pi, pi] as the game does it: pi with the angle's
// sign added, divided into turns (truncated), that many turns taken off.
float WrapAngle(float angle) {
  const float half_turn = std::bit_cast<float>((std::bit_cast<uint32_t>(angle) & 0x80000000u) | kPiBits);
  const float shifted = float(double(half_turn) + double(angle));
  const float turns = float(double(shifted) * double(kInverseTwoPi));
  const float whole = float(double(int64_t(TruncateToInt32(double(turns)))));
  return float(-std::fma(double(whole), double(kTwoPi), -double(angle)));
}

const Matrix kIdentity{{1.0f, 0.0f, 0.0f, 0.0f},
                       {0.0f, 1.0f, 0.0f, 0.0f},
                       {0.0f, 0.0f, 1.0f, 0.0f},
                       {0.0f, 0.0f, 0.0f, 1.0f}};

// `matrix` turned by the three angles (RotateX, RotateY, RotateZ into the
// frame's matrix, each multiplied in), then by the file's matrix. `angle(k)`
// gives angle k when its turn comes.
template <typename Angle>
void TurnMatrix(Guest& g, Matrix* matrix, Matrix* rotation, Angle angle, Matrix* by) {
  g.ctx.f1.f64 = double(angle(0));
  g.Call(sub_820A6818, rotation);  // Matrix::RotateX
  MultiplyInto(g, matrix, rotation);
  g.ctx.f1.f64 = double(angle(1));
  g.Call(sub_820A68E8, rotation);  // Matrix::RotateY
  MultiplyInto(g, matrix, rotation);
  g.ctx.f1.f64 = double(angle(2));
  g.Call(sub_820A3088, rotation);  // Matrix::RotateZ
  MultiplyInto(g, matrix, rotation);
  MultiplyInto(g, matrix, by);
}

// Each node's local matrix from its state: a plain node's (kind 4 or
// below) angles wrapped and stored back, its matrix the scale turned by the
// angles and the file's matrix, its translation; then each bone's (bit
// 0x80) the same without the scale or the wrap.
void SkeletonLocalMatrices(Guest& g, Skeleton* skeleton) {
  if (skeleton->node_values.address == 0) {
    return;
  }
  guest::StackFrame frame(g.ctx, kLocalMatricesFrame);
  Matrix* rotation = At<Matrix>(frame.Local(kRotationOffset));

  const uint32_t states = skeleton->node_states.address;
  const uint32_t matrices = skeleton->matrices;
  const uint32_t file_matrices = skeleton->node_file.matrices;
  for (int32_t i = 0; i < int32_t(SkeletonNodeCount(skeleton)); i++) {
    if (skeleton->Node(i)->kind > NodeRecord::kLastPlainKind) {
      continue;
    }
    NodeState* state = Element<NodeState>(states, i);
    Matrix* matrix = Element<Matrix>(matrices, i);
    const float x = WrapAngle(state->angles.x);
    state->angles.x = x;
    const float y = WrapAngle(state->angles.y);
    state->angles.y = y;
    const float z = WrapAngle(state->angles.z);
    state->angles.z = z;
    *matrix = kIdentity;
    matrix->right.x = state->scale.x;
    matrix->up.y = state->scale.y;
    matrix->forward.z = state->scale.z;
    const float wrapped[3] = {x, y, z};
    TurnMatrix(g, matrix, rotation, [&](int k) { return wrapped[k]; },
               Element<Matrix>(file_matrices, i));
    matrix->position.x = state->translation.x;
    matrix->position.y = state->translation.y;
    matrix->position.z = state->translation.z;
  }

  const uint32_t first_bone = skeleton->node_file.file->first_bone;
  const uint32_t bone_states = skeleton->bone_states.address;
  for (int32_t i = 0; i < skeleton->node_file.file->bone_count; i++) {
    if (!(skeleton->Node(skeleton->node_file.file->first_bone + i)->kind & NodeRecord::kBone)) {
      continue;
    }
    NodeState* state = Element<NodeState>(bone_states, i);
    Matrix* matrix = Element<Matrix>(matrices, first_bone + i);
    *matrix = kIdentity;
    // The bones' angles are read as each turn comes.
    const f32_be* angles = &state->angles.x;
    TurnMatrix(g, matrix, rotation, [&](int k) { return float(*Element<f32_be>(AddressOf(angles), k)); },
               Element<Matrix>(file_matrices, first_bone + i));
    matrix->position.x = state->translation.x;
    matrix->position.y = state->translation.y;
    matrix->position.z = state->translation.z;
  }
}

// --- Model::RecordShape ---

// The command list the draws are recorded into (holds its address).
constexpr uint32_t kCommandList = 0x8296DD78;
// The blend table render state 3 takes its value from (by the material's
// blend index), and the constants put back after a draw that set its own
// (writable data).
constexpr uint32_t kBlendTable = 0x8264EC14;
constexpr uint32_t kDefaultVertexScale = 0x8264EA80;
constexpr uint32_t kDefaultPixelColor = 0x8264EA90;

// The commands' first words, and the constant registers' descriptors.
constexpr uint32_t kRenderStates = 1;
constexpr uint32_t kSamplerStates = 2;
constexpr uint32_t kBlendState = 3;
constexpr uint32_t kTextures = 4;
constexpr uint32_t kShaderConstants = 7;
constexpr uint32_t kVertexShader = 8;
constexpr uint32_t kPixelShader = 9;
constexpr uint32_t kChunkHeader = 10;
constexpr uint32_t kVertexStreams = 11;
constexpr uint32_t kIndexBuffer = 12;
constexpr uint32_t kDraw = 18;
constexpr uint32_t kSkinIndices = 22;
constexpr uint32_t kChunkEnd = 36;
constexpr uint32_t kVertexConstantsC9 = 0x00010905;   // Five vertex constants from c9.
constexpr uint32_t kPixelConstantsC63 = 0x01013F05;   // Five pixel constants from c63.
constexpr uint32_t kVertexConstantC86 = 0x00015601;   // One vertex constant, c86.
constexpr uint32_t kPixelConstantC76 = 0x01014C01;    // One pixel constant, c76.

// A 16-byte slot of the command list.
struct CommandSlot {
  u32_be words[4];
};
GUEST_SIZE(CommandSlot, 16);

// The recorder's frame (304 bytes): the model's draw hook gets the blend
// index's address and may change it.
constexpr uint32_t kRecordShapeFrame = 304;
struct RecordShapeFrame {
  uint8_t linkage[80];
  i32_be blend;
  u32_be vertex_scale_default;  // kDefaultVertexScale's address.
  u32_be pixel_color_default;   // kDefaultPixelColor's address.
  u32_be header;                // The chunk's header slot.
  f32_be texture_z[4];          // Pixel c67.
  Vector vertex_scale;          // Vertex c86, built here.
};
GUEST_OFFSET(RecordShapeFrame, blend, 80);
GUEST_OFFSET(RecordShapeFrame, texture_z, 96);
GUEST_OFFSET(RecordShapeFrame, vertex_scale, 112);

// Slots from the command list (its address read again each time).
CommandSlot* Slots(Guest& g, uint32_t count) {
  return At<CommandSlot>(g.Call(sub_82572638, uint32_t(*At<u32_be>(kCommandList)), count));  // CommandList::AllocateSlots
}
CommandSlot* Put(Guest& g, uint32_t w0, uint32_t w1) {
  CommandSlot* slot = Slots(g, 1);
  slot->words[0] = w0;
  slot->words[1] = w1;
  return slot;
}
CommandSlot* Put(Guest& g, uint32_t w0, uint32_t w1, uint32_t w2) {
  CommandSlot* slot = Put(g, w0, w1);
  slot->words[2] = w2;
  return slot;
}
void CopySlot(CommandSlot* slot, uint32_t from) { *slot = *At<CommandSlot>(from); }

// The three vertex streams (two for two-stream types), and a skinned mesh's
// skin indices.
void RecordStreams(Guest& g, Mesh* mesh, uint32_t stream_1, uint32_t stream_2) {
  const bool two_streams = mesh->stream_type_1 == 9 || mesh->stream_type_1 == 10 ||
                           mesh->stream_type_2 == 6;
  Put(g, kVertexStreams, two_streams ? 2 : 3, 0);
  CommandSlot* slot = Put(g, 0, AddressOf(&mesh->streams[0]), 0);
  slot->words[3] = mesh->stream_stride_0;
  slot = Put(g, 1, stream_1, 0);
  slot->words[3] = mesh->stream_stride_1;
  if (!two_streams) {
    slot = Put(g, 2, stream_2, 0);
    slot->words[3] = mesh->stream_stride_2;
  }
  if (mesh->skinned == 1) {
    const VertexWeights* weights = mesh->weights;
    if (weights->index_buffer != 0) {
      Put(g, kSkinIndices, 1, 0);
      CommandSlot* indices = Slots(g, 1);
      indices->words[1] = weights->index_buffer;
      indices->words[2] = 1;
    }
  }
}

// --- Model::RecordMaterials (a car part's shape) ---

// The car parts' blend table, the shared environment texture (0 when the
// model's XPR list gives it), and the two switches the render states read
// (writable data).
constexpr uint32_t kPartBlendTable = 0x825F1854;
constexpr uint32_t kSharedEnvironmentTexture = 0x8296DD50;
constexpr uint32_t kNoRenderState48 = 0x826B4DE4;
constexpr uint32_t kSwapCullModes = 0x826B4DE0;

// The shared pairs a part's material can use.
constexpr uint32_t kPairVinyl = 1000;
constexpr uint32_t kPairProviderTexture = 1001;
constexpr uint32_t kPairTextureAndVinyl = 1002;
constexpr uint32_t kPairReplaced = 1003;
constexpr uint32_t kPairVinylLike1000 = 1004;
constexpr uint32_t kPairForced = 1005;

// Material categories with a texture of their own source.
constexpr int32_t kCategoryPartTexture = 3;
constexpr int32_t kCategoryRenderTarget = 5;

// A texture of the material from the model's XPR list (slot 5).
uint32_t ListTexture(Guest& g, Model* model, uint32_t xpr, uint32_t id) {
  return g.CallVirtual(At<XPRList>(model->texture_provider.address), 5, xpr, id);  // XPRList::Texture
}

// The textures' two sampler states each (0 and 4), for `count` samplers.
void RecordSamplerStates(Guest& g, Material* material, int32_t count) {
  Put(g, kSamplerStates, uint32_t(count) * 2, 0);
  for (int32_t t = 0; t < count; t++) {
    Put(g, uint32_t(t), 0, *Element<u32_be>(AddressOf(material->sampler_state_0), t));
    Put(g, uint32_t(t), 4, *Element<u32_be>(AddressOf(material->sampler_state_4), t));
  }
}

// Every texture of the material: `texture(t)` gives texture t as its turn
// comes; then their sampler states.
template <typename Texture>
void RecordTextures(Guest& g, Material* material, Texture texture) {
  Put(g, kTextures, uint32_t(int32_t(material->texture_count)), 0);
  for (int32_t t = 0; t < material->texture_count; t++) {
    const uint32_t value = texture(t);
    Put(g, uint32_t(t), value);
  }
  RecordSamplerStates(g, material, material->texture_count);
}

// The textures of a material: its shared pair's (the vinyl sheet, the XPR
// list's, the replaced one), or for the model's own pair its category's
// (the part's texture, a render target) or the part's texture for every
// sampler, else the XPR list's.
void RecordPartTextures(Guest& g, PartModel* part, Material* material) {
  Model* model = &part->model;
  const uint32_t shared = material->shared_pair;
  if ((model->texture_provider.address == 0 || material->texture_count == 0) && shared == 0) {
    Put(g, kTextures, 4, 0);
    for (uint32_t t = 0; t < 4; t++) {
      Put(g, t, 0);
    }
    return;
  }
  auto list_texture = [&](int32_t t) {
    return ListTexture(g, model, *Element<u32_be>(AddressOf(material->texture_xprs), t),
                       *Element<u32_be>(AddressOf(material->texture_ids), t));
  };
  auto single = [&](uint32_t texture) {
    Put(g, 0, texture);
    RecordSamplerStates(g, material, 1);
  };
  if (shared != 0) {
    switch (shared) {
      case kPairVinyl:
      case kPairVinylLike1000:
        Put(g, kTextures, 1, 0);
        single(part->vinyl_textures[0]);
        return;
      case kPairProviderTexture: {
        const uint32_t texture = list_texture(0);
        Put(g, kTextures, 1, 0);
        single(texture);
        return;
      }
      case kPairTextureAndVinyl: {
        const uint32_t texture = list_texture(0);
        Put(g, kTextures, 2, 0);
        Put(g, 0, texture);
        Put(g, 1, part->vinyl_textures[0]);
        RecordSamplerStates(g, material, 2);
        return;
      }
      case kPairReplaced:
        // The texture whose id the part replaces comes from the part.
        if (part->unknown_452[2] != 0 && int32_t(material->texture_ids[0]) == part->unknown_436) {
          RecordTextures(g, material, [&](int32_t t) {
            const uint32_t id = *Element<u32_be>(AddressOf(material->texture_ids), t);
            if (int32_t(id) == part->unknown_436) {
              return uint32_t(part->unknown_452[2]);
            }
            return ListTexture(g, model, *Element<u32_be>(AddressOf(material->texture_xprs), t), id);
          });
        } else {
          Put(g, kTextures, 1, 0);
          single(list_texture(0));
        }
        return;
      default:
        return;  // The other shared pairs have no textures.
    }
  }
  const int32_t category = material->category;
  if (category == kCategoryPartTexture) {
    if (part->unknown_452[0] != 0 && material->texture_count == 1) {
      Put(g, kTextures, 1, 0);
      single(part->unknown_452[0]);
      return;
    }
  } else if (category == kCategoryRenderTarget) {
    if (material->texture_count == 1) {
      Put(g, kTextures, 1, 0);
      CommandSlot* slot = Slots(g, 1);
      slot->words[0] = 0;
      slot->words[1] = g.Call(sub_8209F8A0, 0u);  // CameraPasses::RenderTarget
      RecordSamplerStates(g, material, 1);
      return;
    }
  } else if (part->unknown_432 != 0 && part->unknown_452[1] != 0) {
    RecordTextures(g, material, [&](int32_t) { return uint32_t(part->unknown_452[1]); });
    return;
  } else if (part->unknown_452[2] != 0 && int32_t(material->texture_ids[0]) == part->unknown_436) {
    RecordTextures(g, material, [&](int32_t) { return uint32_t(part->unknown_452[2]); });
    return;
  }
  RecordTextures(g, material, list_texture);
}

// Model::RecordMaterials: a car part's shape for one material group,
// recorded as a chunk: the vertex declaration and streams, then for each
// material its shaders (with `set_shaders`; shared pair 1005 forced by the
// part's +440, 1004 drawn with 1000's), blend, vertex c9-c13 and pixel
// c63-c67, render states, textures, the environment texture and its
// sampler's states, and the draw. Returns the chunk's header slot, or 0.
uint32_t ModelRecordMaterials(Guest& g, PartModel* part, uint32_t shape, int32_t group,
                              uint32_t set_shaders) {
  Model* model = &part->model;
  if (MaterialCount(model, shape, group) == 0) {
    return 0;
  }

  Mesh* mesh = Element<Mesh>(model->meshes, shape);
  uint32_t stream_1 = AddressOf(&mesh->streams[1]);
  uint32_t stream_2 = AddressOf(&mesh->streams[2]);
  if (mesh->skinned == 1) {
    stream_1 = mesh->skin_stream_1;
    stream_2 = mesh->skin_stream_2;
  }
  g.Call(sub_82572568, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::BeginChunk
  CommandSlot* header = Slots(g, 1);
  header->words[0] = kChunkHeader;
  header->words[1] = 1;
  header->words[2] = 0;
  CommandSlot* declaration = Slots(g, 1);
  declaration->words[0] =
      VertexDeclaration(g, int32_t(uint32_t(mesh->stream_type_0)), int32_t(uint32_t(mesh->stream_type_1)),
                        int32_t(uint32_t(mesh->stream_type_2)));
  RecordStreams(g, mesh, stream_1, stream_2);

  const uint32_t first = AddressOf(Materials(model, shape)->InGroup(0, group == 0 ? 0 : 1));
  for (int32_t index = 0; index < int32_t(MaterialCount(model, shape, group)); index++) {
    Material* material = Element<Material>(first, index);
    // Pixel c67: the first four textures' z's (any further ones go unused).
    f32_be texture_z[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    if (set_shaders != 0) {
      const uint32_t shared = material->shared_pair;
      if (shared == 0 && part->unknown_440 == 0) {
        ShaderPairEntry* pair = Element<ShaderPairEntry>(model->shader_pairs, material->shader_pair);
        Put(g, kVertexShader, 1, 0);
        Slots(g, 1)->words[0] = pair->vertex;
        Put(g, kPixelShader, 1, 0);
        Slots(g, 1)->words[0] =
            Element<ShaderPairEntry>(model->shader_pairs, material->shader_pair)->pixel;
      } else {
        // The pair 1005 when the part forces it, 1000 for 1004, else its own.
        const bool forced = part->unknown_440 != 0;
        const bool by_field = !forced && shared != kPairVinylLike1000;
        const uint32_t pair = forced ? kPairForced : shared == kPairVinylLike1000 ? kPairVinyl : 0;
        Put(g, kVertexShader, 1, 0);
        CommandSlot* slot = Slots(g, 1);
        slot->words[0] = SharedShader(kSharedShaders, by_field ? uint32_t(material->shared_pair) : pair);
        Put(g, kPixelShader, 1, 0);
        slot = Slots(g, 1);
        slot->words[0] = SharedShader(kSharedShaders + 4, by_field ? uint32_t(material->shared_pair) : pair);
      }
    }

    Put(g, kBlendState, 1, 0);
    CommandSlot* blend = Slots(g, 1);
    blend->words[0] = 0;
    blend->words[1] = *Element<u32_be>(kPartBlendTable, uint32_t(int32_t(material->blend)));

    // Vertex c9-c13: four (0, 0, 0, 1) with the materials' z's kept, then
    // each texture's color as RecordShape's, then the material's color.
    Put(g, kShaderConstants, 5, kVertexConstantsC9);
    CommandSlot* constants = Slots(g, 5);
    for (int32_t t = 0; t < 4; t++) {
      Vector* color = Element<Vector>(AddressOf(constants), t);
      color->x = 0.0f;
      color->y = 0.0f;
      color->z = 0.0f;
      color->w = 1.0f;
      texture_z[t] = material->texture_color_z[t];
    }
    for (int32_t t = 0; t < material->texture_count; t++) {
      Vector* color = Element<Vector>(AddressOf(constants), t);
      const int32_t id = *Element<i32_be>(AddressOf(material->texture_colors), t);
      float z;
      if (id != -1) {
        color->x = Element<Vector>(model->tracks[0].values, id)->x;
        color->y = Element<Vector>(model->tracks[0].values, id)->y;
        z = Element<Vector>(model->tracks[0].values, id)->z;
      } else {
        color->x = 0.0f;
        color->y = 0.0f;
        color->z = 0.0f;
        color->w = 1.0f;
        z = *Element<f32_be>(AddressOf(material->texture_color_z), t);
      }
      if (t < 4) {
        texture_z[t] = z;
      }
    }
    *Element<Vector>(AddressOf(constants), 4) = material->vertex_color;

    // Pixel c63-c67.
    Put(g, kShaderConstants, 5, kPixelConstantsC63);
    CommandSlot* pixel = Slots(g, 5);
    *Element<Vector>(AddressOf(pixel), 0) = material->color_64;
    *Element<Vector>(AddressOf(pixel), 1) = material->color_80;
    Vector* c65 = Element<Vector>(AddressOf(pixel), 2);
    *c65 = material->color_16;
    *Element<Vector>(AddressOf(pixel), 3) = material->color_32;
    c65->w = material->color_80.z;
    Vector* c67 = Element<Vector>(AddressOf(pixel), 4);
    c67->x = texture_z[0];
    c67->y = texture_z[1];
    c67->z = texture_z[2];
    c67->w = texture_z[3];

    // Render states: 96 (group 0), 48, 100 and the cull mode (2 and 6
    // swapped when the switch is on).
    if (group == 0) {
      Put(g, kRenderStates, 1, 0);
      Put(g, 96, material->has_alpha != 0 ? 1 : 0);
    }
    Put(g, kRenderStates, 3, 0);
    Put(g, 48, *At<u32_be>(kNoRenderState48) != 0 ? 0 : 1);
    Put(g, 100, material->render_state_100);
    const uint32_t cull = material->cull_mode;
    if (*At<u32_be>(kSwapCullModes) != 0 && (cull == 2 || cull == 6)) {
      CommandSlot* slot = Slots(g, 1);
      slot->words[0] = 56;
      slot->words[1] = material->cull_mode == 2 ? 6 : 2;
    } else {
      Put(g, 56, material->cull_mode);
    }

    RecordPartTextures(g, part, material);

    // The environment texture on sampler 6, with its sampler states.
    const int32_t category = material->category;
    if ((material->flags & 4) || material->shared_pair == 0 || category == 1 || category == 2 ||
        category == 7 || category == 9 || category == 10) {
      uint32_t texture = *At<u32_be>(kSharedEnvironmentTexture);
      if (texture == 0) {
        texture = g.CallVirtual(At<XPRList>(model->texture_provider.address), 6, int32_t(material->environment_xpr),
                                int32_t(material->environment_texture));  // XPRList::EnvironmentTexture
      }
      Put(g, kTextures, 1, 0);
      Put(g, 6, texture);
      Put(g, kSamplerStates, 3, 0);
      Put(g, 6, 16, 4);
      Put(g, 6, 20, 4);
      Put(g, 6, 36, 3);
    }

    // The draw.
    Put(g, kIndexBuffer, 1, 0);
    Slots(g, 1)->words[0] = AddressOf(&material->index_buffer);
    Put(g, kDraw, 1, 0);
    CommandSlot* draw = Slots(g, 1);
    draw->words[0] = material->draw_word_0;
    draw->words[1] = 0;  // Two halfwords, both 0.
    draw->words[2] = material->draw_word_8;
  }
  Put(g, kChunkEnd, 0, 0);
  g.Call(sub_825725E8, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::EndChunk
  return AddressOf(header);
}

}  // namespace

// --- The functions other files of the group call (model.h) ---

namespace game {

// Its blocks put on the pending frees (the data with the attributes it was
// made with), and every word cleared.
void XprFree(Guest& g, XPR* xpr) {
  if (xpr->header != 0) {
    FreeLater(g, xpr->header);
  }
  xpr->header = 0;
  xpr->header_size = 0;
  if (xpr->data != 0) {
    FreePhysicalLater(g, xpr->data, 0x807F0000);
  }
  xpr->data = 0;
  xpr->data_size = 0;
  xpr->records = 0;
  xpr->record_count = 0;
}

void XprConstruct(XPR* xpr) { *xpr = XPR{}; }

// The declaration for three stream types: the first and third name their
// kinds, the second picks the table.
uint32_t VertexDeclaration(Guest& g, int32_t first, int32_t second, int32_t third) {
  const uint32_t kind = *Element<u32_be>(kFirstStreamKinds, first);
  const int32_t third_kind = int32_t(*Element<u32_be>(kThirdStreamKinds, third));
  // The declarations are in rows of seven (by the first stream's kind),
  // from two rows before kDeclarations; the row by the second and third
  // streams' kinds.
  constexpr uint32_t kRowBytes = 7 * 4;
  constexpr uint32_t kFirstRow = kDeclarations - 2 * kRowBytes;
  int32_t row;
  if (second == 9) {
    row = 1;
  } else if (second == 10) {
    row = 0;
  } else if (second == 5 && first <= 4 && third_kind >= 0 && third_kind <= 2) {
    row = 10 - third_kind;
  } else if (second == 5 && first > 4 && third_kind >= 0 && third_kind <= 2) {
    row = 7 - third_kind;
  } else if (second == 11 && third_kind >= 0 && third_kind <= 2) {
    row = 4 - third_kind;
  } else {
    // No declaration for these streams: the first of row 9.
    g.Call(sub_82586A00, kDeclarationError);  // DebugPrint
    return *Element<u32_be>(kFirstRow + 9 * kRowBytes, 0);
  }
  return *Element<u32_be>(kFirstRow + uint32_t(row) * kRowBytes, kind);
}

uint32_t SharedVertexShader(uint32_t pair) { return SharedShader(kSharedShaders, pair); }
uint32_t SharedPixelShader(uint32_t pair) { return SharedShader(kSharedShaders + 4, pair); }

// `count` slots (-1: 64), all empty.
void XprListConstruct(Guest& g, XPRList* list, int32_t count) {
  list->vtable = XPRList::kVtable;
  list->items.first = 0;
  list->items.last = 0;
  list->items.end = 0;
  const uint32_t slots = count == -1 ? 64 : uint32_t(count);
  PointersReserve(g, &list->items, slots);
  PointersAssign(g, &list->items, slots, 0);
}

// Model::Model: its XPR cleared, its XPR list (64 slots), skeleton and track
// sets made, its other fields cleared.
Model* ModelConstruct(Guest& g, Model* model) {
  model->vtable = kModelVtable;
  model->xpr = XPR{};
  XprListConstruct(g, &model->xpr_list, -1);
  SkeletonConstruct(g, model->SkeletonObject());
  for (TrackSet& tracks : model->tracks) {
    TrackSetConstruct(&tracks);
  }
  model->header_copy = 0;
  model->vertex_data = 0;
  model->shader_pairs = 0;
  model->header = 0;
  model->meshes = 0;
  model->texture_provider = nullptr;
  model->header_count_0 = 0;
  model->header_count_1 = 0;
  model->shape_count = 0;
  model->header_count_3 = 0;
  model->shader_pair_count = 0;
  return model;
}

// Model::~Model: what it loaded released, then its members destroyed, the
// last first.
void ModelDestruct(Guest& g, Model* model) {
  model->vtable = kModelVtable;
  ModelRelease(g, model);
  TrackSetReleaseArray(g, &model->tracks[4]);
  TrackSetReleaseArray(g, &model->tracks[3]);
  TrackSetRelease(g, &model->tracks[2]);
  TrackSetRelease(g, &model->tracks[1]);
  TrackSetReleaseArray(g, &model->tracks[0]);
  SkeletonDestruct(g, model->SkeletonObject());
  XprListDestruct(g, &model->xpr_list);
  XprFree(g, &model->xpr);  // XPR::~XPR
}

// Its blocks and shader pairs put on the pending frees, its words cleared,
// and its XPR released.
void ModelRelease(Guest& g, Model* model) {
  if (model->header_copy != 0) {
    FreeLater(g, model->header_copy);
  }
  if (model->vertex_data != 0) {
    FreeLater(g, model->vertex_data);
  }
  for (uint32_t i = 0; i < model->shader_pair_count; i++) {
    ShaderPairEntry* pair = Element<ShaderPairEntry>(model->shader_pairs, i);
    for (uint32_t block : {uint32_t(pair->vertex), uint32_t(pair->pixel), uint32_t(pair->vertex_code),
                           uint32_t(pair->pixel_code)}) {
      if (block != 0) {
        FreeLater(g, block);
      }
    }
  }
  if (model->shader_pairs != 0) {
    FreeLater(g, model->shader_pairs);
  }
  model->header_copy = 0;
  model->vertex_data = 0;
  model->shader_pairs = 0;
  model->shader_pair_count = 0;
  model->header = 0;
  model->meshes = 0;
  model->texture_provider = nullptr;
  model->header_count_0 = 0;
  model->shape_count = 0;
  model->header_count_1 = 0;
  model->header_count_3 = 0;
  XprFree(g, &model->xpr);
}

// Model::Load: what it had released, its mesh file parsed, the XPR file
// (when given) made its XPR and put in its XPR list's slot 0, then its
// weights rounded.
void ModelLoad(Guest& g, Model* model, uint32_t file, uint32_t xpr_file) {
  ModelRelease(g, model);
  MeshParse(g, model, file);
  if (xpr_file != 0) {
    g.Call(sub_8257C880, &model->xpr, xpr_file);           // XPR::Create
    g.CallVirtual(&model->xpr_list, 4, 0u, &model->xpr);  // XPRList::Set
    model->texture_provider = &model->xpr_list;
  }
  NormalizeWeights(model);
}

// Model::MaterialCount: how many materials a shape's group has (group 0, or
// any other number for group 1).
uint32_t MaterialCount(Model* model, uint32_t shape, int32_t group) {
  return Materials(model, shape)->group_counts[group == 0 ? 0 : 1];
}

// Model::Material: material `index` of a shape's group.
Material* ModelMaterial(Model* model, uint32_t shape, uint32_t index, int32_t group) {
  return Materials(model, shape)->InGroup(index, group == 0 ? 0 : 1);
}

// Each node's matrix made from its parent's, in node order: the root's by
// the skeleton's matrix first; a plain node's by its parent's, with the
// translation of the node it names; nodes of kind 8 and 16 by their own
// functions; then the bones' by their parents' again.
void SkeletonUpdateNodeMatrices(Guest& g, Skeleton* skeleton) {
  MultiplyInto(g, skeleton->MatrixAt(skeleton->root_index), &skeleton->matrix);
  const uint32_t matrices = skeleton->matrices;
  const uint32_t nodes = skeleton->node_file.nodes.address;
  for (int32_t i = 0; i < skeleton->node_file.file->node_count; i++) {
    Matrix* matrix = Element<Matrix>(matrices, i);
    NodeRecord* node = Element<NodeRecord>(nodes, i);
    const int32_t kind = node->kind;
    if (kind <= NodeRecord::kLastPlainKind) {
      if (node->parent != -1) {
        MultiplyInto(g, matrix, skeleton->MatrixAt(node->parent));
      }
      const int32_t source = skeleton->Node(i)->translation_source;
      if (source != -1) {
        matrix->position = skeleton->MatrixAt(source)->position;
      }
    } else if (kind == NodeRecord::kKind8) {
      g.Call(sub_82580E48, skeleton, i);  // Skeleton::Kind8Matrix
    } else if (kind == NodeRecord::kKind16) {
      g.Call(sub_82581540, skeleton, i);  // Skeleton::Kind16Matrix
    }
  }

  const uint32_t first_bone = skeleton->node_file.file->first_bone;
  const uint32_t bone_matrices = skeleton->matrices;
  const uint32_t bone_nodes = skeleton->node_file.nodes.address;
  for (int32_t i = 0; i < skeleton->node_file.file->bone_count; i++) {
    Matrix* bone = Element<Matrix>(bone_matrices, first_bone + i);
    NodeRecord* bone_node = Element<NodeRecord>(bone_nodes, first_bone + i);
    if ((bone_node->kind & NodeRecord::kBone) && bone_node->parent != -1) {
      MultiplyInto(g, bone, skeleton->MatrixAt(bone_node->parent));
    }
  }
}

// Model::RecordShape: a shape's draws for one material group, recorded as
// a chunk: the vertex declaration, streams and sampler state 28; then for
// each material its shaders (with `set_shaders`), its vertex and pixel
// constants, blend and render states, its textures, the optional constants
// `vertex_scale` (c86) and `pixel_color` (c76) and the draw. Returns the
// chunk's header slot, or 0 when the group has no materials. The frame
// stays in guest memory: the model's draw hook gets the blend's address.
uint32_t ModelRecordShape(Guest& g, Model* model, uint32_t shape, int32_t group,
                          uint32_t set_shaders, uint32_t pixel_color, uint32_t vertex_scale) {
  Mesh* mesh = Element<Mesh>(model->meshes, shape);
  if (MaterialCount(model, shape, group) == 0) {
    return 0;
  }
  guest::StackFrame stack(g.ctx, kRecordShapeFrame);
  RecordShapeFrame* frame = At<RecordShapeFrame>(stack.Local(0));

  uint32_t stream_1 = AddressOf(&mesh->streams[1]);
  uint32_t stream_2 = AddressOf(&mesh->streams[2]);
  if (mesh->skinned == 1) {
    stream_1 = mesh->skin_stream_1;
    stream_2 = mesh->skin_stream_2;
  }
  g.Call(sub_82572568, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::BeginChunk
  CommandSlot* header = Slots(g, 1);
  frame->header = AddressOf(header);
  header->words[0] = kChunkHeader;
  header->words[1] = 1;
  header->words[2] = 0;
  CommandSlot* declaration = Slots(g, 1);
  declaration->words[0] =
      VertexDeclaration(g, int32_t(uint32_t(mesh->stream_type_0)), int32_t(uint32_t(mesh->stream_type_1)),
                        int32_t(uint32_t(mesh->stream_type_2)));
  RecordStreams(g, mesh, stream_1, stream_2);
  Put(g, kSamplerStates, 4, 0);
  for (uint32_t sampler = 0; sampler < 4; sampler++) {
    Put(g, sampler, 28, mesh->sampler_28_value);
  }

  const uint32_t first = AddressOf(Materials(model, shape)->InGroup(0, group == 0 ? 0 : 1));
  if (int32_t(MaterialCount(model, shape, group)) <= 0) {
    Put(g, kChunkEnd, 0, 0);
    g.Call(sub_825725E8, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::EndChunk
    return frame->header;
  }
  frame->pixel_color_default = kDefaultPixelColor;
  frame->vertex_scale_default = kDefaultVertexScale;
  for (int32_t index = 0; index < int32_t(MaterialCount(model, shape, group)); index++) {
    Material* material = Element<Material>(first, index);
    for (f32_be& z : frame->texture_z) {
      z = 1.0f;
    }
    frame->blend = material->blend;

    if (set_shaders != 0) {
      if (material->shared_pair == 0) {
        ShaderPairEntry* pair = Element<ShaderPairEntry>(model->shader_pairs, material->shader_pair);
        Put(g, kVertexShader, 1, 0);
        Slots(g, 1)->words[0] = pair->vertex;
        Put(g, kPixelShader, 1, 0);
        Slots(g, 1)->words[0] =
            Element<ShaderPairEntry>(model->shader_pairs, material->shader_pair)->pixel;
      } else {
        Put(g, kVertexShader, 1, 0);
        CommandSlot* slot = Slots(g, 1);
        slot->words[0] = SharedShader(kSharedShaders, material->shared_pair);
        Put(g, kPixelShader, 1, 0);
        slot = Slots(g, 1);
        slot->words[0] = SharedShader(kSharedShaders + 4, material->shared_pair);
      }
    }

    // Vertex c9-c13: each texture's color (x, y from the model's float4
    // track value, its z kept for the pixel constants) or (0, 0, 0, 1),
    // then the material's own color.
    Put(g, kShaderConstants, 5, kVertexConstantsC9);
    CommandSlot* constants = Slots(g, 5);
    for (int32_t t = 0; t < material->texture_count; t++) {
      Vector* color = Element<Vector>(AddressOf(constants), t);
      f32_be* z = Element<f32_be>(AddressOf(frame->texture_z), t);
      const int32_t id = *Element<i32_be>(AddressOf(material->texture_colors), t);
      if (id != -1) {
        color->x = Element<Vector>(model->tracks[0].values, id)->x;
        color->y = Element<Vector>(model->tracks[0].values, id)->y;
        *z = Element<Vector>(model->tracks[0].values, id)->z;
      } else {
        color->x = 0.0f;
        color->y = 0.0f;
        color->z = 0.0f;
        color->w = 1.0f;
        *z = *Element<f32_be>(AddressOf(material->texture_color_z), t);
      }
    }
    Vector* vertex_color = Element<Vector>(AddressOf(constants), 4);
    *vertex_color = material->vertex_color;
    if (material->animation != -1) {
      g.Call(sub_82586878, &model->tracks[3], vertex_color, int32_t(material->animation));  // TrackSet::ApplyMaterial
    }
    g.CallVirtual(model, 1, shape, index, vertex_color, &frame->blend);  // Model::OnRecordMaterial

    // Pixel c63-c67.
    Put(g, kShaderConstants, 5, kPixelConstantsC63);
    CommandSlot* pixel = Slots(g, 5);
    const Vector texture_z = *Element<Vector>(AddressOf(frame->texture_z), 0);
    *Element<Vector>(AddressOf(pixel), 0) = material->color_64;
    *Element<Vector>(AddressOf(pixel), 1) = material->color_80;
    Vector* c65 = Element<Vector>(AddressOf(pixel), 2);
    *c65 = material->color_16;
    c65->w = material->color_80.z;
    *Element<Vector>(AddressOf(pixel), 3) = material->color_32;
    *Element<Vector>(AddressOf(pixel), 4) = texture_z;

    Put(g, kBlendState, 1, 0);
    Put(g, 0, *Element<u32_be>(kBlendTable, uint32_t(int32_t(frame->blend))));
    if (group == 0) {
      Put(g, kRenderStates, 2, 0);
      if (material->has_alpha != 0) {
        Put(g, 96, 1);
        Put(g, 100, 64);
      } else {
        Put(g, 96, 0);
        Put(g, 100, material->render_state_100);
      }
    }
    Put(g, kRenderStates, 1, 0);
    Put(g, 56, material->cull_mode);

    // Its textures from the model's XPR list, and their sampler states.
    const uint32_t textures = model->texture_provider.address;
    if (textures != 0 && material->texture_count != 0) {
      Put(g, kTextures, uint32_t(int32_t(material->texture_count)), 0);
      for (int32_t t = 0; t < material->texture_count; t++) {
        const uint32_t texture = g.CallVirtual(
            At<XPRList>(model->texture_provider.address), 5, uint32_t(*Element<u32_be>(AddressOf(material->texture_xprs), t)),
            uint32_t(*Element<u32_be>(AddressOf(material->texture_ids), t)));  // XPRList::Texture
        Put(g, uint32_t(t), texture);
      }
      Put(g, kSamplerStates, uint32_t(int32_t(material->texture_count)) * 2, 0);
      for (int32_t t = 0; t < material->texture_count; t++) {
        Put(g, uint32_t(t), 0, *Element<u32_be>(AddressOf(material->sampler_state_0), t));
        Put(g, uint32_t(t), 4, *Element<u32_be>(AddressOf(material->sampler_state_4), t));
      }
    } else {
      Put(g, kTextures, 4, 0);
      for (uint32_t t = 0; t < 4; t++) {
        Put(g, t, 0);
      }
    }
    // The environment textures (two kinds, by the flags).
    const uint32_t environment_slots[2] = {6, 5};
    const uint32_t environment_flags[2] = {2, 4};
    for (int e = 0; e < 2; e++) {
      if ((material->flags & environment_flags[e]) && material->environment_xpr != -1 &&
          material->environment_texture != -1) {
        const uint32_t texture = g.CallVirtual(At<XPRList>(model->texture_provider.address), environment_slots[e],
                                               int32_t(material->environment_xpr),
                                               int32_t(material->environment_texture));
        Put(g, kTextures, 1, 0);
        Put(g, 6, texture);
      }
    }

    // The optional constants: c86 (the scale's first two and 1 / their
    // span) and c76.
    if (vertex_scale != 0) {
      Put(g, kShaderConstants, 1, kVertexConstantC86);
      CommandSlot* slot = Slots(g, 1);
      const float low = At<f32_be>(vertex_scale)[0];
      const float high = At<f32_be>(vertex_scale)[1];
      const float span = float(double(high) - double(low));
      frame->vertex_scale.w = 0.0f;
      frame->vertex_scale.x = low;
      frame->vertex_scale.y = high;
      frame->vertex_scale.z = float(1.0 / double(span));
      CopySlot(slot, AddressOf(&frame->vertex_scale));
    }
    if (pixel_color != 0) {
      Put(g, kShaderConstants, 1, kPixelConstantC76);
      CopySlot(Slots(g, 1), pixel_color);
    }

    // The draw.
    Put(g, kIndexBuffer, 1, 0);
    Slots(g, 1)->words[0] = AddressOf(&material->index_buffer);
    Put(g, kDraw, 1, 0);
    CommandSlot* draw = Slots(g, 1);
    draw->words[1] = 0;  // Two halfwords, both 0.
    draw->words[0] = material->draw_word_0;
    draw->words[2] = material->draw_word_8;

    // The optional constants put back.
    if (vertex_scale != 0) {
      Put(g, kShaderConstants, 1, kVertexConstantC86);
      CopySlot(Slots(g, 1), frame->vertex_scale_default);
    }
    if (pixel_color != 0) {
      Put(g, kShaderConstants, 1, kPixelConstantC76);
      CopySlot(Slots(g, 1), frame->pixel_color_default);
    }
  }
  Put(g, kChunkEnd, 0, 0);
  g.Call(sub_825725E8, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::EndChunk
  return frame->header;
}

}  // namespace game

#define SBX_MODELS_FALLBACK(name)   \
  REX_FUNC_PROLOGUE();              \
  if (!On()) {                      \
    __imp__##name(ctx, base);       \
    return;                         \
  }                                 \
  Guest g(ctx, base);

// --- The shaders ---

// Shaders::LoadAll: every shader pair loaded, then the shared ones set.
extern "C" REX_FUNC(sub_820A0A88) {
  SBX_MODELS_FALLBACK(sub_820A0A88)
  *At<u32_be>(kShadersLoaded) = 0;
  for (const ShaderRun& run : kShaderRuns) {
    for (uint32_t i = 0; i < run.count; i++) {
      ShadersLoadPair(g, run.first_file + 2 * i, run.first_slot + i);
    }
  }
  At<u32_be>(kSharedShaders)[0] = kSharedVertexShaders;
  At<u32_be>(kSharedShaders)[1] = kSharedPixelShaders;
}

// Shaders::LoadPair: r3 the vertex shader's file id, r4 the pair.
extern "C" REX_FUNC(sub_820A09D8) {
  SBX_MODELS_FALLBACK(sub_820A09D8)
  ShadersLoadPair(g, ctx.r3.u32, ctx.r4.u32);
}

// SetSharedShaders: r3 and r4 the shared shaders. Returns r3.
extern "C" REX_FUNC(sub_8256F240) {
  SBX_MODELS_FALLBACK(sub_8256F240)
  At<u32_be>(kSharedShaders)[0] = ctx.r3.u32;
  At<u32_be>(kSharedShaders)[1] = ctx.r4.u32;
}

// SharedVertexShader and SharedPixelShader: r3 a pair; its shader, or 0
// before they're set.
extern "C" REX_FUNC(sub_8256F258) {
  SBX_MODELS_FALLBACK(sub_8256F258)
  ctx.r3.u64 = game::SharedVertexShader(ctx.r3.u32);
}

extern "C" REX_FUNC(sub_8256F280) {
  SBX_MODELS_FALLBACK(sub_8256F280)
  ctx.r3.u64 = game::SharedPixelShader(ctx.r3.u32);
}

// VertexDeclaration: r3, r4, r5 a shape's three stream types. Its vertex
// declaration.
extern "C" REX_FUNC(sub_8256F2A8) {
  SBX_MODELS_FALLBACK(sub_8256F2A8)
  ctx.r3.u64 = game::VertexDeclaration(g, ctx.r3.s32, ctx.r4.s32, ctx.r5.s32);
}

// --- Part models ---

// PartModel::PartModel: r3 the part. Returns it.
extern "C" REX_FUNC(sub_820CE6E8) {
  SBX_MODELS_FALLBACK(sub_820CE6E8)
  PartModel* part = At<PartModel>(ctx.r3.u32);
  game::ModelConstruct(g, &part->model);
  part->model.vtable = kPartModelVtable;
  ClearPartModel(part);
  ctx.r3.u64 = AddressOf(part);
}

// PartModel::~PartModel: r3 the part.
extern "C" REX_FUNC(sub_820CE750) {
  SBX_MODELS_FALLBACK(sub_820CE750)
  PartModel* part = At<PartModel>(ctx.r3.u32);
  part->model.vtable = kPartModelVtable;
  ClearPartModel(part);
  game::ModelDestruct(g, &part->model);
}

// PartModel::Clear: r3 the part. Returns it.
extern "C" REX_FUNC(sub_820CE788) {
  SBX_MODELS_FALLBACK(sub_820CE788)
  ClearPartModel(At<PartModel>(ctx.r3.u32));
}

// Model::SetVinylTextures: r3 the part, r4 and r5 the textures. Returns r3.
extern "C" REX_FUNC(sub_820CE7B8) {
  SBX_MODELS_FALLBACK(sub_820CE7B8)
  PartModel* part = At<PartModel>(ctx.r3.u32);
  part->vinyl_textures[0] = ctx.r4.u32;
  part->vinyl_textures[1] = ctx.r5.u32;
}

// --- The memory blocks ---

// ScopedLock::ScopedLock: r3 the lock holder, r4 the lock, taken. Returns
// r3.
extern "C" REX_FUNC(sub_82574180) {
  SBX_MODELS_FALLBACK(sub_82574180)
  ScopedLock* holder = At<ScopedLock>(ctx.r3.u32);
  holder->lock = ctx.r4.u32;
  g.Call(sub_82574178, holder->lock);  // Lock
  ctx.r3.u64 = AddressOf(holder);
}

// ScopedLock::~ScopedLock: r3 the holder; its lock let go.
extern "C" REX_FUNC(sub_825741B8) {
  SBX_MODELS_FALLBACK(sub_825741B8)
  g.Call(sub_8258C4A8, At<ScopedLock>(ctx.r3.u32)->lock);  // Unlock
}

// Memory::AllocPhysical: r3 bytes, r4 XMemAlloc's attributes. The block
// after its header, or 0.
extern "C" REX_FUNC(sub_82573CC8) {
  SBX_MODELS_FALLBACK(sub_82573CC8)
  const uint32_t bytes = ctx.r3.u32;
  const uint32_t attributes = ctx.r4.u32;
  ctx.r3.u64 = AllocPhysical(g, bytes, attributes);
}

// Memory::FreeLater: r3 a block; put on the pending frees.
extern "C" REX_FUNC(sub_82574010) {
  SBX_MODELS_FALLBACK(sub_82574010)
  FreeLater(g, ctx.r3.u32);
}

// Memory::FreePhysicalLater: r3 a physical block, r4 its attributes.
extern "C" REX_FUNC(sub_82574098) {
  SBX_MODELS_FALLBACK(sub_82574098)
  const uint32_t block = ctx.r3.u32;
  const uint32_t attributes = ctx.r4.u32;
  FreePhysicalLater(g, block, attributes);
}

// PendingFrees::Insert: r3 the list, r4 the node to insert before, r5 the
// value's address.
extern "C" REX_FUNC(sub_82573F98) {
  SBX_MODELS_FALLBACK(sub_82573F98)
  PtrList* list = At<PtrList>(ctx.r3.u32);
  PtrListNode* where = At<PtrListNode>(ctx.r4.u32);
  const uint32_t value = *At<u32_be>(ctx.r5.u32);
  PendingFreesInsert(g, list, where, value);
}

// PendingFrees::IncSize: r3 the list, r4 how many more.
extern "C" REX_FUNC(sub_82573EF8) {
  SBX_MODELS_FALLBACK(sub_82573EF8)
  PtrList* list = At<PtrList>(ctx.r3.u32);
  const uint32_t count = ctx.r4.u32;
  PendingFreesIncSize(g, list, count);
}

// --- XPRs ---

// XPR::XPR: r3 the XPR, empty. Returns it.
extern "C" REX_FUNC(sub_8257C780) {
  SBX_MODELS_FALLBACK(sub_8257C780)
  game::XprConstruct(At<XPR>(ctx.r3.u32));
}

// XPR::~XPR and XPR::Release: r3 the XPR; its blocks freed.
extern "C" REX_FUNC(sub_8257CA60) {
  SBX_MODELS_FALLBACK(sub_8257CA60)
  game::XprFree(g, At<XPR>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_8257C9C0) {
  SBX_MODELS_FALLBACK(sub_8257C9C0)
  game::XprFree(g, At<XPR>(ctx.r3.u32));
}

// XPR::FindTexture: r3 the XPR, r4 a name. That resource, or 0.
extern "C" REX_FUNC(sub_8257C7A0) {
  SBX_MODELS_FALLBACK(sub_8257C7A0)
  XPR* xpr = At<XPR>(ctx.r3.u32);
  const uint32_t name = ctx.r4.u32;
  ctx.r3.u64 = XprFindTexture(g, xpr, name);
}

// Resource::Relocate: r3 a resource's header, r4 its data's address.
extern "C" REX_FUNC(sub_82354B30) {
  SBX_MODELS_FALLBACK(sub_82354B30)
  RelocateResource(g, At<GpuResource>(ctx.r3.u32), ctx.r4.u32);
}

// TextureHeader::Relocate: r3 a texture header, r4 its data's address, r5
// its mips'.
extern "C" REX_FUNC(sub_82354AC8) {
  SBX_MODELS_FALLBACK(sub_82354AC8)
  RelocateTexture(At<GpuResource>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// TextureHeader::MipLevelCount: r3 a texture header.
extern "C" REX_FUNC(sub_822A3150) {
  SBX_MODELS_FALLBACK(sub_822A3150)
  ctx.r3.u64 = MipLevelCount(At<GpuResource>(ctx.r3.u32));
}

// --- XPR lists ---

// XPRList::XPRList: r3 the list, r4 its slot count (-1: 64). Returns it.
extern "C" REX_FUNC(sub_8257A150) {
  SBX_MODELS_FALLBACK(sub_8257A150)
  XPRList* list = At<XPRList>(ctx.r3.u32);
  game::XprListConstruct(g, list, ctx.r4.s32);
  ctx.r3.u64 = AddressOf(list);
}

// XPRList::~XPRList: r3 the list.
extern "C" REX_FUNC(sub_82579948) {
  SBX_MODELS_FALLBACK(sub_82579948)
  XprListDestruct(g, At<XPRList>(ctx.r3.u32));
}

// XPRList::Get (vtable slot 3): r3 the list, r4 a slot. Its XPR, or 0.
extern "C" REX_FUNC(sub_82579FB8) {
  SBX_MODELS_FALLBACK(sub_82579FB8)
  const PointerVector& items = At<XPRList>(ctx.r3.u32)->items;
  const uint32_t slot = ctx.r4.u32;
  ctx.r3.u64 = items.first != 0 && slot < items.Size() ? uint32_t(*Element<u32_be>(items.first, slot)) : 0;
}

// The vector's at: r3 the vector, r4 an index. The element's address
// (std::_Xran, which throws, for one out of range).
extern "C" REX_FUNC(sub_82579F48) {
  SBX_MODELS_FALLBACK(sub_82579F48)
  PointerVector* vector = At<PointerVector>(ctx.r3.u32);
  const uint32_t index = ctx.r4.u32;
  if (vector->first == 0 || vector->Size() <= index) {
    g.Call(sub_82579AD8);  // std::_Xran
  }
  ctx.r3.u64 = vector->first + index * 4;
}

// The vector's reserve: r3 the vector, r4 the count.
extern "C" REX_FUNC(sub_82579E70) {
  SBX_MODELS_FALLBACK(sub_82579E70)
  PointersReserve(g, At<PointerVector>(ctx.r3.u32), ctx.r4.u32);
}

// The vector's allocate: r4 the count. The storage.
extern "C" REX_FUNC(sub_82576800) {
  SBX_MODELS_FALLBACK(sub_82576800)
  ctx.r3.u64 = PointersAllocate(g, At<PointerVector>(ctx.r3.u32), ctx.r4.u32);
}

// The vector's assign: r3 the vector, r4 the count, r5 the value's address.
extern "C" REX_FUNC(sub_8257A0D0) {
  SBX_MODELS_FALLBACK(sub_8257A0D0)
  PointerVector* vector = At<PointerVector>(ctx.r3.u32);
  const uint32_t count = ctx.r4.u32;
  const uint32_t value = *At<u32_be>(ctx.r5.u32);
  PointersAssign(g, vector, count, value);
}

// The vector's insert of copies: r3 the vector, r4 where, r5 how many, r6
// the value's address.
extern "C" REX_FUNC(sub_82579B98) {
  SBX_MODELS_FALLBACK(sub_82579B98)
  PointerVector* vector = At<PointerVector>(ctx.r3.u32);
  const uint32_t where = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t value = *At<u32_be>(ctx.r6.u32);
  PointersInsertN(g, vector, where, count, value);
}

// The vector's clear: r3 the vector. Returns its first element's address.
extern "C" REX_FUNC(sub_825768C0) {
  SBX_MODELS_FALLBACK(sub_825768C0)
  ctx.r3.u64 = PointersClear(At<PointerVector>(ctx.r3.u32));
}

// --- Models ---

// Model::Model: r3 the model. Returns it.
extern "C" REX_FUNC(sub_8257A1C0) {
  SBX_MODELS_FALLBACK(sub_8257A1C0)
  ctx.r3.u64 = AddressOf(game::ModelConstruct(g, At<Model>(ctx.r3.u32)));
}

// Model::~Model: r3 the model.
extern "C" REX_FUNC(sub_8257A858) {
  SBX_MODELS_FALLBACK(sub_8257A858)
  game::ModelDestruct(g, At<Model>(ctx.r3.u32));
}

// Model::Load: r3 the model, r4 its mesh file, r5 an XPR file or 0.
extern "C" REX_FUNC(sub_8257C238) {
  SBX_MODELS_FALLBACK(sub_8257C238)
  Model* model = At<Model>(ctx.r3.u32);
  const uint32_t file = ctx.r4.u32;
  const uint32_t xpr_file = ctx.r5.u32;
  game::ModelLoad(g, model, file, xpr_file);
}

// Mesh::Parse: r3 the model, r4 its mesh file.
extern "C" REX_FUNC(sub_8257BD58) {
  SBX_MODELS_FALLBACK(sub_8257BD58)
  Model* model = At<Model>(ctx.r3.u32);
  const uint32_t file = ctx.r4.u32;
  MeshParse(g, model, file);
}

// Model::Release: r3 the model; what it loaded freed.
extern "C" REX_FUNC(sub_8257A270) {
  SBX_MODELS_FALLBACK(sub_8257A270)
  game::ModelRelease(g, At<Model>(ctx.r3.u32));
}

// Model::MaterialCount: r3 the model, r4 a shape, r5 the group. How many
// materials the group has.
extern "C" REX_FUNC(sub_8257A3D8) {
  SBX_MODELS_FALLBACK(sub_8257A3D8)
  ctx.r3.u64 = game::MaterialCount(At<Model>(ctx.r3.u32), ctx.r4.u32, ctx.r5.s32);
}

// Model::Material: r3 the model, r4 a shape, r5 an index, r6 the group.
// That material.
extern "C" REX_FUNC(sub_8257A420) {
  SBX_MODELS_FALLBACK(sub_8257A420)
  ctx.r3.u64 = AddressOf(game::ModelMaterial(At<Model>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r6.s32));
}

// --- Skeletons over a node file ---

// NodeFile::View: r3 the view, r4 the node file.
extern "C" REX_FUNC(sub_82587928) {
  SBX_MODELS_FALLBACK(sub_82587928)
  NodeFileViewOf(At<NodeFileView>(ctx.r3.u32), ctx.r4.u32);
}

// SkeletonBase::SetUp: r3 the skeleton, r4 the node file, r5 matrices or
// 0, r6 the instance count.
extern "C" REX_FUNC(sub_82580098) {
  SBX_MODELS_FALLBACK(sub_82580098)
  Skeleton* skeleton = At<Skeleton>(ctx.r3.u32);
  const uint32_t file = ctx.r4.u32;
  const uint32_t matrices = ctx.r5.u32;
  const int32_t instances = ctx.r6.s32;
  SkeletonSetUp(g, skeleton, file, matrices, instances);
}

// Skeleton::Bind: r3 the skeleton, r4 the node file, r5 matrices or 0.
extern "C" REX_FUNC(sub_8257F1D8) {
  SBX_MODELS_FALLBACK(sub_8257F1D8)
  Skeleton* skeleton = At<Skeleton>(ctx.r3.u32);
  const uint32_t file = ctx.r4.u32;
  const uint32_t matrices = ctx.r5.u32;
  SkeletonBind(g, skeleton, file, matrices);
}

// Skeleton::ResetPose: r3 the skeleton.
extern "C" REX_FUNC(sub_8257F2A8) {
  SBX_MODELS_FALLBACK(sub_8257F2A8)
  SkeletonResetPose(g, At<Skeleton>(ctx.r3.u32));
}

// Model::RecordMaterials: r3 a car part's model, r4 a shape, r5 the
// material group, r6 whether to set the shaders. The chunk's header slot,
// or 0.
extern "C" REX_FUNC(sub_820CEA60) {
  SBX_MODELS_FALLBACK(sub_820CEA60)
  PartModel* part = At<PartModel>(ctx.r3.u32);
  const uint32_t shape = ctx.r4.u32;
  const int32_t group = ctx.r5.s32;
  const uint32_t set_shaders = ctx.r6.u32;
  ctx.r3.u64 = ModelRecordMaterials(g, part, shape, group, set_shaders);
}

// Model::RecordShape: r3 the model, r4 a shape, r5 the material group, r6
// whether to set the shaders, r7 a pixel constant (c76) or 0, r8 a vertex
// scale (c86) or 0. The chunk's header slot, or 0.
extern "C" REX_FUNC(sub_8257AE50) {
  SBX_MODELS_FALLBACK(sub_8257AE50)
  Model* model = At<Model>(ctx.r3.u32);
  const uint32_t shape = ctx.r4.u32;
  const int32_t group = ctx.r5.s32;
  const uint32_t set_shaders = ctx.r6.u32;
  const uint32_t pixel_color = ctx.r7.u32;
  const uint32_t vertex_scale = ctx.r8.u32;
  ctx.r3.u64 = game::ModelRecordShape(g, model, shape, group, set_shaders, pixel_color, vertex_scale);
}

// Skeleton::LocalMatrices: r3 the skeleton.
extern "C" REX_FUNC(sub_8257F410) {
  SBX_MODELS_FALLBACK(sub_8257F410)
  SkeletonLocalMatrices(g, At<Skeleton>(ctx.r3.u32));
}

// Skeleton::UpdateNodeMatrices: r3 the skeleton.
extern "C" REX_FUNC(sub_82581AA8) {
  SBX_MODELS_FALLBACK(sub_82581AA8)
  game::SkeletonUpdateNodeMatrices(g, At<Skeleton>(ctx.r3.u32));
}
