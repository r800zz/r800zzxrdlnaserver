#include "dlna_server.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <cwctype>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <windows.h>

#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <objbase.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

namespace {

std::string SanitizeWorkerLog(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool esc = false;
    bool csi = false;
    for (unsigned char ch : s) {
        if (!esc && ch == 0x1b) { esc = true; csi = false; continue; }
        if (esc) {
            if (!csi && ch == '[') { csi = true; continue; }
            if (csi) {
                if (ch >= 0x40 && ch <= 0x7e) { esc = false; csi = false; }
                continue;
            }
            esc = false;
            continue;
        }
        if (ch == '\t' || ch >= 0x20) out.push_back(static_cast<char>(ch));
    }
    return out;
}

constexpr const char* kMulticastAddress = "239.255.255.250";
constexpr uint16_t kSsdpPort = 1900;
constexpr uint16_t kFirstHttpPort = 49152;
constexpr uint16_t kLastHttpPort = 49172;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string XmlEscape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 32);
    for (char c : value) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '\"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

bool IsSupportedVideoExtension(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    return ext == ".mp4" || ext == ".m4v" || ext == ".webm" ||
           ext == ".mkv" || ext == ".avi" || ext == ".mov" ||
           ext == ".ts" || ext == ".m2ts";
}

std::string HexEncode(std::string_view value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 2);
    for (unsigned char c : value) {
        result.push_back(digits[c >> 4]);
        result.push_back(digits[c & 0x0f]);
    }
    return result;
}

std::string UrlEncodePathSegment(std::string_view value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() + 16);
    for (unsigned char c : value) {
        const bool unreserved =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~';
        if (unreserved) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(digits[c >> 4]);
            result.push_back(digits[c & 0x0f]);
        }
    }
    return result;
}

size_t ParseSizeValue(const std::string& value, size_t fallback) {
    if (value.empty()) return fallback;
    try {
        return static_cast<size_t>(std::stoull(value));
    } catch (...) {
        return fallback;
    }
}

std::string AiOutputModeArgument(AiOutputMode mode) {
    if (mode == AiOutputMode::WebmVp9Alpha) return "webm-alpha";
    if (mode == AiOutputMode::ChromaKeyHevc) return "chroma-key";
    return "alpha-packed";
}

std::string MakeUuid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        return "uuid:8d82ef1b-5581-4b91-9ed1-880022000001";
    }
    wchar_t buffer[64]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    std::wstring value(buffer);
    if (!value.empty() && value.front() == L'{') value.erase(value.begin());
    if (!value.empty() && value.back() == L'}') value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return "uuid:" + WideToUtf8(value);
}

std::string HeaderValue(const std::string& request, const std::string& header_name) {
    std::istringstream stream(request);
    std::string line;
    const std::string target = ToLower(header_name);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        if (ToLower(Trim(line.substr(0, pos))) == target) {
            return Trim(line.substr(pos + 1));
        }
    }
    return {};
}

int64_t ParseDlnaTimeSeekStartMs(const std::string& value) {
    if (value.empty()) return -1;
    const std::string lower = ToLower(value);
    const size_t npt = lower.find("npt=");
    if (npt == std::string::npos) return -1;
    size_t start = npt + 4;
    size_t end = value.find('-', start);
    if (end == std::string::npos || end <= start) return -1;
    const std::string token = Trim(value.substr(start, end - start));
    if (token.empty() || ToLower(token) == "now") return -1;
    char* parseEnd = nullptr;
    errno = 0;
    const double seconds = std::strtod(token.c_str(), &parseEnd);
    if (errno != 0 || parseEnd == token.c_str() || !std::isfinite(seconds) || seconds < 0.0) return -1;
    return static_cast<int64_t>(seconds * 1000.0 + 0.5);
}

std::string FormatNptMs(int64_t valueMs) {
    if (valueMs < 0) valueMs = 0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << (static_cast<double>(valueMs) / 1000.0);
    return out.str();
}

std::string FormatDlnaDuration(int64_t valueMs) {
    if (valueMs < 0) valueMs = 0;
    const int64_t hours = valueMs / 3600000;
    valueMs %= 3600000;
    const int64_t minutes = valueMs / 60000;
    valueMs %= 60000;
    const int64_t seconds = valueMs / 1000;
    const int64_t millis = valueMs % 1000;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << hours << ':'
        << std::setw(2) << minutes << ':'
        << std::setw(2) << seconds << '.' << std::setw(3) << millis;
    return out.str();
}

std::string FirstLine(const std::string& request) {
    const auto pos = request.find("\r\n");
    return pos == std::string::npos ? request : request.substr(0, pos);
}

bool SendAll(SOCKET socket, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        const int chunk = send(socket, data + sent,
                               static_cast<int>(std::min<size_t>(size - sent, 1u << 20)), 0);
        if (chunk <= 0) return false;
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

bool SendAll(SOCKET socket, const std::string& text) {
    return SendAll(socket, text.data(), text.size());
}

std::string HttpResponse(const std::string& body,
                         const std::string& content_type = "text/xml; charset=\"utf-8\"",
                         bool include_body = true) {
    std::ostringstream out;
    out << "HTTP/1.1 200 OK\r\n"
        << "SERVER: Windows/10.0 UPnP/1.1 R800ZZ-DLNA/1.0\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "EXT:\r\n\r\n";
    if (include_body) out << body;
    return out.str();
}

std::string HttpError(int code, const char* reason) {
    std::ostringstream body;
    body << code << " " << reason << "\n";
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << reason << "\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << body.str().size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body.str();
    return out.str();
}

bool ParseRange(const std::string& value, uint64_t total, uint64_t& start, uint64_t& end) {
    if (total == 0) return false;
    const std::string prefix = "bytes=";
    if (value.rfind(prefix, 0) != 0) return false;
    const std::string spec = value.substr(prefix.size());
    const auto dash = spec.find('-');
    if (dash == std::string::npos) return false;

    try {
        if (dash == 0) {
            const uint64_t suffix = std::stoull(spec.substr(1));
            if (suffix == 0) return false;
            start = suffix >= total ? 0 : total - suffix;
            end = total - 1;
        } else {
            start = std::stoull(spec.substr(0, dash));
            end = (dash + 1 < spec.size()) ? std::stoull(spec.substr(dash + 1)) : total - 1;
            if (start >= total) return false;
            if (end >= total) end = total - 1;
            if (end < start) return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

std::string ExtractTagValue(const std::string& body, const std::string& tag) {
    const std::string wanted = ToLower(tag);
    const std::string lower = ToLower(body);
    size_t cursor = 0;
    while ((cursor = lower.find('<', cursor)) != std::string::npos) {
        const size_t nameStart = cursor + 1;
        if (nameStart >= lower.size() || lower[nameStart] == '/' ||
            lower[nameStart] == '!' || lower[nameStart] == '?') {
            ++cursor;
            continue;
        }
        size_t nameEnd = nameStart;
        while (nameEnd < lower.size() && lower[nameEnd] != '>' &&
               !std::isspace(static_cast<unsigned char>(lower[nameEnd]))) {
            ++nameEnd;
        }
        const std::string qualified = lower.substr(nameStart, nameEnd - nameStart);
        const size_t colon = qualified.rfind(':');
        const std::string local = colon == std::string::npos
            ? qualified : qualified.substr(colon + 1);
        if (local != wanted) {
            cursor = nameEnd;
            continue;
        }
        const size_t contentStart = lower.find('>', nameEnd);
        if (contentStart == std::string::npos) return {};
        size_t close = contentStart + 1;
        while ((close = lower.find("</", close)) != std::string::npos) {
            const size_t closeNameStart = close + 2;
            size_t closeNameEnd = closeNameStart;
            while (closeNameEnd < lower.size() && lower[closeNameEnd] != '>' &&
                   !std::isspace(static_cast<unsigned char>(lower[closeNameEnd]))) {
                ++closeNameEnd;
            }
            const std::string closeQualified =
                lower.substr(closeNameStart, closeNameEnd - closeNameStart);
            const size_t closeColon = closeQualified.rfind(':');
            const std::string closeLocal = closeColon == std::string::npos
                ? closeQualified : closeQualified.substr(closeColon + 1);
            if (closeLocal == wanted) {
                return body.substr(contentStart + 1,
                                   close - (contentStart + 1));
            }
            close = closeNameEnd;
        }
        return {};
    }
    return {};
}

std::wstring QuoteWindowsArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
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

std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

struct AiProcess {
    HANDLE process = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE job = nullptr;
};

bool LaunchAiProcess(const std::filesystem::path& worker,
                     const std::filesystem::path& input,
                     int64_t start_ms,
                     AiOutputMode output_mode,
                     AiProcess& result) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
        !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        if (stdout_read) CloseHandle(stdout_read);
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_read) CloseHandle(stderr_read);
        if (stderr_write) CloseHandle(stderr_write);
        return false;
    }

    HANDLE null_input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;

    const std::filesystem::path model = ExecutableDirectory() / L"rvm_mobilenetv3_fp16.onnx";
    const std::wstring command =
        QuoteWindowsArgument(worker.wstring()) + L" " +
        QuoteWindowsArgument(input.wstring()) + L" --model " +
        QuoteWindowsArgument(model.wstring()) + L" --device 0 --qp 20 --start-ms " +
        std::to_wstring(start_ms) + L" --output-mode " +
        Utf8ToWide(AiOutputModeArgument(output_mode)) + L" --no-preview";

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    PROCESS_INFORMATION process_info{};
    const bool launched = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, ExecutableDirectory().wstring().c_str(),
        &startup, &process_info) != FALSE;

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);

    if (!launched) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, process_info.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    ResumeThread(process_info.hThread);
    CloseHandle(process_info.hThread);

    result.process = process_info.hProcess;
    result.stdout_read = stdout_read;
    result.stderr_read = stderr_read;
    result.job = job;
    return true;
}

