// The string table and the font as our own C++ (--native_strings_and_font,
// on by default): the table's set-up and release, the load checks, a
// string by its id, a text's pages, a string with its arguments
// (Strings::FormatV and Format, numbers with thousands separators); the
// game's swprintf wrapper; the font's set-up (Font::Font, its shaders) and
// load, its layout (a text's width, each line's start), its glyphs'
// vertices and the draw's commands (Font::DrawPass) (game/strings.h).
//
// The rotation of turned text, the shader compiler and vswprintf stay
// recompiled.

#include "base/settings.h"
#include "game/strings.h"

#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

SBX_SETTING_BOOL(native_strings_and_font, true, "Overrides", "Menus and UI",
                 "Our own C++ for the string table (its set-up, release, load checks, a "
                 "string by its id, a text's pages, a string with its arguments) and the font "
                 "(its set-up and shaders, load, text widths, line positions, glyph vertices "
                 "and draw commands) instead of the recompiled functions");

namespace {

using game::Font;
using game::FontChannels;
using game::FontPage;
using game::FontShaderSource;
using game::Glyph;
using game::GlyphVertex;
using game::StringTable;
using game::TextStyle;
using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;
using guest::i32_be;
using guest::ptr;
using guest::u16_be;
using guest::u32_be;

constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kMemRelease = 0x8000;
constexpr uint32_t kPageReadWrite = 4;

// The font page and string files Text::Load asks for.
constexpr uint32_t kTextFontPages = 0xF;
constexpr uint32_t kTextStringFiles = 0x8001;

// The font pages' assets (22654 to 22657), one word each.
constexpr uint32_t kFontPageAssets = 0x82007BF4;
// Font::OnPageLoaded, the archive's callback for a page.
constexpr uint32_t kFontOnPageLoaded = 0x82109280;
// The four pixel shaders' HLSL sources (C strings), in the game's data.
constexpr uint32_t kFontShaderTexts = 0x825F32C4;

// Strings::FormatGrouped's formats (UTF-16, read-only data) are "%d",
// ",%03d" and "-%s"; it's written out here.
constexpr float kThousandth = 0.001f;  // 0x820029B0.

// A rotation's turns per degree (0x82007C10).
constexpr double kTurnsPerDegree = 1.0 / 360.0;

bool On() { return SBX_GET(native_strings_and_font); }

Font* TheFont() { return At<Font>(*At<u32_be>(game::kFont)); }
StringTable* TheTable() { return At<StringTable>(*At<u32_be>(game::kStringTable)); }

// --- Float steps as the game's code makes them ---

// A fused multiply-add rounded to a float (fmadds and its kin).
float FusedMultiplyAdd(float a, float b, float c) {
  return float(std::fma(double(a), double(b), double(c)));
}

// A float to an int toward zero (fctiwz): held at the largest int, a NaN
// or anything below the smallest gives the smallest.
int32_t TruncateToInt(double value) {
  if (std::isnan(value) || value <= -2147483649.0) {
    return INT32_MIN;
  }
  if (value >= double(INT32_MAX)) {
    return INT32_MAX;
  }
  return int32_t(value);
}

// A float to a 64-bit int toward zero (fctidz), of which the game keeps the
// low 16 bits.
uint16_t TruncateToHalfword(double value) {
  constexpr double kTwoTo63 = 9223372036854775808.0;
  if (std::isnan(value) || value < -kTwoTo63) {
    return 0;
  }
  if (value >= kTwoTo63) {
    return 0xFFFF;
  }
  return uint16_t(int64_t(value));
}

// --- The string table ---

// Strings::Init: the table made once, its files, counts and mask cleared.
void StringsInit(Guest& g) {
  u32_be* holder = At<u32_be>(game::kStringTable);
  if (*holder != 0) {
    return;
  }
  *holder = g.Call(sub_82292DD8, 0u, uint32_t(sizeof(StringTable)), kMemCommit,
                   kPageReadWrite);  // VirtualAlloc
  StringTable* table = TheTable();
  for (int i = 0; i < StringTable::kFiles; i++) {
    table->files[i] = 0;
    table->use_counts[i] = 0;
  }
  table->file_mask = 0;
}

// Font::IsLoaded: whether a page is asked for and each one asked for is in.
bool FontIsLoaded(Font* font) {
  int asked = 0;
  for (int i = 0; i < Font::kPages; i++) {
    if ((font->page_mask & (1u << i)) == 0) {
      continue;
    }
    asked++;
    if (font->pages[i]->loaded == 0) {
      return false;
    }
  }
  return asked != 0;
}

// Strings::IsLoaded: whether a file is asked for, each one asked for is
// in, and the font is.
bool TableIsLoaded(StringTable* table) {
  bool all = true;
  int asked = 0;
  for (int i = 0; i < StringTable::kFiles; i++) {
    if ((table->file_mask & (1u << i)) == 0) {
      continue;
    }
    if (table->loaded[i] == 0) {
      all = false;
    }
    asked++;
  }
  return asked > 0 && all && FontIsLoaded(TheFont());
}

// A number's digits with a comma between each group of three, as
// Strings::FormatGrouped prints them: "%d" when the magnitude times 0.001
// is below 1, else the thousands (the same way) and ",%03d" of the rest.
// The magnitude of the smallest int stays negative, and prints as itself.
std::u16string GroupedDigits(int32_t magnitude) {
  if (float(magnitude) * kThousandth < 1.0f) {
    const std::string digits = std::to_string(magnitude);
    return std::u16string(digits.begin(), digits.end());
  }
  const int32_t rest = magnitude % 1000;
  std::u16string text = GroupedDigits((magnitude - rest) / 1000);
  const char group[4] = {char('0' + rest / 100), char('0' + rest / 10 % 10),
                         char('0' + rest % 10), 0};
  text += u',';
  text.append(group, group + 3);
  return text;
}

// Strings::FormatGrouped: `value` with thousands separators appended to
// `out`; a negative value then has the whole output put after a '-' (so
// the smallest int gives "--2147483648"). False, with nothing written,
// where the original's own buffer (256 units) would overrun or its "-%s"
// would stop at a 0xFFFF unit: the original runs instead.
bool StringsFormatGrouped(int32_t value, u16_be* out) {
  const bool negative = value < 0;
  const int32_t magnitude = negative ? int32_t(0u - uint32_t(value)) : value;
  const std::u16string digits = GroupedDigits(magnitude);

  size_t length = 0;
  bool has_ffff = false;
  while (out[length] != 0) {
    has_ffff |= out[length] == 0xFFFF;
    length++;
  }
  if (negative && (has_ffff || length + digits.size() > 254)) {
    return false;
  }

  if (negative) {
    // The output so far moved on by one for the '-'.
    for (size_t i = length; i > 0; i--) {
      out[i] = out[i - 1];
    }
    out[0] = u'-';
    length++;
  }
  for (char16_t c : digits) {
    out[length++] = uint16_t(c);
  }
  out[length] = 0;
  return true;
}

// --- A string with its arguments ---

// The formats Strings::FormatV builds its numbers' formats with (UTF-16,
// read-only data): "%%%dd", "%%.%df" and "%%0%dd".
constexpr uint32_t kWidthFormat = 0x82007E40;
constexpr uint32_t kPrecisionFormat = 0x82007E4C;
constexpr uint32_t kZeroPaddedFormat = 0x82007E5C;

// The control characters of a string with arguments. Each takes the next
// argument (an 8-byte slot of the va_list): 0xFFD7 a string id (formatted
// with the slots after it), 0xFFEC to 0xFFF5 an int printed at least
// 0xFFF5 - c wide, 0xFFE2 to 0xFFEB a double with 0xFFEB - c decimals,
// 0xFFD8 to 0xFFE1 an int with 0xFFE1 - c digits (zero-padded), 0xFFD3 an
// int with thousands separators, 0xFFD2 a number through the font.
constexpr uint16_t kInnerString = 0xFFD7;
constexpr uint16_t kGroupedInt = 0xFFD3;
constexpr uint16_t kFontNumber = 0xFFD2;

// The frames Strings::FormatV and Strings::Format work in (vswprintf and
// Font::FormatNumber are handed their words): a number's one-argument
// va_list at +80, its format at +96, its text (2,048 bytes) at +608.
constexpr uint32_t kFormatVFrame = 2752;
constexpr uint32_t kFormatFrame = 2736;
constexpr uint32_t kNumberArgument = 80;
constexpr uint32_t kNumberFormat = 96;
constexpr uint32_t kNumberText = 608;
constexpr uint32_t kNumberTextBytes = 2048;

// A va_list's argument slot (8 bytes, 8-aligned) as the strings read it:
// an int in its low word, a string id in its last halfword, or a double.
struct IntSlot {
  uint8_t unknown_0[4];
  i32_be value;
};
GUEST_SIZE(IntSlot, 8);
struct StringIdSlot {
  uint8_t unknown_0[6];
  u16_be id;
};
GUEST_SIZE(StringIdSlot, 8);
using DoubleSlot = guest::be<double>;
constexpr uint32_t kSlotBytes = 8;

// The next slot of a va_list (its address), the list moved past it.
uint32_t NextSlot(uint32_t& args) {
  const uint32_t slot = (args + kSlotBytes - 1) & ~(kSlotBytes - 1);
  args = slot + kSlotBytes;
  return slot;
}

void Append(u16_be* out, const u16_be* text) {
  while (*out != 0) {
    out++;
  }
  do {
    *out++ = *text;
  } while (*text++ != 0);
}

void ClearNumberText(uint32_t text) {
  std::memset(At<uint8_t>(text), 0, kNumberTextBytes);
}

// Strings::Get: string `id` (its low 16 bits, UTF-16) from the file whose
// ids it's among (DataFile::GetEntry). An id past the last file gives the
// table itself, as the original leaves it in r3 (and its callers read it
// as the string).
uint32_t StringsGet(Guest& g, StringTable* table, uint32_t id) {
  const i32_be* first_ids = At<i32_be>(game::kStringFileFirstIds);
  const int32_t index = int32_t(id & 0xFFFF);
  for (int i = 0; i < StringTable::kFiles; i++) {
    if (index < first_ids[i + 1]) {
      return g.Call(sub_82292170, table->files[i], index - first_ids[i], 0u);  // DataFile::GetEntry
    }
  }
  return AddressOf(table);
}

// swprintf with its arguments in a va_list (a guest address): printed by
// vswprintf (recompiled). Returns its count.
uint32_t Swprintf(Guest& g, uint32_t buffer, uint32_t format, uint32_t args) {
  return g.Call(sub_8258F420, buffer, format, args);  // vswprintf
}

// Strings::FormatV's and Strings::Format's work: string `id` (Strings::Get)
// appended to `out` with its control characters replaced by their
// arguments (see above; the numbers printed by the game's swprintf). Format
// leaves 0xFFD2 as it is. Returns out.
uint32_t FormatString(Guest& g, uint32_t table, uint32_t out, uint32_t id, uint32_t args,
                      uint32_t frame_bytes, bool font_numbers) {
  guest::StackFrame frame(g.ctx, frame_bytes);
  const uint32_t argument = frame.Local(kNumberArgument);
  const uint32_t format = frame.Local(kNumberFormat);
  const uint32_t number = frame.Local(kNumberText);
  const uint32_t text = StringsGet(g, At<StringTable>(table), id);
  ClearNumberText(number);

  // A number printed with a format made from its width or precision (each
  // swprintf's one argument in an 8-byte va_list slot).
  auto print = [&](uint32_t format_format, uint32_t width, uint64_t value) {
    *At<guest::be<uint64_t>>(argument) = width;
    Swprintf(g, format, format_format, argument);
    *At<guest::be<uint64_t>>(argument) = value;
    Swprintf(g, number, format, argument);
  };

  for (uint32_t at = text; *At<u16_be>(at) != 0; at += 2) {
    const uint16_t c = *At<u16_be>(at);
    if (c == kInnerString) {
      const uint32_t slot = NextSlot(args);
      const uint16_t inner = At<StringIdSlot>(slot)->id;
      ClearNumberText(number);
      Append(At<u16_be>(out), At<u16_be>(game::StringsFormatV(g, table, number, inner, args)));
    } else if (c > 0xFFEB && c <= 0xFFF5) {
      const uint32_t slot = NextSlot(args);
      print(kWidthFormat, 0xFFF5u - c, uint32_t(At<IntSlot>(slot)->value));
      Append(At<u16_be>(out), At<u16_be>(number));
    } else if (c > 0xFFE1 && c <= 0xFFEB) {
      const uint32_t slot = NextSlot(args);
      // The double goes on as a float's value.
      const double value = float(*At<DoubleSlot>(slot));
      print(kPrecisionFormat, 0xFFEBu - c, std::bit_cast<uint64_t>(value));
      Append(At<u16_be>(out), At<u16_be>(number));
    } else if (c > kInnerString && c <= 0xFFE1) {
      const uint32_t slot = NextSlot(args);
      print(kZeroPaddedFormat, 0xFFE1u - c, uint32_t(At<IntSlot>(slot)->value));
      Append(At<u16_be>(out), At<u16_be>(number));
    } else if (c == kGroupedInt) {
      const uint32_t slot = NextSlot(args);
      ClearNumberText(number);
      const int32_t value = At<IntSlot>(slot)->value;
      if (!StringsFormatGrouped(value, At<u16_be>(number))) {
        g.Call(__imp__sub_8210A7B0, table, value, number);  // Strings::FormatGrouped
      }
      Append(At<u16_be>(out), At<u16_be>(number));
    } else if (c == kFontNumber && font_numbers) {
      const uint32_t slot = NextSlot(args);
      ClearNumberText(number);
      g.Call(sub_82108B90, *At<u32_be>(game::kFont), number,
             uint32_t(At<IntSlot>(slot)->value));  // Font::FormatNumber
      Append(At<u16_be>(out), At<u16_be>(number));
    } else {
      const u16_be character[2] = {c, 0};
      Append(At<u16_be>(out), character);
    }
  }
  return out;
}


// --- Font::Font and its shaders ---

constexpr uint32_t kVideo = 0x8265CD68;      // Holds the video output's owner.
constexpr uint32_t kRenderApp = 0x826AA880;  // Holds the render manager.

// The font's HLSL: the vertex shader's source (a C string) at [kFontVertexSource],
// its declaration's elements before it.
constexpr uint32_t kFontVertexSource = 0x825F32C0;
constexpr uint32_t kFontVertexElements = 0x825F3280;
constexpr uint32_t kShaderEntry = 0x82001BC4;          // "main".
constexpr uint32_t kVertexShaderProfile = 0x82001BCC;  // "vs_3_0".
constexpr uint32_t kPixelShaderProfile = 0x82007C04;   // "ps_2_0".

// Font::CreateShaders' frame (160 bytes): D3DXCompileShader's stack
// arguments (+80 and +88, each 8-byte slot's low word read: where the
// errors and the constant table go), the shader buffer (+96) and the
// errors (+100) written there.
struct CompileFrame {
  uint8_t linkage[80];
  u32_be unknown_80;  // Never read (the slots' high words).
  u32_be errors_out;
  u32_be unknown_88;
  u32_be constant_table_out;
  u32_be shader;
  u32_be errors;
  uint8_t unknown_104[56];
};
GUEST_SIZE(CompileFrame, 160);
GUEST_OFFSET(CompileFrame, errors_out, 84);
GUEST_OFFSET(CompileFrame, shader, 96);

// D3DXCompileShader of `source` for `profile` into the frame's buffer; the
// buffer's code (its slot 3) made into a shader by `create` (VertexShader::Create
// or PixelShader::Create), then the buffer released (slot 2). No check for
// errors, as the original.
uint32_t CompileShader(Guest& g, CompileFrame* frame, uint32_t source, uint32_t length,
                       uint32_t profile, uint32_t flags, Guest::Function* create) {
  frame->constant_table_out = 0;
  frame->errors_out = AddressOf(&frame->errors);
  g.Call(sub_822B3678, source, length, 0u, 0u, kShaderEntry, profile, flags,
         &frame->shader);  // D3DXCompileShader
  const uint32_t code = g.CallVirtual(At<u32_be>(frame->shader), 3);  // Buffer::GetPointer
  const uint32_t shader = g.Call(create, code);
  g.CallVirtual(At<u32_be>(frame->shader), 2);  // Buffer::Release
  return shader;
}

// Font::CreateShaders: the font's vertex declaration, its vertex shader
// and its four pixel shaders (one per channel of the sheet) compiled from
// their HLSL.
void FontCreateShaders(Guest& g, Font* font) {
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(CompileFrame)));
  CompileFrame* frame = At<CompileFrame>(stack.Local(0));
  frame->unknown_80 = 0;
  frame->unknown_88 = 0;
  font->vertex_declaration = g.Call(sub_822A5F28, kFontVertexElements);  // CreateVertexDeclaration
  const uint32_t source = *At<u32_be>(kFontVertexSource);
  uint32_t length = 0;
  while (*At<char>(source + length) != 0) {
    length++;
  }
  // The flags are 0 (in the original, r9 is the strlen's last byte read).
  font->vertex_shader = CompileShader(g, frame, source, length, kVertexShaderProfile, 0u,
                                      sub_822A7388);  // VertexShader::Create
  const FontShaderSource* sources = At<FontShaderSource>(game::kFontShaderSources);
  for (int i = 0; i < 4; i++) {
    font->pixel_shaders[i] = CompileShader(g, frame, sources[i].text, sources[i].length,
                                           kPixelShaderProfile, 0u, sub_822A7250);  // PixelShader::Create
  }
}

