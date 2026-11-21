// The part models' meshes and materials, as far as they're worked out.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/model_animation.h"

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// A graphics buffer's header (a vertex or index buffer): +24 its data's
// address.
struct GpuBuffer {
  u32_be common;
  uint8_t unknown_4[20];
  u32_be address;
  u32_be unknown_28;
};
GUEST_SIZE(GpuBuffer, 32);

// A mesh's material (512 bytes): its colors (vertex c13, pixel c63 to c66),
// shaders, blend and render states, textures and their sampler states, its
// index buffer and draw words.
struct Material {
  static constexpr uint32_t kKind1002 = 1002;  // Old name (material_passes.cpp): kPairTextureAndVinyl.

  union {
    Vector vertex_color;  // Vertex c13 (the material animation writes it).
    struct {
      uint8_t unknown_0[12];
      f32_be alpha;  // Old name (material_passes.cpp): vertex_color.w.
    };
  };
  Vector color_16;      // Pixel c65 (w from color_80.z).
  Vector color_32;      // Pixel c66.
  uint8_t unknown_48[16];
  Vector color_64;      // Pixel c63.
  Vector color_80;      // Pixel c64 (the scenery's draw takes only x).
  u32_be shader_pair;   // The model's own pair.
  uint8_t unknown_100[4];
  u32_be flags;         // Bits 0x2 and 0x4: an environment texture.
  i32_be blend;         // Index into the blend table.
  u32_be render_state_100;
  uint8_t unknown_116[4];
  i32_be environment_texture;
  i32_be environment_xpr;
  i32_be texture_count;
  uint8_t unknown_132[12];
  u32_be texture_xprs[8];
  u32_be texture_ids[8];
  i32_be texture_colors[8];   // Into the model's float4 track values, or -1.
  f32_be texture_color_z[8];  // Used when there's no track value.
  u32_be sampler_state_0[8];
  u32_be sampler_state_4[8];
  uint8_t unknown_336[4];
  u32_be draw_word_8;
  u32_be cull_mode;
  u32_be draw_word_0;
  GpuBuffer index_buffer;  // Relocated to the model's vertex data by Mesh::Parse.
  u32_be unknown_384;      // Made an address in the header copy.
  u32_be has_alpha;        // Render state 96 for group 0.
  union {
    u32_be shared_pair;  // A shared shader pair (1000 on), or 0 for the model's own.
    u32_be kind;         // Old name (material_passes.cpp).
  };
  i32_be animation;        // A material animation of the model's +364 set, or -1.
  uint8_t unknown_400[4];
  u32_be pass_mask;        // The depth record's materials by mask.
  i32_be category;         // A car part material's kind (3 the part's texture, 5 a render target).
  uint8_t unknown_412[100];
};
GUEST_SIZE(Material, 512);
GUEST_OFFSET(Material, shader_pair, 96);
GUEST_OFFSET(Material, environment_texture, 120);
GUEST_OFFSET(Material, texture_count, 128);
GUEST_OFFSET(Material, texture_xprs, 144);
GUEST_OFFSET(Material, texture_colors, 208);
GUEST_OFFSET(Material, sampler_state_4, 304);
GUEST_OFFSET(Material, draw_word_8, 340);
GUEST_OFFSET(Material, draw_word_0, 348);
GUEST_OFFSET(Material, index_buffer, 352);
GUEST_OFFSET(Material, has_alpha, 388);
GUEST_OFFSET(Material, shared_pair, 392);
GUEST_OFFSET(Material, animation, 396);
GUEST_OFFSET(Material, pass_mask, 404);
GUEST_OFFSET(Material, category, 408);

// A mesh's materials: the count, how many each of the two groups has
// (Model::MaterialCount; group 1's follow group 0's), then the materials.
struct MaterialList {
  u32_be count;
  u32_be unknown_4;
  u32_be group_counts[2];
  Material materials[1];

  // Material `index` of a group (Model::Material).
  Material* InGroup(uint32_t index, uint32_t group) {
    return &materials[group == 0 ? index : group_counts[0] + index];
  }
};
GUEST_OFFSET(MaterialList, group_counts, 8);
GUEST_OFFSET(MaterialList, materials, 16);