void TerminateAiProcess(AiProcess& process) {
    if (process.job) {
        TerminateJobObject(process.job, 1);
    } else if (process.process) {
        TerminateProcess(process.process, 1);
    }
}

void CloseAiProcess(AiProcess& process) {
    if (process.stdout_read) CloseHandle(process.stdout_read);
    if (process.stderr_read) CloseHandle(process.stderr_read);
    if (process.process) CloseHandle(process.process);
    if (process.job) CloseHandle(process.job);
    process = {};
}

} // namespace

struct DlnaServer::ResidentAiState {
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE job = nullptr;
    std::thread stderr_thread;
    std::filesystem::path input;
    int width{0};
    int height{0};
    std::atomic<bool> ready{false};
    std::atomic<uint64_t> request_sequence{0};
    std::mutex control_mutex;
};

DlnaServer::DlnaServer() {
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    uuid_ = MakeUuid();
    SetStatus("Stopped");
}

DlnaServer::~DlnaServer() {
    Stop();
    StopResidentAiWorker();
    WSACleanup();
}

AiCapabilityResult DlnaServer::ProbeAiCapability() {
    AiCapabilityResult result;
    const std::filesystem::path directory = ExecutableDirectory();
    const std::filesystem::path worker = directory / L"r800zz_ai_worker.exe";
    const std::filesystem::path model = directory / L"rvm_mobilenetv3_fp16.onnx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(worker, ec)) {
        result.message = "r800zz_ai_worker.exe was not found. Realtime AI passthrough is unavailable.";
        return result;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(model, ec)) {
        result.message = "rvm_mobilenetv3_fp16.onnx was not found. Realtime AI passthrough is unavailable.";
        return result;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    if (!CreatePipe(&stderrRead, &stderrWrite, &sa, 0) ||
        !SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) {
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        result.message = "GPU capability check could not create its log pipe. Realtime AI passthrough is unavailable.";
        return result;
    }

    HANDLE nullHandle = CreateFileW(
        L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullHandle != INVALID_HANDLE_VALUE
        ? nullHandle : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = nullHandle != INVALID_HANDLE_VALUE
        ? nullHandle : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = stderrWrite;

    const std::wstring command =
        QuoteWindowsArgument(worker.wstring()) + L" --probe-gpu --model " +
        QuoteWindowsArgument(model.wstring()) + L" --device 0";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    PROCESS_INFORMATION processInfo{};
    const BOOL launched = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.wstring().c_str(),
        &startup, &processInfo);
    const DWORD launchError = launched ? ERROR_SUCCESS : GetLastError();
    CloseHandle(stderrWrite);
    stderrWrite = nullptr;
    if (nullHandle != INVALID_HANDLE_VALUE) CloseHandle(nullHandle);
    if (!launched) {
        CloseHandle(stderrRead);
        result.message = "GPU capability worker could not start (Win32=" +
            std::to_string(launchError) + "). Realtime AI passthrough is unavailable.";
        return result;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, processInfo.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    std::string workerLog;
    std::thread reader([&]() {
        std::array<char, 4096> buffer{};
        DWORD got = 0;
        while (ReadFile(stderrRead, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &got, nullptr) && got > 0) {
            workerLog.append(buffer.data(), static_cast<size_t>(got));
        }
    });

    ResumeThread(processInfo.hThread);
    CloseHandle(processInfo.hThread);
    const DWORD wait = WaitForSingleObject(processInfo.hProcess, 30000);
    const bool timedOut = wait == WAIT_TIMEOUT;
    const bool waitFailed = wait == WAIT_FAILED;
    if (timedOut || waitFailed) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, 5000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    if (reader.joinable()) reader.join();
    CloseHandle(stderrRead);
    CloseHandle(processInfo.hProcess);
    if (job) CloseHandle(job);

    auto markerText = [&](const std::string& marker) -> std::string {
        const size_t markerPos = workerLog.rfind(marker);
        if (markerPos == std::string::npos) return {};
        const size_t begin = markerPos + marker.size();
        size_t end = workerLog.find_first_of("\r\n", begin);
        if (end == std::string::npos) end = workerLog.size();
        return workerLog.substr(begin, end - begin);
    };

    if (!timedOut && !waitFailed && exitCode == 0) {
        const std::string detail = markerText("PROBE_OK ");
        result.available = true;
        result.message = detail.empty()
            ? "NVIDIA CUDA/RVM/NVENC is available."
            : detail;
        return result;
    }

    if (timedOut) {
        result.message = "GPU capability check timed out. Realtime AI passthrough is unavailable.";
    } else if (waitFailed) {
        result.message = "GPU capability check wait failed. Realtime AI passthrough is unavailable.";
    } else {
        const std::string detail = markerText("PROBE_ERROR ");
        result.message = detail.empty()
            ? "NVIDIA CUDA/RVM/NVENC is unavailable (worker exit=" +
                std::to_string(exitCode) + "). Realtime AI passthrough is unavailable."
            : detail + " Realtime AI passthrough is unavailable.";
    }
    return result;
}

bool DlnaServer::StartResidentAiWorker(const std::filesystem::path& media_file) {
    const std::filesystem::path worker = ExecutableDirectory() / L"r800zz_ai_worker.exe";
    const std::filesystem::path model = ExecutableDirectory() / L"rvm_mobilenetv3_fp16.onnx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(worker, ec)) {
        AddLog("AI prewarm error: r800zz_ai_worker.exe was not found next to the server EXE.");
        return false;
    }
    if (!std::filesystem::is_regular_file(model, ec)) {
        AddLog("AI prewarm error: rvm_mobilenetv3_fp16.onnx was not found next to the server EXE.");
        return false;
    }
    if (!std::filesystem::is_regular_file(media_file, ec)) {
        AddLog("AI prewarm deferred: select a video file first.");
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    auto closeIf = [](HANDLE& handle) {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = nullptr;
    };

    if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
        !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD win32 = GetLastError();
        closeIf(stdin_read);
        closeIf(stdin_write);
        closeIf(stderr_read);
        closeIf(stderr_write);
        AddLog("AI prewarm error: CreatePipe failed Win32=" + std::to_string(win32));
        return false;
    }

    HANDLE null_output = CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = null_output != INVALID_HANDLE_VALUE
        ? null_output : stderr_write;
    startup.hStdError = stderr_write;

    const std::wstring command =
        QuoteWindowsArgument(worker.wstring()) + L" " +
        QuoteWindowsArgument(media_file.wstring()) + L" --model " +
        QuoteWindowsArgument(model.wstring()) +
        L" --device 0 --qp 20 --resident --no-preview";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    const BOOL launched = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, ExecutableDirectory().wstring().c_str(), &startup, &pi);

    closeIf(stdin_read);
    closeIf(stderr_write);
    if (null_output != INVALID_HANDLE_VALUE) CloseHandle(null_output);

    if (!launched) {
        const DWORD win32 = GetLastError();
        closeIf(stdin_write);
        closeIf(stderr_read);
        AddLog("AI prewarm error: CreateProcess failed Win32=" + std::to_string(win32));
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    auto state = std::make_unique<ResidentAiState>();
    state->process = pi.hProcess;
    state->stdin_write = stdin_write;
    state->stderr_read = stderr_read;
    state->job = job;
    state->input = media_file;
    int64_t ignoredDuration = 0;
    ProbeMedia(media_file, ignoredDuration, state->width, state->height);
    ResidentAiState* raw = state.get();

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    state->stderr_thread = std::thread([this, raw]() {
        std::array<char, 4096> data{};
        std::string pending;
        DWORD got = 0;
        while (ReadFile(raw->stderr_read, data.data(),
                        static_cast<DWORD>(data.size()), &got, nullptr) && got > 0) {
            pending.append(data.data(), static_cast<size_t>(got));
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const std::string clean = SanitizeWorkerLog(line);
                if (clean.find("RESIDENT_READY") != std::string::npos) {
                    raw->ready.store(true);
                }
                if (!clean.empty()) AddLog("GPU: " + clean);
                pending.erase(0, pos + 1);
            }
        }
        pending = SanitizeWorkerLog(pending);
        if (!pending.empty()) AddLog("GPU: " + pending);
        raw->ready.store(false);
    });

    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        resident_ai_ = std::move(state);
    }
    AddLog("AI worker/RVM prewarm started; waiting for RESIDENT_READY.");
    return true;
}

void DlnaServer::StopResidentAiWorker() {
    std::unique_ptr<ResidentAiState> state;
    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        state = std::move(resident_ai_);
    }
    if (!state) return;

    if (state->stdin_write) {
        std::lock_guard<std::mutex> controlLock(state->control_mutex);
        const char quit[] = "QUIT\n";
        DWORD written = 0;
        WriteFile(state->stdin_write, quit,
                  static_cast<DWORD>(sizeof(quit) - 1), &written, nullptr);
        CloseHandle(state->stdin_write);
        state->stdin_write = nullptr;
    }

    if (state->process) {
        DWORD code = STILL_ACTIVE;
        if (GetExitCodeProcess(state->process, &code) && code == STILL_ACTIVE &&
            WaitForSingleObject(state->process, 3000) == WAIT_TIMEOUT) {
            if (state->job) TerminateJobObject(state->job, 1);
            else TerminateProcess(state->process, 1);
            WaitForSingleObject(state->process, 3000);
        }
    }

    if (state->stderr_thread.joinable()) state->stderr_thread.join();
    if (state->stderr_read) CloseHandle(state->stderr_read);
    if (state->process) CloseHandle(state->process);
    if (state->job) CloseHandle(state->job);
    AddLog("AI worker/RVM resident process stopped.");
}