// Font::Font: the font set up: the video output (the owner's slot 7, then
// slot 3), the command list and render queue (the render manager's slot 8,
// then slots 1 and 2), the shaders made with the device locked, the pages
// and their files cleared, the ten colors, no pages asked for.
void FontConstruct(Guest& g, Font* font) {
  guest::StackFrame stack(g.ctx, 128);  // The original's, the lock's word at +88.
  void* owner = At<void>(g.CallVirtual(At<void>(*At<u32_be>(kVideo)), 7));
  font->video_output = g.CallVirtual(owner, 3);
  void* renderer = At<void>(g.CallVirtual(At<void>(*At<u32_be>(kRenderApp)), 8));
  font->command_list = g.CallVirtual(renderer, 1);
  renderer = At<void>(g.CallVirtual(At<void>(*At<u32_be>(kRenderApp)), 8));
  font->render_queue = g.CallVirtual(renderer, 2);

  u32_be* lock = At<u32_be>(stack.Local(88));
  *lock = g.Call(sub_82573468, uint32_t(font->video_output));  // Device::Lock
  FontCreateShaders(g, font);
  g.Call(sub_825734A8, uint32_t(font->video_output), lock);  // Device::Unlock

  for (int i = 0; i < Font::kPages; i++) {
    font->pages[i] = nullptr;
    font->page_files[i] = 0;
  }
  static constexpr uint8_t kColors[10][3] = {
      {255, 234, 0}, {44, 242, 242}, {82, 240, 105}, {198, 66, 14}, {0, 255, 0},
      {127, 127, 127}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
  };
  for (int i = 0; i < 10; i++) {
    for (int c = 0; c < 3; c++) {
      font->colors[i][c] = kColors[i][c];
    }
  }
  font->page_mask = 0;
}

