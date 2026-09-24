# r800zzXRdlnaServer for Windows+NVIDIA GPU

[English](README.md) | [Русский](README_ru.md) | [Español](README_es.md) | [ภาษาไทย](README_th.md) | [中文](README_zh.md) | [한국어](README_ko.md) | [日本語](README_ja.md)

PICO、Meta Quest、その他の対応デバイスでのXR動画再生に対応し、リアルタイムAI背景除去（RVM）、アルファ／クロマキー出力、動画変換機能を備えたWindows用DLNAサーバーです。

**リアルタイムAIパススルーにはNVIDIA GPUが必要です。**

`r800zzXRdlnaServer` はネイティブC++アプリケーションです。FFmpegライブラリを直接使用し、Pythonや `ffmpeg.exe` は起動しません。

## ダウンロード

https://github.com/r800zz/r800zzxrdlnaserver/releases/latest

<a href="./jpeg/server_ja.jpeg" target="_blank">
  <img src="./jpeg/server_ja.jpeg\" alt=\"r800zzXRdlnaServer for Windows+NVIDIAGPU" width="402" height="356" border="2"></a>

### 出力に対応するVR動画プレイヤー

グリーンバックMP4について、クロマキー機能を備えたVRプレイヤーの例：

- [PICO/Meta用R800ZZbrowser](https://vr180g.com/browser/browser.php?l=en)
- [PICO/Meta](https://vr180g.com/pico/vrplayer.php?l=en)

Alpha Packed（DeoVRアルファ動画形式）：

- [PICO/Meta用r800zzvrplayer](https://vr180g.com/pico/vrplayer.php?l=en)
- DeoVR（非VR動画には対応していません。）

WebM VP9 Alphaについて、動作を確認したVRプレイヤー：

- **[r800zzvrplayer 0.6以降](https://vr180g.com/pico/vrplayer.php?l=en)**

## 機能

- 選択した動画フォルダーと、個別に選択した動画ファイルをDLNAで配信します。
- DLNAメディア一覧に元のファイル名を正確に表示します。
- DeoVRを含む対応クライアント向けのUPnP/DLNA検出とContentDirectoryに対応します。
- Robust Video Matting（RVM）で動画の背景をリアルタイムに除去します。
- Alpha Packed、完全なWebM VP9アルファ、グリーンクロマキー出力に対応します。
- NVIDIA GPUアクセラレーションとCPUフォールバックを備えたオフライン動画変換を内蔵します。
- 音声、シーク、一時停止／再生、全画面、SBS左半分表示に対応した簡易Windows動画プレイヤーを内蔵します。
- NVIDIA CUDA/RVM/NVENCの利用可否を自動判定します。
- 英語、ロシア語、スペイン語、タイ語、中国語、韓国語、日本語のUIを備えています。
- ユーザーごとの設定はインストール先ではなく `%LOCALAPPDATA%` に保存します。

## DLNA出力モード

| モード | 出力 | 備考 |
| --- | --- | --- |
| Alpha Packed | MPEG-TS内のHEVC/NVENC | 対応XRプレイヤー向けの高速リアルタイムモードです。 |
| Alpha WebM VP9 | 完全なVP9アルファを含むWebM | CPUベースのlibvpxエンコードを使用するため、特に4K/60動画ではコマ落ちする場合があります。 |
| Chroma Key | グリーン背景のHEVC/NVENC | 受信側プレイヤーでグリーンクロマキーモードを使用してください。 |
| OFF | 元の動画ファイル | RVM処理を行わず、NVIDIA GPUも不要です。 |

アルファの解釈は受信側プレイヤーに依存します。選択したアルファ形式に対応していないクライアントでは、不透明な映像として表示される場合があります。

## 動作環境

### ビルド済みリリース

- Windows 10またはWindows 11（64ビット）
- Windows PCと再生デバイスが接続された同一のプライベートLAN
- 対応DLNA動画プレイヤー
- リアルタイムAIモードには対応NVIDIA GPUと最新のNVIDIAドライバー

ビルド済みパッケージには、必要なアプリケーション実行時コンポーネントが含まれています。パッケージ版を通常使用する場合、CUDA Toolkitを別途インストールする必要はありません。GPU動作にはNVIDIAディスプレイドライバーが必要です。

対応NVIDIA GPUがない場合：

- `OFF` モードによる通常のDLNA配信は利用できます。
- オフラインRVM変換では、ONNX Runtime CPU EPを通じてFP32 ONNXモデルを使用できます。
- リアルタイムのAlpha Packed、Alpha WebM VP9、Chroma Keyモードは無効になります。

## インストール

1. Repositoryの **Releases** ページから最新のWindows x64インストーラーをダウンロードします。
2. インストーラーを実行します。
3. `r800zzXRdlnaServer` を起動します。
4. Windowsファイアウォールの確認が表示された場合は、**プライベートネットワーク**へのアクセスを許可します。

GPUビルドにはONNX Runtime CUDAサポート、CUDA/cuDNNランタイムコンポーネント、FFmpegライブラリ、RVMモデルが含まれるため、インストーラーのサイズは比較的大きくなります。

## DLNAの基本的な使い方

1. **動画フォルダーを選択...** でフォルダーを選び、**動画ファイルを選択...** で個別ファイルを追加するか、両方を使用します。
2. 出力モードを選択します。
   - **Alpha packed**
   - **Alpha WebM VP9**
   - **Chroma Key**
   - **OFF**
3. 公開するIPv4アドレスを確認します。
4. **DLNAサーバーを開始** を選択します。
5. 同じLAN上の対応DLNAプレイヤーを開きます。
6. `r800zzXRdlnaServer` を選択し、動画を選びます。

サーバーログには、DLNA検出、デバイス記述要求、メディア一覧要求、再生要求、workerのエラーが表示されます。**ログを消去** で表示中のログを消去できます。

## 動画変換

内蔵コンバーターは次の形式を作成できます。

- クロマキーMP4
- Alpha WebM VP9
- Alpha Packed MP4

提案される出力ファイル名には変換開始日時が含まれます。

```text
movie_RVM_ChromaKey_202609210542.mp4
```

変換の進行状況は出力フレーム数として表示されます。コンバーターは最初に内部の一時ファイルへ書き込み、エンコーダーとコンテナが正常に完了した後でのみ、指定した出力ファイル名を公開します。変換ログは出力ファイルと同じ場所に保存されます。

NVIDIAアクセラレーションが利用可能な場合、変換にはNVDEC、CUDA、ONNX Runtime CUDA EP経由のRVM、および適用可能な場合はNVENCを使用します。利用できない場合は、FFmpegソフトウェアデコード、ONNX Runtime CPU EP経由のFP32 RVMモデル、ソフトウェアエンコードを使用します。

## 内蔵動画プレイヤー

**ビデオプレイヤー...** は、DLNAの選択とは独立してローカル動画を開きます。

操作：

- 再生／一時停止
- シークバー
- 現在位置と総時間
- 全画面切り替え
- SBS左半分表示
- Space：再生／一時停止
- 左／右矢印：5秒シーク
- Escape：全画面を解除、または通常表示時にプレイヤーを閉じる

プレイヤーは対応コーデックでNVIDIA NVDECを試し、利用できない場合はソフトウェアデコードへフォールバックします。

## 音声処理

- `OFF` モードは元のファイルを変更せずに送信し、受信側プレイヤーが音声コーデックを処理します。
- Alpha PackedおよびChroma KeyのMPEG-TS出力はAAC音声を使用します。
- Alpha WebM VP9出力はOpus音声を使用します。
- 必要に応じて、FFmpegでデコード可能なその他の入力音声をトランスコードできます。
- マルチチャンネル入力はステレオへダウンミックスし、モノラル入力はモノラルのまま維持します。

## ソースからのビルド

### 必要環境

- Windows 10またはWindows 11（64ビット）
- **C++によるデスクトップ開発** を含むVisual Studio 2022
- CMake 3.24以降
- NVIDIA CUDA Toolkit 12.8
- ビルド依存関係のダウンロード時にインターネット接続

### ビルド

Repositoryのルートでコマンドプロンプトを開き、次を実行します。

```bat
setup_dependencies.bat
build.bat
```

`setup_dependencies.bat` は固定バージョンの開発依存関係と公式RVM ONNXモデルをダウンロードします。`build.bat` はCMakeとNMakeを使用してプロジェクトを構成し、ビルドします。

生成物は次の場所に出力されます。

```text
build_cuda_ep\bin\r800zz_dlna_server.exe
build_cuda_ep\bin\r800zz_ai_worker.exe
build_cuda_ep\bin\rvm_mobilenetv3_fp16.onnx
build_cuda_ep\bin\rvm_mobilenetv3_fp32.onnx
```

現在のビルドでは、FFmpeg共有パッケージを `N-124714-g49a77d37be` に固定しています。NVIDIAドライバーとNVENC APIの要件を確認せずに、固定パッケージを常に最新版へ追従するビルドへ変更しないでください。

## 実装

リアルタイムNVIDIAパス：

```text
FFmpeg NVDEC
    -> CUDA前処理
    -> ONNX Runtime CUDA EP / RVM
    -> CUDAアルファパッキングまたはクロマキー合成
    -> FFmpeg NVENC
    -> MPEG-TS / DLNA
```

完全なWebMアルファのパスでは、CUDA上でYUVA420Pデータを準備し、再利用可能なCPUフレームへ転送して `libvpx-vp9` を使用します。NVIDIA GPUはVP9ハードウェアエンコードに対応していないためです。

FFmpegは次のCライブラリと共有DLLを通じて使用します。

- `avformat`
- `avcodec`
- `avutil`
- `swresample`
- `swscale`

## 既知の制限

- リアルタイムAIパイプラインには現在NVIDIA GPUが必要です。
- AMDおよびIntel GPUアクセラレーションは未実装です。
- WebM VP9アルファエンコードはCPU負荷が高く、入力動画のフレームレートを維持できない場合があります。
- アルファとクロマキーへの対応状況はDLNA/XRプレイヤーによって異なります。
- 現在の対象環境はWindows x64です。

## 第三者コンポーネント

このプロジェクトは次を使用しています。

- [Robust Video Matting](https://github.com/PeterL1n/RobustVideoMatting)
- [FFmpeg](https://ffmpeg.org/)
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit)
- [NVIDIA cuDNN](https://developer.nvidia.com/cudnn)

各第三者コンポーネントには、それぞれのライセンスと再配布条件が適用されます。

## ライセンス

このプロジェクトは **GNU General Public License v3.0** の下で配布されます。[LICENSE](LICENSE)を参照してください。

RVMモデルおよびRVM由来のコンポーネントには、上流のRobust Video Mattingライセンスも適用されます。ソース配布物とバイナリリリースでは、該当するすべての著作権表示とライセンス表示を保持する必要があります。

## リンク

- [vr180g.com](https://vr180g.com/)
- [YouTubeのR800ZZ](https://www.youtube.com/@R800ZZ)

