// SBX's own settings: the flags our code reads (--native_<name>, --subtick,
// --width and the rest), each with its name, default, place in the F4 menu,
// description, and whether a change applies at once or after a restart.
//
// Defining one, at file scope outside any namespace, in one .cpp file:
//
//   SBX_SETTING_BOOL(subtick, true, "Graphics", "Frame Rate",
//                    "Draws frames between the game's ticks.");
//   SBX_SETTING_INT(width, 0, "Graphics", "Window", "Window width (0: the desktop's).",
//                   sbx::kRestart);
//
// Reading it, from any thread, as often as needed (one relaxed atomic load;
// a string setting returns a reference, without a lock or a copy):
//
//   SBX_SETTING_DECLARE(bool, subtick);  // in another file, at file scope
//   if (SBX_GET(subtick)) { ... }
//
// Values come from, in rising priority: the default, sbx.toml
// (settings_file.h), REX_<NAME> environment variables, the command line
// (--name=value or --name value), and the F4 menu. A source never replaces
// a value a higher one already set, as with ReXGlue's cvars.
//
// Our settings are not ReXGlue cvars: the command line is filtered before
// ReXGlue's parser sees it (main.cpp), and ReXGlue's own cvars (input binds,
// log_level and the like) stay in its registry (rex/cvar.h).

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace sbx {

enum class SettingType : uint8_t { kBool, kInt, kFloat, kDouble, kString };

// When a change takes effect.
enum class Applies : uint8_t {
  kLive,     // at once (read every time it's used)
  kRestart,  // at the next start (read once, at startup or first use)
};
inline constexpr Applies kLive = Applies::kLive;
inline constexpr Applies kRestart = Applies::kRestart;

// Where a setting's value came from, in rising priority.
enum class SettingSource : uint8_t {
  kDefault,
  kConfig,       // sbx.toml
  kEnvironment,  // REX_<NAME>
  kCommandLine,  // --name=value
  kRuntime,      // the F4 menu or code
};

// What every setting has, whatever its type. The registry holds these.
class SettingBase {
 public:
  SettingBase(const SettingBase&) = delete;
  SettingBase& operator=(const SettingBase&) = delete;

  std::string_view name() const { return name_; }
  std::string_view group() const { return group_; }        // the F4 tab, e.g. "Overrides"
  std::string_view subgroup() const { return subgroup_; }  // its group in the tab, e.g. "Race"
  std::string_view description() const { return description_; }
  SettingType type() const { return type_; }
  bool restart() const { return applies_ == Applies::kRestart; }
  SettingSource source() const { return source_.load(std::memory_order_relaxed); }

  // The value and the default as text: true/false, a number, or the string.
  virtual std::string ValueText() const = 0;
  virtual std::string DefaultText() const = 0;
  bool IsDefault() const { return ValueText() == DefaultText(); }

  // Parses `text` for this setting's type. Returns false if it doesn't
  // parse. A value that parses but loses to a higher source already set is
  // skipped and still returns true.
  virtual bool SetText(std::string_view text, SettingSource source) = 0;

  // Back to the default, as a change from the menu.
  void ResetToDefault() { SetText(DefaultText(), SettingSource::kRuntime); }

 protected:
  constexpr SettingBase(SettingType type, std::string_view name, std::string_view group,
                        std::string_view subgroup, std::string_view description, Applies applies)
      : name_(name),
        group_(group),
        subgroup_(subgroup),
        description_(description),
        type_(type),
        applies_(applies) {}
  ~SettingBase() = default;

  // Whether a value from `source` may replace the current one; if so, the
  // source is recorded. Called with the write lock held.
  bool Accept(SettingSource source);

 private:
  std::string_view name_;
  std::string_view group_;
  std::string_view subgroup_;
  std::string_view description_;
  SettingType type_;
  Applies applies_;
  std::atomic<SettingSource> source_{SettingSource::kDefault};
};

namespace settings_detail {
// Text parsing shared by every source. Booleans take true/false, 1/0,
// yes/no and on/off (any case).
bool ParseBool(std::string_view text, bool& out);
bool ParseInt(std::string_view text, int32_t& out);
bool ParseDouble(std::string_view text, double& out);
std::string DoubleText(double value);
// Serializes writes (reads take no lock).
void LockWrites();
void UnlockWrites();
// Tells the registry's listeners a setting changed (after the lock is
// released).
void NotifyChanged(const SettingBase& setting);
}  // namespace settings_detail

// A bool, int32_t, float or double setting: an atomic, so reads are one
// load.
template <typename T>
class Setting final : public SettingBase {
  static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int32_t> ||
                    std::is_same_v<T, float> || std::is_same_v<T, double>,
                "a setting is bool, int32_t, float, double or std::string");

 public:
  constexpr Setting(std::string_view name, T default_value, std::string_view group,
                    std::string_view subgroup, std::string_view description,
                    Applies applies = Applies::kLive)
      : SettingBase(TypeOf(), name, group, subgroup, description, applies),
        default_(default_value),
        value_(default_value) {}

  T Get() const { return value_.load(std::memory_order_relaxed); }
  T Default() const { return default_; }

  // Sets the value (skipped if a higher source already set it).
  void Set(T value, SettingSource source = SettingSource::kRuntime) {
    settings_detail::LockWrites();
    bool changed = false;
    if (Accept(source)) {
      changed = value_.exchange(value, std::memory_order_relaxed) != value;
    }
    settings_detail::UnlockWrites();
    if (changed) {
      settings_detail::NotifyChanged(*this);
    }
  }

  std::string ValueText() const override { return Text(Get()); }
  std::string DefaultText() const override { return Text(default_); }

  bool SetText(std::string_view text, SettingSource source) override {
    T value{};
    if constexpr (std::is_same_v<T, bool>) {
      if (!settings_detail::ParseBool(text, value)) return false;
    } else if constexpr (std::is_same_v<T, int32_t>) {
      if (!settings_detail::ParseInt(text, value)) return false;
    } else {
      double number = 0;
      if (!settings_detail::ParseDouble(text, number)) return false;
      value = T(number);
    }
    Set(value, source);
    return true;
  }

 private:
  static constexpr SettingType TypeOf() {
    if constexpr (std::is_same_v<T, bool>) {
      return SettingType::kBool;
    } else if constexpr (std::is_same_v<T, int32_t>) {
      return SettingType::kInt;
    } else if constexpr (std::is_same_v<T, float>) {
      return SettingType::kFloat;
    } else {
      return SettingType::kDouble;
    }
  }

  static std::string Text(T value) {
    if constexpr (std::is_same_v<T, bool>) {
      return value ? "true" : "false";
    } else if constexpr (std::is_same_v<T, int32_t>) {
      return std::to_string(value);
    } else {
      return settings_detail::DoubleText(double(value));
    }
  }

  T default_;
  std::atomic<T> value_;
};

