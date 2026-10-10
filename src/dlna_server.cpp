#include "dlna_server.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <cwctype>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
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

// Must match the realtime MPEG-TS muxrate configured in r800zz_ai_worker.
// Generic clients use this finite byte space; r800zzvrplayer does not.
constexpr uint64_t kRealtimeTsMuxRateBitsPerSecond = 100000000ULL;
constexpr uint64_t kMpegTsPacketSize = 188ULL;
// ExoPlayer/Media3 MPEG-TS duration probing reads the last 600 TS packets.
// Treat that request shape as a duration probe rather than a playback seek.
constexpr uint64_t kTsDurationProbeBytes = 600ULL * kMpegTsPacketSize;
constexpr int64_t kTsDurationProbeLeadMs = 1000;
constexpr size_t kGenericTailProbeCacheMaxEntries = 16;

std::mutex g_generic_tail_probe_cache_mutex;
std::unordered_map<std::string, std::vector<char>> g_generic_tail_probe_cache;

// The resident AI worker can execute only one RUN at a time.  Media players may
// open a replacement GET before the previous GET has completely unwound.  Keep
// that handoff ordered on the server side so a new RUN never races the previous
// stream's pipe/cleanup path.
std::mutex g_ai_stream_serial_mutex;
std::mutex g_ai_stream_owner_mutex;
SOCKET g_active_ai_stream_client = INVALID_SOCKET;
std::atomic<uint64_t> g_ai_stream_generation{0};

// The UI calls FindFiles() with the selected folder before Start(). Keep that
// folder as the DLNA ContentDirectory root so Browse can expose real folders
// instead of flattening every descendant file into object 0.
std::mutex g_last_find_files_root_mutex;
std::filesystem::path g_last_find_files_root;

bool LoadGenericTailProbeCache(const std::string& key,
                               uint64_t expectedBytes,
                               std::vector<char>& body) {
    if (key.empty() || expectedBytes == 0 ||
        expectedBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_generic_tail_probe_cache_mutex);
    const auto it = g_generic_tail_probe_cache.find(key);
    if (it == g_generic_tail_probe_cache.end() ||
        it->second.size() != static_cast<size_t>(expectedBytes)) {
        return false;
    }
    body = it->second;
    return true;
}

void StoreGenericTailProbeCache(const std::string& key,
                                const std::vector<char>& body) {
    if (key.empty() || body.empty()) return;
    std::lock_guard<std::mutex> lock(g_generic_tail_probe_cache_mutex);
    if (g_generic_tail_probe_cache.find(key) == g_generic_tail_probe_cache.end() &&
        g_generic_tail_probe_cache.size() >= kGenericTailProbeCacheMaxEntries) {
        g_generic_tail_probe_cache.erase(g_generic_tail_probe_cache.begin());
    }
    g_generic_tail_probe_cache[key] = body;
}

uint64_t RealtimeTsVirtualSizeBytes(int64_t durationMs) {
    if (durationMs <= 0) return 0;
    const long double bytes =
        static_cast<long double>(durationMs) *
        static_cast<long double>(kRealtimeTsMuxRateBitsPerSecond) / 8000.0L;
    if (!std::isfinite(static_cast<double>(bytes)) || bytes <= 0.0L ||
        bytes > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return 0;
    }

    const uint64_t rounded = static_cast<uint64_t>(std::ceil(bytes));
    const uint64_t remainder = rounded % kMpegTsPacketSize;
    if (remainder == 0) return rounded;

    const uint64_t add = kMpegTsPacketSize - remainder;
    if (rounded > std::numeric_limits<uint64_t>::max() - add) return 0;
    return rounded + add;
}

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

std::string JsonEscape(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() + 32);
    for (unsigned char c : value) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0x0f]);
                out.push_back(hex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}


