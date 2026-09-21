#include "simple_video_player.h"

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kControlHeight = 48;
constexpr int kSeekRange = 10000;
constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int64_t kLateVideoDropUs = 40000;

enum ControlId {
    kPlayPauseButton = 1001,
    kFullscreenButton = 1002,
    kLeftHalfButton = 1003,
    kSeekBar = 1004,
    kPositionLabel = 1005,
};

struct PlayerLabels {
    const wchar_t* play;
    const wchar_t* pause;
    const wchar_t* fullscreen;
    const wchar_t* exitFullscreen;
    const wchar_t* leftHalf;
    const wchar_t* fullWidth;
};

const PlayerLabels& labelsFor(int language) {
    static const PlayerLabels labels[] = {
        {L"Play", L"Pause", L"Fullscreen", L"Exit Fullscreen", L"Left Half", L"Full Width"},
        {L"Воспроизвести", L"Пауза", L"На весь экран", L"Выйти из полного экрана", L"Левая половина", L"Полная ширина"},
        {L"Reproducir", L"Pausa", L"Pantalla completa", L"Salir de pantalla completa", L"Mitad izquierda", L"Ancho completo"},
        {L"เล่น", L"หยุดชั่วคราว", L"เต็มจอ", L"ออกจากเต็มจอ", L"ครึ่งซ้าย", L"เต็มความกว้าง"},
        {L"播放", L"暂停", L"全屏", L"退出全屏", L"左半边", L"完整宽度"},
        {L"재생", L"일시정지", L"전체 화면", L"전체 화면 종료", L"왼쪽 절반", L"전체 너비"},
        {L"再生", L"一時停止", L"全画面", L"全画面解除", L"左半分", L"全幅表示"},
    };
    return labels[std::clamp(language, 0, 6)];
}

std::string fferr(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::wstring formatPlayerTime(int64_t timeUs) {
    const int64_t totalSeconds = std::max<int64_t>(0, timeUs) / AV_TIME_BASE;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds / 60) % 60;
    const int64_t seconds = totalSeconds % 60;
    wchar_t text[32]{};
    if (hours > 0) {
        std::swprintf(text, std::size(text), L"%02lld:%02lld:%02lld",
                     static_cast<long long>(hours),
                     static_cast<long long>(minutes),
                     static_cast<long long>(seconds));
    } else {
        std::swprintf(text, std::size(text), L"%02lld:%02lld",
                     static_cast<long long>(minutes),
                     static_cast<long long>(seconds));
    }
    return text;
}

class PlayerWindow {
public:
    explicit PlayerWindow(int language) : labels_(labelsFor(language)) {}
    ~PlayerWindow() {
        if (hwnd_) DestroyWindow(hwnd_);
        destroyBackBuffer();
    }

