// The patch loader: loads archive entries from loose files in PATCH_DATA
// instead of the .DAT archives, so game files can be replaced without
// rebuilding BUILD.DAT.
//
//   assets/PATCH_DATA/BUILD/TXR/BUILD/<id>.<any extension>       BUILD ids
//   assets/PATCH_DATA/BUILD/TXR/BUILD_CRS/<id>.<any extension>   BUILD_CRS ids
//
// A file is named by its archive id, as GUT Archive Tools names its dumps
// (00000072.bin is the car specs); the extension doesn't matter. In
// BUILD_CRS/, the id is either the game's (23009-24367) or the one within
// that archive (0-1358). Files are uncompressed, like the dumps.
//
// Both folders are read with their sub-folders, so files can be organized
// freely (cars/z33/00003152.node). A file with any other name is served by
// the folder's TOC.xml, which gives an id a file by its path in the folder:
//
//   <TOC>
//     <File Id="3152" Path="cars/z33/z33_a_tr.node"/>
//   </TOC>
//
// TOC.xml is only read at the folder's root. Its entries win over files
// named by the same id; a file it lists isn't also read by its name. Names
// that start with a dot (a submodule's .git) are skipped, files or folders.
// The folders are read once, at Archive::Init: a load looks its id up in a
// table, so neither the folder walk nor TOC.xml costs anything per load.
//
// It uses the game's own loose-file support (docs/game_data.md): an entry
// whose data is a file on disc has a start sector of 0, and
// Archive::FindLooseFile (sub_8209D9F8) gives the file's path. After
// Archive::Init (sub_8209E348) has read the TOCs, each override's TOC entry is
// changed in memory to start sector 0, stored size = the file's size, and
// decompressed size 0 (stored uncompressed); FindLooseFile then returns the
// override's path for that id. Archive::Load (sub_8209DF80) reads the sizes
// from the TOC on every load, and so do the callers that size their buffers
// from it (sub_820C9700 for the parameter tables), so a file may be larger or
// smaller than the original. Its format is still up to the modder.
//
// Languages (docs/modding-plan.md, "Languages"): the same overlay also serves
// a language's files, chosen by --language, from
//
//   assets/ADDON_DATA/LANGUAGES/<LANG>/<id>.<any extension>       text, screens
//   assets/ADDON_DATA/LANGUAGES/FONTS/<SDF|SD>/<SCRIPT>/<id>.<ext>  fonts
//
// Every id found in any language folder or font folder is a "language id".
// For those ids a PATCH_DATA file is ignored, so text left in PATCH_DATA
// can't show up in another language; the chosen language's file is served,
// else the disc's. A font id is looked for in FONTS/SDF/ for the language's
// script (the distance-field fonts sbxr_tools' font_upscale makes, sharp at
// any resolution; src/hooks/font.cpp), then FONTS/SD/ (the 720p fonts, the
// fallback), then the disc's. Without --native_shaders, which the SDF fonts
// need, only SD. A language's
// script comes from its language.xml (<Language Name="English" Font="LATIN"/>);
// without one, JP uses JAPANESE and every other language LATIN. Until
// LANGUAGES/ holds files nothing changes. Phase 1 of the language plan: read
// once at Archive::Init, so a change applies on the next start.
//
// The .TOC and .DAT files on disk aren't changed. Only existing ids can be
// replaced: adding ids needs the entry counts built into the XEX patched.
// New content goes in ADDON_DATA, for the addon loader (docs/modding-plan.md),
// which isn't built yet.
//
// A served file may have full-size textures in a sidecar beside it,
// `<same name>.sbpr` (src/hooks/sidecar.cpp): sidecars aren't archive files
// themselves, and are handed to the sidecar code with the files served.
//
// --patch_loader=false turns it all off.

#include "hooks/patch_loader.h"
#include "base/settings.h"
#include "hooks/native_calls.h"

#include "game/archive.h"
#include "guest.h"
#include "hooks/sidecar.h"

#include <rex/logging.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

SBX_SETTING_BOOL(patch_loader, true, "Hooks", "Patches",
                 "Load archive entries from PATCH_DATA/BUILD/TXR/ (experimental)");
