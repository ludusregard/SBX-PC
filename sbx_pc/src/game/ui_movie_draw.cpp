// The UI movies' drawing as our own C++ (--native_ui_movies, defined in
// ui_movies.cpp): the movie's draw (UiMovie::Draw and DrawNode walking its
// node tree), each node's transform, pictures and primitives built into
// quads and recorded on the command list, texts queued for the movie to draw
// after; the curves of the tracks' keys; and the movie file's fix-ups at
// load (its scene and animation tables, the XMD check, the chunk lookups).
//
// A quad and its vertices are built on the drawing function's guest stack
// (as the game's), since the recompiled matrix math works on them there.
// The recorders hand each chunk to the renderer through its vtable; the
// track step (UiMovie::ApplyTrack) is called through Guest::Call, so
// subtick's wrapper runs. Nothing is kept between calls.

#include "game/ui_movie_draw.h"
#include "base/settings.h"
#include "game/ui_movies.h"

#include "hooks/native_calls.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>

SBX_SETTING_DECLARE(bool, native_ui_movies);

namespace game {
namespace {

using guest::AddressOf;
using guest::At;
using guest::Guest;
using guest::f32_be;
using guest::u16_be;
using guest::u32_be;

bool On() { return SBX_GET(native_ui_movies); }

// The renderer and command list the movies draw with (UiMovie::SetRenderer
// sets them; writable data, read each time).
constexpr uint32_t kRenderer = 0x82973078;
constexpr uint32_t kCommandList = 0x8297307C;
constexpr uint32_t kRendererSubmitSlot = 4;  // r4 the chunk, r5 its key, f1 its depth.

// The read-only floats the originals load.
constexpr float kNearZero = 1.0e-5f;             // 0x82002A20: scales and turns this small count as none.
constexpr float kColorScale = 1.0f / 255.0f;     // 0x820021C0: 0 to 255 to 0 to 1.
constexpr float kColorWords = 255.0f;            // 0x82000C54: 0 to 1 to the quads' 0 to 255.
constexpr float kDegreesToRadians = 0.01745329238474369f;  // 0x82002920
constexpr float kDepthScale = -0.001f;           // 0x8206E300: a layer to a depth.
constexpr float kQuadDepth = 0.0f;               // 0x82000A24: the depth a quad's chunk goes at.
constexpr float kClipResetDepth = -10000.0f;     // 0x82001BF8
constexpr float kClipOffDepth = -9000.0f;        // 0x82001BF4
constexpr float kThird = 0.3333333432674408f;    // 0x82002A14
constexpr float kThree = 3.0f;                   // 0x820021A4

static_assert(std::bit_cast<uint32_t>(kNearZero) == 0x3727C5AC);
static_assert(std::bit_cast<uint32_t>(kColorScale) == 0x3B808081);
static_assert(std::bit_cast<uint32_t>(kDegreesToRadians) == 0x3C8EFA35);
static_assert(std::bit_cast<uint32_t>(kDepthScale) == 0xBA83126F);
static_assert(std::bit_cast<uint32_t>(kThird) == 0x3EAAAAAB);

// The screen the clip goes back to after a clipped quad.
constexpr int16_t kScreenWidth = 1280;
constexpr int16_t kScreenHeight = 720;

// ---------------------------------------------------------------------------
// Number conversions as the originals make them.

// A single-precision result of a double (the originals' fmuls, fadds,
// fsubs and fmadds, on the callers' f1 as it came).
double Single(double value) { return double(float(value)); }

// fctiwz: toward zero; NaN and anything below the range give INT_MIN,
// anything at INT_MAX or above INT_MAX.
int32_t TruncateToInt(double value) {
  if (std::isnan(value) || value < -2147483648.0) {
    return INT32_MIN;
  }
  if (value >= 2147483647.0) {
    return INT32_MAX;
  }
  return int32_t(value);
}

// fctidz, its low word kept (the quads' color words): toward zero; NaN and
// anything out of range give the smallest 64-bit value (low word 0),
// anything above 2^63 the largest.
uint32_t TruncateToLowWord(double value) {
  constexpr double kTwoTo63 = 9223372036854775808.0;
  if (std::isnan(value) || value >= kTwoTo63 || value < -kTwoTo63) {
    return value > kTwoTo63 ? 0xFFFFFFFFu : 0u;
  }
  return uint32_t(int64_t(value));
}

// Whether a scale or a turn is too small to count: its size not above
// kNearZero (so a NaN counts as none, as the original's ble has it).
bool NearZero(float value) { return !((value > 0.0f ? value : -value) > kNearZero); }

void SetIdentity(Matrix& m) {
  m.right = {1.0f, 0.0f, 0.0f, 0.0f};
  m.up = {0.0f, 1.0f, 0.0f, 0.0f};
  m.forward = {0.0f, 0.0f, 1.0f, 0.0f};
  m.position = {0.0f, 0.0f, 0.0f, 1.0f};
}

bool ClipIsSet(const MovieClip& clip) {
  return clip.x0 != 0 || clip.y0 != 0 || clip.x1 != 0 || clip.y1 != 0;
}

// ---------------------------------------------------------------------------
// The movie file's chunks and tables.

}  // namespace

// The chunk after `chunk` tagged `tag`; at the list's end (TRM) none,
// unless TRM is the tag looked for.
MovieChunk* FindChunk(MovieChunk* chunk, uint32_t tag) {
  for (;;) {
    chunk = At<MovieChunk>(AddressOf(chunk) + chunk->next);
    if (chunk->tag == kTagEnd) {
      return tag == kTagEnd ? chunk : nullptr;
    }
    if (chunk->tag == tag) {
      return chunk;
    }
  }
}

MovieNode* Child(MovieGroup* group, uint32_t index) {
  return At<MovieNode>(group->EntryAt(group->children + index)->value);
}

namespace {

// UiMovie::FixUpScene: the file's scene's entries' offsets made pointers
// (its animations through the file's table, the rest from the scene), and
// each child that is a scene too fixed up the same way.
void FixUpScene(MovieGroup* scene, MovieFile* file) {
  for (uint32_t i = 0; i < scene->entry_count; i++) {
    MovieGroup::Entry* entry = scene->EntryAt(i);
    const uint32_t first = scene->animations;
    if (first <= i && i < first + scene->animation_count) {
      entry->value = file->Animation(entry->value);
    } else {
      entry->value = entry->value + AddressOf(scene);
    }
    MovieGroup* child = At<MovieGroup>(entry->value);
    if (child != nullptr && child->node.tag == kTagScene) {
      FixUpScene(child, file);
    }
  }
}

// UiMovie::FixUpAnimation: an animation's entries' offsets made pointers,
// and its sub-animations' animations looked up in the file's table.
void FixUpAnimation(MovieAnimation* animation, MovieFile* file) {
  for (uint32_t i = 0; i < animation->entry_count; i++) {
    MovieEntry* entry = animation->EntryAt(i);
    entry->value = entry->value + AddressOf(animation);
  }
  for (uint32_t i = 0; i < animation->sub_animation_count; i++) {
    MovieSubAnimation* sub = animation->SubAnimation(i);
    sub->animation = file->Animation(sub->animation);
  }
}

}  // namespace

// The file's scene, when the chunk there is one; else none.
MovieGroup* FileScene(MovieFile* file) {
  MovieGroup* scene = At<MovieGroup>(AddressOf(file) + file->scene);
  return scene->node.tag == kTagScene ? scene : nullptr;
}

// UiMovie::FixUpFile: a loaded movie file (16-byte aligned, IsMovieFile)
// made ready: its table's offsets made pointers, its scene and animations
// fixed up. Whether it was one.
bool FixUpFile(MovieFile* file) {
  const uint32_t address = AddressOf(file);
  if ((address & 15) != 0 || !IsMovieFile(reinterpret_cast<const uint8_t*>(file))) {
    return false;
  }
  for (uint32_t i = 0; i < file->entry_count; i++) {
    MovieEntry* entry = file->EntryAt(i);
    entry->value = entry->value + address;
  }
  // A file whose scene isn't one has the original fix up a scene at
  // address 0 (reading nothing there); none is fixed up here.
  if (MovieGroup* scene = FileScene(file)) {
    FixUpScene(scene, file);
  }
  for (uint32_t i = 0; i < file->animation_count; i++) {
    FixUpAnimation(At<MovieAnimation>(file->EntryAt(file->first_animation + i)->value), file);
  }
  return true;
}

// The XMD check: 16-byte aligned and IsXmd. The original then byte-swaps
// the header and offsets when the first word doesn't read "XMD", which
// IsXmd has just made sure it does, so that never runs.
bool CheckXmd(XmdFile* file) {
  return (AddressOf(file) & 15) == 0 && IsXmd(reinterpret_cast<const uint8_t*>(file));
}

// The XMD file's entry `index`, or none past its count.
uint32_t XmdEntry(XmdFile* file, uint32_t index) {
  return index < file->count ? AddressOf(file) + file->OffsetAt(index) : 0;
}

namespace {

// ---------------------------------------------------------------------------
// Recording quads on the command list.

// A command slot of the list (16 bytes; the recorders write the first two
// or three words).
struct Command {
  u32_be command;
  u32_be a;
  u32_be b;
  uint8_t unknown_12[4];
};
GUEST_SIZE(Command, 16);

// A chunk's first slot.
struct ChunkHeader {
  uint8_t unknown_0[2];
  u16_be kind;  // 5
  u32_be unknown_4;
};

// A recorded vertex (CommandList::AllocateVertices, 24 bytes).
struct RecordedVertex {
  f32_be x, y, z;
  u32_be color;  // a, r, g, b bytes
  f32_be u, v;
};
GUEST_SIZE(RecordedVertex, 24);

uint32_t CommandList() { return *At<u32_be>(kCommandList); }

ChunkHeader* OpenChunk(Guest& g) {
  g.Call(sub_82572568, CommandList());  // CommandList::OpenChunk
  ChunkHeader* header = At<ChunkHeader>(g.Call(sub_82572638, CommandList(), 1u));  // AllocateSlots
  header->kind = 5;
  header->unknown_4 = 0;
  return header;
}

Command* NextSlot(Guest& g) {
  return At<Command>(g.Call(sub_82572638, CommandList(), 1u));  // CommandList::AllocateSlots
}

// A command with one word, its slot's third left as it is.
void Put(Guest& g, uint32_t command, uint32_t a) {
  Command* slot = NextSlot(g);
  slot->command = command;
  slot->a = a;
}

void Put(Guest& g, uint32_t command, uint32_t a, uint32_t b) {
  Command* slot = NextSlot(g);
  slot->command = command;
  slot->a = a;
  slot->b = b;
}

// The chunk closed and handed to the renderer at `depth`.
void CloseAndSubmit(Guest& g, ChunkHeader* header, uint64_t key, float depth) {
  g.Call(sub_825725E8, CommandList());  // CommandList::CloseChunk
  void* renderer = At<u32_be>(*At<u32_be>(kRenderer));
  g.ctx.f1.f64 = double(depth);
  g.CallVirtual(renderer, kRendererSubmitSlot, header, key);
}

}  // namespace

// UiMovie::ResetClip (sub_82582EA8, and sub_825831B8 after the movie's
// draws): the clip turned back to the whole screen, then turned off: two
// chunks, keyed `key`.
void ResetClip(Guest& g, uint64_t key) {
  ChunkHeader* header = OpenChunk(g);
  Put(g, 31, 1, 0);
  MovieClip16* screen = At<MovieClip16>(AddressOf(NextSlot(g)));
  screen->x0 = 0;
  screen->y0 = 0;
  screen->x1 = kScreenWidth;
  screen->y1 = kScreenHeight;
  Put(g, 36, 0, 0);
  CloseAndSubmit(g, header, key, kClipResetDepth);

  header = OpenChunk(g);
  Put(g, 1, 1, 0);
  Put(g, 200, 0);
  Put(g, 36, 0, 0);
  CloseAndSubmit(g, header, key, kClipOffDepth);
}

namespace {

// The blend states by the quad's blend mode (0 to 4; others set none).
void PutBlend(Guest& g, uint32_t blend) {
  switch (blend) {
    case 0:
      Put(g, 1, 4, 0);
      Put(g, 56, 0);
      Put(g, 60, 1);
      Put(g, 76, 7);
      Put(g, 72, 6);
      break;
    case 1:
      Put(g, 1, 4, 0);
      Put(g, 56, 0);
      Put(g, 60, 1);
      Put(g, 76, 4);
      Put(g, 72, 0);
      break;
    case 2:
      Put(g, 1, 4, 0);
      Put(g, 56, 0);
      Put(g, 60, 1);
      Put(g, 76, 1);
      Put(g, 72, 6);
      break;
    case 3:
      Put(g, 1, 5, 0);
      Put(g, 56, 0);
      Put(g, 60, 1);
      Put(g, 80, 4);
      Put(g, 76, 1);
      Put(g, 72, 6);
      break;
    case 4:
      Put(g, 1, 3, 0);
      Put(g, 56, 0);
      Put(g, 60, 0);
      Put(g, 40, 0);
      break;
    default:
      break;
  }
}

// A quad recorded as one chunk keyed `key` (sub_825831C8 with a texture,
// sub_82583790 without): its clip (or the clip off), its texture, blend
// and sampler states, its vertices, then handed to the renderer.
void RecordQuad(Guest& g, MovieQuad* quad, uint64_t key, bool textured) {
  ChunkHeader* header = OpenChunk(g);
  if (quad->clipped != 0) {
    Put(g, 31, 1);
    *At<MovieClip16>(AddressOf(NextSlot(g))) = quad->clip;
  } else {
    Put(g, 1, 1, 0);
    Put(g, 200, 0);
  }
  if (textured) {
    Put(g, 4, 1, 0);
    Put(g, 0, quad->texture);
  }
  PutBlend(g, quad->blend);
  const uint32_t scaled = quad->scaled != 0 ? 1 : 0;
  Put(g, 2, 2, 0);
  Put(g, 0, 20, scaled);
  Put(g, 0, 16, scaled);
  Put(g, 13, 1, 0);
  Put(g, 6, 1, textured ? 2 : 0);
  Put(g, 5, quad->vertex_count, 0);

  RecordedVertex* vertices = At<RecordedVertex>(
      g.Call(sub_825726A0, CommandList(), uint32_t(quad->vertex_count), 0u));  // AllocateVertices
  for (uint32_t i = 0; i < quad->vertex_count; i++) {
    const Vector& position = quad->positions[i];
    const MovieColorWords& color = quad->colors[i];
    RecordedVertex& out = vertices[i];
    out.x = position.x;
    out.y = position.y;
    out.z = position.z;
    out.color = (uint32_t(color.a) << 24) | ((uint32_t(color.r) & 0xFF) << 16) |
                ((uint32_t(color.g) & 0xFF) << 8) | (uint32_t(color.b) & 0xFF);
    if (textured) {
      const Vector& texcoord = quad->texcoords[i];
      out.u = texcoord.x;
      out.v = texcoord.y;
    }
  }

  Put(g, 15, 1, 0);
  NextSlot(g);  // Taken and left as it is.
  Put(g, 36, 0, 0);
  CloseAndSubmit(g, header, key, kQuadDepth);
}

}  // namespace

// UiMovie::DrawQuad: a quad or primitive recorded, and the clip reset after
// it when it was clipped (keyed one on).
void DrawQuad(Guest& g, MovieQuad* quad, uint64_t key) {
  RecordQuad(g, quad, key, quad->texture != 0);
  if (quad->clipped != 0) {
    ResetClip(g, key + 1);
  }
}

// DrawContext::DrawContext: no transform, clip or tint.
void InitContext(MovieDrawContext* context) {
  SetIdentity(context->matrix);
  context->red = 1.0f;
  context->green = 1.0f;
  context->blue = 1.0f;
  context->alpha = 1.0f;
  context->clip = {0, 0, 0, 0};
  context->textures.address = 0;
  context->text_list.address = 0;
  context->unknown_104 = 0;
  context->scaled = 0;
}

namespace {

// ---------------------------------------------------------------------------
// The draw context and a node's transform.

// A child's context, a copy of its parent's (the matrix by Matrix::Copy).
void CopyContext(Guest& g, MovieDrawContext* to, const MovieDrawContext* from) {
  to->matrix.right.w = 1.0f;
  to->matrix.up.w = 1.0f;
  to->matrix.forward.w = 1.0f;
  to->matrix.position.w = 1.0f;
  g.Call(sub_820A0E18, &to->matrix, &from->matrix);  // Matrix::Copy
  to->clip = from->clip;
  to->red = from->red;
  to->green = from->green;
  to->blue = from->blue;
  to->alpha = from->alpha;
  to->textures = from->textures;
  to->text_list = from->text_list;
  to->unknown_104 = from->unknown_104;
  to->scaled = from->scaled;
}

// The clip narrowed by another, as the game has it: x0 and y1 the larger,
// y0 and x1 the smaller.
void IntersectClip(MovieClip* clip, const MovieClip& other) {
  clip->x0 = std::max(int32_t(clip->x0), int32_t(other.x0));
  clip->y0 = std::min(int32_t(clip->y0), int32_t(other.y0));
  clip->x1 = std::min(int32_t(clip->x1), int32_t(other.x1));
  clip->y1 = std::max(int32_t(clip->y1), int32_t(other.y1));
}

// Matrix::Multiply (sub_8209BE70): `to` = `a` x `b`.
void Multiply(Guest& g, Matrix* to, const Matrix* a, const Matrix* b) {
  g.Call(sub_8209BE70, to, a, b);
  g.ctx.fpscr.disableFlushMode();
}

// UiMovie::ApplyTransform's frame (352 bytes): the local matrices, which
// the recompiled matrix math is handed.
struct TransformFrame {
  uint8_t linkage[176];
  Matrix local;
  Matrix turn;
  uint8_t unknown_304[48];
};
GUEST_SIZE(TransformFrame, 352);
GUEST_OFFSET(TransformFrame, local, 176);
GUEST_OFFSET(TransformFrame, turn, 240);

// UiMovie::ApplyTransform: the node's clip, color and transform applied to
// the context. Its clip, when set, is offset by the context's (as the game
// reads it: the matrix's third row's x and y, not its position) and kept
// or intersected with the context's. Its color tints the context's. Its
// transform goes on in front of the context's matrix (Matrix::Multiply,
// local first): moved to its position plus its pivot, turned, scaled, moved
// back by the pivot; each step skipped when it does nothing.
void ApplyTransform(Guest& g, MovieDrawContext* context, const MovieNode* node) {
  guest::StackFrame stack(g.ctx, sizeof(TransformFrame));
  TransformFrame* frame = At<TransformFrame>(stack.Local(0));
  g.ctx.fpscr.disableFlushMode();

  if (ClipIsSet(node->clip)) {
    const uint32_t offset_x = uint32_t(TruncateToInt(double(float(context->matrix.forward.x))));
    const uint32_t offset_y = uint32_t(TruncateToInt(double(float(context->matrix.forward.y))));
    MovieClip clip;
    clip.x0 = int32_t(uint32_t(int32_t(node->clip.x0)) + offset_x);
    clip.y0 = int32_t(uint32_t(int32_t(node->clip.y0)) + offset_y);
    clip.x1 = int32_t(uint32_t(int32_t(node->clip.x1)) + offset_x);
    clip.y1 = int32_t(uint32_t(int32_t(node->clip.y1)) + offset_y);
    if (!ClipIsSet(context->clip)) {
      context->clip = clip;
    } else {
      IntersectClip(&context->clip, clip);
    }
  }

  const float red = context->red, green = context->green, blue = context->blue;
  const float alpha = context->alpha;
  context->red = float(int32_t(node->red)) * red * kColorScale;
  context->green = float(int32_t(node->green)) * green * kColorScale;
  context->blue = float(int32_t(node->blue)) * blue * kColorScale;
  context->alpha = float(node->alpha) * alpha;

  Matrix& local = frame->local;
  if (node->pivot_x != 0 || !(float(node->x) == 0.0f) || node->pivot_y != 0 ||
      !(float(node->y) == 0.0f)) {
    SetIdentity(local);
    local.position.z = 0.0f;
    local.position.x = float(node->x) + float(int32_t(node->pivot_x));
    local.position.y = float(node->y) + float(int32_t(node->pivot_y));
    Multiply(g, &context->matrix, &local, &context->matrix);
  }

  const float rotation = node->rotation;
  if (!NearZero(rotation)) {
    SetIdentity(frame->turn);
    g.ctx.f1.f64 = double(rotation * kDegreesToRadians);
    g.Call(sub_820A3088, &local);  // Matrix::RotateZ
    Multiply(g, &frame->turn, &frame->turn, &local);
    Multiply(g, &context->matrix, &frame->turn, &context->matrix);
  }

  const float scale_x = node->scale_x;
  if (!NearZero(scale_x) || !NearZero(node->scale_y)) {
    SetIdentity(local);
    local.right.x = scale_x;
    local.up.y = node->scale_y;
    Multiply(g, &context->matrix, &local, &context->matrix);
    if (!(float(node->scale_x) == 1.0f) || !(float(node->scale_y) == 1.0f)) {
      context->scaled = 1;
    }
  }

  if (node->pivot_x != 0 || node->pivot_y != 0) {
    SetIdentity(local);
    local.position.z = 0.0f;
    local.position.x = float(int32_t(0u - uint32_t(int32_t(node->pivot_x))));
    local.position.y = float(int32_t(0u - uint32_t(int32_t(node->pivot_y))));
    Multiply(g, &context->matrix, &local, &context->matrix);
  }
}

// ---------------------------------------------------------------------------
// Pictures and primitives.

// The movie's texture `index`, or none.
const MovieTexture* TextureAt(const MovieDrawContext* context, uint32_t index) {
  return At<MovieTexture>(*guest::Element<u32_be>(context->textures->data, index));
}

// The context's clip on a quad, each pair sorted; none when its x0, y0 and
// y1 are 0 (the game doesn't look at x1 here).
void SetQuadClip(MovieQuad* quad, const MovieDrawContext* context) {
  const MovieClip& clip = context->clip;
  if (clip.x0 == 0 && clip.y0 == 0 && clip.y1 == 0) {
    quad->clipped = 0;
    quad->clip = {0, 0, 0, 0};
    return;
  }
  const int16_t x0 = int16_t(int32_t(clip.x0)), y0 = int16_t(int32_t(clip.y0));
  const int16_t x1 = int16_t(int32_t(clip.x1)), y1 = int16_t(int32_t(clip.y1));
  quad->clipped = 1;
  quad->clip = {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
}

// A color in the quads' 0 to 255 words.
uint32_t ColorWord(float value) { return TruncateToLowWord(double(value * kColorWords)); }

// A layer's depth: farther for higher layers.
double LayerDepth(uint32_t layer) { return double(float(layer) * kDepthScale); }

// UiMovie::DrawPicture's frame (1,088 bytes): the quad and its four
// vertices (with room for 16), which the recompiled Vector::Transform is
// handed.
struct PictureFrame {
  uint8_t linkage[112];
  MovieQuad quad;
  uint8_t unknown_228[12];
  Vector positions[16];
  MovieColorWords colors[16];
  Vector texcoords[16];
  uint8_t unknown_1008[80];
};
GUEST_SIZE(PictureFrame, 1088);
GUEST_OFFSET(PictureFrame, quad, 112);
GUEST_OFFSET(PictureFrame, positions, 240);
GUEST_OFFSET(PictureFrame, colors, 496);
GUEST_OFFSET(PictureFrame, texcoords, 752);

// UiMovie::DrawPicture: the picture's transform applied, then a quad from
// (0, 0) to its size transformed by the context, in the context's color,
// showing its pixels of its texture (or all of a texture without a size),
// flipped as set, clipped by the context's clip, recorded keyed by its own
// layer.
void DrawPicture(Guest& g, MovieDrawContext* context, MoviePicture* picture) {
  guest::StackFrame stack(g.ctx, sizeof(PictureFrame));
  PictureFrame* frame = At<PictureFrame>(stack.Local(0));
  for (Vector& position : frame->positions) {
    position.w = 1.0f;
  }
  MovieQuad* quad = &frame->quad;
  MakeQuad(quad);
  ApplyTransform(g, context, &picture->node);
  g.ctx.fpscr.disableFlushMode();

  quad->vertex_count = 4;
  quad->positions = frame->positions;
  quad->colors = frame->colors;
  quad->texcoords = frame->texcoords;
  const MovieTexture* texture = nullptr;
  if (picture->textured != 0) {
    texture = TextureAt(context, picture->TextureEntryAt(picture->texture_entry)->index);
  }
  quad->texture = texture != nullptr ? uint32_t(texture->texture) : 0;

  for (int i = 0; i < 4; i++) {
    MovieColorWords& color = frame->colors[i];
    color.r = ColorWord(context->red);
    color.g = ColorWord(context->green);
    color.b = ColorWord(context->blue);
    color.a = ColorWord(context->alpha);
  }

  Vector* corners = frame->positions;
  corners[0] = {0.0f, 0.0f, 0.0f, 1.0f};
  corners[1] = {0.0f, 0.0f, 0.0f, 1.0f};
  corners[1].x = picture->width;
  corners[2] = {0.0f, 0.0f, 0.0f, 1.0f};
  corners[2].y = picture->height;
  corners[3] = {0.0f, 0.0f, 0.0f, 1.0f};
  corners[3].x = picture->width;
  corners[3].y = picture->height;
  for (int i = 0; i < 4; i++) {
    g.Call(sub_820AB518, &corners[i], context, &corners[i]);  // Vector::Transform
  }
  g.ctx.fpscr.disableFlushMode();

  // A missing texture reads as one without a size (the original reads
  // guest address 4 and 8 for it).
  Vector* uv = frame->texcoords;
  const uint32_t width = texture != nullptr ? uint32_t(texture->width) : 0;
  const uint32_t height = texture != nullptr ? uint32_t(texture->height) : 0;
  if (width != 0 && height != 0) {
    const float w = float(width), h = float(height);
    const float left = float(int32_t(picture->source_left));
    const float top = float(int32_t(picture->source_top));
    const float right = float(int32_t(picture->source_right));
    const float bottom = float(int32_t(picture->source_bottom));
    uv[0].x = left / w;
    uv[0].y = top / h;
    uv[1].x = right / w;
    uv[1].y = top / h;
    uv[2].x = left / w;
    uv[2].y = bottom / h;
    uv[3].x = right / w;
    uv[3].y = bottom / h;
  } else {
    uv[0].x = 0.0f;
    uv[0].y = 0.0f;
    uv[1].x = 1.0f;
    uv[1].y = 0.0f;
    uv[2].x = 0.0f;
    uv[2].y = 1.0f;
    uv[3].x = 1.0f;
    uv[3].y = 1.0f;
  }
  if (picture->flip_x != 0) {
    std::swap(uv[0], uv[1]);
    std::swap(uv[2], uv[3]);
  }
  if (picture->flip_y != 0) {
    std::swap(uv[0], uv[2]);
    std::swap(uv[1], uv[3]);
  }

  SetQuadClip(quad, context);
  quad->blend = 0;
  quad->unknown_72 = 0;
  quad->scaled = context->scaled;
  DrawQuad(g, quad, uint32_t(picture->node.layer));
}

// UiMovie::DrawPrimitive's frame (1,152 bytes): the quad and room for 16
// vertices, which the recompiled Vector::Transform is handed.
struct PrimitiveFrame {
  uint8_t linkage[96];
  MovieQuad quad;
  uint8_t unknown_212[60];
  Vector positions[16];
  MovieColorWords colors[16];
  Vector texcoords[16];
  uint8_t unknown_1040[112];
};
GUEST_SIZE(PrimitiveFrame, 1152);
GUEST_OFFSET(PrimitiveFrame, quad, 96);
GUEST_OFFSET(PrimitiveFrame, positions, 272);
GUEST_OFFSET(PrimitiveFrame, colors, 528);
GUEST_OFFSET(PrimitiveFrame, texcoords, 784);
constexpr uint32_t kPrimitiveRoom = 16;

// UiMovie::DrawPrimitive: as a picture, with the primitive's own vertices:
// each position transformed by the context, its color tinted by the
// context's (squared when set), its texture coordinates in its texture's
// pixels.
void DrawPrimitive(Guest& g, MovieDrawContext* context, MoviePrimitive* primitive) {
  guest::StackFrame stack(g.ctx, sizeof(PrimitiveFrame));
  PrimitiveFrame* frame = At<PrimitiveFrame>(stack.Local(0));
  for (Vector& position : frame->positions) {
    position.w = 1.0f;
  }
  MovieQuad* quad = &frame->quad;
  MakeQuad(quad);
  ApplyTransform(g, context, &primitive->node);
  g.ctx.fpscr.disableFlushMode();

  quad->positions = frame->positions;
  quad->vertex_count = primitive->vertex_count;
  quad->colors = frame->colors;
  quad->texcoords = frame->texcoords;
  const MovieTexture* texture = nullptr;
  if (primitive->texture_count != 0) {
    texture = TextureAt(context, primitive->TextureEntryAt(primitive->texture_entry)->index);
  }
  quad->texture = texture != nullptr ? uint32_t(texture->texture) : 0;

  for (uint32_t i = 0; i < primitive->vertex_count; i++) {
    const MovieVertex* vertex = primitive->VertexAt(i);
    Vector& position = frame->positions[i];
    position.x = vertex->x;
    position.y = vertex->y;
    position.z = 0.0f;
    position.w = 1.0f;
    g.Call(sub_820AB518, &position, context, &position);  // Vector::Transform
    g.ctx.fpscr.disableFlushMode();

    Vector& uv = frame->texcoords[i];
    if (texture != nullptr) {
      uv.x = float(int32_t(vertex->u)) / float(uint32_t(texture->width));
      uv.y = float(int32_t(vertex->v)) / float(uint32_t(texture->height));
    } else {
      uv.x = 0.0f;
      uv.y = 0.0f;
    }

    float red = float(int32_t(vertex->red)) * kColorScale * float(context->red);
    float green = float(int32_t(vertex->green)) * kColorScale * float(context->green);
    float blue = float(int32_t(vertex->blue)) * kColorScale * float(context->blue);
    const float alpha = float(vertex->alpha) * float(context->alpha);
    if (primitive->squared != 0) {
      red = red * red;
      green = green * green;
      blue = blue * blue;
    }
    MovieColorWords& color = frame->colors[i];
    color.r = ColorWord(red);
    color.g = ColorWord(green);
    color.b = ColorWord(blue);
    color.a = ColorWord(alpha);
  }

  SetQuadClip(quad, context);
  quad->blend = primitive->squared;
  quad->unknown_72 = 0;
  quad->scaled = context->scaled;
  DrawQuad(g, quad, uint32_t(primitive->node.layer));
}

// The recompiled UiMovie::DrawPrimitive, for a primitive with more
// vertices than its frame has room for (the original runs past them).
void DrawPrimitiveRecompiled(Guest& g, MovieDrawContext* context, MoviePrimitive* primitive,
                             uint32_t layer) {
  g.ctx.r3.u64 = AddressOf(context);
  g.ctx.r5.u64 = AddressOf(primitive);
  g.ctx.r6.u64 = layer;
  __imp__sub_82584A40(g.ctx, g.base);
}

// ---------------------------------------------------------------------------
// Texts.

// A queued text copied into a list's slot (the matrix by Matrix::Copy).
void CopyText(Guest& g, MovieTextItem* to, const MovieTextItem* from) {
  to->node = from->node;
  g.Call(sub_820A0E18, &to->matrix, &from->matrix);  // Matrix::Copy
  to->depth = from->depth;
  to->red = from->red;
  to->green = from->green;
  to->blue = from->blue;
  to->alpha = from->alpha;
  to->clip = from->clip;
}

// TextList::Append: the text appended to the list, which grows to twice
// its size when full (sub_82584350).
void AppendText(Guest& g, MovieTextList* list, const MovieTextItem* text) {
  if (!(list->count < list->capacity)) {
    g.Call(sub_82584350, list, uint32_t(list->capacity) << 1);  // TextList::Grow
  }
  const uint32_t index = list->count;
  MovieTextItem* slot = guest::Element<MovieTextItem>(list->items.address, index);
  list->count = index + 1;
  CopyText(g, slot, text);
}

// UiMovie::QueueText: the text node's transform applied; with a text list,
// a text item made with the node, the context's matrix, color and clip and
// the layer's depth, appended, and the item destroyed. The item is on the
// guest stack (the original's frame, +96): the recompiled Matrix::Copy
// writes its matrix.
void QueueText(Guest& g, MovieDrawContext* context, MovieNode* node, uint32_t layer) {
  guest::StackFrame stack(g.ctx, 272);
  MovieTextItem* item = At<MovieTextItem>(stack.Local(96));
  ApplyTransform(g, context, node);
  if (context->text_list == nullptr) {
    return;
  }
  MakeTextItem(item);
  item->node = node;
  g.Call(sub_820A0E18, &item->matrix, &context->matrix);  // Matrix::Copy
  g.ctx.fpscr.disableFlushMode();
  item->depth = float(LayerDepth(layer));
  item->red = context->red;
  item->green = context->green;
  item->blue = context->blue;
  item->alpha = context->alpha;
  item->clip = context->clip;
  AppendText(g, context->text_list, item);
  ClearTextItem(item);
}

// ---------------------------------------------------------------------------
// The draw.

// UiMovie::DrawNode: the node's transform applied to the context, then its
// children (a range of the scene's child list), last first, each with its
// own copy of the context: skipped when hidden or scaled to nothing, groups
// and scenes walked the same way (a scene's children from its own list),
// pictures and primitives recorded, texts queued, other tags drawing
// nothing. A child's layer adds to the node's (on 64 bits; the draws take
// its low word). The children's context is on the guest stack (the
// original's frame, +80): the recompiled Matrix::Copy writes its matrix.
void DrawNode(Guest& g, MovieDrawContext* context, MovieGroup* scene, MovieGroup* node,
              uint64_t layer) {
  guest::StackFrame stack(g.ctx, 272);
  MovieDrawContext* child_context = At<MovieDrawContext>(stack.Local(80));
  ApplyTransform(g, context, &node->node);
  const uint32_t last = uint32_t(int32_t(node->first_child)) + uint32_t(int32_t(node->child_count));
  for (uint32_t i = last - 1; int32_t(i) >= int32_t(node->first_child); i--) {
    MovieNode* child = Child(scene, i);
    g.ctx.fpscr.disableFlushMode();
    if (child->visible == 0 || NearZero(child->scale_x) || NearZero(child->scale_y)) {
      continue;
    }
    CopyContext(g, child_context, context);
    const uint64_t child_layer = layer + uint32_t(child->layer);
    MovieGroup* group = reinterpret_cast<MovieGroup*>(child);
    switch (uint32_t(child->tag)) {
      case kTagGroup:
        DrawNode(g, child_context, scene, group, child_layer);
        break;
      case kTagScene:
        DrawNode(g, child_context, group, group, child_layer);
        break;
      case kTagPicture:
        DrawPicture(g, child_context, reinterpret_cast<MoviePicture*>(child));
        break;
      case kTagPrimitive: {
        MoviePrimitive* primitive = reinterpret_cast<MoviePrimitive*>(child);
        if (primitive->vertex_count > kPrimitiveRoom) {
          DrawPrimitiveRecompiled(g, child_context, primitive, uint32_t(child_layer));
        } else {
          DrawPrimitive(g, child_context, primitive);
        }
        break;
      }
      case kTagText:
        QueueText(g, child_context, child, uint32_t(child_layer));
        break;
      default:
        break;
    }
  }
}

}  // namespace

// UiMovie::Draw: the scene drawn at layer 0 with the movie's textures and
// text list.
void DrawMovie(Guest& g, MovieDrawContext* context, MovieGroup* scene, MovieWords* textures,
               MovieTextList* text_list, uint32_t unknown_104) {
  context->unknown_104 = unknown_104;
  context->textures = textures;
  context->text_list = text_list;
  DrawNode(g, context, scene, scene, 0);
}

// ---------------------------------------------------------------------------
// The tracks' key curves (from UiMovie::ApplyTrack, with t how far between
// the keys, 0 to 1).

// The eased blend of two integer keys: the ease from the second key's two
// curve values.
int32_t EaseInteger(Guest& g, const MovieTrackKey* from, const MovieTrackKey* to, double t) {
  g.ctx.fpscr.disableFlushMode();
  const double one = 1.0, three = double(kThree);
  const double rest = Single(one - t);
  const double out = Single(Single(Single(double(float(to->curve_16)) * t) * three) * rest);
  const double in = Single(std::fma(Single(double(float(to->curve_12)) * rest), three, t));
  const double eased = Single(std::fma(Single(in * t), t, Single(out * rest)));
  const double to_value = double(float(to->IntValue()));
  const double from_value = double(float(from->IntValue()));
  return TruncateToInt(Single(std::fma(from_value, Single(one - eased), Single(to_value * eased))));
}

namespace {

// The cubic (Bezier) between two keys' values: control points a third of
// the keys' time apart along the tangents of the second key's two curve
// angles (tan, sub_8258E2F0).
double Cubic(Guest& g, const MovieTrackKey* from, const MovieTrackKey* to, double t,
             double from_value, double to_value) {
  const double one = 1.0, third = double(kThird), three = double(kThree);
  const double rest = Single(one - t);
  const double span = Single(double(float(to->time)) - double(float(from->time)));
  g.ctx.f1.f64 = double(float(to->curve_16));
  g.Call(sub_8258E2F0);  // tan
  const double tan_out = Single(g.ctx.f1.f64);
  g.ctx.f1.f64 = double(float(to->curve_12));
  g.Call(sub_8258E2F0);  // tan
  g.ctx.fpscr.disableFlushMode();
  const double tan_in = Single(g.ctx.f1.f64);

  const double control_1 = Single(std::fma(Single(tan_out * span), third, from_value));
  const double control_2 = Single(std::fma(Single(tan_in * span), third, to_value));
  const double term_1 = Single(Single(Single(Single(control_1 * rest) * rest) * t) * three);
  const double term_2 = Single(Single(Single(control_2 * rest) * t) * t);
  const double term_3 = Single(Single(to_value * t) * t);
  const double term_4 = Single(Single(from_value * rest) * rest);
  double value = Single(std::fma(term_2, three, term_1));
  value = Single(std::fma(term_3, t, value));
  return Single(std::fma(term_4, rest, value));
}

}  // namespace

double CubicFloat(Guest& g, const MovieTrackKey* from, const MovieTrackKey* to, double t) {
  g.ctx.fpscr.disableFlushMode();
  return Cubic(g, from, to, t, double(from->FloatValue()), double(to->FloatValue()));
}

int32_t CubicInteger(Guest& g, const MovieTrackKey* from, const MovieTrackKey* to, double t) {
  g.ctx.fpscr.disableFlushMode();
  const double from_value = double(float(from->IntValue()));
  const double to_value = double(float(to->IntValue()));
  return TruncateToInt(Cubic(g, from, to, t, from_value, to_value));
}

namespace {

// ---------------------------------------------------------------------------
// The movie's rest pose.

void RestorePicture(MoviePicture* live, const MoviePicture* rest) {
  live->width = rest->width;
  live->height = rest->height;
  live->texture_entry = rest->texture_entry;
  std::memcpy(live->unknown_112, rest->unknown_112, sizeof(live->unknown_112));
  live->source_left = rest->source_left;
  live->source_top = rest->source_top;
  live->source_right = rest->source_right;
  live->source_bottom = rest->source_bottom;
  live->flip_x = rest->flip_x;
  live->flip_y = rest->flip_y;
}

void RestoreText(MovieTextNode* live, const MovieTextNode* rest) {
  live->box_width = rest->box_width;
  live->box_height = rest->box_height;
  live->letter_spacing = rest->letter_spacing;
  live->line_spacing = rest->line_spacing;
  live->horizontal_align = rest->horizontal_align;
  live->vertical_align = rest->vertical_align;
  live->unknown_132 = rest->unknown_132;
  live->unknown_136 = rest->unknown_136;
}

// A primitive's words and its vertices (as many as the rest pose has).
void RestorePrimitive(MoviePrimitive* live, MoviePrimitive* rest) {
  live->squared = rest->squared;
  live->texture_entry = rest->texture_entry;
  std::memcpy(live->unknown_108, rest->unknown_108, sizeof(live->unknown_108));
  for (uint32_t k = 0; k < rest->vertex_count; k++) {
    *live->VertexAt(k) = *rest->VertexAt(k);
  }
}

}  // namespace

// UiMovie::RestorePose: the live scene's nodes put back to the rest
// scene's, child by child, last first (the children of `live_group` when
// both groups are given, else of the scene). Groups recurse within the same
// scenes, scenes into themselves; pictures, primitives and texts also get
// their own fields back, TFM nodes only the common ones.
void RestorePose(MovieGroup* live_scene, MovieGroup* rest_scene, MovieGroup* live_group,
                 MovieGroup* rest_group) {
  MovieGroup* range = live_group != nullptr && rest_group != nullptr ? live_group : live_scene;
  const uint32_t last =
      uint32_t(int32_t(range->first_child)) + uint32_t(int32_t(range->child_count));
  for (uint32_t i = last - 1; int32_t(i) >= int32_t(range->first_child); i--) {
    MovieNode* live = Child(live_scene, i);
    MovieNode* rest = Child(rest_scene, i);
    const uint32_t tag = live->tag;
    if (tag != kTagGroup && tag != kTagPicture && tag != kTagPrimitive && tag != kTagScene &&
        tag != kTagTransform && tag != kTagText) {
      continue;
    }
    live->SetPose(*rest);
    switch (tag) {
      case kTagGroup:
        RestorePose(live_scene, rest_scene, reinterpret_cast<MovieGroup*>(live),
                    reinterpret_cast<MovieGroup*>(rest));
        break;
      case kTagScene:
        RestorePose(reinterpret_cast<MovieGroup*>(live), reinterpret_cast<MovieGroup*>(rest),
                    nullptr, nullptr);
        break;
      case kTagPicture:
        RestorePicture(reinterpret_cast<MoviePicture*>(live), reinterpret_cast<MoviePicture*>(rest));
        break;
      case kTagPrimitive:
        RestorePrimitive(reinterpret_cast<MoviePrimitive*>(live),
                         reinterpret_cast<MoviePrimitive*>(rest));
        break;
      case kTagText:
        RestoreText(reinterpret_cast<MovieTextNode*>(live), reinterpret_cast<MovieTextNode*>(rest));
        break;
      default:
        break;
    }
  }
}

namespace {

// ---------------------------------------------------------------------------
// The animations.

// The property descriptors' table (64 entries), the byte set once it's
// filled, and the first-use guard of its clear (writable data).
constexpr uint32_t kTrackProperties = 0x82973088;
constexpr uint32_t kTrackPropertiesReady = 0x82973080;
constexpr uint32_t kTrackPropertiesGuard = 0x82973388;
constexpr uint32_t kTrackPropertyCount = 64;
constexpr uint32_t kTrackPropertiesCleanUp = 0x825C0778;  // Registered with atexit.

// The 48 descriptors the game fills (the rest stay cleared): the node
// offset, the type, set (not added to), in the node's vertex array.
struct TrackPropertyEntry {
  uint32_t offset, type;
  bool set, in_array;
};
constexpr TrackPropertyEntry kTrackPropertyEntries[] = {
    {12, 0, false, false},   {16, 0, false, false},   {20, 1, false, false},   // x, y, pivot x
    {24, 1, false, false},   {28, 0, true, false},    {32, 0, true, false},    // pivot y, scale
    {36, 0, false, false},   {40, 1, false, false},   {44, 1, false, false},   // turn, clip
    {48, 1, false, false},   {52, 1, false, false},   {56, 1, true, false},    // clip, red
    {60, 1, true, false},    {64, 1, true, false},    {68, 0, true, false},    // green, blue, alpha
    {80, 2, true, false},    {72, 1, false, false},   {76, 2, false, false},   // visible, layer, +76
    {76, 2, false, false},   {0, 0, false, true},     {4, 0, false, true},     // a vertex's x, y
    {8, 1, true, true},      {12, 1, true, true},     {16, 1, true, true},     // its color
    {20, 0, true, true},     {24, 1, false, true},    {28, 1, false, true},    // its alpha, u, v
    {100, 1, true, false},   {104, 1, true, false},   {108, 1, true, false},   // a primitive's
    {100, 0, false, false},  {104, 0, false, false},  {108, 1, true, false},   // a picture's
    {112, 1, true, false},   {116, 1, false, false},  {120, 1, false, false},
    {124, 1, false, false},  {128, 1, false, false},  {132, 2, false, false},
    {136, 2, false, false},  {100, 0, false, false},  {104, 0, false, false},  // a text's
    {116, 0, false, false},  {120, 0, false, false},  {124, 1, true, false},
    {128, 1, true, false},   {132, 1, true, false},   {136, 2, true, false},
};
static_assert(std::size(kTrackPropertyEntries) == 48);

}  // namespace

// UiMovie::TrackProperty: property `id`'s descriptor. On first use the
// table is cleared (and its clean-up registered with atexit, sub_8258C7E8);
// the first call with the table not yet filled fills it. The descriptors'
// last two bytes are the original's stack leftovers; 0 here (nothing reads
// them).
MovieTrackProperty* TrackProperty(Guest& g, uint32_t id) {
  MovieTrackProperty* table = At<MovieTrackProperty>(kTrackProperties);
  u32_be* guard = At<u32_be>(kTrackPropertiesGuard);
  if ((*guard & 1) == 0) {
    *guard = *guard | 1;
    for (uint32_t i = 0; i < kTrackPropertyCount; i++) {
      table[i].offset = 0;
      table[i].type = 0;
      table[i].set = 0;
      table[i].in_array = 0;
    }
    g.Call(sub_8258C7E8, kTrackPropertiesCleanUp);  // atexit
  }
  uint8_t* ready = At<uint8_t>(kTrackPropertiesReady);
  if (*ready == 0) {
    for (uint32_t i = 0; i < std::size(kTrackPropertyEntries); i++) {
      const TrackPropertyEntry& entry = kTrackPropertyEntries[i];
      table[i].offset = entry.offset;
      table[i].type = entry.type;
      table[i].set = entry.set ? 1 : 0;
      table[i].in_array = entry.in_array ? 1 : 0;
      table[i].unknown_10[0] = 0;
      table[i].unknown_10[1] = 0;
    }
    *ready = 1;
  }
  return guest::Element<MovieTrackProperty>(kTrackProperties, id);
}

namespace {

// UiMovie::ApplyTrack (sub_82585C40) for one track with the keys around
// the time and how far between them: through Guest::Call, so subtick's
// wrapper runs.
void ApplyTrack(Guest& g, MovieNode* node, MovieTrack* track, MovieTrackKey* from,
                MovieTrackKey* to, float t) {
  g.ctx.f1.f64 = double(t);
  g.Call(sub_82585C40, node, track, from, to);
}

// One track of a node at the animation's `time`: the keys the time is
// between found (by the track's own time from its start, as the game
// compares) and the track applied at how far between them (clamped to 0 to
// 1; 0 for keys at one time). Before the first key, the first; at or after
// the last, the last; the animation's length stands in for a missing second
// key's time.
void ApplyNodeTrack(Guest& g, MovieAnimation* animation, MovieNode* node, MovieTrack* track,
                    double time) {
  MovieKeyList* keys = animation->KeyList(track->key_list);
  g.ctx.fpscr.disableFlushMode();
  const double local = Single(time - double(float(track->start)));
  if (local < 0.0) {
    return;
  }
  MovieTrackKey* from = nullptr;
  MovieTrackKey* to = nullptr;
  bool found = false;
  if (keys->count - 1 != 0) {
    uint32_t i = 0;
    do {
      if (!(double(keys->Key(i)->time) > local) && time < double(keys->Key(i + 1)->time)) {
        from = keys->Key(i);
        to = keys->Key(i + 1);
        found = from != nullptr;
        break;
      }
      i++;
    } while (i < keys->count - 1);
  }
  if (!found) {
    if (time < double(keys->Key(0)->time)) {
      from = nullptr;
      to = keys->Key(0);
    } else if (!(double(keys->Key(keys->count - 1)->time) > time)) {
      to = nullptr;
      from = keys->Key(keys->count - 1);
    }
  }
  g.ctx.fpscr.disableFlushMode();
  const double from_time = from != nullptr ? double(from->time) : 0.0;
  const double to_time = to != nullptr ? double(to->time) : double(float(animation->length));
  const double span = Single(to_time - from_time);
  double t = 0.0;
  if (!(span == 0.0)) {
    t = Single(Single(local - from_time) / span);
    if (t < 0.0) {
      t = 0.0;
    } else if (t > 1.0) {
      t = 1.0;
    }
  }
  ApplyTrack(g, node, track, from, to, float(t));
}

}  // namespace

// UiMovie::ApplyAnimation: the animation at `time` on the node: each node
// track list's tracks on the node's children with its id, then each
// sub-animation (from its start on) at its own mapped time, on the node or
// on the children with its node id. Whether it's still going: before its
// end, or for one running backwards past 0.
bool ApplyAnimation(Guest& g, MovieGroup* node, MovieAnimation* animation, double time,
                    uint32_t backwards) {
  for (uint32_t i = 0; i < animation->node_track_list_count; i++) {
    MovieNodeTrackList* list = animation->NodeTrackList(i);
    for (uint32_t j = 0; j < node->scene_child_count; j++) {
      MovieNode* child = Child(node, j);
      if (child->id != list->id) {
        continue;
      }
      for (uint32_t k = 0; k < list->track_count; k++) {
        ApplyNodeTrack(g, animation, child, list->Track(k), time);
      }
    }
  }

  for (uint32_t i = 0; i < animation->sub_animation_count; i++) {
    MovieSubAnimation* sub = animation->SubAnimation(i);
    g.ctx.fpscr.disableFlushMode();
    const float start = sub->start;
    if (time < double(start)) {
      continue;
    }
    const float end = sub->end;
    double amount = 0.0;
    if (!(Single(double(end) - double(start)) == 0.0)) {
      amount = Single(Single(time - double(start)) / Single(double(end) - double(start)));
      if (amount > 1.0) {
        amount = 1.0;
      }
    }
    const double from = double(float(sub->from));
    const double sub_time = Single(std::fma(Single(double(float(sub->to)) - from), amount, from));
    MovieAnimation* played = At<MovieAnimation>(sub->animation);
    if (sub->node_id == 0) {
      ApplyAnimation(g, node, played, sub_time, backwards);
      continue;
    }
    for (uint32_t j = 0; j < node->scene_child_count; j++) {
      MovieNode* child = Child(node, j);
      if (child->id == sub->node_id) {
        ApplyAnimation(g, reinterpret_cast<MovieGroup*>(child), played, sub_time, backwards);
      }
    }
  }

  g.ctx.fpscr.disableFlushMode();
  if (int32_t(backwards) != 0) {
    return time > 0.0;
  }
  return time < double(float(animation->length));
}

// UiMovie::SetRenderer: the renderer and command list the movies draw with.
void SetMovieRenderer(uint32_t renderer, uint32_t command_list) {
  *At<u32_be>(kRenderer) = renderer;
  *At<u32_be>(kCommandList) = command_list;
}

}  // namespace game

using namespace game;

#define SBX_UI_MOVIE_DRAW_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                   \
  if (!On()) {                           \
    __imp__##name(ctx, base);            \
    return;                              \
  }                                      \
  Guest g(ctx, base);

// MovieChunks::Find: r3 a chunk list, r4 a tag; the chunk tagged so, or 0
// at the list's end (its end chunk when r4 is "TRM").
extern "C" REX_FUNC(sub_82582CC0) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582CC0)
  ctx.r3.u64 = AddressOf(FindChunk(At<MovieChunk>(ctx.r3.u32), ctx.r4.u32));
}