std::string Pc2HmdHtml() {
    static constexpr char kHtmlPart1[] = R"PC1(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>R800ZZ DLNA Downloader</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; font-family:system-ui,-apple-system,"Segoe UI",sans-serif; background:#111; color:#eee; }
  main { max-width:980px; margin:0 auto; padding:24px; }
  h1 { margin:0 0 10px; font-size:1.6rem; }
  h1 a { color:inherit; }
  .languages { display:flex; flex-wrap:wrap; gap:8px 14px; margin:0 0 12px; }
  .intro { margin:0 0 6px; line-height:1.5; }
  .intro-last { margin:0 0 20px; line-height:1.5; }
  .languages label { display:inline-flex; align-items:center; gap:5px; white-space:nowrap; cursor:pointer; }
  .languages input { width:auto; margin:0; padding:0; accent-color:auto; }
  .server-row { display:grid; grid-template-columns:1fr auto; gap:10px; }
  input { width:100%; padding:12px; border:1px solid #555; border-radius:8px; background:#1c1c1c; color:#fff; font-size:1rem; }
  button { padding:11px 16px; border:1px solid #666; border-radius:8px; background:#2a2a2a; color:#fff; cursor:pointer; }
  button:disabled { opacity:.55; cursor:default; }
  #status { margin:14px 0; min-height:1.5em; white-space:pre-wrap; overflow-wrap:anywhere; }
  .toolbar { display:flex; gap:10px; flex-wrap:wrap; margin:12px 0 18px; }
  .toolbar input { flex:1 1 260px; }
  .file-list { display:grid; gap:8px; }
  .file { display:grid; grid-template-columns:minmax(0,1fr) auto auto; gap:10px; align-items:center; padding:12px; border:1px solid #333; border-radius:8px; background:#181818; }
  .name { min-width:0; overflow-wrap:anywhere; }
  .meta { margin-top:4px; color:#aaa; font-size:.82rem; }
  .empty { color:#aaa; padding:18px 0; }
  .note { margin-top:24px; color:#aaa; line-height:1.5; font-size:.9rem; }
  progress { width:100%; height:18px; margin:12px 0; }
  @media (max-width:700px) {
    .server-row { grid-template-columns:1fr; }
    .file { grid-template-columns:1fr 1fr; }
    .name { grid-column:1 / -1; }
  }
</style>
</head>
<body>
<main>
  <h1>Download from <a id="serverReadmeLink" href="https://github.com/r800zz/r800zzxrdlnaserver/blob/main/README.md" target="_blank" rel="noopener">r800zzXRdlnaServer for Windows</a></h1>

  <div class="languages" id="languages">
    <label><input type="radio" name="language" value="en"> English</label>
    <label><input type="radio" name="language" value="ru"> Русский</label>
    <label><input type="radio" name="language" value="es"> Español</label>
    <label><input type="radio" name="language" value="th"> ภาษาไทย</label>
    <label><input type="radio" name="language" value="cn"> 中文</label>
    <label><input type="radio" name="language" value="kr"> 한국어</label>
    <label><input type="radio" name="language" value="jp"> 日本語</label>
  </div>

  <p class="intro" id="downloadDescription"></p>
  <p class="intro-last" id="serverOnlyDescription"></p>

  <div class="server-row">
    <input id="server" placeholder="http://192.168.1.102:49152" autocomplete="off" spellcheck="false">
    <button id="load">Load File List</button>
  </div>

  <div id="status"></div>

  <div class="toolbar">
    <input id="filter" placeholder="Filter files..." autocomplete="off">
    <button id="reload">Reload</button>
  </div>

  <progress id="progress" hidden></progress>
  <div id="files" class="file-list"></div>

  <div class="note">
    <span id="apiLabel">API endpoint:</span> <code>/-_-9213--api/files</code><br>
    <span id="serverHelp">Enter only the server base URL, for example</span> <code>http://192.168.1.102:49152</code>.
  </div>
</main>

<script>
var lass = "en";
var langlang = "en";

(() => {
  "use strict";

  const API_PATH = "/-_-9213--api/files";
  const countryCodes = ["en", "ru", "es", "th", "cn", "kr", "jp"];
  const languageCodes = {
    en: "en",
    ru: "ru",
    es: "es",
    th: "th",
    cn: "zh",
    kr: "ko",
    jp: "ja"
  };

  const readmeBaseUrl = "https://github.com/r800zz/r800zzxrdlnaserver/blob/main/";

  const text = {
    en: {
      downloadDescription: "Download files stored on your PC from the web browser of your VR HMD.",
      serverOnlyDescription: "You need to enter the DLNA server URL. This is for r800zzXRdlnaServer only.",
      load: "Load File List",
      reload: "Reload",
      filter: "Filter files...",
      apiLabel: "API endpoint:",
      serverHelp: "Enter only the server base URL, for example",
      enterServer: "Enter the DLNA server URL.",
      protocolError: "Only http:// or https:// is supported.",
      noMatching: "No matching files.",
      noFiles: "No files.",
      unnamed: "(unnamed)",
      parent: "Parent",
      video: "Video",
      download: "Download",
      open: "Open",
      loading: "Loading file list...",
      invalidApi: "Invalid API response.",
      files: "file(s)",
      loadFailed: "Could not load the file list.",
      downloading: "Downloading:",
      saved: "Saved:",
      downloadFailed: "JavaScript download failed.",
      openingDirectly: "Opening the file URL directly..."
    },
    ru: {
      downloadDescription: "Загружайте файлы, хранящиеся на вашем ПК, из веб-браузера VR-шлема.",
      serverOnlyDescription: "Необходимо ввести URL DLNA-сервера. Это предназначено только для r800zzXRdlnaServer.",
      load: "Загрузить список файлов",
      reload: "Обновить",
      filter: "Фильтр файлов...",
      apiLabel: "API-адрес:",
      serverHelp: "Введите только базовый URL сервера, например",
      enterServer: "Введите URL DLNA-сервера.",
      protocolError: "Поддерживаются только http:// и https://.",
      noMatching: "Подходящие файлы не найдены.",
      noFiles: "Файлов нет.",
      unnamed: "(без имени)",
      parent: "Родитель",
      video: "Видео",
      download: "Скачать",
      open: "Открыть",
      loading: "Загрузка списка файлов...",
      invalidApi: "Некорректный ответ API.",
      files: "файл(ов)",
      loadFailed: "Не удалось загрузить список файлов.",
      downloading: "Загрузка:",
      saved: "Сохранено:",
      downloadFailed: "Не удалось скачать файл с помощью JavaScript.",
      openingDirectly: "Открывается прямой URL файла..."
    },
    es: {
      downloadDescription: "Descarga archivos almacenados en tu PC desde el navegador web de tu visor VR.",
      serverOnlyDescription: "Debes introducir la URL del servidor DLNA. Esto es exclusivamente para r800zzXRdlnaServer.",
      load: "Cargar lista de archivos",
      reload: "Recargar",
)PC1";
    static constexpr char kHtmlPart2[] = R"PC2(      filter: "Filtrar archivos...",
      apiLabel: "Endpoint de API:",
      serverHelp: "Introduce solo la URL base del servidor, por ejemplo",
      enterServer: "Introduce la URL del servidor DLNA.",
      protocolError: "Solo se admite http:// o https://.",
      noMatching: "No hay archivos coincidentes.",
      noFiles: "No hay archivos.",
      unnamed: "(sin nombre)",
      parent: "Superior",
      video: "Vídeo",
      download: "Descargar",
      open: "Abrir",
      loading: "Cargando lista de archivos...",
      invalidApi: "Respuesta de API no válida.",
      files: "archivo(s)",
      loadFailed: "No se pudo cargar la lista de archivos.",
      downloading: "Descargando:",
      saved: "Guardado:",
      downloadFailed: "La descarga con JavaScript ha fallado.",
      openingDirectly: "Abriendo directamente la URL del archivo..."
    },
    th: {
      downloadDescription: "ดาวน์โหลดไฟล์ที่เก็บไว้ในพีซีของคุณจากเว็บเบราว์เซอร์ของ VR HMD",
      serverOnlyDescription: "คุณต้องป้อน URL ของเซิร์ฟเวอร์ DLNA เครื่องมือนี้ใช้สำหรับ r800zzXRdlnaServer เท่านั้น",
      load: "โหลดรายการไฟล์",
      reload: "โหลดใหม่",
      filter: "กรองไฟล์...",
      apiLabel: "ตำแหน่ง API:",
      serverHelp: "ป้อนเฉพาะ URL หลักของเซิร์ฟเวอร์ ตัวอย่างเช่น",
      enterServer: "ป้อน URL ของเซิร์ฟเวอร์ DLNA",
      protocolError: "รองรับเฉพาะ http:// หรือ https:// เท่านั้น",
      noMatching: "ไม่พบไฟล์ที่ตรงกัน",
      noFiles: "ไม่มีไฟล์",
      unnamed: "(ไม่มีชื่อ)",
      parent: "โฟลเดอร์แม่",
      video: "วิดีโอ",
      download: "ดาวน์โหลด",
      open: "เปิด",
      loading: "กำลังโหลดรายการไฟล์...",
      invalidApi: "การตอบกลับจาก API ไม่ถูกต้อง",
      files: "ไฟล์",
      loadFailed: "ไม่สามารถโหลดรายการไฟล์ได้",
      downloading: "กำลังดาวน์โหลด:",
      saved: "บันทึกแล้ว:",
      downloadFailed: "การดาวน์โหลดด้วย JavaScript ล้มเหลว",
      openingDirectly: "กำลังเปิด URL ของไฟล์โดยตรง..."
    },
    cn: {
      downloadDescription: "从 VR HMD 的网页浏览器下载存储在电脑上的文件。",
      serverOnlyDescription: "需要输入 DLNA 服务器 URL。本工具仅适用于 r800zzXRdlnaServer。",
      load: "加载文件列表",
      reload: "重新加载",
      filter: "筛选文件...",
      apiLabel: "API 地址：",
      serverHelp: "只需输入服务器的基础 URL，例如",
      enterServer: "请输入 DLNA 服务器 URL。",
      protocolError: "仅支持 http:// 或 https://。",
      noMatching: "没有匹配的文件。",
      noFiles: "没有文件。",
      unnamed: "（未命名）",
      parent: "父目录",
      video: "视频",
      download: "下载",
      open: "打开",
      loading: "正在加载文件列表...",
      invalidApi: "API 响应无效。",
      files: "个文件",
      loadFailed: "无法加载文件列表。",
      downloading: "正在下载：",
      saved: "已保存：",
      downloadFailed: "JavaScript 下载失败。",
      openingDirectly: "正在直接打开文件 URL..."
    },
    kr: {
      downloadDescription: "VR HMD의 웹 브라우저에서 PC에 저장된 파일을 다운로드합니다.",
      serverOnlyDescription: "DLNA 서버 URL을 입력해야 합니다. 이 도구는 r800zzXRdlnaServer 전용입니다.",
      load: "파일 목록 불러오기",
      reload: "다시 불러오기",
      filter: "파일 필터...",
      apiLabel: "API 엔드포인트:",
      serverHelp: "서버 기본 URL만 입력하십시오. 예:",
      enterServer: "DLNA 서버 URL을 입력하십시오.",
      protocolError: "http:// 또는 https://만 지원됩니다.",
      noMatching: "일치하는 파일이 없습니다.",
      noFiles: "파일이 없습니다.",
      unnamed: "(이름 없음)",
      parent: "상위",
      video: "비디오",
      download: "다운로드",
      open: "열기",
      loading: "파일 목록을 불러오는 중...",
      invalidApi: "잘못된 API 응답입니다.",
      files: "개 파일",
      loadFailed: "파일 목록을 불러오지 못했습니다.",
      downloading: "다운로드 중:",
      saved: "저장됨:",
      downloadFailed: "JavaScript 다운로드에 실패했습니다.",
      openingDirectly: "파일 URL을 직접 엽니다..."
    },
    jp: {
      downloadDescription: "パソコン上にあるファイルをVR HMDのウェブブラウザからダウンロードします。",
      serverOnlyDescription: "DLNAサーバーURLを入力する必要があります。r800zzXRdlnaServer 専用です。",
      load: "ファイル一覧を読み込む",
      reload: "再読み込み",
      filter: "ファイルを絞り込む...",
      apiLabel: "APIエンドポイント:",
      serverHelp: "サーバーのベースURLだけを入力してください。例:",
      enterServer: "DLNAサーバーのURLを入力してください。",
      protocolError: "http:// または https:// のみ対応しています。",
      noMatching: "一致するファイルがありません。",
      noFiles: "ファイルがありません。",
      unnamed: "（名前なし）",
      parent: "親",
      video: "動画",
      download: "ダウンロード",
      open: "開く",
      loading: "ファイル一覧を読み込んでいます...",
      invalidApi: "APIの応答が不正です。",
      files: "ファイル",
      loadFailed: "ファイル一覧を読み込めませんでした。",
      downloading: "ダウンロード中:",
      saved: "保存しました:",
      downloadFailed: "JavaScriptでのダウンロードに失敗しました。",
      openingDirectly: "ファイルURLを直接開きます..."
    }
  };

  const serverInput = document.getElementById("server");
  const loadButton = document.getElementById("load");
  const reloadButton = document.getElementById("reload");
  const filterInput = document.getElementById("filter");
  const filesElement = document.getElementById("files");
  const statusElement = document.getElementById("status");
  const progress = document.getElementById("progress");
)PC2";
    static constexpr char kHtmlPart3[] = R"PC3(  const apiLabel = document.getElementById("apiLabel");
  const serverHelp = document.getElementById("serverHelp");
  const serverReadmeLink = document.getElementById("serverReadmeLink");
  const downloadDescription = document.getElementById("downloadDescription");
  const serverOnlyDescription = document.getElementById("serverOnlyDescription");
  const languageRadios = document.querySelectorAll('input[name="language"]');

  let currentFiles = [];

  function t(key) {
    return (text[lass] && text[lass][key]) || text.en[key] || key;
  }

  function initializeLanguage() {
    const params = new URLSearchParams(window.location.search);
    const requested = (params.get("l") || "en").toLowerCase();
    lass = countryCodes.includes(requested) ? requested : "en";
    langlang = languageCodes[lass];
    document.documentElement.lang = langlang;
    for (const radio of languageRadios) radio.checked = radio.value === lass;
  }

  function setLanguage(code, updateUrl) {
    lass = countryCodes.includes(code) ? code : "en";
    langlang = languageCodes[lass];
    document.documentElement.lang = langlang;
    for (const radio of languageRadios) radio.checked = radio.value === lass;

    if (updateUrl) {
      const url = new URL(window.location.href);
      url.searchParams.set("l", lass);
      history.replaceState(null, "", url);
    }

    applyLanguage();
    renderFiles();
  }

  function applyLanguage() {
    loadButton.textContent = t("load");
    reloadButton.textContent = t("reload");
    filterInput.placeholder = t("filter");
    apiLabel.textContent = t("apiLabel");
    serverHelp.textContent = t("serverHelp");
    downloadDescription.textContent = t("downloadDescription");
    serverOnlyDescription.textContent = t("serverOnlyDescription");
    serverReadmeLink.href = readmeBaseUrl + (lass === "en" ? "README.md" : "README_" + langlang + ".md");
  }

  const savedServer = localStorage.getItem("r800zz_dlna_server");
  if (savedServer) {
    serverInput.value = savedServer;
  } else if (window.location.protocol === "http:" || window.location.protocol === "https:") {
    serverInput.value = window.location.origin;
  }

  function setStatus(value) { statusElement.textContent = value; }

  function normalizeServer(value) {
    value = value.trim();
    if (!value) throw new Error(t("enterServer"));
    if (!/^https?:\/\//i.test(value)) value = "http://" + value;
    const url = new URL(value);
    if (url.protocol !== "http:" && url.protocol !== "https:") throw new Error(t("protocolError"));
    return url.origin;
  }

  function humanSize(bytes) {
    const n = Number(bytes);
    if (!Number.isFinite(n) || n < 0) return "";
    const units = ["B", "KB", "MB", "GB", "TB"];
    let value = n, unit = 0;
    while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
    return (unit === 0 ? value.toFixed(0) : value.toFixed(1)) + " " + units[unit];
  }

  function safeName(name) {
    return String(name || "download").replace(/[\\/:*?"<>|]/g, "_");
  }

  function resolveFileUrl(server, value) {
    return new URL(String(value || ""), server + "/").toString();
  }

  function renderFiles() {
    let server;
    try { server = normalizeServer(serverInput.value); }
    catch (_) { server = ""; }

    const q = filterInput.value.trim().toLowerCase();
    const list = currentFiles.filter(item => !q || String(item.name || "").toLowerCase().includes(q));
    filesElement.textContent = "";

    if (!list.length) {
      const div = document.createElement("div");
      div.className = "empty";
      div.textContent = currentFiles.length ? t("noMatching") : t("noFiles");
      filesElement.appendChild(div);
      return;
    }

    for (const item of list) {
      const row = document.createElement("div");
      row.className = "file";

      const info = document.createElement("div");
      info.className = "name";

      const title = document.createElement("div");
      title.textContent = item.name || t("unnamed");
      info.appendChild(title);

      const meta = document.createElement("div");
      meta.className = "meta";
      const parts = [];
      if (item.size !== undefined) parts.push(humanSize(item.size));
      if (item.id !== undefined) parts.push("ID " + item.id);
      if (item.parentId !== undefined) parts.push(t("parent") + " " + item.parentId);
      if (item.isVideo === true) parts.push(t("video"));
      meta.textContent = parts.join("  |  ");
      info.appendChild(meta);

      const downloadButton = document.createElement("button");
      downloadButton.textContent = t("download");
      downloadButton.addEventListener("click", () => downloadFile(item, server, downloadButton));

      const openButton = document.createElement("button");
      openButton.textContent = t("open");
      openButton.addEventListener("click", () => {
        try { window.open(resolveFileUrl(server, item.url), "_blank", "noopener"); }
        catch (e) { setStatus(e.message || String(e)); }
      });

      row.appendChild(info);
      row.appendChild(downloadButton);
      row.appendChild(openButton);
      filesElement.appendChild(row);
    }
  }

  async function loadFiles() {
    progress.hidden = true;
    setStatus("");
    filesElement.textContent = "";

    try {
      const server = normalizeServer(serverInput.value);
      serverInput.value = server;
      localStorage.setItem("r800zz_dlna_server", server);

      loadButton.disabled = true;
      reloadButton.disabled = true;
      setStatus(t("loading"));

      const response = await fetch(server + API_PATH, { method:"GET", mode:"cors", cache:"no-store" });
      if (!response.ok) throw new Error("HTTP " + response.status + " " + response.statusText);

      const data = await response.json();
      if (!Array.isArray(data)) throw new Error(t("invalidApi"));

      currentFiles = data;
      renderFiles();
      setStatus(currentFiles.length + " " + t("files"));
    } catch (e) {
      currentFiles = [];
      renderFiles();
      setStatus(t("loadFailed") + "\n" + (e && e.message ? e.message : String(e)));
    } finally {
      loadButton.disabled = false;
      reloadButton.disabled = false;
    }
  }

  async function downloadFile(item, server, button) {
    let url;
    try { url = resolveFileUrl(server, item.downloadUrl || item.url); }
    catch (e) { setStatus(e.message || String(e)); return; }

    const filename = safeName(item.name || "download");
    button.disabled = true;
    progress.hidden = true;

    try {
      setStatus(t("downloading") + " " + filename);
      const response = await fetch(url, { method:"GET", mode:"cors", cache:"no-store" });
      if (!response.ok) throw new Error("HTTP " + response.status + " " + response.statusText);

      const total = Number(response.headers.get("Content-Length")) || 0;
      const reader = response.body && response.body.getReader ? response.body.getReader() : null;

      if (!reader) {
        const blob = await response.blob();
)PC3";
    static constexpr char kHtmlPart4[] = R"PC4(        saveBlob(blob, filename);
        setStatus(t("saved") + " " + filename);
        return;
      }

      const chunks = [];
      let received = 0;
      progress.hidden = false;
      if (total > 0) { progress.max = total; progress.value = 0; }
      else { progress.removeAttribute("value"); }

      while (true) {
        const result = await reader.read();
        if (result.done) break;
        chunks.push(result.value);
        received += result.value.byteLength;
        if (total > 0) {
          progress.value = received;
          setStatus(t("downloading") + " " + filename + "  " + Math.floor(received * 100 / total) + "%");
        } else {
          setStatus(t("downloading") + " " + filename + "  " + (received / 1024 / 1024).toFixed(1) + " MB");
        }
      }

      const contentType = response.headers.get("Content-Type") || "application/octet-stream";
      saveBlob(new Blob(chunks, { type:contentType }), filename);
      progress.hidden = true;
      setStatus(t("saved") + " " + filename);
    } catch (e) {
      progress.hidden = true;
      setStatus(t("downloadFailed") + "\n" + (e && e.message ? e.message : String(e)) + "\n" + t("openingDirectly"));
      window.open(url, "_blank", "noopener");
    } finally {
      button.disabled = false;
    }
  }

  function saveBlob(blob, filename) {
    const objectUrl = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = objectUrl;
    a.download = filename;
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(objectUrl), 60000);
  }

  initializeLanguage();
  applyLanguage();
  renderFiles();
  if (serverInput.value) {
    loadFiles();
  }

  for (const radio of languageRadios) {
    radio.addEventListener("change", () => {
      if (radio.checked) setLanguage(radio.value, true);
    });
  }

  loadButton.addEventListener("click", loadFiles);
  reloadButton.addEventListener("click", loadFiles);
  filterInput.addEventListener("input", renderFiles);
  serverInput.addEventListener("keydown", e => { if (e.key === "Enter") loadFiles(); });
})();
</script>
</body>
</html>
)PC4";
    std::string html;
    html.reserve((sizeof(kHtmlPart1) - 1) + (sizeof(kHtmlPart2) - 1) + (sizeof(kHtmlPart3) - 1) + (sizeof(kHtmlPart4) - 1));
    html.append(kHtmlPart1, sizeof(kHtmlPart1) - 1);
    html.append(kHtmlPart2, sizeof(kHtmlPart2) - 1);
    html.append(kHtmlPart3, sizeof(kHtmlPart3) - 1);
    html.append(kHtmlPart4, sizeof(kHtmlPart4) - 1);
    return html;
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

std::string SourceMimeTypeForPath(const std::filesystem::path& path) {
    const std::string ext = ToLower(path.extension().string());
    if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".avi") return "video/x-msvideo";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".ts" || ext == ".m2ts") return "video/mp2t";
    if (ext == ".vrm" || ext == ".glb") return "model/gltf-binary";
    if (ext == ".gltf") return "model/gltf+json";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    return "application/octet-stream";
}

std::string DownloadContentDisposition(std::string_view utf8Filename) {
    std::string fallback;
    fallback.reserve(utf8Filename.size());
    for (unsigned char c : utf8Filename) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\' && c != ';') {
            fallback.push_back(static_cast<char>(c));
        } else if (c >= 0x20) {
            fallback.push_back('_');
        }
    }
    if (fallback.empty()) fallback = "download";
    return "attachment; filename=\"" + fallback +
           "\"; filename*=UTF-8''" + UrlEncodePathSegment(utf8Filename);
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

std::string AiBackendArgument(AiBackend backend) {
    if (backend == AiBackend::DirectML) return "dml";
    if (backend == AiBackend::Cpu) return "cpu";
    return "cuda";
}

std::string DisplayCodecName(std::string codec) {
    codec = ToLower(std::move(codec));
    if (codec == "h264") return "H.264";
    if (codec == "hevc" || codec == "h265") return "HEVC";
    if (codec == "vp9") return "VP9";
    if (codec == "vp8") return "VP8";
    if (codec == "av1") return "AV1";
    if (codec == "mpeg4") return "MPEG-4";
    return codec.empty() ? "unknown" : codec;
}

std::string AiOutputModeDisplayName(AiOutputMode mode) {
    if (mode == AiOutputMode::WebmVp9Alpha) return "Alpha WebM VP9";
    if (mode == AiOutputMode::ChromaKeyHevc) return "Chroma Key";
    return "Alpha Packed";
}

std::string AiBackendDisplayName(AiBackend backend, int directmlDevice) {
    if (backend == AiBackend::DirectML) {
        return "DirectML device " + std::to_string(directmlDevice);
    }
    if (backend == AiBackend::Cpu) return "CPU";
    return "NVIDIA CUDA";
}

bool ShouldShowWorkerLog(const std::string& line) {
    // Periodic realtime statistics are useful for profiling but make real errors
    // disappear from the visible log. Keep them out of the normal UI log.
    return line.find("realtime fpsOut=") == std::string::npos;
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

// Request-local compatibility profile. Each HTTP connection is handled by its
// own worker thread, so simultaneous r800zzvrplayer and generic DLNA clients do
// not change each other's advertised seek capabilities.
thread_local bool g_r800zz_vrplayer_request = false;

bool IsR800zzVrPlayerRequest(const std::string& request) {
    const std::string userAgent = ToLower(HeaderValue(request, "user-agent"));
    return userAgent.find("r800zzvrplayer") != std::string::npos;
}

int64_t ParseDlnaTimeSeekStartMs(const std::string& value) {
    if (value.empty()) return -1;
    const std::string lower = ToLower(value);
    const size_t npt = lower.find("npt=");
    if (npt == std::string::npos) return -1;
    const size_t start = npt + 4;
    const size_t end = value.find('-', start);
    if (end == std::string::npos || end <= start) return -1;
    const std::string token = Trim(value.substr(start, end - start));
    if (token.empty() || ToLower(token) == "now") return -1;

    double seconds = 0.0;
    if (token.find(':') != std::string::npos) {
        // DLNA NPT also permits hh:mm:ss[.fraction].
        std::istringstream in(token);
        long long hours = 0;
        int minutes = 0;
        double secs = 0.0;
        char colon1 = 0;
        char colon2 = 0;
        if (!(in >> hours >> colon1 >> minutes >> colon2 >> secs) ||
            colon1 != ':' || colon2 != ':' || hours < 0 ||
            minutes < 0 || minutes >= 60 || !std::isfinite(secs) ||
            secs < 0.0 || secs >= 60.0) {
            return -1;
        }
        in >> std::ws;
        if (!in.eof()) return -1;
        seconds = static_cast<double>(hours) * 3600.0 +
                  static_cast<double>(minutes) * 60.0 + secs;
    } else {
        char* parseEnd = nullptr;
        errno = 0;
        seconds = std::strtod(token.c_str(), &parseEnd);
        if (errno != 0 || parseEnd == token.c_str()) return -1;
        while (*parseEnd != '\0' &&
               std::isspace(static_cast<unsigned char>(*parseEnd))) {
            ++parseEnd;
        }
        if (*parseEnd != '\0' || !std::isfinite(seconds) || seconds < 0.0) {
            return -1;
        }
    }

    constexpr double kMaxMilliseconds = static_cast<double>(INT64_MAX);
    if (!std::isfinite(seconds) || seconds * 1000.0 > kMaxMilliseconds) {
        return -1;
    }
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

void KeepTailBytes(std::vector<char>& tail,
                   const char* data,
                   size_t size,
                   size_t limit) {
    if (limit == 0 || size == 0) return;
    if (size >= limit) {
        tail.assign(data + (size - limit), data + size);
        return;
    }
    if (tail.size() + size > limit) {
        const size_t drop = tail.size() + size - limit;
        tail.erase(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    tail.insert(tail.end(), data, data + size);
}

bool SendMpegTsNullPadding(SOCKET socket, uint64_t bytes) {
    if (bytes == 0) return true;
    if ((bytes % kMpegTsPacketSize) != 0) return false;

    constexpr size_t kPacketCount = 256;
    std::array<char, static_cast<size_t>(kMpegTsPacketSize) * kPacketCount> padding{};
    for (size_t packet = 0; packet < kPacketCount; ++packet) {
        const size_t offset = packet * static_cast<size_t>(kMpegTsPacketSize);
        padding[offset + 0] = 0x47;
        padding[offset + 1] = 0x1f;
        padding[offset + 2] = static_cast<char>(0xff);
        padding[offset + 3] = static_cast<char>(0x10 | (packet & 0x0f));
        std::fill(padding.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                  padding.begin() + static_cast<std::ptrdiff_t>(offset + kMpegTsPacketSize),
                  static_cast<char>(0xff));
    }

    while (bytes > 0) {
        const size_t chunk = static_cast<size_t>(
            std::min<uint64_t>(bytes, static_cast<uint64_t>(padding.size())));
        if (!SendAll(socket, padding.data(), chunk)) return false;
        bytes -= static_cast<uint64_t>(chunk);
    }
    return true;
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
                     const std::filesystem::path& model,
                     int64_t start_ms,
                     AiBackend backend,
                     int device,
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

    std::wstring command =
        QuoteWindowsArgument(worker.wstring()) + L" " +
        QuoteWindowsArgument(input.wstring()) + L" --model " +
        QuoteWindowsArgument(model.wstring()) + L" --device " +
        std::to_wstring(device) + L" --qp 20 --start-ms " +
        std::to_wstring(start_ms) + L" --output-mode " +
        Utf8ToWide(AiOutputModeArgument(output_mode)) + L" --no-preview";
    if (backend == AiBackend::DirectML) command += L" --backend dml";
    else if (backend == AiBackend::Cpu) command += L" --backend cpu";

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
    AiBackend backend{AiBackend::NvidiaCuda};
    int directml_device{0};
    int width{0};
    int height{0};
    std::atomic<bool> ready{false};
    // PREPARE can fail while the resident controller itself remains alive.
    // Publish that terminal result so Start() does not sit in the 60 s READY wait.
    std::atomic<bool> prepare_failed{false};
    std::atomic<uint64_t> request_sequence{0};

    // HTTP streaming code keeps a raw pointer to this state while it waits on
    // the named pipe.  Keep the state alive until every such user has left.
    // This closes the race where Stop()/Start() could destroy ResidentAiState
    // while a detached HTTP worker was still using control_mutex/stdin_write.
    std::atomic<uint32_t> active_http_users{0};
    std::atomic<bool> stopping{false};
    std::mutex http_users_mutex;
    std::condition_variable http_users_cv;

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
    const DWORD wait = WaitForSingleObject(processInfo.hProcess, 60000);
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
    // The resident process is backend-neutral. CUDA / DirectML / CPU are selected
    // later by PREPARE/RUN commands without restarting r800zz_ai_worker.exe.
    const std::filesystem::path directory = ExecutableDirectory();
    const std::filesystem::path worker = directory / L"r800zz_ai_worker.exe";
    const std::filesystem::path cudaModel = directory / L"rvm_mobilenetv3_fp16.onnx";
    const std::filesystem::path dmlModel = directory / L"rvm_mobilenetv3_fp32.onnx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(worker, ec)) {
        AddLog("AI prewarm error: resident worker was not found: " + WideToUtf8(worker.wstring()));
        return false;
    }
    ec.clear();
    if (ai_backend_ == AiBackend::NvidiaCuda) {
        if (!std::filesystem::is_regular_file(cudaModel, ec)) {
            AddLog("AI prewarm error: CUDA model was not found: " + WideToUtf8(cudaModel.wstring()));
            return false;
        }
    } else {
        if (!std::filesystem::is_regular_file(dmlModel, ec)) {
            AddLog("AI prewarm error: DirectML/CPU model was not found: " + WideToUtf8(dmlModel.wstring()));
            return false;
        }
    }
    ec.clear();
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
        QuoteWindowsArgument(media_file.wstring()) +
        L" --qp 20 --resident --no-preview";
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
    state->backend = ai_backend_;
    state->directml_device = directml_device_;
    int64_t ignoredDuration = 0;
    ProbeMedia(media_file, ignoredDuration, state->width, state->height);
    ResidentAiState* raw = state.get();

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    state->stderr_thread = std::thread([this, raw]() {
        std::array<char, 4096> data{};
        std::string pending;
        std::string errorGpu;
        std::string errorMode;
        std::string errorCodec;
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
                    raw->prepare_failed.store(false);
                    raw->ready.store(true);
                }
                if (clean.find("RESIDENT_ERROR prepare backend=") != std::string::npos) {
                    raw->ready.store(false);
                    raw->prepare_failed.store(true);
                }
                if (!clean.empty()) {
                    HandleWorkerDiagnosticLine(
                        clean, errorGpu, errorMode, errorCodec);
                    if (ShouldShowWorkerLog(clean)) AddLog("GPU: " + clean);
                }
                pending.erase(0, pos + 1);
            }
        }
        pending = SanitizeWorkerLog(pending);
        if (!pending.empty()) {
            if (pending.find("RESIDENT_READY") != std::string::npos) {
                raw->prepare_failed.store(false);
                raw->ready.store(true);
            }
            if (pending.find("RESIDENT_ERROR prepare backend=") != std::string::npos) {
                raw->ready.store(false);
                raw->prepare_failed.store(true);
            }
            HandleWorkerDiagnosticLine(
                pending, errorGpu, errorMode, errorCodec);
            if (ShouldShowWorkerLog(pending)) AddLog("GPU: " + pending);
        }
        raw->ready.store(false);
    });

    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        resident_ai_ = std::move(state);
    }
    AddLog("AI worker resident controller started.");
    return true;
}

void DlnaServer::StopResidentAiWorker() {
    std::unique_ptr<ResidentAiState> state;
    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        if (resident_ai_) {
            // Prevent new HTTP users from borrowing this state before removing
            // it from resident_ai_. Existing users hold an active_http_users
            // reference until their resident streaming path is finished.
            resident_ai_->stopping.store(true, std::memory_order_release);
        }
        state = std::move(resident_ai_);
    }
    if (!state) return;

    {
        std::unique_lock<std::mutex> usersLock(state->http_users_mutex);
        state->http_users_cv.wait(usersLock, [&state]() {
            return state->active_http_users.load(std::memory_order_acquire) == 0;
        });
    }

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

bool DlnaServer::SetAiPrewarm(bool enabled, const std::filesystem::path& media_file,
                                AiBackend ai_backend, int directml_device) {
    ai_backend_ = ai_backend;
    directml_device_ = directml_device;

    if (!enabled) {
        // Keep the worker process and the prepared backend alive. Only stop the
        // active stream; mode changes later reuse the same resident process.
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        if (resident_ai_ && resident_ai_->stdin_write) {
            DWORD code = STILL_ACTIVE;
            if (resident_ai_->process &&
                GetExitCodeProcess(resident_ai_->process, &code) &&
                code == STILL_ACTIVE) {
                std::lock_guard<std::mutex> controlLock(resident_ai_->control_mutex);
                const char idle[] = "IDLE\n";
                DWORD written = 0;
                WriteFile(resident_ai_->stdin_write, idle,
                          static_cast<DWORD>(sizeof(idle) - 1), &written, nullptr);
            }
        }
        return true;
    }

    bool residentAlive = false;
    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        if (resident_ai_ && resident_ai_->process) {
            DWORD code = STILL_ACTIVE;
            residentAlive =
                GetExitCodeProcess(resident_ai_->process, &code) &&
                code == STILL_ACTIVE;
        }
    }

    if (!residentAlive) {
        StopResidentAiWorker();
        if (!StartResidentAiWorker(media_file)) {
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(resident_ai_mutex_);
    if (!resident_ai_ || !resident_ai_->stdin_write) {
        return false;
    }

    if (resident_ai_->input == media_file &&
        resident_ai_->backend == ai_backend &&
        resident_ai_->directml_device == directml_device &&
        resident_ai_->ready.load()) {
        return true;
    }

    resident_ai_->input = media_file;
    resident_ai_->backend = ai_backend;
    resident_ai_->directml_device = directml_device;
    resident_ai_->prepare_failed.store(false);
    resident_ai_->ready.store(false);
    int64_t ignoredDuration = 0;
    ProbeMedia(media_file, ignoredDuration,
               resident_ai_->width, resident_ai_->height);

    const int device =
        ai_backend == AiBackend::NvidiaCuda ? 0 :
        (ai_backend == AiBackend::DirectML ? directml_device : -1);
    const std::string command =
        "PREPARE " + AiBackendArgument(ai_backend) + " " +
        std::to_string(device) + " " +
        HexEncode(WideToUtf8(media_file.wstring())) + "\n";

    std::lock_guard<std::mutex> controlLock(resident_ai_->control_mutex);
    DWORD written = 0;
    const bool ok =
        WriteFile(resident_ai_->stdin_write, command.data(),
                  static_cast<DWORD>(command.size()), &written, nullptr) &&
        written == command.size();
    if (!ok) {
        AddLog("AI resident PREPARE command failed Win32=" +
               std::to_string(GetLastError()));
        return false;
    }

    AddLog("AI resident backend prepare requested backend=" +
           AiBackendArgument(ai_backend) +
           " device=" + std::to_string(device) + ".");
    return true;
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
    return FindFiles(media_directory, true);
}

std::vector<std::filesystem::path> DlnaServer::FindFiles(
    const std::filesystem::path& media_directory, bool video_files_only) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(media_directory, ec)) {
        std::lock_guard<std::mutex> lock(g_last_find_files_root_mutex);
        g_last_find_files_root.clear();
        return result;
    }

    std::filesystem::path normalizedRoot =
        std::filesystem::absolute(media_directory, ec).lexically_normal();
    if (ec) {
        ec.clear();
        normalizedRoot = media_directory.lexically_normal();
    }
    {
        std::lock_guard<std::mutex> lock(g_last_find_files_root_mutex);
        g_last_find_files_root = normalizedRoot;
    }

    std::filesystem::recursive_directory_iterator iterator(
        media_directory,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        const auto& entry = *iterator;
        std::error_code fileError;
        if (entry.is_regular_file(fileError) && !fileError) {
            if (!video_files_only) {
                result.push_back(entry.path());
            } else if (IsSupportedVideoExtension(entry.path())) {
                int64_t duration = 0;
                int width = 0;
                int height = 0;
                if (ProbeMedia(entry.path(), duration, width, height)) {
                    result.push_back(entry.path());
                }
            }
        }
        iterator.increment(ec);
        if (ec) ec.clear();
    }

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        std::wstring a = left.wstring();
        std::wstring b = right.wstring();
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

bool DlnaServer::IsShareableFile(const std::filesystem::path& file,
                                 bool video_files_only) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || ec) return false;
    return !video_files_only || IsMediaFile(file);
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
                       AiOutputMode ai_output_mode,
                       AiBackend ai_backend,
                       int directml_device,
                       bool video_files_only) {
    Stop();

    if (media_files.empty()) {
        SetStatus(video_files_only ? "No video files are selected."
                                   : "No files are selected.");
        return false;
    }
    if (advertised_ip.empty()) {
        SetStatus("No local IPv4 address selected.");
        return false;
    }

    media_items_.clear();
    directory_items_.clear();
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
        if (!std::filesystem::is_regular_file(normalized, ec) || ec) {
            ec.clear();
            continue;
        }
        item.size = std::filesystem::file_size(normalized, ec);
        if (ec) {
            ec.clear();
            item.size = 0;
        }

        if (IsSupportedVideoExtension(normalized)) {
            item.is_video = ProbeMedia(
                normalized, item.duration_ms, item.width, item.height);
        }
        if (video_files_only && !item.is_video) {
            AddLog("Skipped unsupported/unreadable video: " +
                   WideToUtf8(normalized.filename().wstring()));
            continue;
        }

        item.id = nextId++;
        media_items_.push_back(std::move(item));
    }
    if (media_items_.empty()) {
        SetStatus(video_files_only
            ? "No supported video files were found in the selection."
            : "No readable files were found in the selection.");
        return false;
    }

    // Build a real ContentDirectory folder tree. FindFiles() remembers the
    // selected folder used by the UI, so that folder itself maps to object 0
    // and each descendant folder becomes a DLNA container.
    std::filesystem::path browseRoot;
    {
        std::lock_guard<std::mutex> lock(g_last_find_files_root_mutex);
        browseRoot = g_last_find_files_root;
    }

    std::unordered_map<std::wstring, std::string> directoryIds;
    auto pathKey = [](const std::filesystem::path& path) {
        std::wstring key = path.lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        return key;
    };

    auto ensureDirectory = [&](const std::filesystem::path& directory) {
        if (browseRoot.empty()) return std::string("0");

        const std::filesystem::path relative =
            directory.lexically_normal().lexically_relative(browseRoot);
        if (relative.empty() || relative == L".") return std::string("0");

        for (const auto& component : relative) {
            if (component == L"..") return std::string("0");
        }

        std::filesystem::path current = browseRoot;
        std::string parentId = "0";
        for (const auto& component : relative) {
            if (component.empty() || component == L".") continue;
            current /= component;
            const std::wstring key = pathKey(current);
            const auto existing = directoryIds.find(key);
            if (existing != directoryIds.end()) {
                parentId = existing->second;
                continue;
            }

            DirectoryItem item;
            item.id = "D" + std::to_string(directory_items_.size() + 1);
            item.parent_id = parentId;
            item.path = current.lexically_normal();
            parentId = item.id;
            directoryIds.emplace(key, item.id);
            directory_items_.push_back(std::move(item));
        }
        return parentId;
    };

    for (auto& item : media_items_) {
        item.parent_id = ensureDirectory(item.path.parent_path());
    }

    advertised_ip_ = advertised_ip;
    ai_passthrough_.store(ai_passthrough, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ai_mode_mutex_);
        selected_ai_passthrough_ = ai_passthrough;
        ai_passthrough_change_pending_ = false;
        ai_output_mode_ = ai_output_mode;
        active_ai_output_mode_ = ai_output_mode;
        ai_output_mode_change_pending_ = false;
        active_ai_media_id_ = 0;
    }
    ai_backend_ = ai_backend;
    directml_device_ = directml_device;
    video_files_only_ = video_files_only;

    // CUDA/DirectML/CPU initialization starts only when the user starts the
    // DLNA server.  If AI Passthrough was requested, do not report Running
    // until the selected backend/RVM worker is actually ready.
    const auto firstVideo = std::find_if(
        media_items_.begin(), media_items_.end(),
        [](const MediaItem& item) { return item.is_video; });
    if (ai_passthrough_.load(std::memory_order_relaxed) &&
        firstVideo != media_items_.end()) {
        SetStatus("Starting AI Passthrough...");
        AddLog("AI Passthrough startup: initializing backend=" +
               AiBackendArgument(ai_backend_) + ".");

        bool backendReady = false;
        std::string startupFailure;

        // A resident controller can stay alive after PREPARE itself failed.
        // Try the selected backend once more from a completely new worker, but
        // never keep waiting for READY after a terminal PREPARE error.
        for (int attempt = 0; attempt < 2 && !backendReady; ++attempt) {
            startupFailure.clear();
            bool retryableFailure = false;

            if (!SetAiPrewarm(true, firstVideo->path,
                              ai_backend_, directml_device_)) {
                startupFailure =
                    "AI Passthrough startup failed before backend preparation.";
                retryableFailure = true;
            } else {
                const auto startupDeadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(60);

                for (;;) {
                    if (IsAiPrewarmReady()) {
                        backendReady = true;
                        break;
                    }

                    bool workerAlive = false;
                    bool prepareFailed = false;
                    DWORD workerExitCode = STILL_ACTIVE;
                    {
                        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
                        if (resident_ai_) {
                            prepareFailed =
                                resident_ai_->prepare_failed.load();
                            if (resident_ai_->process) {
                                workerAlive =
                                    GetExitCodeProcess(
                                        resident_ai_->process,
                                        &workerExitCode) &&
                                    workerExitCode == STILL_ACTIVE;
                            }
                        }
                    }

                    if (prepareFailed) {
                        startupFailure =
                            "AI Passthrough startup failed: worker reported "
                            "backend preparation failure.";
                        retryableFailure = true;
                        break;
                    }

                    if (!workerAlive) {
                        startupFailure =
                            "AI Passthrough startup failed: worker exited before ready"
                            " (exit=" + std::to_string(workerExitCode) + ").";
                        retryableFailure = true;
                        break;
                    }

                    if (std::chrono::steady_clock::now() >= startupDeadline) {
                        startupFailure =
                            "AI Passthrough startup failed: backend initialization "
                            "timed out after 60 seconds.";
                        break;
                    }
                    Sleep(20);
                }
            }

            if (!backendReady) {
                StopResidentAiWorker();
                if (attempt == 0 && retryableFailure) {
                    AddLog(startupFailure +
                           " Retrying selected backend once with a new worker.");
                } else {
                    break;
                }
            }
        }

        if (!backendReady) {
            SetStatus("AI Passthrough startup failed.");
            AddLog(startupFailure);
            return false;
        }

        AddLog("AI Passthrough startup: backend ready.");
    } else {
        SetAiPrewarm(false, {}, ai_backend_, directml_device_);
    }
    uuid_ = MakeUuid();

    if (!SetupHttpListener()) {
        StopResidentAiWorker();
        SetStatus("Could not open an HTTP port.");
        return false;
    }
    if (!SetupSsdpSocket()) {
        closesocket(http_listen_socket_);
        http_listen_socket_ = INVALID_SOCKET;
        StopResidentAiWorker();
        SetStatus("Could not open SSDP port 1900. Check firewall/network settings.");
        return false;
    }

    running_ = true;
    SetStatus("Running");
    AddLog("DLNA server started: http://" + advertised_ip_ + ":" +
           std::to_string(http_port_) + "/media/<id>");
    AddLog(std::string(video_files_only_ ? "Video files: " : "Shared files: ") +
           std::to_string(media_items_.size()));
    AddLog("DLNA folders: " + std::to_string(directory_items_.size()));
    AddLog(std::string("AI Passthrough: ") +
           (ai_passthrough_.load(std::memory_order_relaxed) ? "ON" : "OFF"));
    if (ai_passthrough_.load(std::memory_order_relaxed)) {
        AddLog("AI Output: " + AiOutputLabel());
    }

    http_thread_ = std::thread(&DlnaServer::HttpLoop, this);
    ssdp_thread_ = std::thread(&DlnaServer::SsdpLoop, this);
    SendAliveNotifications();
    return true;
}

