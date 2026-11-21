// The game's string table (the menus' and HUD's text, in UTF-16) and the
// font's pages.

#pragma once

#include "game/guest_types.h"

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u16_be;
using guest::u32_be;

// The strings' container files (DataFile), 17 of them, loaded and freed
// by a mask of files with a use count each.
struct StringTable {
  static constexpr int kFiles = 17;

  u32_be files[kFiles];   // Each a DataFile, or 0.
  u32_be loaded[kFiles];  // Set by the archive's callback.
  u32_be file_mask;       // The files asked for.
  uint8_t unknown_140[2048];
  u32_be use_counts[kFiles];
};
GUEST_SIZE(StringTable, 2256);
GUEST_OFFSET(StringTable, file_mask, 136);
GUEST_OFFSET(StringTable, use_counts, 2188);

// The head of a font page's glyph table: the size of a glyph's cell in the
// sheet, in texels.
struct GlyphTableHeader {
  u16_be cell_width;
  u16_be cell_height;
};
GUEST_SIZE(GlyphTableHeader, 4);

// A character's glyph: its cell in the sheet, and its advance (in texels)
// with the sheet's color channel it's drawn from in the low 2 bits.
struct Glyph {
  u16_be cell;
  u16_be advance_and_channel;

  uint32_t Advance() const { return uint32_t(advance_and_channel) >> 2; }
  uint32_t Channel() const { return advance_and_channel & 3; }
};
GUEST_SIZE(Glyph, 4);

// The font: up to four pages of glyphs. A page's glyph table is indexed by
// the character (UTF-16).
struct FontPage {
  ptr<GlyphTableHeader> header;
  ptr<Glyph> glyphs;
  u32_be xpr;
  u32_be texture;
  u32_be texture_width;
  u32_be texture_height;
  u32_be loaded;

  // The glyph table's entry for a character.
  const Glyph* GlyphFor(uint32_t c) const { return guest::Element<Glyph>(glyphs.address, c); }
};
GUEST_SIZE(FontPage, 28);
GUEST_OFFSET(FontPage, loaded, 24);

// How a text is drawn (each text's style, given to Font::Draw and
// Font::TextWidth). Positions and sizes are in pixels.
struct TextStyle {
  enum Align : uint32_t {
    kAlignStart = 0,  // Left or top.
    kAlignCenter = 1,
    kAlignEnd = 2,    // Right or bottom.
    kJustify = 3,
  };

  i32_be x;
  i32_be y;
  i32_be scale_origin_x;  // The point the text is scaled about, from x and y.
  i32_be scale_origin_y;
  f32_be rotation;  // Degrees; the text is turned when not a multiple of 360.
  uint8_t red;
  uint8_t green;
  uint8_t blue;
  uint8_t unknown_23;
  f32_be alpha;  // 0 to 1.
  f32_be scale_x;
  f32_be scale_y;
  f32_be glyph_scale_x;  // A glyph's size and advance from its cell's.
  f32_be glyph_scale_y;
  i32_be clip_left;  // The clip rectangle, all 0 for none.
  i32_be clip_top;
  i32_be clip_width;
  i32_be clip_height;
  u32_be page;
  uint8_t unknown_64[4];
  i32_be box_width;  // The box the text is aligned in.
  i32_be box_height;
  u32_be horizontal_align;
  u32_be vertical_align;
  i32_be letter_spacing;
  i32_be line_spacing;
  uint8_t unknown_92[4];
  f32_be second_offset_x;  // The second pass's glyphs from the first's, unscaled.
  f32_be second_offset_y;
  uint8_t unknown_104[4];
  u32_be second_page;  // The second pass's page.
  u32_be second_pass;  // Not 0: the text drawn again (never in play).
  i32_be layer;
};
GUEST_OFFSET(TextStyle, red, 20);
GUEST_OFFSET(TextStyle, alpha, 24);
GUEST_OFFSET(TextStyle, clip_left, 44);
GUEST_OFFSET(TextStyle, page, 60);
GUEST_OFFSET(TextStyle, box_width, 68);
GUEST_OFFSET(TextStyle, letter_spacing, 84);
GUEST_OFFSET(TextStyle, second_offset_x, 96);
GUEST_OFFSET(TextStyle, second_page, 108);
GUEST_OFFSET(TextStyle, layer, 116);

// A point of a text's second pass: where a glyph's copy goes.
struct FontPoint {
  f32_be x;
  f32_be y;
};
GUEST_SIZE(FontPoint, 8);

// The font itself (Font::Init makes it, 8,400 bytes, at [kFont]).
struct Font {
  static constexpr int kPages = 4;