    bool create(const std::filesystem::path& input, int videoWidth,
                int videoHeight, int64_t durationUs, bool usingNvdec,
                std::string& error) {
        width_ = videoWidth;
        height_ = videoHeight;
        durationUs_ = std::max<int64_t>(0, durationUs);
        pixels_.resize(static_cast<size_t>(width_) * height_ * 4u);
        bmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi_.bmiHeader.biWidth = width_;
        bmi_.bmiHeader.biHeight = -height_;
        bmi_.bmiHeader.biPlanes = 1;
        bmi_.bmiHeader.biBitCount = 32;
        bmi_.bmiHeader.biCompression = BI_RGB;

        INITCOMMONCONTROLSEX commonControls{};
        commonControls.dwSize = sizeof(commonControls);
        commonControls.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&commonControls);

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &PlayerWindow::WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        windowClass.lpszClassName = L"R800ZZSimpleVideoPlayerWindow";
        if (!RegisterClassExW(&windowClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = "Video Player window registration failed. Win32=" +
                    std::to_string(GetLastError());
            return false;
        }

        int clientWidth = std::min(width_, 960);
        int videoClientHeight = std::max(1, static_cast<int>(
            static_cast<double>(clientWidth) * height_ / width_));
        if (videoClientHeight > 540) {
            videoClientHeight = 540;
            clientWidth = std::max(1, static_cast<int>(
                static_cast<double>(videoClientHeight) * width_ / height_));
        }
        constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        RECT windowRect{0, 0, std::max(clientWidth, 700),
                        videoClientHeight + kControlHeight};
        AdjustWindowRect(&windowRect, windowStyle, FALSE);
        const std::wstring title = std::wstring(L"Video Player [") +
            (usingNvdec ? L"NVIDIA NVDEC" : L"CPU") + L"] - " +
            input.filename().wstring();
        hwnd_ = CreateWindowExW(
            0, windowClass.lpszClassName, title.c_str(), windowStyle,
            CW_USEDEFAULT, CW_USEDEFAULT,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            nullptr, nullptr, instance, this);
        if (!hwnd_) {
            error = "Video Player window creation failed. Win32=" +
                    std::to_string(GetLastError());
            return false;
        }

        playPauseButton_ = CreateWindowExW(
            0, L"BUTTON", labels_.pause, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 100, 30, hwnd_, reinterpret_cast<HMENU>(kPlayPauseButton),
            instance, nullptr);
        fullscreenButton_ = CreateWindowExW(
            0, L"BUTTON", labels_.fullscreen, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 120, 30, hwnd_, reinterpret_cast<HMENU>(kFullscreenButton),
            instance, nullptr);
        leftHalfButton_ = CreateWindowExW(
            0, L"BUTTON", labels_.leftHalf, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 120, 30, hwnd_, reinterpret_cast<HMENU>(kLeftHalfButton),
            instance, nullptr);
        seekBar_ = CreateWindowExW(
            0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0, 0, 200, 30, hwnd_, reinterpret_cast<HMENU>(kSeekBar),
            instance, nullptr);
        positionLabel_ = CreateWindowExW(
            0, L"STATIC", L"00:00 / 00:00",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
            0, 0, 120, 30, hwnd_, reinterpret_cast<HMENU>(kPositionLabel),
            instance, nullptr);
        if (!playPauseButton_ || !fullscreenButton_ || !leftHalfButton_ ||
            !seekBar_ || !positionLabel_) {
            error = "Video Player controls creation failed. Win32=" +
                    std::to_string(GetLastError());
            return false;
        }
        SendMessageW(seekBar_, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSeekRange));
        updatePositionText(0);
        layoutControls();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        SetFocus(hwnd_);
        return true;
    }

    bool pump() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { closed_ = true; break; }
            if (message.message == WM_KEYDOWN && message.hwnd != hwnd_ &&
                (message.wParam == VK_SPACE || message.wParam == VK_LEFT ||
                 message.wParam == VK_RIGHT || message.wParam == VK_ESCAPE)) {
                SendMessageW(hwnd_, WM_KEYDOWN, message.wParam, message.lParam);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return !closed_;
    }

    bool paused() const { return paused_; }
    bool seekPending() const { return seekAbsolutePending_ || seekDeltaUs_ != 0; }

    bool consumePlaybackClockReset() {
        const bool pending = playbackClockResetPending_;
        playbackClockResetPending_ = false;
        return pending;
    }

    bool consumeSeekRequest(int64_t currentUs, int64_t& targetUs) {
        if (seekAbsolutePending_) {
            targetUs = seekAbsoluteUs_;
            seekAbsolutePending_ = false;
            seekDeltaUs_ = 0;
            return true;
        }
        if (seekDeltaUs_ != 0) {
            targetUs = currentUs + seekDeltaUs_;
            seekDeltaUs_ = 0;
            return true;
        }
        return false;
    }

    uint8_t* pixels() { return pixels_.data(); }
    int stride() const { return width_ * 4; }

    void setPosition(int64_t positionUs) {
        if (!seekBar_ || durationUs_ <= 0) return;
        if (seekDragging_) return;
        const int position = static_cast<int>(std::clamp<int64_t>(
            positionUs * kSeekRange / durationUs_, 0, kSeekRange));
        SendMessageW(seekBar_, TBM_SETPOS, TRUE, position);
        updatePositionText(positionUs);
    }

    void present() {
        if (!hwnd_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        client.bottom = std::max<LONG>(0, client.bottom - kControlHeight);
        InvalidateRect(hwnd_, &client, FALSE);
        UpdateWindow(hwnd_);
    }

private:
    void setPaused(bool paused) {
        paused_ = paused;
        if (playPauseButton_) {
            SetWindowTextW(playPauseButton_, paused_ ? labels_.play : labels_.pause);
        }
    }

    void toggleFullscreen() {
        if (!hwnd_) return;
        if (!fullscreen_) {
            windowedStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
            windowedPlacement_.length = sizeof(windowedPlacement_);
            GetWindowPlacement(hwnd_, &windowedPlacement_);
            MONITORINFO monitorInfo{sizeof(monitorInfo)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST),
                            &monitorInfo);
            SetWindowLongW(hwnd_, GWL_STYLE, windowedStyle_ & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd_, HWND_TOP, monitorInfo.rcMonitor.left,
                         monitorInfo.rcMonitor.top,
                         monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                         monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            fullscreen_ = true;
        } else {
            SetWindowLongW(hwnd_, GWL_STYLE, windowedStyle_);
            SetWindowPlacement(hwnd_, &windowedPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            fullscreen_ = false;
        }
        if (fullscreenButton_) {
            SetWindowTextW(fullscreenButton_,
                           fullscreen_ ? labels_.exitFullscreen : labels_.fullscreen);
        }
    }

    void toggleLeftHalf() {
        leftHalf_ = !leftHalf_;
        SetWindowTextW(leftHalfButton_, leftHalf_ ? labels_.fullWidth : labels_.leftHalf);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void layoutControls() {
        if (!hwnd_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int clientBottom = static_cast<int>(client.bottom);
        const int clientRight = static_cast<int>(client.right);
        const int y = std::max(0, clientBottom - kControlHeight + 8);
        const int height = 32;
        const int playWidth = 105;
        const int fullWidth = 125;
        const int halfWidth = 125;
        const int positionWidth = 118;
        MoveWindow(playPauseButton_, 8, y, playWidth, height, TRUE);
        MoveWindow(fullscreenButton_, 8 + playWidth + 6, y, fullWidth, height, TRUE);
        MoveWindow(leftHalfButton_, 8 + playWidth + 6 + fullWidth + 6,
                   y, halfWidth, height, TRUE);
        const int positionX = 8 + playWidth + 6 + fullWidth + 6 + halfWidth + 6;
        MoveWindow(positionLabel_, positionX, y, positionWidth, height, TRUE);
        const int seekX = positionX + positionWidth + 6;
        MoveWindow(seekBar_, seekX, y,
                   std::max(80, clientRight - seekX - 8), height, TRUE);
    }

    bool ensureBackBuffer(HDC screenDc, int width, int height) {
        if (width <= 0 || height <= 0) return false;
        if (!backDc_) backDc_ = CreateCompatibleDC(screenDc);
        if (!backDc_) return false;
        if (backBitmap_ && width == backWidth_ && height == backHeight_) return true;
        if (backBitmap_) {
            SelectObject(backDc_, backDefaultBitmap_);
            DeleteObject(backBitmap_);
            backBitmap_ = nullptr;
            backDefaultBitmap_ = nullptr;
        }
        backBitmap_ = CreateCompatibleBitmap(screenDc, width, height);
        if (!backBitmap_) return false;
        backDefaultBitmap_ = SelectObject(backDc_, backBitmap_);
        backWidth_ = width;
        backHeight_ = height;
        return true;
    }

    void destroyBackBuffer() {
        if (backDc_ && backBitmap_) {
            SelectObject(backDc_, backDefaultBitmap_);
            DeleteObject(backBitmap_);
        }
        backBitmap_ = nullptr;
        backDefaultBitmap_ = nullptr;
        if (backDc_) DeleteDC(backDc_);
        backDc_ = nullptr;
        backWidth_ = 0;
        backHeight_ = 0;
    }

    void resetDrawingSurface(bool updateNow) {
        destroyBackBuffer();
        if (!hwnd_) return;
        UINT flags = RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN;
        if (updateNow) flags |= RDW_UPDATENOW;
        RedrawWindow(hwnd_, nullptr, nullptr, flags);
    }

    void refreshAfterResizeEnd() {
        if (!hwnd_) return;
        destroyBackBuffer();
        RedrawWindow(hwnd_, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOERASE | RDW_ALLCHILDREN |
                     RDW_UPDATENOW);
    }

    void queueSeekFromSlider(int position) {
        if (durationUs_ <= 0) return;
        const int clamped = std::clamp(position, 0, kSeekRange);
        seekAbsoluteUs_ = durationUs_ * clamped / kSeekRange;
        seekAbsolutePending_ = true;
        updatePositionText(seekAbsoluteUs_);
    }

    void updatePositionText(int64_t positionUs) {
        if (!positionLabel_) return;
        const int64_t positionSecond = std::max<int64_t>(0, positionUs) / AV_TIME_BASE;
        const int64_t durationSecond = std::max<int64_t>(0, durationUs_) / AV_TIME_BASE;
        if (positionSecond == displayedPositionSecond_ &&
            durationSecond == displayedDurationSecond_) return;
        displayedPositionSecond_ = positionSecond;
        displayedDurationSecond_ = durationSecond;
        const std::wstring text = formatPlayerTime(positionUs) + L" / " +
                                  formatPlayerTime(durationUs_);
        SetWindowTextW(positionLabel_, text.c_str());
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
        PlayerWindow* self = reinterpret_cast<PlayerWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<PlayerWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);
        switch (message) {
        case WM_COMMAND:
            if (LOWORD(wParam) == kPlayPauseButton) {
                self->setPaused(!self->paused_); SetFocus(hwnd); return 0;
            }
            if (LOWORD(wParam) == kFullscreenButton) {
                self->toggleFullscreen(); SetFocus(hwnd); return 0;
            }
            if (LOWORD(wParam) == kLeftHalfButton) {
                self->toggleLeftHalf(); SetFocus(hwnd); return 0;
            }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == self->seekBar_) {
                const int code = LOWORD(wParam);
                self->seekDragging_ = code == TB_THUMBTRACK;
                int position = static_cast<int>(
                    SendMessageW(self->seekBar_, TBM_GETPOS, 0, 0));
                if (code == TB_THUMBTRACK || code == TB_THUMBPOSITION) {
                    position = static_cast<int>(HIWORD(wParam));
                }
                switch (code) {
                case TB_LINEUP:
                case TB_LINEDOWN:
                case TB_PAGEUP:
                case TB_PAGEDOWN:
                case TB_THUMBPOSITION:
                case TB_THUMBTRACK:
                case TB_TOP:
                case TB_BOTTOM:
                case TB_ENDTRACK:
                    self->queueSeekFromSlider(position);
                    break;
                default:
                    break;
                }
                if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                    self->seekDragging_ = false;
                }
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_SPACE) { self->setPaused(!self->paused_); return 0; }
            if (wParam == VK_LEFT) { self->seekDeltaUs_ -= 5LL * AV_TIME_BASE; return 0; }
            if (wParam == VK_RIGHT) { self->seekDeltaUs_ += 5LL * AV_TIME_BASE; return 0; }
            if (wParam == VK_ESCAPE) {
                if (self->fullscreen_) self->toggleFullscreen();
                else DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_SIZE:
            self->layoutControls();
            if (wParam != SIZE_MINIMIZED) {
                // Moving/sizing a Win32 window enters a modal message loop.
                // Decode and presentation cannot advance during that loop, so
                // the video clock must be anchored again when playback resumes.
                self->playbackClockResetPending_ = true;
            }
            return 0;
        case WM_EXITSIZEMOVE:
            self->playbackClockResetPending_ = true;
            self->refreshAfterResizeEnd();
            return 0;
        case WM_DISPLAYCHANGE:
            self->playbackClockResetPending_ = true;
            self->resetDrawingSurface(true);
            return 0;
        case WM_SHOWWINDOW:
            if (wParam) self->resetDrawingSurface(true);
            break;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int videoBottom = std::max(
                0, static_cast<int>(client.bottom) - kControlHeight);
            const int sourceWidth = self->leftHalf_
                ? std::max(1, self->width_ / 2) : self->width_;
            const int areaWidth = std::max(1, static_cast<int>(client.right));
            const int areaHeight = std::max(1, static_cast<int>(client.bottom));
            const int videoAreaHeight = std::max(1, videoBottom);
            HDC drawDc = dc;
            if (self->ensureBackBuffer(dc, areaWidth, areaHeight)) {
                drawDc = self->backDc_;
            }
            RECT bufferArea{0, 0, static_cast<LONG>(areaWidth),
                            static_cast<LONG>(areaHeight)};
            FillRect(drawDc, &bufferArea,
                     static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            const double sourceAspect = static_cast<double>(sourceWidth) / self->height_;
            int drawWidth = areaWidth;
            int drawHeight = static_cast<int>(drawWidth / sourceAspect);
            if (drawHeight > videoAreaHeight) {
                drawHeight = videoAreaHeight;
                drawWidth = static_cast<int>(drawHeight * sourceAspect);
            }
            const int drawX = (areaWidth - drawWidth) / 2;
            const int drawY = (videoAreaHeight - drawHeight) / 2;
            SetStretchBltMode(drawDc, HALFTONE);
            StretchDIBits(drawDc, drawX, drawY, drawWidth, drawHeight,
                          0, 0, sourceWidth, self->height_, self->pixels_.data(),
                          &self->bmi_, DIB_RGB_COLORS, SRCCOPY);
            if (drawDc != dc) {
                if (!BitBlt(dc, 0, 0, areaWidth, areaHeight,
                            drawDc, 0, 0, SRCCOPY)) {
                    // If the resized compatible bitmap cannot be transferred,
                    // keep playback visible by drawing the retained frame
                    // directly to the paint DC.
                    FillRect(dc, &bufferArea,
                             static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                    SetStretchBltMode(dc, HALFTONE);
                    StretchDIBits(dc, drawX, drawY, drawWidth, drawHeight,
                                  0, 0, sourceWidth, self->height_,
                                  self->pixels_.data(), &self->bmi_,
                                  DIB_RGB_COLORS, SRCCOPY);
                }
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lParam) == self->positionLabel_) {
                SetTextColor(reinterpret_cast<HDC>(wParam), RGB(235, 235, 235));
                SetBkColor(reinterpret_cast<HDC>(wParam), RGB(0, 0, 0));
                return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            self->hwnd_ = nullptr;
            self->closed_ = true;
            return 0;
        default: break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    const PlayerLabels& labels_;
    HWND hwnd_ = nullptr;
    HWND playPauseButton_ = nullptr;
    HWND fullscreenButton_ = nullptr;
    HWND leftHalfButton_ = nullptr;
    HWND seekBar_ = nullptr;
    HWND positionLabel_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int64_t durationUs_ = 0;
    bool closed_ = false;
    bool paused_ = false;
    bool fullscreen_ = false;
    bool leftHalf_ = false;
    bool seekDragging_ = false;
    bool playbackClockResetPending_ = false;
    bool seekAbsolutePending_ = false;
    int64_t seekAbsoluteUs_ = 0;
    int64_t seekDeltaUs_ = 0;
    int64_t displayedPositionSecond_ = -1;
    int64_t displayedDurationSecond_ = -1;
    LONG windowedStyle_ = WS_OVERLAPPEDWINDOW;
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};
    BITMAPINFO bmi_{};
    std::vector<uint8_t> pixels_;
    HDC backDc_ = nullptr;
    HBITMAP backBitmap_ = nullptr;
    HGDIOBJ backDefaultBitmap_ = nullptr;
    int backWidth_ = 0;
    int backHeight_ = 0;
};

class WaveAudioOutput {
public:
    ~WaveAudioOutput() { close(); }

    bool open(std::string& error) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = kOutputChannels;
        format.nSamplesPerSec = kOutputSampleRate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        const MMRESULT result = waveOutOpen(&handle_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            error = "Video Player could not open Windows audio output. MMRESULT=" +
                    std::to_string(result);
            return false;
        }
        // Keep a larger queue than the old 12-buffer ring. High-frame-rate 4K
        // video can briefly delay demuxing while a frame is transferred and
        // converted; the deeper queue prevents those short stalls from
        // starving waveOut.
        buffers_.resize(48);
        timeBeginPeriod(1);
        timerResolutionActive_ = true;
        return true;
    }

    bool write(const uint8_t* data, size_t bytes, PlayerWindow& window,
               std::string& error) {
        if (!handle_ || bytes == 0) return true;
        Buffer& buffer = buffers_[nextBuffer_];
        while (buffer.prepared && !(buffer.header.dwFlags & WHDR_DONE)) {
            if (!window.pump() || window.seekPending()) return true;
            setPaused(window.paused());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (buffer.prepared) {
            waveOutUnprepareHeader(handle_, &buffer.header, sizeof(WAVEHDR));
            buffer.prepared = false;
        }
        buffer.data.assign(data, data + bytes);
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.data.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.data.size());
        MMRESULT result = waveOutPrepareHeader(handle_, &buffer.header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            error = "waveOutPrepareHeader failed. MMRESULT=" + std::to_string(result);
            return false;
        }
        buffer.prepared = true;
        result = waveOutWrite(handle_, &buffer.header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            error = "waveOutWrite failed. MMRESULT=" + std::to_string(result);
            return false;
        }
        nextBuffer_ = (nextBuffer_ + 1) % buffers_.size();
        return true;
    }

    void setPaused(bool paused) {
        if (!handle_ || paused_ == paused) return;
        if (paused) waveOutPause(handle_); else waveOutRestart(handle_);
        paused_ = paused;
    }

    void reset(bool startPaused) {
        if (!handle_) return;
        waveOutReset(handle_);
        for (Buffer& buffer : buffers_) {
            if (buffer.prepared) {
                waveOutUnprepareHeader(handle_, &buffer.header, sizeof(WAVEHDR));
                buffer.prepared = false;
            }
            buffer.data.clear();
        }
        nextBuffer_ = 0;
        paused_ = false;
        setPaused(startPaused);
    }

private:
    struct Buffer { WAVEHDR header{}; std::vector<uint8_t> data; bool prepared = false; };
    void close() {
        if (!handle_) return;
        reset(false);
        waveOutClose(handle_);
        handle_ = nullptr;
        if (timerResolutionActive_) {
            timeEndPeriod(1);
            timerResolutionActive_ = false;
        }
    }
    HWAVEOUT handle_ = nullptr;
    std::vector<Buffer> buffers_;
    size_t nextBuffer_ = 0;
    bool paused_ = false;
    bool timerResolutionActive_ = false;
};

struct FormatCloser { void operator()(AVFormatContext* p) const { if (p) avformat_close_input(&p); } };
struct CodecCloser { void operator()(AVCodecContext* p) const { if (p) avcodec_free_context(&p); } };
struct FrameCloser { void operator()(AVFrame* p) const { if (p) av_frame_free(&p); } };
struct PacketCloser { void operator()(AVPacket* p) const { if (p) av_packet_free(&p); } };
struct SwrCloser { void operator()(SwrContext* p) const { if (p) swr_free(&p); } };
struct BufferRefCloser {
    void operator()(AVBufferRef* p) const {
        if (p) av_buffer_unref(&p);
    }
};

AVPixelFormat chooseCudaFormat(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats;
         format && *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_CUDA) return *format;
    }
    return AV_PIX_FMT_NONE;
}

const char* cuvidDecoderName(AVCodecID codecId) {
    switch (codecId) {
    case AV_CODEC_ID_H264: return "h264_cuvid";
    case AV_CODEC_ID_HEVC: return "hevc_cuvid";
    case AV_CODEC_ID_VP8: return "vp8_cuvid";
    case AV_CODEC_ID_VP9: return "vp9_cuvid";
    case AV_CODEC_ID_AV1: return "av1_cuvid";
    case AV_CODEC_ID_MPEG1VIDEO: return "mpeg1_cuvid";
    case AV_CODEC_ID_MPEG2VIDEO: return "mpeg2_cuvid";
    case AV_CODEC_ID_VC1: return "vc1_cuvid";
    case AV_CODEC_ID_MJPEG: return "mjpeg_cuvid";
    default: return nullptr;
    }
}

bool openDecoder(AVFormatContext* format, int streamIndex,
                 std::unique_ptr<AVCodecContext, CodecCloser>& decoder,
                 std::string& error) {
    AVStream* stream = format->streams[streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) { error = "Video Player decoder not found"; return false; }
    decoder.reset(avcodec_alloc_context3(codec));
    if (!decoder) { error = "Video Player decoder allocation failed"; return false; }
    int rc = avcodec_parameters_to_context(decoder.get(), stream->codecpar);
    if (rc >= 0) rc = avcodec_open2(decoder.get(), codec, nullptr);
    if (rc < 0) {
        error = "Video Player decoder initialization: " + fferr(rc);
        return false;
    }
    return true;
}

bool openVideoDecoder(
    AVFormatContext* format, int streamIndex,
    std::unique_ptr<AVCodecContext, CodecCloser>& decoder,
    std::unique_ptr<AVBufferRef, BufferRefCloser>& cudaDevice,
    bool& usingNvdec, std::string& error) {
    usingNvdec = false;
    AVStream* stream = format->streams[streamIndex];
    const char* decoderName = cuvidDecoderName(stream->codecpar->codec_id);
    const AVCodec* hardwareCodec = decoderName
        ? avcodec_find_decoder_by_name(decoderName) : nullptr;

    if (hardwareCodec) {
        AVBufferRef* rawDevice = nullptr;
        AVDictionary* deviceOptions = nullptr;
        av_dict_set(&deviceOptions, "primary_ctx", "1", 0);
        const int deviceResult = av_hwdevice_ctx_create(
            &rawDevice, AV_HWDEVICE_TYPE_CUDA, "0", deviceOptions, 0);
        av_dict_free(&deviceOptions);
        if (deviceResult >= 0 && rawDevice) {
            std::unique_ptr<AVCodecContext, CodecCloser> candidate(
                avcodec_alloc_context3(hardwareCodec));
            int rc = candidate
                ? avcodec_parameters_to_context(candidate.get(), stream->codecpar)
                : AVERROR(ENOMEM);
            if (rc >= 0) {
                candidate->hw_device_ctx = av_buffer_ref(rawDevice);
                if (!candidate->hw_device_ctx) rc = AVERROR(ENOMEM);
            }
            if (rc >= 0) {
                candidate->get_format = chooseCudaFormat;
                candidate->pkt_timebase = stream->time_base;
                rc = avcodec_open2(candidate.get(), hardwareCodec, nullptr);
            }
            if (rc >= 0) {
                decoder = std::move(candidate);
                cudaDevice.reset(rawDevice);
                usingNvdec = true;
                return true;
            }
            av_buffer_unref(&rawDevice);
        } else if (rawDevice) {
            av_buffer_unref(&rawDevice);
        }
    }

    // NVIDIA hardware decoding is optional. Unsupported codecs, unsupported
    // profiles, systems without an NVIDIA GPU, and FFmpeg builds without CUVID
    // all continue through the ordinary software decoder.
    return openDecoder(format, streamIndex, decoder, error);
}

} // namespace