void DlnaServer::SetAiPassthroughMode(bool enabled, AiOutputMode mode) {
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(ai_mode_mutex_);
        selected_ai_passthrough_ = enabled;
        ai_output_mode_ = mode;

        if (!running_.load()) {
            ai_passthrough_.store(enabled, std::memory_order_relaxed);
            active_ai_output_mode_ = mode;
            ai_passthrough_change_pending_ = false;
            ai_output_mode_change_pending_ = false;
        } else {
            ai_passthrough_change_pending_ =
                enabled != ai_passthrough_.load(std::memory_order_relaxed);
            ai_output_mode_change_pending_ =
                enabled && mode != active_ai_output_mode_;
        }
        pending = ai_passthrough_change_pending_ ||
                  ai_output_mode_change_pending_;
    }

    if (!enabled) {
        AddLog(std::string("AI Passthrough ") +
               (pending ? "pending: OFF" : "selected: OFF"));
    } else {
        AddLog(std::string("AI Passthrough ") +
               (pending ? "pending: ON, " : "selected: ON, ") +
               AiOutputModeDisplayName(mode));
    }
}

void DlnaServer::SetAiOutputMode(AiOutputMode mode) {
    bool enabled = true;
    {
        std::lock_guard<std::mutex> lock(ai_mode_mutex_);
        enabled = selected_ai_passthrough_;
    }
    SetAiPassthroughMode(enabled, mode);
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

    // The AI worker belongs to the running DLNA server.  Do not leave a
    // prepared/failed GPU state alive after Stop(); the next Start() must retry
    // initialization from a clean state.
    StopResidentAiWorker();
    {
        std::lock_guard<std::mutex> lock(ai_mode_mutex_);
        active_ai_media_id_ = 0;
        ai_passthrough_.store(selected_ai_passthrough_,
                              std::memory_order_relaxed);
        active_ai_output_mode_ = ai_output_mode_;
        ai_passthrough_change_pending_ = false;
        ai_output_mode_change_pending_ = false;
    }
}