// --- The font's set-up and load ---

// Font::Init: the font made (VirtualAlloc) into [kFont] and set up
// (Font::Font).
void FontInit(Guest& g) {
  const uint32_t font = g.Call(sub_82292DD8, 0u, uint32_t(sizeof(Font)), kMemCommit,
                               kPageReadWrite);  // VirtualAlloc
  *At<u32_be>(game::kFont) = font;
  FontConstruct(g, At<Font>(font));
}

// Font::Load: the pages in `mask` asked for; each one not loaded yet gets
// a page (operator new), its file's memory (VirtualAlloc, the asset's size)
// and its load (Archive::Load, Font::OnPageLoaded called when it's in).
void FontLoad(Guest& g, Font* font, uint32_t mask) {
  font->page_mask = font->page_mask | mask;
  for (int i = 0; i < Font::kPages; i++) {
    if ((mask & (1u << i)) == 0 || font->page_files[i] != 0) {
      continue;
    }
    font->pages[i] = At<FontPage>(g.Call(sub_8258C000, uint32_t(sizeof(FontPage))));  // operator new
    const uint32_t asset = *Element<u32_be>(kFontPageAssets, i);
    const uint32_t size = g.Call(sub_820A0860, asset);  // Archive::AssetSize
    font->page_files[i] =
        g.Call(sub_82292DD8, 0u, size, kMemCommit, kPageReadWrite);  // VirtualAlloc
    font->pages[i]->loaded = 0;
    g.Call(sub_8209DF80, asset, font->page_files[i], kFontOnPageLoaded, font->pages[i], 0u,
           0u);  // Archive::Load
  }
}

// --- The font's layout ---

FontPage* PageOf(Font* font, uint32_t index) {
  return *Element<ptr<FontPage>>(AddressOf(font->pages), index);
}

// A glyph's advance in pixels: its advance in texels times the glyph
// scale, as a whole number of pixels (its low 16 bits).
float GlyphAdvance(const Font* font, uint16_t c, float glyph_scale) {
  const uint32_t advance = font->page->GlyphFor(c)->Advance();
  return float(TruncateToHalfword(float(advance) * glyph_scale));
}

// Font::SetParams: the style kept, the text's top left (the position moved
// for the scale about the style's origin, a whole pixel) and the spacings.
void FontSetParams(Font* font, TextStyle* style) {
  font->style = style;
  auto start = [](int32_t position, int32_t origin, float scale) {
    const double moved = -std::fma(double(scale) - 1.0, double(float(origin)),
                                   -double(float(position)));
    return float(TruncateToInt(moved));
  };
  font->start_x = start(style->x, style->scale_origin_x, style->scale_x);
  font->start_y = start(style->y, style->scale_origin_y, style->scale_y);
  font->letter_spacing = float(int32_t(style->letter_spacing));
  font->line_spacing = float(int32_t(style->line_spacing));
}

// Font::LineGlyphs: the glyphs drawn in a line (to '\n', a page break or
// the end).
uint16_t FontLineGlyphs(const u16_be* line) {
  uint16_t count = 0;
  for (; *line != 0; line++) {
    const uint16_t c = *line;
    if (c == game::kPageBreak || c == '\n') {
      break;
    }
    if (!game::IsColorCode(c)) {
      count++;
    }
  }
  return count;
}

// Font::LineWidth: a line's width, each glyph's advance and the letter
// spacing, less the last spacing and times the scale; for justified text
// the advances alone.
float FontLineWidth(Font* font, const u16_be* line) {
  float width = 0.0f;
  for (; *line != 0; line++) {
    const uint16_t c = *line;
    if (c == game::kPageBreak || c == '\n') {
      break;
    }
    if (game::IsColorCode(c)) {
      continue;
    }
    const TextStyle* style = font->style;
    const float advance = GlyphAdvance(font, c, style->glyph_scale_x);
    if (style->horizontal_align == TextStyle::kJustify) {
      width = advance + width;
    } else {
      width = (advance + font->letter_spacing) + width;
    }
  }
  const TextStyle* style = font->style;
  if (style->horizontal_align == TextStyle::kJustify) {
    return width;
  }
  return (width - font->letter_spacing) * style->scale_x;
}