// Primitive::TextureEntry: r3 a primitive, r4 the index.
extern "C" REX_FUNC(sub_82582D10) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582D10)
  ctx.r3.u64 = AddressOf(At<MoviePrimitive>(ctx.r3.u32)->TextureEntryAt(ctx.r4.u32));
}

// Primitive::Vertex: r3 a primitive, r4 the index.
extern "C" REX_FUNC(sub_82582D20) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582D20)
  ctx.r3.u64 = AddressOf(At<MoviePrimitive>(ctx.r3.u32)->VertexAt(ctx.r4.u32));
}

// Picture::TextureEntry: r3 a picture, r4 the index.
extern "C" REX_FUNC(sub_82582D40) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582D40)
  ctx.r3.u64 = AddressOf(At<MoviePicture>(ctx.r3.u32)->TextureEntryAt(ctx.r4.u32));
}

// UiMovie::Child: r3 a group or scene, r4 the index.
extern "C" REX_FUNC(sub_82582D50) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582D50)
  ctx.r3.u64 = AddressOf(Child(At<MovieGroup>(ctx.r3.u32), ctx.r4.u32));
}

// Scene::Animation: r3 a scene, r4 the index.
extern "C" REX_FUNC(sub_82582D68) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582D68)
  ctx.r3.u64 = At<MovieGroup>(ctx.r3.u32)->Animation(ctx.r4.u32);
}