std::string DlnaServer::AdvertisedIp() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return advertised_ip_;
}

std::string DlnaServer::BaseUrl() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (advertised_ip_.empty() || http_port_ == 0) return {};
    return "http://" + advertised_ip_ + ":" +
           std::to_string(http_port_) + "/";
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

void DlnaServer::SetAiErrorCallback(
        std::function<void(const AiErrorInfo&)> callback) {
    std::lock_guard<std::mutex> lock(ai_error_callback_mutex_);
    ai_error_callback_ = std::move(callback);
}

void DlnaServer::ReportAiError(const std::string& severity,
                               const std::string& gpu,
                               const std::string& mode,
                               const std::string& codec,
                               const std::string& message) {
    AiErrorInfo info;
    info.severity = severity.empty() ? "ERROR" : severity;
    info.gpu = gpu.empty() ? "unknown" : gpu;
    info.mode = mode.empty() ? "unknown" : mode;
    info.codec = codec.empty() ? "unknown" : codec;
    info.message = message;

    std::function<void(const AiErrorInfo&)> callback;
    {
        std::lock_guard<std::mutex> lock(ai_error_callback_mutex_);
        callback = ai_error_callback_;
    }
    if (callback) callback(info);
}

void DlnaServer::HandleWorkerDiagnosticLine(const std::string& line,
                                            std::string& gpu,
                                            std::string& mode,
                                            std::string& codec) {
    // Event-driven only: this runs only when the worker writes a stderr line.
    // There is no timer, polling, log-history scan, or video-frame-path check.
    constexpr std::string_view decoderKey = "decoder selected name=";
    if (const size_t pos = line.find(decoderKey); pos != std::string::npos) {
        const size_t begin = pos + decoderKey.size();
        size_t end = line.find_first_of(" \t\r\n", begin);
        if (end == std::string::npos) end = line.size();
        codec = DisplayCodecName(line.substr(begin, end - begin));
    }

    if (line.find("RVM backend=DirectML") != std::string::npos) {
        constexpr std::string_view nameKey = "name=\"";
        if (const size_t pos = line.find(nameKey); pos != std::string::npos) {
            const size_t begin = pos + nameKey.size();
            const size_t end = line.find('\"', begin);
            if (end != std::string::npos && end > begin) {
                gpu = line.substr(begin, end - begin);
            }
        }
        if (gpu.empty()) gpu = "DirectML";
    } else if (line.find("RVM backend=CPU") != std::string::npos) {
        gpu = "CPU";
    }

    if (line.find("AlphaPacked") != std::string::npos) {
        mode = "Alpha Packed";
    } else if (line.find("chroma-key") != std::string::npos ||
               line.find("Chroma Key") != std::string::npos) {
        mode = "Chroma Key";
    } else if (line.find("libvpx-vp9") != std::string::npos ||
               line.find("WebM VP9") != std::string::npos) {
        mode = "Alpha WebM VP9";
    }

    const auto report = [&](const std::string& severity,
                            const std::string& message) {
        const std::string displayGpu = gpu.empty()
            ? AiBackendDisplayName(ai_backend_, directml_device_)
            : gpu;
        AiOutputMode selectedMode;
        {
            std::lock_guard<std::mutex> lock(ai_mode_mutex_);
            selectedMode = ai_output_mode_;
        }
        const std::string displayMode = mode.empty()
            ? AiOutputModeDisplayName(selectedMode)
            : mode;
        ReportAiError(severity, displayGpu, displayMode,
                      codec.empty() ? "unknown" : codec, message);
    };

    constexpr std::string_view errorMarker = "PERSISTENT_ERROR:";
    const size_t errorPos = line.find(errorMarker);
    if (errorPos != std::string::npos) {
        std::string detail = Trim(line.substr(errorPos + errorMarker.size()));
        std::string severity = "ERROR";
        constexpr std::string_view fallbackPrefix = "FALLBACK:";
        constexpr std::string_view fatalPrefix = "FATAL:";
        if (detail.rfind(fallbackPrefix, 0) == 0) {
            severity = "FALLBACK";
            detail = Trim(detail.substr(fallbackPrefix.size()));
        } else if (detail.rfind(fatalPrefix, 0) == 0) {
            severity = "FATAL";
            detail = Trim(detail.substr(fatalPrefix.size()));
        }
        report(severity, detail);
        return;
    }

    // Existing resident-worker failures predate PERSISTENT_ERROR. They trigger
    // a one-shot worker fallback, so surface them immediately instead of hiding
    // them in the scrolling log.
    constexpr std::string_view residentError = "RESIDENT_STREAM_ERROR";
    if (const size_t pos = line.find(residentError); pos != std::string::npos) {
        report("FALLBACK", Trim(line.substr(pos)));
        return;
    }

    // Existing one-shot initialization failures also predate PERSISTENT_ERROR.
    if (const size_t pos = line.find("ERROR init:"); pos != std::string::npos) {
        report("FATAL", Trim(line.substr(pos)));
        return;
    }

    // FFmpeg can reject D3D11 hardware decode and silently continue with a
    // software frame. That is a real fallback and must remain visible.
    if (line.find("Failed setup for format d3d11") != std::string::npos ||
        line.find("hwaccel initialisation returned error") != std::string::npos) {
        report("FALLBACK", line);
    }
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

    // Only our own player is a guaranteed compatibility exception. All other
    // clients use the generic DLNA/HTTP virtual-range experiment.
    g_r800zz_vrplayer_request = IsR800zzVrPlayerRequest(request);
    if (ai_passthrough_.load(std::memory_order_relaxed) &&
        (routePathLower == "/contentdirectory/control" ||
         routePathLower == "/connectionmanager/control" ||
         routePathLower == "/media" ||
         routePathLower.rfind("/media/", 0) == 0)) {
        AddLog(std::string("CLIENT_PROFILE=") +
               (g_r800zz_vrplayer_request ? "R800ZZ" : "GENERIC_SEEKABLE") +
               " request=\"" + FirstLine(request) + "\"");
    }

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

    if ((method == "GET" || method == "HEAD") &&
        (routePathLower == "/" || routePathLower == "/pc2hmd.html")) {
        const std::string body = Pc2HmdHtml();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "SERVER: Windows/10.0 UPnP/1.1 R800ZZ-DLNA/1.0\r\n"
                 << "Content-Type: text/html; charset=\"utf-8\"\r\n"
                 << "Cache-Control: no-store\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n\r\n";
        if (method != "HEAD") response << body;
        SendAll(client, response.str());
        AddLog(method + " " + routePath + " -> 200 PC-to-HMD page");
    } else if ((method == "GET" || method == "HEAD") &&
        routePathLower == "/-_-9213--api/files") {
        std::ostringstream json;
        json << "[\n";
        for (size_t i = 0; i < media_items_.size(); ++i) {
            const MediaItem& item = media_items_[i];
            if (i != 0) json << ",\n";
            json << "  {\"id\":" << item.id
                 << ",\"name\":\""
                 << JsonEscape(WideToUtf8(item.path.filename().wstring())) << "\""
                 << ",\"url\":\"" << JsonEscape(MediaUrl(item)) << "\""
                 << ",\"downloadUrl\":\"http://"
                 << JsonEscape(advertised_ip_) << ':' << http_port_
                 << "/-_-9213--api/download/" << item.id << '/'
                 << UrlEncodePathSegment(WideToUtf8(item.path.filename().wstring()))
                 << "\""
                 << ",\"size\":" << item.size
                 << ",\"parentId\":\"" << JsonEscape(item.parent_id) << "\""
                 << ",\"isVideo\":" << (item.is_video ? "true" : "false")
                 << "}";
        }
        json << "\n]\n";
        const std::string body = json.str();
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n"
                 << "SERVER: Windows/10.0 UPnP/1.1 R800ZZ-DLNA/1.0\r\n"
                 << "Content-Type: application/json; charset=\"utf-8\"\r\n"
                 << "Access-Control-Allow-Origin: *\r\n"
                 << "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                 << "Access-Control-Allow-Headers: *\r\n"
                 << "Cache-Control: no-store\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n\r\n";
        if (method != "HEAD") response << body;
        SendAll(client, response.str());
        AddLog(method + " /-_-9213--api/files -> 200 files=" +
               std::to_string(media_items_.size()));
    } else if (method == "OPTIONS" &&
               (routePathLower == "/-_-9213--api/files" ||
                routePathLower.rfind("/-_-9213--api/download/", 0) == 0)) {
        SendAll(client,
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                "Access-Control-Allow-Headers: *\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n");
    } else if ((method == "GET" || method == "HEAD") &&
               routePathLower.rfind("/-_-9213--api/download/", 0) == 0) {
        constexpr std::string_view prefix = "/-_-9213--api/download/";
        std::string idText = routePath.substr(prefix.size());
        const size_t slash = idText.find('/');
        if (slash != std::string::npos) idText.resize(slash);

        const MediaItem* item = nullptr;
        try {
            const unsigned long parsed = std::stoul(idText);
            if (parsed > 0 && parsed <= media_items_.size()) {
                const MediaItem& candidate = media_items_[parsed - 1];
                if (candidate.id == parsed) item = &candidate;
            }
        } catch (...) {
        }

        if (!item) {
            SendAll(client, HttpError(404, "Not Found"));
            closesocket(client);
            return;
        }

        // Downloader path is intentionally independent from DLNA playback and
        // AI passthrough. Always return the exact source file bytes and name.
        const uint64_t total = item->size;
        uint64_t start = 0;
        uint64_t end = total > 0 ? total - 1 : 0;
        const std::string range = HeaderValue(request, "range");
        const bool hasRange = !range.empty();
        const bool partial = total > 0 && hasRange && ParseRange(range, total, start, end);

        if (hasRange && !partial) {
            std::ostringstream response;
            response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                     << "Content-Range: bytes */" << total << "\r\n"
                     << "Accept-Ranges: bytes\r\n"
                     << "Access-Control-Allow-Origin: *\r\n"
                     << "Access-Control-Expose-Headers: Content-Disposition, Content-Length, Content-Range, Accept-Ranges\r\n"
                     << "Content-Length: 0\r\n"
                     << "Connection: close\r\n\r\n";
            SendAll(client, response.str());
            AddLog(method + " /-_-9213--api/download -> 416");
            closesocket(client);
            return;
        }

        const uint64_t length64 = total == 0 ? 0 : end - start + 1;
        const std::string sourceName = WideToUtf8(item->path.filename().wstring());
        std::ostringstream headers;
        headers << "HTTP/1.1 " << (partial ? "206 Partial Content" : "200 OK") << "\r\n"
                << "Content-Type: " << SourceMimeTypeForPath(item->path) << "\r\n"
                << "Content-Disposition: " << DownloadContentDisposition(sourceName) << "\r\n"
                << "Accept-Ranges: bytes\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Access-Control-Expose-Headers: Content-Disposition, Content-Length, Content-Range, Accept-Ranges\r\n"
                << "Cache-Control: no-store\r\n"
                << "Content-Length: " << length64 << "\r\n";
        if (partial) {
            headers << "Content-Range: bytes " << start << '-' << end << '/' << total << "\r\n";
        }
        headers << "Connection: close\r\n\r\n";

        if (!SendAll(client, headers.str()) || method == "HEAD" || length64 == 0) {
            AddLog(method + " /-_-9213--api/download/" + std::to_string(item->id) +
                   " -> " + (partial ? "206" : "200") + " source file");
            closesocket(client);
            return;
        }

        std::ifstream file(item->path, std::ios::binary);
        if (!file) {
            AddLog("download API failed to open source file");
            closesocket(client);
            return;
        }

        file.seekg(static_cast<std::streamoff>(start));
        uint64_t remaining = length64;
        uint64_t sentBody = 0;
        bool sendFailed = false;
        std::array<char, 256 * 1024> fileBuffer{};
        while (remaining > 0 && file && running_) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(remaining, fileBuffer.size()));
            file.read(fileBuffer.data(), static_cast<std::streamsize>(want));
            const std::streamsize got = file.gcount();
            if (got <= 0) break;
            if (!SendAll(client, fileBuffer.data(), static_cast<size_t>(got))) {
                sendFailed = true;
                break;
            }
            sentBody += static_cast<uint64_t>(got);
            remaining -= static_cast<uint64_t>(got);
        }
        AddLog("download API source bytes sent=" + std::to_string(sentBody) +
               " expected=" + std::to_string(length64) +
               (sendFailed ? " (client disconnected/send failed)" : ""));
    } else if ((method == "GET" || method == "HEAD") && routePathLower == "/device.xml") {
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
        if (item->is_video && method == "GET") {
            // AI ON/OFF and output-mode changes use the same two playback
            // boundaries: loading a different media item, or an explicit seek
            // to 0. Never mutate the in-flight RUN that was already started.
            bool explicitZeroSeek = false;
            const std::string timeSeek = HeaderValue(request, "timeseekrange.dlna.org");
            const int64_t dlnaStartMs = ParseDlnaTimeSeekStartMs(timeSeek);
            if (dlnaStartMs >= 0) {
                explicitZeroSeek = dlnaStartMs == 0;
            } else {
                const std::string startHeader = HeaderValue(request, "x-r800zz-start-ms");
                if (!startHeader.empty()) {
                    char* end = nullptr;
                    const long long parsed = std::strtoll(startHeader.c_str(), &end, 10);
                    if (end != startHeader.c_str() && parsed >= 0) {
                        explicitZeroSeek = parsed == 0;
                    }
                }
            }

            bool fileLoadTrigger = false;
            bool zeroSeekTrigger = false;
            bool stateApplied = false;
            bool appliedEnabled = ai_passthrough_.load(std::memory_order_relaxed);
            AiOutputMode appliedMode = AiOutputMode::AlphaPackedHevc;
            {
                std::lock_guard<std::mutex> lock(ai_mode_mutex_);
                fileLoadTrigger = active_ai_media_id_ == 0 ||
                                  active_ai_media_id_ != item->id;
                zeroSeekTrigger = !fileLoadTrigger && explicitZeroSeek;

                if (fileLoadTrigger) {
                    active_ai_media_id_ = item->id;
                }

                if ((fileLoadTrigger || zeroSeekTrigger) &&
                    (ai_passthrough_change_pending_ ||
                     ai_output_mode_change_pending_)) {
                    ai_passthrough_.store(selected_ai_passthrough_,
                                          std::memory_order_relaxed);
                    if (selected_ai_passthrough_) {
                        active_ai_output_mode_ = ai_output_mode_;
                    }
                    ai_passthrough_change_pending_ = false;
                    ai_output_mode_change_pending_ = false;
                    appliedEnabled = selected_ai_passthrough_;
                    appliedMode = active_ai_output_mode_;
                    stateApplied = true;
                }
            }

            if (stateApplied) {
                const std::string boundary =
                    fileLoadTrigger ? "file load" : "zero seek";
                if (appliedEnabled) {
                    AddLog("AI Passthrough applied at " + boundary +
                           ": ON, " + AiOutputModeDisplayName(appliedMode) + ".");
                } else {
                    AddLog("AI Passthrough applied at " + boundary + ": OFF.");
                }
            }
        }

        if (ai_passthrough_.load(std::memory_order_relaxed) && item->is_video) {
            // CUDA / DirectML / CPU all use the same DLNA/HTTP/pipe streaming path.
            // Non-video files such as .vrm are always served as their raw bytes.
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


void DlnaServer::HandleDirectMlPassthroughStream(
        SOCKET client,
        const std::string& method,
        const std::string& request,
        const MediaItem& item) {
    // Compatibility wrapper: DirectML must use exactly the same streaming
    // response/pipe/timing path as CUDA. Backend differences are selected
    // inside HandleAiPassthroughStream().
    HandleAiPassthroughStream(client, method, request, item);
}

void DlnaServer::HandleAiPassthroughStream(SOCKET client,
                                            const std::string& method,
                                            const std::string& request,
                                            const MediaItem& item) {
    // AI output is generated on demand. DLNA time seek is preferred over
    // HTTP byte-range seek because the transcoded output length is not known.
    int64_t requested_start_ms = 0;
    bool dlna_time_seek_request = false;
    bool legacy_r800zz_seek_request = false;
    bool explicit_zero_seek_request = false;
    const std::string timeSeek = HeaderValue(request, "timeseekrange.dlna.org");
    const int64_t dlnaStartMs = ParseDlnaTimeSeekStartMs(timeSeek);
    if (dlnaStartMs >= 0) {
        requested_start_ms = dlnaStartMs;
        dlna_time_seek_request = true;
        explicit_zero_seek_request = dlnaStartMs == 0;
    } else {
        // Backward-compatible fallback for existing R800ZZ VR Player builds.
        const std::string startHeader = HeaderValue(request, "x-r800zz-start-ms");
        if (!startHeader.empty()) {
            char* end = nullptr;
            const long long parsed = std::strtoll(startHeader.c_str(), &end, 10);
            if (end != startHeader.c_str() && parsed >= 0) {
                requested_start_ms = static_cast<int64_t>(parsed);
                explicit_zero_seek_request = requested_start_ms == 0;
                legacy_r800zz_seek_request = requested_start_ms > 0;
            }
        }
    }

    AiOutputMode requestOutputMode = AiOutputMode::AlphaPackedHevc;
    {
        std::lock_guard<std::mutex> lock(ai_mode_mutex_);
        requestOutputMode = active_ai_output_mode_;
    }

    const auto requestMimeType = [&]() -> const char* {
        return requestOutputMode == AiOutputMode::WebmVp9Alpha
            ? "video/webm" : "video/mp2t";
    };
    const auto requestOutputLabel = [&]() {
        if (requestOutputMode == AiOutputMode::WebmVp9Alpha) {
            return std::string("Alpha: WebM VP9 Alpha/libvpx (may drop frames)");
        }
        if (requestOutputMode == AiOutputMode::ChromaKeyHevc) {
            return std::string("Chroma Key: Green background HEVC/NVENC");
        }
        return std::string("Alpha: AlphaPacked HEVC/NVENC");
    };

    // Generic DLNA/HTTP clients may use the experimental virtual byte range.
    // r800zzvrplayer is the only compatibility exception because its User-Agent
    // and custom X-R800ZZ-Start-Ms behavior are under our control.
    const bool virtual_range_enabled = !g_r800zz_vrplayer_request;
    const bool fixed_muxrate_ts =
        requestOutputMode != AiOutputMode::WebmVp9Alpha;
    const uint64_t virtual_total_bytes = virtual_range_enabled
        ? (fixed_muxrate_ts
            ? RealtimeTsVirtualSizeBytes(item.duration_ms)
            : item.size)
        : 0;
    uint64_t virtual_range_start = 0;
    uint64_t virtual_range_end = virtual_total_bytes > 0 ? virtual_total_bytes - 1 : 0;
    bool virtual_byte_seek_request = false;
    bool virtual_tail_duration_probe = false;
    uint64_t worker_prefix_discard_bytes = 0;
    const std::string byteRange = HeaderValue(request, "range");
    if (!byteRange.empty() && !dlna_time_seek_request && !legacy_r800zz_seek_request) {
        if (!virtual_range_enabled) {
            std::ostringstream response;
            response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                     << "Accept-Ranges: none\r\n"
                     << "Content-Length: 0\r\n"
                     << "Connection: close\r\n\r\n";
            AddLog(method + " /media " + byteRange +
                   " -> 416 byte range disabled for r800zzvrplayer");
            SendAll(client, response.str());
            return;
        }

        if (virtual_total_bytes == 0 || item.duration_ms <= 0 ||
            !ParseRange(byteRange, virtual_total_bytes,
                        virtual_range_start, virtual_range_end)) {
            std::ostringstream response;
            response << "HTTP/1.1 416 Range Not Satisfiable\r\n"
                     << "Accept-Ranges: bytes\r\n";
            if (virtual_total_bytes > 0) {
                response << "Content-Range: bytes */" << virtual_total_bytes << "\r\n";
            }
            response << "Content-Length: 0\r\n"
                     << "Connection: close\r\n\r\n";
            AddLog(method + " /media " + byteRange + " -> 416 virtual AI range");
            SendAll(client, response.str());
            return;
        }

        virtual_byte_seek_request = true;

        const uint64_t requested_range_length =
            virtual_range_end - virtual_range_start + 1;
        virtual_tail_duration_probe =
            fixed_muxrate_ts &&
            virtual_range_end + 1 == virtual_total_bytes &&
            requested_range_length <= kTsDurationProbeBytes &&
            virtual_range_start >=
                (virtual_total_bytes > kTsDurationProbeBytes
                    ? virtual_total_bytes - kTsDurationProbeBytes : 0);

        if (virtual_tail_duration_probe) {
            // Duration readers probe a small window at logical EOF to find
            // the final PCR. A proportional seek can leave only a few
            // milliseconds of source and produce no TS packet. Give the muxer
            // one second of source material, while returning only the exact
            // requested tail byte count.
            requested_start_ms = std::max<int64_t>(
                0, item.duration_ms - kTsDurationProbeLeadMs);
            AddLog("GENERIC_SEEKABLE tail PCR probe range=" + byteRange +
                   " bytes=" + std::to_string(requested_range_length) +
                   " workerStartMs=" + std::to_string(requested_start_ms));
        } else {
            const long double fraction =
                static_cast<long double>(virtual_range_start) /
                static_cast<long double>(virtual_total_bytes);
            requested_start_ms = static_cast<int64_t>(
                fraction * static_cast<long double>(item.duration_ms));
            if (item.duration_ms > 0) {
                requested_start_ms =
                    std::min(requested_start_ms, item.duration_ms - 1);
            }

            // Worker seek granularity is milliseconds, while HTTP Range is
            // byte exact. Discard the remaining sub-millisecond prefix so a
            // request such as bytes=940- does not incorrectly return byte 0.
            if (fixed_muxrate_ts) {
                const long double mapped_bytes_ld =
                    static_cast<long double>(requested_start_ms) *
                    static_cast<long double>(
                        kRealtimeTsMuxRateBitsPerSecond) / 8000.0L;
                const uint64_t mapped_start_byte =
                    mapped_bytes_ld > 0.0L
                        ? static_cast<uint64_t>(std::floor(mapped_bytes_ld))
                        : 0;
                if (virtual_range_start > mapped_start_byte) {
                    worker_prefix_discard_bytes =
                        virtual_range_start - mapped_start_byte;
                }
            }

            AddLog("AI virtual byte seek " + byteRange +
                   " -> startMs=" + std::to_string(requested_start_ms) +
                   " prefixDiscard=" +
                   std::to_string(worker_prefix_discard_bytes) +
                   " virtualTotal=" +
                   std::to_string(virtual_total_bytes));
        }
    }

    if (dlna_time_seek_request) {
        AddLog("DLNA TimeSeekRange request startMs=" +
               std::to_string(requested_start_ms));
        if (!byteRange.empty()) {
            AddLog("DLNA TimeSeekRange takes precedence over HTTP Range: " +
                   byteRange);
        }
    } else if (legacy_r800zz_seek_request && !byteRange.empty()) {
        AddLog("R800ZZ start-ms takes precedence over HTTP Range: " + byteRange);
    }
    if (item.duration_ms > 0 && requested_start_ms >= item.duration_ms) {
        // A player can carry the previous media position into the first GET for
        // a newly selected, shorter item. Seeking to the exact EOF produces no
        // output bytes, so treat an out-of-range time seek as a fresh start.
        // Valid seeks inside an audio-only tail remain unchanged.
        AddLog("AI seek startMs=" + std::to_string(requested_start_ms) +
               " is outside durationMs=" + std::to_string(item.duration_ms) +
               "; starting from 0");
        requested_start_ms = 0;
    }

    const bool virtual_finite_response =
        virtual_range_enabled && virtual_total_bytes > 0 && item.duration_ms > 0 &&
        !dlna_time_seek_request && !legacy_r800zz_seek_request;
    const uint64_t virtual_head_length = virtual_byte_seek_request
        ? (virtual_range_end - virtual_range_start + 1)
        : (virtual_finite_response ? virtual_total_bytes : 0);

    // Only fixed-muxrate MPEG-TS can make streaming GET Content-Length
    // match the bytes actually delivered. WebM keeps its existing behavior.
    const bool finite_ts_body =
        fixed_muxrate_ts && virtual_finite_response;
    const uint64_t virtual_body_length = finite_ts_body
        ? (virtual_byte_seek_request
            ? (virtual_range_end - virtual_range_start + 1)
            : virtual_total_bytes)
        : 0;

    std::string generic_tail_probe_cache_key;
    if (virtual_tail_duration_probe && finite_ts_body) {
        const int cacheDevice =
            ai_backend_ == AiBackend::NvidiaCuda ? 0 :
            (ai_backend_ == AiBackend::DirectML ? directml_device_ : -1);
        generic_tail_probe_cache_key =
            WideToUtf8(item.path.wstring()) + "|" +
            std::to_string(item.size) + "|" +
            std::to_string(item.duration_ms) + "|" +
            AiBackendArgument(ai_backend_) + "|" +
            std::to_string(cacheDevice) + "|" +
            AiOutputModeArgument(requestOutputMode) + "|" +
            std::to_string(virtual_total_bytes);
    }

    if (virtual_range_enabled) {
        AddLog("GENERIC_SEEKABLE totalBytes=" +
               std::to_string(virtual_total_bytes) +
               " muxRateBitsPerSecond=" +
               std::to_string(kRealtimeTsMuxRateBitsPerSecond) +
               " contentLength=" + std::to_string(virtual_body_length) +
               " startMs=" + std::to_string(requested_start_ms) +
               " tailProbe=" + (virtual_tail_duration_probe ? "1" : "0") +
               " prefixDiscard=" +
               std::to_string(worker_prefix_discard_bytes));
    }

    // HEAD is a file/capability probe and never starts the GPU worker.
    if (method == "HEAD") {
        std::ostringstream headers;
        headers << "HTTP/1.1 "
                << (virtual_byte_seek_request ? "206 Partial Content" : "200 OK") << "\r\n"
                << "Content-Type: " << requestMimeType() << "\r\n"
                << "Accept-Ranges: "
                << ((virtual_finite_response || virtual_byte_seek_request) ? "bytes" : "none")
                << "\r\n"
                << "transferMode.dlna.org: Streaming\r\n"
                << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n";
        if (virtual_finite_response || virtual_byte_seek_request) {
            headers << "Content-Length: " << virtual_head_length << "\r\n";
        }
        if (virtual_byte_seek_request) {
            headers << "Content-Range: bytes " << virtual_range_start << '-'
                    << virtual_range_end << '/' << virtual_total_bytes << "\r\n";
        }
        if (item.duration_ms > 0) {
            const std::string duration = FormatNptMs(item.duration_ms);
            headers << "X-R800ZZ-Duration-Ms: " << item.duration_ms << "\r\n"
                    << "X-AvailableSeekRange: 1 npt=0.000-" << duration << "\r\n";
        }
        headers << "EXT:\r\n"
                << "Connection: close\r\n\r\n";
        AddLog(std::string("HEAD /media -> ") +
               (virtual_byte_seek_request ? "206 virtual AI range" :
                                            "200 virtual finite AI file") +
               " Content-Length=" + std::to_string(virtual_head_length) +
               " totalBytes=" + std::to_string(virtual_total_bytes));
        SendAll(client, headers.str());
        return;
    }

    // ExoPlayer/Media3 repeats the logical-EOF PCR probe when rebuilding its
    // extractor around a seek. The probe bytes are stable for the same media /
    // backend / output mode, so reuse the first successful result instead of
    // interrupting the resident AI stream with another RUN command.
    if (method == "GET" && virtual_tail_duration_probe && finite_ts_body) {
        std::vector<char> cachedTail;
        if (LoadGenericTailProbeCache(
                generic_tail_probe_cache_key, virtual_body_length, cachedTail)) {
            std::ostringstream headers;
            headers << "HTTP/1.1 206 Partial Content\r\n"
                    << "Content-Type: " << requestMimeType() << "\r\n"
                    << "Accept-Ranges: bytes\r\n"
                    << "transferMode.dlna.org: Streaming\r\n"
                    << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n"
                    << "Content-Length: " << cachedTail.size() << "\r\n"
                    << "Content-Range: bytes " << virtual_range_start << '-'
                    << virtual_range_end << '/' << virtual_total_bytes << "\r\n";
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

            const bool ok = SendAll(client, headers.str()) &&
                SendAll(client, cachedTail.data(), cachedTail.size());
            AddLog("GENERIC_SEEKABLE tail PCR probe cache hit bytes=" +
                   std::to_string(cachedTail.size()) +
                   (ok ? "" : " (client disconnected/send failed)"));
            return;
        }
        AddLog("GENERIC_SEEKABLE tail PCR probe cache miss");
    }

    // Keep the 10/10 stream-handoff protection only for r800zzvrplayer.
    // Generic DLNA clients may open independent Range/PCR probe GETs while the
    // main playback GET is still active.  Those requests must not be treated as
    // replacement playback streams; this preserves the previously working
    // generic/DeoVR resident-worker behavior.
    const bool useStreamHandoff = g_r800zz_vrplayer_request;
    const uint64_t aiStreamGeneration = useStreamHandoff
        ? g_ai_stream_generation.fetch_add(1, std::memory_order_acq_rel) + 1
        : 0;

    if (useStreamHandoff) {
        std::lock_guard<std::mutex> ownerLock(g_ai_stream_owner_mutex);
        if (g_active_ai_stream_client != INVALID_SOCKET &&
            g_active_ai_stream_client != client) {
            shutdown(g_active_ai_stream_client, SD_BOTH);
        }
        g_active_ai_stream_client = client;
    }

    struct AiStreamOwnerGuard {
        SOCKET client{INVALID_SOCKET};
        uint64_t generation{0};
        bool enabled{false};
        ~AiStreamOwnerGuard() {
            if (!enabled) return;
            std::lock_guard<std::mutex> ownerLock(g_ai_stream_owner_mutex);
            if (g_active_ai_stream_client == client &&
                g_ai_stream_generation.load(std::memory_order_acquire) == generation) {
                g_active_ai_stream_client = INVALID_SOCKET;
            }
        }
    } aiStreamOwner{client, aiStreamGeneration, useStreamHandoff};

    std::unique_lock<std::mutex> aiStreamLock;
    if (useStreamHandoff) {
        aiStreamLock = std::unique_lock<std::mutex>(g_ai_stream_serial_mutex);
        if (!running_ ||
            g_ai_stream_generation.load(std::memory_order_acquire) != aiStreamGeneration) {
            return;
        }
        {
            std::lock_guard<std::mutex> ownerLock(g_ai_stream_owner_mutex);
            if (g_active_ai_stream_client != client) {
                return;
            }
        }
    }

    const auto aiStreamSuperseded = [&]() -> bool {
        return !running_ ||
            (useStreamHandoff &&
             g_ai_stream_generation.load(std::memory_order_acquire) !=
                 aiStreamGeneration);
    };

    const bool cudaBackend = ai_backend_ == AiBackend::NvidiaCuda;
    // CUDA / DirectML / CPU all use the same worker executable.
    const std::filesystem::path worker =
        ExecutableDirectory() / L"r800zz_ai_worker.exe";
    const std::filesystem::path model = cudaBackend
        ? ExecutableDirectory() / L"rvm_mobilenetv3_fp16.onnx"
        : ExecutableDirectory() / L"rvm_mobilenetv3_fp32.onnx";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(worker, ec)) {
        AddLog(std::string("AI Passthrough error: worker missing: ") +
               WideToUtf8(worker.wstring()));
        SendAll(client, HttpError(500, "AI Worker Missing"));
        return;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(model, ec)) {
        AddLog(std::string("AI Passthrough error: model missing: ") +
               WideToUtf8(model.wstring()));
        SendAll(client, HttpError(500, "RVM Model Missing"));
        return;
    }

    struct ResidentAiUseGuard {
        ResidentAiState* state{nullptr};

        void release() {
            if (!state) return;
            ResidentAiState* current = state;
            state = nullptr;
            if (current->active_http_users.fetch_sub(
                    1, std::memory_order_acq_rel) == 1) {
                current->http_users_cv.notify_all();
            }
        }

        ~ResidentAiUseGuard() {
            release();
        }
    } residentUse;

    ResidentAiState* resident = nullptr;
    {
        std::lock_guard<std::mutex> lock(resident_ai_mutex_);
        if (resident_ai_ &&
            !resident_ai_->stopping.load(std::memory_order_acquire)) {
            DWORD code = STILL_ACTIVE;
            if (resident_ai_->process && resident_ai_->stdin_write &&
                GetExitCodeProcess(resident_ai_->process, &code) &&
                code == STILL_ACTIVE) {
                resident = resident_ai_.get();
                resident->active_http_users.fetch_add(
                    1, std::memory_order_acq_rel);
                residentUse.state = resident;
            }
        }
    }

    // Do not gate RUN on the currently prepared backend or media dimensions.
    // RUN carries backend/device/input and the resident worker switches its
    // internal session in-process when necessary.

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
            const std::string message =
                "AI resident output pipe creation failed Win32=" +
                std::to_string(GetLastError()) + "; using one-shot worker.";
            AddLog(message);
            ReportAiError("FALLBACK",
                          AiBackendDisplayName(ai_backend_, directml_device_),
                          AiOutputModeDisplayName(requestOutputMode),
                          "unknown", message);
            residentUse.release();
            resident = nullptr;
        } else {
            const int residentDevice =
                ai_backend_ == AiBackend::NvidiaCuda ? 0 :
                (ai_backend_ == AiBackend::DirectML ? directml_device_ : -1);
            const std::string runCommand =
                "RUN " + std::to_string(sequence) + " " +
                std::to_string(requested_start_ms) + " " +
                AiBackendArgument(ai_backend_) + " " +
                std::to_string(residentDevice) + " " +
                AiOutputModeArgument(requestOutputMode) + " " + pipeName + " " +
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
                const std::string message =
                    "AI resident RUN command failed Win32=" +
                    std::to_string(GetLastError()) + "; using one-shot worker.";
                AddLog(message);
                ReportAiError("FALLBACK",
                              AiBackendDisplayName(ai_backend_, directml_device_),
                              AiOutputModeDisplayName(requestOutputMode),
                              "unknown", message);
                CloseHandle(streamPipe);
                residentUse.release();
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
                std::vector<char> data(256 * 1024);
                DWORD firstGot = 0;
                const auto startupBegin = std::chrono::steady_clock::now();

                while (!connected) {
                    if (aiStreamSuperseded()) {
                        startupFailed = true;
                        break;
                    }
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
                    if (aiStreamSuperseded()) {
                        startupFailed = true;
                        break;
                    }
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
                    if (aiStreamSuperseded()) {
                        AddLog("AI resident stream replaced by a newer media request before first output.");
                        return;
                    }
                    const std::string message = startupTimeout
                        ? "AI resident stream startup timed out; using one-shot worker."
                        : "AI resident stream failed before first output byte; using one-shot worker.";
                    AddLog(message);
                    ReportAiError("FALLBACK",
                                  AiBackendDisplayName(ai_backend_, directml_device_),
                                  AiOutputModeDisplayName(requestOutputMode),
                                  "unknown", message);
                    residentUse.release();
                    resident = nullptr;
                } else {
                    const auto startupMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startupBegin).count();
                    AddLog("AI resident first output bytes ready=" +
                           std::to_string(firstGot) +
                           " startupMs=" + std::to_string(startupMs));

                    std::ostringstream headers;
                    headers << "HTTP/1.1 "
                            << ((dlna_time_seek_request || legacy_r800zz_seek_request || virtual_byte_seek_request)
                                ? "206 Partial Content" : "200 OK") << "\r\n"
                            << "Content-Type: " << requestMimeType() << "\r\n"
                            << "Accept-Ranges: "
                            << ((virtual_finite_response || virtual_byte_seek_request) ? "bytes" : "none")
                            << "\r\n"
                            << "transferMode.dlna.org: Streaming\r\n"
                            << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n";
                    if (finite_ts_body) {
                        headers << "Content-Length: " << virtual_body_length << "\r\n";
                    }
                    if (virtual_byte_seek_request) {
                        headers << "Content-Range: bytes " << virtual_range_start << '-'
                                << virtual_range_end << '/' << virtual_total_bytes << "\r\n";
                    }
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
                    bool virtualBodyComplete = false;
                    std::vector<char> tailProbeBody;
                    if (virtual_tail_duration_probe) {
                        tailProbeBody.reserve(static_cast<size_t>(virtual_body_length));
                    }

                    uint64_t prefixDiscardRemaining =
                        worker_prefix_discard_bytes;
                    auto sendBodyChunk = [&](const char* bytes, size_t count) -> bool {
                        if (count == 0) return true;
                        if (prefixDiscardRemaining > 0) {
                            const size_t discard = static_cast<size_t>(
                                std::min<uint64_t>(
                                    prefixDiscardRemaining,
                                    static_cast<uint64_t>(count)));
                            bytes += discard;
                            count -= discard;
                            prefixDiscardRemaining -=
                                static_cast<uint64_t>(discard);
                            if (count == 0) return true;
                        }
                        if (virtual_tail_duration_probe) {
                            KeepTailBytes(tailProbeBody, bytes, count,
                                          static_cast<size_t>(virtual_body_length));
                            return true;
                        }
                        size_t sendSize = count;
                        if (finite_ts_body) {
                            if (sentBody >= virtual_body_length) {
                                virtualBodyComplete = true;
                                return true;
                            }
                            sendSize = static_cast<size_t>(std::min<uint64_t>(
                                static_cast<uint64_t>(count),
                                virtual_body_length - sentBody));
                        }
                        if (sendSize > 0 && !SendAll(client, bytes, sendSize)) return false;
                        sentBody += static_cast<uint64_t>(sendSize);
                        if (finite_ts_body && sentBody >= virtual_body_length) {
                            virtualBodyComplete = true;
                        }
                        return true;
                    };

                    if (!sendFailed) {
                        sendFailed = !sendBodyChunk(
                            data.data(), static_cast<size_t>(firstGot));
                    }

                    auto pipeGapStart = std::chrono::steady_clock::time_point{};
                    while (!sendFailed && !virtualBodyComplete &&
                           running_ && !aiStreamSuperseded()) {
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
                            if (pipeGapStart == std::chrono::steady_clock::time_point{}) {
                                pipeGapStart = std::chrono::steady_clock::now();
                            }
                            Sleep(2);
                            continue;
                        }
                        if (pipeGapStart != std::chrono::steady_clock::time_point{}) {
                            const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - pipeGapStart).count();
                            // High-frequency pipe timing diagnostics are intentionally
                            // not written to the normal UI log. They hide real errors
                            // and add formatting/mutex overhead on the streaming path.
                            (void)gapMs;
                            pipeGapStart = std::chrono::steady_clock::time_point{};
                        }
                        DWORD got = 0;
                        const DWORD want = static_cast<DWORD>(
                            std::min<size_t>(data.size(), available));
                        if (!ReadFile(streamPipe, data.data(), want, &got, nullptr) || got == 0) {
                            sendFailed = true;
                            break;
                        }
                        const auto sendStart = std::chrono::steady_clock::now();
                        if (!sendBodyChunk(data.data(), static_cast<size_t>(got))) {
                            sendFailed = true;
                            break;
                        }
                        const auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - sendStart).count();
                        // Keep send timing off the normal UI log.
                        (void)sendMs;
                    }

                    if (!sendFailed && virtual_tail_duration_probe) {
                        if (tailProbeBody.size() == virtual_body_length) {
                            StoreGenericTailProbeCache(
                                generic_tail_probe_cache_key, tailProbeBody);
                            if (!SendAll(client, tailProbeBody.data(), tailProbeBody.size())) {
                                sendFailed = true;
                            } else {
                                sentBody = static_cast<uint64_t>(tailProbeBody.size());
                                virtualBodyComplete = true;
                                AddLog("GENERIC_SEEKABLE tail PCR probe returned actual EOF tail bytes=" +
                                       std::to_string(sentBody));
                            }
                        } else {
                            AddLog("GENERIC_SEEKABLE tail PCR probe ended short bytes=" +
                                   std::to_string(tailProbeBody.size()) +
                                   " expected=" + std::to_string(virtual_body_length));
                        }
                    }

                    if (!sendFailed && !virtual_tail_duration_probe &&
                        finite_ts_body && sentBody < virtual_body_length) {
                        const int64_t remainingMs = std::max<int64_t>(
                            0, item.duration_ms - requested_start_ms);
                        const auto elapsedMs =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - startupBegin).count();
                        const bool naturalEofLikely =
                            remainingMs <= 0 || elapsedMs + 2000 >= remainingMs;
                        const uint64_t missing = virtual_body_length - sentBody;
                        if (naturalEofLikely &&
                            (missing % kMpegTsPacketSize) == 0) {
                            AddLog("GENERIC_SEEKABLE natural EOF padding MPEG-TS bytes=" +
                                   std::to_string(missing) +
                                   " elapsedMs=" + std::to_string(elapsedMs) +
                                   " expectedPlaybackMs=" + std::to_string(remainingMs));
                            if (SendMpegTsNullPadding(client, missing)) {
                                sentBody = virtual_body_length;
                                virtualBodyComplete = true;
                            } else {
                                sendFailed = true;
                            }
                        } else {
                            AddLog("GENERIC_SEEKABLE stream ended short; padding suppressed bytes=" +
                                   std::to_string(sentBody) +
                                   " expected=" + std::to_string(virtual_body_length) +
                                   " elapsedMs=" + std::to_string(elapsedMs) +
                                   " expectedPlaybackMs=" + std::to_string(remainingMs));
                        }
                    }

                    CloseHandle(streamPipe);
                    if (sendFailed || !running_ || virtualBodyComplete ||
                        aiStreamSuperseded()) {
                        std::lock_guard<std::mutex> controlLock(resident->control_mutex);
                        const std::string cancel =
                            "CANCEL " + std::to_string(sequence) + "\n";
                        DWORD ignored = 0;
                        WriteFile(resident->stdin_write, cancel.data(),
                                  static_cast<DWORD>(cancel.size()), &ignored, nullptr);
                    }

                    AddLog("AI /media resident body sent=" + std::to_string(sentBody) +
                           (finite_ts_body
                               ? " expected=" + std::to_string(virtual_body_length)
                               : std::string{}) +
                           (sendFailed ? " (client disconnected/send failed)" : ""));
                    return;
                }
            }
        }
    }

    if (aiStreamSuperseded()) {
        return;
    }

    // Start the GPU worker BEFORE sending HTTP 200.  The previous build sent
    // 200 first and then initialized CUDA/ONNX/NVENC.  Some clients time out
    // when the response body remains empty during that initialization window.
    AiProcess process;
    const bool launched = LaunchAiProcess(
        worker, item.path, model, requested_start_ms,
        ai_backend_, cudaBackend ? 0 : directml_device_,
        requestOutputMode, process);
    if (!launched) {
        const std::string message =
            "AI Passthrough could not start worker. Win32=" +
            std::to_string(GetLastError());
        AddLog("AI Passthrough error: " + message);
        ReportAiError("FATAL", "unknown", requestOutputLabel(), "unknown", message);
        SendAll(client, HttpError(500, "AI Worker Start Failed"));
        return;
    }

    AddLog(std::string("AI Passthrough common stream path starting backend=") +
           (cudaBackend ? "CUDA" :
            (ai_backend_ == AiBackend::DirectML ? "DirectML" : "CPU")) +
           "; waiting for first output bytes before HTTP 200. Output=" +
           requestOutputLabel());
    if (requested_start_ms > 0) {
        AddLog("AI Passthrough seek request startMs=" + std::to_string(requested_start_ms));
    }

    std::atomic<int64_t> source_duration_ms{0};
    std::string workerErrorGpu = cudaBackend ? "NVIDIA CUDA" : std::string{};
    std::string workerErrorMode = AiOutputModeDisplayName(requestOutputMode);
    std::string workerErrorCodec;
    std::thread stderr_thread([this, handle = process.stderr_read, &source_duration_ms,
                               &workerErrorGpu, &workerErrorMode,
                               &workerErrorCodec, cudaBackend]() {
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
                if (!line.empty()) {
                    HandleWorkerDiagnosticLine(
                        line, workerErrorGpu, workerErrorMode, workerErrorCodec);
                    if (ShouldShowWorkerLog(line)) {
                        AddLog(std::string(cudaBackend ? "GPU: " : "DML: ") + line);
                    }
                }
                pending.erase(0, pos + 1);
            }
        }
        pending = SanitizeWorkerLog(pending);
        if (!pending.empty()) {
            HandleWorkerDiagnosticLine(
                pending, workerErrorGpu, workerErrorMode, workerErrorCodec);
            if (ShouldShowWorkerLog(pending)) {
                AddLog(std::string(cudaBackend ? "GPU: " : "DML: ") + pending);
            }
        }
    });

    std::vector<char> data(256 * 1024);
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
                AddLog(std::string(cudaBackend ? "GPU: CUDA" : "DML: DirectML") +
                       " pipeline startup still running elapsedSec=" + std::to_string(elapsed));
            }
        }
        if (elapsed >= 120) {
            startup_timeout = true;
            break;
        }
        Sleep(10);
    }

    if (startup_failed || startup_timeout || first_got == 0) {
        std::string message;
        if (startup_timeout) {
            message = "AI Passthrough startup failed: no output bytes after 120 seconds.";
        } else {
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(process.process, &exit_code);
            message =
                "AI Passthrough startup failed before first output byte. processExit=" +
                std::to_string(exit_code);
        }
        AddLog(message);
        TerminateAiProcess(process);
        WaitForSingleObject(process.process, 5000);
        if (stderr_thread.joinable()) stderr_thread.join();
        ReportAiError(
            "FATAL",
            workerErrorGpu.empty()
                ? AiBackendDisplayName(ai_backend_, directml_device_)
                : workerErrorGpu,
            workerErrorMode.empty()
                ? AiOutputModeDisplayName(requestOutputMode)
                : workerErrorMode,
            workerErrorCodec.empty() ? "unknown" : workerErrorCodec,
            message);
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
    headers << "HTTP/1.1 "
            << ((dlna_time_seek_request || legacy_r800zz_seek_request || virtual_byte_seek_request)
                ? "206 Partial Content" : "200 OK") << "\r\n"
            << "Content-Type: " << requestMimeType() << "\r\n"
            << "Accept-Ranges: "
            << ((virtual_finite_response || virtual_byte_seek_request) ? "bytes" : "none")
            << "\r\n"
            << "transferMode.dlna.org: Streaming\r\n"
            << "contentFeatures.dlna.org: " << DlnaContentFeatures() << "\r\n";
    if (finite_ts_body) {
        headers << "Content-Length: " << virtual_body_length << "\r\n";
    }
    if (virtual_byte_seek_request) {
        headers << "Content-Range: bytes " << virtual_range_start << '-'
                << virtual_range_end << '/' << virtual_total_bytes << "\r\n";
    }
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
    bool virtual_body_complete = false;
    std::vector<char> tail_probe_body;
    if (virtual_tail_duration_probe) {
        tail_probe_body.reserve(static_cast<size_t>(virtual_body_length));
    }

    uint64_t prefix_discard_remaining = worker_prefix_discard_bytes;
    auto send_body_chunk = [&](const char* bytes, size_t count) -> bool {
        if (count == 0) return true;
        if (prefix_discard_remaining > 0) {
            const size_t discard = static_cast<size_t>(
                std::min<uint64_t>(
                    prefix_discard_remaining,
                    static_cast<uint64_t>(count)));
            bytes += discard;
            count -= discard;
            prefix_discard_remaining -= static_cast<uint64_t>(discard);
            if (count == 0) return true;
        }
        if (virtual_tail_duration_probe) {
            KeepTailBytes(tail_probe_body, bytes, count,
                          static_cast<size_t>(virtual_body_length));
            return true;
        }
        size_t send_size = count;
        if (finite_ts_body) {
            if (sent_body >= virtual_body_length) {
                virtual_body_complete = true;
                return true;
            }
            send_size = static_cast<size_t>(std::min<uint64_t>(
                static_cast<uint64_t>(count),
                virtual_body_length - sent_body));
        }
        if (send_size > 0 && !SendAll(client, bytes, send_size)) return false;
        sent_body += static_cast<uint64_t>(send_size);
        if (finite_ts_body && sent_body >= virtual_body_length) {
            virtual_body_complete = true;
        }
        return true;
    };

    if (!send_body_chunk(data.data(), static_cast<size_t>(first_got))) {
        send_failed = true;
    }

    while (!send_failed && !virtual_body_complete &&
           running_ && !aiStreamSuperseded()) {
        DWORD got = 0;
        const auto readStart = std::chrono::steady_clock::now();
        const BOOL ok = ReadFile(process.stdout_read, data.data(),
                                 static_cast<DWORD>(data.size()), &got, nullptr);
        const auto readMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - readStart).count();
        // Keep pipe timing off the normal UI log.
        (void)readMs;
        if (!ok || got == 0) break;

        const auto sendStart = std::chrono::steady_clock::now();
        if (!send_body_chunk(data.data(), static_cast<size_t>(got))) {
            send_failed = true;
            break;
        }
        const auto sendMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - sendStart).count();
        // Keep send timing off the normal UI log.
        (void)sendMs;
    }

    if (!send_failed && virtual_tail_duration_probe) {
        if (tail_probe_body.size() == virtual_body_length) {
            StoreGenericTailProbeCache(
                generic_tail_probe_cache_key, tail_probe_body);
            if (!SendAll(client, tail_probe_body.data(), tail_probe_body.size())) {
                send_failed = true;
            } else {
                sent_body = static_cast<uint64_t>(tail_probe_body.size());
                virtual_body_complete = true;
                AddLog("GENERIC_SEEKABLE tail PCR probe returned actual EOF tail bytes=" +
                       std::to_string(sent_body));
            }
        } else {
            AddLog("GENERIC_SEEKABLE tail PCR probe ended short bytes=" +
                   std::to_string(tail_probe_body.size()) +
                   " expected=" + std::to_string(virtual_body_length));
        }
    }

    if (!send_failed && !virtual_tail_duration_probe &&
        finite_ts_body && sent_body < virtual_body_length) {
        const int64_t remaining_ms = std::max<int64_t>(
            0, item.duration_ms - requested_start_ms);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startup_begin).count();
        const bool natural_eof_likely =
            remaining_ms <= 0 || elapsed_ms + 2000 >= remaining_ms;
        const uint64_t missing = virtual_body_length - sent_body;
        if (natural_eof_likely &&
            (missing % kMpegTsPacketSize) == 0) {
            AddLog("GENERIC_SEEKABLE natural EOF padding MPEG-TS bytes=" +
                   std::to_string(missing) +
                   " elapsedMs=" + std::to_string(elapsed_ms) +
                   " expectedPlaybackMs=" + std::to_string(remaining_ms));
            if (SendMpegTsNullPadding(client, missing)) {
                sent_body = virtual_body_length;
                virtual_body_complete = true;
            } else {
                send_failed = true;
            }
        } else {
            AddLog("GENERIC_SEEKABLE stream ended short; padding suppressed bytes=" +
                   std::to_string(sent_body) +
                   " expected=" + std::to_string(virtual_body_length) +
                   " elapsedMs=" + std::to_string(elapsed_ms) +
                   " expectedPlaybackMs=" + std::to_string(remaining_ms));
        }
    }

    if (send_failed || !running_ || virtual_body_complete ||
        aiStreamSuperseded()) {
        TerminateAiProcess(process);
    }

    WaitForSingleObject(process.process, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.process, &exit_code);

    if (stderr_thread.joinable()) stderr_thread.join();

    AddLog("AI /media body sent=" + std::to_string(sent_body) +
           (finite_ts_body
               ? " expected=" + std::to_string(virtual_body_length)
               : std::string{}) +
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
    if (ai_passthrough_.load(std::memory_order_relaxed) && item.is_video) {
        AiOutputMode selectedMode;
        {
            std::lock_guard<std::mutex> lock(ai_mode_mutex_);
            selectedMode = ai_output_mode_;
        }
        return selectedMode == AiOutputMode::WebmVp9Alpha
            ? "video/webm" : "video/mp2t";
    }
    const std::string ext = ToLower(item.path.extension().string());
    if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
    if (ext == ".webm") return "video/webm";
    if (ext == ".mkv") return "video/x-matroska";
    if (ext == ".avi") return "video/x-msvideo";
    if (ext == ".mov") return "video/quicktime";
    if (ext == ".ts" || ext == ".m2ts") return "video/mp2t";
    if (ext == ".vrm" || ext == ".glb") return "model/gltf-binary";
    if (ext == ".gltf") return "model/gltf+json";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    return "application/octet-stream";
}