bool DlnaServer::SetAiPrewarm(bool enabled, const std::filesystem::path& media_file) {
    if (!enabled) {
        StopResidentAiWorker();
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        if (resident_ai_ && resident_ai_->input == media_file) {
            DWORD code = STILL_ACTIVE;
            if (resident_ai_->process &&
                GetExitCodeProcess(resident_ai_->process, &code) &&
                code == STILL_ACTIVE) {
                return true;
            }
        }
    }

    StopResidentAiWorker();
    return StartResidentAiWorker(media_file);
}

bool DlnaServer::IsAiPrewarmReady() const {
    std::lock_guard<std::mutex> lock(resident_ai_mutex_);
    return resident_ai_ && resident_ai_->ready.load();
}

std::vector<std::string> DlnaServer::GetLocalIPv4Addresses() {
    std::vector<std::string> result;

    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG rc = GetAdaptersAddresses(AF_INET,
                                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, addresses, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, addresses, &size);
    }
    if (rc != NO_ERROR) return result;

    for (auto* adapter = addresses; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            char ip[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
            std::string value(ip);
            if (value.rfind("169.254.", 0) == 0 || value == "127.0.0.1") continue;
            if (std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
        }
    }
    return result;
}

std::vector<std::filesystem::path> DlnaServer::FindMediaFiles(
    const std::filesystem::path& media_directory) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(media_directory, ec)) return result;

    std::filesystem::directory_iterator iterator(
        media_directory,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::directory_iterator end;
    while (!ec && iterator != end) {
        const auto& entry = *iterator;
        std::error_code fileError;
        if (entry.is_regular_file(fileError) && !fileError &&
            IsSupportedVideoExtension(entry.path())) {
            int64_t duration = 0;
            int width = 0;
            int height = 0;
            if (ProbeMedia(entry.path(), duration, width, height)) {
                result.push_back(entry.path());
            }
        }
        iterator.increment(ec);
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        std::wstring a = left.filename().wstring();
        std::wstring b = right.filename().wstring();
        std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        return a < b;
    });
    return result;
}

bool DlnaServer::IsMediaFile(const std::filesystem::path& media_file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(media_file, ec) ||
        !IsSupportedVideoExtension(media_file)) {
        return false;
    }
    int64_t duration = 0;
    int width = 0;
    int height = 0;
    return ProbeMedia(media_file, duration, width, height);
}

bool DlnaServer::ProbeMedia(const std::filesystem::path& path,
                            int64_t& duration_ms,
                            int& width,
                            int& height) {
    duration_ms = 0;
    width = 0;
    height = 0;
    AVFormatContext* fmt = nullptr;
    const std::string input = WideToUtf8(path.wstring());
    if (input.empty() ||
        avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0 || !fmt) {
        if (fmt) avformat_close_input(&fmt);
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        duration_ms = av_rescale_q(
            fmt->duration, AV_TIME_BASE_Q, AVRational{1, 1000});
    }
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        const AVCodecParameters* parameters = fmt->streams[i]->codecpar;
        if (parameters && parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            width = parameters->width;
            height = parameters->height;
            break;
        }
    }
    avformat_close_input(&fmt);
    return width > 0 && height > 0;
}

bool DlnaServer::Start(const std::vector<std::filesystem::path>& media_files,
                       const std::string& advertised_ip,
                       bool ai_passthrough,
                       AiOutputMode ai_output_mode) {
    Stop();

    if (media_files.empty()) {
        SetStatus("No video files are selected.");
        return false;
    }
    if (advertised_ip.empty()) {
        SetStatus("No local IPv4 address selected.");
        return false;
    }

    media_items_.clear();
    std::error_code ec;
    std::set<std::filesystem::path> seenFiles;
    uint32_t nextId = 1;
    for (const auto& file : media_files) {
        const std::filesystem::path normalized =
            std::filesystem::absolute(file, ec).lexically_normal();
        if (ec) {
            ec.clear();
            continue;
        }
        if (!seenFiles.insert(normalized).second) continue;
        MediaItem item;
        item.path = normalized;
        item.size = std::filesystem::file_size(normalized, ec);
        if (ec) {
            ec.clear();
            item.size = 0;
        }
        if (!ProbeMedia(normalized, item.duration_ms, item.width, item.height)) {
            AddLog("Skipped unreadable video: " +
                   WideToUtf8(normalized.filename().wstring()));
            continue;
        }
        item.id = nextId++;
        media_items_.push_back(std::move(item));
    }
    if (media_items_.empty()) {
        SetStatus("No supported video files were found in the selection.");
        return false;
    }
    advertised_ip_ = advertised_ip;
    ai_passthrough_ = ai_passthrough;
    ai_output_mode_ = ai_output_mode;
    if (ai_passthrough_) {
        SetAiPrewarm(true, media_items_.front().path);
    } else {
        SetAiPrewarm(false, {});
    }
    uuid_ = MakeUuid();

    if (!SetupHttpListener()) {
        SetStatus("Could not open an HTTP port.");
        return false;
    }
    if (!SetupSsdpSocket()) {
        closesocket(http_listen_socket_);
        http_listen_socket_ = INVALID_SOCKET;
        SetStatus("Could not open SSDP port 1900. Check firewall/network settings.");
        return false;
    }

    running_ = true;
    SetStatus("Running");
    AddLog("Build: DeoVR_Search_NoEraseRedraw_SkyBlueUI_TitleLinks");
    AddLog("DLNA server started: http://" + advertised_ip_ + ":" +
           std::to_string(http_port_) + "/media/<id>");
    AddLog("Video files: " + std::to_string(media_items_.size()));
    AddLog(std::string("AI Passthrough: ") + (ai_passthrough_ ? "ON" : "OFF"));
    if (ai_passthrough_) AddLog("AI Output: " + AiOutputLabel());

    http_thread_ = std::thread(&DlnaServer::HttpLoop, this);
    ssdp_thread_ = std::thread(&DlnaServer::SsdpLoop, this);
    SendAliveNotifications();
    return true;
}

void DlnaServer::Stop() {
    const bool was_running = running_.exchange(false);

    if (was_running) {
        SendByebyeNotifications();
    }

    if (http_listen_socket_ != INVALID_SOCKET) {
        closesocket(http_listen_socket_);
        http_listen_socket_ = INVALID_SOCKET;
    }
    if (ssdp_socket_ != INVALID_SOCKET) {
        closesocket(ssdp_socket_);
        ssdp_socket_ = INVALID_SOCKET;
    }

    // Stop accepting clients first so no new HTTP worker can be registered.
    if (http_thread_.joinable()) http_thread_.join();
    if (ssdp_thread_.joinable()) ssdp_thread_.join();

    // Wake any worker that is blocked in recv()/send(). Each worker owns and
    // closes its socket after HandleHttpClient() returns.
    {
        std::lock_guard<std::mutex> lock(http_clients_mutex_);
        for (SOCKET client : active_http_clients_) {
            shutdown(client, SD_BOTH);
        }
    }

    // Detached HTTP workers may still be unwinding after shutdown().
    // Wait until all of them have removed themselves before Start() reuses
    // server state or the DlnaServer object is destroyed.
    {
        std::unique_lock<std::mutex> lock(http_clients_mutex_);
        http_clients_cv_.wait(lock, [this]() {
            return active_http_clients_.empty();
        });
    }

    if (was_running) {
        SetStatus("Stopped");
        AddLog("DLNA server stopped.");
    }
}

std::string DlnaServer::AdvertisedIp() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return advertised_ip_;
}

std::string DlnaServer::Status() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_;
}

std::vector<std::string> DlnaServer::Logs() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return logs_;
}

void DlnaServer::ClearLogs() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    logs_.clear();
}

void DlnaServer::RecordAiCapabilityResult(const AiCapabilityResult& result) {
    AddLog(std::string("AI capability: ") +
           (result.available ? "AVAILABLE - " : "UNAVAILABLE - ") +
           result.message);
}