// UiMovie::FixUpScene: r3 the scene, r4 the movie file. Returns 1.
extern "C" REX_FUNC(sub_82582D80) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582D80)
  FixUpScene(At<MovieGroup>(ctx.r3.u32), At<MovieFile>(ctx.r4.u32));
  ctx.r3.u64 = 1;
}

// Clip::Intersect: r3 a clip narrowed by r4; returns r3.
extern "C" REX_FUNC(sub_82582E28) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582E28)
  IntersectClip(At<MovieClip>(ctx.r3.u32), *At<MovieClip>(ctx.r4.u32));
}

// UiMovie::ResetClip: r3 the key (the whole register, as the renderer is
// handed it).
extern "C" REX_FUNC(sub_82582EA8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582EA8)
  ResetClip(g, ctx.r3.u64);
}

// DrawContext::DrawContext: r3 the context, returned.
extern "C" REX_FUNC(sub_82583028) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82583028)
  InitContext(At<MovieDrawContext>(ctx.r3.u32));
}

// DrawContext::Copy: r3 the new context, r4 the parent's; returns r3.
extern "C" REX_FUNC(sub_825830F0) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825830F0)
  const uint32_t to = ctx.r3.u32;
  CopyContext(g, At<MovieDrawContext>(to), At<MovieDrawContext>(ctx.r4.u32));
  ctx.r3.u64 = to;
}

