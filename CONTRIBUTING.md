# Contributing

Pull requests and issues are welcome. Below is the checklist every pull request is reviewed against: a PR that already
answers these points is merged sooner. In short: open it against `dev`, one topic per PR, nothing from the game (code,
assets, keys, firmware) and no code from the NintendoSDK, and say how it affects speed and image quality.

UltraTears runs code on players' machines and builds from their own copy of the game, so a review looks for three
things at once: harm (malware, leaked game material), bugs, and regressions in speed or image quality.

## 1. Triage

- The base branch is `dev`. `main` holds releases only; a PR against `main` is moved to `dev`.
- One topic per PR. A PR that mixes unrelated changes is split before review.
- Close spam, empty changes and drive-by edits without a purpose.
- Label it by area (`tools`, `ci`, `render`, `runtime`, `fs`, `audio`, `platform`, `docs`) and by risk (below).

## 2. Risk by path

| Risk | Paths | Why |
|---|---|---|
| Highest | `tools/setup.py`, `tools/make_package.py` (and the `setup.bat` it writes into the package), `tools/toolchain.py`, anything that downloads, unpacks or runs programs | It runs on every builder's PC and installs the tools they then execute |
| Highest | `.github/` | Workflows run with repository tokens |
| High | `.gitignore`, `third_party/` | Can let game files into git; large vendored diffs hide changes |
| High | `src/runtime/`, `src/host/`, memory, threads, file system | Crashes, data corruption, saves |
| Performance | `src/gfx/`, `src/vk/`, the lifter and code generation in `tools/` | Frame time, stutter, image quality |
| Low | `README*`, `BUILDING.md`, comments | Still check that instructions are correct and point nowhere unexpected |

## 3. Read the diff before running anything

Look first at which files changed, then at the code. Stop and ask on any of these:

- Network access: `urllib`, `requests`, `curl`, `Invoke-WebRequest`, sockets, new URLs or hosts, a changed release or
  download address.
- Running programs: `subprocess`, `os.system`, `eval`, `exec`, `ShellExecute`, `CreateProcess`, `system()`, `popen`.
- Encoded or obscured content: base64, hex blobs, compressed strings, minified code, unusual Unicode (homoglyphs,
  bidirectional control characters).
- Files read or written outside the checkout, the output folder or the dump: home directories, browser or credential
  stores, other personal files.
- Binary files, generated files, or large reformatting that hides a small real change.
- `.gitignore` entries removed for `game/`, `data/`, `dist/`, `build/`, `userdata/` or game file types.
- Workflows using `pull_request_target` together with a checkout of the PR's code, new secrets, wider `permissions`,
  actions not pinned to a full commit hash.
- Downloaded tools without a fixed SHA-256 check.

## 4. Legal

- Nothing from the game in the repository: no translated or decompiled game code, disassembly listings, assets,
  shaders, textures, fonts, keys or firmware. Only the necessary facts: addresses, hashes, short instruction patches.
- No NintendoSDK code or headers, leaked or not. NVN and Horizon are our own implementations written from observed
  behaviour.
- New code from elsewhere must have a GPL-3.0-compatible license (MIT, BSD, 0BSD, zlib, Apache-2.0,
  GPL-2.0-or-later, GPL-3.0), keep its license and authorship, and its origin must be stated in the PR.
- DLSS and XeSS only as a separate module the player adds; never compiled in.
- The PR is contributed under GPL-3.0-or-later.

## 5. Correctness

- The PR says what was broken and how the change was checked (log line, crash address, scene, platform).
- Fix the cause, not the symptom. Be suspicious of changes that hide failures instead of fixing them: zero-filled
  fallback files, swallowed errors, "defensive" allocator or memory fallbacks, catch-all retries. If a fallback is
  needed, it must log loudly and be justified in the PR.
- No silent behaviour changes for players: config keys, file layout, save data stay compatible, or the PR says how
  they migrate.
- Windows (the main target) must keep working when a PR adds Linux or Steam Deck support, and the other way round.
- Threads: shared state is locked or atomic; no new waits on the render or audio thread.
- Memory: sizes and bounds come from the data, not guesses; guest memory rules (base, guard pages, TLS) stay
  consistent with the rest of the runtime.

## 6. Performance and image quality

The project's goal is maximum frame rate without visible quality loss, so a change that costs speed needs a reason.

- Anything on a hot path (per frame, per draw, per memory access, per guest call) is measured before and after: the
  same save and scene, `TOTK_PROF`, average frame time, worst 1 % frame time, stutter count. On PC, and on Steam Deck
  for changes that matter there.
- No per-access checks, logging, allocations or locks added to hot paths without numbers.
- No shader, pipeline or texture work moved from build time to run time.
- Image quality: compare screenshots of the same scene; no new artefacts, banding, missing effects or lower
  resolution.

## 7. Build and test in isolation

Only after steps 3 and 4 pass:

- A separate clean clone with no personal files in or next to it; preferably a sandbox or virtual machine, offline
  once the tools are in place.
- `git fetch origin pull/<n>/head:pr-<n>`, then the normal build (`python tools/setup.py`) and a play test of the
  areas the PR touches.
- A dump is given to the test build only for PRs that passed the review of their code.

## 8. Merge

- Squash into `dev` with a clear commit message in English; keep the author's credit.
- Small fixes found in review may be made by the maintainer instead of another round with the author.
- Changes reach `main` only with a release.
