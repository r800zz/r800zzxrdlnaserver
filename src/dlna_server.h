#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
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

    // AI worker lifetime follows the selected AI Passthrough mode, not an HTTP request.
    // Backend/device are supplied here because prewarm can start before DLNA Server Start().
    bool SetAiPrewarm(bool enabled, const std::filesystem::path& media_file,
                      AiBackend ai_backend, int directml_device);
    bool IsAiPrewarmReady() const;

    bool IsRunning() const { return running_.load(); }
    uint16_t Port() const { return http_port_; }
    std::string AdvertisedIp() const;
    std::string BaseUrl() const;
    std::string Status() const;
    std::vector<std::string> Logs() const;
    void ClearLogs();
    void RecordAiCapabilityResult(const AiCapabilityResult& result);

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
    bool ai_passthrough_{false};
    AiOutputMode ai_output_mode_{AiOutputMode::AlphaPackedHevc};
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
};