// UiMovie::ResetClipAfter: the clip reset keyed r4 (the movie update's
// last draws, keys 676 and 148).
extern "C" REX_FUNC(sub_825831B8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825831B8)
  ResetClip(g, ctx.r4.u64);
}

// UiMovie::RecordTexturedQuad and RecordQuad: r3 the quad, r4 its key.
extern "C" REX_FUNC(sub_825831C8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825831C8)
  RecordQuad(g, At<MovieQuad>(ctx.r3.u32), ctx.r4.u64, true);
}

extern "C" REX_FUNC(sub_82583790) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82583790)
  RecordQuad(g, At<MovieQuad>(ctx.r3.u32), ctx.r4.u64, false);
}

// TextItem::Copy: r3 the slot, r4 the text; returns r3.
extern "C" REX_FUNC(sub_82583D18) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82583D18)
  const uint32_t to = ctx.r3.u32;
  CopyText(g, At<MovieTextItem>(to), At<MovieTextItem>(ctx.r4.u32));
  ctx.r3.u64 = to;
}

// UiMovie::ApplyTransform: r3 the context, r4 the node.
extern "C" REX_FUNC(sub_82583DC0) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82583DC0)
  ApplyTransform(g, At<MovieDrawContext>(ctx.r3.u32), At<MovieNode>(ctx.r4.u32));
}