// A string setting. The value is an immutable string behind an atomic
// pointer: a reader gets a reference without a lock or a copy. A change
// publishes a new string and keeps the old one alive (changes are rare: the
// menu, the file, the command line), so a reference never dangles.
template <>
class Setting<std::string> final : public SettingBase {
 public:
  constexpr Setting(std::string_view name, std::string_view default_value,
                    std::string_view group, std::string_view subgroup,
                    std::string_view description, Applies applies = Applies::kLive)
      : SettingBase(SettingType::kString, name, group, subgroup, description, applies),
        default_(default_value) {}

  const std::string& Get() const {
    if (const std::string* value = value_.load(std::memory_order_acquire)) {
      return *value;
    }
    return FirstValue();
  }
  std::string_view Default() const { return default_; }

  void Set(std::string_view value, SettingSource source = SettingSource::kRuntime);

  std::string ValueText() const override { return Get(); }
  std::string DefaultText() const override { return std::string(default_); }
  bool SetText(std::string_view text, SettingSource source) override {
    Set(text, source);
    return true;
  }

 private:
  // The default as a string, made on the first read.
  const std::string& FirstValue() const;

  std::string_view default_;
  mutable std::atomic<const std::string*> value_{nullptr};
};

// Adds a setting to the registry (from the SBX_SETTING_* macros, during
// static initialization).
class SettingRegistrar {
 public:
  explicit SettingRegistrar(SettingBase& setting);
};

