# r800zzXRdlnaServer for Windows+NVIDIA GPU

[English](README.md) | [Русский](README_ru.md) | [Español](README_es.md) | [ภาษาไทย](README_th.md) | [中文](README_zh.md) | [한국어](README_ko.md) | [日本語](README_ja.md)

PICO, Meta Quest 및 기타 호환 장치에서 XR 비디오를 재생하기 위한 Windows용 DLNA 서버로, 실시간 AI 배경 제거(RVM), 알파/크로마 키 출력 및 비디오 변환 기능을 제공합니다.

**실시간 AI 패스스루에는 NVIDIA GPU가 필요합니다.**

`r800zzXRdlnaServer`는 네이티브 C++ 애플리케이션입니다. FFmpeg 라이브러리를 직접 사용하며 Python이나 `ffmpeg.exe`를 실행하지 않습니다.

## 다운로드

https://github.com/r800zz/r800zzxrdlnaserver/releases/latest

<a href="./jpeg/server_ko.jpeg" target="_blank">
  <img src="./jpeg/server_ko.jpeg\" alt=\"r800zzXRdlnaServer for Windows+NVIDIAGPU" width="402" height="356" border="2"></a>

### 출력 형식을 지원하는 VR 비디오 플레이어

그린 스크린 MP4의 경우 크로마 키 기능을 제공하는 VR 플레이어의 예는 다음과 같습니다.