bool DlnaServer::SetupHttpListener() {
    http_listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (http_listen_socket_ == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(http_listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    for (uint16_t port = kFirstHttpPort; port <= kLastHttpPort; ++port) {
        address.sin_port = htons(port);
        if (bind(http_listen_socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            if (listen(http_listen_socket_, SOMAXCONN) == 0) {
                http_port_ = port;
                return true;
            }
            break;
        }
    }

    closesocket(http_listen_socket_);
    http_listen_socket_ = INVALID_SOCKET;
    return false;
}

bool DlnaServer::SetupSsdpSocket() {
    ssdp_socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ssdp_socket_ == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(ssdp_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(kSsdpPort);
    if (bind(ssdp_socket_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closesocket(ssdp_socket_);
        ssdp_socket_ = INVALID_SOCKET;
        return false;
    }

    ip_mreq membership{};
    inet_pton(AF_INET, kMulticastAddress, &membership.imr_multiaddr);
    inet_pton(AF_INET, advertised_ip_.c_str(), &membership.imr_interface);
    if (setsockopt(ssdp_socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(ssdp_socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
            closesocket(ssdp_socket_);
            ssdp_socket_ = INVALID_SOCKET;
            return false;
        }
    }

    in_addr iface{};
    inet_pton(AF_INET, advertised_ip_.c_str(), &iface);
    setsockopt(ssdp_socket_, IPPROTO_IP, IP_MULTICAST_IF,
               reinterpret_cast<const char*>(&iface), sizeof(iface));
    return true;
}

void DlnaServer::HttpLoop() {
    while (running_) {
        sockaddr_in client_address{};
        int length = sizeof(client_address);
        SOCKET client = accept(http_listen_socket_, reinterpret_cast<sockaddr*>(&client_address), &length);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // A DLNA seek opens a new HTTP Range connection while the previous
        // media connection may still be blocked in send(). Handle each client
        // independently so the old stream cannot delay the new Range request.
        {
            std::lock_guard<std::mutex> lock(http_clients_mutex_);
            if (!running_) {
                closesocket(client);
                break;
            }
            active_http_clients_.push_back(client);
        }

        try {
            std::thread([this, client]() {
                try {
                    HandleHttpClient(client);
                } catch (...) {
                    closesocket(client);
                    AddLog("HTTP client worker terminated by an exception.");
                }
                FinishHttpClient(client);
            }).detach();
        } catch (...) {
            closesocket(client);
            FinishHttpClient(client);
            AddLog("Could not start HTTP client worker.");
        }
    }
}

void DlnaServer::FinishHttpClient(SOCKET client) {
    {
        std::lock_guard<std::mutex> lock(http_clients_mutex_);
        const auto it =
            std::find(active_http_clients_.begin(),
                      active_http_clients_.end(),
                      client);
        if (it != active_http_clients_.end()) {
            active_http_clients_.erase(it);
        }
    }
    http_clients_cv_.notify_all();
}

void DlnaServer::SsdpLoop() {
    std::array<char, 8192> buffer{};
    while (running_) {
        sockaddr_in from{};
        int from_length = sizeof(from);
        const int received = recvfrom(ssdp_socket_, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                                      reinterpret_cast<sockaddr*>(&from), &from_length);
        if (received <= 0) {
            if (!running_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        buffer[static_cast<size_t>(received)] = '\0';
        std::string request(buffer.data(), static_cast<size_t>(received));
        const std::string lower = ToLower(request);
        if (lower.rfind("m-search * http/1.1", 0) != 0) continue;

        std::string st = HeaderValue(request, "st");
        const std::string st_lower = ToLower(st);
        char sourceAddress[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, sourceAddress,
                  std::size(sourceAddress));
        AddLog("SSDP M-SEARCH from=" + std::string(sourceAddress) + ":" +
               std::to_string(ntohs(from.sin_port)) + " ST=" + st);

        const auto targets = SsdpTargets();
        if (st_lower == "ssdp:all") {
            for (const auto& [target, unusedUsn] : targets) {
                (void)unusedUsn;
                SendSsdpResponse(from, target);
            }
        } else if (st_lower == ToLower(uuid_)) {
            SendSsdpResponse(from, uuid_);
        } else if (st_lower == "upnp:rootdevice") {
            SendSsdpResponse(from, "upnp:rootdevice");
        } else if (st_lower.find("mediaserver") != std::string::npos) {
            SendSsdpResponse(from, "urn:schemas-upnp-org:device:MediaServer:1");
        } else if (st_lower.find("contentdirectory") != std::string::npos) {
            SendSsdpResponse(from, "urn:schemas-upnp-org:service:ContentDirectory:1");
        } else if (st_lower.find("connectionmanager") != std::string::npos) {
            SendSsdpResponse(from, "urn:schemas-upnp-org:service:ConnectionManager:1");
        }
    }
}

std::vector<std::pair<std::string, std::string>> DlnaServer::SsdpTargets() const {
    return {
        {"upnp:rootdevice", uuid_ + "::upnp:rootdevice"},
        {uuid_, uuid_},
        {"urn:schemas-upnp-org:device:MediaServer:1",
         uuid_ + "::urn:schemas-upnp-org:device:MediaServer:1"},
        {"urn:schemas-upnp-org:service:ContentDirectory:1",
         uuid_ + "::urn:schemas-upnp-org:service:ContentDirectory:1"},
        {"urn:schemas-upnp-org:service:ConnectionManager:1",
         uuid_ + "::urn:schemas-upnp-org:service:ConnectionManager:1"}
    };
}

void DlnaServer::SendSsdpResponse(const sockaddr_in& destination, const std::string& st) {
    std::string usn = uuid_;
    for (const auto& [target, targetUsn] : SsdpTargets()) {
        if (ToLower(target) == ToLower(st)) {
            usn = targetUsn;
            break;
        }
    }

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n"
             << "CACHE-CONTROL: max-age=1800\r\n"
             << "EXT:\r\n"
             << "LOCATION: http://" << advertised_ip_ << ':' << http_port_ << "/device.xml\r\n"
             << "SERVER: Windows/10.0 UPnP/1.1 R800ZZ-DLNA/1.0\r\n"
             << "BOOTID.UPNP.ORG: 1\r\n"
             << "CONFIGID.UPNP.ORG: 1\r\n"
             << "ST: " << st << "\r\n"
             << "USN: " << usn << "\r\n\r\n";

    const std::string text = response.str();
    sendto(ssdp_socket_, text.data(), static_cast<int>(text.size()), 0,
           reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
}

void DlnaServer::SendAliveNotifications() {
    if (ssdp_socket_ == INVALID_SOCKET) return;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kSsdpPort);
    inet_pton(AF_INET, kMulticastAddress, &destination.sin_addr);

    for (const auto& [nt, usn] : SsdpTargets()) {
        std::ostringstream msg;
        msg << "NOTIFY * HTTP/1.1\r\n"
            << "HOST: " << kMulticastAddress << ':' << kSsdpPort << "\r\n"
            << "CACHE-CONTROL: max-age=1800\r\n"
            << "LOCATION: http://" << advertised_ip_ << ':' << http_port_ << "/device.xml\r\n"
            << "NT: " << nt << "\r\n"
            << "NTS: ssdp:alive\r\n"
            << "SERVER: Windows/10.0 UPnP/1.1 R800ZZ-DLNA/1.0\r\n"
            << "BOOTID.UPNP.ORG: 1\r\n"
            << "CONFIGID.UPNP.ORG: 1\r\n"
            << "USN: " << usn << "\r\n\r\n";
        const std::string text = msg.str();
        sendto(ssdp_socket_, text.data(), static_cast<int>(text.size()), 0,
               reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    }
}

void DlnaServer::SendByebyeNotifications() {
    if (ssdp_socket_ == INVALID_SOCKET) return;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kSsdpPort);
    inet_pton(AF_INET, kMulticastAddress, &destination.sin_addr);

    for (const auto& [nt, usn] : SsdpTargets()) {
        std::ostringstream msg;
        msg << "NOTIFY * HTTP/1.1\r\n"
            << "HOST: " << kMulticastAddress << ':' << kSsdpPort << "\r\n"
            << "NT: " << nt << "\r\n"
            << "NTS: ssdp:byebye\r\n"
            << "BOOTID.UPNP.ORG: 1\r\n"
            << "CONFIGID.UPNP.ORG: 1\r\n"
            << "USN: " << usn << "\r\n\r\n";
        const std::string text = msg.str();
        sendto(ssdp_socket_, text.data(), static_cast<int>(text.size()), 0,
               reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    }
}

void DlnaServer::HandleHttpClient(SOCKET client) {
    DWORD timeout = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string request;
    std::array<char, 16384> buffer{};
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    while (request.size() < 1024 * 1024) {
        const int got = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (got <= 0) break;
        request.append(buffer.data(), static_cast<size_t>(got));
        header_end = request.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::string cl = HeaderValue(request.substr(0, header_end + 2), "content-length");
            if (!cl.empty()) {
                try { content_length = static_cast<size_t>(std::stoull(cl)); } catch (...) { content_length = 0; }
            }
            if (request.size() >= header_end + 4 + content_length) break;
        }
    }

    if (request.empty()) {
        closesocket(client);
        return;
    }

    std::istringstream first(FirstLine(request));
    std::string method, path, version;
    first >> method >> path >> version;
    std::string routePath = path;
    const size_t routeQuery = routePath.find('?');
    if (routeQuery != std::string::npos) routePath.resize(routeQuery);
    const std::string routePathLower = ToLower(routePath);

    if (routePathLower == "/device.xml" ||
        routePathLower.rfind("/contentdirectory/", 0) == 0 ||
        routePathLower.rfind("/connectionmanager/", 0) == 0) {
        std::string discoveryLog = "UPnP HTTP " + FirstLine(request);
        const std::string soapAction = HeaderValue(request, "soapaction");
        if (!soapAction.empty()) discoveryLog += " SOAPAction=" + soapAction;
        AddLog(discoveryLog);
    }

    // Log the complete HTTP request header for media requests.
    if (path == "/media" || path.rfind("/media/", 0) == 0) {
        const size_t request_header_end = request.find("\r\n\r\n");
        const std::string request_headers =
            request.substr(0, request_header_end == std::string::npos ? request.size() : request_header_end);
        AddLog("===== HTTP REQUEST " + path + " =====\n" + request_headers);
    }

    if ((method == "GET" || method == "HEAD") && routePathLower == "/device.xml") {
        SendAll(client, HttpResponse(DeviceDescriptionXml(),
                                     "text/xml; charset=\"utf-8\"", method != "HEAD"));
    } else if ((method == "GET" || method == "HEAD") && routePathLower == "/contentdirectory/scpd.xml") {
        SendAll(client, HttpResponse(ContentDirectoryScpdXml(),
                                     "text/xml; charset=\"utf-8\"", method != "HEAD"));
    } else if ((method == "GET" || method == "HEAD") && routePathLower == "/connectionmanager/scpd.xml") {
        SendAll(client, HttpResponse(ConnectionManagerScpdXml(),
                                     "text/xml; charset=\"utf-8\"", method != "HEAD"));
    } else if (method == "POST" && routePathLower == "/contentdirectory/control") {
        const std::string action = HeaderValue(request, "soapaction");
        const std::string actionProbe = ToLower(action + "\n" + request);
        if (actionProbe.find("#browse") != std::string::npos ||
            actionProbe.find("<u:browse") != std::string::npos ||
            actionProbe.find("<browse") != std::string::npos) {
            const std::string browseFlag = ExtractTagValue(request, "BrowseFlag");
            const bool metadata = ToLower(browseFlag) == "browsemetadata";
            std::string objectId = ExtractTagValue(request, "ObjectID");
            if (objectId.empty()) objectId = "0";
            const size_t startingIndex = ParseSizeValue(
                ExtractTagValue(request, "StartingIndex"), 0);
            const size_t requestedCount = ParseSizeValue(
                ExtractTagValue(request, "RequestedCount"), 0);
            AddLog("ContentDirectory Browse object=" + objectId +
                   " flag=" + browseFlag +
                   " start=" + std::to_string(startingIndex) +
                   " requested=" + std::to_string(requestedCount) +
                   " libraryItems=" + std::to_string(media_items_.size()));
            SendAll(client, HttpResponse(BrowseSoapResponse(
                objectId, metadata, startingIndex, requestedCount)));
        } else if (actionProbe.find("#search") != std::string::npos ||
                   actionProbe.find("<u:search") != std::string::npos ||
                   actionProbe.find("<search") != std::string::npos) {
            const size_t startingIndex = ParseSizeValue(
                ExtractTagValue(request, "StartingIndex"), 0);
            const size_t requestedCount = ParseSizeValue(
                ExtractTagValue(request, "RequestedCount"), 0);
            std::string criteria = ExtractTagValue(request, "SearchCriteria");
            if (criteria.size() > 240) criteria.resize(240);
            AddLog("ContentDirectory Search criteria=" + criteria +
                   " start=" + std::to_string(startingIndex) +
                   " requested=" + std::to_string(requestedCount) +
                   " libraryItems=" + std::to_string(media_items_.size()));
            SendAll(client, HttpResponse(BrowseSoapResponse(
                "0", false, startingIndex, requestedCount, "Search")));
        } else if (actionProbe.find("#getsearchcapabilities") != std::string::npos) {
            SendAll(client, HttpResponse(SimpleSoapResponse("ContentDirectory", "GetSearchCapabilities", "<SearchCaps>upnp:class,dc:title</SearchCaps>")));
        } else if (actionProbe.find("#getsortcapabilities") != std::string::npos) {
            SendAll(client, HttpResponse(SimpleSoapResponse("ContentDirectory", "GetSortCapabilities", "<SortCaps>dc:title</SortCaps>")));
        } else if (actionProbe.find("#getsystemupdateid") != std::string::npos) {
            SendAll(client, HttpResponse(SimpleSoapResponse("ContentDirectory", "GetSystemUpdateID", "<Id>1</Id>")));
        } else {
            SendAll(client, HttpError(500, "Unsupported SOAP Action"));
        }
    } else if (method == "POST" && routePathLower == "/connectionmanager/control") {
        const std::string action = HeaderValue(request, "soapaction");
        if (action.find("#GetProtocolInfo") != std::string::npos) {
            const std::string inner = "<Source>" + XmlEscape(DlnaProtocolInfo()) + "</Source><Sink></Sink>";
            SendAll(client, HttpResponse(SimpleSoapResponse("ConnectionManager", "GetProtocolInfo", inner)));
        } else if (action.find("#GetCurrentConnectionIDs") != std::string::npos) {
            SendAll(client, HttpResponse(SimpleSoapResponse("ConnectionManager", "GetCurrentConnectionIDs", "<ConnectionIDs>0</ConnectionIDs>")));
        } else if (action.find("#GetCurrentConnectionInfo") != std::string::npos) {
            const std::string inner =
                "<RcsID>-1</RcsID><AVTransportID>-1</AVTransportID>"
                "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
                "<PeerConnectionID>-1</PeerConnectionID><Direction>Output</Direction>"
                "<Status>OK</Status>";
            SendAll(client, HttpResponse(SimpleSoapResponse(
                "ConnectionManager", "GetCurrentConnectionInfo", inner)));
        } else {
            SendAll(client, HttpError(500, "Unsupported SOAP Action"));
        }
    } else if (method == "SUBSCRIBE") {
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "SID: uuid:r800zz-subscription\r\n"
                 << "TIMEOUT: Second-1800\r\n"
                 << "Content-Length: 0\r\n\r\n";
        SendAll(client, response.str());
    } else if (method == "UNSUBSCRIBE") {
        SendAll(client, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    } else if ((method == "GET" || method == "HEAD") &&
               (path == "/media" || path.rfind("/media/", 0) == 0)) {
        const MediaItem* item = FindMediaItem(path);
        if (!item) {
            SendAll(client, HttpError(404, "Not Found"));
            closesocket(client);
            return;
        }
        if (ai_passthrough_) {
            HandleAiPassthroughStream(client, method, request, *item);
            closesocket(client);
            return;
        }

        const uint64_t total = item->size;
        if (total == 0) {
            SendAll(client, HttpError(404, "Not Found"));
            closesocket(client);
            return;
        }

        uint64_t start = 0;
        uint64_t end = total - 1;
        const std::string range = HeaderValue(request, "range");
        const bool has_range = !range.empty();
        const bool partial = has_range && ParseRange(range, total, start, end);

        if (has_range && !partial) {
            std::ostringstream response;
            response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                     << "Content-Range: bytes */" << total << "\r\n"
                     << "Accept-Ranges: bytes\r\n"
                     << "Content-Length: 0\r\n"
                     << "Connection: close\r\n\r\n";
            AddLog(method + " /media " + range + " -> 416");
            AddLog("===== HTTP RESPONSE /media =====\n" + response.str().substr(0, response.str().find("\r\n\r\n")));
            SendAll(client, response.str());
            closesocket(client);
            return;
        }

        const uint64_t length64 = end - start + 1;

        std::ostringstream headers;
        headers << "HTTP/1.1 " << (partial ? "206 Partial Content" : "200 OK") << "\r\n"
                << "Content-Type: " << MimeType(*item) << "\r\n"
                << "Accept-Ranges: bytes\r\n"
                << "transferMode.dlna.org: Streaming\r\n"
                << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n"
                << "EXT:\r\n"
                << "Content-Length: " << length64 << "\r\n";
        if (partial) {
            headers << "Content-Range: bytes " << start << '-' << end << '/' << total << "\r\n";
        }
        headers << "Connection: close\r\n\r\n";

        if (partial) {
            AddLog(method + " /media " + range + " -> 206 bytes " +
                   std::to_string(start) + "-" + std::to_string(end));
        } else {
            AddLog(method + " /media -> 200");
        }
        {
            const std::string response_headers = headers.str();
            const size_t response_header_end = response_headers.find("\r\n\r\n");
            AddLog("===== HTTP RESPONSE /media =====\n" +
                   response_headers.substr(0, response_header_end == std::string::npos
                                                  ? response_headers.size()
                                                  : response_header_end));
        }
        if (!SendAll(client, headers.str()) || method == "HEAD") {
            closesocket(client);
            return;
        }

        std::ifstream file(item->path, std::ios::binary);
        if (file) {
            file.seekg(static_cast<std::streamoff>(start));
            uint64_t remaining = length64;
            uint64_t sent_body = 0;
            bool send_failed = false;
            std::array<char, 256 * 1024> file_buffer{};
            while (remaining > 0 && file && running_) {
                const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, file_buffer.size()));
                file.read(file_buffer.data(), static_cast<std::streamsize>(want));
                const std::streamsize got = file.gcount();
                if (got <= 0) break;
                if (!SendAll(client, file_buffer.data(), static_cast<size_t>(got))) {
                    send_failed = true;
                    break;
                }
                sent_body += static_cast<uint64_t>(got);
                remaining -= static_cast<uint64_t>(got);
            }
            AddLog("/media body sent=" + std::to_string(sent_body) +
                   " expected=" + std::to_string(length64) +
                   (send_failed ? " (client disconnected/send failed)" : ""));
        } else {
            AddLog("/media failed to open source file");
        }
    } else {
        SendAll(client, HttpError(404, "Not Found"));
    }

    closesocket(client);
}

void DlnaServer::HandleAiPassthroughStream(SOCKET client,
                                            const std::string& method,
                                            const std::string& request,
                                            const MediaItem& item) {
    // AI output is generated on demand, so byte-range seek is not advertised.
    // A fresh response can instead be generated from a DLNA time position.
    const std::string byteRange = HeaderValue(request, "range");
    if (!byteRange.empty()) {
        std::ostringstream response;
        response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                 << "Accept-Ranges: none\r\n"
                 << "Content-Length: 0\r\n"
                 << "Connection: close\r\n\r\n";
        AddLog(method + " /media " + byteRange + " -> 416 (use DLNA TimeSeekRange)");
        SendAll(client, response.str());
        return;
    }

    int64_t requested_start_ms = 0;
    bool time_seek_request = false;
    const std::string timeSeek = HeaderValue(request, "timeseekrange.dlna.org");
    const int64_t dlnaStartMs = ParseDlnaTimeSeekStartMs(timeSeek);
    if (dlnaStartMs >= 0) {
        requested_start_ms = dlnaStartMs;
        time_seek_request = true;
    } else {
        // Backward-compatible fallback for older R800ZZ VR Player builds.
        const std::string startHeader = HeaderValue(request, "x-r800zz-start-ms");
        if (!startHeader.empty()) {
            char* end = nullptr;
            const long long parsed = std::strtoll(startHeader.c_str(), &end, 10);
            if (end != startHeader.c_str() && parsed >= 0) {
                requested_start_ms = static_cast<int64_t>(parsed);
                time_seek_request = requested_start_ms > 0;
            }
        }
    }
    if (item.duration_ms > 0) {
        requested_start_ms = std::min(requested_start_ms, item.duration_ms);
    }

    // HEAD is a file/capability probe and never starts the GPU worker.
    if (method == "HEAD") {
        std::ostringstream headers;
        headers << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: " << MimeType(item) << "\r\n"
                << "Accept-Ranges: none\r\n"
                << "transferMode.dlna.org: Streaming\r\n"
                << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n";
        if (item.duration_ms > 0) {
            const std::string duration = FormatNptMs(item.duration_ms);
            headers << "X-R800ZZ-Duration-Ms: " << item.duration_ms << "\r\n"
                    << "X-AvailableSeekRange: 1 npt=0.000-" << duration << "\r\n";
        }
        headers << "EXT:\r\n"
                << "Connection: close\r\n\r\n";
        AddLog("HEAD /media -> 200 AI file capability probe");
        SendAll(client, headers.str());
        return;
    }

    const std::filesystem::path worker = ExecutableDirectory() / L"r800zz_ai_worker.exe";
    const std::filesystem::path model = ExecutableDirectory() / L"rvm_mobilenetv3_fp16.onnx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(worker, ec)) {
        AddLog("AI Passthrough error: r800zz_ai_worker.exe was not found next to the server EXE.");
        SendAll(client, HttpError(500, "AI Worker Missing"));
        return;
    }
    if (!std::filesystem::is_regular_file(model, ec)) {
        AddLog("AI Passthrough error: rvm_mobilenetv3_fp16.onnx was not found next to the server EXE.");
        SendAll(client, HttpError(500, "RVM Model Missing"));
        return;
    }

    ResidentAiState* resident = nullptr;
    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        if (resident_ai_ && resident_ai_->width == item.width &&
            resident_ai_->height == item.height) {
            DWORD code = STILL_ACTIVE;
            if (resident_ai_->process &&
                GetExitCodeProcess(resident_ai_->process, &code) &&
                code == STILL_ACTIVE) {
                resident = resident_ai_.get();
            }
        }
    }

    if (resident) {
        const auto readyBegin = std::chrono::steady_clock::now();
        while (!resident->ready.load()) {
            DWORD code = STILL_ACTIVE;
            if (!GetExitCodeProcess(resident->process, &code) || code != STILL_ACTIVE) {
                AddLog("AI resident worker exited before becoming ready; using one-shot worker.");
                resident = nullptr;
                break;
            }
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - readyBegin).count() >= 120) {
                AddLog("AI resident worker warmup timed out; using one-shot worker.");
                resident = nullptr;
                break;
            }
            Sleep(10);
        }
    }

    if (resident) {
        const uint64_t sequence = resident->request_sequence.fetch_add(1) + 1;
        const std::string pipeName =
            "\\\\.\\pipe\\r800zz_ai_" + std::to_string(GetCurrentProcessId()) +
            "_" + std::to_string(sequence);
        HANDLE streamPipe = CreateNamedPipeA(
            pipeName.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
            1, 256 * 1024, 256 * 1024, 0, nullptr);

        if (streamPipe == INVALID_HANDLE_VALUE) {
            AddLog("AI resident output pipe creation failed Win32=" +
                   std::to_string(GetLastError()) + "; using one-shot worker.");
            resident = nullptr;
        } else {
            const std::string runCommand =
                "RUN " + std::to_string(sequence) + " " +
                std::to_string(requested_start_ms) + " " +
                AiOutputModeArgument(ai_output_mode_) + " " + pipeName + " " +
                HexEncode(WideToUtf8(item.path.wstring())) + "\n";
            bool commandOk = false;
            {
                std::lock_guard<std::mutex> controlLock(resident->control_mutex);
                DWORD written = 0;
                commandOk = WriteFile(
                    resident->stdin_write, runCommand.data(),
                    static_cast<DWORD>(runCommand.size()), &written, nullptr) &&
                    written == runCommand.size();
            }

            if (!commandOk) {
                AddLog("AI resident RUN command failed Win32=" +
                       std::to_string(GetLastError()) + "; using one-shot worker.");
                CloseHandle(streamPipe);
                resident = nullptr;
            } else {
                AddLog("AI Passthrough using resident worker/RVM session.");
                if (requested_start_ms > 0) {
                    AddLog("AI Passthrough seek request startMs=" +
                           std::to_string(requested_start_ms));
                }

                bool connected = false;
                bool startupFailed = false;
                bool startupTimeout = false;
                std::array<char, 256 * 1024> data{};
                DWORD firstGot = 0;
                const auto startupBegin = std::chrono::steady_clock::now();

                while (!connected) {
                    if (ConnectNamedPipe(streamPipe, nullptr)) {
                        connected = true;
                        break;
                    }
                    const DWORD win32 = GetLastError();
                    if (win32 == ERROR_PIPE_CONNECTED) {
                        connected = true;
                        break;
                    }
                    if (win32 != ERROR_PIPE_LISTENING && win32 != ERROR_NO_DATA) {
                        startupFailed = true;
                        break;
                    }
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - startupBegin).count() >= 120) {
                        startupTimeout = true;
                        break;
                    }
                    Sleep(5);
                }

                while (connected && !startupFailed && !startupTimeout && firstGot == 0) {
                    DWORD available = 0;
                    if (!PeekNamedPipe(streamPipe, nullptr, 0, nullptr, &available, nullptr)) {
                        startupFailed = true;
                        break;
                    }
                    if (available > 0) {
                        const DWORD want = static_cast<DWORD>(
                            std::min<size_t>(data.size(), available));
                        if (!ReadFile(streamPipe, data.data(), want, &firstGot, nullptr) ||
                            firstGot == 0) {
                            startupFailed = true;
                        }
                        break;
                    }
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - startupBegin).count() >= 120) {
                        startupTimeout = true;
                        break;
                    }
                    Sleep(5);
                }

                if (startupFailed || startupTimeout || firstGot == 0) {
                    CloseHandle(streamPipe);
                    {
                        std::lock_guard<std::mutex> controlLock(resident->control_mutex);
                        const std::string cancel =
                            "CANCEL " + std::to_string(sequence) + "\n";
                        DWORD ignored = 0;
                        WriteFile(resident->stdin_write, cancel.data(),
                                  static_cast<DWORD>(cancel.size()), &ignored, nullptr);
                    }
                    AddLog(startupTimeout
                        ? "AI resident stream startup timed out; using one-shot worker."
                        : "AI resident stream failed before first output byte; using one-shot worker.");
                    resident = nullptr;
                } else {
                    const auto startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startupBegin).count();
                    AddLog("AI resident first output bytes ready=" +
                           std::to_string(firstGot) +
                           " startupMs=" + std::to_string(startupMs));

                    std::ostringstream headers;
                    headers << "HTTP/1.1 "
                            << (time_seek_request ? "206 Partial Content" : "200 OK") << "\r\n"
                            << "Content-Type: " << MimeType(item) << "\r\n"
                            << "Accept-Ranges: none\r\n"
                            << "transferMode.dlna.org: Streaming\r\n"
                            << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n";
                    if (item.duration_ms > 0) {
                        const std::string duration = FormatNptMs(item.duration_ms);
                        const std::string begin = FormatNptMs(requested_start_ms);
                        headers << "X-R800ZZ-Duration-Ms: " << item.duration_ms << "\r\n"
                                << "X-AvailableSeekRange: 1 npt=0.000-" << duration << "\r\n"
                                << "TimeSeekRange.dlna.org: npt=" << begin << '-'
                                << duration << '/' << duration << "\r\n";
                    }
                    if (requested_start_ms > 0) {
                        headers << "X-R800ZZ-Start-Ms: " << requested_start_ms << "\r\n";
                    }
                    headers << "EXT:\r\nConnection: close\r\n\r\n";

                    bool sendFailed = !SendAll(client, headers.str());
                    uint64_t sentBody = 0;
                    if (!sendFailed) {
                        sendFailed = !SendAll(client, data.data(), static_cast<size_t>(firstGot));
                        if (!sendFailed) sentBody += firstGot;
                    }

                    while (!sendFailed && running_) {
                        DWORD available = 0;
                        if (!PeekNamedPipe(streamPipe, nullptr, 0, nullptr,
                                           &available, nullptr)) {
                            const DWORD win32 = GetLastError();
                            if (win32 != ERROR_BROKEN_PIPE &&
                                win32 != ERROR_PIPE_NOT_CONNECTED) {
                                sendFailed = true;
                            }
                            break;
                        }
                        if (available == 0) {
                            Sleep(2);
                            continue;
                        }
                        DWORD got = 0;
                        const DWORD want = static_cast<DWORD>(
                            std::min<size_t>(data.size(), available));
                        if (!ReadFile(streamPipe, data.data(), want, &got, nullptr) || got == 0) {
                            sendFailed = true;
                            break;
                        }
                        if (!SendAll(client, data.data(), static_cast<size_t>(got))) {
                            sendFailed = true;
                            break;
                        }
                        sentBody += got;
                    }

                    CloseHandle(streamPipe);
                    if (sendFailed || !running_) {
                        std::lock_guard<std::mutex> controlLock(resident->control_mutex);
                        const std::string cancel =
                            "CANCEL " + std::to_string(sequence) + "\n";
                        DWORD ignored = 0;
                        WriteFile(resident->stdin_write, cancel.data(),
                                  static_cast<DWORD>(cancel.size()), &ignored, nullptr);
                    }

                    AddLog("AI /media resident body sent=" + std::to_string(sentBody) +
                           (sendFailed ? " (client disconnected/send failed)" : ""));
                    return;
                }
            }
        }
    }

    // Start the GPU worker BEFORE sending HTTP 200.  The previous build sent
    // 200 first and then initialized CUDA/ONNX/NVENC.  Some clients time out
    // when the response body remains empty during that initialization window.
    AiProcess process;
    if (!LaunchAiProcess(worker, item.path, requested_start_ms,
                         ai_output_mode_, process)) {
        AddLog("AI Passthrough error: could not start r800zz_ai_worker.exe. Win32=" +
               std::to_string(GetLastError()));
        SendAll(client, HttpError(500, "AI Worker Start Failed"));
        return;
    }

    AddLog("AI Passthrough C++ GPU worker starting; waiting for first output bytes before HTTP 200. RVM uses ONNX Runtime CUDA EP with GPU I/O binding. Output=" + AiOutputLabel());
    if (requested_start_ms > 0) {
        AddLog("AI Passthrough seek request startMs=" + std::to_string(requested_start_ms));
    }

    std::atomic<int64_t> source_duration_ms{0};
    std::thread stderr_thread([this, handle = process.stderr_read, &source_duration_ms]() {
        std::array<char, 4096> data{};
        std::string pending;
        DWORD got = 0;
        while (ReadFile(handle, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got > 0) {
            pending.append(data.data(), static_cast<size_t>(got));
            size_t pos = 0;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const std::string metaKey = "CPP_GPU: META durationMs=";
                const size_t metaPos = line.find(metaKey);
                if (metaPos != std::string::npos) {
                    const char* value = line.c_str() + metaPos + metaKey.size();
                    char* end = nullptr;
                    const long long parsed = std::strtoll(value, &end, 10);
                    if (end != value && parsed > 0) {
                        source_duration_ms.store(static_cast<int64_t>(parsed));
                    }
                }
                line = SanitizeWorkerLog(line);
                if (!line.empty()) AddLog("GPU: " + line);
                pending.erase(0, pos + 1);
            }
        }
        pending = SanitizeWorkerLog(pending);
        if (!pending.empty()) AddLog("GPU: " + pending);
    });

    std::array<char, 256 * 1024> data{};
    DWORD first_got = 0;
    bool startup_failed = false;
    bool startup_timeout = false;
    const auto startup_begin = std::chrono::steady_clock::now();

    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(process.stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
            startup_failed = true;
            break;
        }
        if (available > 0) {
            const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size(), available));
            if (!ReadFile(process.stdout_read, data.data(), want, &first_got, nullptr) || first_got == 0) {
                startup_failed = true;
            }
            break;
        }

        DWORD exit_code = STILL_ACTIVE;
        if (!GetExitCodeProcess(process.process, &exit_code) || exit_code != STILL_ACTIVE) {
            startup_failed = true;
            break;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startup_begin).count();
        if (elapsed > 0 && (elapsed % 10) == 0) {
            static thread_local long long last_reported = -1;
            if (elapsed != last_reported) {
                last_reported = elapsed;
                AddLog("GPU: CUDA pipeline startup still running elapsedSec=" + std::to_string(elapsed));
            }
        }
        if (elapsed >= 120) {
            startup_timeout = true;
            break;
        }
        Sleep(10);
    }

    if (startup_failed || startup_timeout || first_got == 0) {
        if (startup_timeout) {
            AddLog("AI Passthrough startup failed: no output bytes after 120 seconds.");
        } else {
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(process.process, &exit_code);
            AddLog("AI Passthrough startup failed before first output byte. processExit=" +
                   std::to_string(exit_code));
        }
        TerminateAiProcess(process);
        WaitForSingleObject(process.process, 5000);
        if (stderr_thread.joinable()) stderr_thread.join();
        SendAll(client, HttpError(500, "AI Pipeline Failed"));
        CloseAiProcess(process);
        return;
    }

    const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startup_begin).count();
    AddLog("AI Passthrough first output bytes ready=" + std::to_string(first_got) +
           " startupMs=" + std::to_string(startup_ms));

    // The worker reports source duration during input probing, before it emits output.
    // Give the stderr reader a brief chance to publish that metadata before headers.
    for (int i = 0; i < 50 && source_duration_ms.load() <= 0; ++i) {
        Sleep(2);
    }

    std::ostringstream headers;
    headers << "HTTP/1.1 " << (time_seek_request ? "206 Partial Content" : "200 OK") << "\r\n"
            << "Content-Type: " << MimeType(item) << "\r\n"
            << "Accept-Ranges: none\r\n"
            << "transferMode.dlna.org: Streaming\r\n"
            << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n";
    const int64_t duration_ms = item.duration_ms > 0
        ? item.duration_ms
        : source_duration_ms.load();
    if (duration_ms > 0) {
        const std::string duration = FormatNptMs(duration_ms);
        const std::string begin = FormatNptMs(requested_start_ms);
        headers << "X-R800ZZ-Duration-Ms: " << duration_ms << "\r\n"
                << "X-AvailableSeekRange: 1 npt=0.000-" << duration << "\r\n"
                << "TimeSeekRange.dlna.org: npt=" << begin << '-' << duration << '/' << duration << "\r\n";
    }
    if (requested_start_ms > 0) {
        headers << "X-R800ZZ-Start-Ms: " << requested_start_ms << "\r\n";
    }
    headers << "EXT:\r\n"
            << "Connection: close\r\n\r\n";

    if (!SendAll(client, headers.str())) {
        TerminateAiProcess(process);
        WaitForSingleObject(process.process, 5000);
        if (stderr_thread.joinable()) stderr_thread.join();
        CloseAiProcess(process);
        return;
    }

    uint64_t sent_body = 0;
    bool send_failed = false;
    if (!SendAll(client, data.data(), static_cast<size_t>(first_got))) {
        send_failed = true;
    } else {
        sent_body += static_cast<uint64_t>(first_got);
    }

    while (!send_failed && running_) {
        DWORD got = 0;
        const BOOL ok = ReadFile(process.stdout_read, data.data(),
                                 static_cast<DWORD>(data.size()), &got, nullptr);
        if (!ok || got == 0) break;
        if (!SendAll(client, data.data(), static_cast<size_t>(got))) {
            send_failed = true;
            break;
        }
        sent_body += static_cast<uint64_t>(got);
    }

    if (send_failed || !running_) {
        TerminateAiProcess(process);
    }

    WaitForSingleObject(process.process, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.process, &exit_code);

    if (stderr_thread.joinable()) stderr_thread.join();

    AddLog("AI /media body sent=" + std::to_string(sent_body) +
           " processExit=" + std::to_string(exit_code) +
           (send_failed ? " (client disconnected/send failed)" : ""));

    CloseAiProcess(process);
}