SBX_SETTING_STRING(language, "", "Hooks", "Language",
                   "The game's language: a folder in ADDON_DATA/LANGUAGES/ (EN, JP, ...). "
                   "Empty: EN if that folder exists, else JP (the disc's text). Read at "
                   "startup", sbx::kRestart);
// The distance-field fonts draw right only through the hand-written shaders
// (src/renderer/vulkan/glsl/boot/font_sdf.glsl).
SBX_SETTING_DECLARE(bool, native_shaders);

namespace {

constexpr uint32_t kBuildCount = 23009;    // BUILD ids are 0-23008.
constexpr uint32_t kBuildCrsCount = 1359;  // BUILD_CRS ids are 23009-24367.
constexpr uint32_t kBuildTocPointer = 0x82694E00;
constexpr uint32_t kBuildCrsTocPointer = 0x82694E08;

struct Override {
  uint32_t id;
  uint32_t size;
  std::string path;  // Relative to game:\, as the loose-file table's.
  uint32_t guest_path = 0;
};

// A loose file found in a folder: its id, size and game:\ path.
struct Found {
  uint32_t id;
  uint32_t size;
  std::string path;
};

std::filesystem::path g_game_data_root;
// Archive id -> override. Filled once in Archive::Init, read-only after.
std::unordered_map<uint32_t, Override> g_overrides;

// For the F4 menu: the languages found, and which one is in use.
std::mutex g_languages_mutex;
std::vector<patch_loader::Language> g_languages;
std::string g_active_language;

std::string Upper(std::string text) {
  for (char& c : text) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  return text;
}

// The file's name if it's printable ASCII (the guest path is a C string).
std::optional<std::string> AsciiName(const std::filesystem::path& file) {
  std::string name;
  const std::filesystem::path filename = file.filename();  // kept: native() refers into it
  for (auto c : filename.native()) {
    if (c < 0x20 || c > 0x7E) {
      return std::nullopt;
    }
    name.push_back(char(c));
  }
  return name;
}

// The archive id a file name gives, or nullopt. crs: a BUILD_CRS folder,
// where the id within that archive is also taken. any: a flat folder (the
// languages'), where 0-23008 are BUILD ids and 23009-24367 BUILD_CRS ids.
std::optional<uint32_t> ParseId(const std::string& name, bool crs, bool any = false) {
  std::string stem = name.substr(0, name.find('.'));
  uint32_t n = 0;
  auto [end, ec] = std::from_chars(stem.data(), stem.data() + stem.size(), n);
  if (stem.empty() || ec != std::errc() || end != stem.data() + stem.size()) {
    return std::nullopt;
  }
  if (any) {
    return n < kBuildCount + kBuildCrsCount ? std::optional(n) : std::nullopt;
  }
  if (!crs) {
    return n < kBuildCount ? std::optional(n) : std::nullopt;
  }
  if (n < kBuildCrsCount) {
    return kBuildCount + n;
  }
  if (n >= kBuildCount && n < kBuildCount + kBuildCrsCount) {
    return n;
  }
  return std::nullopt;
}

// The regular files in `dir`, sorted; empty if it isn't a folder.
std::vector<std::filesystem::path> FilesIn(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return files;
  }
  std::filesystem::directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

// The sub-folders of `dir` (names), sorted.
std::vector<std::string> FoldersIn(const std::filesystem::path& dir) {
  std::vector<std::string> folders;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return folders;
  }
  std::filesystem::directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec)) {
      if (auto name = AsciiName(it->path())) {
        folders.push_back(*name);
      }
    }
  }
  std::sort(folders.begin(), folders.end());
  return folders;
}