// Font::TextWidth: the widest line of a text's page (up to a page break)
// in the style, as Font::LineWidth measures them. A '\n' counts its own
// advance in the line it starts.
float FontTextWidth(Font* font, TextStyle* style, const u16_be* text) {
  FontSetParams(font, style);
  font->page = PageOf(font, style->page);
  const bool justified = style->horizontal_align == TextStyle::kJustify;
  float widest = 0.0f;
  float line = 0.0f;
  for (; *text != 0; text++) {
    const uint16_t c = *text;
    if (c == game::kPageBreak) {
      break;
    }
    if (c == '\n') {
      if (line > widest) {
        widest = line;
      }
      line = 0.0f;
    } else if (game::IsColorCode(c)) {
      continue;
    }
    const float advance = GlyphAdvance(font, c, style->glyph_scale_x);
    line = (justified ? advance : advance + font->letter_spacing) + line;
  }
  if (line > widest) {
    widest = line;
  }
  return (widest - font->letter_spacing) * style->scale_x;
}

// Font::LineX: where a line starts, by the style's horizontal alignment in
// its box (a whole pixel). Justified text gets the letter spacing that
// fills the box. An unknown alignment gives the line's width.
float FontLineX(Font* font, const u16_be* line) {
  const float line_width = FontLineWidth(font, line);
  const float width = float(TruncateToInt(line_width));
  const TextStyle* style = font->style;
  const float box_width = float(int32_t(style->box_width));
  switch (uint32_t(style->horizontal_align)) {
    case TextStyle::kAlignStart:
      return font->start_x;
    case TextStyle::kAlignCenter: {
      const float middle = FusedMultiplyAdd(box_width * style->scale_x, 0.5f, font->start_x);
      return float(TruncateToInt(FusedMultiplyAdd(-width, 0.5f, middle)));
    }
    case TextStyle::kAlignEnd: {
      const float right = FusedMultiplyAdd(box_width, style->scale_x, font->start_x);
      return float(TruncateToInt(right - width));
    }
    case TextStyle::kJustify: {
      const uint16_t glyphs = FontLineGlyphs(line);
      const float start = font->start_x;
      if (glyphs > 1) {
        font->letter_spacing = (box_width - width) / float(int32_t(glyphs) - 1);
      }
      return start;
    }
    default:
      return line_width;
  }
}

// Font::LineY: where line `line` of `line_count` starts, by the style's
// vertical alignment in its box. Justified text gets the line spacing that
// fills the box (a whole pixel). An unknown alignment gives nothing (the
// caller's f1 stays).
std::optional<float> FontLineY(Font* font, uint16_t line_count, uint16_t line) {
  const TextStyle* style = font->style;
  const float glyph_scale_y = style->glyph_scale_y;
  const float scale_y = style->scale_y;
  // A line's height, a whole pixel.
  const uint16_t line_height =
      TruncateToHalfword(float(font->page->header->cell_height) * glyph_scale_y);
  const float height = float(line_height);
  const float spacing = font->line_spacing;
  const float lines = float(line_count);
  const float index = float(line);
  const float block = FusedMultiplyAdd(height + spacing, lines, -spacing) * scale_y;
  const int32_t box_height = style->box_height;

  switch (uint32_t(style->vertical_align)) {
    case TextStyle::kAlignStart:
      return FusedMultiplyAdd((height + spacing) * index, scale_y, font->start_y);
    case TextStyle::kAlignCenter: {
      const float middle = FusedMultiplyAdd(float(box_height / 2), scale_y, font->start_y);
      const float top = FusedMultiplyAdd(-block, 0.5f, middle);
      return FusedMultiplyAdd((height + spacing) * index, scale_y, top);
    }
    case TextStyle::kAlignEnd: {
      const float bottom = FusedMultiplyAdd(float(box_height), scale_y, font->start_y);
      return FusedMultiplyAdd((height + spacing) * index, scale_y, bottom - block);
    }
    case TextStyle::kJustify: {
      if (line_count <= 1) {
        return float(font->start_y);
      }
      const int32_t fill = int32_t(uint32_t(box_height) - uint32_t(line_height) * line_count);
      const float new_spacing = float(fill / (int32_t(line_count) - 1));
      font->line_spacing = new_spacing;
      return FusedMultiplyAdd((height + new_spacing) * index, scale_y, font->start_y);
    }
    default:
      return std::nullopt;
  }
}

// Font::CountGlyphs: each drawn character's glyph counted in its channel's
// count (spaces, '\n' and color codes aren't drawn). Returns the text's
// length.
uint32_t FontCountGlyphs(Font* font, u32_be* counts, const u16_be* text) {
  uint32_t length = 0;
  while (text[length] != 0) {
    length++;
  }
  for (uint32_t i = 0; i < length; i++) {
    const uint16_t c = text[i];
    if (c == '\n' || c == ' ' || game::IsColorCode(c)) {
      continue;
    }
    counts[font->page->GlyphFor(c)->Channel()] += 1;
  }
  return length;
}

// Font::OutCode: where a vertex (x, y) is against the style's clip
// rectangle: 1 left of it, 2 right of it, 4 across; 8 above, 16 below, 32
// across.
uint32_t FontOutCode(const Font* font, float x, float y) {
  const TextStyle* style = font->style;
  const float left = float(int32_t(style->clip_left));
  const float right = float(int32_t(uint32_t(int32_t(style->clip_left)) +
                                    uint32_t(int32_t(style->clip_width))));
  const float top = float(int32_t(style->clip_top));
  const float bottom = float(int32_t(uint32_t(int32_t(style->clip_top)) +
                                     uint32_t(int32_t(style->clip_height))));
  uint32_t code = x < left ? 1 : x > right ? 2 : 4;
  code |= y < top ? 8 : y > bottom ? 16 : 32;
  return code;
}

// Font::ClipGlyph: a glyph's six vertices clipped to the style's clip
// rectangle, their texture coordinates moved with them. False when the
// glyph is wholly outside it; true with no rectangle (all 0).
bool FontClipGlyph(Font* font, GlyphVertex* vertices) {
  const TextStyle* style = font->style;
  const int32_t left = style->clip_left;
  if (left == 0 && style->clip_top == 0 && style->clip_width == 0 && style->clip_height == 0) {
    return true;
  }
  auto edge = [](int32_t start, int32_t size) {
    return float(int32_t(uint32_t(start) + uint32_t(size)));
  };
  // Vertex 0 is the top left, 1 the top right, 2 the bottom left.
  if (vertices[0].y > edge(style->clip_top, style->clip_height) ||
      vertices[2].y < float(int32_t(style->clip_top)) ||
      vertices[0].x > edge(style->clip_left, style->clip_width) ||
      vertices[1].x < float(left)) {
    return false;
  }
  for (int i = 0; i < 6; i++) {
    GlyphVertex& vertex = vertices[i];
    const uint32_t code = FontOutCode(font, vertex.x, vertex.y);
    if ((code & 4) && (code & 32)) {
      continue;
    }
    // Moved to the edge, its u or v by the same distance in texels.
    if (code & 1) {
      const float old_x = vertex.x;
      const float x = float(int32_t(font->style->clip_left));
      vertex.x = x;
      const float texels = (x - old_x) / font->style->scale_x;
      vertex.u = texels / float(uint32_t(font->page->texture_width)) + vertex.u;
    } else if (code & 2) {
      const float old_x = vertex.x;
      const float x = edge(font->style->clip_left, font->style->clip_width);
      vertex.x = x;
      const float texels = (old_x - x) / font->style->scale_x;
      vertex.u = vertex.u - texels / float(uint32_t(font->page->texture_width));
    }
    if (code & 8) {
      const float old_y = vertex.y;
      const float y = float(int32_t(font->style->clip_top));
      vertex.y = y;
      const float texels = (y - old_y) / font->style->scale_y;
      vertex.v = texels / float(uint32_t(font->page->texture_height)) + vertex.v;
    } else if (code & 16) {
      const float old_y = vertex.y;
      const float y = edge(font->style->clip_top, font->style->clip_height);
      vertex.y = y;
      const float texels = (old_y - y) / font->style->scale_y;
      vertex.v = vertex.v - texels / float(uint32_t(font->page->texture_height));
    }
  }
  return true;
}

