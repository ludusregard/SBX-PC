// difftest shim for base/settings.h: settings are plain globals, booleans
// all on (as shim/rex/cvar.h does for the SDK's macros). The names match
// the cvar shim's, so dt.py's stubs for undefined ones still apply.
#pragma once
#include <cstdint>
#include <string>
namespace sbx {
enum class Applies { kLive, kRestart };
inline constexpr Applies kLive = Applies::kLive;
inline constexpr Applies kRestart = Applies::kRestart;
}  // namespace sbx
#define SBX_SETTING_BOOL(name, def, group, subgroup, desc, ...) bool cvar_##name = true
#define SBX_SETTING_INT(name, def, group, subgroup, desc, ...) int32_t cvar_##name = def
#define SBX_SETTING_FLOAT(name, def, group, subgroup, desc, ...) float cvar_##name = def
#define SBX_SETTING_DOUBLE(name, def, group, subgroup, desc, ...) double cvar_##name = def
#define SBX_SETTING_STRING(name, def, group, subgroup, desc, ...) std::string cvar_##name = def
#define SBX_SETTING_DECLARE(type, name) extern type cvar_##name
#define SBX_GET(name) cvar_##name
#define SBX_SETTING(name) cvar_##name
