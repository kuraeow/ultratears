<p align="center"><img src="assets/cover.png" alt="UltraTears" width="100%"></p>

# UltraTears

**UltraTears** is an unofficial builder: on your PC it turns your own copy of a Nintendo Switch game into a native
Windows program. It works with one game, *Tears of the Kingdom* version 1.4.3. Steam Deck and Linux support is planned
and is a high priority. It is not an emulator: the program code of your copy is translated ahead of time (static
recompilation, ARM64 → C → x86-64), and the Switch system and its NVN graphics API are replaced by the port's own
implementations on Windows and Vulkan. Shaders and textures are converted for PC graphics cards when the game is built,
not while you play.

> **This repository contains no game code, no game assets, no Nintendo SDK code, no firmware and no keys, and it does
> not tell you where to get any of them.** It does nothing without a copy of the game you dumped yourself from your own
> console. Everything made from the game (the translated code, the compiled game, shaders, textures) is made on your
> PC from your files and must stay there: do not share it, and do not open issues asking for it.
>
> UltraTears is not affiliated with or endorsed by Nintendo. *The Legend of Zelda* and *Tears of the Kingdom* are
> trademarks of Nintendo; the name is used only to say which game this works with.

Русская версия: [README.ru.md](README.ru.md).

## Status

Early. Measured on the developer's PC (Windows 10, Ryzen, 16 threads, GeForce RTX 3060):

| What | Result |
|---|---|
| Title screen, prologue, Great Sky Island, Hyrule, the Depths | plays |
| Frame rate | the game's own 30 fps; all shaders are compiled ahead (in the background on the first start, ~3 min), so they cause no hitches |
| Steam Deck / Linux | not supported yet; a native version is planned and is a high priority |
| AMD graphics cards | a likely fix for the black screen on Windows is in 0.2.0, not yet confirmed on real AMD hardware |
| Other PCs and graphics cards | a few player reports so far |

Known issues: in the Depths the ground is broken or black in places; some cloth and banners tear (for example at a
tower); the controller layout follows the Switch (A is the right face button); no settings menu yet (window size,
resolution, upscaling and 60 fps come later).

## Choose how to build first

There is no downloadable game-filled `UltraTears.exe`. You download a builder, and it translates and compiles the game
on your PC from your own extracted dump. There are two ways to do that:

| Method | Who it is for | What you install |
|---|---|---|
| **Builder package from Releases** | Almost every player. This is the recommended and easiest method | Nothing |
| **Current GitHub source** | Developers and people testing their own changes | Git and any maintained Python 3; the remaining tools download automatically |

Both methods produce the same playable folder. The Releases package already carries every build tool. The source build
uses the tools already on your PC and downloads only the missing ones from where they are published (Python from
python.org, Clang from MSYS2, .NET from Microsoft to build the shader translator from the same source), then performs
the same game build.

## Requirements for either method

- Windows 10/11 x64 and a processor with AVX2 (Intel Haswell / AMD Ryzen or newer). Older processors with SSE4.2
  (Intel Core 2nd and 3rd generation, AMD FX) work too: the builder detects the CPU and builds the game for it,
  somewhat slower.
- A graphics card with a current Vulkan 1.3 driver and `VK_EXT_shader_object`. What counts is what the driver can do,
  not the model: the build checks the card first and asks whether to go on if it will not do.
  - **Tested:** NVIDIA RTX 3060.
  - **Target:** AMD RX 6000 (RDNA 2) or newer; work on AMD is in progress.
  - **May work, not tested:** older NVIDIA (GTX 900 / 10 / 16 series), AMD RX 5000 (RDNA 1), Intel Arc.
  - **Not supported:** AMD RX 400/500 (Polaris) and Vega, Intel HD 500/600 — their Windows drivers have no
    `VK_EXT_shader_object`.
- At least **16 GB of RAM** and a graphics card with at least **4 GB of video memory**. A PC with 8 GB of RAM, or a
  GPU that shares the system memory (integrated graphics), is not supported for now.
- At least **40 GB free** on the drive of the game folder (an SSD is recommended): the build takes about 32 GB, and the
  texture cache grows while you play. The original game dump needs about 16 GB more; the dump in the working folder is
  no longer needed after a successful build.