// A skinned mesh's vertex weights: for each vertex its weight count, and
// the weights (a Vector each, x, y, z used), one vertex's after another's;
// then the skinned mesh's index buffer.
struct VertexWeights {
  u32_be vertex_count;
  u32_be unknown_4;
  u32_be counts;   // A u32_be a vertex.
  u32_be weights;  // A Vector a weight.
  u32_be unknown_16;
  u32_be index_buffer;  // 0 for none.
};
GUEST_SIZE(VertexWeights, 24);

// A model's mesh (a shape, 544 bytes): its bounding box, its three vertex
// streams (each a type, a stride and a size, a buffer and the buffer's
// data), its materials and its skinning.
struct Mesh {
  uint8_t unknown_0[96];
  Vector corners[8];  // The bounding box (Model::IsShapeVisible).
  uint8_t unknown_224[12];
  f32_be unknown_236;  // A set model is drawn only within the view's far distance of it.
  uint8_t unknown_240[4];
  u32_be stream_type_0;
  u32_be stream_stride_0;
  uint8_t unknown_252[8];
  u32_be stream_type_1;  // With 9 or 10 (or stream 2's type 6) the mesh has two streams.
  u32_be stream_stride_1;
  u32_be stream_size;  // Stream 1's.
  uint8_t unknown_272[4];
  u32_be stream_type_2;
  u32_be stream_stride_2;
  u32_be stream2_size;      // Stream 2's.
  GpuBuffer streams[3];     // The vertex buffers.
  u32_be vertex_data;       // Stream 0's data.
  u32_be stream_data;       // Stream 1's.
  u32_be stream2_data;      // Stream 2's.
  ptr<MaterialList> materials;
  u32_be unknown_400;  // Made an address in the header copy.
  u32_be unknown_404;  // Made an address in the header copy.
  ptr<VertexWeights> weights;
  u32_be skin_stream_1;  // A skinned mesh's streams 1 and 2.
  uint8_t unknown_416[4];
  u32_be skin_stream_2;
  uint8_t unknown_424[4];
  u32_be skinned;           // 1 for a skinned mesh (it has only stream 0's buffer).
  u32_be sampler_28_value;  // Sampler state 28 of samplers 0-3.
  uint8_t unknown_436[108];
};
GUEST_SIZE(Mesh, 544);
GUEST_OFFSET(Mesh, corners, 96);
GUEST_OFFSET(Mesh, unknown_236, 236);
GUEST_OFFSET(Mesh, stream_type_0, 244);
GUEST_OFFSET(Mesh, stream_type_1, 260);
GUEST_OFFSET(Mesh, stream_size, 268);
GUEST_OFFSET(Mesh, stream_type_2, 276);
GUEST_OFFSET(Mesh, stream2_size, 284);
GUEST_OFFSET(Mesh, streams, 288);
GUEST_OFFSET(Mesh, vertex_data, 384);
GUEST_OFFSET(Mesh, stream_data, 388);
GUEST_OFFSET(Mesh, materials, 396);
GUEST_OFFSET(Mesh, weights, 408);
GUEST_OFFSET(Mesh, skin_stream_2, 420);
GUEST_OFFSET(Mesh, skinned, 428);
GUEST_OFFSET(Mesh, sampler_28_value, 432);

// An XPR (the game's packed resources) a model or holder keeps: its header
// (copied to normal memory) and data (physical memory), and the header's
// records.
struct XPR {
  union {
    struct {
      u32_be header;
      u32_be header_size;
      u32_be data;
      u32_be data_size;
      u32_be records;  // An XPRRecord each, in the header.
      u32_be record_count;
    };
    u32_be words[6];  // Old view (scene.cpp, scene_loaders.cpp clear it by words).
  };
};
GUEST_SIZE(XPR, 24);

// A record of an XPR's header: a named resource, its header at an offset
// from the XPR's header.
struct XPRRecord {
  u32_be type;
  u32_be offset;
  u32_be unknown_8;
  u32_be name;
};
GUEST_SIZE(XPRRecord, 16);