// UiMovie::DrawQuad: r3 the quad, r4 its key (f1, a depth, isn't read).
extern "C" REX_FUNC(sub_825842E0) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825842E0)
  DrawQuad(g, At<MovieQuad>(ctx.r3.u32), ctx.r4.u64);
}

// UiMovie::DrawPicture: r3 the context, r5 the picture (r6, the layer, only
// gives a depth nothing reads).
extern "C" REX_FUNC(sub_82584438) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82584438)
  DrawPicture(g, At<MovieDrawContext>(ctx.r3.u32), At<MoviePicture>(ctx.r5.u32));
}

// UiMovie::DrawPrimitive: as DrawPicture.
extern "C" REX_FUNC(sub_82584A40) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82584A40)
  MoviePrimitive* primitive = At<MoviePrimitive>(ctx.r5.u32);
  if (primitive->vertex_count > kPrimitiveRoom) {
    __imp__sub_82584A40(ctx, base);
    return;
  }
  DrawPrimitive(g, At<MovieDrawContext>(ctx.r3.u32), primitive);
}

// TextList::Append: r3 the list, r4 the text.
extern "C" REX_FUNC(sub_82584DF8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82584DF8)
  AppendText(g, At<MovieTextList>(ctx.r3.u32), At<MovieTextItem>(ctx.r4.u32));
}