// The archive files in `dir` (on disk), given their game:\ folder `guest_dir`
// (backslashes, no trailing one) and how their names are read. `label` names
// the folder in the log.
std::vector<Found> ScanFolder(const std::filesystem::path& dir, const std::string& guest_dir,
                              const std::string& label, bool crs, bool any) {
  std::vector<Found> found;
  for (const auto& file : FilesIn(dir)) {
    auto name = AsciiName(file);
    if (!name) {
      REXLOG_WARN("patch_loader: skipped a file in {}: not an ASCII name", label);
      continue;
    }
    if (Upper(*name) == "LANGUAGE.XML") {
      continue;  // a language's settings, not an archive file
    }
    if (Upper(file.extension().string()) == ".SBPR") {
      continue;  // a sidecar: read with the file it's beside
    }
    auto id = ParseId(*name, crs, any);
    if (!id) {
      REXLOG_WARN("patch_loader: skipped {}/{}: not an archive id", label, *name);
      continue;
    }
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(file, ec);
    if (ec || size > UINT32_MAX) {
      REXLOG_WARN("patch_loader: skipped {}/{}: can't read its size", label, *name);
      continue;
    }
    auto same = std::find_if(found.begin(), found.end(),
                             [&](const Found& f) { return f.id == *id; });
    if (same != found.end()) {
      REXLOG_WARN("patch_loader: skipped {}/{}: id {} is already given by {}", label, *name, *id,
                  same->path);
      continue;
    }
    found.push_back({*id, uint32_t(size), guest_dir + "\\" + *name});
  }
  return found;
}

// A file under a patch folder: where it is on disk, and its path in the
// folder (backslashes, as the guest path takes it).
struct TreeFile {
  std::filesystem::path file;
  std::string relative;
};

// A folder path as the log shows it ('/' between folders).
std::string Shown(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  return path;
}

// The regular files under `dir`, sub-folders included, sorted by their path
// in it. Names starting with a dot are skipped (files and folders), and so
// are names that aren't printable ASCII (logged).
std::vector<TreeFile> FilesUnder(const std::filesystem::path& dir, const std::string& label) {
  std::vector<TreeFile> files;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return files;
  }
  std::filesystem::recursive_directory_iterator it(dir, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    auto name = AsciiName(it->path());
    bool dot = name && !name->empty() && (*name)[0] == '.';
    if (it->is_directory(ec)) {
      if (!name || dot) {
        if (!name) {
          REXLOG_WARN("patch_loader: skipped a folder in {}: not an ASCII name", label);
        }
        it.disable_recursion_pending();
      }
      continue;
    }
    if (dot || !it->is_regular_file(ec)) {
      continue;
    }
    if (!name) {
      REXLOG_WARN("patch_loader: skipped a file in {}: not an ASCII name", label);
      continue;
    }
    // The folders above it are ASCII (others aren't walked into).
    std::string relative;
    for (const auto& part : it->path().lexically_relative(dir)) {
      if (!relative.empty()) {
        relative += '\\';
      }
      relative += *AsciiName(part);
    }
    files.push_back({it->path(), relative});
  }
  std::sort(files.begin(), files.end(), [](const TreeFile& a, const TreeFile& b) {
    return Upper(a.relative) < Upper(b.relative);
  });
  return files;
}

// An attribute's value in a small XML element's text, or "" (enough for
// language.xml's one element; a malformed file just gives no values).
std::string XmlAttribute(const std::string& text, const std::string& name) {
  size_t at = 0;
  while ((at = text.find(name, at)) != std::string::npos) {
    bool starts_word = at == 0 || std::isspace(static_cast<unsigned char>(text[at - 1]));
    size_t eq = text.find_first_not_of(" \t\r\n", at + name.size());
    if (starts_word && eq != std::string::npos && text[eq] == '=') {
      size_t quote = text.find_first_not_of(" \t\r\n", eq + 1);
      if (quote != std::string::npos && (text[quote] == '"' || text[quote] == '\'')) {
        size_t close = text.find(text[quote], quote + 1);
        if (close != std::string::npos) {
          return text.substr(quote + 1, close - quote - 1);
        }
      }
    }
    at += name.size();
  }
  return {};
}