- [PICO/Meta용 R800ZZbrowser](https://vr180g.com/browser/browser.php?l=kr)
- [PICO/Meta용 r800zzvrplayer](https://vr180g.com/pico/vrplayer.php?l=kr)

Alpha Packed(DeoVR 알파 비디오 형식)의 경우:

- [PICO/Meta용 r800zzvrplayer](https://vr180g.com/pico/vrplayer.php?l=kr)
- DeoVR(비VR 비디오는 지원되지 않습니다.)

WebM VP9 Alpha에서 동작을 확인한 VR 플레이어:

- **[r800zzvrplayer 0.6 이상](https://vr180g.com/pico/vrplayer.php?l=kr)**

## 기능

- 선택한 비디오 폴더와 개별적으로 선택한 비디오 파일을 DLNA로 제공합니다.
- DLNA 미디어 목록에 원본 파일명을 정확히 유지합니다.
- DeoVR을 포함한 호환 클라이언트를 위해 UPnP/DLNA 검색 및 ContentDirectory를 지원합니다.
- Robust Video Matting(RVM)을 사용해 비디오 배경을 실시간으로 제거합니다.
- Alpha Packed, 실제 WebM VP9 알파 및 그린 크로마 키 출력을 지원합니다.
- NVIDIA GPU 가속 및 CPU 폴백을 지원하는 오프라인 비디오 변환 기능을 내장합니다.
- 오디오, 탐색, 일시 정지/재생, 전체 화면 및 SBS 왼쪽 절반 표시를 지원하는 간단한 Windows 비디오 플레이어를 포함합니다.
- NVIDIA CUDA/RVM/NVENC 사용 가능 여부를 자동으로 감지합니다.
- 영어, 러시아어, 스페인어, 태국어, 중국어, 한국어 및 일본어 인터페이스를 제공합니다.
- 사용자별 설정은 설치 디렉터리가 아닌 `%LOCALAPPDATA%`에 저장됩니다.

## DLNA 출력 모드

| 모드 | 출력 | 참고 |
| --- | --- | --- |
| Alpha Packed | MPEG-TS의 HEVC/NVENC | 호환 XR 플레이어용 고속 실시간 모드입니다. |
| Alpha WebM VP9 | 실제 VP9 알파가 포함된 WebM | CPU 기반 libvpx 인코딩을 사용하며 특히 4K/60 비디오에서 프레임이 누락될 수 있습니다. |
| Chroma Key | 그린 배경의 HEVC/NVENC | 수신 플레이어에서 그린 크로마 키 모드를 사용하십시오. |
| OFF | 원본 소스 파일 | RVM 처리를 하지 않으며 NVIDIA GPU도 필요하지 않습니다. |

알파 해석 방식은 수신 플레이어에 따라 달라집니다. 선택한 알파 형식을 지원하지 않는 클라이언트에서는 불투명한 영상으로 표시될 수 있습니다.

## 시스템 요구 사항

### 사전 빌드 릴리스

- Windows 10 또는 Windows 11, 64비트
- Windows PC와 재생 장치가 연결된 동일한 사설 로컬 네트워크
- 호환 DLNA 비디오 플레이어
- 실시간 AI 모드용 지원 NVIDIA GPU 및 최신 NVIDIA 드라이버

사전 빌드 패키지에는 필요한 애플리케이션 런타임 구성 요소가 포함되어 있습니다. 패키지 애플리케이션을 일반적으로 사용할 때는 CUDA Toolkit을 별도로 설치할 필요가 없습니다. GPU 작동에는 NVIDIA 디스플레이 드라이버가 필요합니다.

호환 NVIDIA GPU가 없는 경우:

- `OFF` 모드의 일반 DLNA 전송은 계속 사용할 수 있습니다.
- 오프라인 RVM 변환은 ONNX Runtime CPU EP를 통해 FP32 ONNX 모델을 사용할 수 있습니다.
- 실시간 Alpha Packed, Alpha WebM VP9 및 Chroma Key 모드는 비활성화됩니다.

## 설치

1. Repository의 **Releases** 페이지에서 최신 Windows x64 설치 프로그램을 다운로드합니다.
2. 설치 프로그램을 실행합니다.
3. `r800zzXRdlnaServer`를 시작합니다.
4. Windows 방화벽에서 요청하면 **개인 네트워크**에 대한 액세스를 허용합니다.

GPU 빌드에는 ONNX Runtime CUDA 지원, CUDA/cuDNN 런타임 구성 요소, FFmpeg 라이브러리 및 RVM 모델이 포함되므로 설치 프로그램의 크기가 비교적 큽니다.

## 기본 DLNA 사용법

1. **비디오 폴더 선택...**으로 폴더를 선택하고 **비디오 파일 선택...**으로 개별 파일을 추가하거나 두 방법을 함께 사용합니다.
2. 출력 모드를 선택합니다.
   - **Alpha packed**
   - **Alpha WebM VP9**
   - **Chroma Key**
   - **OFF**
3. 게시할 IPv4 주소를 확인합니다.
4. **DLNA 서버 시작**을 선택합니다.
5. 동일한 로컬 네트워크에서 호환 DLNA 플레이어를 엽니다.
6. `r800zzXRdlnaServer`를 선택한 다음 비디오를 선택합니다.

서버 로그에는 DLNA 검색, 장치 설명 요청, 미디어 목록 요청, 재생 요청 및 worker 오류가 표시됩니다. **로그 지우기**로 표시된 로그를 지울 수 있습니다.

## 비디오 변환

내장 변환기는 다음 출력을 만들 수 있습니다.

- 크로마 키 MP4
- Alpha WebM VP9
- Alpha Packed MP4

제안되는 출력 파일명에는 변환 시작 시간이 포함됩니다.

```text
movie_RVM_ChromaKey_202609210542.mp4
```

변환 진행 상황은 출력 프레임 수로 표시됩니다. 변환기는 먼저 내부 임시 파일에 기록하고 인코더와 컨테이너가 성공적으로 완료된 후에만 요청된 출력 파일명을 생성합니다. 변환 로그는 출력 파일 옆에 저장됩니다.

NVIDIA 가속을 사용할 수 있으면 변환에 NVDEC, CUDA, ONNX Runtime CUDA EP를 통한 RVM, 그리고 해당하는 경우 NVENC를 사용합니다. 그렇지 않으면 FFmpeg 소프트웨어 디코딩, ONNX Runtime CPU EP를 통한 FP32 RVM 모델 및 소프트웨어 인코딩을 사용합니다.

## 내장 비디오 플레이어

**비디오 플레이어...**는 DLNA 선택과 별개로 로컬 비디오를 엽니다.

컨트롤:

- 재생/일시 정지
- 탐색 막대
- 현재 시간 및 전체 시간
- 전체 화면 전환
- SBS 왼쪽 절반 표시
- Space: 재생/일시 정지
- 왼쪽/오른쪽 화살표: 5초 탐색
- Escape: 전체 화면 종료 또는 일반 창에서 플레이어 닫기

플레이어는 지원되는 코덱에서 NVIDIA NVDEC를 먼저 시도하고 사용할 수 없으면 소프트웨어 디코딩으로 폴백합니다.

## 오디오 처리

- `OFF` 모드는 원본 파일을 변경하지 않고 전송하며 수신 플레이어가 오디오 코덱을 처리합니다.
- Alpha Packed 및 Chroma Key MPEG-TS 출력은 AAC 오디오를 사용합니다.
- Alpha WebM VP9 출력은 Opus 오디오를 사용합니다.
- 필요한 경우 FFmpeg에서 디코딩할 수 있는 다른 소스 오디오를 트랜스코딩할 수 있습니다.
- 멀티채널 입력은 스테레오로 다운믹스하고 모노 입력은 모노로 유지합니다.

## 소스에서 빌드

### 요구 사항

- Windows 10 또는 Windows 11, 64비트
- **C++를 사용한 데스크톱 개발** 워크로드가 설치된 Visual Studio 2022
- CMake 3.24 이상
- NVIDIA CUDA Toolkit 12.8
- 빌드 종속성을 다운로드할 때 인터넷 연결

### 빌드

Repository 루트에서 명령 프롬프트를 열고 다음을 실행합니다.

```bat
setup_dependencies.bat
build.bat
```

`setup_dependencies.bat`는 고정 버전의 개발 종속성과 공식 RVM ONNX 모델을 다운로드합니다. `build.bat`는 CMake와 NMake를 사용해 프로젝트를 구성하고 빌드합니다.

생성된 파일은 다음 위치에 있습니다.

```text
build_cuda_ep\bin\r800zz_dlna_server.exe
build_cuda_ep\bin\r800zz_ai_worker.exe
build_cuda_ep\bin\rvm_mobilenetv3_fp16.onnx
build_cuda_ep\bin\rvm_mobilenetv3_fp32.onnx
```

현재 빌드는 FFmpeg 공유 패키지를 `N-124714-g49a77d37be`로 고정합니다. NVIDIA 드라이버 및 NVENC API 요구 사항을 확인하지 않고 고정 패키지를 항상 최신 버전을 사용하는 빌드로 변경하지 마십시오.

## 구현

실시간 NVIDIA 처리 경로:

```text
FFmpeg NVDEC
    -> CUDA 전처리
    -> ONNX Runtime CUDA EP / RVM
    -> CUDA 알파 패킹 또는 크로마 키 합성
    -> FFmpeg NVENC
    -> MPEG-TS / DLNA
```

실제 WebM 알파 경로는 CUDA에서 YUVA420P 데이터를 준비하고 재사용 가능한 CPU 프레임으로 전송한 다음 `libvpx-vp9`를 사용합니다. NVIDIA GPU가 VP9 하드웨어 인코딩을 제공하지 않기 때문입니다.

FFmpeg는 다음 C 라이브러리와 공유 DLL을 통해 사용됩니다.

- `avformat`
- `avcodec`
- `avutil`
- `swresample`
- `swscale`

## 알려진 제한 사항

- 실시간 AI 파이프라인에는 현재 NVIDIA GPU가 필요합니다.
- AMD 및 Intel GPU 가속은 구현되지 않았습니다.
- WebM VP9 알파 인코딩은 CPU 사용량이 높아 소스 프레임 속도를 유지하지 못할 수 있습니다.
- 알파 및 크로마 키 지원은 DLNA/XR 플레이어마다 다릅니다.
- 애플리케이션은 현재 Windows x64를 대상으로 합니다.

## 서드파티 구성 요소

이 프로젝트는 다음을 사용합니다.

- [Robust Video Matting](https://github.com/PeterL1n/RobustVideoMatting)
- [FFmpeg](https://ffmpeg.org/)
- [ONNX Runtime](https://github.com/microsoft/onnxruntime)
- [Dear ImGui](https://github.com/ocornut/imgui)
- [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit)
- [NVIDIA cuDNN](https://developer.nvidia.com/cudnn)

각 서드파티 구성 요소에는 해당 라이선스와 재배포 조건이 적용됩니다.

## 라이선스

이 프로젝트는 **GNU General Public License v3.0**에 따라 배포됩니다. [LICENSE](LICENSE)를 참조하십시오.

RVM 모델과 RVM에서 파생된 구성 요소에는 업스트림 Robust Video Matting 라이선스도 적용됩니다. 소스 배포 및 바이너리 릴리스는 해당하는 모든 저작권 및 라이선스 고지를 유지해야 합니다.

## 링크

- [vr180g.com](https://vr180g.com/)
- [YouTube의 R800ZZ](https://www.youtube.com/@R800ZZ)