// UiMovie::QueueText: r3 the context, r5 the text node, r6 the layer (its
// low word).
extern "C" REX_FUNC(sub_82584E68) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82584E68)
  QueueText(g, At<MovieDrawContext>(ctx.r3.u32), At<MovieNode>(ctx.r5.u32), uint32_t(ctx.r6.u64));
}

// UiMovie::DrawNode: r3 the context, r4 the scene, r5 the node, r6 the
// layer.
extern "C" REX_FUNC(sub_82584F40) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82584F40)
  DrawNode(g, At<MovieDrawContext>(ctx.r3.u32), At<MovieGroup>(ctx.r4.u32),
           At<MovieGroup>(ctx.r5.u32), ctx.r6.u64);
}

// UiMovie::Draw: r3 the context (movie + 64), r4 the scene, r5 the
// textures, r6 the text list, r7 the context's +104 word (-1).
extern "C" REX_FUNC(sub_825850F8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825850F8)
  DrawMovie(g, At<MovieDrawContext>(ctx.r3.u32), At<MovieGroup>(ctx.r4.u32),
            At<MovieWords>(ctx.r5.u32), At<MovieTextList>(ctx.r6.u32), ctx.r7.u32);
}

// The integer and float keys' curves: r3 and r4 the keys, f1 t. The
// integers in r3, the float in f1.
extern "C" REX_FUNC(sub_82585118) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82585118)
  ctx.r3.u64 = uint32_t(
      EaseInteger(g, At<MovieTrackKey>(ctx.r3.u32), At<MovieTrackKey>(ctx.r4.u32), ctx.f1.f64));
}

