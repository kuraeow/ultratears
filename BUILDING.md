# Building from source

For developers and anyone testing changes. To just play, use a builder package from
[Releases](https://github.com/kuraeow/ultratears/releases) as the [README](README.md) says: it needs no Git, Python or
terminal.

## Quick start

```powershell
git clone https://github.com/kuraeow/ultratears.git
cd ultratears
python tools/setup.py --dump D:\path\to\dump
```

One command: it copies the source into `build\`, installs the tools this PC lacks there and builds the game. The result
is a complete game folder, the same as a player's: start `build\UltraTears Launcher.exe` (or `build\UltraTears.exe`
directly). Run the same command after changing the source: it builds again only what changed.

> [!WARNING]
> `build\` (and any folder given to `--output`) holds material made from your copy of the game: the translated code, the
> data, shaders and textures. Git ignores it. Never commit, upload or share it.

## What you need

| | |
|---|---|
| System | Windows 10/11 x64, or Linux with glibc 2.34+ (kernel 6.7+ to play) |
| To start | Git and any maintained Python 3 |
| Your dump | `exefs\` and `romfs\` of Tears of the Kingdom 1.4.3, extracted, in `dump\` of the checkout or any folder (`--dump`) |
| Disk | about 40 GB for the game folder, and the dump |

The setup installs the rest into `build\deps\`, pinned with URL, size and hash in `tools/deps.json`: Python 3.14 with
`lz4`, `capstone` and `unicorn`, Clang 23.1.3 with lld, ninja, SDL3, Opus, libvpx and zstd (MSYS2 CLANG64), and the
.NET 10 SDK to build the shader translator from this source. A PC with none of them downloads about 450 MB once (on
Linux: python-build-standalone, LLVM's release and the Linux kit instead, about 3 GB). Tools already on the PC are used
when they will do: a Python 3.14 with the packages, MSYS2 in `C:\msys64\clang64` (or `ULTRS_CLANG64`), `dotnet` on PATH.

Downloads resume where they stopped, and a host that does not answer is left for the next mirror in `tools/deps.json`.
`ULTRS_MSYS2_MIRROR` / `ULTRS_PYPI_MIRROR` put a mirror of your choice first; `ULTRS_NO_SYSTEM_TOOLS=1` ignores what
the PC has (to see what a clean PC gets).

## Options

| Command | Builds into | Takes the dump from |
|---|---|---|
| `python tools/setup.py` | `build\` | the last `--dump` for that folder, else `dump\` |
| `python tools/setup.py --dump <folder>` | `build\` | `<folder>` (remembered) |
| `python tools/setup.py --output <game folder>` | `<game folder>` | the last `--dump` for that folder, else `dump\` |

| Option | |
|---|---|
| `--platform windows \| linux \| both` | what to build on Windows (remembered); `linux` and `both` also pack the Steam Deck's tar |
| `--cpus N` | logical processors to use (default: all, at low priority) |
| `--only <step>` | one step only, after the check |
| `--full` | forget what was built and make everything again (the launcher's *Rebuild completely*) |

## What the build does

Times on an 8-core PC. Every step is skipped when its result is up to date, and a build that stopped continues where it
was.

| Step | Makes | Time |
|---|---|---|
| `check` | checks the tools and that the dump is 1.4.3 (the code modules' hashes); refuses `.nsp` / `.xci` / `.nca` | seconds |
| `romfs` | `data\romfs\romfs.bin`: the RomFS's 304,000 files in one image, its content checked against 1.4.3 | ~4 min |
| `lift` | `data\generated`: the game's code translated to C (~0.9 GB) | ~1 min |
| `compile` | `UltraTears.exe` (and `linux\ultratears`): x86-64-v3, or v2 on a CPU without AVX2 | ~15 min, seconds after small changes |
| `shaders` | `data\shaders\translated.pack`: the game's ~28,700 shaders as SPIR-V | ~4 min |
| `graphics` | `data\shaders\compiled.bin`: those shaders compiled by your graphics driver, so the game has no compile stalls | ~1–3 min |
| `textures` | `data\textures`: every ASTC texture as BC7 / BC1 (~7.5 GB) | ~8 min |
| `finish` | `ultratears.cfg` (your own settings in it are kept) | seconds |
| `deck` | `UltraTears-SteamDeck.tar`, with `--platform linux` or `both` | ~3 min |

Then the launcher is built beside the game. The game reads only its folder: `ultratears.cfg` points it at `data\`, so
the dump can go once the build is done.

## Linux and the Steam Deck

**From Windows.** `--platform linux` or `both` cross-compiles the game for Linux with the Linux kit (`deps\linux`:
files of Valve's Steam Runtime "sniper" sysroot; a source build makes it from the pinned sysroot, 1.2 GB once). The
`deck` step packs the game, its data and the AppImage of the same release into `UltraTears-SteamDeck.tar`; on the
Deck, that AppImage is the launcher and the builder. A package from Releases downloads the AppImage itself; from the
source, set `ULTRS_LINUX_PACKAGE` to a folder with `UltraTears.AppImage`.

**On Linux.** `python3 tools/setup.py --dump <folder>` builds for that machine, the same way. The game needs kernel
6.7+ (userfaultfd and `PAGEMAP_SCAN` for its write watch).

## If something goes wrong

- **A download fails:** check the connection and run the command again; it resumes. A damaged tool: delete that
  folder's `deps\` and run again (the downloaded archives in `deps\downloads` are reused if they are still there).
- **`exefs\main is not the one of ... 1.4.3`:** the dump is not 1.4.3; extract it again with update 1.4.3 applied.
- **`the RomFS is not the one of ... 1.4.3`:** the message names the folders that differ: update not applied, mods, or
  a damaged copy.
- **`encrypted packages are not accepted`:** give the extracted folders, not `.nsp` / `.xci` / `.nca`.
- **A black window or a Vulkan error:** update the graphics driver (Vulkan 1.3 with `VK_EXT_shader_object`).
- **A bug report:** attach `ultratears.log` from the game folder (`ultratears-setup.log` for the build). For
  performance, turn on Debug mode first (the launcher's Debug tab, or `debug=1` in `ultratears.cfg`).

## Making a release (maintainers)

A release is two files: the Windows builder package and the AppImage. Both are made from the same tagged commit, into
`release\`.

```powershell
git tag v<x.y.z>                                     # the version: tools/version.py reads it from the tag
python tools/setup.py --output <game folder> --platform both   # a complete build, measured for the packages
python tools/build_xlate.py                          # the shader translator, Windows and Linux
python tools/build_xlate.py --rid linux-x64
python tools/build_launcher.py --platform linux      # the AppImage's launcher
python tools/linux_used.py --home <game folder>      # what a Linux build reads of the Linux kit
```

Then, on Linux (under WSL on an ext4 file system: `/mnt/c` and `/mnt/d` lose the modes and links of the tools):

```bash
python3 tools/make_package_linux.py --xlate <ultrs-shaders> --launcher <ultratears-launcher>   # release/UltraTears.AppImage
```

It takes from `dev-local\release-inputs\` the Linux kit (`ultrs-linux-kit.zip`, `tools/make_linux_kit.py`),
`linux-used.txt` and the AppImage runtime with this project's patch (`runtime-x86_64`,
[tools/appimage_runtime](tools/appimage_runtime/README.md)), so `release\` holds only the two files of the release.

And back on Windows:

```powershell
python tools/make_package.py --home <game folder>    # release/UltraTears-<x.y.z>-windows-x86_64.zip
git push origin dev:main
git push origin v<x.y.z>
```

The packages carry only what a build reads: the compiler and the Linux kit are trimmed to the files the game's,
the texture encoder's and the launcher's builds open (measured from ninja's records, `clang -M` and the linker's trace),
and Python to the packages the build imports. The Windows package names the AppImage with its hash in
`deps\linux-package.json`, which a Steam Deck build downloads. `main` follows the latest release, so its README matches
the package.