std::string DlnaServer::AiOutputLabel() const {
    AiOutputMode selectedMode;
    {
        std::lock_guard<std::mutex> lock(ai_mode_mutex_);
        selectedMode = ai_output_mode_;
    }
    if (selectedMode == AiOutputMode::WebmVp9Alpha) {
        return "Alpha: WebM VP9 Alpha/libvpx (may drop frames)";
    }
    if (selectedMode == AiOutputMode::ChromaKeyHevc) {
        return "Chroma Key: Green background HEVC/NVENC";
    }
    return "Alpha: AlphaPacked HEVC/NVENC";
}

std::string DlnaServer::DlnaContentFeatures() const {
    if (ai_passthrough_.load(std::memory_order_relaxed)) {
        if (g_r800zz_vrplayer_request) {
            // Preserve the existing r800zzvrplayer contract: DLNA time seek only.
            return "DLNA.ORG_OP=10;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01700000000000000000000000000000";
        }
        // Generic clients get the experimental virtual HTTP byte-range capability.
        return "DLNA.ORG_OP=11;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01700000000000000000000000000000";
    }
    return "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";
}

std::string DlnaServer::DlnaProtocolInfo(const MediaItem* item) const {
    if (item && !item->is_video) {
        return "http-get:*:" + MimeType(*item) +
            ":DLNA.ORG_OP=01;DLNA.ORG_CI=0;"
            "DLNA.ORG_FLAGS=01700000000000000000000000000000";
    }
    if (item || ai_passthrough_.load(std::memory_order_relaxed)) {
        AiOutputMode selectedMode;
        {
            std::lock_guard<std::mutex> lock(ai_mode_mutex_);
            selectedMode = ai_output_mode_;
        }
        const std::string mime = item
            ? MimeType(*item)
            : (selectedMode == AiOutputMode::WebmVp9Alpha
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

const DlnaServer::DirectoryItem* DlnaServer::FindDirectoryItemByObjectId(
    const std::string& object_id) const {
    const auto found = std::find_if(
        directory_items_.begin(), directory_items_.end(),
        [&](const DirectoryItem& item) { return item.id == object_id; });
    return found == directory_items_.end() ? nullptr : &*found;
}

size_t DlnaServer::DirectChildCount(const std::string& parent_id) const {
    const size_t directoryCount = static_cast<size_t>(std::count_if(
        directory_items_.begin(), directory_items_.end(),
        [&](const DirectoryItem& item) { return item.parent_id == parent_id; }));
    const size_t fileCount = static_cast<size_t>(std::count_if(
        media_items_.begin(), media_items_.end(),
        [&](const MediaItem& item) { return item.parent_id == parent_id; }));
    return directoryCount + fileCount;
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

    auto appendContainer = [this](std::ostringstream& d,
                                  const DirectoryItem& directory) {
        d << "<container id=\"" << XmlEscape(directory.id)
          << "\" parentID=\"" << XmlEscape(directory.parent_id)
          << "\" restricted=\"1\" searchable=\"0\" childCount=\""
          << DirectChildCount(directory.id) << "\">"
          << "<dc:title>"
          << XmlEscape(WideToUtf8(directory.path.filename().wstring()))
          << "</dc:title>"
          << "<upnp:class>object.container.storageFolder</upnp:class>"
          << "</container>";
    };

    auto appendItem = [this](std::ostringstream& d, const MediaItem& item) {
        d << "<item id=\"" << item.id
          << "\" parentID=\"" << XmlEscape(item.parent_id)
          << "\" restricted=\"1\">"
          << "<dc:title>" << XmlEscape(DisplayTitle(item)) << "</dc:title>"
          << "<upnp:class>"
          << (item.is_video ? "object.item.videoItem.movie" : "object.item")
          << "</upnp:class>"
          << "<res protocolInfo=\"" << XmlEscape(DlnaProtocolInfo(&item)) << "\"";
        // For AI passthrough, generic fixed-muxrate MPEG-TS uses the same
        // virtual size model as HEAD/GET/Range. r800zzvrplayer keeps its
        // established stream-only metadata.
        uint64_t advertisedSize = item.size;
        AiOutputMode selectedMode;
        {
            std::lock_guard<std::mutex> lock(ai_mode_mutex_);
            selectedMode = ai_output_mode_;
        }
        if (ai_passthrough_.load(std::memory_order_relaxed) && item.is_video &&
            selectedMode != AiOutputMode::WebmVp9Alpha) {
            advertisedSize = RealtimeTsVirtualSizeBytes(item.duration_ms);
        }
        if (advertisedSize > 0 &&
            (!ai_passthrough_.load(std::memory_order_relaxed) ||
             !item.is_video || !g_r800zz_vrplayer_request)) {
            d << " size=\"" << advertisedSize << "\"";
        }
        if (item.duration_ms > 0) {
            d << " duration=\"" << FormatDlnaDuration(item.duration_ms) << "\"";
        }
        d << ">" << XmlEscape(MediaUrl(item)) << "</res></item>";
    };

    auto pathNameLess = [](const std::filesystem::path& left,
                           const std::filesystem::path& right) {
        std::wstring a = left.filename().wstring();
        std::wstring b = right.filename().wstring();
        std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c));
        });
        return a < b;
    };

    std::ostringstream didl;
    didl << didlStart;
    size_t numberReturned = 0;
    size_t totalMatches = 0;

    if (metadata && object_id == "0") {
        didl << "<container id=\"0\" parentID=\"-1\" restricted=\"1\" "
             << "searchable=\"0\" childCount=\"" << DirectChildCount("0") << "\">"
             << "<dc:title>r800zzXRdlnaServer</dc:title>"
             << "<upnp:class>object.container</upnp:class></container>";
        numberReturned = 1;
        totalMatches = 1;
    } else if (metadata) {
        if (const DirectoryItem* directory =
                FindDirectoryItemByObjectId(object_id)) {
            appendContainer(didl, *directory);
            numberReturned = 1;
            totalMatches = 1;
        } else if (const MediaItem* item =
                       FindMediaItemByObjectId(object_id)) {
            appendItem(didl, *item);
            numberReturned = 1;
            totalMatches = 1;
        }
    } else if (response_action == "Search") {
        // Preserve the previous Search behavior: Search exposes the complete
        // selected file set even though BrowseDirectChildren is hierarchical.
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
    } else {
        std::string parentId;
        if (object_id == "0") {
            parentId = "0";
        } else if (const DirectoryItem* directory =
                       FindDirectoryItemByObjectId(object_id)) {
            parentId = directory->id;
        }

        if (!parentId.empty()) {
            std::vector<const DirectoryItem*> directories;
            std::vector<const MediaItem*> files;
            for (const auto& directory : directory_items_) {
                if (directory.parent_id == parentId) {
                    directories.push_back(&directory);
                }
            }
            for (const auto& item : media_items_) {
                if (item.parent_id == parentId) {
                    files.push_back(&item);
                }
            }

            std::sort(directories.begin(), directories.end(),
                      [&](const DirectoryItem* left, const DirectoryItem* right) {
                          return pathNameLess(left->path, right->path);
                      });
            std::sort(files.begin(), files.end(),
                      [&](const MediaItem* left, const MediaItem* right) {
                          return pathNameLess(left->path, right->path);
                      });

            totalMatches = directories.size() + files.size();
            const size_t begin = std::min(starting_index, totalMatches);
            size_t end = totalMatches;
            if (requested_count > 0) {
                end = std::min(end, begin + requested_count);
            }

            for (size_t index = begin; index < end; ++index) {
                if (index < directories.size()) {
                    appendContainer(didl, *directories[index]);
                } else {
                    appendItem(didl, *files[index - directories.size()]);
                }
                ++numberReturned;
            }
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

