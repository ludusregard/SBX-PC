// sbx - ReXGlue Recompiled Project

#include "generated/default/sbx_init.h"

#include "sbx_app.h"

#include "base/command_line.h"
#include "base/settings.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stdlib.h>
#include <windows.h>
#endif

REX_DEFINE_APP(sbx, SbxApp::Create)

// Our settings come off the command line before the SDK's entry point reads
// it (base/settings.h): ReXGlue's parser only sees its own options, as it
// may reject names it doesn't know. This runs after every static
// initializer, so all our settings are registered by then.

#if defined(_WIN32)

namespace {

std::string Utf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr,
                                 nullptr);
  std::string result(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size, nullptr,
                      nullptr);
  return result;
}

// Cuts the arguments at `positions` out of a process-wide command line
// string, in place (the result is never longer).
template <typename Char>
void RemoveInPlace(Char* line, const std::vector<size_t>& positions, size_t expected_count) {
  if (!line) {
    return;
  }
  std::basic_string_view<Char> view(line);
  auto tokens = sbx::command_line::Split(view);
  if (tokens.size() != expected_count) {
    return;  // split differently (unusual characters): this copy is left alone
  }
  std::basic_string<Char> filtered = sbx::command_line::Remove(view, tokens, positions);
  std::copy(filtered.begin(), filtered.end(), line);
  line[filtered.size()] = Char(0);
}

// Takes the arguments at `positions` out of one of the C runtime's argv
// arrays.
template <typename Char>
void RemoveFromArray(Char** argv, int argc, const std::vector<size_t>& positions) {
  if (!argv) {
    return;
  }
  int kept = 0;
  size_t next = 0;
  for (int i = 0; i < argc; i++) {
    if (next < positions.size() && positions[next] == size_t(i)) {
      next++;
      continue;
    }
    argv[kept++] = argv[i];
  }
  argv[kept] = nullptr;
}

// The SDK's entry point may split GetCommandLineW() itself or use the C
// runtime's argv arrays, so every copy is filtered: the wide and ANSI
// command line strings, and __argv and __wargv.
void __cdecl FilterLaunchOptions() {
  wchar_t* line = GetCommandLineW();
  auto tokens = sbx::command_line::Split(std::wstring_view(line));
  std::vector<std::string> args;
  args.reserve(tokens.size());
  for (const auto& token : tokens) {
    args.push_back(Utf8(token.text));
  }
  std::vector<size_t> used = sbx::settings::ApplyLaunchOptions(args);
  if (used.empty()) {
    return;
  }
  RemoveInPlace(line, used, tokens.size());
  RemoveInPlace(GetCommandLineA(), used, tokens.size());
  int* argc = __p___argc();
  if (argc && size_t(*argc) == tokens.size()) {
    RemoveFromArray(*__p___argv(), *argc, used);
    RemoveFromArray(*__p___wargv(), *argc, used);
    *argc -= int(used.size());
  }
}

}  // namespace

// Called by the C runtime after the user initializers (.CRT$XCU, where our
// settings register) and before WinMain.
#pragma section(".CRT$XCY", long, read)
extern "C" __declspec(allocate(".CRT$XCY")) void(__cdecl* const sbx_filter_launch_options)() =
    FilterLaunchOptions;
#pragma comment(linker, "/include:sbx_filter_launch_options")

#else

// Linux: the link wraps main (-Wl,--wrap=main in CMakeLists.txt), so the C
// runtime calls this after the static initializers, and this calls the
// SDK's main with the arguments that are left.
extern "C" int __real_main(int argc, char** argv, char** envp);

extern "C" int __wrap_main(int argc, char** argv, char** envp) {
  std::vector<std::string> args(argv, argv + argc);
  std::vector<size_t> used = sbx::settings::ApplyLaunchOptions(args);
  std::vector<char*> kept;
  kept.reserve(size_t(argc) + 1);
  size_t next = 0;
  for (int i = 0; i < argc; i++) {
    if (next < used.size() && used[next] == size_t(i)) {
      next++;
      continue;
    }
    kept.push_back(argv[i]);
  }
  kept.push_back(nullptr);
  return __real_main(int(kept.size() - 1), kept.data(), envp);
}

#endif