// A std::vector of guest pointers: its storage from first to end, the
// elements up to last.
struct PointerVector {
  u32_be allocator;  // Never read.
  u32_be first;
  u32_be last;
  u32_be end;

  // The element count and capacity as the game works them out (a signed
  // shift of the byte distance).
  uint32_t Size() const { return first == 0 ? 0 : uint32_t(int32_t(last - first) >> 2); }
  uint32_t Capacity() const { return first == 0 ? 0 : uint32_t(int32_t(end - first) >> 2); }
};
GUEST_SIZE(PointerVector, 16);

// A list of XPR objects by slot (XPRList::XPRList, sub_8257A150; vtable
// 0x82069A94): a model's, and the texture provider the draws look their
// textures up in (its vtable's slots 2 Reset, 4 Set, 5 and 6 the textures).
struct XPRList {
  static constexpr uint32_t kVtable = 0x82069A94;

  u32_be vtable;
  PointerVector items;  // XPR objects by slot.
};
GUEST_SIZE(XPRList, 20);
GUEST_OFFSET(XPRList, items, 4);

struct Model {
  u32_be vtable;
  uint8_t unknown_4[12];
  u32_be header_copy;   // Its mesh file's header, copied to normal memory.
  u32_be vertex_data;   // Its vertex data, in physical memory.
  u32_be shader_pairs;  // The address of its ShaderPairEntry array.
  u32_be header;        // The header copy, as a MeshFileHeader; 0 before a load.
  u32_be header_sections[2];  // The header's sections at its +20 and +24.
  u32_be meshes;              // The address of its Mesh array.
  u32_be header_section_44;   // The header's section at its +32.
  union {
    ptr<XPRList> texture_provider;  // Where its draws look textures up (its own XPR list once loaded).
    u32_be unknown_48;              // Old name (load_poll.cpp, scene_loaders.cpp).
  };
  uint8_t unknown_52[4];
  XPR xpr;
  XPRList xpr_list;  // Its XPRs by slot: slot 0 the model's own.
  uint8_t unknown_100[12];
  uint8_t skeleton[192];  // A Skeleton (SkeletonObject()).
  TrackSet tracks[5];
  union {
    struct {  // The header's counts (MeshFileHeader::counts).
      u32_be header_count_0;
      u32_be header_count_1;  // 0: no skeleton (as scene_loaders.cpp reads it).
      u32_be shape_count;
      u32_be header_count_3;
    };
    u32_be unknown_404[4];  // Old view (scene_loaders.cpp reads [1] and [2]).
  };
  u32_be shader_pair_count;

  Mesh* MeshAt(uint32_t index) { return guest::Element<Mesh>(meshes, index); }
  Skeleton* SkeletonObject() { return reinterpret_cast<Skeleton*>(skeleton); }
};
GUEST_SIZE(Model, 424);
GUEST_OFFSET(Model, header_copy, 16);
GUEST_OFFSET(Model, shader_pairs, 24);
GUEST_OFFSET(Model, header, 28);
GUEST_OFFSET(Model, meshes, 40);
GUEST_OFFSET(Model, texture_provider, 48);
GUEST_OFFSET(Model, xpr, 56);
GUEST_OFFSET(Model, xpr_list, 80);
GUEST_OFFSET(Model, skeleton, 112);
GUEST_OFFSET(Model, tracks, 304);
GUEST_OFFSET(Model, header_count_0, 404);
GUEST_OFFSET(Model, shape_count, 412);
GUEST_OFFSET(Model, shader_pair_count, 420);
static_assert(sizeof(Skeleton) <= sizeof(Model::skeleton));

