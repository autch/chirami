# AGENTS.md — agent instructions for chirami

Fast, lightweight, Windows-native image viewer "chirami" (チラ見).

**All design decisions, specifications, and their rationale live in [DESIGN.md](DESIGN.md) (Japanese).**
Consult the relevant section before implementing or changing behavior, and record any new
design decision there. This file contains only working instructions: build, CI, coding
conventions, and workflow.

## Hard rules

- **Never block the UI thread.** File I/O, decoding, and encoding always run on background threads
- **No network access whatsoever** (no update checks, no telemetry; see DESIGN.md「ネットワーク」)
- **No registry for settings.** Settings go to an INI under %APPDATA% (file association is the
  one conditional exception; see DESIGN.md「ファイルの関連付け」)
- **Stay portable.** Runs from an unzipped folder, no admin rights, no features that require regsvr32
- **Every user-facing string goes into the STRINGTABLE (ja/en).** No literals in code

## Build environment

- Visual C++ (MSVC), Visual Studio 2022 or 2026
- Build system: CMake + vcpkg (manifest mode, pinned builtin-baseline)
- UI framework: WTL (Windows Template Library)
- Utilities: WIL (Windows Implementation Libraries)
- CRT: Universal CRT, statically linked (/MT) so the portable ZIP needs no VC++ redistributable
  (triplet x64-windows-static)

## CI

GitHub Actions (`.github/workflows/build.yml`) runs a release build on every push to main,
PR, and release publish, producing the `chirami-win64` artifact (chirami.exe, turbojpeg.dll,
README.md, LICENSE, licenses/libjpeg-turbo.txt). On release publish it additionally attaches
the ZIP to the release assets.

- Runner is windows-latest (VS 2026). Locate MSVC via vswhere and call vcvars64; no
  third-party setup actions
- The runner's preinstalled vcpkg is a shallow clone whose checkout may be older than the
  pinned builtin-baseline. baseline.json is read via git, but the versions database and port
  files are read from the working tree, so before configuring, `git fetch --depth 1` the
  baseline commit **and check it out** (fetch alone yields "no version database entry" for
  newer ports)
- turbojpeg.dll is built with classic-mode `vcpkg install libjpeg-turbo:x64-windows`.
  Classic mode refuses to run inside a manifest directory, so set working-directory to
  runner.temp. Ship vcpkg's generated share/libjpeg-turbo/copyright as
  licenses/libjpeg-turbo.txt

## Coding conventions

- C++20
- Windows API: Unicode (W) variants only
- Error handling: WIL THROW_IF_FAILED / RETURN_IF_FAILED patterns
- COM lifetime: wil::com_ptr
- Thread synchronization: standard library (std::thread, std::mutex, etc.)

## Workflow

- Roadmap and progress: DESIGN.md「ロードマップと進捗」. Work proceeds phase by phase,
  step by step; each step starts on the user's instruction and the next begins only after
  the user has reviewed the result
- Commit and push only when the user explicitly asks
- Keep DESIGN.md and README.md in sync with reality after each completed step or design change

## Language policy

| Target | Language |
|---|---|
| Source code (identifiers, comments) | English |
| Commit messages | English |
| README | Japanese and English |
| AGENTS.md | English |
| DESIGN.md and other design docs | Japanese |
| Communication with the user | Japanese |
