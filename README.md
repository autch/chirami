# chirami

[![build](https://github.com/autch/chirami/actions/workflows/build.yml/badge.svg)](https://github.com/autch/chirami/actions/workflows/build.yml)

高速・軽量な Windows ネイティブ画像ビューア / A fast and lightweight native image viewer for Windows.

## 概要 / Overview

**日本語:**
chirami は Windows 11 x64 以降をターゲットとした画像ビューアです。名前は「チラ見」に由来します。WIC + Direct2D による高速表示、ZIP 展開だけで使えるポータブル性、管理者権限不要、そして商用利用に制限のない MIT ライセンスを特徴とします。インターネットアクセスは一切行いません。

**English:**
chirami is an image viewer targeting Windows 11 x64 and later. The name comes from the Japanese word *chira-mi* (チラ見), "a quick glance". It features fast rendering via WIC + Direct2D, portable deployment (just unzip and run), no administrator privileges required, and the MIT license with no restrictions on commercial use. It never accesses the internet.

現在活発に開発中です。閲覧・編集の主要機能が動作します。 / Under active development; the main viewing and editing features work.

## 機能 / Features

- Windows 標準の WIC コーデックでデコード（JPEG, PNG, BMP, GIF, TIFF, ICO, WebP, AVIF など。OS に入っているコーデックを自動検出） / Decodes through the OS-provided WIC codecs (JPEG, PNG, BMP, GIF, TIFF, ICO, WebP, AVIF, ...; installed codecs are detected automatically)
- ファイル I/O とデコードは常にバックグラウンドで行い、UI は固まらない（遅い SMB / OneDrive でも操作可能） / File I/O and decoding always run in the background; the UI never freezes, even on slow SMB shares or OneDrive
- 前後のファイルを自動で先読みし、←→での切り替えは瞬時 / Neighboring files are prefetched automatically, so flipping with the arrow keys is instant
- GPU のテクスチャ上限を超える巨大画像（8K・16K 超）もタイル分割で表示（約 23000×23000 まで） / Images beyond the GPU texture limit (8K, 16K+) display through tiling, up to roughly 23000x23000
- アニメーション GIF / WebP の再生（フレーム遅延・部分フレーム・透過に対応） / Animated GIF / WebP playback honoring frame delays, partial frames, and transparency
- フィット・等倍・自由ズーム、ドラッグでのパン、必要な軸のみのスクロールバー / Fit, actual-size, and free zoom with drag panning and per-axis scrollbars
- 同一フォルダ内を前後移動（並び順は名前/更新日時/サイズ・昇順/降順を選択可、既定はエクスプローラーと同じ自然順。隠しファイルは対象外） / Flips through the folder (sort by name/date/size, ascending/descending; Explorer-like natural order by default; hidden files are skipped)
- ファイルでもフォルダでも開ける（D&D・関連付け・コマンドライン。フォルダは中の最初の画像を表示） / Opens files and folders alike (drag & drop, file association, command line; a folder opens to its first image)
- フォーマットを変換して保存（PNG/JPEG/BMP/TIFF）、クリップボードからの貼り付け、90 度回転・反転 / Save as PNG/JPEG/BMP/TIFF, paste from the clipboard, rotate/flip
- クロップと黒塗り（矩形選択は 8 方向ハンドルでリサイズ・ドラッグで移動と、後から調整可能） / Crop and blackout with a rubber-band selection that stays adjustable: resize via 8 handles, move by dragging
- リサイズはピクセルでも % でも指定可能（4 つの入力欄がライブ連動。縦横比維持なら片方だけの指定で残りはなりゆき） / Resize by pixels or percent through four live-linked fields; with the aspect lock, entering any one value settles the rest
- 設定は %APPDATA% の INI ファイルに保存（レジストリ不使用） / Settings live in an INI file under %APPDATA% (no registry)
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
| Ctrl + O | ファイルを開く / open a file |
| Ctrl + S | 名前を付けて保存（形式変換） / save as (format conversion) |
| Ctrl + V | クリップボードから貼り付け / paste from the clipboard |
| R / L | 右へ / 左へ 90 度回転 / rotate right / left 90° |
| H / V | 左右反転 / 上下反転 / flip horizontal / vertical |
| C / B | クロップ / 黒塗りの範囲選択を開始 / start crop / blackout selection |
| Ctrl + R | リサイズ（ピクセル / % 指定） / resize (pixel / percent dialog) |
| Enter または選択内ダブルクリック / Enter or double-click inside | 選択を適用 / apply the selection |
| Esc | 範囲選択の解除、またはフルスクリーン解除 / cancel the selection, or leave fullscreen |
| ドラッグ＆ドロップ / drop a file or folder | ファイルまたはフォルダを開く / open the dropped file or folder |

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

## サードパーティソフトウェア / Third-party software

**日本語:**
chirami は任意コンポーネントとして [libjpeg-turbo](https://libjpeg-turbo.org/) を同梱します。exe と同じフォルダに `turbojpeg.dll` があれば JPEG のデコードに使用し、削除しても Windows 標準の WIC デコーダで動作します。ライセンスは `licenses/libjpeg-turbo.txt` を参照してください。

**English:**
chirami optionally bundles [libjpeg-turbo](https://libjpeg-turbo.org/). When `turbojpeg.dll` sits next to the exe it is used for JPEG decoding; remove it and the standard Windows WIC decoder takes over. See `licenses/libjpeg-turbo.txt` for its license.

This software is based in part on the work of the Independent JPEG Group.

## ライセンス / License

MIT License — see [LICENSE](LICENSE)