void DlnaServer::AddLog(const std::string& text) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    logs_.push_back(text);
    if (logs_.size() > 1000) logs_.erase(logs_.begin(), logs_.begin() + 200);
}

void DlnaServer::SetStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_ = text;
}

std::string DlnaServer::MediaUrl(const MediaItem& item) const {
    std::ostringstream out;
    out << "http://" << advertised_ip_ << ':' << http_port_
        << "/media/" << item.id << '/'
        << UrlEncodePathSegment(WideToUtf8(item.path.filename().wstring()));
    return out.str();
}

std::string DlnaServer::MimeType(const MediaItem& item) const {
    if (ai_passthrough_) {
        return ai_output_mode_ == AiOutputMode::WebmVp9Alpha
            ? "video/webm" : "video/mp2t";
    }
    const std::string ext = ToLower(item.path.extension().string());
    if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".avi") return "video/x-msvideo";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".ts" || ext == ".m2ts") return "video/mp2t";
    return "application/octet-stream";
}

std::string DlnaServer::AiOutputLabel() const {
    if (ai_output_mode_ == AiOutputMode::WebmVp9Alpha) {
        return "Alpha: WebM VP9 Alpha/libvpx (may drop frames)";
    }
    if (ai_output_mode_ == AiOutputMode::ChromaKeyHevc) {
        return "Chroma Key: Green background HEVC/NVENC";
    }
    return "Alpha: AlphaPacked HEVC/NVENC";
}

