# Shutokō Battle X PC

A PC port of *Shutokō Battle X* (Xbox 360, 2006) for Windows and
Linux. It has ReXGlue roots [ReXGlue](https://github.com/rexglue/rexglue-sdk),
but the game is being rewritten subsystem by substem for modding and extendabillity.
[ITC-Recomp](https://github.com/Spooky098529/ITC-Recomp), the recompilation
of the game's US release, *Import Tuner Challenge* has been pulled at times to helped
with progress.

This project was built with assistance of Claude with manual review and testing done by myself.
Much of the research into the game was being done by myself and the community prior to this project.

<img width="3840" height="2160" alt="image" src="https://github.com/user-attachments/assets/f149178a-bdee-4b87-b07a-75e82796e35c" />


## Features

- **Fix for the black screen after quest races** (on by default;
  `--post_race_check_cycles=60` puts back the game's wait).
- **Unlocked Frame Rate**: game logic is 30hz still but rendering is unlocked.
- **Unlocked Resolution**: Play in 4k! (aspect ratio is still forced 16:9)
- **HD Menu Text** (more menu redraws in the works)
- **Additional Anti-Aliasing**: toggle options for CMAA2 and FXAA
- **Patch Loader**: game archive files are replaced by loose files in
  `assets/PATCH_DATA/` (on by default; `--patch_loader=false` turns it off).
- **Custom Music Support**: add your own music to the game mix
- **Shutokō Battle X Restored** archive files, loaded through the patch
  loader from a submodule in `assets/PATCH_DATA/BUILD/TXR/BUILD/`.
- **Cut Cars**: the ECLI, S30, L86 and T86 are offered from the start of a
  new game (`--unlock_cut_cars=false` turns it off), and come on their own
  stock wheels instead of AD00 rims: the ECLI its own, the L86, S30 and T86
  the LORRY's steel wheels (`--cut_car_steelies=false` gives them the
  BOSS00's wheels; `--cut_car_wheels=false` turns it all off).

## Requirements

- Windows 10 or 11 on a 64-bit x86 CPU with SSE4.2.
- Your own copy of the game, extracted from a backup.

## Install

Build it from source (below), then run `py sbx_pc\build_vulkan_release.py`
to package it in `sbx_pc\release\`: a Windows `.zip`, a Linux `.tar.gz`
(when the Linux build exists) and an assets `.zip`. The packages hold the
program only, with no game files. The game's files go in
`assets/` (`default.xex` and `DATA/`); `assets/PATCH_DATA/` (replacements)
and `assets/ADDON_DATA/` (new content, not used yet) are part of this repo.

## Settings

Press F4 in the game for the settings menu. Most changes apply at once (the
ones marked apply after a restart); **Save** writes them to `sbx.toml` next to
`sbx.exe`, loaded at every start. The file can also be edited by hand while
the game is closed: one setting per line, `name = value`. Options on the
command line win over it.

## Reporting problems

Play with `run.bat --log_level=debug`, then open an issue describing what you saw, and
attach the newest file from the `logs` folder. My messages are always open so don't
hesitate to shoot me a message on Discord or any socials if you have issues/feedback.

## Building from source

You need git, clang 19 or newer (`clang`/`clang++` on `PATH`), CMake (3.25
or newer), Ninja, Python 3 and the Vulkan SDK.

```bat
python sbx_pc\tools\fix_symlink_stubs.py
cd sbx_pc
codegen.bat
build_vulkan.bat
run_vulkan.bat
```

Clone with its submodules. `fix_symlink_stubs.py` replaces the SDK's
symlink stubs with real files (git on Windows checks symlinks out as text
files). This builds `sbx_pc/out/build/win-amd64-relwithdebinfo-vulkan/sbx.exe`.

`build_vulkan.bat` then builds the Linux version in WSL
(`sbx_pc/out/build/linux-amd64-relwithdebinfo-vulkan/sbx`). That needs
WSL with Ubuntu 24.04 and, once, `./setup_linux.sh` run from `sbx_pc`
inside it. Set `SBX_NO_LINUX=1` to build Windows only. On Linux itself:

```sh
cd sbx_pc
./setup_linux.sh      # once: clang 20, CMake, Ninja, X11/Wayland/audio headers
./build_vulkan.sh
./run_vulkan.sh
```

Both builds share `sbx_pc/generated/` and need the game's
`assets/default.xex`, since codegen reruns whenever its inputs change.

## Credits and links

- [Shutokō Battle X Restored](https://github.com/ludusregard/Shutokō-Battle-X-Restored):
  the English relocalization of the game.
- [GUT Archive Tools](https://github.com/igorciz777/GUTArchiveTools) by
  igorciz777: the extractor for Genki's GUT archives (`BUILD.DAT` and
  `.TOC`), used as a reference for the archive format.
- [Jake's TXR Resources and Research](https://github.com/JakeMR2/Jake-s-TXR-Resources-and-Research)
  by JakeMR2: Cheat Engine tables, ImHex patterns and notes on the earlier
  Tokyo Xtreme Racer and Kaido Battle games, which helped work out the car
  specs.
- [Shutokōu Battle Discord](https://discord.gg/n4FvF3VkHQ): the series'
  community, for support and feedback.

## Documentation (This is written by LLMs primarily for LLMs)

- [docs/functions.md](docs/functions.md): what's known about the game's
  code.
- [docs/native-code.md](docs/native-code.md): the game's code rewritten as
  our own C++, subsystem by subsystem.
- [docs/status.md](docs/status.md): what works now, fixes,
  experiments, and what's next.
- [docs/game_data.md](docs/game_data.md): what's known about the game's
  data files.
- [docs/codegen.md](docs/codegen.md): how the recompiled code is
  generated, and the Windows build.
- [docs/porting-from-itc.md](docs/porting-from-itc.md): how ITC-Recomp's
  findings are matched to this game's code.
- [docs/build_run.md](docs/build_run.md): info on building, running and testing.
- [docs/audio.md](docs/audio.md): the sound banks and the code that loads
  them.
- [docs/modding-plan.md](docs/modding-plan.md): the plan for mod support
  (the patch and addon loaders, adding cars).
- [docs/cut_car_wheels.md](docs/cut_car_wheels.md): stock wheel and tire
  baselines for the cut cars.
- [sbxr_tools](https://github.com/ludusregard/sbxr_tools): tools for
  editing the game's data files, and the Blender add-on for the game's car
  models (moved out of this repo's `mod_tools/`).
