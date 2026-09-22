# r800zzXRdlnaServer for Windows+NVIDIA GPU

[English](README.md) | [Русский](README_ru.md) | [Español](README_es.md) | [ภาษาไทย](README_th.md) | [中文](README_zh.md) | [한국어](README_ko.md) | [日本語](README_ja.md)

一款用于在 PICO、Meta Quest 及其他兼容设备上播放 XR 视频的 Windows DLNA 服务器，支持实时 AI 背景移除（RVM）、Alpha／色键输出和视频转换。

**实时 AI Passthrough 需要 NVIDIA GPU。**

`r800zzXRdlnaServer` 是原生 C++ 应用程序。它直接使用 FFmpeg 库，不会启动 Python 或 `ffmpeg.exe`。

## 下载

https://github.com/r800zz/r800zzxrdlnaserver/releases/latest

### 支持输出格式的 VR 视频播放器

对于绿幕 MP4，具备色键功能的 VR 播放器示例包括：

- [适用于 PICO/Meta 的 R800ZZbrowser](https://vr180g.com/browser/browser.php?l=en)
- [适用于 PICO 4 Ultra/PICO4 的 r800zzvrplayer](https://vr180g.com/pico/vrplayer.php?l=en)

对于 Alpha Packed（DeoVR Alpha 视频格式）：

- [适用于 PICO 4 Ultra/PICO4 的 r800zzvrplayer](https://vr180g.com/pico/vrplayer.php?l=en)
- DeoVR（不支持非 VR 视频。）

对于 WebM VP9 Alpha，我已确认可用的 VR 播放器是：

- **[r800zzvrplayer 0.6 或更高版本](https://vr180g.com/pico/vrplayer.php?l=en)**

## 功能

- 通过 DLNA 提供所选视频文件夹和单独选择的视频文件。
- 在 DLNA 媒体列表中保留准确的源文件名。
- 为兼容客户端（包括 DeoVR）提供 UPnP/DLNA 发现和 ContentDirectory 支持。
- 使用 Robust Video Matting（RVM）实时移除视频背景。
- 支持 Alpha Packed、真正的 WebM VP9 Alpha 和绿色色键输出。
- 内置离线视频转换，支持 NVIDIA GPU 加速和 CPU 回退。
- 内置简易 Windows 视频播放器，支持音频、跳转、暂停／继续、全屏和 SBS 左半边显示。
- 自动检测 NVIDIA CUDA/RVM/NVENC 是否可用。
- 提供英语、俄语、西班牙语、泰语、中文、韩语和日语界面。
- 每位用户的设置保存在 `%LOCALAPPDATA%` 中，而不是安装目录中。

## DLNA 输出模式

| 模式 | 输出 | 说明 |
| --- | --- | --- |
| Alpha Packed | MPEG-TS 中的 HEVC/NVENC | 面向兼容 XR 播放器的快速实时模式。 |
| Alpha WebM VP9 | 包含真正 VP9 Alpha 的 WebM | 使用基于 CPU 的 libvpx 编码，可能会丢帧，尤其是 4K/60 视频。 |
| Chroma Key | 绿色背景的 HEVC/NVENC | 请在接收播放器中使用绿色色键模式。 |
| OFF | 原始源文件 | 不进行 RVM 处理，也不需要 NVIDIA GPU。 |

Alpha 的解释方式取决于接收播放器。不支持所选 Alpha 格式的客户端可能会显示不透明画面。

## 系统要求

### 预编译版本

- Windows 10 或 Windows 11，64 位
- Windows PC 与播放设备连接到同一专用局域网
- 兼容的 DLNA 视频播放器
- 实时 AI 模式需要受支持的 NVIDIA GPU 和较新的 NVIDIA 驱动程序

预编译包包含所需的应用程序运行时组件。正常使用打包版本时，无需另行安装 CUDA Toolkit。GPU 操作仍需要 NVIDIA 显示驱动程序。

没有兼容 NVIDIA GPU 时：

- 仍可在 `OFF` 模式下使用普通 DLNA 传输。
- 离线 RVM 转换可通过 ONNX Runtime CPU EP 使用 FP32 ONNX 模型。
- 实时 Alpha Packed、Alpha WebM VP9 和 Chroma Key 模式将被禁用。

## 安装

1. 从 Repository 的 **Releases** 页面下载最新的 Windows x64 安装程序。
2. 运行安装程序。
3. 启动 `r800zzXRdlnaServer`。
4. 如果 Windows 防火墙询问，请允许访问**专用网络**。

安装程序体积较大，因为 GPU 版本包含 ONNX Runtime CUDA 支持、CUDA/cuDNN 运行时组件、FFmpeg 库和 RVM 模型。

## DLNA 基本用法

1. 使用 **选择视频文件夹...** 选择文件夹，使用 **选择视频文件...** 添加单独文件，或者同时使用两者。
2. 选择输出模式：
   - **Alpha packed**
   - **Alpha WebM VP9**
   - **Chroma Key**
   - **OFF**
3. 确认公布的 IPv4 地址。
4. 选择 **启动 DLNA 服务器**。
5. 在同一局域网上打开兼容的 DLNA 播放器。
6. 选择 `r800zzXRdlnaServer`，然后选择视频。

服务器日志会显示 DLNA 发现、设备描述请求、媒体列表请求、播放请求和 worker 错误。使用 **清除日志** 可清除当前显示的日志。

## 视频转换

内置转换器可生成：

- 色键 MP4
- Alpha WebM VP9
- Alpha Packed MP4

建议的输出文件名包含转换开始时间：

```text
movie_RVM_ChromaKey_202609210542.mp4
```

转换进度以输出帧数显示。转换器首先写入内部临时文件，只有在编码器和容器成功完成后，才会生成所请求的输出文件名。转换日志保存在输出文件旁边。

当 NVIDIA 加速可用时，转换使用 NVDEC、CUDA、通过 ONNX Runtime CUDA EP 运行的 RVM，并在适用时使用 NVENC。否则，转换使用 FFmpeg 软件解码、通过 ONNX Runtime CPU EP 运行的 FP32 RVM 模型以及软件编码。

## 内置视频播放器

**视频播放器...** 可独立于 DLNA 选择打开本地视频。

控制功能包括：

- 播放／暂停
- 进度条
- 当前时间和总时间
- 全屏切换
- SBS 左半边显示
- Space：播放／暂停
- 左／右方向键：跳转五秒
- Escape：退出全屏，或在非全屏状态下关闭播放器

播放器会为受支持的编解码器尝试使用 NVIDIA NVDEC，无法使用时回退到软件解码。

## 音频处理

- `OFF` 模式会原样发送源文件，由接收播放器处理其音频编解码器。
- Alpha Packed 和 Chroma Key 的 MPEG-TS 输出使用 AAC 音频。
- Alpha WebM VP9 输出使用 Opus 音频。
- 必要时可转码 FFmpeg 能够解码的其他源音频。
- 多声道输入会下混为立体声；单声道输入保持单声道。

## 从源代码构建

### 要求

- Windows 10 或 Windows 11，64 位
- 安装了 **使用 C++ 的桌面开发** 工作负载的 Visual Studio 2022
- CMake 3.24 或更高版本
- NVIDIA CUDA Toolkit 12.8
- 下载构建依赖项时需要互联网连接

### 构建

在 Repository 根目录打开命令提示符并运行：

```bat
setup_dependencies.bat
build.bat
```

`setup_dependencies.bat` 下载固定版本的开发依赖项和官方 RVM ONNX 模型。`build.bat` 使用 CMake 和 NMake 配置并构建项目。

生成的文件位于：

```text
build_cuda_ep\bin\r800zz_dlna_server.exe
build_cuda_ep\bin\r800zz_ai_worker.exe
build_cuda_ep\bin\rvm_mobilenetv3_fp16.onnx
build_cuda_ep\bin\rvm_mobilenetv3_fp32.onnx
```

当前构建将 FFmpeg 共享包固定为 `N-124714-g49a77d37be`。除非确认 NVIDIA 驱动程序和 NVENC API 要求，否则不要将固定包改为浮动的最新版本。

## 实现

实时 NVIDIA 处理路径：

```text
FFmpeg NVDEC
    -> CUDA 预处理
    -> ONNX Runtime CUDA EP / RVM
    -> CUDA Alpha 打包或色键合成
    -> FFmpeg NVENC
    -> MPEG-TS / DLNA
```

真正的 WebM Alpha 路径在 CUDA 上准备 YUVA420P 数据，将其传输到可复用的 CPU 帧，然后使用 `libvpx-vp9`，因为 NVIDIA GPU 不提供 VP9 硬件编码。

FFmpeg 通过以下 C 库和共享 DLL 使用：

- `avformat`
- `avcodec`
- `avutil`
- `swresample`
- `swscale`

## 已知限制

- 实时 AI 处理管线目前需要 NVIDIA GPU。
- 尚未实现 AMD 和 Intel GPU 加速。
- WebM VP9 Alpha 编码会占用大量 CPU，可能无法维持源视频帧率。
- 不同 DLNA/XR 播放器对 Alpha 和色键的支持情况不同。
- 应用程序目前仅面向 Windows x64。

## 第三方组件

本项目使用：

- [Robust Video Matting](https://github.com/PeterL1n/RobustVideoMatting)
- [FFmpeg](https://ffmpeg.org/)
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit)
- [NVIDIA cuDNN](https://developer.nvidia.com/cudnn)

每个第三方组件均受其各自的许可证和再分发条款约束。

## 许可证

本项目根据 **GNU General Public License v3.0** 分发。请参阅 [LICENSE](LICENSE)。

RVM 模型和源自 RVM 的组件也受上游 Robust Video Matting 许可证约束。源代码分发和二进制发布必须保留所有适用的版权及许可证声明。

## 链接

- [vr180g.com](https://vr180g.com/)
- [YouTube 上的 R800ZZ](https://www.youtube.com/@R800ZZ)