extern "C" REX_FUNC(sub_82585228) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82585228)
  const double t = ctx.f1.f64;
  ctx.f1.f64 = CubicFloat(g, At<MovieTrackKey>(ctx.r3.u32), At<MovieTrackKey>(ctx.r4.u32), t);
}

extern "C" REX_FUNC(sub_82585310) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82585310)
  const double t = ctx.f1.f64;
  ctx.r3.u64 =
      uint32_t(CubicInteger(g, At<MovieTrackKey>(ctx.r3.u32), At<MovieTrackKey>(ctx.r4.u32), t));
}

// XmdFile::Entry: r3 the file, r4 the index; 0 past its count.
extern "C" REX_FUNC(sub_825863C8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825863C8)
  ctx.r3.u64 = XmdEntry(At<XmdFile>(ctx.r3.u32), ctx.r4.u32);
}

// XmdFile::Check: r3 the file; whether it's one.
extern "C" REX_FUNC(sub_825863F0) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825863F0)
  ctx.r3.u64 = CheckXmd(At<XmdFile>(ctx.r3.u32)) ? 1 : 0;
}

// MovieFile::Scene: r3 the file; its scene, or 0.
extern "C" REX_FUNC(sub_825865A8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825865A8)
  ctx.r3.u64 = AddressOf(FileScene(At<MovieFile>(ctx.r3.u32)));
}