std::string DlnaServer::DlnaContentFeatures() const {
    if (ai_passthrough_) {
        // Finite transcoded file: DLNA time seek is supported, byte-range seek is not.
        return "DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01700000000000000000000000000000";
    }
    return "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";
}

std::string DlnaServer::DlnaProtocolInfo(const MediaItem* item) const {
    if (item || ai_passthrough_) {
        const std::string mime = item
            ? MimeType(*item)
            : (ai_output_mode_ == AiOutputMode::WebmVp9Alpha
                ? "video/webm" : "video/mp2t");
        return "http-get:*:" + mime + ":" + DlnaContentFeatures();
    }

    std::set<std::string> protocols;
    for (const auto& media : media_items_) {
        protocols.insert("http-get:*:" + MimeType(media) + ":" +
                         DlnaContentFeatures());
    }
    std::ostringstream result;
    bool first = true;
    for (const auto& protocol : protocols) {
        if (!first) result << ',';
        first = false;
        result << protocol;
    }
    return result.str();
}

std::string DlnaServer::DisplayTitle(const MediaItem& item) const {
    // Always show the exact source filename. The actual output
    // container is advertised by protocolInfo/MIME and must not be disguised
    // by inventing a different display filename.
    return WideToUtf8(item.path.filename().wstring());
}

