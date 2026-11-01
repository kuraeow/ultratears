# AppImage runtime with UltraTears' patch

The AppImage's runtime (the program at the start of `UltraTears-<version>-x86_64.AppImage` that mounts the image and
starts `AppRun`) is [AppImage/type2-runtime](https://github.com/AppImage/type2-runtime) (MIT, `LICENSE` there) at tag
`20251108` (`dd6cebe`), the release totk-pc-recomp pins, with one patch: `steam-sigchld.patch`.

## Why

Started by Steam (a non-Steam game in Game Mode, or from Steam on the desktop), the released runtime prints
`Error: No suitable fusermount binary found on the $PATH` and the AppImage does not start, although `/usr/bin/fusermount3`
is there, setuid root, and the same AppImage starts from a file manager (Steam Deck, SteamOS, 2026-11-01). Its
`find_fusermount` runs each candidate with `--version` and keeps it only when `waitpid` reports exit status 0, without
checking `waitpid`'s result. The likely cause (not confirmed yet): Steam starts games with `SIGCHLD` ignored, so the kernel
reaps the child itself, `waitpid` fails with `ECHILD`, the status is never set and every candidate is rejected. Setting
`FUSERMOUNT_PROG=/usr/bin/fusermount3` (which skips the search) made it start.

The patch runs the search with `SIGCHLD` at its default, checks `waitpid`'s result, and, when no candidate passes the
check, uses a setuid root `fusermount3` (else `fusermount`) found on the way. A player then adds the AppImage to Steam and
starts it, with `--play` as the only launch option.

## Build

Its own build: `scripts/chroot/chroot_build.sh` of type2-runtime with `ALPINE_ARCH=x86_64` (an Alpine minirootfs chroot,
verified with Alpine's key, builds static libfuse 3.15.0 and squashfuse 0.5.2 by their pinned hashes, then the runtime).
It needs root (chroot, mounts) and gpg2. In this project's development setup: `dev-local/wsl/runtime.sh` in WSL, inside
the ext4 image `dev-local/wsl/stage.img`, gives `dev-local/dist/runtime-x86_64`; `tools/make_package_linux.py --runtime`
takes it (the default path).
