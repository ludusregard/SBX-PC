// SBX's own settings (settings.h): the registry, value parsing, and the
// command line and environment.

#include "base/settings.h"

#include "base/log.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>

namespace sbx {
namespace {

struct Registry {
  std::vector<SettingBase*> all;
  std::map<std::string_view, SettingBase*, std::less<>> by_name;
};

// Filled during static initialization, read-only after that.
Registry& GetRegistry() {
  static Registry registry;
  return registry;
}

std::mutex& WriteMutex() {
  static std::mutex mutex;
  return mutex;
}

std::mutex& ListenerMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<settings::ChangeListener>& Listeners() {
  static std::vector<settings::ChangeListener> listeners;
  return listeners;
}

std::atomic<bool> g_launch_options_applied{false};

// String values replaced by a change: kept for good (a reader may still hold
// one), and never destroyed, not even at exit, while game threads may run.
std::vector<const std::string*>& RetiredStrings() {
  static auto* retired = new std::vector<const std::string*>;
  return *retired;
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return lower;
}

const char* TypeName(SettingType type) {
  switch (type) {
    case SettingType::kBool:
      return "true or false";
    case SettingType::kInt:
      return "a whole number";
    case SettingType::kFloat:
    case SettingType::kDouble:
      return "a number";
    case SettingType::kString:
      return "text";
  }
  return "a value";
}

// A boolean word, for "--name false" (a bare "--name" is true).
bool IsBoolWord(std::string_view text) {
  bool unused = false;
  return settings_detail::ParseBool(text, unused);
}

}  // namespace

bool SettingBase::Accept(SettingSource source) {
  if (source < source_.load(std::memory_order_relaxed)) {
    return false;
  }
  source_.store(source, std::memory_order_relaxed);
  return true;
}

namespace settings_detail {

bool ParseBool(std::string_view text, bool& out) {
  std::string word = Lower(text);
  if (word == "true" || word == "1" || word == "yes" || word == "on") {
    out = true;
    return true;
  }
  if (word == "false" || word == "0" || word == "no" || word == "off") {
    out = false;
    return true;
  }
  return false;
}

bool ParseInt(std::string_view text, int32_t& out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return false;
  }
  int32_t value = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

bool ParseDouble(std::string_view text, double& out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return false;
  }
  double value = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    return false;
  }
  out = value;
  return true;
}

std::string DoubleText(double value) {
  char text[64];
  auto [end, error] = std::to_chars(text, text + sizeof(text), value);
  return error == std::errc() ? std::string(text, end) : std::to_string(value);
}

void LockWrites() {
  WriteMutex().lock();
}

void UnlockWrites() {
  WriteMutex().unlock();
}

void NotifyChanged(const SettingBase& setting) {
  std::vector<settings::ChangeListener> listeners;
  {
    std::lock_guard lock(ListenerMutex());
    listeners = Listeners();
  }
  for (const settings::ChangeListener& listener : listeners) {
    listener(setting);
  }
}

}  // namespace settings_detail

void Setting<std::string>::Set(std::string_view value, SettingSource source) {
  bool changed = false;
  {
    std::lock_guard lock(WriteMutex());
    if (Accept(source) && Get() != value) {
      const std::string* old = value_.exchange(new std::string(value), std::memory_order_acq_rel);
      RetiredStrings().push_back(old);  // a reader may still hold it
      changed = true;
    }
  }
  if (changed) {
    settings_detail::NotifyChanged(*this);
  }
}

const std::string& Setting<std::string>::FirstValue() const {
  const std::string* made = new std::string(default_);
  const std::string* expected = nullptr;
  if (value_.compare_exchange_strong(expected, made, std::memory_order_acq_rel)) {
    return *made;
  }
  delete made;  // another thread made it first; nobody saw ours
  return *expected;
}

SettingRegistrar::SettingRegistrar(SettingBase& setting) {
  Registry& registry = GetRegistry();
  if (!registry.by_name.emplace(setting.name(), &setting).second) {
    ITC_LOG_ERROR("settings: {} is defined twice; the second one is ignored", setting.name());
    return;
  }
  registry.all.push_back(&setting);
}

namespace settings {

std::span<SettingBase* const> All() {
  return GetRegistry().all;
}

SettingBase* Find(std::string_view name) {
  const Registry& registry = GetRegistry();
  auto it = registry.by_name.find(name);
  return it != registry.by_name.end() ? it->second : nullptr;
}

void AddChangeListener(ChangeListener listener) {
  std::lock_guard lock(ListenerMutex());
  Listeners().push_back(std::move(listener));
}

std::vector<size_t> ApplyCommandLine(std::span<const std::string> args) {
  std::vector<size_t> used;
  for (size_t i = 1; i < args.size(); i++) {
    std::string_view arg = args[i];
    if (arg == "--") {
      break;  // the rest is for whoever reads it after us
    }
    if (!arg.starts_with("--")) {
      continue;
    }
    std::string_view body = arg.substr(2);
    size_t equals = body.find('=');
    std::string_view name = body.substr(0, equals);
    std::optional<std::string_view> value;
    if (equals != std::string_view::npos) {
      value = body.substr(equals + 1);
    }

    SettingBase* setting = Find(name);
    if (!setting && !value && name.starts_with("no-")) {
      // --no-name for a boolean.
      SettingBase* negated = Find(name.substr(3));
      if (negated && negated->type() == SettingType::kBool) {
        negated->SetText("false", SettingSource::kCommandLine);
        used.push_back(i);
      }
      continue;
    }
    if (!setting) {
      continue;  // ReXGlue's, or nobody's: passed on
    }
    used.push_back(i);

    if (!value) {
      const bool has_next = i + 1 < args.size() && !args[i + 1].starts_with("--");
      if (setting->type() == SettingType::kBool) {
        if (has_next && IsBoolWord(args[i + 1])) {
          value = args[++i];
          used.push_back(i);
        } else {
          value = "true";
        }
      } else if (has_next) {
        value = args[++i];
        used.push_back(i);
      } else {
        ITC_LOG_WARN("settings: --{} needs a value ({})", name, TypeName(setting->type()));
        continue;
      }
    }
    if (!setting->SetText(*value, SettingSource::kCommandLine)) {
      ITC_LOG_WARN("settings: --{}={} isn't {}; kept {}", name, *value,
                   TypeName(setting->type()), setting->ValueText());
    }
  }
  return used;
}

std::string EnvironmentName(std::string_view name) {
  std::string variable = "REX_";
  for (char c : name) {
    variable += char(std::toupper(static_cast<unsigned char>(c)));
  }
  return variable;
}

void ApplyEnvironment(const std::function<const char*(const char*)>& lookup) {
  for (SettingBase* setting : All()) {
    std::string variable = EnvironmentName(setting->name());
    const char* value = lookup ? lookup(variable.c_str()) : std::getenv(variable.c_str());
    if (!value) {
      continue;
    }
    if (!setting->SetText(value, SettingSource::kEnvironment)) {
      ITC_LOG_WARN("settings: {}={} isn't {}; kept {}", variable, value,
                   TypeName(setting->type()), setting->ValueText());
    }
  }
}

std::vector<size_t> ApplyLaunchOptions(std::span<const std::string> args) {
  std::vector<size_t> used = ApplyCommandLine(args);
  ApplyEnvironment();
  g_launch_options_applied = true;
  return used;
}

bool LaunchOptionsApplied() {
  return g_launch_options_applied;
}

}  // namespace settings
}  // namespace sbx