const DlnaServer::MediaItem* DlnaServer::FindMediaItem(
    const std::string& request_path) const {
    std::string path = request_path;
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    if (path == "/media") {
        return media_items_.size() == 1 ? &media_items_.front() : nullptr;
    }
    constexpr std::string_view prefix = "/media/";
    if (path.rfind(prefix.data(), 0) != 0) return nullptr;
    const std::string idText = path.substr(prefix.size());
    try {
        const unsigned long parsed = std::stoul(idText);
        if (parsed == 0 || parsed > media_items_.size()) return nullptr;
        const MediaItem& item = media_items_[parsed - 1];
        return item.id == parsed ? &item : nullptr;
    } catch (...) {
        return nullptr;
    }
}

const DlnaServer::MediaItem* DlnaServer::FindMediaItemByObjectId(
    const std::string& object_id) const {
    try {
        const unsigned long parsed = std::stoul(object_id);
        if (parsed == 0 || parsed > media_items_.size()) return nullptr;
        const MediaItem& item = media_items_[parsed - 1];
        return item.id == parsed ? &item : nullptr;
    } catch (...) {
        return nullptr;
    }
}

std::string DlnaServer::DeviceDescriptionXml() const {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        << "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" "
        << "xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
        << "<specVersion><major>1</major><minor>0</minor></specVersion>"
        << "<URLBase>http://" << advertised_ip_ << ':' << http_port_ << "/</URLBase>"
        << "<device>"
        << "<deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>"
        << "<friendlyName>r800zzXRdlnaServer</friendlyName>"
        << "<manufacturer>R800ZZ</manufacturer>"
        << "<manufacturerURL>https://vr180g.com</manufacturerURL>"
        << "<modelDescription>AI Passthrough DLNA Media Server</modelDescription>"
        << "<modelName>r800zzXRdlnaServer</modelName>"
        << "<modelNumber>1.0</modelNumber>"
        << "<modelURL>https://vr180g.com</modelURL>"
        << "<serialNumber>1</serialNumber>"
        << "<UDN>" << uuid_ << "</UDN>"
        << "<dlna:X_DLNADOC>DMS-1.50</dlna:X_DLNADOC>"
        << "<serviceList>"
        << "<service>"
        << "<serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType>"
        << "<serviceId>urn:upnp-org:serviceId:ContentDirectory</serviceId>"
        << "<SCPDURL>/ContentDirectory/scpd.xml</SCPDURL>"
        << "<controlURL>/ContentDirectory/control</controlURL>"
        << "<eventSubURL>/ContentDirectory/event</eventSubURL>"
        << "</service>"
        << "<service>"
        << "<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
        << "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
        << "<SCPDURL>/ConnectionManager/scpd.xml</SCPDURL>"
        << "<controlURL>/ConnectionManager/control</controlURL>"
        << "<eventSubURL>/ConnectionManager/event</eventSubURL>"
        << "</service>"
        << "</serviceList>"
        << "<presentationURL>https://vr180g.com</presentationURL>"
        << "</device></root>";
    return xml.str();
}

