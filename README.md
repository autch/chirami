# chirami

高速・軽量な Windows ネイティブ画像ビューア / A fast and lightweight native image viewer for Windows.

## 概要 / Overview

**日本語:**
chirami は Windows 11 x64 以降をターゲットとした画像ビューアです。名前は「チラ見」に由来します。WIC + Direct2D による高速表示、ZIP 展開だけで使えるポータブル性、管理者権限不要、そして商用利用に制限のない MIT ライセンスを特徴とします。インターネットアクセスは一切行いません。

**English:**
chirami is an image viewer targeting Windows 11 x64 and later. The name comes from the Japanese word *chira-mi* (チラ見), "a quick glance". It features fast rendering via WIC + Direct2D, portable deployment (just unzip and run), no administrator privileges required, and the MIT license with no restrictions on commercial use. It never accesses the internet.

## ビルド / Building

要件 / Requirements:

- Visual Studio 2022 or later (MSVC, C++20)
- CMake 3.30+ / Ninja (bundled with Visual Studio)
- [vcpkg](https://github.com/microsoft/vcpkg) with the `VCPKG_ROOT` environment variable set

“x64 Native Tools Command Prompt” など、vcvars64 を通した環境で / From a vcvars64 environment such as the "x64 Native Tools Command Prompt":

```
cmake --preset release
cmake --build --preset release
```

生成物 / Output: `build/release/chirami.exe`

## ライセンス / License

MIT License
