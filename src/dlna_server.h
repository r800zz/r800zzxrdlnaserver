#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <winsock2.h>

enum class AiOutputMode {
    AlphaPackedHevc = 0,
    WebmVp9Alpha = 1,
    ChromaKeyHevc = 2,
};

enum class AiBackend {
    NvidiaCuda = 0,
    DirectML = 1,
    Cpu = 2,
};

struct AiCapabilityResult {
    bool available{false};
    std::string message;
};

struct AiErrorInfo {
    std::string severity;
    std::string gpu;
    std::string mode;
    std::string codec;
    std::string message;
};

class DlnaServer {
public:
    DlnaServer();
    ~DlnaServer();

    DlnaServer(const DlnaServer&) = delete;
    DlnaServer& operator=(const DlnaServer&) = delete;

    static std::vector<std::string> GetLocalIPv4Addresses();
    static std::vector<std::filesystem::path> FindMediaFiles(
        const std::filesystem::path& media_directory);
    static std::vector<std::filesystem::path> FindFiles(
        const std::filesystem::path& media_directory, bool video_files_only);
    static bool IsMediaFile(const std::filesystem::path& media_file);
    static bool IsShareableFile(const std::filesystem::path& file,
                                bool video_files_only);
    static AiCapabilityResult ProbeAiCapability();

    bool Start(const std::vector<std::filesystem::path>& media_files,
               const std::string& advertised_ip,
               bool ai_passthrough, AiOutputMode ai_output_mode,
               AiBackend ai_backend = AiBackend::NvidiaCuda,
               int directml_device = 0,
               bool video_files_only = true);
    void Stop();

    // AI worker initialization starts from DLNA Server Start() when AI
    // Passthrough is enabled. Backend/device changes while running can reuse
    // this helper, but merely selecting UI options must not initialize a GPU.
    bool SetAiPrewarm(bool enabled, const std::filesystem::path& media_file,
                      AiBackend ai_backend, int directml_device);
    bool IsAiPrewarmReady() const;
    // While the server is running, AI ON/OFF and output-mode selection are
    // pending only. The active stream keeps its current state until the next
    // file change or explicit zero seek starts a new RUN.
    void SetAiPassthroughMode(bool enabled, AiOutputMode mode);
    void SetAiOutputMode(AiOutputMode mode);

    bool IsRunning() const { return running_.load(); }
    uint16_t Port() const { return http_port_; }
    std::string AdvertisedIp() const;
    std::string BaseUrl() const;
    std::string Status() const;
    std::vector<std::string> Logs() const;
    void ClearLogs();
    void RecordAiCapabilityResult(const AiCapabilityResult& result);
    void SetAiErrorCallback(std::function<void(const AiErrorInfo&)> callback);

private:
    bool SetupHttpListener();
    bool SetupSsdpSocket();
    void HttpLoop();
    void SsdpLoop();
    void HandleHttpClient(SOCKET client);
    void FinishHttpClient(SOCKET client);
    struct MediaItem {
        uint32_t id{0};
        std::filesystem::path path;
        uint64_t size{0};
        int64_t duration_ms{0};
        int width{0};
        int height{0};
        bool is_video{false};
        std::string parent_id{"0"};
    };

    struct DirectoryItem {
        std::string id;
        std::string parent_id{"0"};
        std::filesystem::path path;
    };

    // Common DLNA/HTTP/pipe streaming path. CUDA/DirectML/CPU differ only in
    // worker/model/backend selection; the response and transport behavior is shared.
    void HandleAiPassthroughStream(SOCKET client, const std::string& method,
                                   const std::string& request, const MediaItem& item);

    // Compatibility wrapper; forwards to HandleAiPassthroughStream().
    void HandleDirectMlPassthroughStream(SOCKET client, const std::string& method,
                                         const std::string& request, const MediaItem& item);

    void SendSsdpResponse(const sockaddr_in& destination, const std::string& st);
    void SendAliveNotifications();
    void SendByebyeNotifications();
    std::vector<std::pair<std::string, std::string>> SsdpTargets() const;

    void AddLog(const std::string& text);
    void SetStatus(const std::string& text);
    void HandleWorkerDiagnosticLine(const std::string& line,
                                    std::string& gpu,
                                    std::string& mode,
                                    std::string& codec);
    void ReportAiError(const std::string& severity,
                       const std::string& gpu,
                       const std::string& mode,
                       const std::string& codec,
                       const std::string& message);

    std::string DeviceDescriptionXml() const;
    std::string ContentDirectoryScpdXml() const;
    std::string ConnectionManagerScpdXml() const;
    std::string BrowseSoapResponse(const std::string& object_id, bool metadata,
                                   size_t starting_index, size_t requested_count,
                                   const std::string& response_action = "Browse") const;
    std::string SimpleSoapResponse(const std::string& service,
                                   const std::string& action,
                                   const std::string& inner_xml) const;
    std::string MediaUrl(const MediaItem& item) const;
    std::string MimeType(const MediaItem& item) const;
    std::string AiOutputLabel() const;
    std::string DlnaProtocolInfo(const MediaItem* item = nullptr) const;
    std::string DlnaContentFeatures() const;
    std::string DisplayTitle(const MediaItem& item) const;
    const MediaItem* FindMediaItem(const std::string& request_path) const;
    const MediaItem* FindMediaItemByObjectId(const std::string& object_id) const;
    const DirectoryItem* FindDirectoryItemByObjectId(
        const std::string& object_id) const;
    size_t DirectChildCount(const std::string& parent_id) const;
    static bool ProbeMedia(const std::filesystem::path& path, int64_t& duration_ms,
                           int& width, int& height);

    struct ResidentAiState;
    bool StartResidentAiWorker(const std::filesystem::path& media_file);
    void StopResidentAiWorker();

    std::vector<MediaItem> media_items_;
    std::vector<DirectoryItem> directory_items_;
    std::string advertised_ip_;
    std::string uuid_;
    // ai_passthrough_ is the state used for new media requests. The GUI can
    // queue a different ON/OFF state without mutating an in-flight stream.
    std::atomic<bool> ai_passthrough_{false};
    mutable std::mutex ai_mode_mutex_;
    bool selected_ai_passthrough_{false};
    bool ai_passthrough_change_pending_{false};
    // ai_output_mode_ is the UI-selected mode. active_ai_output_mode_ is the
    // mode used by the current RUN.
    AiOutputMode ai_output_mode_{AiOutputMode::AlphaPackedHevc};
    AiOutputMode active_ai_output_mode_{AiOutputMode::AlphaPackedHevc};
    bool ai_output_mode_change_pending_{false};
    uint32_t active_ai_media_id_{0};
    AiBackend ai_backend_{AiBackend::NvidiaCuda};
    int directml_device_{0};
    bool video_files_only_{true};

    std::atomic<bool> running_{false};
    SOCKET http_listen_socket_{INVALID_SOCKET};
    SOCKET ssdp_socket_{INVALID_SOCKET};
    uint16_t http_port_{0};

    std::thread http_thread_;
    std::thread ssdp_thread_;

    std::mutex http_clients_mutex_;
    std::condition_variable http_clients_cv_;
    std::vector<SOCKET> active_http_clients_;

    mutable std::mutex resident_ai_mutex_;
    std::unique_ptr<ResidentAiState> resident_ai_;

    mutable std::mutex state_mutex_;
    std::string status_;
    std::vector<std::string> logs_;

    mutable std::mutex ai_error_callback_mutex_;
    std::function<void(const AiErrorInfo&)> ai_error_callback_;
};

