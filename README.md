<p align="center"><b>English</b> · <a href="README.ru.md">Русский</a></p>

<p align="center"><img src="assets/cover.png" alt="UltraTears" width="100%"></p>

# UltraTears

**UltraTears** builds a native version of *The Legend of Zelda: Tears of the Kingdom* (version 1.4.3) for Windows, Linux
and the Steam Deck from your own copy of the game. It is not an emulator: the game's code is translated to native code
ahead of time, and its shaders and textures are prepared when the game is built, not while you play.

> [!IMPORTANT]
> **No game files are included.** You need your own copy of the game with update 1.4.3, dumped from your own console as
> plain `exefs` and `romfs` folders. This project provides no game files, keys, firmware or dumping tools, and
> UltraTears decrypts nothing. Everything made from the game is made on your PC, for your own use: do not share it.
>
> UltraTears is not affiliated with or endorsed by Nintendo. *The Legend of Zelda* and *Tears of the Kingdom* are
> trademarks of Nintendo.

Report problems on [Issues](https://github.com/kuraeow/ultratears/issues) with `ultratears.log` from the game folder.

## Installing

**You need:** Windows 10/11 or Linux (kernel 6.7+, glibc 2.34+; SteamOS 3 has both), a graphics card with a current
Vulkan 1.3 driver, 16 GB of RAM, 4 GB of video memory, about 40 GB free, and your dump:

```text
dump\
├── exefs\   main, subsdk0, sdk, main.npdm, ...
└── romfs\   about 304,000 files, 16 GB
```

### Windows

1. Download `UltraTears-<version>-windows-x86_64.zip` from [Releases](https://github.com/kuraeow/ultratears/releases)
   and extract it into its own folder.
2. Start `UltraTears Launcher.exe`. If SmartScreen warns about it, choose **More info → Run anyway**.
3. Choose the folder with `exefs` and `romfs` and click **Prepare the game**. It builds the game once, about 30 minutes.
4. Click **Play**.

To play on a Steam Deck, choose **For Linux (Steam Deck)** or **Both platforms** next to **Prepare the game**: you get
`UltraTears-SteamDeck.tar` (about 28 GB), which you extract on the Deck and start as below, from step 4.

### Linux and Steam Deck

1. Download `UltraTears.AppImage` from [Releases](https://github.com/kuraeow/ultratears/releases). On a
   Steam Deck, do this in Desktop Mode.
2. In the file's Properties, on the **Permissions** tab, turn on **Allow executing file as program**, then start it.
3. Choose the folder with `exefs` and `romfs` and click **Prepare the game**.
4. When the game is prepared, add `UltraTears.AppImage` from the game folder to Steam as a non-Steam game. With `--play`
   in its launch options it starts the game directly, without the launcher. Start the game from Gaming Mode.

The launcher finds new versions itself and rebuilds only what changed; saves in `userdata` are kept. On Linux you can
also put a newer `UltraTears.AppImage` into the game folder in place of the old one: the launcher then offers the update.

<details>
<summary>Controls</summary>

The controller's button layout follows the Switch (A is the right button); on the Steam Deck it follows the Deck's own
labels (A is the bottom button).

Keyboard and mouse: WASD move, the mouse (or the arrow keys) turns the camera and aims, E = A, Left Shift = B,
Space = X, left click / T = Y, Q = L, R = R, right click / Z = ZL, F / X = ZR, Esc = + (menu), Tab / M = − (map),
Left Ctrl / C = stick buttons, 1 2 3 4 = d-pad (left, up, right, down; the mouse's side buttons are left and right),
F11 or Alt+Enter full screen. Holding L, the mouse picks an ability; holding a d-pad key, the wheel or moving the mouse
sideways steps through weapons, shields and materials. Mouse sensitivity, the bow's and inverted up and down are on the
**Keyboard** page of the launcher's **Controls** tab.

Both can be changed in the launcher's **Controls** tab: every button and key (mouse buttons too), and the controller's
layout (Nintendo: A is the right button; Xbox / Steam Deck: A is the bottom one). The game's button prompts follow
your layout and bindings. The **Sticks and gyro** page sets each stick's and the gyro's dead zone, for a stick that
drifts.
</details>

<details>
<summary>Upscaling and anti-aliasing</summary>

On the launcher's **General** tab:

- **Scene scale:** the scene can be drawn below the screen's resolution and upscaled with **FSR 1** (the game's own) or
  **FSR 3.1** (temporal, sharper and steadier in motion), or drawn at full size (**None**).
- **Anti-aliasing** (without FSR 3.1, which anti-aliases by itself): the game's **FXAA**, **SMAA** (smoother edges for
  little GPU time) or **Off**.
</details>

<details>
<summary>Mods and cheats</summary>

Mods made for the Switch emulators work as they are. In the launcher's **Mods** tab click **Open mods folder** and
unpack each mod into a folder of its own there, so that its `romfs`, `exefs` or `cheats` folder sits directly in it
(`atmosphere/contents/0100F2C0115B6000/...` works too). The same tab turns each mod and each cheat on or off and sets
the mods' order: when two mods change the same file, the one higher in the list wins. Mods and cheats need no rebuild
of the game.

- RomFS mods (textures, models, text, ...), ExeFS patches (`.ips`, `.pchtxt`) and Atmosphere cheats (`.txt` named by
  the game's build ID) for game version 1.4.3.
- Patches and cheats that change the game's code are prepared when you press **Play**, a few seconds when they
  changed. On Linux and the Steam Deck this needs the game folder set up by the AppImage; in a folder made on Windows
  for the Deck, only RomFS mods and cheats that change data work.
- A texture from a mod causes a short hitch the first time it is seen.
</details>

<details>
<summary>Chinese and Korean text</summary>

For Simplified Chinese, Traditional Chinese and Korean the game uses the Switch's own system fonts. They are part of
the console's system software, not of the game, so the port can't make them from the dump. Without them the game stops
when it starts in one of these languages; all other languages don't need them.

Put the console's shared font files, taken from your own Switch, into `sysdata\fonts\` next to `exefs` and `romfs` in
your dump folder before **Prepare the game** (or later straight into `data\fonts\` in the game folder):

```text
dump\
└── sysdata\fonts\   0-standard.bfttf, 1-chinese-simplified.bfttf, 2-chinese-simplified-extension.bfttf,
                     3-chinese-traditional.bfttf, 4-korean.bfttf, 5-nintendo-extension.bfttf
```

Put in all six; the game reads only the ones it asks for.
</details>

## Building from source

Install Git and Python 3, then:

```powershell
git clone https://github.com/kuraeow/ultratears.git
cd ultratears
python tools/setup.py --dump D:\path\to\dump
```

The setup downloads the tools it lacks and makes the complete game folder in `build\`, launcher included, as in a
release. Start `build\UltraTears Launcher.exe`. Without `--dump` it takes the dump from `dump\` in the repository. Options, Linux and how it works: [BUILDING.md](BUILDING.md).
Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

- Nothing of the game is in this repository or in a release: no code (also none translated from it), assets, shaders,
  textures, fonts, keys or firmware. All of that is made from your dump, on your PC.
- No Nintendo SDK code or headers are used: the system and graphics layers are UltraTears' own.
- The source holds only the facts needed to run the game: addresses, hashes, short instruction replacements.
- The name, emblem, icon and cover are the author's own work and are not under the GPL:
  [assets/BRANDING.md](assets/BRANDING.md). Third-party code: [third_party/README.md](third_party/README.md).
