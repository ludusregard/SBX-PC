// The car models' loads: what the first set's objects (scene_loads.h's
// CarModel, InteriorObject, CockpitObject, Wheels and the others) are
// loaded with besides their records, and the TGA reader the decals are
// decoded with.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/model.h"
#include "game/scene_loads.h"
#include "game/std_types.h"

namespace game {

using guest::i32_be;
using guest::ptr;
using guest::u16_be;
using guest::u32_be;

// --- The first set's car objects' loads ---

// What the accessories' set's load (its vtable slot 1) answers.
struct InteriorAccessoryLoad {
  uint8_t unknown_0[808];
  u32_be unknown_808;  // Cleared by Interior::Load for set a.
};
GUEST_OFFSET(InteriorAccessoryLoad, unknown_808, 808);

// A row of the wheels' defaults table (0x82004490, read-only): by the
// car's block, or 45 + a traffic car's index.
struct WheelDefaults {
  u32_be unknown_0;
  u32_be unknown_4[2];
  u32_be caliper_texture;
  u32_be unknown_16[2];
  guest::f32_be unknown_24[2];
  u32_be unknown_32[2];
  u32_be aftermarket_tires;
};
GUEST_SIZE(WheelDefaults, 44);

// --- The decals' TGA reader ---

// A TGA file's header, as the game keeps it (aligned; +3 and +9 never
// written).
struct TGAHeader {
  uint8_t id_length;
  uint8_t color_map_type;
  uint8_t image_type;
  uint8_t pad_3;
  u16_be color_map_first;
  u16_be color_map_length;
  uint8_t color_map_depth;
  uint8_t pad_9;
  u16_be x_origin;
  u16_be y_origin;
  u16_be width;
  u16_be height;
  uint8_t bits_per_pixel;
  uint8_t descriptor;
};
GUEST_SIZE(TGAHeader, 20);
GUEST_OFFSET(TGAHeader, width, 14);
GUEST_OFFSET(TGAHeader, bits_per_pixel, 18);

// A decoded TGA: the file, its header and its pixels as the file has them.
struct TGAImage {
  u32_be unknown_0;
  u32_be file;
  ptr<TGAHeader> header;
  u32_be pixels;
  u32_be size;
  u32_be width;
  u32_be height;
};
GUEST_SIZE(TGAImage, 28);
GUEST_OFFSET(TGAImage, pixels, 12);

struct TGAReader {
  String name;
  u32_be file_size;
  u32_be file;  // The file's buffer while it's read; freed with the reader.
  TGAHeader header;
  TGAImage image;
};
GUEST_OFFSET(TGAReader, file, 32);
GUEST_OFFSET(TGAReader, header, 36);
GUEST_OFFSET(TGAReader, image, 56);
GUEST_SIZE(TGAReader, 84);

// --- The car models' functions (car_models.cpp) ---

using guest::Guest;

struct PolledWheels;

// The plate's choices (the record's part order, 11 words passed by value):
// the plate's set of images, then the characters picked for its slots.
struct PlateWords {
  int32_t image_set;    // W0: which of each kind's images.
  int32_t kana;         // W1: slot 1's character, through the kana cells.
  int32_t region;       // W2: slot 5's cell (six a row).
  int32_t small[3];     // W3-W5: slots 2-4 (12 for none).
  int32_t digits_a[2];  // W6, W7: slots 6, 7.
  int32_t separator;    // W8: slot 8 drawn unless 12.
  int32_t digits_b[2];  // W9, W10: slots 9, 10.

  static PlateWords Of(const u32_be (&part_order)[11]) {
    int32_t w[11];
    for (int i = 0; i < 11; i++) {
      w[i] = int32_t(uint32_t(part_order[i]));
    }
    return {w[0], w[1], w[2], {w[3], w[4], w[5]}, {w[6], w[7]}, w[8], {w[9], w[10]}};
  }
};

void LoadSlotReset(LoadSlot* slot);
uint32_t LoadSlotLoad(Guest& g, LoadSlot* slot, uint32_t id, uint32_t context, uint32_t callback);
uint32_t LoadSlotLoadShared(Guest& g, LoadSlot* slot, uint32_t id, uint32_t context,
                            uint32_t callback);
uint32_t LoadSlotReleaseBuffer(Guest& g, LoadSlot* slot);
void DeferRelease(ScenePoolSet* set, const TexturePair& textures);
void TextureCreate(Guest& g, uint32_t format, uint32_t width, uint32_t height, u32_be* header_out,
                   u32_be* data_out);
void CarModelRelease(Guest& g, CarModel* model);
void CarModelReloadDecals(Guest& g, CarModel* model);
void CarModelMakePlate(Guest& g, u32_be* header_a, u32_be* data_a, u32_be* header_b,
                       u32_be* data_b, const PlateWords& words);
void CockpitSetVinylTextures(Guest& g, CockpitObject* cockpit);
void SceneWheelsSetUp(Guest& g, PolledWheels* wheels, uint32_t setup);

}  // namespace game