// XML's five entities read back (&amp; last, so "&amp;lt;" stays "&lt;").
std::string XmlUnescape(std::string text) {
  for (auto [from, to] : {std::pair{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"},
                          {"&amp;", "&"}}) {
    std::string f = from;
    for (size_t at = 0; (at = text.find(f, at)) != std::string::npos; at += std::strlen(to)) {
      text.replace(at, f.size(), to);
    }
  }
  return text;
}

// One entry of a patch folder's TOC.xml: an id and the file's path in the
// folder (backslashes).
struct TocEntry {
  uint32_t id;
  std::string relative;
};

// A TOC.xml path as the guest path takes it ('/' or '\' between folders,
// "." parts dropped), or nullopt if it leaves the folder or isn't printable
// ASCII.
std::optional<std::string> TocPath(const std::string& path) {
  std::string relative, part;
  auto flush = [&]() -> bool {
    if (part == "..") {
      return false;
    }
    if (!part.empty() && part != ".") {
      relative += (relative.empty() ? "" : "\\") + part;
    }
    part.clear();
    return true;
  };
  if (path.empty() || path[0] == '/' || path[0] == '\\') {
    return std::nullopt;
  }
  for (char c : path) {
    if (c < 0x20 || c > 0x7E || c == ':') {
      return std::nullopt;
    }
    if (c == '/' || c == '\\') {
      if (!flush()) {
        return std::nullopt;
      }
    } else {
      part.push_back(c);
    }
  }
  if (!flush() || relative.empty()) {
    return std::nullopt;
  }
  return relative;
}

// The <File Id="..." Path="..."/> entries of `dir`'s TOC.xml (none without
// one). Ids read as file names do (crs: BUILD_CRS's two kinds). Comments are
// skipped; a bad entry is logged and skipped.
std::vector<TocEntry> ReadToc(const std::filesystem::path& dir, const std::string& label,
                              bool crs) {
  std::vector<TocEntry> entries;
  std::filesystem::path xml;
  for (const auto& file : FilesIn(dir)) {
    if (auto name = AsciiName(file); name && Upper(*name) == "TOC.XML") {
      xml = file;
    }
  }
  if (xml.empty()) {
    return entries;
  }
  std::ifstream in(xml, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  std::string body = text.str();
  for (size_t at; (at = body.find("<!--")) != std::string::npos;) {
    size_t close = body.find("-->", at + 4);
    body.erase(at, close == std::string::npos ? std::string::npos : close + 3 - at);
  }
  if (!in || body.find("<TOC") == std::string::npos) {
    REXLOG_WARN("patch_loader: {}/TOC.xml has no <TOC> element; ignored", label);
    return entries;
  }
  for (size_t at = 0; (at = body.find("<File", at)) != std::string::npos; at += 5) {
    char next = at + 5 < body.size() ? body[at + 5] : '>';
    if (!std::isspace(static_cast<unsigned char>(next)) && next != '/' && next != '>') {
      continue;  // another element (<Files>, say)
    }
    size_t close = body.find('>', at);
    std::string tag = body.substr(at, close == std::string::npos ? std::string::npos : close - at);
    std::string id_text = XmlAttribute(tag, "Id");
    std::string path_text = XmlUnescape(XmlAttribute(tag, "Path"));
    auto id = ParseId(id_text, crs);
    auto relative = TocPath(path_text);
    if (!id || !relative) {
      REXLOG_WARN("patch_loader: {}/TOC.xml: skipped <File Id=\"{}\" Path=\"{}\">: {}", label,
                  id_text, path_text, !id ? "not an archive id" : "not a path in the folder");
      continue;
    }
    entries.push_back({*id, *relative});
  }
  return entries;
}

// A patch folder's files: those TOC.xml lists, then the rest of the folder's
// tree named by an id. The first file found for an id wins (TOC.xml's, else
// the first by path); the others are logged.
std::vector<Found> ScanPatchFolder(const std::filesystem::path& dir, const std::string& guest_dir,
                                   const std::string& label, bool crs) {
  std::vector<Found> found;
  std::set<std::string> listed;  // TOC.xml's paths, upper case
  auto add = [&](uint32_t id, const std::filesystem::path& file, const std::string& relative) {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(file, ec);
    if (ec || size > UINT32_MAX) {
      REXLOG_WARN("patch_loader: skipped {}/{}: can't read its size", label, Shown(relative));
      return;
    }
    auto same =
        std::find_if(found.begin(), found.end(), [&](const Found& f) { return f.id == id; });
    if (same != found.end()) {
      REXLOG_WARN("patch_loader: skipped {}/{}: id {} is already given by {}", label,
                  Shown(relative), id, same->path);
      return;
    }
    found.push_back({id, uint32_t(size), guest_dir + "\\" + relative});
  };

  const std::vector<TocEntry> toc = ReadToc(dir, label, crs);
  for (const TocEntry& entry : toc) {
    std::filesystem::path file = dir;
    for (size_t at = 0; at <= entry.relative.size();) {
      size_t cut = std::min(entry.relative.find('\\', at), entry.relative.size());
      file /= entry.relative.substr(at, cut - at);
      at = cut + 1;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
      REXLOG_WARN("patch_loader: {}/TOC.xml: id {}: {} not found", label, entry.id,
                  Shown(entry.relative));
      continue;
    }
    listed.insert(Upper(entry.relative));
    add(entry.id, file, entry.relative);
  }

  size_t named = 0;
  for (const TreeFile& f : FilesUnder(dir, label)) {
    const std::string upper = Upper(f.relative);
    if (listed.count(upper) || upper == "TOC.XML") {
      continue;
    }
    if (Upper(f.file.extension().string()) == ".SBPR") {
      continue;  // a sidecar: read with the file it's beside
    }
    auto id = ParseId(*AsciiName(f.file), crs);
    if (!id) {
      REXLOG_WARN("patch_loader: skipped {}/{}: not an archive id, and not in TOC.xml", label,
                  Shown(f.relative));
      continue;
    }
    size_t before = found.size();
    add(*id, f.file, f.relative);
    named += found.size() - before;
  }
  if (!found.empty()) {
    REXLOG_INFO("patch_loader: {}: {} files ({} by TOC.xml, {} by their names)", label,
                found.size(), found.size() - named, named);
  }
  return found;
}

void ScanPatchData(std::vector<Found>& found, bool crs) {
  const std::string folder = crs ? "BUILD_CRS" : "BUILD";
  auto dir = g_game_data_root / "PATCH_DATA" / "BUILD" / "TXR" / folder;
  for (Found& f : ScanPatchFolder(dir, "PATCH_DATA\\BUILD\\TXR\\" + folder,
                                  "PATCH_DATA/BUILD/TXR/" + folder, crs)) {
    found.push_back(std::move(f));
  }
}

// A language folder's language.xml: its display name and font script.
patch_loader::Language ReadLanguage(const std::filesystem::path& dir, const std::string& code) {
  patch_loader::Language language;
  language.code = code;
  language.name = code;
  language.script = Upper(code) == "JP" ? "JAPANESE" : "LATIN";
  std::filesystem::path xml;
  for (const auto& file : FilesIn(dir)) {
    if (auto name = AsciiName(file); name && Upper(*name) == "LANGUAGE.XML") {
      xml = file;
    }
  }
  if (xml.empty()) {
    return language;
  }
  std::ifstream in(xml, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  std::string body = text.str();
  size_t element = body.find("<Language");
  if (!in || element == std::string::npos) {
    REXLOG_WARN("patch_loader: LANGUAGES/{}/language.xml has no <Language> element; using the "
                "defaults (font {})",
                code, language.script);
    return language;
  }
  size_t close = body.find('>', element);
  std::string tag = body.substr(element, close == std::string::npos ? std::string::npos
                                                                      : close - element);
  if (std::string name = XmlAttribute(tag, "Name"); !name.empty()) {
    language.name = name;
  }
  if (std::string font = XmlAttribute(tag, "Font"); !font.empty()) {
    language.script = Upper(font);
  }
  return language;
}

// The languages and fonts in ADDON_DATA/LANGUAGES/, applied to `found` (the
// PATCH_DATA files, changed in place): language ids lose their PATCH_DATA
// file, and get the chosen language's (and its fonts') instead.
void ApplyLanguages(std::vector<Found>& found) {
  const auto root = g_game_data_root / "ADDON_DATA" / "LANGUAGES";
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec)) {
    return;
  }

  // Each language folder's files, and each font folder's (quality, script).
  std::map<std::string, std::vector<Found>> language_files;  // by upper-case code
  std::vector<patch_loader::Language> languages;
  std::string fonts_folder;
  for (const std::string& folder : FoldersIn(root)) {
    if (Upper(folder) == "FONTS") {
      fonts_folder = folder;
      continue;
    }
    language_files[Upper(folder)] =
        ScanFolder(root / folder, "ADDON_DATA\\LANGUAGES\\" + folder, "LANGUAGES/" + folder,
                   false, true);
    languages.push_back(ReadLanguage(root / folder, folder));
  }
  // Fonts: (quality, script), both upper case, -> files.
  std::map<std::pair<std::string, std::string>, std::vector<Found>> font_files;
  if (!fonts_folder.empty()) {
    for (const std::string& quality : FoldersIn(root / fonts_folder)) {
      for (const std::string& script : FoldersIn(root / fonts_folder / quality)) {
        const std::string guest =
            "ADDON_DATA\\LANGUAGES\\" + fonts_folder + "\\" + quality + "\\" + script;
        font_files[{Upper(quality), Upper(script)}] =
            ScanFolder(root / fonts_folder / quality / script, guest,
                       "LANGUAGES/FONTS/" + quality + "/" + script, false, true);
      }
    }
  }

  std::set<uint32_t> language_ids, font_ids;
  for (const auto& [code, files] : language_files) {
    for (const Found& f : files) {
      language_ids.insert(f.id);
    }
  }
  for (const auto& [key, files] : font_files) {
    for (const Found& f : files) {
      font_ids.insert(f.id);
    }
  }
  {
    std::lock_guard lock(g_languages_mutex);
    g_languages = languages;
  }
  if (language_ids.empty() && font_ids.empty()) {
    REXLOG_INFO("patch_loader: ADDON_DATA/LANGUAGES/ holds no archive files; languages off");
    return;
  }

  // The language: --language, else EN if it exists, else JP (the disc's).
  auto has = [&](const std::string& code) { return language_files.count(Upper(code)) != 0; };
  std::string wanted = Upper(SBX_GET(language));
  if (!wanted.empty() && !has(wanted) && wanted != "JP") {
    REXLOG_WARN("patch_loader: --language={}: no such folder in ADDON_DATA/LANGUAGES/; "
                "choosing one",
                wanted);
    wanted.clear();
  }
  if (wanted.empty()) {
    wanted = has("EN") ? "EN" : "JP";
  }
  patch_loader::Language chosen;
  chosen.code = wanted;
  chosen.name = wanted;
  chosen.script = wanted == "JP" ? "JAPANESE" : "LATIN";
  for (const auto& language : languages) {
    if (Upper(language.code) == wanted) {
      chosen = language;
    }
  }
  {
    std::lock_guard lock(g_languages_mutex);
    g_active_language = chosen.code;
  }

  // Language ids served from PATCH_DATA: dropped, logged once each.
  std::set<uint32_t> all_ids = language_ids;
  all_ids.insert(font_ids.begin(), font_ids.end());
  found.erase(std::remove_if(found.begin(), found.end(),
                             [&](const Found& f) {
                               if (!all_ids.count(f.id)) {
                                 return false;
                               }
                               REXLOG_INFO("patch_loader: id {}: {} ignored (a language id; "
                                           "the language folders serve it)",
                                           f.id, f.path);
                               return true;
                             }),
              found.end());

  // The chosen language's own files.
  size_t text_count = 0;
  if (auto it = language_files.find(Upper(chosen.code)); it != language_files.end()) {
    for (const Found& f : it->second) {
      found.push_back(f);
      text_count++;
    }
  }

  // Its fonts: SDF for its script, then SD. SDF only with the hand-written
  // shaders, which draw its distance fields (the translated ones would show
  // them as blurred coverage).
  const bool sdf = SBX_GET(native_shaders);
  if (!sdf) {
    REXLOG_INFO("patch_loader: the SDF fonts need native_shaders; using the SD fonts");
  }
  const std::string first = sdf ? "SDF" : "SD";
  const std::vector<std::string> qualities =
      sdf ? std::vector<std::string>{"SDF", "SD"} : std::vector<std::string>{"SD"};
  size_t font_count = 0;
  for (uint32_t id : font_ids) {
    const Found* pick = nullptr;
    for (const std::string& quality : qualities) {
      auto it = font_files.find({quality, chosen.script});
      if (it == font_files.end()) {
        continue;
      }
      for (const Found& f : it->second) {
        if (f.id == id) {
          pick = &f;
          break;
        }
      }
      if (pick) {
        break;
      }
    }
    if (pick) {
      found.push_back(*pick);
      font_count++;
    } else {
      REXLOG_INFO("patch_loader: font id {}: none for script {}; the disc's", id, chosen.script);
    }
  }
  REXLOG_INFO("patch_loader: language {} ({}), font {} ({} first): {} text files, {} fonts; "
              "{} language ids in all, the rest from the disc",
              chosen.code, chosen.name, chosen.script, first, text_count, font_count,
              all_ids.size());
}

}  // namespace