- **Your own extracted copy of Tears of the Kingdom version 1.4.3** as two ordinary folders:

  ```text
  game\
  ├── exefs\    main, subsdk0, sdk, main.npdm, ...
  └── romfs\    Actor\, Pack\, Shader\, ... (about 304,000 files, 16 GB)
  ```

  You obtain these folders yourself from your own console and copy of the game with update 1.4.3 installed. This project
  does not provide or point to extraction tools. `.nsp`, `.xci`, `.nca` and other encrypted packages do not work:
  UltraTears decrypts nothing and reads no keys.

## Method 1 — builder package from Releases (recommended)

### 1. Download and extract the builder

Download this file from the [**Releases**](https://github.com/kuraeow/ultratears/releases) page:

```text
UltraTears-builder-<version>-windows-x86_64.zip
```

Extract the **entire archive** into its own folder, for example:

```text
D:\Games\UltraTears
```

The game is built and installed in this folder, so choose a drive with at least 40 GB free. Do not run files from
inside the ZIP.

### 2. Start UltraTears Launcher

Double-click `UltraTears Launcher.exe` in that folder.

> **Windows SmartScreen or an antivirus may warn about it.** The launcher is free open-source software without a paid
> code-signing certificate, so Windows does not know its publisher yet. In the blue "Windows protected your PC" window
> click **More info → Run anyway**. If an antivirus quarantines the launcher or files in `deps`, restore them and add
> the folder to its exclusions: the builder compiles programs on your PC, which some antiviruses find suspicious. The
> launcher's source is in `src/launcher`, and the Releases page shows the SHA-256 of every file.

### 3. Choose the game folder and prepare the game

In the launcher, choose the folder that contains `exefs` and `romfs`, or drag it onto the window. It can be anywhere:
nothing is copied into it, and the launcher also finds the dump one folder above or below. The launcher checks the
version and your PC; then click **Prepare the game**:

| Step in the launcher | What happens | Approximate time on 8 cores |
|---|---|---|
| Hardware check | The graphics card, memory and disk space; the exact game version 1.4.3 | seconds |
| Copying the game data | 304,000 files are packed into `data\romfs\romfs.bin` | ~4 min |
| Translating the game code | Game code from the ExeFS is translated to C | ~2 min |
| Building the game | The game code and port are compiled into `UltraTears.exe` | ~15 min |
| Preparing the graphics | The shaders are translated for Vulkan | ~3 min |
| Compressing textures | The textures are converted to BC7/BC1 | ~7 min |

Times depend on the CPU, disk and antivirus. The window may be minimized. If the PC shuts down or the launcher is
closed, start it again and click **Continue preparing**: completed work is reused. If preparing stops with an error,
**Open the log folder** shows `ultratears-setup.log`; attach it to an issue.

### 4. Play

Click **Play** in the launcher, or run `UltraTears.exe` in the same folder. Do not move the EXE by itself: it needs
`ultratears.cfg` and the `data` folder beside it. Saves are stored in `userdata`; keep that folder when moving the
game.

## Method 2 — build the current GitHub source

Use this method when changing the code or building the newest commit. Install Git and Python 3, open PowerShell, then
run:

```powershell
git clone https://github.com/kuraeow/ultratears.git
cd ultratears
```

Put `exefs` and `romfs` in `ultratears\game` as shown above, then run:

```powershell
python tools/setup.py
```

This **one command performs the entire process**:

1. Creates `dist` inside the repository.
2. Copies the current tracked source files there.
3. Installs the build tools this PC lacks into `dist\deps` (Python 3.14, Clang, and the shader translator built from
   this source); nothing comes from a release.
4. Finds the dump: first in the output folder's `game`, then in the repository's `game`, then the folder given with
   `--dump` last time for this output folder.
5. Builds the complete game into `dist`.

If the selected output folder already has its own populated `game`, that copy takes priority. `--dump` always selects
the dump explicitly and removes any ambiguity.

The executable appears here:

```text
ultratears\dist\UltraTears.exe
```

To build directly into another folder:

```powershell
python tools/setup.py --output D:\Games\UltraTears
```

The original dump remains in `ultratears\game`, while the completed data is written to
`D:\Games\UltraTears\data`. If the dump is elsewhere, name it explicitly:

```powershell
python tools/setup.py --output D:\Games\UltraTears --dump D:\Dumps\TOTK-1.4.3
```

Run the same command again after changing the source: unchanged data and object files are reused. More options and
implementation details are in [BUILDING.md](BUILDING.md).

## What the builder does with the dump

The original `exefs` and `romfs` are never run directly or moved. The builder reads them and creates a PC-ready copy
inside the output folder:

```text
game\exefs  ──→ data\exefs + data\generated ──→ UltraTears.exe
game\romfs  ──→ data\romfs\romfs.bin
             ├→ data\shaders
             └→ data\textures
```

After the `Done` message, the game reads only `data` beside `UltraTears.exe`. You may remove the working copies of
`game\exefs` and `game\romfs` if the original dump is backed up safely elsewhere. Never delete `data` from the built
game.

The finished folder looks roughly like this:

```text
UltraTears\
├── UltraTears.exe      ← run this file
├── ultratears.cfg      ← paths to the data
├── data\               ← built game data; do not delete
├── userdata\           ← saves
├── build\              ← intermediate files for fast rebuilds
└── deps\               ← build tools
```

## Controls and updates

Keyboard: WASD moves, arrow keys move the camera, J = A, K / Space = B, I = X, U = Y, Q / E = L / R, left Alt /
Shift = ZL / ZR, Enter = +, Tab / Esc = − (map), T F G H = d-pad, Z / C = stick buttons, and F11 or Alt+Enter toggles
full screen. Xbox, DualSense, Switch Pro and other controllers work through SDL3. The face-button layout follows the
Switch: A is the right button.

A Releases build updates itself: once a day the launcher asks GitHub Releases for a newer version and offers it.
**Update** downloads the new builder, checks its size and SHA-256, puts it in place of the old one (`data`, `build`
and `userdata` stay) and rebuilds only what changed. `updates=off` in `ultratears-launcher.ini` turns the check off.
From 0.2.0 or older (no launcher yet), extract the new package over the old folder and start the launcher. For a Git build, run `git pull`, then repeat the same `python tools/setup.py` command and
options.

> **Steam Deck and Linux are not supported yet.** Through Proton the Windows build starts, but runs at a few frames
> per second and closes in Gaming Mode. A native Linux and Steam Deck version is planned and is one of the project's
> high-priority tasks.

## If the build stops with an error

- The `python` command is not found — install Python 3 from
  [python.org](https://www.python.org/downloads/windows/) and enable `Add python.exe to PATH`, or try
  `py tools/setup.py`.
- `exefs\main is not the one ... 1.4.3` — the ExeFS is not from version 1.4.3.
- `the RomFS is not the one ... 1.4.3` — the RomFS lacks the required update, is damaged or contains mods.
- `encrypted packages are not accepted` — `.nsp`, `.xci` or `.nca` was supplied instead of ordinary `exefs` and
  `romfs` folders.
- A Vulkan error or black window — update the graphics driver; Vulkan 1.3 with `VK_EXT_shader_object` is required.
- After correcting the cause, start the launcher again (Releases) or run the same command (Git). You do not need to
  repeat successful steps manually.

## Legal

- Nothing of the game is in this repository or in a release: no code (also none translated from the game), no assets,
  shaders, textures, fonts, keys or firmware. Everything derived from the game is made on the player's PC from the
  player's own dump and stays there.
- UltraTears decrypts nothing and reads no keys: it takes the ExeFS and RomFS the player has already extracted.
- No Nintendo SDK code or headers are used; the system and graphics layers are the port's own implementations.
- Only the facts needed to run are in the source: addresses, hashes, short instruction replacements.
- The icon, emblem and cover are the author's own drawings, not Nintendo artwork.
- UltraTears is not affiliated with or endorsed by Nintendo; you need your own purchased copy of the game.

## License

UltraTears is free software under the **GNU General Public License v3.0 or later** ([LICENSE](LICENSE)). Third-party
code keeps its own license: [third_party/README.md](third_party/README.md); the builder package adds
`THIRD-PARTY-NOTICES.md` and `licenses\` for the tools it carries.

The **branding is not under the GPL**: the name UltraTears, the emblem and icon, the cover and the launcher's background
pattern may be shared unchanged as part of UltraTears and used to refer to it, but not for other projects or modified
versions. A fork keeps the code under the GPL with its own name and images. Details: [assets/BRANDING.md](assets/BRANDING.md).
