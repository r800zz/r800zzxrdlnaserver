#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tchar.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "dlna_server.h"
#include "webxr_https_server.h"
#include "../resources/resource.h"

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static constexpr UINT kAiErrorUiMessage = WM_APP + 0x280;

struct AiErrorUiPayload {
    bool fatal{false};
    std::string text;
};

static std::string g_aiErrorText;
static bool g_aiErrorFatal = false;

struct DirectMlUiAdapter {
    int index{-1};
    UINT vendorId{0};
    std::string name;
};

static std::vector<DirectMlUiAdapter> EnumerateDirectMlAdapters() {
    std::vector<DirectMlUiAdapter> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr) || !adapter) continue;
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nullptr, 0, nullptr, nullptr);
            std::string name(len > 1 ? static_cast<size_t>(len - 1) : 0, '\0');
            if (len > 1) {
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name.data(), len, nullptr, nullptr);
            }
            out.push_back({static_cast<int>(i), d.VendorId, name});
        }
        adapter->Release();
    }
    factory->Release();
    return out;
}


/*
static float DrawTitleBarWebLink(const char* label, const wchar_t* url,
                                 const ImVec2& position) {
    const ImVec2 size = ImGui::CalcTextSize(label);
    const ImVec2 maximum(position.x + size.x, position.y + size.y);
    //const ImU32 color = IM_COL32(22, 55, 75, 255); // dark blue #16374B
    const ImU32 color = IM_COL32(16, 24, 32, 255); // dark blue #16374B
    ImGui::GetWindowDrawList()->AddText(position, color, label);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(position.x, maximum.y), maximum, color);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered = mouse.x >= position.x && mouse.x < maximum.x &&
                         mouse.y >= position.y && mouse.y < maximum.y;
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Open %s", label);
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }
    return maximum.x;
}
*/
static float DrawTitleBarWebLink(
    const char* label,
    const wchar_t* url,
    const ImVec2& position)
{
    const ImVec2 size = ImGui::CalcTextSize(label);
    const ImVec2 maximum(
        position.x + size.x,
        position.y + size.y);

    const ImU32 color = IM_COL32(16, 24, 32, 255);

    // Draw the text in the foreground after the title section.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    drawList->AddText(position, color, label);
    drawList->AddLine(
        ImVec2(position.x, maximum.y),
        maximum,
        color);

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool hovered =
        mouse.x >= position.x &&
        mouse.x < maximum.x &&
        mouse.y >= position.y &&
        mouse.y < maximum.y;

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Open %s", label);
    }

    if (hovered &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ShellExecuteW(
            nullptr,
            L"open",
            url,
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
    }

    return maximum.x;
}