// Font::AddGlyph: character `c`'s glyph at (x, y) in `color`: six vertices
// (two triangles) at its channel's write pointer, its size the cell's
// times the glyph scale and the scale, its texture coordinates its cell's
// place in the sheet. A clipped glyph is kept (the pointer moves on) or
// dropped (its channel's count down by one); a turned one goes through
// Font::RotateVertex (recompiled) instead. False, with nothing written,
// for a cell of no width or a sheet narrower than a cell (the original's
// divisions trap): the original runs instead.
bool FontAddGlyph(Guest& g, Font* font, uint16_t c, uint32_t color, double x, double y) {
  const FontPage* page = font->page;
  const game::GlyphTableHeader* header = page->header;
  const Glyph* glyph = page->GlyphFor(c);
  const TextStyle* style = font->style;
  const uint32_t cell_width = header->cell_width;
  const uint32_t cell_height = header->cell_height;
  const uint32_t texture_width = page->texture_width;
  const uint32_t texture_height = page->texture_height;
  if (cell_width == 0 || uint16_t(texture_width / cell_width) == 0) {
    return false;
  }
  const uint32_t columns = uint16_t(texture_width / cell_width);
  const uint32_t column = glyph->cell % columns;
  const uint32_t row = glyph->cell / columns;

  const float width = (style->glyph_scale_x * style->scale_x) * float(cell_width);
  const float height = (style->glyph_scale_y * style->scale_y) * float(cell_height);
  const float u0 = float(column * cell_width) / float(texture_width);
  const float u1 = float(cell_width) / float(texture_width) + u0;
  const float v0 = float(row * cell_height) / float(texture_height);
  const float v1 = float(cell_height) / float(texture_height) + v0;
  const float x0 = float(x), x1 = float(double(width) + x);
  const float y0 = float(y), y1 = float(double(height) + y);

  FontChannels* channels = At<FontChannels>(game::kFontChannels);
  const uint32_t channel = glyph->Channel();
  GlyphVertex* vertices = At<GlyphVertex>(channels->write_pointers[channel]);
  const GlyphVertex corners[6] = {
      {x0, y0, 0.0f, color, u0, v0}, {x1, y0, 0.0f, color, u1, v0},
      {x0, y1, 0.0f, color, u0, v1}, {x0, y1, 0.0f, color, u0, v1},
      {x1, y0, 0.0f, color, u1, v0}, {x1, y1, 0.0f, color, u1, v1},
  };
  for (int i = 0; i < 6; i++) {
    vertices[i] = corners[i];
  }

  // Turned unless the rotation is a whole number of turns.
  const float turns = float(double(float(style->rotation)) * kTurnsPerDegree);
  if (turns != float(TruncateToInt(turns))) {
    for (int i = 0; i < 6; i++) {
      g.Call(sub_82109088, font, &vertices[i]);  // Font::RotateVertex
    }
  } else if (!FontClipGlyph(font, vertices)) {
    channels->glyph_counts[channel] -= 1;
    return true;
  }
  channels->write_pointers[channel] += 6 * uint32_t(sizeof(GlyphVertex));
  return true;
}

// Font::BuildVertices: a text's glyphs written as vertices (Font::AddGlyph),
// line by line from each line's start (Font::LineX, Font::LineY), in the
// style's color until a color code changes it (0xFFD6 back to the style's,
// 0xFFF6 to 0xFFFF the font's ten colors; the alpha stays the style's).
// Every character up to the end is gone through, page breaks too. With the
// second pass on, each glyph's place for it is kept. A turned text has its
// rotation set up first (Font::SetUpRotation, recompiled).
void FontBuildVertices(Guest& g, Font* font, const u16_be* text) {
  uint32_t length = 0;
  while (text[length] != 0) {
    length++;
  }
  uint16_t line_count = 1;
  for (const u16_be* c = text; *c != 0 && *c != game::kPageBreak; c++) {
    if (*c == '\n') {
      line_count++;
    }
  }

  const TextStyle* style = font->style;
  const uint32_t alpha = uint8_t(TruncateToHalfword(style->alpha * 255.0f));
  auto argb = [alpha](uint32_t red, uint32_t green, uint32_t blue) {
    return alpha << 24 | red << 16 | green << 8 | blue;
  };
  uint32_t color = argb(style->red, style->green, style->blue);

  // A line's start; LineY leaves x as y for an unknown alignment.
  float x = 0.0f, y = 0.0f;
  auto start_line = [&](const u16_be* line, uint16_t index) {
    x = FontLineX(font, line);
    y = FontLineY(font, line_count, index).value_or(x);
  };
  start_line(text, 0);

  const float turns = float(double(float(font->style->rotation)) * kTurnsPerDegree);
  if (turns != float(TruncateToInt(turns))) {
    g.Call(sub_82109910, font);  // Font::SetUpRotation
  }

  uint16_t line = 0;
  for (uint32_t i = 0; i < length; i++) {
    const uint16_t c = text[i];
    style = font->style;
    const float advance = GlyphAdvance(font, c, style->glyph_scale_x);
    if (c == '\n') {
      line++;
      start_line(&text[i + 1], line);
      continue;
    }
    if (c == game::kColorCode) {
      color = argb(font->style->red, font->style->green, font->style->blue);
      continue;
    }
    if (c >= game::kFirstColorCode) {
      const uint8_t* rgb = font->colors[0xFFFF - c];
      color = argb(rgb[0], rgb[1], rgb[2]);
      continue;
    }
    if (c != ' ') {
      if (!FontAddGlyph(g, font, c, color, x, y)) {
        g.ctx.f1.f64 = x;
        g.ctx.f2.f64 = y;
        g.Call(__imp__sub_82109680, font, 0u, 0u, uint32_t(c), color);  // Font::AddGlyph
      }
      style = font->style;
      if (style->second_pass != 0) {
        game::FontPoint* point =
            Element<game::FontPoint>(AddressOf(font->second_pass_points), i);
        point->x = FusedMultiplyAdd(style->second_offset_x, style->scale_x, x);
        point->y = FusedMultiplyAdd(style->second_offset_y, style->scale_y, y);
      }
      style = font->style;
    }
    x = FusedMultiplyAdd(font->letter_spacing + advance, style->scale_x, x);
  }
}

// --- The draw's commands ---

// A command's first slot on the command list (16 bytes a slot): its id and
// how many slots of payload follow.
struct Command {
  u32_be id;
  u32_be count;
  u32_be unknown_8;
  uint8_t unknown_12[4];
};
GUEST_SIZE(Command, 16);

// Payload slots of the commands the font records.
struct ChunkStart {  // The draw's first slot (handed to the render queue).
  uint8_t unknown_0[2];
  u16_be unknown_2;  // 5.
  u32_be unknown_4;
  uint8_t unknown_8[8];
};
struct TextureValue {
  u32_be sampler;
  u32_be texture;
  uint8_t unknown_8[8];
};
struct StateValue {
  u32_be state;
  u32_be value;
  uint8_t unknown_8[8];
};
struct SamplerStateValue {
  u32_be sampler;
  u32_be state;
  u32_be value;
  uint8_t unknown_12[4];
};
struct ScissorValue {
  u16_be left;
  u16_be top;
  u16_be width;
  u16_be height;
  uint8_t unknown_8[8];
};
struct ObjectValue {  // A vertex declaration or shader.
  u32_be object;
  uint8_t unknown_4[12];
};
struct DrawValue {
  u32_be primitive;
  u16_be vertex_count;
  u16_be stride;
  u32_be vertices;
  uint8_t unknown_12[4];
};
GUEST_SIZE(ChunkStart, 16);
GUEST_SIZE(TextureValue, 16);
GUEST_SIZE(StateValue, 16);
GUEST_SIZE(SamplerStateValue, 16);
GUEST_SIZE(ScissorValue, 16);
GUEST_SIZE(ObjectValue, 16);
GUEST_SIZE(DrawValue, 16);