  u32_be video_output;
  u32_be command_list;
  u32_be render_queue;
  u32_be page_mask;         // The pages asked for.
  ptr<TextStyle> style;     // The style being drawn (Font::SetParams).
  ptr<FontPage> pages[kPages];
  ptr<FontPage> page;       // The page being drawn.
  u32_be page_files[kPages];  // Each page's container, as loaded.
  f32_be start_x;           // The text's top left, the style's scaling done.
  f32_be start_y;
  FontPoint second_pass_points[1024];  // By the character's index in the text.
  f32_be letter_spacing;    // Pixels between glyphs, between lines.
  f32_be line_spacing;
  uint8_t unknown_8264[72];
  uint8_t colors[10][3];    // The colors' red, green, blue (Font::Font).
  uint8_t unknown_8366[2];
  u32_be vertex_declaration;  // Font::CreateShaders makes them.
  u32_be vertex_shader;
  u32_be pixel_shaders[4];    // One per channel of the sheet.
  uint8_t unknown_8392[8];
};
GUEST_SIZE(Font, 8400);
GUEST_OFFSET(Font, style, 16);
GUEST_OFFSET(Font, pages, 20);
GUEST_OFFSET(Font, page, 36);
GUEST_OFFSET(Font, page_files, 40);
GUEST_OFFSET(Font, start_x, 56);
GUEST_OFFSET(Font, letter_spacing, 8256);
GUEST_OFFSET(Font, colors, 8336);
GUEST_OFFSET(Font, vertex_declaration, 8368);
GUEST_OFFSET(Font, pixel_shaders, 8376);

// A glyph's vertex (two triangles, six vertices, a glyph).
struct GlyphVertex {
  f32_be x;
  f32_be y;
  f32_be z;
  u32_be color;  // a, r, g, b.
  f32_be u;
  f32_be v;
};
GUEST_SIZE(GlyphVertex, 24);

// The glyphs of a draw by the sheet's channel they're drawn from: how many
// (Font::CountGlyphs), and where the next one's vertices go.
struct FontChannels {
  u32_be glyph_counts[4];
  u32_be write_pointers[4];  // Each a GlyphVertex array.
  u32_be vertices[4];        // Each channel's first vertex (Font::DrawPass).
  u32_be first_slot;         // The draw's first command slot, while it's recorded.
};
GUEST_SIZE(FontChannels, 52);

// One of the font's four pixel shaders' sources (HLSL), one per channel of
// the sheet; set by a static initializer.
struct FontShaderSource {
  u32_be text;    // A C string.
  u32_be length;  // Its strlen.
  u32_be unknown_8;
};
GUEST_SIZE(FontShaderSource, 12);

constexpr uint32_t kFont = 0x826B58E0;         // Holds the font's address.
constexpr uint32_t kFontShaderSources = 0x826B58E4;  // Four FontShaderSource.
constexpr uint32_t kFontChannels = 0x826B58AC;       // A FontChannels.
constexpr uint32_t kStringTable = 0x826B5914;  // Holds the table's address.
// Each file's first string id, and the end of the last (18 words).
constexpr uint32_t kStringFileFirstIds = 0x82007DB0;

// The character that starts a new page of a text.
constexpr uint16_t kPageBreak = 0xFFD1;

// Characters the font doesn't draw: 0xFFD6 and 0xFFF6 to 0xFFFF set the
// color, '\n' starts a line.
constexpr uint16_t kColorCode = 0xFFD6;
constexpr uint16_t kFirstColorCode = 0xFFF6;

inline bool IsColorCode(uint16_t c) { return c == kColorCode || c >= kFirstColorCode; }

// ---- strings_and_font.cpp's functions the UI movies call.

// Strings::PageCount (sub_8210A6F0): how many page breaks a text has.
uint32_t PageCount(const u16_be* text);
// Strings::CopyPage (sub_8210A718): page `page` of a text appended to
// `out`, from the line after the first `skip`.
void CopyPage(u16_be* out, const u16_be* text, int32_t page, int32_t skip);
// Strings::FormatV and Strings::Format (sub_8210AAC0, sub_8210ACF0): string
// `id` with its arguments from the va_list `args` (a guest address)
// appended to `out`; Format leaves 0xFFD2 as it is. Return `out`.
uint32_t StringsFormatV(guest::Guest& g, uint32_t table, uint32_t out, uint32_t id, uint32_t args);
uint32_t StringsFormat(guest::Guest& g, uint32_t table, uint32_t out, uint32_t id, uint32_t args);
// Text::Draw (sub_8210A888): a text drawn on the game's font.
void TextDraw(guest::Guest& g, TextStyle* style, uint32_t text);
// XPR::Release (sub_82108980): the texture set in `slot` destroyed and
// freed, the slot cleared.
void ReleaseXpr(guest::Guest& g, u32_be* slot);

}  // namespace game
