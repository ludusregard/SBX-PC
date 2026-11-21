// The game's text font (docs/game_data.md, "Font"): a glyph table and a
// TSPFONT sheet in one container, BUILD 22656.
//
// Font::LoadSheet (sub_82109280) fills the font's description from the
// container: +0 the table's header (cell width and height, u16 each), +4 the
// glyph entries, +8 the XPR, +12 the texture, +16 and +20 the texture's width
// and height (its level 0 description, sub_822A3160), +24 1.
//
// The glyph code (Font::AddGlyph, Font::ClipGlyph: src/game/strings_and_font.cpp)
// works out each glyph's texture coordinates in the sheet's texels: columns =
// width / cell width, u = column * cell width / width, v = row * cell height /
// height. The cell size also sets the glyph's size on screen, so it has to
// stay as it is. A sheet drawn at N times the table's cell size is given its
// size divided by N here, so the coordinates come out as the same fractions
// of the larger sheet and the text keeps its size and spacing:
//
// - A sheet of exactly N times the original's size (576 x 928): N from its
//   size (an HD redraw of the original's layout).
// - A container with an SBXF (sbxr_tools' font_upscale writes it, a third
//   file the game doesn't read): N its cell size over the table's, any grid
//   (2048 x 2048 of 56-pixel cells reads as 1024 x 1024: 36 columns of 28).
//   With its distance-field flag, the renderer is told the sheet's address
//   and spread, and the font shaders threshold the distance for it
//   (Renderer_SetDistanceFieldFont, glsl/boot/font_sdf.glsl).
//
// Any other sheet is left as loaded.

#include "guest.h"
#include "renderer/renderer.h"

#include <rex/logging.h>

#include <bit>

namespace {

// The original sheet's size.
constexpr uint32_t kSheetWidth = 576;
constexpr uint32_t kSheetHeight = 928;

// Offsets in the font's description.
constexpr uint32_t kTableHeader = 0;
constexpr uint32_t kTexture = 12;
constexpr uint32_t kTextureWidth = 16;
constexpr uint32_t kTextureHeight = 20;

// The fetch constant's place in a texture header (as in hooks/sidecar.cpp).
constexpr uint32_t kTextureFetch = 0x1C;

// SBXF (font_upscale's layout.rs): "SBXF", u32 version (1), u32 flags,
// u32 sheet pixels per cell, f32 spread, u32 columns, u32 rows; big-endian.
constexpr uint32_t kSbxfMagic = 0x53425846;  // "SBXF"
constexpr uint32_t kSbxfSize = 28;
constexpr uint32_t kSbxfDistanceField = 1;

struct Sbxf {
  uint32_t flags = 0, cell_pixels = 0, columns = 0, rows = 0;
  float spread = 0;
};

// A container's u32 (little-endian, as the archive's containers are).
uint32_t LoadLE32(uint8_t* base, uint32_t address) {
  return std::byteswap(guest::LoadU32(base, address));
}

// The container's SBXF: its third file, if it has one.
bool ReadSbxf(uint8_t* base, uint32_t container, Sbxf& out) {
  if (container == 0 || LoadLE32(base, container) != 3) {
    return false;
  }
  const uint32_t at = container + LoadLE32(base, container + 12);
  if (guest::LoadU32(base, at) != kSbxfMagic || guest::LoadU32(base, at + 4) != 1) {
    return false;
  }
  out.flags = guest::LoadU32(base, at + 8);
  out.cell_pixels = guest::LoadU32(base, at + 12);
  out.spread = std::bit_cast<float>(guest::LoadU32(base, at + 16));
  out.columns = guest::LoadU32(base, at + 20);
  out.rows = guest::LoadU32(base, at + 24);
  static_assert(kSbxfSize == 28);
  return true;
}

}  // namespace

// Font::LoadSheet: r3 the font, r4 the container, r6 the description.
extern "C" REX_FUNC(sub_82109280) {
  REX_FUNC_PROLOGUE();
  const uint32_t container = ctx.r4.u32;
  const uint32_t description = ctx.r6.u32;
  __imp__sub_82109280(ctx, base);

  const uint32_t width = guest::LoadU32(base, description + kTextureWidth);
  const uint32_t height = guest::LoadU32(base, description + kTextureHeight);

  Sbxf sbxf;
  if (ReadSbxf(base, container, sbxf)) {
    const uint32_t header = guest::LoadU32(base, description + kTableHeader);
    const uint32_t cell = guest::LoadU16(base, header);
    const uint32_t scale = cell != 0 && sbxf.cell_pixels % cell == 0 ? sbxf.cell_pixels / cell : 0;
    if (scale == 0 || width % scale != 0 || height % scale != 0) {
      REXLOG_WARN("font: SBXF's {}-pixel cells don't fit the table's {} on a {}x{} sheet; left as "
                  "loaded",
                  sbxf.cell_pixels, cell, width, height);
      return;
    }
    // PATCH: the glyph coordinates are worked out on the sheet's size at the
    // table's cell size.
    guest::StoreU32(base, description + kTextureWidth, width / scale);
    guest::StoreU32(base, description + kTextureHeight, height / scale);
    const bool distance_field = (sbxf.flags & kSbxfDistanceField) != 0;
    if (distance_field) {
      const uint32_t texture = guest::LoadU32(base, description + kTexture);
      const uint32_t base_address = guest::LoadU32(base, texture + kTextureFetch + 4) & 0xFFFFF000u;
      renderer::Renderer_SetDistanceFieldFont(base_address, sbxf.spread);
    }
    if (distance_field) {
      REXLOG_INFO("font: sheet is {}x{}, {}x{} cells of {} ({}x), a distance field (spread {}); "
                  "glyphs mapped as on {}x{}",
                  width, height, sbxf.columns, sbxf.rows, sbxf.cell_pixels, scale, sbxf.spread,
                  width / scale, height / scale);
    } else {
      REXLOG_INFO("font: sheet is {}x{}, {}x{} cells of {} ({}x); glyphs mapped as on {}x{}", width,
                  height, sbxf.columns, sbxf.rows, sbxf.cell_pixels, scale, width / scale,
                  height / scale);
    }
    return;
  }

  if (width == kSheetWidth && height == kSheetHeight) {
    return;
  }
  const uint32_t scale = width / kSheetWidth;
  if (scale < 2 || width != kSheetWidth * scale || height != kSheetHeight * scale) {
    REXLOG_INFO("font: sheet is {}x{}, not a multiple of {}x{}; left as loaded", width, height,
                kSheetWidth, kSheetHeight);
    return;
  }
  // PATCH: the glyph coordinates are worked out on the original sheet's size.
  guest::StoreU32(base, description + kTextureWidth, kSheetWidth);
  guest::StoreU32(base, description + kTextureHeight, kSheetHeight);
  REXLOG_INFO("font: sheet is {}x{} ({}x); glyphs mapped as on {}x{}", width, height, scale,
              kSheetWidth, kSheetHeight);
}
