# AviUtl APNG出力ツール

AviUtl（無印・32bit）からAPNG（アニメーションPNG）を書き出すための自作ツール2つ。Claude Codeで開発。

> 開発中。動作確認は作者環境のみ。

## 構成

| ツール | 種類 | アルファ | 用途 |
|---|---|---|---|
| `apng_output/` | AviUtl出力プラグイン（.auo / Win32） | なし（RGB24bit） | AviUtlから直接APNG出力 |
| `png2apng/` | 単独変換ツール（.exe / x64） | あり（RGBA32bit） | 透過AVI・連番PNGをAPNGに変換 |

## ダウンロード

ビルド済みの `apng_output.auo` と `png2apng.exe` は [Releases](../../releases) から入手できる。

※ 署名なしの自作exeのため、ダウンロード時や実行時にブラウザ・Windows Defenderが警告を出す場合がある。

## 1. APNG出力2（apng_output.auo）

AviUtlの「プラグイン出力」に **「APNG出力2」** として表示される。

- フルスクリーン出力対応
- 設定ダイアログで指定可能：圧縮率（zlib 0〜9）・フレームレート・ループ回数
- 適応PNGフィルタ＋zlib圧縮

### 導入
1. `apng_output.auo` を `aviutl/Plugins/` に置く
2. AviUtlを起動 →「ファイル」→「プラグイン出力」→「APNG出力2」

## 2. png2apng（透過APNG変換）

AviUtl無印の出力プラグインSDKにはアルファチャンネルを受け取る仕組みがない。そのため、透過付きで出力したい場合はAviUtl標準機能で透過AVIを出し、このツールで変換する2段階方式にした。

### 使い方
1. AviUtlで「ファイル」→「プラグイン出力」→「拡張編集 AVI/BMP出力(RGBA)」
2. ビデオ圧縮で **RGBA(32bit) 非圧縮** を選んでAVI出力
3. 出力したAVIを `png2apng.exe` にドラッグ＆ドロップ（またはコマンドライン指定）
4. 同じフォルダに `元のファイル名_apng.png` が生成される

```
png2apng.exe input.avi
```

### 対応入力
- 無圧縮 RGBA(32bit) AVI（自作RIFFパーサで直接読み込み）
- 連番PNGフォルダ

※ UtVideo・Lagarith等の圧縮コーデックAVIは非対応

## ビルド

- Visual Studio 2022（C++によるデスクトップ開発）
- `apng_output`：**Win32**（AviUtl無印が32bitのため）
- `png2apng`：**x64**

### 必要なもの（同梱していないもの）
- `output.h`：AviUtl出力プラグインSDK付属のヘッダ。再配布可否が不明なため同梱していない。AviUtl公式配布のSDKから取得し `apng_output/` に置くこと

### ビルドコマンド例
```
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" && msbuild apng_output\apng_output.vcxproj /p:Configuration=Release /p:Platform=Win32
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && msbuild png2apng\png2apng.vcxproj /p:Configuration=Release /p:Platform=x64
```

### 文字コードについて
ソースはUTF-8、AviUtlはShift-JIS前提のため、コンパイルオプションに以下を指定している。

```
/source-charset:utf-8 /execution-charset:.932
```

`/utf-8` だけだと埋め込み文字列までUTF-8になり、AviUtl上でプラグイン名が文字化けする。

## 既知の問題

- `png2apng.exe` にエクスプローラーからドラッグ＆ドロップすると、環境によって黒い画面が一瞬出て終了する。コマンドラインから同じファイルを指定すると正常に動作する。原因調査中

## 使用ライブラリ

- [zlib](https://zlib.net/) 1.3.1（zlib License）
- [stb_image.h](https://github.com/nothings/stb)（Public Domain / MIT）

## 作者

松尾龍太（[@matsuo_kun2013](https://x.com/matsuo_kun2013)）
