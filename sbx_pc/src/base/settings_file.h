// The settings file: sbx.toml next to sbx.exe (src/base/settings_file.cpp).
//
// It holds one setting per line, `name = value`, flat (no [sections]: the
// SDK would read `[Input]` and `mnk_mode` as a flag named
// `Input_mnk_mode`). Our settings (base/settings.h) are read from it by
// Load here, at every start; ReXGlue reads its own cvars from the same file
// itself, right after (ours aren't among its cvars). Both rank the file below REX_<NAME>
// environment variables and the command line, which win over it. The F4
// menu's Save button writes it through Save here.

#pragma once

#include "base/settings.h"

#include <rex/cvar.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace settings_file {

// The file the SDK reads (PathConfig::config_path). Set by SbxApp before the
// SDK loads it.
void SetPath(const std::filesystem::path& path);
const std::filesystem::path& Path();

// Reads our settings from the file (a missing file is no error). A value
// the environment or the command line already set is kept. Lines that
// aren't ours (ReXGlue's cvars, its [log.levels] table) are left to the SDK.
// Returns how many settings were set from the file.
int Load();

// A value's text as written in the file (after the '=', the comment cut
// off) as plain text: a string's contents unescaped, anything else as it
// stands. Returns false for an unfinished or badly escaped string.
bool ReadTOMLValue(std::string_view text, std::string& out);

// A value as TOML text: true/false, a number, or a quoted string. For one
// of ReXGlue's cvars, and for one of ours.
std::string TOMLValue(rex::cvar::FlagType type, std::string_view value);
std::string TOMLValue(sbx::SettingType type, std::string_view value);

// One setting to write.
struct Entry {
  std::string name;
  std::string value;     // TOMLValue's text
  bool is_default = false;
  std::string section;   // a comment over new lines, e.g. "Graphics / Window"
};

// Writes `entries` into the file, keeping everything else in it:
// - a setting already in the file has its value replaced in its line (its
//   comment kept);
// - a new setting is added after the file's other settings, under its
//   section's comment, unless it's at its default (nothing to keep);
// - other lines (comments, settings not given, [log.levels] and other
//   tables) are kept as they are.
// The file is written to a temporary file first and then moved over the old
// one. Returns false with `error` set if it couldn't be written.
bool Save(const std::vector<Entry>& entries, std::string& error);

}  // namespace settings_file