// A mesh file's header (the part of the file copied to normal memory):
// its counts and its sections' offsets from itself.
struct MeshFileHeader {
  u32_be unknown_0;
  u32_be counts[4];          // To the model's +404 to +416 (+412 the shape count).
  u32_be section_offsets[4]; // +20 to +32: the model's +32, +36, +40 (the meshes), +44.
  uint8_t unknown_36[40];
  u32_be has_skeleton;  // +76: with it, the skeleton and the other track sets.
};
GUEST_OFFSET(MeshFileHeader, counts, 4);
GUEST_OFFSET(MeshFileHeader, section_offsets, 20);
GUEST_OFFSET(MeshFileHeader, has_skeleton, 76);

// A mesh (part) file: its version, its sections' offsets from the header
// copy, the header's and the vertex data's sizes (the data follows the
// header), and the shaders' section's offset.
struct MeshFile {
  static constexpr uint32_t kVersion = 0x00150000;

  u32_be version;
  uint8_t unknown_4[32];
  u32_be skeleton;       // +36: the node file.
  u32_be node_values;    // +40.
  u32_be float4_tracks;  // +44: the +304 track set's.
  u32_be float_tracks[2];  // +48, +52: the +324 and +344 sets'.
  u32_be tracks_32;        // +56: the +364 set's.
  u32_be tracks_208;       // +60: the +384 set's.
  u32_be header_bytes;     // +64.
  u32_be vertex_data_bytes;  // +68.
  u32_be shaders;            // +72: from the file.
};
GUEST_OFFSET(MeshFile, skeleton, 36);
GUEST_OFFSET(MeshFile, header_bytes, 64);
GUEST_OFFSET(MeshFile, shaders, 72);

// A car part's model.
struct PartModel {
  Model model;
  uint8_t unknown_424[8];
  u32_be unknown_432;
  guest::i32_be unknown_436;
  u32_be unknown_440;
  u32_be vinyl_textures[2];
  u32_be unknown_452[3];
};
GUEST_OFFSET(PartModel, unknown_432, 432);
GUEST_OFFSET(PartModel, vinyl_textures, 444);

// A row of the paint table (0x82003660).
struct PaintRecord {
  uint8_t unknown_0[48];
};
GUEST_SIZE(PaintRecord, 48);

// --- The functions other files of the group call (models.cpp) ---

// The models: construction and destruction, Model::Load (its mesh file
// parsed, the XPR file when given made its own, its weights rounded), the
// release of what it loaded, and a shape's materials.
Model* ModelConstruct(guest::Guest& g, Model* model);  // sub_8257A1C0
void ModelDestruct(guest::Guest& g, Model* model);     // sub_8257A858
void ModelLoad(guest::Guest& g, Model* model, uint32_t file, uint32_t xpr_file);  // sub_8257C238
void ModelRelease(guest::Guest& g, Model* model);                                 // sub_8257A270
uint32_t MaterialCount(Model* model, uint32_t shape, int32_t group);              // sub_8257A3D8
Material* ModelMaterial(Model* model, uint32_t shape, uint32_t index, int32_t group);  // sub_8257A420

// Model::RecordShape: a shape's draws for one material group recorded as a
// chunk; the chunk's header slot, or 0.
uint32_t ModelRecordShape(guest::Guest& g, Model* model, uint32_t shape, int32_t group,
                          uint32_t set_shaders, uint32_t pixel_color,
                          uint32_t vertex_scale);  // sub_8257AE50

// XPRs and XPR lists.
void XprConstruct(XPR* xpr);                                     // sub_8257C780
void XprFree(guest::Guest& g, XPR* xpr);                         // sub_8257C9C0, sub_8257CA60
void XprListConstruct(guest::Guest& g, XPRList* list, int32_t count);  // sub_8257A150

// The shared shaders (0 before they're set) and the vertex declarations.
uint32_t SharedVertexShader(uint32_t pair);  // sub_8256F258
uint32_t SharedPixelShader(uint32_t pair);   // sub_8256F280
uint32_t VertexDeclaration(guest::Guest& g, int32_t first, int32_t second,
                           int32_t third);  // sub_8256F2A8

// Skeleton::UpdateNodeMatrices: each node's matrix made from its parent's.
void SkeletonUpdateNodeMatrices(guest::Guest& g, Skeleton* skeleton);  // sub_82581AA8

}  // namespace game