std::string DlnaServer::ContentDirectoryScpdXml() const {
    return R"xml(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<actionList>
<action><name>Browse</name><argumentList>
<argument><name>ObjectID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
<argument><name>BrowseFlag</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_BrowseFlag</relatedStateVariable></argument>
<argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>
<argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>
<argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>
<argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
<argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>
</argumentList></action>
<action><name>Search</name><argumentList>
<argument><name>ContainerID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ObjectID</relatedStateVariable></argument>
<argument><name>SearchCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SearchCriteria</relatedStateVariable></argument>
<argument><name>Filter</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Filter</relatedStateVariable></argument>
<argument><name>StartingIndex</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Index</relatedStateVariable></argument>
<argument><name>RequestedCount</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>SortCriteria</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_SortCriteria</relatedStateVariable></argument>
<argument><name>Result</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Result</relatedStateVariable></argument>
<argument><name>NumberReturned</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>TotalMatches</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Count</relatedStateVariable></argument>
<argument><name>UpdateID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_UpdateID</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetSearchCapabilities</name><argumentList>
<argument><name>SearchCaps</name><direction>out</direction><relatedStateVariable>SearchCapabilities</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetSortCapabilities</name><argumentList>
<argument><name>SortCaps</name><direction>out</direction><relatedStateVariable>SortCapabilities</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetSystemUpdateID</name><argumentList>
<argument><name>Id</name><direction>out</direction><relatedStateVariable>SystemUpdateID</relatedStateVariable></argument>
</argumentList></action>
</actionList>
<serviceStateTable>
<stateVariable sendEvents="no"><name>SearchCapabilities</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>SortCapabilities</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SystemUpdateID</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ObjectID</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_BrowseFlag</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_SearchCriteria</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Filter</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Index</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Count</name><dataType>ui4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_SortCriteria</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Result</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_UpdateID</name><dataType>ui4</dataType></stateVariable>
</serviceStateTable>
</scpd>)xml";
}

std::string DlnaServer::ConnectionManagerScpdXml() const {
    return R"xml(<?xml version="1.0" encoding="utf-8"?>
<scpd xmlns="urn:schemas-upnp-org:service-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<actionList>
<action><name>GetProtocolInfo</name><argumentList>
<argument><name>Source</name><direction>out</direction><relatedStateVariable>SourceProtocolInfo</relatedStateVariable></argument>
<argument><name>Sink</name><direction>out</direction><relatedStateVariable>SinkProtocolInfo</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetCurrentConnectionIDs</name><argumentList>
<argument><name>ConnectionIDs</name><direction>out</direction><relatedStateVariable>CurrentConnectionIDs</relatedStateVariable></argument>
</argumentList></action>
<action><name>GetCurrentConnectionInfo</name><argumentList>
<argument><name>ConnectionID</name><direction>in</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
<argument><name>RcsID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_RcsID</relatedStateVariable></argument>
<argument><name>AVTransportID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_AVTransportID</relatedStateVariable></argument>
<argument><name>ProtocolInfo</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ProtocolInfo</relatedStateVariable></argument>
<argument><name>PeerConnectionManager</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionManager</relatedStateVariable></argument>
<argument><name>PeerConnectionID</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionID</relatedStateVariable></argument>
<argument><name>Direction</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_Direction</relatedStateVariable></argument>
<argument><name>Status</name><direction>out</direction><relatedStateVariable>A_ARG_TYPE_ConnectionStatus</relatedStateVariable></argument>
</argumentList></action>
</actionList>
<serviceStateTable>
<stateVariable sendEvents="yes"><name>SourceProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>SinkProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="yes"><name>CurrentConnectionIDs</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionStatus</name><dataType>string</dataType><allowedValueList><allowedValue>OK</allowedValue><allowedValue>ContentFormatMismatch</allowedValue><allowedValue>InsufficientBandwidth</allowedValue><allowedValue>UnreliableChannel</allowedValue><allowedValue>Unknown</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionManager</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_Direction</name><dataType>string</dataType><allowedValueList><allowedValue>Input</allowedValue><allowedValue>Output</allowedValue></allowedValueList></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ProtocolInfo</name><dataType>string</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_ConnectionID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_AVTransportID</name><dataType>i4</dataType></stateVariable>
<stateVariable sendEvents="no"><name>A_ARG_TYPE_RcsID</name><dataType>i4</dataType></stateVariable>
</serviceStateTable>
</scpd>)xml";
}

std::string DlnaServer::BrowseSoapResponse(const std::string& object_id,
                                           bool metadata,
                                           size_t starting_index,
                                           size_t requested_count,
                                           const std::string& response_action) const {
    constexpr const char* didlStart =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">";

    auto appendItem = [this](std::ostringstream& d, const MediaItem& item) {
        d << "<item id=\"" << item.id
          << "\" parentID=\"0\" restricted=\"1\">"
          << "<dc:title>" << XmlEscape(DisplayTitle(item)) << "</dc:title>"
          << "<upnp:class>object.item.videoItem.movie</upnp:class>"
          << "<res protocolInfo=\"" << XmlEscape(DlnaProtocolInfo(&item)) << "\"";
        if (!ai_passthrough_) d << " size=\"" << item.size << "\"";
        if (item.duration_ms > 0) {
            d << " duration=\"" << FormatDlnaDuration(item.duration_ms) << "\"";
        }
        d << ">" << XmlEscape(MediaUrl(item)) << "</res></item>";
    };

    std::ostringstream didl;
    didl << didlStart;
    size_t numberReturned = 0;
    size_t totalMatches = 0;

    if (metadata && object_id == "0") {
        didl << "<container id=\"0\" parentID=\"-1\" restricted=\"1\" "
             << "searchable=\"0\" childCount=\"" << media_items_.size() << "\">"
             << "<dc:title>r800zzXRdlnaServer</dc:title>"
             << "<upnp:class>object.container</upnp:class></container>";
        numberReturned = 1;
        totalMatches = 1;
    } else if (metadata) {
        const MediaItem* item = FindMediaItemByObjectId(object_id);
        if (item) {
            appendItem(didl, *item);
            numberReturned = 1;
            totalMatches = 1;
        }
    } else if (object_id == "0") {
        totalMatches = media_items_.size();
        const size_t begin = std::min(starting_index, media_items_.size());
        size_t end = media_items_.size();
        if (requested_count > 0) {
            end = std::min(end, begin + requested_count);
        }
        for (size_t i = begin; i < end; ++i) {
            appendItem(didl, media_items_[i]);
            ++numberReturned;
        }
    }
    didl << "</DIDL-Lite>";

    const std::string inner =
        "<Result>" + XmlEscape(didl.str()) + "</Result>" +
        "<NumberReturned>" + std::to_string(numberReturned) + "</NumberReturned>" +
        "<TotalMatches>" + std::to_string(totalMatches) + "</TotalMatches>" +
        "<UpdateID>1</UpdateID>";
    return SimpleSoapResponse("ContentDirectory", response_action, inner);
}

std::string DlnaServer::SimpleSoapResponse(const std::string& service,
                                           const std::string& action,
                                           const std::string& inner_xml) const {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        << "<s:Body>"
        << "<u:" << action << "Response xmlns:u=\"urn:schemas-upnp-org:service:" << service << ":1\">"
        << inner_xml
        << "</u:" << action << "Response>"
        << "</s:Body></s:Envelope>";
    return xml.str();
}