static void CreateRenderTarget() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer) {
        g_pd3dDevice->CreateRenderTargetView(back_buffer, nullptr, &g_mainRenderTargetView);
        back_buffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT create_flags = 0;
    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, create_flags,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &feature_level, &g_pd3dDeviceContext);
    if (result == DXGI_ERROR_UNSUPPORTED) {
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, create_flags,
            levels, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &feature_level, &g_pd3dDeviceContext);
    }
    if (result != S_OK) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = static_cast<UINT>(LOWORD(lParam));
        g_ResizeHeight = static_cast<UINT>(HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case kAiErrorUiMessage: {
        auto* payload = reinterpret_cast<AiErrorUiPayload*>(lParam);
        if (payload) {
            g_aiErrorFatal = payload->fatal;
            g_aiErrorText = payload->text;
            delete payload;
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static std::filesystem::path SelectVideoFolder(
        HWND owner, bool videoFilesOnly = true) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = videoFilesOnly
        ? L"Select the folder containing video files"
        : L"Select the folder containing files to publish";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return {};
    wchar_t folder[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(item, folder);
    CoTaskMemFree(item);
    if (ok) return std::filesystem::path(folder);
    return {};
}

static std::filesystem::path SelectWebXrRootFolder(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Select the WebXR public folder";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return {};
    wchar_t folder[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(item, folder);
    CoTaskMemFree(item);
    if (ok) return std::filesystem::path(folder);
    return {};
}

static std::filesystem::path SelectVideoFile(
        HWND owner, bool videoFilesOnly = true) {
    wchar_t file[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = videoFilesOnly
        ? L"Video files\0*.mp4;*.m4v;*.webm;*.mkv;*.avi;*.mov;*.ts;*.m2ts\0"
          L"All files\0*.*\0"
        : L"All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = videoFilesOnly
        ? L"Select a video file"
        : L"Select a file to publish";
    if (GetOpenFileNameW(&dialog)) return std::filesystem::path(file);
    return {};
}

struct UiStrings {
    const char* selectFolder;
    const char* noFolder;
    const char* selectFile;
    const char* clearFiles;
    const char* videoFilesOnly;
    const char* videoPlayer;
    const char* stopPlayer;
    const char* playerRunning;
    const char* playerHint;
    const char* address;
    const char* noAddress;
    const char* refresh;
    const char* copyUrl;
    const char* aiPassthrough;
    const char* aiMode;
    const char* alpha;
    const char* chromaKey;
    const char* alphaFormat;
    const char* convertSection;
    const char* selectInput;
    const char* noInput;
    const char* conversionFormat;
    const char* conversionBackend;
    const char* conversionDevice;
    const char* convert;
    const char* cancel;
    const char* outputFrames;
    const char* startServer;
    const char* stopServer;
    const char* status;
    const char* firewall;
    const char* log;
    const char* copyLog;
    const char* clearLog;
    const char* webxrSection;
    const char* webxrRoot;
    const char* webxrOpenFolder;
    const char* webxrChangeFolder;
    const char* webxrResetFolder;
    const char* webxrStart;
    const char* webxrStop;
    const char* webxrStatus;
    const char* webxrHttps;
    const char* webxrCopyUrl;
    const char* webxrRootCa;
    const char* webxrInstallCaHint;
    const char* webxrServeHint;
    const char* webxrRunning;
    const char* webxrStopped;
};

static const UiStrings& GetUiStrings(int language) {
    static const UiStrings strings[] = {
        {"Select Folder...", "No folder selected", "Select File...", "Clear Individual Files", "Video files only",
         "Video Player...", "Stop Video Player", "Video Player: Running",
         "Audio playback; controls and seek bar are in the player window",
         "DLNA server URL:", "No LAN IPv4 address found", "Refresh IPs", "Copy URL",
         "AI Passthrough", "AI Passthrough Mode:", "Alpha", "Chroma Key", "Alpha Format",
         "Convert Video File", "Select Conversion Input...", "No conversion input selected",
         "Conversion Format", "Conversion Backend:", "Conversion GPU:", "Convert Video...", "Cancel Conversion", "Output frames",
         "Start DLNA Server", "Stop DLNA Server", "Status", "Allow access on your private network if Windows Firewall asks.",
         "Log", "Copy Log", "Log Clear",
         "WebXR HTTPS Server", "Web root:", "Open WebXR Folder", "Change WebXR Folder...", "Reset WebXR Folder", "Start WebXR HTTPS", "Stop WebXR HTTPS",
         "WebXR:", "HTTPS:", "Copy WebXR URL", "Root CA:",
         "Install the Root CA on the HMD once, then open the HTTPS URL.",
         "Serves files from the WebXR folder. Ports: 8443-8463.", "Running", "Stopped"},
        {"Выбрать папку...", "Папка не выбрана", "Выбрать файл...", "Очистить выбранные файлы", "Только видеофайлы",
         "Видеоплеер...", "Остановить видеоплеер", "Видеоплеер: работает",
         "Звук, элементы управления и полоса поиска находятся в окне плеера",
         "URL DLNA-сервера:", "IPv4-адрес LAN не найден", "Обновить IP", "Копировать URL",
         "ИИ-прозрачность", "Режим ИИ-прозрачности:", "Альфа", "Хромакей", "Формат альфа",
         "Преобразование видеофайла", "Выбрать исходное видео...", "Исходное видео не выбрано",
         "Формат преобразования", "Бэкенд преобразования:", "GPU преобразования:", "Преобразовать...", "Отменить преобразование", "Выходные кадры",
         "Запустить DLNA-сервер", "Остановить DLNA-сервер", "Состояние", "Разрешите доступ в частной сети, если спросит брандмауэр Windows.",
         "Журнал", "Копировать журнал", "Очистить журнал",
         "Сервер WebXR HTTPS", "Web-папка:", "Открыть папку WebXR", "Изменить папку WebXR...", "Сбросить папку WebXR", "Запустить WebXR HTTPS", "Остановить WebXR HTTPS",
         "WebXR:", "HTTPS:", "Копировать WebXR URL", "Корневой CA:",
         "Один раз установите корневой CA на HMD, затем откройте HTTPS URL.",
         "Файлы из папки WebXR. Порты: 8443-8463.", "Работает", "Остановлен"},
        {"Seleccionar carpeta...", "Ninguna carpeta seleccionada", "Seleccionar archivo...", "Borrar archivos individuales", "Solo archivos de vídeo",
         "Reproductor de vídeo...", "Detener reproductor", "Reproductor: en ejecución",
         "El audio, los controles y la barra de búsqueda están en la ventana del reproductor",
         "URL del servidor DLNA:", "No se encontró una dirección IPv4 LAN", "Actualizar IP", "Copiar URL",
         "Transparencia por IA", "Modo de transparencia por IA:", "Alfa", "Croma", "Formato alfa",
         "Convertir archivo de vídeo", "Seleccionar vídeo de entrada...", "No se seleccionó vídeo de entrada",
         "Formato de conversión", "Backend de conversión:", "GPU de conversión:", "Convertir vídeo...", "Cancelar conversión", "Fotogramas de salida",
         "Iniciar servidor DLNA", "Detener servidor DLNA", "Estado", "Permita el acceso a la red privada si lo solicita el Firewall de Windows.",
         "Registro", "Copiar registro", "Borrar registro",
         "Servidor HTTPS WebXR", "Raíz web:", "Abrir carpeta WebXR", "Cambiar carpeta WebXR...", "Restablecer carpeta WebXR", "Iniciar HTTPS WebXR", "Detener HTTPS WebXR",
         "WebXR:", "HTTPS:", "Copiar URL WebXR", "CA raíz:",
         "Instala una vez la CA raíz en el HMD y abre la URL HTTPS.",
         "Sirve archivos de la carpeta WebXR. Puertos: 8443-8463.", "Activo", "Detenido"},
        {"เลือกโฟลเดอร์...", "ยังไม่ได้เลือกโฟลเดอร์", "เลือกไฟล์...", "ล้างไฟล์ที่เลือก", "เฉพาะไฟล์วิดีโอ",
         "โปรแกรมเล่นวิดีโอ...", "หยุดโปรแกรมเล่น", "โปรแกรมเล่น: กำลังทำงาน",
         "เสียง ปุ่มควบคุม และแถบค้นหาอยู่ในหน้าต่างโปรแกรมเล่น",
         "URL เซิร์ฟเวอร์ DLNA:", "ไม่พบที่อยู่ IPv4 ของ LAN", "รีเฟรช IP", "คัดลอก URL",
         "AI Passthrough", "โหมด AI Passthrough:", "อัลฟา", "โครมาคีย์", "รูปแบบอัลฟา",
         "แปลงไฟล์วิดีโอ", "เลือกวิดีโอต้นฉบับ...", "ยังไม่ได้เลือกวิดีโอต้นฉบับ",
         "รูปแบบการแปลง", "แบ็กเอนด์การแปลง:", "GPU สำหรับการแปลง:", "แปลงวิดีโอ...", "ยกเลิกการแปลง", "จำนวนเฟรมเอาต์พุต",
         "เริ่มเซิร์ฟเวอร์ DLNA", "หยุดเซิร์ฟเวอร์ DLNA", "สถานะ", "อนุญาตการเข้าถึงเครือข่ายส่วนตัวหาก Windows Firewall ถาม",
         "บันทึก", "คัดลอกบันทึก", "ล้างบันทึก",
         "เซิร์ฟเวอร์ HTTPS WebXR", "โฟลเดอร์เว็บ:", "เปิดโฟลเดอร์ WebXR", "เปลี่ยนโฟลเดอร์ WebXR...", "รีเซ็ตโฟลเดอร์ WebXR", "เริ่ม HTTPS WebXR", "หยุด HTTPS WebXR",
         "WebXR:", "HTTPS:", "คัดลอก URL WebXR", "Root CA:",
         "ติดตั้ง Root CA บน HMD ครั้งเดียว แล้วเปิด HTTPS URL",
         "ให้บริการไฟล์จากโฟลเดอร์ WebXR พอร์ต: 8443-8463", "ทำงาน", "หยุด"},
        {"选择文件夹...", "未选择文件夹", "选择文件...", "清除单独选择的文件", "仅视频文件",
         "视频播放器...", "停止视频播放器", "视频播放器：运行中",
         "声音、控制按钮和进度条位于播放器窗口内",
         "DLNA 服务器 URL：", "未找到局域网 IPv4 地址", "刷新 IP", "复制 URL",
         "AI 透视", "AI 透视模式：", "Alpha", "色键", "Alpha 格式",
         "转换视频文件", "选择转换输入...", "未选择转换输入",
         "转换格式", "转换后端：", "转换 GPU：", "转换视频...", "取消转换", "输出帧数",
         "启动 DLNA 服务器", "停止 DLNA 服务器", "状态", "如果 Windows 防火墙询问，请允许专用网络访问。",
         "日志", "复制日志", "清除日志",
         "WebXR HTTPS 服务器", "Web 根目录:", "打开 WebXR 文件夹", "更改 WebXR 文件夹...", "重置 WebXR 文件夹", "启动 WebXR HTTPS", "停止 WebXR HTTPS",
         "WebXR:", "HTTPS:", "复制 WebXR URL", "根 CA:",
         "在 HMD 上安装一次根 CA，然后打开 HTTPS URL。",
         "提供 WebXR 文件夹中的文件。端口: 8443-8463。", "运行中", "已停止"},
        {"폴더 선택...", "폴더를 선택하지 않음", "파일 선택...", "개별 파일 지우기", "비디오 파일만",
         "비디오 플레이어...", "비디오 플레이어 중지", "비디오 플레이어: 실행 중",
         "오디오, 조작 버튼 및 탐색 막대는 플레이어 창 안에 있습니다",
         "DLNA 서버 URL:", "LAN IPv4 주소를 찾지 못함", "IP 새로 고침", "URL 복사",
         "AI 패스스루", "AI 패스스루 모드:", "알파", "크로마 키", "알파 형식",
         "비디오 파일 변환", "변환 입력 선택...", "변환 입력을 선택하지 않음",
         "변환 형식", "변환 백엔드:", "변환 GPU:", "비디오 변환...", "변환 취소", "출력 프레임",
         "DLNA 서버 시작", "DLNA 서버 중지", "상태", "Windows 방화벽이 요청하면 개인 네트워크 액세스를 허용하십시오.",
         "로그", "로그 복사", "로그 지우기",
         "WebXR HTTPS 서버", "웹 루트:", "WebXR 폴더 열기", "WebXR 폴더 변경...", "WebXR 폴더 초기화", "WebXR HTTPS 시작", "WebXR HTTPS 중지",
         "WebXR:", "HTTPS:", "WebXR URL 복사", "루트 CA:",
         "HMD에 루트 CA를 한 번 설치한 뒤 HTTPS URL을 여세요.",
         "WebXR 폴더의 파일을 제공합니다. 포트: 8443-8463.", "실행 중", "중지됨"},
        {"フォルダーを選択...", "フォルダー未選択", "ファイルを選択...", "個別選択を消去", "動画ファイルのみ",
         "ビデオプレイヤー...", "ビデオプレイヤーを停止", "ビデオプレイヤー：実行中",
         "音声・操作ボタン・シークバーはプレイヤーウィンドウ内にあります",
         "DLNAサーバーURL：", "LANのIPv4アドレスがありません", "IPを更新", "URLをコピー",
         "AIパススルー", "AIパススルーモード：", "アルファ", "クロマキー", "アルファ形式",
         "動画ファイル変換", "変換元動画を選択...", "変換元動画未選択",
         "変換形式", "変換バックエンド：", "変換GPU：", "動画を変換...", "変換を中止", "出力フレーム数",
         "DLNAサーバーを開始", "DLNAサーバーを停止", "状態", "Windowsファイアウォールに表示された場合は、プライベートネットワークへのアクセスを許可してください。",
         "ログ", "ログをコピー", "ログを消去",
         "WebXR HTTPSサーバー", "Webルート:", "WebXRフォルダーを開く", "WebXRフォルダーを変更...", "WebXRフォルダーをリセット", "WebXR HTTPS開始", "WebXR HTTPS停止",
         "WebXR:", "HTTPS:", "WebXR URLをコピー", "ルートCA:",
         "HMDにルートCAを一度インストールし、HTTPS URLを開いてください。",
         "WebXRフォルダー内のファイルを配信します。ポート: 8443-8463。", "実行中", "停止"},
    };
    return strings[std::clamp(language, 0, 6)];
}

static void ConfigureMultilingualFonts(ImGuiIO& io) {
    io.Fonts->AddFontDefault();
    ImFontConfig merge{};
    merge.MergeMode = true;
    merge.PixelSnapH = true;
    const auto add = [&](const char* path, const ImWchar* ranges, int fontNo = 0) {
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(path))) return;
        ImFontConfig config = merge;
        config.FontNo = fontNo;
        io.Fonts->AddFontFromFileTTF(path, 16.0f, &config, ranges);
    };
    add("C:/Windows/Fonts/segoeui.ttf", io.Fonts->GetGlyphRangesCyrillic());
    add("C:/Windows/Fonts/leelawui.ttf", io.Fonts->GetGlyphRangesThai());
    add("C:/Windows/Fonts/msyh.ttc", io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    add("C:/Windows/Fonts/malgun.ttf", io.Fonts->GetGlyphRangesKorean());
    add("C:/Windows/Fonts/meiryo.ttc", io.Fonts->GetGlyphRangesJapanese());
}

static const wchar_t* ConversionModeName(AiOutputMode mode) {
    if (mode == AiOutputMode::ChromaKeyHevc) return L"chroma-key";
    if (mode == AiOutputMode::WebmVp9Alpha) return L"webm-alpha";
    return L"alpha-packed";
}

static std::filesystem::path SelectConversionOutput(
    HWND owner, const std::filesystem::path& input, AiOutputMode mode) {
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    wchar_t timestamp[32]{};
    swprintf_s(timestamp, L"_%04u%02u%02u%02u%02u",
               static_cast<unsigned>(localTime.wYear),
               static_cast<unsigned>(localTime.wMonth),
               static_cast<unsigned>(localTime.wDay),
               static_cast<unsigned>(localTime.wHour),
               static_cast<unsigned>(localTime.wMinute));
    std::filesystem::path suggested = input.parent_path();
    if (mode == AiOutputMode::WebmVp9Alpha) {
        suggested /= input.stem().wstring() + L"_RVM_Alpha" + timestamp + L".webm";
    } else if (mode == AiOutputMode::ChromaKeyHevc) {
        suggested /= input.stem().wstring() + L"_RVM_ChromaKey" + timestamp + L".mp4";
    } else {
        suggested /= input.stem().wstring() + L"_RVM_AlphaPacked" + timestamp + L".mp4";
    }

    wchar_t file[32768]{};
    const std::wstring suggestedText = suggested.wstring();
    wcsncpy_s(file, std::size(file), suggestedText.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = mode == AiOutputMode::WebmVp9Alpha
        ? L"WebM video (*.webm)\0*.webm\0All files\0*.*\0"
        : L"MP4 video (*.mp4)\0*.mp4\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.lpstrDefExt = mode == AiOutputMode::WebmVp9Alpha ? L"webm" : L"mp4";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR |
                   OFN_OVERWRITEPROMPT;
    dialog.lpstrTitle = L"Save converted video";
    if (GetSaveFileNameW(&dialog)) return std::filesystem::path(file);
    return {};
}

static std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

static std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

static constexpr const char* kLanguageCodes[] = {
    "en", "ru", "es", "th", "zh", "ko", "ja"
};

// Country codes used by the linked websites.  Chinese, Korean and Japanese
// differ from their UI language codes; the other codes remain unchanged.
static constexpr const wchar_t* kWebLanguageCodes[] = {
    L"en", L"ru", L"es", L"th", L"cn", L"kr", L"jp"
};

static constexpr const wchar_t* kGitHubReadmeFiles[] = {
    L"README.md", L"README_ru.md", L"README_es.md", L"README_th.md",
    L"README_zh.md", L"README_ko.md", L"README_ja.md"
};

static std::filesystem::path SettingsFilePath() {
    PWSTR localAppData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData);
    if (FAILED(result) || !localAppData) return {};
    std::filesystem::path path(localAppData);
    CoTaskMemFree(localAppData);
    return path / L"R800ZZ" / L"AI Passthrough DLNA Server" /
           L"settings.json";
}

static std::filesystem::path MediaSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"media_selection.txt";
}

static std::filesystem::path AiBackendSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"ai_backend.txt";
}

static int LoadAiBackendSelection() {
    const std::filesystem::path path = AiBackendSelectionFilePath();
    if (path.empty()) return 0;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return 0;

    std::string value;
    std::getline(stream, value);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    if (value == "dml") return 1;
    if (value == "cpu") return 2;
    return 0;
}

static bool SaveAiBackendSelection(int backend) {
    if (backend < 0 || backend > 2) return false;

    const std::filesystem::path path = AiBackendSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    const char* value = backend == 1 ? "dml" : (backend == 2 ? "cpu" : "cuda");
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << value << "\n";
        stream.flush();
        if (!stream) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static std::filesystem::path DirectMlAdapterSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"directml_adapter.txt";
}

struct SavedDirectMlAdapterSelection {
    int adapterIndex{-1};
    std::string name;
    bool valid{false};
};

static SavedDirectMlAdapterSelection LoadDirectMlAdapterSelection() {
    SavedDirectMlAdapterSelection result;
    const std::filesystem::path path = DirectMlAdapterSelectionFilePath();
    if (path.empty()) return result;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return result;

    std::string indexText;
    if (!std::getline(stream, indexText)) return result;
    if (!indexText.empty() && indexText.back() == '\r') indexText.pop_back();

    char* end = nullptr;
    const long parsed = std::strtol(indexText.c_str(), &end, 10);
    if (end == indexText.c_str() || *end != '\0' || parsed < 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        return result;
    }

    if (!std::getline(stream, result.name)) return result;
    if (!result.name.empty() && result.name.back() == '\r') result.name.pop_back();
    if (result.name.empty()) return result;

    result.adapterIndex = static_cast<int>(parsed);
    result.valid = true;
    return result;
}

static bool SaveDirectMlAdapterSelection(
        const std::vector<DirectMlUiAdapter>& adapters,
        int choice) {
    if (choice < 0 || choice >= static_cast<int>(adapters.size())) return false;

    const std::filesystem::path path = DirectMlAdapterSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    const auto& adapter = adapters[static_cast<size_t>(choice)];
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << adapter.index << "\n" << adapter.name << "\n";
        stream.flush();
        if (!stream) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static std::filesystem::path ConversionBackendSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"conversion_backend.txt";
}

static int LoadConversionBackendSelection() {
    const std::filesystem::path path = ConversionBackendSelectionFilePath();
    if (path.empty()) return 0;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return 0;

    std::string value;
    std::getline(stream, value);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    if (value == "dml") return 1;
    if (value == "cpu") return 2;
    return 0;
}

static bool SaveConversionBackendSelection(int backend) {
    if (backend < 0 || backend > 2) return false;
    const std::filesystem::path path = ConversionBackendSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    const char* value = backend == 1 ? "dml" : (backend == 2 ? "cpu" : "cuda");
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << value << "\n";
        stream.flush();
        if (!stream) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static std::filesystem::path ConversionDirectMlAdapterSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"conversion_directml_adapter.txt";
}

static SavedDirectMlAdapterSelection LoadConversionDirectMlAdapterSelection() {
    SavedDirectMlAdapterSelection result;
    const std::filesystem::path path = ConversionDirectMlAdapterSelectionFilePath();
    if (path.empty()) return result;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return result;

    std::string indexText;
    if (!std::getline(stream, indexText)) return result;
    if (!indexText.empty() && indexText.back() == '\r') indexText.pop_back();

    char* end = nullptr;
    const long parsed = std::strtol(indexText.c_str(), &end, 10);
    if (end == indexText.c_str() || *end != '\0' || parsed < 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        return result;
    }
    if (!std::getline(stream, result.name)) return result;
    if (!result.name.empty() && result.name.back() == '\r') result.name.pop_back();
    if (result.name.empty()) return result;

    result.adapterIndex = static_cast<int>(parsed);
    result.valid = true;
    return result;
}

static bool SaveConversionDirectMlAdapterSelection(
        const std::vector<DirectMlUiAdapter>& adapters,
        int choice) {
    if (choice < 0 || choice >= static_cast<int>(adapters.size())) return false;
    const std::filesystem::path path = ConversionDirectMlAdapterSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    const auto& adapter = adapters[static_cast<size_t>(choice)];
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << adapter.index << "\n" << adapter.name << "\n";
        stream.flush();
        if (!stream) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static std::filesystem::path AiStreamModeSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"ai_passthrough_mode.txt";
}

static int LoadAiStreamModeSelection() {
    const std::filesystem::path path = AiStreamModeSelectionFilePath();
    if (path.empty()) return -1;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return -1;

    std::string value;
    std::getline(stream, value);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    if (value == "alpha-packed") return 0;
    if (value == "webm-alpha") return 1;
    if (value == "chroma-key") return 2;
    if (value == "off") return 3;
    return -1;
}

static bool SaveAiStreamModeSelection(int mode) {
    if (mode < 0 || mode > 3) return false;

    const std::filesystem::path path = AiStreamModeSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    const char* value = mode == 0 ? "alpha-packed" :
        (mode == 1 ? "webm-alpha" : (mode == 2 ? "chroma-key" : "off"));
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << value << "\n";
        stream.flush();
        if (!stream) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static std::filesystem::path WebXrRootSelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"webxr_root.txt";
}

static std::filesystem::path LoadWebXrRootSelection() {
    const std::filesystem::path path = WebXrRootSelectionFilePath();
    if (path.empty()) return {};

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};

    std::string value;
    std::getline(stream, value);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    if (value.empty()) return {};

    const std::wstring wideValue = Utf8ToWide(value);
    if (wideValue.empty()) return {};

    const std::filesystem::path candidate(wideValue);
    std::error_code filesystemError;
    if (!std::filesystem::is_directory(candidate, filesystemError) ||
        filesystemError) {
        return {};
    }

    std::filesystem::path normalized =
        std::filesystem::absolute(candidate, filesystemError).lexically_normal();
    if (filesystemError) return candidate.lexically_normal();
    return normalized;
}

static bool SaveWebXrRootSelection(const std::filesystem::path& root) {
    if (root.empty()) return false;

    const std::filesystem::path path = WebXrRootSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << WideToUtf8(root.wstring()) << "\n";
        stream.flush();
        if (!stream) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static bool ClearWebXrRootSelection() {
    const std::filesystem::path path = WebXrRootSelectionFilePath();
    if (path.empty()) return false;
    if (DeleteFileW(path.c_str())) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

static std::filesystem::path VideoFilesOnlySelectionFilePath() {
    const std::filesystem::path settings = SettingsFilePath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"video_files_only.txt";
}

static bool LoadVideoFilesOnlySelection() {
    const std::filesystem::path path = VideoFilesOnlySelectionFilePath();
    if (path.empty()) return true;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return true;

    std::string value;
    std::getline(stream, value);
    if (!value.empty() && value.back() == '\r') value.pop_back();
    return value != "off";
}

static bool SaveVideoFilesOnlySelection(bool enabled) {
    const std::filesystem::path path = VideoFilesOnlySelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << (enabled ? "on\n" : "off\n");
        stream.flush();
        if (!stream) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static bool SaveMediaSelection(
        const std::filesystem::path& folder,
        const std::vector<std::filesystem::path>& individualFiles) {
    const std::filesystem::path path = MediaSelectionFilePath();
    if (path.empty()) return false;

    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        if (!folder.empty()) {
            stream << "F " << WideToUtf8(folder.wstring()) << "\n";
        }
        for (const auto& file : individualFiles) {
            if (!file.empty()) {
                stream << "I " << WideToUtf8(file.wstring()) << "\n";
            }
        }
        stream.flush();
        if (!stream) return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static void LoadMediaSelection(
        std::filesystem::path& folder,
        std::vector<std::filesystem::path>& individualFiles) {
    folder.clear();
    individualFiles.clear();

    const std::filesystem::path path = MediaSelectionFilePath();
    if (path.empty()) return;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return;

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 3 || line[1] != ' ') continue;

        const std::wstring widePath = Utf8ToWide(line.substr(2));
        if (widePath.empty()) continue;
        const std::filesystem::path candidate(widePath);

        std::error_code filesystemError;
        if (line[0] == 'F') {
            if (folder.empty() &&
                std::filesystem::is_directory(candidate, filesystemError) &&
                !filesystemError) {
                folder = candidate;
            }
        } else if (line[0] == 'I') {
            if (std::filesystem::is_regular_file(candidate, filesystemError) &&
                !filesystemError) {
                const auto duplicate = std::find_if(
                    individualFiles.begin(), individualFiles.end(),
                    [&](const std::filesystem::path& existing) {
                        return _wcsicmp(existing.wstring().c_str(),
                                        candidate.wstring().c_str()) == 0;
                    });
                if (duplicate == individualFiles.end()) {
                    individualFiles.push_back(candidate);
                }
            }
        }
    }
}

static int LoadUiLanguage() {
    const std::filesystem::path path = SettingsFilePath();
    if (path.empty()) return 0;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return 0;
    const std::string json((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    const size_t key = json.find("\"language\"");
    if (key == std::string::npos) return 0;
    const size_t colon = json.find(':', key + 10);
    const size_t quote = colon == std::string::npos
        ? std::string::npos : json.find('"', colon + 1);
    const size_t endQuote = quote == std::string::npos
        ? std::string::npos : json.find('"', quote + 1);
    if (quote == std::string::npos || endQuote == std::string::npos) return 0;
    const std::string code = json.substr(quote + 1, endQuote - quote - 1);
    for (int language = 0; language < static_cast<int>(std::size(kLanguageCodes));
         ++language) {
        if (code == kLanguageCodes[language]) return language;
    }
    return 0;
}

static bool SaveUiLanguage(int language) {
    if (language < 0 || language >= static_cast<int>(std::size(kLanguageCodes))) {
        return false;
    }
    const std::filesystem::path path = SettingsFilePath();
    if (path.empty()) return false;
    std::error_code filesystemError;
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError) return false;

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return false;
        stream << "{\n  \"language\": \"" << kLanguageCodes[language]
               << "\"\n}\n";
        stream.flush();
        if (!stream) return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

static std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

static std::wstring QuoteWindowsArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

struct VideoPlayerProcess {
    HANDLE process = nullptr;
    DWORD lastExitCode = 0;

    ~VideoPlayerProcess() { Stop(); }

    bool IsRunning() {
        if (!process) return false;
        DWORD code = 0;
        if (!GetExitCodeProcess(process, &code)) {
            CloseHandle(process);
            process = nullptr;
            lastExitCode = GetLastError();
            return false;
        }
        if (code == STILL_ACTIVE) return true;
        lastExitCode = code;
        CloseHandle(process);
        process = nullptr;
        return false;
    }

    bool Start(const std::filesystem::path& input, int language) {
        Stop();
        const auto dir = ExecutableDirectory();
        const auto worker = dir / L"r800zz_ai_worker.exe";
        if (!std::filesystem::is_regular_file(worker)) return false;

        std::wstring command =
            QuoteWindowsArgument(worker.wstring()) + L" " +
            QuoteWindowsArgument(input.wstring()) + L" --video-player --player-language " +
            std::to_wstring(std::clamp(language, 0, 6));
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = nul != INVALID_HANDLE_VALUE ? nul : GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = nul != INVALID_HANDLE_VALUE ? nul : GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(
            nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, dir.wstring().c_str(), &startup, &pi);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            lastExitCode = GetLastError();
            return false;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        lastExitCode = STILL_ACTIVE;
        return true;
    }

    void Stop() {
        if (!process) return;
        DWORD code = 0;
        if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
        }
        GetExitCodeProcess(process, &lastExitCode);
        CloseHandle(process);
        process = nullptr;
    }
};

struct ConversionProcess {
    HANDLE process = nullptr;
    DWORD lastExitCode = 0;
    bool completed = false;
    AiBackend backend = AiBackend::NvidiaCuda;
    int directmlDevice = 0;
    std::filesystem::path output;
    std::filesystem::path logFile;
    std::string error;
    uint64_t outputFrames = 0;
    std::chrono::steady_clock::time_point lastProgressRead{};

    void UpdateProgressFromLog(bool force = false) {
        if (logFile.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastProgressRead.time_since_epoch().count() != 0 &&
            now - lastProgressRead < std::chrono::milliseconds(200)) {
            return;
        }
        lastProgressRead = now;
        std::ifstream stream(logFile, std::ios::binary);
        if (!stream) return;
        const std::string contents(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
        constexpr const char* marker = "PROGRESS frames=";
        const size_t position = contents.rfind(marker);
        if (position == std::string::npos) return;
        const char* number = contents.c_str() + position + std::char_traits<char>::length(marker);
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(number, &end, 10);
        if (end != number) outputFrames = static_cast<uint64_t>(parsed);
    }

    static std::filesystem::path PartialOutputPath(
            const std::filesystem::path& finalOutput) {
        auto partial = finalOutput;
        partial += L".r800zz-part";
        return partial;
    }

    void RemovePartialOutput() const {
        if (output.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(PartialOutputPath(output), ignored);
    }

    ~ConversionProcess() { Stop(false); }

    bool IsRunning() {
        if (!process) return false;
        UpdateProgressFromLog();
        DWORD code = 0;
        if (!GetExitCodeProcess(process, &code)) {
            lastExitCode = GetLastError();
            error = "Could not read conversion process status. Win32=" +
                    std::to_string(lastExitCode);
            CloseHandle(process);
            process = nullptr;
            completed = true;
            return false;
        }
        if (code == STILL_ACTIVE) return true;
        UpdateProgressFromLog(true);
        lastExitCode = code;
        CloseHandle(process);
        process = nullptr;
        completed = true;
        if (code != 0) {
            RemovePartialOutput();
            error = "Conversion failed. Exit code=" + std::to_string(code) +
                    ". The final output was not replaced. See log: " +
                    WideToUtf8(logFile.wstring());
        }
        else{
          //if convert OK then delete convert log file
          std::error_code ec;
          std::filesystem::remove(logFile, ec);
          logFile.clear();
        }
        return false;
    }

    bool Start(const std::filesystem::path& input,
               const std::filesystem::path& outputFile,
               AiOutputMode mode,
               AiBackend selectedBackend,
               int selectedDirectMlDevice) {
        if (IsRunning()) return false;
        completed = false;
        lastExitCode = STILL_ACTIVE;
        error.clear();
        outputFrames = 0;
        lastProgressRead = {};
        output = outputFile;
        logFile = outputFile;
        logFile += L".conversion.log";
        backend = selectedBackend;
        directmlDevice = selectedDirectMlDevice;

        const auto dir = ExecutableDirectory();
        const std::filesystem::path application = dir / L"r800zz_ai_worker.exe";
        const std::filesystem::path model = dir /
            (selectedBackend == AiBackend::NvidiaCuda
                ? L"rvm_mobilenetv3_fp16.onnx"
                : L"rvm_mobilenetv3_fp32.onnx");
        if (!std::filesystem::is_regular_file(application) ||
            !std::filesystem::is_regular_file(model)) {
            error = selectedBackend == AiBackend::NvidiaCuda
                ? "CUDA worker or FP16 RVM model is missing beside the server executable."
                : "Worker or FP32 RVM model is missing beside the server executable.";
            return false;
        }

        std::wstring command =
            QuoteWindowsArgument(application.wstring()) + L" " +
            QuoteWindowsArgument(input.wstring());

        if (selectedBackend == AiBackend::NvidiaCuda) {
            // Keep the existing CUDA offline-conversion command unchanged.
            command += L" --model " + QuoteWindowsArgument(model.wstring()) +
                       L" --device 0 --qp 20 --start-ms 0 --output-mode " +
                       std::wstring(ConversionModeName(mode)) +
                       L" --convert-output " +
                       QuoteWindowsArgument(outputFile.wstring()) +
                       L" --no-preview";
        } else {
            // DirectML/CPU offline conversion uses the DML backend parser:
            //   worker <input> <output> --backend dml|cpu ...
            // The positional output argument enables offlineConvert there.
            command += L" " + QuoteWindowsArgument(outputFile.wstring()) +
                       L" --backend " +
                       (selectedBackend == AiBackend::DirectML ? L"dml" : L"cpu") +
                       L" --model " + QuoteWindowsArgument(model.wstring());
            if (selectedBackend == AiBackend::DirectML) {
                command += L" --device " +
                           std::to_wstring(selectedDirectMlDevice);
            }
            command += L" --qp 20 --start-ms 0 --output-mode " +
                       std::wstring(ConversionModeName(mode)) +
                       L" --no-preview";
        }

        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE log = CreateFileW(
            logFile.wstring().c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) {
            lastExitCode = GetLastError();
            error = "Could not create conversion log. Win32=" +
                    std::to_string(lastExitCode);
            return false;
        }
        HANDLE nul = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nul != INVALID_HANDLE_VALUE
            ? nul : GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = log;
        startup.hStdError = log;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(
            application.wstring().c_str(), mutableCommand.data(),
            nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, dir.wstring().c_str(), &startup, &pi);
        CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            lastExitCode = GetLastError();
            error = "Could not start conversion. Win32=" +
                    std::to_string(lastExitCode);
            return false;
        }
        CloseHandle(pi.hThread);
        process = pi.hProcess;
        return true;
    }

    void Stop(bool markCanceled = true) {
        if (!process) return;
        DWORD code = 0;
        if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 2000);
        }
        GetExitCodeProcess(process, &lastExitCode);
        CloseHandle(process);
        process = nullptr;
        RemovePartialOutput();
        completed = markCanceled;
        if (markCanceled) {
            error = "Conversion canceled. The final output was not replaced.";
        }
    }
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleInstanceMutex = CreateMutexW(
        nullptr,
        TRUE,
        L"Local\\R800ZZ_XR_DLNA_SERVER_SINGLE_INSTANCE");

    if (!singleInstanceMutex) {
        return 1;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"r800zzXRdlnaServer is already running.",
            L"r800zzXRdlnaServer",
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(singleInstanceMutex);
        return 0;
    }

    SetProcessDPIAware();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, instance, nullptr, nullptr, nullptr, nullptr,
                   L"R800ZZDlnaServerWindow", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName,
                              L"r800zzXRdlnaServer 0.8",
                              WS_OVERLAPPEDWINDOW, 100, 100, 820, 720,
                              nullptr, nullptr, wc.hInstance, nullptr);

    HICON largeIcon = static_cast<HICON>(
        LoadImageW(
            instance,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            32,
            32,
            LR_DEFAULTCOLOR
        )
    );
    
    HICON smallIcon = static_cast<HICON>(
        LoadImageW(
            instance,
            MAKEINTRESOURCEW(IDI_APP_ICON),
            IMAGE_ICON,
            16,
            16,
            LR_DEFAULTCOLOR
        )
    );
    
    SendMessageW(
        hwnd,
        WM_SETICON,
        ICON_BIG,
        reinterpret_cast<LPARAM>(largeIcon)
    );
    
    SendMessageW(
        hwnd,
        WM_SETICON,
        ICON_SMALL,
        reinterpret_cast<LPARAM>(smallIcon)
    );

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ConfigureMultilingualFonts(io);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    //const ImVec4 backgroundColor(241.0f / 255.0f, 204.0f / 255.0f, 165.0f / 255.0f, 1.0f); // #F1CCA5
    const ImVec4 backgroundColor(0xff / 250.0f, 0xef / 255.0f, 0xd5 / 255.0f, 1.0f);
    const ImVec4 serverRunningBackgroundColor(
        0xff / 255.0f, 0xc0 / 255.0f, 0xcb / 255.0f, 1.0f); // #FFC0CB
    style.Colors[ImGuiCol_WindowBg] = backgroundColor;
    // Sky-blue UI surfaces requested for the title bar, unchecked radio/
    // framed controls, and the log/list area.  Keep the main window color
    // independent so this does not recolor unrelated parts of the UI.
    style.Colors[ImGuiCol_ChildBg] = ImVec4(216.0f / 255.0f, 239.0f / 255.0f,
                                            250.0f / 255.0f, 1.0f); // #D8EFFA
    style.Colors[ImGuiCol_PopupBg] = style.Colors[ImGuiCol_ChildBg];
    style.Colors[ImGuiCol_Text] = ImVec4(0.16f, 0.09f, 0.04f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.40f, 0.28f, 0.18f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(120.0f / 255.0f, 200.0f / 255.0f,
                                            240.0f / 255.0f, 1.0f); // #78C8F0
    //style.Colors[ImGuiCol_TitleBgActive] = ImVec4(84.0f / 255.0f, 181.0f / 255.0f,
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0xff / 255.0f, 0xe4 / 255.0f, 0xe1 / 255.0f , 1.0f); 
    style.Colors[ImGuiCol_TitleBgCollapsed] = style.Colors[ImGuiCol_TitleBg];
    style.Colors[ImGuiCol_FrameBg] = ImVec4(184.0f / 255.0f, 222.0f / 255.0f,
                                            242.0f / 255.0f, 1.0f); // #B8DEF2
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(152.0f / 255.0f, 207.0f / 255.0f,
                                                   236.0f / 255.0f, 1.0f); // #98CFEC
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(120.0f / 255.0f, 192.0f / 255.0f,
                                                  232.0f / 255.0f, 1.0f); // #78C0E8

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    DlnaServer server;
    server.SetAiErrorCallback([hwnd](const AiErrorInfo& info) {
        // The AI Error UI is reserved for failures that stop AI passthrough.
        // Compatibility fallbacks and other recoverable diagnostics remain in
        // the server log but are not surfaced as AI Error.
        if (info.severity != "FATAL") {
            return;
        }

        auto* payload = new AiErrorUiPayload();
        payload->fatal = info.severity == "FATAL";
        payload->text =
            "GPU: " + info.gpu +
            " | Mode: " + info.mode +
            " | Codec: " + info.codec +
            " | " + info.severity + ": " + info.message;
        if (!PostMessageW(
                hwnd, kAiErrorUiMessage, 0,
                reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    });
    WebXrHttpsServer webxrServer;
    const std::filesystem::path savedWebXrRoot = LoadWebXrRootSelection();
    if (!savedWebXrRoot.empty()) {
        webxrServer.SetRootDirectory(savedWebXrRoot);
    }
    VideoPlayerProcess videoPlayerProcess;
    ConversionProcess conversionProcess;
    int ai_backend = LoadAiBackendSelection(); // 0=NVIDIA CUDA, 1=DirectML, 2=CPU
    const int saved_ai_stream_mode = LoadAiStreamModeSelection();
    std::future<AiCapabilityResult> aiCapabilityFuture;
    bool aiCapabilityProbeStarted = false;
    bool aiCapabilityChecked = false;
    bool aiCapabilityAvailable = false;
    std::string aiCapabilityMessage = "NVIDIA CUDA capability check not started.";
    auto startCudaCapabilityProbe = [&]() {
        if (aiCapabilityProbeStarted || aiCapabilityChecked) return;
        aiCapabilityProbeStarted = true;
        aiCapabilityMessage = "Checking NVIDIA GPU support...";
        aiCapabilityFuture = std::async(
            std::launch::async, []() {
                try {
                    return DlnaServer::ProbeAiCapability();
                } catch (const std::exception& error) {
                    return AiCapabilityResult{
                        false,
                        std::string("GPU capability check failed: ") + error.what() +
                            ". Realtime AI passthrough is unavailable."
                    };
                }
            });
    };
    bool video_files_only = LoadVideoFilesOnlySelection();
    std::filesystem::path selected_folder;
    std::vector<std::filesystem::path> folder_files;
    std::vector<std::filesystem::path> individually_selected_files;
    std::vector<std::filesystem::path> active_individually_selected_files;
    std::vector<std::filesystem::path> selected_files;
    std::filesystem::path first_selected_video;
    auto rebuildSelectedFiles = [&]() {
        selected_files = folder_files;
        active_individually_selected_files.clear();
        for (const auto& file : individually_selected_files) {
            if (!DlnaServer::IsShareableFile(file, video_files_only)) continue;
            active_individually_selected_files.push_back(file);
            const auto duplicate = std::find_if(
                selected_files.begin(), selected_files.end(),
                [&](const std::filesystem::path& existing) {
                    return _wcsicmp(existing.wstring().c_str(),
                                    file.wstring().c_str()) == 0;
                });
            if (duplicate == selected_files.end()) selected_files.push_back(file);
        }

        first_selected_video.clear();
        for (const auto& file : selected_files) {
            if (DlnaServer::IsMediaFile(file)) {
                first_selected_video = file;
                break;
            }
        }
    };
    LoadMediaSelection(selected_folder, individually_selected_files);
    if (!selected_folder.empty()) {
        folder_files = DlnaServer::FindFiles(
            selected_folder, video_files_only);
    }
    rebuildSelectedFiles();
    std::vector<std::string> addresses = DlnaServer::GetLocalIPv4Addresses();
    int selected_address = addresses.empty() ? -1 : 0;
    // 0=Alpha packed, 1=Alpha WebM VP9, 2=Chroma Key, 3=OFF.
    bool ai_stream_mode_has_saved_or_user_value = saved_ai_stream_mode >= 0;
    int ai_stream_mode = ai_stream_mode_has_saved_or_user_value
        ? saved_ai_stream_mode : 3;
    int previous_ai_backend = ai_backend;
    auto directml_adapters = EnumerateDirectMlAdapters();
    int directml_adapter_choice = directml_adapters.empty() ? -1 : 0;
    if (!directml_adapters.empty()) {
        const SavedDirectMlAdapterSelection savedAdapter =
            LoadDirectMlAdapterSelection();
        if (savedAdapter.valid) {
            const auto match = std::find_if(
                directml_adapters.begin(), directml_adapters.end(),
                [&](const DirectMlUiAdapter& adapter) {
                    return adapter.index == savedAdapter.adapterIndex &&
                           adapter.name == savedAdapter.name;
                });
            if (match != directml_adapters.end()) {
                directml_adapter_choice = static_cast<int>(
                    std::distance(directml_adapters.begin(), match));
            }
        }
    }
    int previous_directml_adapter_choice = directml_adapter_choice;

    int conversion_backend = LoadConversionBackendSelection(); // independent of AI backend
    int conversion_directml_adapter_choice = directml_adapters.empty() ? -1 : 0;
    if (!directml_adapters.empty()) {
        const SavedDirectMlAdapterSelection savedConversionAdapter =
            LoadConversionDirectMlAdapterSelection();
        if (savedConversionAdapter.valid) {
            const auto match = std::find_if(
                directml_adapters.begin(), directml_adapters.end(),
                [&](const DirectMlUiAdapter& adapter) {
                    return adapter.index == savedConversionAdapter.adapterIndex &&
                           adapter.name == savedConversionAdapter.name;
                });
            if (match != directml_adapters.end()) {
                conversion_directml_adapter_choice = static_cast<int>(
                    std::distance(directml_adapters.begin(), match));
            }
        }
    }
    int previous_conversion_backend = conversion_backend;
    int previous_conversion_directml_adapter_choice =
        conversion_directml_adapter_choice;

    auto aiPassthroughEnabled = [&]() { return ai_stream_mode != 3; };
    auto selectedAiOutputMode = [&]() {
        if (ai_stream_mode == 1) return AiOutputMode::WebmVp9Alpha;
        if (ai_stream_mode == 2) return AiOutputMode::ChromaKeyHevc;
        return AiOutputMode::AlphaPackedHevc;
    };
    auto selectedAiBackend = [&]() {
        return ai_backend == 0 ? AiBackend::NvidiaCuda :
            (ai_backend == 1 ? AiBackend::DirectML : AiBackend::Cpu);
    };
    auto selectedDirectMlDevice = [&]() {
        return (ai_backend == 1 && directml_adapter_choice >= 0)
            ? directml_adapters[static_cast<size_t>(directml_adapter_choice)].index
            : 0;
    };
    auto refreshAiPrewarm = [&]() {
        // AI initialization belongs to DLNA Server Start().  Merely selecting
        // a file/backend/mode must not touch CUDA/DirectML/CPU.
        if (!server.IsRunning()) return;

        const AiBackend backend = selectedAiBackend();
        const int device = selectedDirectMlDevice();
        if (!aiPassthroughEnabled() || first_selected_video.empty()) {
            server.SetAiPrewarm(false, {}, backend, device);
            return;
        }
        server.SetAiPrewarm(true, first_selected_video, backend, device);
    };
    auto selectedConversionBackend = [&]() {
        return conversion_backend == 0 ? AiBackend::NvidiaCuda :
            (conversion_backend == 1 ? AiBackend::DirectML : AiBackend::Cpu);
    };
    auto selectedConversionDirectMlDevice = [&]() {
        return (conversion_backend == 1 && conversion_directml_adapter_choice >= 0)
            ? directml_adapters[static_cast<size_t>(conversion_directml_adapter_choice)].index
            : 0;
    };

    std::filesystem::path conversion_input;
    int conversion_mode = 0; // 0=Chroma Key, 1=WebM Alpha, 2=Alpha Packed
    bool resume_prewarm_after_conversion = false;
    int ui_language = LoadUiLanguage();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (aiCapabilityProbeStarted && !aiCapabilityChecked &&
            aiCapabilityFuture.valid() &&
            aiCapabilityFuture.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            const AiCapabilityResult result = aiCapabilityFuture.get();
            aiCapabilityChecked = true;
            aiCapabilityAvailable = result.available;
            aiCapabilityMessage = result.message;
            // This probe is used only by CUDA file conversion.  DLNA Server
            // startup performs its own selected-backend initialization, so a
            // conversion probe result must never change the AI Passthrough mode.
            server.RecordAiCapabilityResult(result);
        }

        style.Colors[ImGuiCol_WindowBg] = server.IsRunning()
            ? serverRunningBackgroundColor
            : backgroundColor;

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("##r800zzXRdlnaServer", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        // Draw the title and website links manually so every item remains
        // clickable in the non-content title-bar area.
        const ImVec2 windowPosition = ImGui::GetWindowPos();
        const float titleTextY = windowPosition.y +
            (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f;
        float titleLinkX = windowPosition.x + ImGui::GetStyle().FramePadding.x;
        const int webLanguage = std::clamp(ui_language, 0, 6);
        const std::wstring repositoryUrl = std::wstring(L"https://github.com/r800zz/r800zzxrdlnaserver/blob/main/") + kGitHubReadmeFiles[webLanguage];
        const std::wstring vr180gUrl = std::wstring(L"https://vr180g.com/?l=") + kWebLanguageCodes[webLanguage]; const std::wstring vrplayerUrl = std::wstring(L"https://vr180g.com/pico/vrplayer.php?l=") + kWebLanguageCodes[webLanguage];
        const std::wstring browserUrl = std::wstring(L"https://vr180g.com/browser/browser.php?l=") + kWebLanguageCodes[webLanguage];
        const std::wstring pctohmdUrl = std::wstring(L"https://vr180g.com/pc2hmd.html?l=") + kWebLanguageCodes[webLanguage];
        titleLinkX = DrawTitleBarWebLink( "r800zzXRdlnaServer", repositoryUrl.c_str(), ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink( "vr180g.com", vr180gUrl.c_str(), ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink( "R800ZZ", L"https://www.youtube.com/@R800ZZ", ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink( "vrplayer", vrplayerUrl.c_str(), ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink( "Browser", browserUrl.c_str(), ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink( "PC to VR HMD", pctohmdUrl.c_str(), ImVec2(titleLinkX, titleTextY)) + 18.0f;

        ImGui::TextUnformatted("AI Passthrough DLNA Server for r800zzvrplayer");
        const char* languageNames[] = {
            "English", "Русский", "Español", "ภาษาไทย",
            "中文", "한국어", "日本語"
        };
        for (int language = 0; language < 7; ++language) {
            if (language > 0) ImGui::SameLine();
            std::string label = std::string(languageNames[language]) +
                "##language" + std::to_string(language);
            if (ImGui::RadioButton(label.c_str(), &ui_language, language)) {
                SaveUiLanguage(ui_language);
            }
        }
        const UiStrings& uiText = GetUiStrings(ui_language);
        ImGui::Separator();
        const bool conversionRunning = conversionProcess.IsRunning();
        if (!conversionRunning && conversionProcess.completed &&
            resume_prewarm_after_conversion) {
            refreshAiPrewarm();
            resume_prewarm_after_conversion = false;
        }

        if (server.IsRunning()) ImGui::BeginDisabled();
        if (ImGui::Checkbox(uiText.videoFilesOnly, &video_files_only)) {
            SaveVideoFilesOnlySelection(video_files_only);
            folder_files = selected_folder.empty()
                ? std::vector<std::filesystem::path>{}
                : DlnaServer::FindFiles(selected_folder, video_files_only);
            rebuildSelectedFiles();
            refreshAiPrewarm();
        }
        if (server.IsRunning()) ImGui::EndDisabled();

        if (ImGui::Button(uiText.selectFolder)) {
            const auto path = SelectVideoFolder(hwnd, video_files_only);
            if (!path.empty()) {
                selected_folder = path;
                folder_files = DlnaServer::FindFiles(
                    selected_folder, video_files_only);
                rebuildSelectedFiles();
                SaveMediaSelection(selected_folder, individually_selected_files);
                if (!server.IsRunning()) refreshAiPrewarm();
            }
        }
        ImGui::SameLine();
        if (selected_folder.empty()) {
            ImGui::TextDisabled("%s", uiText.noFolder);
        } else {
            ImGui::TextWrapped("%s (%zu folder files)",
                               WideToUtf8(selected_folder.wstring()).c_str(),
                               folder_files.size());
        }

        if (ImGui::Button(uiText.selectFile)) {
            const auto path = SelectVideoFile(hwnd, video_files_only);
            if (!path.empty() &&
                DlnaServer::IsShareableFile(path, video_files_only)) {
                individually_selected_files.push_back(path);
                rebuildSelectedFiles();
                SaveMediaSelection(selected_folder, individually_selected_files);
                if (!server.IsRunning()) refreshAiPrewarm();
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu individual, %zu total files",
                            active_individually_selected_files.size(),
                            selected_files.size());
        if (!active_individually_selected_files.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(uiText.clearFiles)) {
                individually_selected_files.clear();
                rebuildSelectedFiles();
                SaveMediaSelection(selected_folder, individually_selected_files);
                if (!server.IsRunning()) refreshAiPrewarm();
            }
            for (const auto& file : active_individually_selected_files) {
                ImGui::TextWrapped("  Individual: %s",
                    WideToUtf8(file.wstring()).c_str());
            }
        }

        const bool videoPlayerRunning = videoPlayerProcess.IsRunning();
        ImGui::Spacing();
        if (!videoPlayerRunning) {
            if (ImGui::Button(uiText.videoPlayer, ImVec2(180, 0))) {
                const auto path = SelectVideoFile(hwnd);
                if (!path.empty()) videoPlayerProcess.Start(path, ui_language);
            }
            ImGui::SameLine();
            if (videoPlayerProcess.lastExitCode != 0 &&
                videoPlayerProcess.lastExitCode != STILL_ACTIVE) {
                ImGui::TextDisabled("Player exited (code %lu)",
                    static_cast<unsigned long>(videoPlayerProcess.lastExitCode));
            } else {
                ImGui::TextDisabled("%s", uiText.playerHint);
            }
        } else {
            if (ImGui::Button(uiText.stopPlayer, ImVec2(180, 0))) {
                videoPlayerProcess.Stop();
            }
            ImGui::SameLine();
            ImGui::Text("%s", uiText.playerRunning);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(uiText.address);
        if (addresses.empty()) {
            ImGui::TextDisabled("%s", uiText.noAddress);
        } else {
            std::string addressPreview;
            if (server.IsRunning()) {
                addressPreview = server.BaseUrl();
            } else if (selected_address >= 0) {
                addressPreview = addresses[static_cast<size_t>(selected_address)];
            }

            ImGui::SetNextItemWidth(360.0f);
            const bool addressLocked = server.IsRunning() || webxrServer.IsRunning();
            if (addressLocked) ImGui::BeginDisabled();
            if (ImGui::BeginCombo("##ip", addressPreview.c_str())) {
                for (int i = 0; i < static_cast<int>(addresses.size()); ++i) {
                    const bool selected = i == selected_address;
                    if (ImGui::Selectable(
                            addresses[static_cast<size_t>(i)].c_str(), selected)) {
                        selected_address = i;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (addressLocked) ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const bool refreshAddressLocked = server.IsRunning() || webxrServer.IsRunning();
        if (refreshAddressLocked) ImGui::BeginDisabled();
        if (ImGui::Button(uiText.refresh)) {
            addresses = DlnaServer::GetLocalIPv4Addresses();
            selected_address = addresses.empty() ? -1 : 0;
        }
        if (refreshAddressLocked) ImGui::EndDisabled();

        ImGui::SameLine();
        const std::string baseUrl = server.IsRunning() ? server.BaseUrl() : std::string{};
        if (baseUrl.empty()) ImGui::BeginDisabled();
        if (ImGui::Button(uiText.copyUrl) && !baseUrl.empty()) {
            ImGui::SetClipboardText(baseUrl.c_str());
        }
        if (baseUrl.empty()) ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::TextUnformatted("AI Backend:");
        if (server.IsRunning()) ImGui::BeginDisabled();
        const char* backendItems[] = {"NVIDIA CUDA", "DirectML", "CPU"};
        ImGui::SetNextItemWidth(260.0f);
        ImGui::Combo("##ai_backend", &ai_backend, backendItems, 3);

        if (ai_backend == 1) {
            if (directml_adapters.empty()) {
                ImGui::TextDisabled("No hardware DXGI adapters found");
            } else {
                directml_adapter_choice = std::clamp(
                    directml_adapter_choice, 0, static_cast<int>(directml_adapters.size()) - 1);
                const auto& current = directml_adapters[static_cast<size_t>(directml_adapter_choice)];
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::BeginCombo("DirectML Device", current.name.c_str())) {
                    for (int i = 0; i < static_cast<int>(directml_adapters.size()); ++i) {
                        const bool selected = i == directml_adapter_choice;
                        const std::string label = "[" + std::to_string(directml_adapters[i].index) + "] " + directml_adapters[i].name;
                        if (ImGui::Selectable(label.c_str(), selected)) directml_adapter_choice = i;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        }
        if (server.IsRunning()) ImGui::EndDisabled();

        if (ai_backend != previous_ai_backend ||
            directml_adapter_choice != previous_directml_adapter_choice) {
            g_aiErrorText.clear();
            g_aiErrorFatal = false;
            if (ai_backend != previous_ai_backend) {
                SaveAiBackendSelection(ai_backend);
            }
            if (ai_backend == 1 && directml_adapter_choice >= 0) {
                SaveDirectMlAdapterSelection(
                    directml_adapters, directml_adapter_choice);
            }
            refreshAiPrewarm();
            previous_ai_backend = ai_backend;
            previous_directml_adapter_choice = directml_adapter_choice;
        }

        ImGui::TextUnformatted(uiText.aiMode);
        const int previousAiStreamMode = ai_stream_mode;
        // All four modes may be selected while the server is running. The
        // current RUN is left untouched; the selection is applied only at the
        // next file change or explicit zero seek.
        ImGui::RadioButton("Alpha packed", &ai_stream_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Alpha WebM VP9", &ai_stream_mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Chroma Key", &ai_stream_mode, 2);
        ImGui::SameLine();
        ImGui::RadioButton("OFF", &ai_stream_mode, 3);

        if (ai_stream_mode != previousAiStreamMode) {
            g_aiErrorText.clear();
            g_aiErrorFatal = false;
            ai_stream_mode_has_saved_or_user_value = true;
            SaveAiStreamModeSelection(ai_stream_mode);
            if (server.IsRunning()) {
                server.SetAiPassthroughMode(
                    aiPassthroughEnabled(), selectedAiOutputMode());
            } else {
                refreshAiPrewarm();
            }
        }

        if (ai_backend == 0) {
            if (aiPassthroughEnabled()) {
                if (first_selected_video.empty()) {
                    ImGui::TextDisabled("RVM: No selected video; non-video files are served raw");
                } else if (server.IsRunning()) {
                    ImGui::TextDisabled("RVM: Ready");
                } else {
                    ImGui::TextDisabled(
                        "NVIDIA CUDA: initializes when DLNA Server starts");
                }
            } else {
                ImGui::TextDisabled("NVIDIA CUDA: Selected");
            }
        } else if (ai_backend == 1) {
            if (directml_adapters.empty()) {
                ImGui::TextDisabled("DirectML: No hardware DXGI adapters found");
            } else if (aiPassthroughEnabled()) {
                if (first_selected_video.empty()) {
                    ImGui::TextDisabled("RVM: No selected video; non-video files are served raw");
                } else if (server.IsRunning()) {
                    ImGui::TextDisabled("RVM: Ready");
                } else {
                    ImGui::TextDisabled(
                        "DirectML: initializes when DLNA Server starts");
                }
            } else {
                ImGui::TextDisabled("DirectML: Selected");
            }
        } else {
            if (aiPassthroughEnabled() && !first_selected_video.empty() &&
                !server.IsRunning()) {
                ImGui::TextDisabled(
                    "CPU backend: initializes when DLNA Server starts");
            } else {
                ImGui::TextDisabled("CPU backend: Selected");
            }
        }

        ImGui::Spacing();
        if (!server.IsRunning()) {
          const bool aiNeedsBackend =
            aiPassthroughEnabled() && !first_selected_video.empty();
          const bool backendReady = !aiNeedsBackend ||
            (ai_backend == 1 ? directml_adapter_choice >= 0 : true);
          const bool can_start = backendReady &&
            !selected_files.empty() && selected_address >= 0 && !conversionRunning;

            if (!can_start) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(255.0f / 255.0f, 192.0f / 255.0f, 203.0f / 255.0f, 1.0f)); // #FFC0CB
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(255.0f / 255.0f, 205.0f / 255.0f, 214.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(245.0f / 255.0f, 170.0f / 255.0f, 184.0f / 255.0f, 1.0f));
            if (ImGui::Button(uiText.startServer, ImVec2(180, 0))) {
                const bool aiEnabled = aiPassthroughEnabled();
                server.Start(
                    selected_files,
                    addresses[static_cast<size_t>(selected_address)],
                    aiEnabled,
                    selectedAiOutputMode(),
                    selectedAiBackend(),
                    selectedDirectMlDevice(),
                    video_files_only);
            }
            ImGui::PopStyleColor(3);
            if (!can_start) ImGui::EndDisabled();
        } else {
          ImGui::PushStyleColor(ImGuiCol_Button, backgroundColor);
          ImGui::PushStyleColor(
           ImGuiCol_ButtonHovered,
           ImVec4(1.0f, 0.90f, 0.70f, 1.0f));
          ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4(1.0f, 0.84f, 0.58f, 1.0f));

          if (ImGui::Button(uiText.stopServer, ImVec2(180, 0))) {
            server.Stop();
          }
          ImGui::PopStyleColor(3);
        }

        ImGui::SameLine();
        ImGui::Text("%s: %s", uiText.status, server.Status().c_str());

        if (g_aiErrorText.empty()) {
            ImGui::TextDisabled("AI Error: none");
        } else {
            const ImVec4 errorColor = g_aiErrorFatal
                ? ImVec4(0.85f, 0.10f, 0.10f, 1.0f)
                : ImVec4(0.85f, 0.45f, 0.05f, 1.0f);
            ImGui::TextColored(errorColor, "AI Error:");
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy##ai_error")) {
                ImGui::SetClipboardText(g_aiErrorText.c_str());
            }
            ImGui::PushStyleColor(ImGuiCol_Text, errorColor);
            ImGui::InputTextMultiline(
                "##ai_error_text",
                g_aiErrorText.data(),
                g_aiErrorText.size() + 1,
                ImVec2(ImGui::GetContentRegionAvail().x,
                       ImGui::GetTextLineHeightWithSpacing() * 2.5f),
                ImGuiInputTextFlags_ReadOnly);
            ImGui::PopStyleColor();
        }

        if (server.IsRunning()) {
            ImGui::Text("DLNA name: r800zzXRdlnaServer");
            ImGui::Text("HTTP: %s", server.BaseUrl().c_str());
            ImGui::TextWrapped("%s", uiText.firewall);
        }

        ImGui::SeparatorText(uiText.webxrSection);
        const std::filesystem::path webxrRoot = webxrServer.RootDirectory();
        ImGui::TextWrapped("%s %s", uiText.webxrRoot,
            WideToUtf8(webxrRoot.wstring()).c_str());
        if (ImGui::Button(uiText.webxrOpenFolder)) {
            std::error_code webxrDirectoryError;
            std::filesystem::create_directories(webxrRoot, webxrDirectoryError);
            if (!webxrDirectoryError) {
                ShellExecuteW(nullptr, L"open", webxrRoot.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        ImGui::SameLine();
        if (webxrServer.IsRunning()) ImGui::BeginDisabled();
        if (ImGui::Button(uiText.webxrChangeFolder)) {
            const std::filesystem::path selectedWebXrRoot =
                SelectWebXrRootFolder(hwnd);
            if (!selectedWebXrRoot.empty() &&
                webxrServer.SetRootDirectory(selectedWebXrRoot)) {
                SaveWebXrRootSelection(webxrServer.RootDirectory());
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(uiText.webxrResetFolder)) {
            if (webxrServer.ResetRootDirectory()) {
                ClearWebXrRootSelection();
            }
        }
        if (webxrServer.IsRunning()) ImGui::EndDisabled();

        if (!webxrServer.IsRunning()) {
            if (selected_address < 0) ImGui::BeginDisabled();
            if (ImGui::Button(uiText.webxrStart, ImVec2(210, 0)) &&
                selected_address >= 0) {
                webxrServer.Start(addresses[static_cast<size_t>(selected_address)]);
            }
            if (selected_address < 0) ImGui::EndDisabled();
        } else {
            if (ImGui::Button(uiText.webxrStop, ImVec2(210, 0))) {
                webxrServer.Stop();
            }
        }
        ImGui::SameLine();
        const std::string webxrStatus = webxrServer.Status();
        const char* webxrStatusText =
            webxrStatus == "Running" ? uiText.webxrRunning :
            (webxrStatus == "Stopped" ? uiText.webxrStopped : webxrStatus.c_str());
        ImGui::Text("%s %s", uiText.webxrStatus, webxrStatusText);

        if (webxrServer.IsRunning()) {
            const std::string webxrUrl = webxrServer.BaseUrl();
            ImGui::TextUnformatted(uiText.webxrHttps);
            ImGui::SameLine();
            ImGui::TextUnformatted(webxrUrl.c_str());
            const bool webxrUrlHovered = ImGui::IsItemHovered();
            const ImVec2 webxrLinkMin = ImGui::GetItemRectMin();
            const ImVec2 webxrLinkMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(webxrLinkMin.x, webxrLinkMax.y),
                webxrLinkMax,
                ImGui::GetColorU32(ImGuiCol_Text));
            if (webxrUrlHovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", webxrUrl.c_str());
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    const std::wstring webxrUrlWide = Utf8ToWide(webxrUrl);
                    ShellExecuteW(nullptr, L"open", webxrUrlWide.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(uiText.webxrCopyUrl)) {
                ImGui::SetClipboardText(webxrUrl.c_str());
            }
            ImGui::TextWrapped("%s %s", uiText.webxrRootCa,
                WideToUtf8(webxrServer.RootCertificateFile().wstring()).c_str());
            ImGui::TextDisabled("%s", uiText.webxrInstallCaHint);
            ImGui::TextDisabled("%s", uiText.webxrServeHint);
        }

        ImGui::SeparatorText(uiText.convertSection);
        if (conversionRunning) ImGui::BeginDisabled();
        if (ImGui::Button(uiText.selectInput)) {
            const auto path = SelectVideoFile(hwnd);
            if (!path.empty() && DlnaServer::IsMediaFile(path)) {
                conversion_input = path;
                conversionProcess.completed = false;
                conversionProcess.error.clear();
                if (conversion_backend == 0) startCudaCapabilityProbe();
            }
        }
        if (conversionRunning) ImGui::EndDisabled();
        ImGui::SameLine();
        if (conversion_input.empty()) {
            ImGui::TextDisabled("%s", uiText.noInput);
        } else {
            ImGui::TextWrapped("%s", WideToUtf8(conversion_input.wstring()).c_str());
        }

        const char* conversionModes[] = {
            "Chroma Key (green background)",
            "Alpha WebM VP9",
            "Alpha Packed"
        };
        if (conversionRunning) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(300.0f);
        ImGui::Combo(uiText.conversionFormat, &conversion_mode, conversionModes,
                     static_cast<int>(std::size(conversionModes)));
        if (conversionRunning) ImGui::EndDisabled();

        if (conversionRunning) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(260.0f);
        ImGui::Combo(uiText.conversionBackend, &conversion_backend,
                     backendItems, 3);
        if (conversion_backend == 1) {
            if (directml_adapters.empty()) {
                ImGui::TextDisabled("DirectML: No hardware DXGI adapters found");
            } else {
                conversion_directml_adapter_choice = std::clamp(
                    conversion_directml_adapter_choice, 0,
                    static_cast<int>(directml_adapters.size()) - 1);
                const auto& current = directml_adapters[
                    static_cast<size_t>(conversion_directml_adapter_choice)];
                ImGui::SetNextItemWidth(360.0f);
                if (ImGui::BeginCombo(uiText.conversionDevice, current.name.c_str())) {
                    for (int i = 0; i < static_cast<int>(directml_adapters.size()); ++i) {
                        const bool selected = i == conversion_directml_adapter_choice;
                        const std::string label = "[" +
                            std::to_string(directml_adapters[i].index) + "] " +
                            directml_adapters[i].name;
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            conversion_directml_adapter_choice = i;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        }
        if (conversionRunning) ImGui::EndDisabled();

        if (conversion_backend != previous_conversion_backend ||
            conversion_directml_adapter_choice !=
                previous_conversion_directml_adapter_choice) {
            if (conversion_backend != previous_conversion_backend) {
                SaveConversionBackendSelection(conversion_backend);
                if (conversion_backend == 0 && !conversion_input.empty()) {
                    startCudaCapabilityProbe();
                }
            }
            if (conversion_backend == 1 &&
                conversion_directml_adapter_choice >= 0) {
                SaveConversionDirectMlAdapterSelection(
                    directml_adapters, conversion_directml_adapter_choice);
            }
            previous_conversion_backend = conversion_backend;
            previous_conversion_directml_adapter_choice =
                conversion_directml_adapter_choice;
        }

        if (!conversionRunning) {
            const bool conversionBackendReady =
                conversion_backend == 0
                    ? (aiCapabilityChecked && aiCapabilityAvailable)
                    : (conversion_backend == 1
                        ? conversion_directml_adapter_choice >= 0
                        : true);
            const bool convertDisabled = conversion_input.empty() ||
                !conversionBackendReady || server.IsRunning() || videoPlayerRunning;
            if (convertDisabled) ImGui::BeginDisabled();
            if (ImGui::Button(uiText.convert, ImVec2(180, 0))) {
                const AiOutputMode mode = conversion_mode == 0
                    ? AiOutputMode::ChromaKeyHevc
                    : (conversion_mode == 1
                        ? AiOutputMode::WebmVp9Alpha
                        : AiOutputMode::AlphaPackedHevc);
                const auto output = SelectConversionOutput(hwnd, conversion_input, mode);
                if (!output.empty()) {
                    if (aiPassthroughEnabled()) {
                        server.SetAiPrewarm(
                            false, {}, selectedAiBackend(), selectedDirectMlDevice());
                        resume_prewarm_after_conversion = true;
                    }
                    if (!conversionProcess.Start(
                            conversion_input, output, mode,
                            selectedConversionBackend(),
                            selectedConversionDirectMlDevice()) &&
                        resume_prewarm_after_conversion) {
                        refreshAiPrewarm();
                        resume_prewarm_after_conversion = false;
                    }
                }
            }
            if (convertDisabled) ImGui::EndDisabled();
        } else {
            if (ImGui::Button(uiText.cancel, ImVec2(180, 0))) {
                conversionProcess.Stop();
            }
        }
        ImGui::SameLine();
        if (conversion_backend == 0) {
            if (!aiCapabilityProbeStarted && !aiCapabilityChecked) {
                ImGui::TextDisabled("Conversion: NVIDIA CUDA (check starts when input is selected)");
            } else if (!aiCapabilityChecked) {
                ImGui::TextDisabled("Conversion: checking NVIDIA CUDA...");
            } else if (aiCapabilityAvailable) {
                ImGui::TextDisabled("Conversion: NVIDIA CUDA device 0");
            } else {
                ImGui::TextDisabled("Conversion: NVIDIA CUDA unavailable");
            }
        } else if (conversion_backend == 1) {
            if (conversion_directml_adapter_choice >= 0 &&
                conversion_directml_adapter_choice < static_cast<int>(directml_adapters.size())) {
                ImGui::TextDisabled("Conversion: DirectML [%d] %s",
                    directml_adapters[static_cast<size_t>(
                        conversion_directml_adapter_choice)].index,
                    directml_adapters[static_cast<size_t>(
                        conversion_directml_adapter_choice)].name.c_str());
            } else {
                ImGui::TextDisabled("Conversion: DirectML device unavailable");
            }
        } else {
            ImGui::TextDisabled("Conversion: CPU");
        }
        if (conversionRunning) {
            const char* runningBackend =
                conversionProcess.backend == AiBackend::NvidiaCuda ? "NVIDIA CUDA" :
                (conversionProcess.backend == AiBackend::DirectML ? "DirectML" : "CPU");
            ImGui::Text("Conversion: Running (%s)", runningBackend);
            ImGui::Text("%s: %llu", uiText.outputFrames,
                static_cast<unsigned long long>(conversionProcess.outputFrames));
            ImGui::TextWrapped("Output: %s",
                WideToUtf8(conversionProcess.output.wstring()).c_str());
        } else if (conversionProcess.completed) {
            if (conversionProcess.lastExitCode == 0 && conversionProcess.error.empty()) {
                ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
                    "Conversion complete: %s",
                    WideToUtf8(conversionProcess.output.wstring()).c_str());
                ImGui::Text("%s: %llu", uiText.outputFrames,
                    static_cast<unsigned long long>(conversionProcess.outputFrames));
            } else {
                ImGui::TextWrapped("%s", conversionProcess.error.c_str());
            }
        } else if (!conversionProcess.error.empty()) {
            ImGui::TextWrapped("%s", conversionProcess.error.c_str());
        }
        if (!conversionProcess.logFile.empty()) {
            ImGui::TextWrapped("Conversion log: %s",
                WideToUtf8(conversionProcess.logFile.wstring()).c_str());
        }

        ImGui::SeparatorText(uiText.log);
        const auto log_lines = server.Logs();
        std::string log_text;
        for (const auto& line : log_lines) {
            log_text += line;
            log_text += "\n";
        }

        if (ImGui::Button(uiText.copyLog)) {
            ImGui::SetClipboardText(log_text.c_str());
        }
        ImGui::SameLine();
        if (ImGui::Button(uiText.clearLog)) {
            server.ClearLogs();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Copies the complete log to the Windows clipboard");

        ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (const auto& line : log_lines) ImGui::TextUnformatted(line.c_str());
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        const ImVec4 clearColor = server.IsRunning()
            ? serverRunningBackgroundColor
            : ImVec4(241.0f / 255.0f, 204.0f / 255.0f,
                     165.0f / 255.0f, 1.0f);
        const float clear_color[4] = {
            clearColor.x, clearColor.y, clearColor.z, clearColor.w
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT present_result = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (present_result == DXGI_STATUS_OCCLUDED);
    }

    SaveAiBackendSelection(ai_backend);
    if (ai_backend == 1 && directml_adapter_choice >= 0) {
        SaveDirectMlAdapterSelection(directml_adapters, directml_adapter_choice);
    }
    SaveAiStreamModeSelection(ai_stream_mode);
    SaveVideoFilesOnlySelection(video_files_only);
    SaveConversionBackendSelection(conversion_backend);
    if (conversion_backend == 1 && conversion_directml_adapter_choice >= 0) {
        SaveConversionDirectMlAdapterSelection(
            directml_adapters, conversion_directml_adapter_choice);
    }
    videoPlayerProcess.Stop();
    conversionProcess.Stop(false);
    webxrServer.Stop();
    server.Stop();
    server.SetAiErrorCallback({});
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(comResult)) CoUninitialize();
    ReleaseMutex(singleInstanceMutex);
    CloseHandle(singleInstanceMutex);
    return 0;
}