// The command ids.
constexpr uint32_t kSetRenderState = 1;
constexpr uint32_t kSetSamplerState = 2;
constexpr uint32_t kSetTexture = 4;
constexpr uint32_t kSetVertexShader = 8;
constexpr uint32_t kSetPixelShader = 9;
constexpr uint32_t kSetVertexDeclaration = 10;
constexpr uint32_t kDrawPrimitiveUp = 19;
constexpr uint32_t kInlineData = 21;
constexpr uint32_t kSetScissorRect = 31;
constexpr uint32_t kEndCommands = 36;

// The render and sampler states (the device's ids) and their values.
constexpr uint32_t kCullMode = 56;
constexpr uint32_t kAlphaBlendEnable = 60;
constexpr uint32_t kSourceBlend = 72;
constexpr uint32_t kDestinationBlend = 76;
constexpr uint32_t kScissorTestEnable = 200;
constexpr uint32_t kViewportEnable = 308;  // Probably: off while the text is drawn.
constexpr uint32_t kSourceAlpha = 6;
constexpr uint32_t kInverseSourceAlpha = 7;
constexpr uint32_t kMagFilter = 16;
constexpr uint32_t kMinFilter = 20;
constexpr uint32_t kMipFilter = 24;
constexpr uint32_t kTriangleList = 4;
constexpr uint32_t kVerticesPerGlyph = 6;

// The depth the second pass is drawn at (0x820067E0); the first is at 0.
constexpr float kSecondPassDepth = -1000.0f;

// Records on a command list a slot at a time (CommandList::AllocateSlots).
class Recorder {
 public:
  Recorder(Guest& g, uint32_t list) : g_(g), list_(list) {}

  template <typename T>
  T* Slot() {
    return At<T>(g_.Call(sub_82572638, list_, 1u));  // CommandList::AllocateSlots
  }
  void Begin(uint32_t id, uint32_t count) {
    Command* command = Slot<Command>();
    command->id = id;
    command->count = count;
    command->unknown_8 = 0;
  }
  void RenderState(uint32_t state, uint32_t value) {
    StateValue* slot = Slot<StateValue>();
    slot->state = state;
    slot->value = value;
  }
  void SamplerState(uint32_t state, uint32_t value) {
    SamplerStateValue* slot = Slot<SamplerStateValue>();
    slot->sampler = 0;
    slot->state = state;
    slot->value = value;
  }
  void Object(uint32_t id, uint32_t object) {
    Begin(id, 1);
    Slot<ObjectValue>()->object = object;
  }

 private:
  Guest& g_;
  uint32_t list_;
};

// Font::DrawPass: a text drawn on the font's command list (`pass` 1 the
// second pass, its glyphs laid out by Font::LayOutPositioned, recompiled).
// Nothing for no text or no glyphs. In one chunk: the page's texture, the
// blend, cull and viewport states, the scissor (the style's clip rectangle,
// or off), the sampler's filters, the vertex declaration and shader, each
// channel's vertices (inline, 144 bytes a glyph, Font::BuildVertices writes
// them), then each channel's pixel shader and draw, the viewport state
// back on. The chunk is handed to the render queue (its slot 4) at the
// style's layer + 1.
void FontDrawPass(Guest& g, Font* font, uint32_t text, uint32_t pass) {
  if (text == 0) {
    return;
  }
  FontChannels* channels = At<FontChannels>(game::kFontChannels);
  for (u32_be& count : channels->glyph_counts) {
    count = 0;
  }
  if (FontCountGlyphs(font, channels->glyph_counts, At<u16_be>(text)) == 0) {
    return;
  }
  const uint32_t list = font->command_list;
  const uint32_t queue = font->render_queue;
  g.Call(sub_82572568, list);  // CommandList::OpenChunk
  Recorder record(g, list);

  ChunkStart* start = record.Slot<ChunkStart>();
  channels->first_slot = AddressOf(start);
  start->unknown_2 = 5;
  start->unknown_4 = 0;

  record.Begin(kSetTexture, 1);
  TextureValue* texture = record.Slot<TextureValue>();
  texture->sampler = 0;
  texture->texture = font->page->texture;

  record.Begin(kSetRenderState, 5);
  record.RenderState(kAlphaBlendEnable, 1);
  record.RenderState(kSourceBlend, kSourceAlpha);
  record.RenderState(kDestinationBlend, kInverseSourceAlpha);
  record.RenderState(kViewportEnable, 0);
  record.RenderState(kCullMode, 0);

  const TextStyle* style = font->style;
  if (style->clip_left == 0 && style->clip_top == 0 && style->clip_width == 0 &&
      style->clip_height == 0) {
    record.Begin(kSetRenderState, 1);
    record.RenderState(kScissorTestEnable, 0);
  } else {
    record.Begin(kSetScissorRect, 1);
    ScissorValue* scissor = record.Slot<ScissorValue>();
    scissor->left = uint16_t(int32_t(font->style->clip_left));
    scissor->top = uint16_t(int32_t(font->style->clip_top));
    scissor->width = uint16_t(int32_t(font->style->clip_width));
    scissor->height = uint16_t(int32_t(font->style->clip_height));
  }

  record.Begin(kSetSamplerState, 3);
  record.SamplerState(kMipFilter, 2);
  record.SamplerState(kMinFilter, 1);
  record.SamplerState(kMagFilter, 1);
  record.Object(kSetVertexDeclaration, font->vertex_declaration);
  record.Object(kSetVertexShader, font->vertex_shader);

  for (int i = 0; i < 4; i++) {
    const uint32_t vertex_count = channels->glyph_counts[i] * kVerticesPerGlyph;
    uint32_t vertices = 0;
    if (vertex_count != 0) {
      const uint32_t bytes = vertex_count * uint32_t(sizeof(GlyphVertex));
      record.Begin(kInlineData, bytes);
      vertices = g.Call(sub_82572730, list, bytes, 0u);  // CommandList::AllocateBytes
    }
    channels->write_pointers[i] = vertices;
    channels->vertices[i] = vertices;
  }

  if (pass != 0) {
    g.Call(sub_82109EB0, font, text);  // Font::LayOutPositioned
  } else {
    FontBuildVertices(g, font, At<u16_be>(text));
  }

  for (int i = 0; i < 4; i++) {
    // The counts as the clipping left them.
    const uint32_t vertex_count = channels->glyph_counts[i] * kVerticesPerGlyph;
    if (vertex_count == 0) {
      continue;
    }
    record.Object(kSetPixelShader, font->pixel_shaders[i]);
    record.Begin(kDrawPrimitiveUp, 1);
    DrawValue* draw = record.Slot<DrawValue>();
    draw->primitive = kTriangleList;
    draw->vertex_count = uint16_t(vertex_count);
    draw->stride = uint16_t(sizeof(GlyphVertex));
    draw->vertices = channels->vertices[i];
  }

  record.Begin(kSetRenderState, 1);
  record.RenderState(kViewportEnable, 1);
  record.Begin(kEndCommands, 0);
  g.Call(sub_825725E8, list);  // CommandList::CloseChunk

  if (channels->first_slot != 0) {
    const uint32_t first = channels->first_slot;
    g.ctx.f1.f64 = pass != 0 ? kSecondPassDepth : 0.0f;
    g.CallVirtual(At<u32_be>(queue), 4, first,
                  uint32_t(int32_t(font->style->layer) + 1));  // RenderQueue::Add
  }
  channels->first_slot = 0;
}

// Font::Draw: the style set, the text drawn on its page (Font::DrawPass),
// and again on the second page with the second pass on.
void FontDraw(Guest& g, Font* font, TextStyle* style, uint32_t text) {
  FontSetParams(font, style);
  font->page = PageOf(font, style->page);
  FontDrawPass(g, font, text, 0);
  if (font->style->second_pass != 0) {
    font->page = PageOf(font, font->style->second_page);
    FontDrawPass(g, font, text, 1);
  }
}

