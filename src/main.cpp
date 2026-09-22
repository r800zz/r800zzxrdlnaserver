#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tchar.h>

#include <algorithm>
#include <chrono>
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
#include "../resources/resource.h"

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static std::filesystem::path SelectVideoFolder(HWND owner) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = L"Select the folder containing video files";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (!item) return {};
    wchar_t folder[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(item, folder);
    CoTaskMemFree(item);
    if (ok) return std::filesystem::path(folder);
    return {};
}

static std::filesystem::path SelectVideoFile(HWND owner) {
    wchar_t file[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter =
        L"Video files\0*.mp4;*.m4v;*.webm;*.mkv;*.avi;*.mov;*.ts;*.m2ts\0"
        L"All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_EXPLORER | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Select a video file";
    if (GetOpenFileNameW(&dialog)) return std::filesystem::path(file);
    return {};
}

struct UiStrings {
    const char* selectFolder;
    const char* noFolder;
    const char* selectFile;
    const char* clearFiles;
    const char* videoPlayer;
    const char* stopPlayer;
    const char* playerRunning;
    const char* playerHint;
    const char* address;
    const char* noAddress;
    const char* refresh;
    const char* aiPassthrough;
    const char* aiMode;
    const char* alpha;
    const char* chromaKey;
    const char* alphaFormat;
    const char* convertSection;
    const char* selectInput;
    const char* noInput;
    const char* conversionFormat;
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
};

static const UiStrings& GetUiStrings(int language) {
    static const UiStrings strings[] = {
        {"Select Video Folder...", "No folder selected", "Select Video File...", "Clear Individual Files",
         "Video Player...", "Stop Video Player", "Video Player: Running",
         "Audio playback; controls and seek bar are in the player window",
         "Advertised IPv4 address:", "No LAN IPv4 address found", "Refresh IPs",
         "AI Passthrough", "AI Passthrough Mode:", "Alpha", "Chroma Key", "Alpha Format",
         "Convert Video File", "Select Conversion Input...", "No conversion input selected",
         "Conversion Format", "Convert Video...", "Cancel Conversion", "Output frames",
         "Start DLNA Server", "Stop DLNA Server", "Status", "Allow access on your private network if Windows Firewall asks.",
         "Log", "Copy Log", "Log Clear"},
        {"Выбрать папку с видео...", "Папка не выбрана", "Выбрать видеофайл...", "Очистить выбранные файлы",
         "Видеоплеер...", "Остановить видеоплеер", "Видеоплеер: работает",
         "Звук, элементы управления и полоса поиска находятся в окне плеера",
         "Объявляемый IPv4-адрес:", "IPv4-адрес LAN не найден", "Обновить IP",
         "ИИ-прозрачность", "Режим ИИ-прозрачности:", "Альфа", "Хромакей", "Формат альфа",
         "Преобразование видеофайла", "Выбрать исходное видео...", "Исходное видео не выбрано",
         "Формат преобразования", "Преобразовать...", "Отменить преобразование", "Выходные кадры",
         "Запустить DLNA-сервер", "Остановить DLNA-сервер", "Состояние", "Разрешите доступ в частной сети, если спросит брандмауэр Windows.",
         "Журнал", "Копировать журнал", "Очистить журнал"},
        {"Seleccionar carpeta de vídeos...", "Ninguna carpeta seleccionada", "Seleccionar archivo de vídeo...", "Borrar archivos individuales",
         "Reproductor de vídeo...", "Detener reproductor", "Reproductor: en ejecución",
         "El audio, los controles y la barra de búsqueda están en la ventana del reproductor",
         "Dirección IPv4 anunciada:", "No se encontró una dirección IPv4 LAN", "Actualizar IP",
         "Transparencia por IA", "Modo de transparencia por IA:", "Alfa", "Croma", "Formato alfa",
         "Convertir archivo de vídeo", "Seleccionar vídeo de entrada...", "No se seleccionó vídeo de entrada",
         "Formato de conversión", "Convertir vídeo...", "Cancelar conversión", "Fotogramas de salida",
         "Iniciar servidor DLNA", "Detener servidor DLNA", "Estado", "Permita el acceso a la red privada si lo solicita el Firewall de Windows.",
         "Registro", "Copiar registro", "Borrar registro"},
        {"เลือกโฟลเดอร์วิดีโอ...", "ยังไม่ได้เลือกโฟลเดอร์", "เลือกไฟล์วิดีโอ...", "ล้างไฟล์ที่เลือก",
         "โปรแกรมเล่นวิดีโอ...", "หยุดโปรแกรมเล่น", "โปรแกรมเล่น: กำลังทำงาน",
         "เสียง ปุ่มควบคุม และแถบค้นหาอยู่ในหน้าต่างโปรแกรมเล่น",
         "ที่อยู่ IPv4 ที่ประกาศ:", "ไม่พบที่อยู่ IPv4 ของ LAN", "รีเฟรช IP",
         "AI Passthrough", "โหมด AI Passthrough:", "อัลฟา", "โครมาคีย์", "รูปแบบอัลฟา",
         "แปลงไฟล์วิดีโอ", "เลือกวิดีโอต้นฉบับ...", "ยังไม่ได้เลือกวิดีโอต้นฉบับ",
         "รูปแบบการแปลง", "แปลงวิดีโอ...", "ยกเลิกการแปลง", "จำนวนเฟรมเอาต์พุต",
         "เริ่มเซิร์ฟเวอร์ DLNA", "หยุดเซิร์ฟเวอร์ DLNA", "สถานะ", "อนุญาตการเข้าถึงเครือข่ายส่วนตัวหาก Windows Firewall ถาม",
         "บันทึก", "คัดลอกบันทึก", "ล้างบันทึก"},
        {"选择视频文件夹...", "未选择文件夹", "选择视频文件...", "清除单独选择的文件",
         "视频播放器...", "停止视频播放器", "视频播放器：运行中",
         "声音、控制按钮和进度条位于播放器窗口内",
         "公布的 IPv4 地址：", "未找到局域网 IPv4 地址", "刷新 IP",
         "AI 透视", "AI 透视模式：", "Alpha", "色键", "Alpha 格式",
         "转换视频文件", "选择转换输入...", "未选择转换输入",
         "转换格式", "转换视频...", "取消转换", "输出帧数",
         "启动 DLNA 服务器", "停止 DLNA 服务器", "状态", "如果 Windows 防火墙询问，请允许专用网络访问。",
         "日志", "复制日志", "清除日志"},
        {"비디오 폴더 선택...", "폴더를 선택하지 않음", "비디오 파일 선택...", "개별 파일 지우기",
         "비디오 플레이어...", "비디오 플레이어 중지", "비디오 플레이어: 실행 중",
         "오디오, 조작 버튼 및 탐색 막대는 플레이어 창 안에 있습니다",
         "공개할 IPv4 주소:", "LAN IPv4 주소를 찾지 못함", "IP 새로 고침",
         "AI 패스스루", "AI 패스스루 모드:", "알파", "크로마 키", "알파 형식",
         "비디오 파일 변환", "변환 입력 선택...", "변환 입력을 선택하지 않음",
         "변환 형식", "비디오 변환...", "변환 취소", "출력 프레임",
         "DLNA 서버 시작", "DLNA 서버 중지", "상태", "Windows 방화벽이 요청하면 개인 네트워크 액세스를 허용하십시오.",
         "로그", "로그 복사", "로그 지우기"},
        {"動画フォルダーを選択...", "フォルダー未選択", "動画ファイルを選択...", "個別選択を消去",
         "ビデオプレイヤー...", "ビデオプレイヤーを停止", "ビデオプレイヤー：実行中",
         "音声・操作ボタン・シークバーはプレイヤーウィンドウ内にあります",
         "公開するIPv4アドレス：", "LANのIPv4アドレスがありません", "IPを更新",
         "AIパススルー", "AIパススルーモード：", "アルファ", "クロマキー", "アルファ形式",
         "動画ファイル変換", "変換元動画を選択...", "変換元動画未選択",
         "変換形式", "動画を変換...", "変換を中止", "出力フレーム数",
         "DLNAサーバーを開始", "DLNAサーバーを停止", "状態", "Windowsファイアウォールに表示された場合は、プライベートネットワークへのアクセスを許可してください。",
         "ログ", "ログをコピー", "ログを消去"},
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
    bool usedGpu = false;
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
        return false;
    }

    bool Start(const std::filesystem::path& input,
               const std::filesystem::path& outputFile,
               AiOutputMode mode,
               bool gpuAvailable) {
        if (IsRunning()) return false;
        completed = false;
        lastExitCode = STILL_ACTIVE;
        error.clear();
        outputFrames = 0;
        lastProgressRead = {};
        output = outputFile;
        logFile = outputFile;
        logFile += L".conversion.log";
        usedGpu = gpuAvailable;

        const auto dir = ExecutableDirectory();
        std::filesystem::path application;
        std::wstring command;
        if (gpuAvailable) {
            application = dir / L"r800zz_ai_worker.exe";
            const auto model = dir / L"rvm_mobilenetv3_fp16.onnx";
            if (!std::filesystem::is_regular_file(application) ||
                !std::filesystem::is_regular_file(model)) {
                error = "GPU worker or RVM model is missing beside the server executable.";
                return false;
            }
            command = QuoteWindowsArgument(application.wstring()) + L" " +
                      QuoteWindowsArgument(input.wstring()) + L" --model " +
                      QuoteWindowsArgument(model.wstring()) +
                      L" --device 0 --qp 20 --start-ms 0 --output-mode " +
                      ConversionModeName(mode) + L" --convert-output " +
                      QuoteWindowsArgument(outputFile.wstring()) + L" --no-preview";
        } else {
            application = dir / L"r800zz_ai_worker.exe";
            const auto model = dir / L"rvm_mobilenetv3_fp32.onnx";
            if (!std::filesystem::is_regular_file(application) ||
                !std::filesystem::is_regular_file(model)) {
                error = "C++ worker or CPU FP32 RVM model is missing beside the server executable.";
                return false;
            }
            command = QuoteWindowsArgument(application.wstring()) + L" " +
                      QuoteWindowsArgument(input.wstring()) + L" --model " +
                      QuoteWindowsArgument(model.wstring()) +
                      L" --cpu --qp 20 --output-mode " +
                      ConversionModeName(mode) + L" --convert-output " +
                      QuoteWindowsArgument(outputFile.wstring()) + L" --no-preview";
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
    SetProcessDPIAware();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, instance, nullptr, nullptr, nullptr, nullptr,
                   L"R800ZZDlnaServerWindow", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName,
                              L"r800zzXRdlnaServer 0.2",
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
    VideoPlayerProcess videoPlayerProcess;
    ConversionProcess conversionProcess;
    std::future<AiCapabilityResult> aiCapabilityFuture = std::async(
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
    bool aiCapabilityChecked = false;
    bool aiCapabilityAvailable = false;
    std::string aiCapabilityMessage = "Checking NVIDIA GPU support...";
    std::filesystem::path selected_folder;
    std::vector<std::filesystem::path> folder_files;
    std::vector<std::filesystem::path> individually_selected_files;
    std::vector<std::filesystem::path> selected_files;
    auto rebuildSelectedFiles = [&]() {
        selected_files = folder_files;
        for (const auto& file : individually_selected_files) {
            const auto duplicate = std::find_if(
                selected_files.begin(), selected_files.end(),
                [&](const std::filesystem::path& existing) {
                    return _wcsicmp(existing.wstring().c_str(),
                                    file.wstring().c_str()) == 0;
                });
            if (duplicate == selected_files.end()) selected_files.push_back(file);
        }
    };
    std::vector<std::string> addresses = DlnaServer::GetLocalIPv4Addresses();
    int selected_address = addresses.empty() ? -1 : 0;
    // 0=Alpha packed, 1=Alpha WebM VP9, 2=Chroma Key, 3=OFF.
    int ai_stream_mode = 3;
    auto aiPassthroughEnabled = [&]() { return ai_stream_mode != 3; };
    auto selectedAiOutputMode = [&]() {
        if (ai_stream_mode == 1) return AiOutputMode::WebmVp9Alpha;
        if (ai_stream_mode == 2) return AiOutputMode::ChromaKeyHevc;
        return AiOutputMode::AlphaPackedHevc;
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

        if (!aiCapabilityChecked &&
            aiCapabilityFuture.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            const AiCapabilityResult result = aiCapabilityFuture.get();
            aiCapabilityChecked = true;
            aiCapabilityAvailable = result.available;
            aiCapabilityMessage = result.message;
            ai_stream_mode = result.available ? 0 : 3;
            server.RecordAiCapabilityResult(result);
            if (result.available && !selected_files.empty()) {
                server.SetAiPrewarm(true, selected_files.front());
            }
        }

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
        const std::wstring repositoryUrl =
            std::wstring(L"https://github.com/r800zz/r800zzxrdlnaserver/blob/main/") +
            kGitHubReadmeFiles[webLanguage];
        const std::wstring vr180gUrl =
            std::wstring(L"https://vr180g.com/?l=") +
            kWebLanguageCodes[webLanguage];
        const std::wstring vrplayerUrl =
            std::wstring(L"https://vr180g.com/pico/vrplayer.php?l=") +
            kWebLanguageCodes[webLanguage];
        const std::wstring browserUrl =
            std::wstring(L"https://vr180g.com/browser/browser.php?l=") +
            kWebLanguageCodes[webLanguage];
        titleLinkX = DrawTitleBarWebLink(
            "r800zzXRdlnaServer", repositoryUrl.c_str(),
            ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink(
            "vr180g.com", vr180gUrl.c_str(),
            ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink(
            "R800ZZ", L"https://www.youtube.com/@R800ZZ",
            ImVec2(titleLinkX, titleTextY)) + 18.0f;
        titleLinkX = DrawTitleBarWebLink(
            "vrplayer", vrplayerUrl.c_str(),
            ImVec2(titleLinkX, titleTextY)) + 18.0f;
        DrawTitleBarWebLink(
            "Browser", browserUrl.c_str(),
            ImVec2(titleLinkX, titleTextY));

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
            if (aiPassthroughEnabled() && !selected_files.empty()) {
                server.SetAiPrewarm(true, selected_files.front());
            }
            resume_prewarm_after_conversion = false;
        }

        if (ImGui::Button(uiText.selectFolder)) {
            const auto path = SelectVideoFolder(hwnd);
            if (!path.empty()) {
                selected_folder = path;
                folder_files = DlnaServer::FindMediaFiles(selected_folder);
                rebuildSelectedFiles();
                if (aiPassthroughEnabled() && !server.IsRunning()) {
                    server.SetAiPrewarm(
                        true, selected_files.empty()
                            ? std::filesystem::path{} : selected_files.front());
                }
            }
        }
        ImGui::SameLine();
        if (selected_folder.empty()) {
            ImGui::TextDisabled("%s", uiText.noFolder);
        } else {
            ImGui::TextWrapped("%s (%zu folder videos)",
                               WideToUtf8(selected_folder.wstring()).c_str(),
                               folder_files.size());
        }

        if (ImGui::Button(uiText.selectFile)) {
            const auto path = SelectVideoFile(hwnd);
            if (!path.empty() && DlnaServer::IsMediaFile(path)) {
                individually_selected_files.push_back(path);
                rebuildSelectedFiles();
                if (aiPassthroughEnabled() && !server.IsRunning() &&
                    !selected_files.empty()) {
                    server.SetAiPrewarm(true, selected_files.front());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu individual, %zu total video files",
                            individually_selected_files.size(),
                            selected_files.size());
        if (!individually_selected_files.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton(uiText.clearFiles)) {
                individually_selected_files.clear();
                rebuildSelectedFiles();
                if (aiPassthroughEnabled() && !server.IsRunning()) {
                    if (selected_files.empty()) server.SetAiPrewarm(false, {});
                    else server.SetAiPrewarm(true, selected_files.front());
                }
            }
            for (const auto& file : individually_selected_files) {
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
            const char* preview = selected_address >= 0 ? addresses[static_cast<size_t>(selected_address)].c_str() : "";
            if (ImGui::BeginCombo("##ip", preview)) {
                for (int i = 0; i < static_cast<int>(addresses.size()); ++i) {
                    const bool selected = i == selected_address;
                    if (ImGui::Selectable(addresses[static_cast<size_t>(i)].c_str(), selected)) selected_address = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(uiText.refresh) && !server.IsRunning()) {
            addresses = DlnaServer::GetLocalIPv4Addresses();
            selected_address = addresses.empty() ? -1 : 0;
        }

        ImGui::Spacing();
        ImGui::TextUnformatted(uiText.aiMode);
        const int previousAiStreamMode = ai_stream_mode;
        const bool aiChoicesDisabled = server.IsRunning() ||
            !aiCapabilityChecked || !aiCapabilityAvailable;
        if (aiChoicesDisabled) ImGui::BeginDisabled();
        ImGui::RadioButton("Alpha packed", &ai_stream_mode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Alpha WebM VP9", &ai_stream_mode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Chroma Key", &ai_stream_mode, 2);
        if (aiChoicesDisabled) ImGui::EndDisabled();
        ImGui::SameLine();
        if (server.IsRunning()) ImGui::BeginDisabled();
        ImGui::RadioButton("OFF", &ai_stream_mode, 3);
        if (server.IsRunning()) ImGui::EndDisabled();

        if (ai_stream_mode != previousAiStreamMode && !server.IsRunning()) {
            if (aiPassthroughEnabled()) {
                server.SetAiPrewarm(
                    true, selected_files.empty()
                        ? std::filesystem::path{} : selected_files.front());
            } else {
                server.SetAiPrewarm(false, {});
            }
        }

        if (!aiCapabilityChecked) {
            ImGui::TextDisabled("GPU: Checking...");
        } else if (!aiCapabilityAvailable) {
            ImGui::TextDisabled(
                "GPU: Unavailable - raw DLNA and CPU file conversion available");
        } else if (aiPassthroughEnabled()) {
            if (selected_files.empty()) {
                ImGui::TextDisabled("RVM: Select a video folder or file");
            } else {
                ImGui::TextDisabled("RVM: %s", server.IsAiPrewarmReady() ? "Ready" : "Warming up");
            }
        } else {
            ImGui::TextDisabled("GPU: Available");
        }

        if (aiCapabilityChecked && !aiCapabilityAvailable) {
            ImGui::TextWrapped("%s", aiCapabilityMessage.c_str());
        }

        ImGui::Spacing();
        ImGui::SeparatorText(uiText.convertSection);
        if (conversionRunning) ImGui::BeginDisabled();
        if (ImGui::Button(uiText.selectInput)) {
            const auto path = SelectVideoFile(hwnd);
            if (!path.empty() && DlnaServer::IsMediaFile(path)) {
                conversion_input = path;
                conversionProcess.completed = false;
                conversionProcess.error.clear();
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

        if (!conversionRunning) {
            const bool convertDisabled = conversion_input.empty() ||
                !aiCapabilityChecked || server.IsRunning() || videoPlayerRunning;
            if (convertDisabled) ImGui::BeginDisabled();
            if (ImGui::Button(uiText.convert, ImVec2(180, 0))) {
                const AiOutputMode mode = conversion_mode == 0
                    ? AiOutputMode::ChromaKeyHevc
                    : (conversion_mode == 1
                        ? AiOutputMode::WebmVp9Alpha
                        : AiOutputMode::AlphaPackedHevc);
                const auto output = SelectConversionOutput(hwnd, conversion_input, mode);
                if (!output.empty()) {
                    if (aiCapabilityAvailable && aiPassthroughEnabled()) {
                        server.SetAiPrewarm(false, {});
                        resume_prewarm_after_conversion = true;
                    }
                    if (!conversionProcess.Start(
                            conversion_input, output, mode,
                            aiCapabilityAvailable) &&
                        resume_prewarm_after_conversion) {
                        if (!selected_files.empty()) {
                            server.SetAiPrewarm(true, selected_files.front());
                        }
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
        if (!aiCapabilityChecked) {
            ImGui::TextDisabled("Backend: checking GPU...");
        } else {
            ImGui::TextDisabled("Backend: %s",
                aiCapabilityAvailable
                    ? "NVIDIA GPU (C++/CUDA)"
                    : "CPU (C++/ONNX Runtime)" );
        }
        if (conversionRunning) {
            ImGui::Text("Conversion: Running (%s)",
                conversionProcess.usedGpu ? "NVIDIA GPU" : "CPU");
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

        ImGui::Spacing();
        if (!server.IsRunning()) {
            const bool can_start = aiCapabilityChecked &&
                !selected_files.empty() && selected_address >= 0 &&
                !conversionRunning;
            if (!can_start) ImGui::BeginDisabled();
            if (ImGui::Button(uiText.startServer, ImVec2(180, 0))) {
                const bool aiEnabled = aiPassthroughEnabled();
                server.Start(
                    selected_files,
                    addresses[static_cast<size_t>(selected_address)],
                    aiEnabled,
                    selectedAiOutputMode());
            }
            if (!can_start) ImGui::EndDisabled();
        } else {
            if (ImGui::Button(uiText.stopServer, ImVec2(180, 0))) server.Stop();
        }

        ImGui::SameLine();
        ImGui::Text("%s: %s", uiText.status, server.Status().c_str());

        if (server.IsRunning()) {
            ImGui::Text("DLNA name: r800zzXRdlnaServer");
            ImGui::Text("HTTP: http://%s:%u/media/<id>", server.AdvertisedIp().c_str(), server.Port());
            ImGui::TextWrapped("%s", uiText.firewall);
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
        const float clear_color[4] = {
            241.0f / 255.0f, 204.0f / 255.0f, 165.0f / 255.0f, 1.0f
        };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT present_result = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (present_result == DXGI_STATUS_OCCLUDED);
    }

    videoPlayerProcess.Stop();
    conversionProcess.Stop(false);
    server.Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return 0;
}