namespace patch_loader {

void SetGameDataRoot(const std::filesystem::path& root) { g_game_data_root = root; }

std::vector<Language> Languages() {
  std::lock_guard lock(g_languages_mutex);
  return g_languages;
}

std::string ActiveLanguage() {
  std::lock_guard lock(g_languages_mutex);
  return g_active_language;
}

}  // namespace patch_loader

// Archive::Init: reads both TOCs, then the overrides are applied to them.
extern "C" REX_FUNC(sub_8209E348) {
  hooks::NativeArchive_Init(ctx, base);
  if (!SBX_GET(patch_loader) || g_game_data_root.empty()) {
    return;
  }

  std::vector<Found> files;
  ScanPatchData(files, false);
  ScanPatchData(files, true);
  ApplyLanguages(files);
  if (files.empty()) {
    return;
  }
  std::vector<std::filesystem::path> served;
  for (const Found& f : files) {
    std::string relative = f.path;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    served.push_back(g_game_data_root / relative);
  }
  sidecar::IndexSidecars(served);
  std::vector<Override> found;
  for (Found& f : files) {
    found.push_back({f.id, f.size, std::move(f.path)});
  }

  // Guest copies of the paths for FindLooseFile to return, from the game's
  // operator new (sub_8258C000). Never freed: queued loads keep the pointer.
  uint32_t total = 0;
  for (const auto& o : found) {
    total += uint32_t(o.path.size()) + 1;
  }
  uint64_t result = ctx.r3.u64;
  ctx.r3.u64 = total;
  sub_8258C000(ctx, base);
  uint32_t strings = ctx.r3.u32;
  ctx.r3.u64 = result;
  if (strings == 0) {
    REXLOG_ERROR("patch_loader: couldn't allocate {} bytes for the paths; no overrides", total);
    return;
  }

  for (auto& o : found) {
    o.guest_path = strings;
    for (char c : o.path) {
      guest::StoreU8(base, strings++, uint8_t(c));
    }
    guest::StoreU8(base, strings++, 0);

    bool crs = o.id >= kBuildCount;
    uint32_t toc = guest::LoadU32(base, crs ? kBuildCrsTocPointer : kBuildTocPointer);
    uint32_t entry = toc + 16 * ((crs ? o.id - kBuildCount : o.id) + 1);
    uint32_t stored = guest::LoadU32(base, entry + 4);
    uint32_t unpacked = guest::LoadU32(base, entry + 8);
    guest::StoreU32(base, entry + 0, 0);       // Start sector: a loose file.
    guest::StoreU32(base, entry + 4, o.size);  // Stored size.
    guest::StoreU32(base, entry + 8, 0);       // Decompressed size: stored as is.

    REXLOG_INFO("patch_loader: id {} from {} ({} bytes; was {} stored, {} unpacked)", o.id,
                o.path, o.size, stored, unpacked);
    g_overrides.emplace(o.id, o);
  }
}

// Archive::FindLooseFile: r3 = an archive id; returns its loose file's path:
// a PATCH_DATA override's, else the game's (our walk of its table with
// --native_archive, src/game/archive.cpp).
extern "C" REX_FUNC(sub_8209D9F8) {
  auto it = g_overrides.find(ctx.r3.u32);
  if (it != g_overrides.end()) {
    REXLOG_INFO("patch_loader: loading id {} from {}", it->first, it->second.path);
    ctx.r3.u64 = it->second.guest_path;
    return;
  }
  if (game::NativeArchive()) {
    guest::Guest g(ctx, base);
    ctx.r3.u64 = game::FindLooseFile(ctx.r3.u32);
    return;
  }
  __imp__sub_8209D9F8(ctx, base);
}