// The font's pixel shader sources set (a static initializer): one per
// channel, each with its length.
void SetFontShaderSources() {
  FontShaderSource* sources = At<FontShaderSource>(game::kFontShaderSources);
  for (int i = 0; i < 4; i++) {
    const uint32_t text = *Element<u32_be>(kFontShaderTexts, i);
    const char* source = At<char>(text);
    uint32_t length = 0;
    while (source[length] != 0) {
      length++;
    }
    sources[i].text = text;
    sources[i].length = length;
    sources[i].unknown_8 = 0;
  }
}

}  // namespace

namespace game {

uint32_t PageCount(const u16_be* text) {
  uint32_t count = 0;
  for (; *text != 0; text++) {
    if (*text == kPageBreak) {
      count++;
    }
  }
  return count;
}

void CopyPage(u16_be* out, const u16_be* text, int32_t page, int32_t skip) {
  while (page > 0 && *text != 0) {
    if (*text == kPageBreak) {
      page--;
    }
    text++;
  }
  u16_be* end = out;
  while (*end != 0) {
    end++;
  }
  for (; *text != 0 && *text != kPageBreak; text++) {
    if (skip <= 0) {
      *end++ = *text;
      *end = 0;
    }
    if (*text == '\n') {
      skip--;
    }
  }
}

uint32_t StringsFormatV(Guest& g, uint32_t table, uint32_t out, uint32_t id, uint32_t args) {
  return FormatString(g, table, out, id, args, kFormatVFrame, true);
}

uint32_t StringsFormat(Guest& g, uint32_t table, uint32_t out, uint32_t id, uint32_t args) {
  return FormatString(g, table, out, id, args, kFormatFrame, false);
}

void TextDraw(Guest& g, TextStyle* style, uint32_t text) { FontDraw(g, TheFont(), style, text); }

void ReleaseXpr(Guest& g, u32_be* slot) {
  const uint32_t xpr = *slot;
  if (xpr == 0) {
    return;
  }
  g.Call(sub_8257CA60, xpr);  // XPR::~XPR
  g.Call(sub_8258C868, xpr);  // operator delete
  *slot = 0;
}

}  // namespace game

#define SBX_STRINGS_FALLBACK(name)   \
  REX_FUNC_PROLOGUE();               \
  if (!On()) {                       \
    __imp__##name(ctx, base);        \
    return;                          \
  }                                  \
  Guest g(ctx, base);

// --- The string table ---

// Strings::Init: no arguments.
extern "C" REX_FUNC(sub_8210A990) {
  SBX_STRINGS_FALLBACK(sub_8210A990)
  StringsInit(g);
}

// Strings::OnLoaded: the archive's callback; r6 the file's loaded flag.
extern "C" REX_FUNC(sub_8210A980) {
  SBX_STRINGS_FALLBACK(sub_8210A980)
  *At<u32_be>(ctx.r6.u32) = 1;
}

// Strings::Release: r3 the table, r4 a mask of files. Each one's use count
// down by one; at 0 it's freed.
extern "C" REX_FUNC(sub_8210A668) {
  SBX_STRINGS_FALLBACK(sub_8210A668)
  StringTable* table = At<StringTable>(ctx.r3.u32);
  const uint32_t mask = ctx.r4.u32;
  for (int i = 0; i < StringTable::kFiles; i++) {
    const uint32_t bit = 1u << i;
    if ((mask & bit) == 0 || table->files[i] == 0) {
      continue;
    }
    table->use_counts[i] -= 1;
    if (table->use_counts[i] != 0) {
      continue;
    }
    g.Call(sub_82292D70, table->files[i], 0u, kMemRelease);  // VirtualFree
    table->files[i] = 0;
    table->loaded[i] = 0;
    table->file_mask &= ~bit;
  }
}

// Font::IsLoaded: r3 the font.
extern "C" REX_FUNC(sub_821082C0) {
  SBX_STRINGS_FALLBACK(sub_821082C0)
  ctx.r3.u64 = FontIsLoaded(At<Font>(ctx.r3.u32));
}

// Strings::IsLoaded: r3 the table.
extern "C" REX_FUNC(sub_8210A898) {
  SBX_STRINGS_FALLBACK(sub_8210A898)
  ctx.r3.u64 = TableIsLoaded(At<StringTable>(ctx.r3.u32));
}

// Text::IsLoaded: the table and the font.
extern "C" REX_FUNC(sub_8220FA18) {
  SBX_STRINGS_FALLBACK(sub_8220FA18)
  ctx.r3.u64 = TableIsLoaded(TheTable()) && FontIsLoaded(TheFont());
}

// Strings::Get: r3 the table, r4 the string's id. The string, or the table
// for an id past the last file.
extern "C" REX_FUNC(sub_8210A930) {
  SBX_STRINGS_FALLBACK(sub_8210A930)
  ctx.r3.u64 = StringsGet(g, At<StringTable>(ctx.r3.u32), ctx.r4.u32);
}

// Strings::FormatGrouped: r3 unused, r4 the number, r5 the output (appended
// to). Returns the output.
extern "C" REX_FUNC(sub_8210A7B0) {
  SBX_STRINGS_FALLBACK(sub_8210A7B0)
  const uint32_t out = ctx.r5.u32;
  if (!StringsFormatGrouped(ctx.r4.s32, At<u16_be>(out))) {
    __imp__sub_8210A7B0(ctx, base);
    return;
  }
  ctx.r3.u64 = out;
}