bool RunSimpleVideoPlayer(const std::filesystem::path& input,
                          int language,
                          std::string& error) {
    AVFormatContext* rawFormat = nullptr;
    const std::string inputUtf8 = input.u8string();
    int rc = avformat_open_input(&rawFormat, inputUtf8.c_str(), nullptr, nullptr);
    if (rc < 0) { error = "Video Player could not open input: " + fferr(rc); return false; }
    std::unique_ptr<AVFormatContext, FormatCloser> format(rawFormat);
    rc = avformat_find_stream_info(format.get(), nullptr);
    if (rc < 0) { error = "Video Player stream information: " + fferr(rc); return false; }

    const int videoIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) { error = "Video Player found no video stream"; return false; }
    const int audioIndex = av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
    AVStream* videoStream = format->streams[videoIndex];
    AVStream* audioStream = audioIndex >= 0 ? format->streams[audioIndex] : nullptr;

    std::unique_ptr<AVCodecContext, CodecCloser> videoDecoder;
    std::unique_ptr<AVBufferRef, BufferRefCloser> cudaDevice;
    bool usingNvdec = false;
    if (!openVideoDecoder(format.get(), videoIndex, videoDecoder, cudaDevice,
                          usingNvdec, error)) return false;
    if (videoDecoder->width <= 0 || videoDecoder->height <= 0) {
        error = "Video Player received invalid video dimensions"; return false;
    }

    std::unique_ptr<AVCodecContext, CodecCloser> audioDecoder;
    std::unique_ptr<SwrContext, SwrCloser> resampler;
    WaveAudioOutput audioOutput;
    if (audioIndex >= 0) {
        if (!openDecoder(format.get(), audioIndex, audioDecoder, error)) return false;
        AVChannelLayout sourceLayout{};
        if (audioDecoder->ch_layout.nb_channels > 0)
            av_channel_layout_copy(&sourceLayout, &audioDecoder->ch_layout);
        else
            av_channel_layout_default(&sourceLayout, 2);
        AVChannelLayout destinationLayout = AV_CHANNEL_LAYOUT_STEREO;
        SwrContext* rawResampler = nullptr;
        rc = swr_alloc_set_opts2(&rawResampler, &destinationLayout, AV_SAMPLE_FMT_S16,
            kOutputSampleRate, &sourceLayout, audioDecoder->sample_fmt,
            audioDecoder->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&sourceLayout);
        if (rc < 0 || !rawResampler) {
            error = "Video Player audio resampler allocation: " + fferr(rc); return false;
        }
        resampler.reset(rawResampler);
        rc = swr_init(resampler.get());
        if (rc < 0) { error = "Video Player audio resampler initialization: " + fferr(rc); return false; }
        if (!audioOutput.open(error)) return false;
        audioOutput.setPaused(true);
    }

    const int64_t durationUs = format->duration != AV_NOPTS_VALUE
        ? std::max<int64_t>(0, format->duration) : 0;
    const int64_t inputStartUs = format->start_time != AV_NOPTS_VALUE ? format->start_time : 0;
    PlayerWindow window(language);
    if (!window.create(input, videoDecoder->width, videoDecoder->height,
                       durationUs, usingNvdec, error)) return false;

    std::unique_ptr<AVPacket, PacketCloser> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameCloser> videoFrame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameCloser> softwareVideoFrame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameCloser> audioFrame(av_frame_alloc());
    if (!packet || !videoFrame || !softwareVideoFrame || !audioFrame) {
        error = "Video Player frame allocation failed"; return false;
    }

    SwsContext* scaler = nullptr;
    int64_t currentUs = 0;
    int64_t discardBeforeUs = 0;
    int64_t anchorPtsUs = AV_NOPTS_VALUE;
    auto anchorWall = std::chrono::steady_clock::now();
    bool resetClock = true;
    bool eof = false;
    bool audioStarted = audioIndex < 0;
    std::vector<uint8_t> audioPcm;
    auto cleanupScaler = [&]() { if (scaler) sws_freeContext(scaler); scaler = nullptr; };

    auto writeAudioFrame = [&](AVFrame* frame) -> bool {
        const int inputRate = std::max(1, audioDecoder->sample_rate);
        const int outputSamples = static_cast<int>(av_rescale_rnd(
            swr_get_delay(resampler.get(), inputRate) + frame->nb_samples,
            kOutputSampleRate, inputRate, AV_ROUND_UP));
        audioPcm.resize(static_cast<size_t>(std::max(0, outputSamples)) *
                        kOutputChannels * sizeof(int16_t));
        uint8_t* outputData[1]{audioPcm.data()};
        const AVSampleFormat inputFormat = static_cast<AVSampleFormat>(frame->format);
        const int inputPlaneCount = av_sample_fmt_is_planar(inputFormat)
            ? std::max(1, frame->ch_layout.nb_channels) : 1;
        std::vector<const uint8_t*> inputPlanes(
            static_cast<size_t>(inputPlaneCount));
        for (size_t i = 0; i < inputPlanes.size(); ++i) {
            inputPlanes[i] = frame->extended_data[i];
        }
        const int converted = swr_convert(resampler.get(), outputData, outputSamples,
            inputPlanes.data(), frame->nb_samples);
        if (converted < 0) { error = "Video Player audio conversion: " + fferr(converted); return false; }
        const size_t outputBytes = static_cast<size_t>(converted) *
            kOutputChannels * sizeof(int16_t);
        return audioOutput.write(audioPcm.data(), outputBytes, window, error);
    };

    auto drainAudio = [&]() -> bool {
        for (;;) {
            const int result = avcodec_receive_frame(audioDecoder.get(), audioFrame.get());
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return true;
            if (result < 0) { error = "Video Player audio decode: " + fferr(result); return false; }
            int64_t pts = audioFrame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = audioFrame->pts;
            const int64_t ptsUs = pts == AV_NOPTS_VALUE ? currentUs :
                av_rescale_q(pts, audioStream->time_base, AV_TIME_BASE_Q) - inputStartUs;
            if (ptsUs + 50000 >= discardBeforeUs && !writeAudioFrame(audioFrame.get())) return false;
            av_frame_unref(audioFrame.get());
            if (window.seekPending()) return true;
        }
    };

    while (window.pump()) {
        if (window.consumePlaybackClockReset()) {
            // A resize/fullscreen transition pauses this single-threaded
            // decode loop inside Win32's modal sizing loop.  Re-anchor on the
            // next decoded frame instead of dropping frames to catch up with
            // wall-clock time that elapsed while the user resized the window.
            resetClock = true;
        }
        audioOutput.setPaused(!audioStarted || window.paused());
        int64_t seekTargetUs = 0;
        if (window.consumeSeekRequest(currentUs, seekTargetUs)) {
            seekTargetUs = durationUs > 0
                ? std::clamp<int64_t>(seekTargetUs, 0, durationUs)
                : std::max<int64_t>(0, seekTargetUs);
            const int64_t absoluteTargetUs = inputStartUs + seekTargetUs;
            rc = avformat_seek_file(format.get(), -1, INT64_MIN,
                                    absoluteTargetUs, INT64_MAX,
                                    AVSEEK_FLAG_BACKWARD);
            if (rc < 0) {
                rc = av_seek_frame(format.get(), -1, absoluteTargetUs,
                                   AVSEEK_FLAG_BACKWARD);
            }
            if (rc >= 0) {
                avcodec_flush_buffers(videoDecoder.get());
                if (audioDecoder) avcodec_flush_buffers(audioDecoder.get());
                if (resampler) { swr_close(resampler.get()); swr_init(resampler.get()); }
                audioOutput.reset(true);
                currentUs = seekTargetUs;
                discardBeforeUs = seekTargetUs;
                anchorPtsUs = AV_NOPTS_VALUE;
                resetClock = true;
                audioStarted = audioIndex < 0;
                eof = false;
                window.setPosition(currentUs);
            }
        }
        if (window.paused()) {
            resetClock = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (eof) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

        rc = av_read_frame(format.get(), packet.get());
        if (rc == AVERROR_EOF) {
            avcodec_send_packet(videoDecoder.get(), nullptr);
            if (audioDecoder) avcodec_send_packet(audioDecoder.get(), nullptr);
            eof = true;
        } else if (rc < 0) {
            cleanupScaler(); error = "Video Player read error: " + fferr(rc); return false;
        } else if (packet->stream_index == videoIndex) {
            rc = avcodec_send_packet(videoDecoder.get(), packet.get());
            av_packet_unref(packet.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                cleanupScaler(); error = "Video Player video decode input: " + fferr(rc); return false;
            }
        } else if (audioDecoder && packet->stream_index == audioIndex) {
            rc = avcodec_send_packet(audioDecoder.get(), packet.get());
            av_packet_unref(packet.get());
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                cleanupScaler(); error = "Video Player audio decode input: " + fferr(rc); return false;
            }
            if (!drainAudio()) { cleanupScaler(); return false; }
            continue;
        } else {
            av_packet_unref(packet.get());
            continue;
        }

        for (;;) {
            rc = avcodec_receive_frame(videoDecoder.get(), videoFrame.get());
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) { cleanupScaler(); error = "Video Player video decode output: " + fferr(rc); return false; }
            int64_t pts = videoFrame->best_effort_timestamp;
            if (pts == AV_NOPTS_VALUE) pts = videoFrame->pts;
            const int64_t ptsUs = pts == AV_NOPTS_VALUE ? currentUs :
                av_rescale_q(pts, videoStream->time_base, AV_TIME_BASE_Q) - inputStartUs;
            if (ptsUs + 50000 < discardBeforeUs) {
                av_frame_unref(videoFrame.get());
                continue;
            }
            discardBeforeUs = 0;
            if (window.consumePlaybackClockReset()) resetClock = true;
            if (resetClock || anchorPtsUs == AV_NOPTS_VALUE) {
                anchorPtsUs = ptsUs;
                anchorWall = std::chrono::steady_clock::now();
                resetClock = false;
            }
            const auto target = anchorWall + std::chrono::microseconds(
                std::max<int64_t>(0, ptsUs - anchorPtsUs));
            const auto beforeWait = std::chrono::steady_clock::now();
            if (beforeWait > target + std::chrono::microseconds(kLateVideoDropUs)) {
                // Do not spend another full color-conversion interval on a
                // frame that is already visibly late. Catching up also lets
                // the demux loop replenish the asynchronous audio queue.
                currentUs = std::max<int64_t>(0, ptsUs);
                window.setPosition(currentUs);
                av_frame_unref(videoFrame.get());
                continue;
            }
            while (window.pump() && !window.paused() && !window.seekPending() &&
                   std::chrono::steady_clock::now() < target) {
                const auto remaining = target - std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::min(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                    std::chrono::milliseconds(10)));
            }
            // The resize can begin inside the pump above and return only after
            // the user releases the mouse.  Reset here as well so the current
            // frame becomes the new clock anchor immediately.
            if (window.consumePlaybackClockReset()) {
                anchorPtsUs = ptsUs;
                anchorWall = std::chrono::steady_clock::now();
                resetClock = false;
            }
            if (window.paused() || window.seekPending()) {
                resetClock = true;
                av_frame_unref(videoFrame.get());
                break;
            }
            AVFrame* displayFrame = videoFrame.get();
            if (videoFrame->format == AV_PIX_FMT_CUDA) {
                av_frame_unref(softwareVideoFrame.get());
                rc = av_hwframe_transfer_data(
                    softwareVideoFrame.get(), videoFrame.get(), 0);
                if (rc < 0) {
                    cleanupScaler();
                    error = "Video Player NVDEC frame transfer: " + fferr(rc);
                    return false;
                }
                rc = av_frame_copy_props(softwareVideoFrame.get(), videoFrame.get());
                if (rc < 0) {
                    cleanupScaler();
                    error = "Video Player NVDEC frame properties: " + fferr(rc);
                    return false;
                }
                displayFrame = softwareVideoFrame.get();
            }
            scaler = sws_getCachedContext(scaler, displayFrame->width, displayFrame->height,
                static_cast<AVPixelFormat>(displayFrame->format), videoDecoder->width,
                videoDecoder->height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            if (!scaler) { cleanupScaler(); error = "Video Player color conversion initialization failed"; return false; }
            uint8_t* destination[4]{window.pixels(), nullptr, nullptr, nullptr};
            int destinationStride[4]{window.stride(), 0, 0, 0};
            sws_scale(scaler, displayFrame->data, displayFrame->linesize, 0,
                      displayFrame->height, destination, destinationStride);
            currentUs = std::max<int64_t>(0, ptsUs);
            window.setPosition(currentUs);
            window.present();
            if (!audioStarted) { audioStarted = true; audioOutput.setPaused(false); }
            av_frame_unref(videoFrame.get());
        }
        if (eof && audioDecoder && !drainAudio()) { cleanupScaler(); return false; }
    }
    cleanupScaler();
    return true;
}