namespace settings {

// Every setting, in registration order.
std::span<SettingBase* const> All();

// A setting by name, or null.
SettingBase* Find(std::string_view name);

// Called after a setting's value changes (from any thread that changed it).
// For code that has to react to a change at once; most code just reads the
// value where it's used.
using ChangeListener = std::function<void(const SettingBase&)>;
void AddChangeListener(ChangeListener listener);

// Applies our settings found on the command line (args[0] is the program)
// and returns the positions of the arguments they used, in order, so the
// caller can take them off before ReXGlue parses the rest (it may reject
// names it doesn't know). Accepted: --name=value, --name value, and for
// booleans --name (true), --name=false, --name false and --no-name.
// Everything after a bare "--" is left alone. An argument naming one of our
// settings is always taken, even with a bad value (logged).
std::vector<size_t> ApplyCommandLine(std::span<const std::string> args);

// Applies REX_<NAME> environment variables (the name in capitals) for our
// settings. `lookup` is std::getenv unless a test passes its own.
void ApplyEnvironment(const std::function<const char*(const char*)>& lookup = {});

// The command line, then the environment. Called once, before ReXGlue sees
// the command line (main.cpp); returns what ApplyCommandLine returned.
std::vector<size_t> ApplyLaunchOptions(std::span<const std::string> args);

// Whether ApplyLaunchOptions ran (checked at startup: if the build didn't
// filter the command line, our options weren't applied).
bool LaunchOptionsApplied();

// The variable the environment is read from: REX_ and the name in capitals.
std::string EnvironmentName(std::string_view name);

}  // namespace settings
}  // namespace sbx

// The macros. `group` and `subgroup` place the setting in the F4 menu (its
// tab and the group within it); a trailing sbx::kRestart marks a setting
// that only takes effect at the next start.
#define SBX_SETTING_DEFINE_(type, name, default_val, group, subgroup, desc, ...)              \
  constinit ::sbx::Setting<type> sbx_setting_##name(#name, default_val, group, subgroup,      \
                                                    desc __VA_OPT__(, ) __VA_ARGS__);         \
  static ::sbx::SettingRegistrar sbx_setting_registrar_##name(sbx_setting_##name)

#define SBX_SETTING_BOOL(name, default_val, group, subgroup, desc, ...) \
  SBX_SETTING_DEFINE_(bool, name, default_val, group, subgroup, desc __VA_OPT__(, ) __VA_ARGS__)
#define SBX_SETTING_INT(name, default_val, group, subgroup, desc, ...)                    \
  SBX_SETTING_DEFINE_(int32_t, name, default_val, group, subgroup, desc __VA_OPT__(, ) \
                          __VA_ARGS__)
#define SBX_SETTING_FLOAT(name, default_val, group, subgroup, desc, ...) \
  SBX_SETTING_DEFINE_(float, name, default_val, group, subgroup, desc __VA_OPT__(, ) __VA_ARGS__)
#define SBX_SETTING_DOUBLE(name, default_val, group, subgroup, desc, ...)                \
  SBX_SETTING_DEFINE_(double, name, default_val, group, subgroup, desc __VA_OPT__(, ) \
                          __VA_ARGS__)
#define SBX_SETTING_STRING(name, default_val, group, subgroup, desc, ...)                     \
  SBX_SETTING_DEFINE_(std::string, name, default_val, group, subgroup, desc __VA_OPT__(, ) \
                          __VA_ARGS__)

// A setting defined in another file (at file scope, outside any namespace):
// SBX_SETTING_DECLARE(bool, subtick).
#define SBX_SETTING_DECLARE(type, name) extern ::sbx::Setting<type> sbx_setting_##name

// A setting's value.
#define SBX_GET(name) (sbx_setting_##name.Get())

// The setting itself (Set, source and the like).
#define SBX_SETTING(name) (sbx_setting_##name)