// Strings::FormatV: r3 the table, r4 the output (appended to), r5 the
// string's id, r6 the arguments (a va_list). Returns the output.
extern "C" REX_FUNC(sub_8210AAC0) {
  SBX_STRINGS_FALLBACK(sub_8210AAC0)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = game::StringsFormatV(g, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// Strings::Format: as Strings::FormatV, the arguments from r6 on (varargs:
// r6 to r10 stored in the caller's parameter area, the rest after them).
// No 0xFFD2.
extern "C" REX_FUNC(sub_8210ACF0) {
  SBX_STRINGS_FALLBACK(sub_8210ACF0)
  ctx.fpscr.disableFlushMode();
  struct CallerParameters {
    uint8_t unknown_0[40];
    guest::be<uint64_t> varargs[5];  // r6 to r10; the rest follow.
  };
  CallerParameters* parameters = At<CallerParameters>(ctx.r1.u32);
  const PPCRegister* registers[] = {&ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (int i = 0; i < 5; i++) {
    parameters->varargs[i] = registers[i]->u64;
  }
  const uint32_t args = AddressOf(parameters->varargs);
  ctx.r3.u64 = game::StringsFormat(g, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, args);
}

// swprintf: r3 the buffer, r4 the format (UTF-16), the arguments from r5
// on (varargs: r5 to r10 stored in the caller's parameter area at +32, the
// rest after them, as the va_list). Returns vswprintf's count.
extern "C" REX_FUNC(sub_8210A5C0) {
  SBX_STRINGS_FALLBACK(sub_8210A5C0)
  struct CallerParameters {
    uint8_t unknown_0[32];
    guest::be<uint64_t> varargs[6];  // r5 to r10; the rest follow.
  };
  CallerParameters* parameters = At<CallerParameters>(ctx.r1.u32);
  const PPCRegister* registers[] = {&ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (int i = 0; i < 6; i++) {
    parameters->varargs[i] = registers[i]->u64;
  }
  const uint32_t args = AddressOf(parameters->varargs);
  ctx.r3.u64 = Swprintf(g, ctx.r3.u32, ctx.r4.u32, args);
}

// Strings::PageCount: r4 the text; how many page breaks it has.
extern "C" REX_FUNC(sub_8210A6F0) {
  SBX_STRINGS_FALLBACK(sub_8210A6F0)
  ctx.r3.u64 = game::PageCount(At<u16_be>(ctx.r4.u32));
}

// Strings::CopyPage: r4 out, r5 the text, r6 the page, r7 the lines to
// skip. The page's characters (up to the next break) appended to `out`,
// from the line after the skipped ones. Returns out.
extern "C" REX_FUNC(sub_8210A718) {
  SBX_STRINGS_FALLBACK(sub_8210A718)
  const uint32_t out = ctx.r4.u32;
  game::CopyPage(At<u16_be>(out), At<u16_be>(ctx.r5.u32), ctx.r6.s32, ctx.r7.s32);
  ctx.r3.u64 = out;
}

// --- Text: the font and the table together ---

// Text::Init: no arguments; once at boot. The font made, then the table.
extern "C" REX_FUNC(sub_8210AEF8) {
  SBX_STRINGS_FALLBACK(sub_8210AEF8)
  FontInit(g);
  StringsInit(g);
}

// Text::Load: the font's pages and the text's string files asked for.
extern "C" REX_FUNC(sub_8210AF20) {
  SBX_STRINGS_FALLBACK(sub_8210AF20)
  FontLoad(g, TheFont(), kTextFontPages);
  g.Call(sub_8210AA00, TheTable(), kTextStringFiles);  // Strings::Load
}

// Text::Draw: r4 the style, r5 the text, on the font.
extern "C" REX_FUNC(sub_8210A888) {
  SBX_STRINGS_FALLBACK(sub_8210A888)
  game::TextDraw(g, At<TextStyle>(ctx.r4.u32), ctx.r5.u32);
}

// --- The font ---

// Font::Init: no arguments; once at boot (from Text::Init).
extern "C" REX_FUNC(sub_82109B28) {
  SBX_STRINGS_FALLBACK(sub_82109B28)
  FontInit(g);
}

// Font::Font: r3 the font.
extern "C" REX_FUNC(sub_82109348) {
  SBX_STRINGS_FALLBACK(sub_82109348)
  FontConstruct(g, At<Font>(ctx.r3.u32));
}

// Font::CreateShaders: r3 the font.
extern "C" REX_FUNC(sub_821089D8) {
  SBX_STRINGS_FALLBACK(sub_821089D8)
  FontCreateShaders(g, At<Font>(ctx.r3.u32));
}

// Font::Load: r3 the font, r4 a mask of pages.
extern "C" REX_FUNC(sub_821095B8) {
  SBX_STRINGS_FALLBACK(sub_821095B8)
  FontLoad(g, At<Font>(ctx.r3.u32), ctx.r4.u32);
}

// The font's pixel shader sources: a static initializer (the C runtime's
// start-up table).
extern "C" REX_FUNC(sub_825BE990) {
  SBX_STRINGS_FALLBACK(sub_825BE990)
  SetFontShaderSources();
}

// Font::Draw: r3 the font, r4 the style, r5 the text.
extern "C" REX_FUNC(sub_8210A530) {
  SBX_STRINGS_FALLBACK(sub_8210A530)
  FontDraw(g, At<Font>(ctx.r3.u32), At<TextStyle>(ctx.r4.u32), ctx.r5.u32);
}

// Font::SetParams: r3 the font, r4 the style.
extern "C" REX_FUNC(sub_82108488) {
  SBX_STRINGS_FALLBACK(sub_82108488)
  ctx.fpscr.disableFlushMode();
  FontSetParams(At<Font>(ctx.r3.u32), At<TextStyle>(ctx.r4.u32));
}

// Font::TextWidth: r3 the font, r4 the style, r5 the text. The width in f1.
extern "C" REX_FUNC(sub_82108C50) {
  SBX_STRINGS_FALLBACK(sub_82108C50)
  ctx.fpscr.disableFlushMode();
  ctx.f1.f64 = FontTextWidth(At<Font>(ctx.r3.u32), At<TextStyle>(ctx.r4.u32),
                             At<u16_be>(ctx.r5.u32));
}

// Font::LineWidth: r3 the font, r4 the line. The width in f1.
extern "C" REX_FUNC(sub_82108578) {
  SBX_STRINGS_FALLBACK(sub_82108578)
  ctx.fpscr.disableFlushMode();
  ctx.f1.f64 = FontLineWidth(At<Font>(ctx.r3.u32), At<u16_be>(ctx.r4.u32));
}

// Font::LineX: r3 the font, r4 the line. Its x in f1.
extern "C" REX_FUNC(sub_82109100) {
  SBX_STRINGS_FALLBACK(sub_82109100)
  ctx.fpscr.disableFlushMode();
  ctx.f1.f64 = FontLineX(At<Font>(ctx.r3.u32), At<u16_be>(ctx.r4.u32));
}

// Font::LineGlyphs: r4 the line. The count in r3.
extern "C" REX_FUNC(sub_82108930) {
  SBX_STRINGS_FALLBACK(sub_82108930)
  ctx.r3.u64 = FontLineGlyphs(At<u16_be>(ctx.r4.u32));
}

// Font::LineY: r3 the font, r4 the line count, r5 the line. Its y in f1.
extern "C" REX_FUNC(sub_82108688) {
  SBX_STRINGS_FALLBACK(sub_82108688)
  ctx.fpscr.disableFlushMode();
  if (std::optional<float> y = FontLineY(At<Font>(ctx.r3.u32), uint16_t(ctx.r4.u32),
                                         uint16_t(ctx.r5.u32))) {
    ctx.f1.f64 = *y;
  }
}

// Font::CountGlyphs: r3 the font, r4 the four counts, r5 the text. Returns
// the text's length.
extern "C" REX_FUNC(sub_82108330) {
  SBX_STRINGS_FALLBACK(sub_82108330)
  ctx.r3.u64 = FontCountGlyphs(At<Font>(ctx.r3.u32), At<u32_be>(ctx.r4.u32),
                               At<u16_be>(ctx.r5.u32));
}

// Font::OutCode: r3 the font, r4 the vertex (x, y floats).
extern "C" REX_FUNC(sub_821083C0) {
  SBX_STRINGS_FALLBACK(sub_821083C0)
  ctx.fpscr.disableFlushMode();
  const guest::f32_be* vertex = At<guest::f32_be>(ctx.r4.u32);
  ctx.r3.u64 = FontOutCode(At<Font>(ctx.r3.u32), vertex[0], vertex[1]);
}

// Font::ClipGlyph: r3 the font, r4 the glyph's six vertices. 0 when
// nothing is left of it.
extern "C" REX_FUNC(sub_82108DB0) {
  SBX_STRINGS_FALLBACK(sub_82108DB0)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = FontClipGlyph(At<Font>(ctx.r3.u32), At<GlyphVertex>(ctx.r4.u32));
}

// Font::AddGlyph: r3 the font, r6 the character, r7 the color, f1 and f2
// the glyph's top left.
extern "C" REX_FUNC(sub_82109680) {
  SBX_STRINGS_FALLBACK(sub_82109680)
  ctx.fpscr.disableFlushMode();
  if (!FontAddGlyph(g, At<Font>(ctx.r3.u32), uint16_t(ctx.r6.u32), ctx.r7.u32, ctx.f1.f64,
                    ctx.f2.f64)) {
    __imp__sub_82109680(ctx, base);
  }
}

// Font::DrawPass: r3 the font, r4 the text, r5 the pass.
extern "C" REX_FUNC(sub_8210A020) {
  SBX_STRINGS_FALLBACK(sub_8210A020)
  ctx.fpscr.disableFlushMode();
  FontDrawPass(g, At<Font>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// Font::BuildVertices: r3 the font, r4 the text. Returns 0.
extern "C" REX_FUNC(sub_82109BD0) {
  SBX_STRINGS_FALLBACK(sub_82109BD0)
  ctx.fpscr.disableFlushMode();
  FontBuildVertices(g, At<Font>(ctx.r3.u32), At<u16_be>(ctx.r4.u32));
  ctx.r3.u64 = 0;
}

// XPR::Release: r3 a slot holding an XPR (a texture set). When there's
// one, it's destroyed (XPR::~XPR), freed (operator delete) and the slot
// cleared.
extern "C" REX_FUNC(sub_82108980) {
  SBX_STRINGS_FALLBACK(sub_82108980)
  game::ReleaseXpr(g, At<u32_be>(ctx.r3.u32));
}
