// The UI movies' drawing: the draw context handed down a movie's node tree
// (its transform, clip, color, the movie's textures and text list), and
// ui_movie_draw.cpp's functions the other movie files call: the draw, the
// rest pose, the animations, the key curves and the movie file's fix-ups.
// The nodes are in ui_movie_nodes.h, the movie file and its animations in
// ui_movies.h.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/ui_movie_nodes.h"

namespace game {

using guest::f32_be;
using guest::ptr;
using guest::u32_be;

// What a node is drawn with, handed down the tree (a copy per child): its
// transform, clip, color, the movie's textures and text list (UiMovie +64,
// made by DrawContext::DrawContext).
struct MovieDrawContext {
  Matrix matrix;
  MovieClip clip;
  f32_be red, green, blue, alpha;  // 0 to 1
  ptr<MovieWords> textures;        // The movie's MovieTexture pointers.
  ptr<MovieTextList> text_list;
  u32_be unknown_104;  // -1 from UiMovie::Draw.
  u32_be scaled;       // Set once a node on the way down scales.
};
GUEST_SIZE(MovieDrawContext, 112);
GUEST_OFFSET(MovieDrawContext, clip, 64);
GUEST_OFFSET(MovieDrawContext, red, 80);
GUEST_OFFSET(MovieDrawContext, textures, 96);
GUEST_OFFSET(MovieDrawContext, scaled, 108);

struct MovieAnimation;
struct MovieChunk;
struct MovieFile;
struct MovieTrackKey;
struct MovieTrackProperty;
struct XmdFile;

// UiMovie::Child (sub_82582D50): a group's or scene's child `index`.
MovieNode* Child(MovieGroup* group, uint32_t index);
// MovieChunks::Find (sub_82582CC0): the chunk after `chunk` tagged `tag`.
MovieChunk* FindChunk(MovieChunk* chunk, uint32_t tag);

// UiMovie::SetRenderer (sub_82582E90): the renderer and command list the
// movies draw with.
void SetMovieRenderer(uint32_t renderer, uint32_t command_list);
// DrawContext::DrawContext (sub_82583028).
void InitContext(MovieDrawContext* context);
// UiMovie::Draw (sub_825850F8): the scene drawn at layer 0.
void DrawMovie(guest::Guest& g, MovieDrawContext* context, MovieGroup* scene,
               MovieWords* textures, MovieTextList* text_list, uint32_t unknown_104);
// UiMovie::DrawQuad (sub_825842E0): a quad recorded at `key`.
void DrawQuad(guest::Guest& g, MovieQuad* quad, uint64_t key);
// UiMovie::ResetClipAfter (sub_825831B8): the clip reset keyed `key`.
void ResetClip(guest::Guest& g, uint64_t key);

// UiMovie::RestorePose (sub_82119D50): the live scene's nodes put back
// from the rest scene's (a group's subtree when both groups are given).
void RestorePose(MovieGroup* live_scene, MovieGroup* rest_scene, MovieGroup* live_group,
                 MovieGroup* rest_group);

// UiMovie::TrackProperty (sub_82585560): a node property's descriptor.
MovieTrackProperty* TrackProperty(guest::Guest& g, uint32_t id);
// UiMovie::ApplyAnimation (sub_82585FD8): the animation at `time` on the
// node. Whether it's still going.
bool ApplyAnimation(guest::Guest& g, MovieGroup* node, MovieAnimation* animation, double time,
                    uint32_t backwards);
// The keys' curves (sub_82585118, sub_82585228, sub_82585310): an eased
// integer, a cubic float and a cubic integer, at `t` between the keys.
int32_t EaseInteger(guest::Guest& g, const MovieTrackKey* from, const MovieTrackKey* to,
                    double t);
double CubicFloat(guest::Guest& g, const MovieTrackKey* from, const MovieTrackKey* to, double t);
int32_t CubicInteger(guest::Guest& g, const MovieTrackKey* from, const MovieTrackKey* to,
                     double t);

// The movie file's load: XmdFile::Entry, XmdFile::Check, MovieFile::Scene
// and UiMovie::FixUpFile (sub_825863C8, sub_825863F0, sub_825865A8,
// sub_825865E8).
uint32_t XmdEntry(XmdFile* file, uint32_t index);
bool CheckXmd(XmdFile* file);
MovieGroup* FileScene(MovieFile* file);
bool FixUpFile(MovieFile* file);

}  // namespace game
