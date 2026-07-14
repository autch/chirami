# chirami

[![build](https://github.com/autch/chirami/actions/workflows/build.yml/badge.svg)](https://github.com/autch/chirami/actions/workflows/build.yml)

高速・軽量な Windows ネイティブ画像ビューア / A fast and lightweight native image viewer for Windows.

## 概要 / Overview

**日本語:**
chirami は Windows 11 x64 以降をターゲットとした画像ビューアです。名前は「チラ見」に由来します。WIC + Direct2D による高速表示、ZIP 展開だけで使えるポータブル性、管理者権限不要、そして商用利用に制限のない MIT ライセンスを特徴とします。インターネットアクセスは一切行いません。

**English:**
chirami is an image viewer targeting Windows 11 x64 and later. The name comes from the Japanese word *chira-mi* (チラ見), "a quick glance". It features fast rendering via WIC + Direct2D, portable deployment (just unzip and run), no administrator privileges required, and the MIT license with no restrictions on commercial use. It never accesses the internet.

現在活発に開発中です。基本的なビューア機能（Phase 1）が動作します。 / Under active development; the core viewer functionality (Phase 1) is working.

## 機能 / Features

- Windows 標準の WIC コーデックでデコード（JPEG, PNG, BMP, GIF, TIFF, ICO, WebP, AVIF など。OS に入っているコーデックを自動検出） / Decodes through the OS-provided WIC codecs (JPEG, PNG, BMP, GIF, TIFF, ICO, WebP, AVIF, ...; installed codecs are detected automatically)
- ファイル I/O とデコードは常にバックグラウンドで行い、UI は固まらない（遅い SMB / OneDrive でも操作可能） / File I/O and decoding always run in the background; the UI never freezes, even on slow SMB shares or OneDrive
- フィット・等倍・自由ズーム、ドラッグでのパン、必要な軸のみのスクロールバー / Fit, actual-size, and free zoom with drag panning and per-axis scrollbars
- 同一フォルダ内をエクスプローラーと同じ自然順で前後移動、ドラッグ＆ドロップで開く / Flips through the folder in Explorer-like natural order; open files by drag & drop
- 画像を開くたびにウィンドウサイズを画像に合わせて自動調整 / The window automatically resizes to wrap each image
- フルスクリーン表示 / Fullscreen mode
- Per-Monitor V2 の DPI 対応（等倍表示は表示スケール設定に依らず dot-by-dot） / Per-Monitor V2 DPI awareness (actual size is true dot-by-dot regardless of display scaling)
- 日本語 / 英語 UI（OS の言語設定に追従） / Japanese and English UI following the OS language

## 操作 / Controls

| 操作 / Input | 動作 / Action |
|---|---|
| ← → | フォルダ内の前後のファイルへ（端で警告音、続けて押すと反対端へ） / previous / next file in the folder (beeps at the ends, wraps on the next press) |
| 0 | フィット表示 / fit to window |
| 1 | 等倍表示 / actual size (dot-by-dot) |
| + − | 拡大・縮小 / zoom in / out |
| Ctrl + ホイール / wheel | カーソル位置を中心にズーム / zoom anchored at the cursor |
| ホイール / wheel | 縦スクロール / vertical scroll |
| 左ドラッグ / left drag | パン（表示がウィンドウより大きいとき） / pan (when the image overflows the window) |
| F11 | フルスクリーン切り替え / toggle fullscreen |
| Esc | フルスクリーン解除 / leave fullscreen |
| ドラッグ＆ドロップ / drop a file | そのファイルを開く / open the dropped file |

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

CRT は静的リンクのため、VC++ 再頒布可能パッケージのインストールは不要です。 / The CRT is statically linked; no VC++ redistributable is required to run the binary.

main への push ごとに GitHub Actions が release ビルドを行い、`chirami-win64` アーティファクトとして exe を生成します。 / Every push to main is built by GitHub Actions, producing the exe as the `chirami-win64` artifact.

## ライセンス / License

MIT License