// UiMovie::FixUpFile: r3 the file; whether it was one.
extern "C" REX_FUNC(sub_825865E8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_825865E8)
  ctx.r3.u64 = FixUpFile(At<MovieFile>(ctx.r3.u32)) ? 1 : 0;
}

// UiMovie::FixUpAnimation: r3 the animation, r4 the file. Returns 1.
extern "C" REX_FUNC(sub_8258BD30) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_8258BD30)
  FixUpAnimation(At<MovieAnimation>(ctx.r3.u32), At<MovieFile>(ctx.r4.u32));
  ctx.r3.u64 = 1;
}

// UiMovie::RestorePose: r3 the movie (not read), r4 the live scene, r5 the
// rest scene, r6 and r7 a live and a rest group (or 0).
extern "C" REX_FUNC(sub_82119D50) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82119D50)
  RestorePose(At<MovieGroup>(ctx.r4.u32), At<MovieGroup>(ctx.r5.u32), At<MovieGroup>(ctx.r6.u32),
              At<MovieGroup>(ctx.r7.u32));
}

// UiMovie::SetRenderer: r3 the renderer, r4 the command list.
extern "C" REX_FUNC(sub_82582E90) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82582E90)
  SetMovieRenderer(ctx.r3.u32, ctx.r4.u32);
}

// UiMovie::TrackProperty: r3 a property id; its descriptor.
extern "C" REX_FUNC(sub_82585560) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82585560)
  ctx.r3.u64 = AddressOf(TrackProperty(g, ctx.r3.u32));
}

// UiMovie::ApplyAnimation: r3 the node, r4 the animation, f1 the time, r6
// whether it runs backwards. Whether it's still going.
extern "C" REX_FUNC(sub_82585FD8) {
  SBX_UI_MOVIE_DRAW_FALLBACK(sub_82585FD8)
  const double time = ctx.f1.f64;
  ctx.r3.u64 =
      ApplyAnimation(g, At<MovieGroup>(ctx.r3.u32), At<MovieAnimation>(ctx.r4.u32), time,
                     ctx.r6.u32)
          ? 1
          : 0;
}
