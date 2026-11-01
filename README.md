<p align="center"><b>English</b> · <a href="README.ru.md">Русский</a></p>

<p align="center"><img src="assets/cover.png" alt="UltraTears" width="100%"></p>

# UltraTears

**UltraTears** builds a native PC version of *The Legend of Zelda: Tears of the Kingdom* (version 1.4.3) for Windows and
Linux / Steam Deck from your own copy of the game. It is not an emulator: the game's code is translated to native code
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

1. Download `UltraTears-builder-<version>-windows-x86_64.zip` from [Releases](https://github.com/kuraeow/ultratears/releases)
   and extract it into its own folder.
2. Start `UltraTears Launcher.exe`. If SmartScreen warns about it, choose **More info → Run anyway**.
3. Choose the folder with `exefs` and `romfs` and click **Prepare the game**. It builds the game once, about 30 minutes.
4. Click **Play**.

To play on a Steam Deck, choose **For Linux (Steam Deck)** or **Both platforms** next to **Prepare the game**: you get
`UltraTears-SteamDeck.tar` (about 28 GB), which you extract on the Deck and start as below, from step 4.

### Linux and Steam Deck

1. Download `UltraTears-<version>-x86_64.AppImage` from [Releases](https://github.com/kuraeow/ultratears/releases). On a
   Steam Deck, do this in Desktop Mode.
2. In the file's Properties, on the **Permissions** tab, turn on **Allow executing file as program**, then start it.
3. Choose the folder with `exefs` and `romfs` and click **Prepare the game**.
4. When the game is prepared, add `UltraTears.AppImage` from the game folder to Steam as a non-Steam game. With `--play`
   in its launch options it starts the game directly, without the launcher. Start the game from Gaming Mode.

The launcher finds new versions itself and rebuilds only what changed; saves in `userdata` are kept.

<details>
<summary>Controls</summary>

Xbox, DualSense, Switch Pro and other controllers work; the button layout follows the Switch (A is the right button).

Keyboard: WASD move, arrow keys camera, J = A, K / Space = B, I = X, U = Y, Q / E = L / R, Alt / Shift = ZL / ZR,
Enter = +, Tab / Esc = − (map), T F G H = d-pad, Z / C = stick buttons, F11 or Alt+Enter full screen.
</details>

## Building from source

Install Git and Python 3, then:

```powershell
git clone https://github.com/kuraeow/ultratears.git
cd ultratears
python tools/setup.py --dump D:\path\to\dump
```

The setup downloads the tools it lacks and builds the game into `build\` (`build\UltraTears.exe`). Without `--dump` it
takes the dump from `dump\` in the repository. Options, Linux and how it works: [BUILDING.md](BUILDING.md).
Contributing: [CONTRIBUTING.md](CONTRIBUTING.md).

## Legal

- Nothing of the game is in this repository or in a release: no code (also none translated from it), assets, shaders,
  textures, fonts, keys or firmware. All of that is made from your dump, on your PC.
- No Nintendo SDK code or headers are used: the system and graphics layers are UltraTears' own.
- The source holds only the facts needed to run the game: addresses, hashes, short instruction replacements.
- The name, emblem, icon and cover are the author's own work and are not under the GPL:
  [assets/BRANDING.md](assets/BRANDING.md). Third-party code: [third_party/README.md](third_party/README.md).
