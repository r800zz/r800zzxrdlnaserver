#pragma once

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "ws2_32.lib")

class WebXrHttpsServer {
public:
    WebXrHttpsServer() = default;
    ~WebXrHttpsServer() { Stop(); }

    WebXrHttpsServer(const WebXrHttpsServer&) = delete;
    WebXrHttpsServer& operator=(const WebXrHttpsServer&) = delete;

    bool Start(const std::string& advertisedIp) {
        Stop();
        if (!IsIpv4Address(advertisedIp)) {
            SetStatus("Invalid LAN IPv4 address.");
            return false;
        }

        root_directory_ = ExecutableDirectory() / L"webxr";
        std::error_code ec;
        std::filesystem::create_directories(root_directory_, ec);
        if (ec) {
            SetStatus("Could not create WebXR folder: " + ec.message());
            return false;
        }

        advertised_ip_ = advertisedIp;
        std::string certificateError;
        if (!EnsureServerCertificate(certificateError)) {
            SetStatus("HTTPS certificate error: " + certificateError);
            CleanupTlsCredentials();
            return false;
        }

        SCHANNEL_CRED credentials{};
        credentials.dwVersion = SCHANNEL_CRED_VERSION;
        credentials.cCreds = 1;
        credentials.paCred = &server_certificate_;
        credentials.dwFlags = SCH_CRED_NO_DEFAULT_CREDS;
        // TLS 1.2 is supported by current Chromium-based HMD browsers and
        // avoids TLS 1.3 post-handshake messages, keeping this small Schannel
        // server deliberately simple.
        credentials.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;

        TimeStamp expiry{};
        static wchar_t schannelProvider[] =
            L"Microsoft Unified Security Protocol Provider";
        const SECURITY_STATUS acquire = AcquireCredentialsHandleW(
            nullptr,
            schannelProvider,
            SECPKG_CRED_INBOUND,
            nullptr,
            &credentials,
            nullptr,
            nullptr,
            &credential_handle_,
            &expiry);
        if (acquire != SEC_E_OK) {
            SetStatus("Schannel AcquireCredentialsHandle failed: " +
                      SecurityStatusHex(acquire));
            CleanupTlsCredentials();
            return false;
        }
        credential_acquired_ = true;

        WSADATA wsa{};
        const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (wsaResult != 0) {
            SetStatus("WSAStartup failed: " + std::to_string(wsaResult));
            CleanupTlsCredentials();
            return false;
        }
        wsa_started_ = true;

        SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            SetStatus("WebXR socket() failed: " + std::to_string(WSAGetLastError()));
            Stop();
            return false;
        }

        BOOL reuse = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        if (inet_pton(AF_INET, advertisedIp.c_str(), &address.sin_addr) != 1) {
            closesocket(listener);
            SetStatus("Could not parse WebXR IPv4 address.");
            Stop();
            return false;
        }

        constexpr uint16_t kFirstPort = 8443;
        constexpr uint16_t kLastPort = 8463;
        bool bound = false;
        for (uint16_t port = kFirstPort; port <= kLastPort; ++port) {
            address.sin_port = htons(port);
            if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0) {
                https_port_ = port;
                bound = true;
                break;
            }
        }
        if (!bound) {
            const int error = WSAGetLastError();
            closesocket(listener);
            SetStatus("No free WebXR HTTPS port in 8443-8463. WinSock=" +
                      std::to_string(error));
            Stop();
            return false;
        }

        if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            closesocket(listener);
            SetStatus("WebXR listen() failed: " + std::to_string(error));
            Stop();
            return false;
        }

        listen_socket_ = listener;
        running_.store(true);
        SetStatus("Running");
        accept_thread_ = std::thread([this]() { AcceptLoop(); });
        return true;
    }

    void Stop() {
        const bool wasRunning = running_.exchange(false);
        SOCKET listener = listen_socket_.exchange(INVALID_SOCKET);
        if (listener != INVALID_SOCKET) {
            shutdown(listener, SD_BOTH);
            closesocket(listener);
        }

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (SOCKET client : active_clients_) {
                shutdown(client, SD_BOTH);
            }
        }

        if (accept_thread_.joinable()) accept_thread_.join();

        {
            std::unique_lock<std::mutex> lock(clients_mutex_);
            clients_cv_.wait(lock, [this]() { return active_clients_.empty(); });
        }

        CleanupTlsCredentials();
        if (wsa_started_) {
            WSACleanup();
            wsa_started_ = false;
        }
        https_port_ = 0;
        if (wasRunning) SetStatus("Stopped");
    }

    bool IsRunning() const { return running_.load(); }
    uint16_t Port() const { return https_port_; }

    std::string BaseUrl() const {
        if (!IsRunning() || advertised_ip_.empty() || https_port_ == 0) return {};
        return "https://" + advertised_ip_ + ":" +
               std::to_string(https_port_) + "/";
    }

    std::filesystem::path RootDirectory() const {
        if (!root_directory_.empty()) return root_directory_;
        return ExecutableDirectory() / L"webxr";
    }

    std::filesystem::path RootCertificateFile() const {
        if (!root_certificate_file_.empty()) return root_certificate_file_;
        return RootDirectory() / L"r800zz-webxr-root-ca.cer";
    }

    std::string Status() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return status_;
    }

private:
    struct SecurityContextGuard {
        CtxtHandle handle{};
        bool valid{false};
        ~SecurityContextGuard() {
            if (valid) DeleteSecurityContext(&handle);
        }
    };

    static std::filesystem::path ExecutableDirectory() {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) return {};
        return std::filesystem::path(
            std::wstring(buffer.data(), length)).parent_path();
    }

    static std::wstring Utf8ToWide(const std::string& value) {
        if (value.empty()) return {};
        const int size = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0) return {};
        std::wstring out(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), out.data(), size);
        return out;
    }

    static std::string WideToUtf8(const std::wstring& value) {
        if (value.empty()) return {};
        const int size = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string out(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            out.data(), size, nullptr, nullptr);
        return out;
    }

    static bool IsIpv4Address(const std::string& value) {
        in_addr address{};
        return inet_pton(AF_INET, value.c_str(), &address) == 1;
    }

    static std::wstring QuoteWindowsArgument(const std::wstring& value) {
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

    static std::string SecurityStatusHex(SECURITY_STATUS status) {
        std::ostringstream out;
        out << "0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(status);
        return out.str();
    }

    static bool SendRaw(SOCKET socket, const void* data, size_t size) {
        const char* bytes = static_cast<const char*>(data);
        size_t sent = 0;
        while (sent < size) {
            const int chunk = send(
                socket, bytes + sent,
                static_cast<int>(std::min<size_t>(size - sent, 1u << 20)), 0);
            if (chunk <= 0) return false;
            sent += static_cast<size_t>(chunk);
        }
        return true;
    }

    static std::wstring CertificateCommonName(PCCERT_CONTEXT certificate) {
        if (!certificate) return {};
        const DWORD needed = CertGetNameStringW(
            certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
        if (needed <= 1) return {};
        std::wstring value(static_cast<size_t>(needed), L'\0');
        CertGetNameStringW(
            certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
            value.data(), needed);
        value.resize(static_cast<size_t>(needed - 1));
        return value;
    }

    static bool CertificateHasMinimumRemainingYears(
            PCCERT_CONTEXT certificate, uint64_t minimumYears) {
        if (!certificate || !certificate->pCertInfo) return false;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER nowValue{};
        nowValue.LowPart = now.dwLowDateTime;
        nowValue.HighPart = now.dwHighDateTime;
        ULARGE_INTEGER notAfter{};
        notAfter.LowPart = certificate->pCertInfo->NotAfter.dwLowDateTime;
        notAfter.HighPart = certificate->pCertInfo->NotAfter.dwHighDateTime;
        constexpr uint64_t k100nsPerDay = 864000000000ULL;
        const uint64_t minimumRemaining =
            minimumYears * 365ULL * k100nsPerDay;
        return notAfter.QuadPart > nowValue.QuadPart &&
               notAfter.QuadPart - nowValue.QuadPart >= minimumRemaining;
    }

    static PCCERT_CONTEXT FindValidCertificateByCommonName(
            HCERTSTORE store, const std::wstring& commonName,
            uint64_t minimumRemainingYears = 0) {
        if (!store || commonName.empty()) return nullptr;
        PCCERT_CONTEXT cursor = nullptr;
        while ((cursor = CertFindCertificateInStore(
                    store,
                    X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                    0,
                    CERT_FIND_SUBJECT_STR_W,
                    commonName.c_str(),
                    cursor)) != nullptr) {
            if (_wcsicmp(CertificateCommonName(cursor).c_str(),
                         commonName.c_str()) != 0) {
                continue;
            }
            if (CertVerifyTimeValidity(nullptr, cursor->pCertInfo) != 0) continue;
            if (minimumRemainingYears > 0 &&
                !CertificateHasMinimumRemainingYears(
                    cursor, minimumRemainingYears)) {
                continue;
            }
            HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key = 0;
            DWORD keySpec = 0;
            BOOL callerFree = FALSE;
            if (!CryptAcquireCertificatePrivateKey(
                    cursor,
                    CRYPT_ACQUIRE_SILENT_FLAG | CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG,
                    nullptr, &key, &keySpec, &callerFree)) {
                continue;
            }
            if (callerFree) NCryptFreeObject(static_cast<NCRYPT_HANDLE>(key));
            PCCERT_CONTEXT result = CertDuplicateCertificateContext(cursor);
            CertFreeCertificateContext(cursor);
            return result;
        }
        return nullptr;
    }

    static bool RunPowerShell(const std::wstring& script, std::string& error) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE readPipe = nullptr;
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
            error = "CreatePipe failed: " + std::to_string(GetLastError());
            return false;
        }
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;

        std::wstring command =
            L"powershell.exe -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -Command " +
            QuoteWindowsArgument(script);
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        PROCESS_INFORMATION process{};
        const BOOL launched = CreateProcessW(
            nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(writePipe);
        writePipe = nullptr;
        if (!launched) {
            CloseHandle(readPipe);
            error = "Could not start PowerShell: " + std::to_string(GetLastError());
            return false;
        }
        CloseHandle(process.hThread);

        std::string output;
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(readPipe, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &got, nullptr) ||
                got == 0) {
                break;
            }
            output.append(buffer.data(), got);
            if (output.size() > 32768) {
                output.erase(0, output.size() - 32768);
            }
        }
        CloseHandle(readPipe);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);

        if (exitCode != 0) {
            error = "PowerShell certificate creation failed. Exit=" +
                    std::to_string(exitCode);
            if (!output.empty()) error += " " + output;
            return false;
        }
        return true;
    }

    bool EnsureServerCertificate(std::string& error) {
        const std::wstring rootCommonName = L"R800ZZ WebXR Local CA";
        const std::wstring leafCommonName =
            L"R800ZZ WebXR " + Utf8ToWide(advertised_ip_);

        HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
        if (!store) {
            error = "Could not open CurrentUser\\MY certificate store. Win32=" +
                    std::to_string(GetLastError());
            return false;
        }

        // Existing 10-year/5-year certificates from older builds are replaced
        // once. Long-lived certificates are then reused for decades.
        constexpr uint64_t kMinimumLongCertificateRemainingYears = 20;
        PCCERT_CONTEXT root = FindValidCertificateByCommonName(
            store, rootCommonName, kMinimumLongCertificateRemainingYears);
        PCCERT_CONTEXT leaf = FindValidCertificateByCommonName(
            store, leafCommonName, kMinimumLongCertificateRemainingYears);

        if (!root || !leaf) {
            if (root) CertFreeCertificateContext(root);
            if (leaf) CertFreeCertificateContext(leaf);
            CertCloseStore(store, 0);
            store = nullptr;

            const std::wstring ip = Utf8ToWide(advertised_ip_);
            // Build a normal two-certificate local PKI. The HMD only needs to
            // trust the exported root CA once; a separate server certificate is
            // generated for each LAN IPv4 address and contains that IP in SAN.
            std::wstring script =
                L"$ErrorActionPreference='Stop';"
                L"$rootName='R800ZZ WebXR Local CA';"
                L"$rootSubject='CN='+$rootName;"
                L"$root=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $rootSubject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddYears(20) } | Sort-Object NotAfter -Descending | Select-Object -First 1;"
                L"if(-not $root){$root=New-SelfSignedCertificate -Type Custom -Subject $rootSubject -CertStoreLocation 'Cert:\\CurrentUser\\My' -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -KeyUsage CertSign,CRLSign,DigitalSignature -NotAfter (Get-Date).AddYears(101) -TextExtension @('2.5.29.19={critical}{text}ca=1&pathlength=1')};"
                L"$leafName='R800ZZ WebXR " + ip + L"';"
                L"$leafSubject='CN='+$leafName;"
                L"$leaf=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq $leafSubject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddYears(20) } | Sort-Object NotAfter -Descending | Select-Object -First 1;"
                L"$leafNotAfter=(Get-Date).AddYears(100);if($leafNotAfter -ge $root.NotAfter){$leafNotAfter=$root.NotAfter.AddMinutes(-1)};"
                L"if(-not $leaf){$leaf=New-SelfSignedCertificate -Type Custom -Subject $leafSubject -Signer $root -CertStoreLocation 'Cert:\\CurrentUser\\My' -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -KeyUsage DigitalSignature,KeyEncipherment -NotAfter $leafNotAfter -TextExtension @('2.5.29.17={text}IPAddress=" + ip + L"','2.5.29.37={text}1.3.6.1.5.5.7.3.1')};";

            if (!RunPowerShell(script, error)) return false;

            store = CertOpenSystemStoreW(0, L"MY");
            if (!store) {
                error = "Could not reopen CurrentUser\\MY certificate store.";
                return false;
            }
            root = FindValidCertificateByCommonName(
                store, rootCommonName, kMinimumLongCertificateRemainingYears);
            leaf = FindValidCertificateByCommonName(
                store, leafCommonName, kMinimumLongCertificateRemainingYears);
        }

        if (!root || !leaf) {
            if (root) CertFreeCertificateContext(root);
            if (leaf) CertFreeCertificateContext(leaf);
            CertCloseStore(store, 0);
            error = "WebXR root or server certificate was not found after creation.";
            return false;
        }

        root_certificate_file_ =
            root_directory_ / L"r800zz-webxr-root-ca.cer";
        {
            std::ofstream output(root_certificate_file_, std::ios::binary | std::ios::trunc);
            if (!output) {
                CertFreeCertificateContext(root);
                CertFreeCertificateContext(leaf);
                CertCloseStore(store, 0);
                error = "Could not write root CA file: " +
                        WideToUtf8(root_certificate_file_.wstring());
                return false;
            }
            output.write(
                reinterpret_cast<const char*>(root->pbCertEncoded),
                static_cast<std::streamsize>(root->cbCertEncoded));
            if (!output) {
                CertFreeCertificateContext(root);
                CertFreeCertificateContext(leaf);
                CertCloseStore(store, 0);
                error = "Could not finish writing root CA file.";
                return false;
            }
        }

        CertFreeCertificateContext(root);
        if (server_certificate_) CertFreeCertificateContext(server_certificate_);
        server_certificate_ = CertDuplicateCertificateContext(leaf);
        CertFreeCertificateContext(leaf);
        CertCloseStore(store, 0);

        if (!server_certificate_) {
            error = "Could not duplicate WebXR server certificate.";
            return false;
        }
        return true;
    }

    void CleanupTlsCredentials() {
        if (credential_acquired_) {
            FreeCredentialsHandle(&credential_handle_);
            credential_acquired_ = false;
            SecInvalidateHandle(&credential_handle_);
        }
        if (server_certificate_) {
            CertFreeCertificateContext(server_certificate_);
            server_certificate_ = nullptr;
        }
    }

    void AcceptLoop() {
        while (running_.load()) {
            sockaddr_in peer{};
            int peerSize = sizeof(peer);
            SOCKET client = accept(
                listen_socket_.load(), reinterpret_cast<sockaddr*>(&peer), &peerSize);
            if (client == INVALID_SOCKET) {
                if (!running_.load()) break;
                Sleep(10);
                continue;
            }

            DWORD timeout = 15000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char*>(&timeout), sizeof(timeout));

            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                active_clients_.push_back(client);
            }
            std::thread([this, client]() {
                HandleClient(client);
                FinishClient(client);
            }).detach();
        }
    }

    void FinishClient(SOCKET client) {
        shutdown(client, SD_BOTH);
        closesocket(client);
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto it = std::find(active_clients_.begin(), active_clients_.end(), client);
            if (it != active_clients_.end()) active_clients_.erase(it);
        }
        clients_cv_.notify_all();
    }

    bool PerformHandshake(SOCKET client, SecurityContextGuard& context,
                          std::vector<char>& encryptedExtra) {
        std::vector<char> input(64 * 1024);
        size_t inputSize = 0;
        bool haveContext = false;

        for (;;) {
            if (inputSize == input.size()) return false;
            if (inputSize == 0) {
                const int got = recv(client, input.data(),
                                     static_cast<int>(input.size()), 0);
                if (got <= 0) return false;
                inputSize = static_cast<size_t>(got);
            }

            SecBuffer inBuffers[2]{};
            inBuffers[0].BufferType = SECBUFFER_TOKEN;
            inBuffers[0].pvBuffer = input.data();
            inBuffers[0].cbBuffer = static_cast<unsigned long>(inputSize);
            inBuffers[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc inDesc{};
            inDesc.ulVersion = SECBUFFER_VERSION;
            inDesc.cBuffers = 2;
            inDesc.pBuffers = inBuffers;

            SecBuffer outBuffer{};
            outBuffer.BufferType = SECBUFFER_TOKEN;
            SecBufferDesc outDesc{};
            outDesc.ulVersion = SECBUFFER_VERSION;
            outDesc.cBuffers = 1;
            outDesc.pBuffers = &outBuffer;

            ULONG attributes = 0;
            TimeStamp expiry{};
            SECURITY_STATUS status = AcceptSecurityContext(
                &credential_handle_,
                haveContext ? &context.handle : nullptr,
                &inDesc,
                ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                    ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                    ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM,
                SECURITY_NATIVE_DREP,
                &context.handle,
                &outDesc,
                &attributes,
                &expiry);

            if (status == SEC_I_COMPLETE_NEEDED ||
                status == SEC_I_COMPLETE_AND_CONTINUE) {
                if (CompleteAuthToken(&context.handle, &outDesc) != SEC_E_OK) {
                    if (outBuffer.pvBuffer) FreeContextBuffer(outBuffer.pvBuffer);
                    return false;
                }
                status = (status == SEC_I_COMPLETE_NEEDED)
                    ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
            }

            if (outBuffer.pvBuffer && outBuffer.cbBuffer > 0) {
                const bool sent = SendRaw(
                    client, outBuffer.pvBuffer, outBuffer.cbBuffer);
                FreeContextBuffer(outBuffer.pvBuffer);
                outBuffer.pvBuffer = nullptr;
                if (!sent) {
                    if (haveContext || status != SEC_E_INVALID_HANDLE) {
                        context.valid = true;
                    }
                    return false;
                }
            }

            if (!haveContext && status != SEC_E_INVALID_HANDLE &&
                status != SEC_E_INCOMPLETE_MESSAGE) {
                haveContext = true;
                context.valid = true;
            }

            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (inputSize == input.size()) return false;
                const int got = recv(
                    client, input.data() + inputSize,
                    static_cast<int>(input.size() - inputSize), 0);
                if (got <= 0) return false;
                inputSize += static_cast<size_t>(got);
                continue;
            }

            size_t extra = 0;
            if (inBuffers[1].BufferType == SECBUFFER_EXTRA) {
                extra = inBuffers[1].cbBuffer;
            }
            if (extra > inputSize) return false;

            if (status == SEC_E_OK) {
                context.valid = true;
                if (extra > 0) {
                    encryptedExtra.assign(
                        input.data() + (inputSize - extra),
                        input.data() + inputSize);
                }
                return true;
            }

            if (status != SEC_I_CONTINUE_NEEDED &&
                status != SEC_I_COMPLETE_AND_CONTINUE) {
                return false;
            }

            if (extra > 0) {
                memmove(input.data(), input.data() + (inputSize - extra), extra);
                inputSize = extra;
            } else {
                inputSize = 0;
            }
            if (inputSize == 0) {
                const int got = recv(client, input.data(),
                                     static_cast<int>(input.size()), 0);
                if (got <= 0) return false;
                inputSize = static_cast<size_t>(got);
            }
        }
    }

    bool ReceiveHttpRequest(SOCKET client, CtxtHandle& context,
                            std::vector<char> encrypted,
                            std::string& request) {
        request.clear();
        constexpr size_t kMaxRequest = 64 * 1024;

        for (;;) {
            if (request.find("\r\n\r\n") != std::string::npos) return true;
            if (request.size() >= kMaxRequest) return false;

            if (encrypted.empty()) {
                std::array<char, 16384> buffer{};
                const int got = recv(client, buffer.data(),
                                     static_cast<int>(buffer.size()), 0);
                if (got <= 0) return false;
                encrypted.assign(buffer.data(), buffer.data() + got);
            }

            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_DATA;
            buffers[0].pvBuffer = encrypted.data();
            buffers[0].cbBuffer = static_cast<unsigned long>(encrypted.size());
            buffers[1].BufferType = SECBUFFER_EMPTY;
            buffers[2].BufferType = SECBUFFER_EMPTY;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;

            const SECURITY_STATUS status = DecryptMessage(&context, &desc, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                std::array<char, 16384> more{};
                const int got = recv(client, more.data(),
                                     static_cast<int>(more.size()), 0);
                if (got <= 0) return false;
                if (encrypted.size() + static_cast<size_t>(got) > 256 * 1024) return false;
                encrypted.insert(encrypted.end(), more.data(), more.data() + got);
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) return false;
            if (status != SEC_E_OK) return false;

            std::vector<char> extra;
            for (SecBuffer& buffer : buffers) {
                if (buffer.BufferType == SECBUFFER_DATA &&
                    buffer.pvBuffer && buffer.cbBuffer > 0) {
                    request.append(
                        static_cast<const char*>(buffer.pvBuffer),
                        static_cast<size_t>(buffer.cbBuffer));
                } else if (buffer.BufferType == SECBUFFER_EXTRA &&
                           buffer.cbBuffer > 0) {
                    const size_t extraSize = buffer.cbBuffer;
                    if (extraSize > encrypted.size()) return false;
                    extra.assign(
                        encrypted.data() + (encrypted.size() - extraSize),
                        encrypted.data() + encrypted.size());
                }
            }
            encrypted.swap(extra);
        }
    }

    bool SendTlsData(SOCKET client, CtxtHandle& context,
                     const char* data, size_t size) {
        SecPkgContext_StreamSizes sizes{};
        if (QueryContextAttributes(
                &context, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
            return false;
        }

        size_t offset = 0;
        while (offset < size) {
            const size_t chunk = std::min<size_t>(
                size - offset, static_cast<size_t>(sizes.cbMaximumMessage));
            std::vector<char> packet(
                static_cast<size_t>(sizes.cbHeader) + chunk +
                static_cast<size_t>(sizes.cbTrailer));
            memcpy(packet.data() + sizes.cbHeader, data + offset, chunk);

            SecBuffer buffers[4]{};
            buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
            buffers[0].pvBuffer = packet.data();
            buffers[0].cbBuffer = sizes.cbHeader;
            buffers[1].BufferType = SECBUFFER_DATA;
            buffers[1].pvBuffer = packet.data() + sizes.cbHeader;
            buffers[1].cbBuffer = static_cast<unsigned long>(chunk);
            buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
            buffers[2].pvBuffer = packet.data() + sizes.cbHeader + chunk;
            buffers[2].cbBuffer = sizes.cbTrailer;
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc desc{};
            desc.ulVersion = SECBUFFER_VERSION;
            desc.cBuffers = 4;
            desc.pBuffers = buffers;

            if (EncryptMessage(&context, 0, &desc, 0) != SEC_E_OK) return false;
            for (int i = 0; i < 3; ++i) {
                if (buffers[i].cbBuffer > 0 &&
                    !SendRaw(client, buffers[i].pvBuffer, buffers[i].cbBuffer)) {
                    return false;
                }
            }
            offset += chunk;
        }
        return true;
    }

    bool SendTlsString(SOCKET client, CtxtHandle& context,
                       const std::string& text) {
        return SendTlsData(client, context, text.data(), text.size());
    }

    static std::string ToLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        return value;
    }

    static std::string UrlDecode(const std::string& value, bool& ok) {
        ok = true;
        std::string out;
        out.reserve(value.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%') {
                if (i + 2 >= value.size()) { ok = false; return {}; }
                const int hi = hex(value[i + 1]);
                const int lo = hex(value[i + 2]);
                if (hi < 0 || lo < 0) { ok = false; return {}; }
                const char decoded = static_cast<char>((hi << 4) | lo);
                if (decoded == '\0') { ok = false; return {}; }
                out.push_back(decoded);
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    static std::string MimeType(const std::filesystem::path& path) {
        const std::string ext = ToLower(path.extension().string());
        if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
        if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
        if (ext == ".css") return "text/css; charset=utf-8";
        if (ext == ".json") return "application/json; charset=utf-8";
        if (ext == ".wasm") return "application/wasm";
        if (ext == ".vrm" || ext == ".glb") return "model/gltf-binary";
        if (ext == ".gltf") return "model/gltf+json";
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".webp") return "image/webp";
        if (ext == ".svg") return "image/svg+xml";
        if (ext == ".ico") return "image/x-icon";
        if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
        if (ext == ".webm") return "video/webm";
        if (ext == ".cer" || ext == ".crt") return "application/pkix-cert";
        if (ext == ".txt") return "text/plain; charset=utf-8";
        return "application/octet-stream";
    }

    static bool ParseRange(const std::string& request, uint64_t total,
                           uint64_t& start, uint64_t& end) {
        const std::string lower = ToLower(request);
        const std::string marker = "\r\nrange:";
        const size_t position = lower.find(marker);
        if (position == std::string::npos || total == 0) return false;
        size_t valueStart = position + marker.size();
        while (valueStart < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[valueStart]))) {
            ++valueStart;
        }
        const size_t lineEnd = lower.find("\r\n", valueStart);
        if (lineEnd == std::string::npos) return false;
        std::string value = lower.substr(valueStart, lineEnd - valueStart);
        if (value.rfind("bytes=", 0) != 0) return false;
        value.erase(0, 6);
        const size_t dash = value.find('-');
        if (dash == std::string::npos) return false;
        try {
            if (dash == 0) {
                const uint64_t suffix = std::stoull(value.substr(1));
                if (suffix == 0) return false;
                start = suffix >= total ? 0 : total - suffix;
                end = total - 1;
            } else {
                start = std::stoull(value.substr(0, dash));
                end = dash + 1 < value.size()
                    ? std::stoull(value.substr(dash + 1))
                    : total - 1;
                if (start >= total) return false;
                if (end >= total) end = total - 1;
                if (end < start) return false;
            }
        } catch (...) {
            return false;
        }
        return true;
    }

    bool ResolvePath(const std::string& requestPath,
                     std::filesystem::path& file) const {
        std::string route = requestPath;
        const size_t query = route.find('?');
        if (query != std::string::npos) route.resize(query);
        const size_t fragment = route.find('#');
        if (fragment != std::string::npos) route.resize(fragment);
        if (route.empty() || route == "/") route = "/index.html";
        if (route.front() != '/') return false;

        bool decodedOk = false;
        const std::string decoded = UrlDecode(route.substr(1), decodedOk);
        if (!decodedOk || decoded.empty()) return false;
        if (decoded.find('\\') != std::string::npos ||
            decoded.find(':') != std::string::npos) {
            return false;
        }

        std::filesystem::path relative;
        std::stringstream parts(decoded);
        std::string segment;
        while (std::getline(parts, segment, '/')) {
            if (segment.empty() || segment == ".") continue;
            if (segment == "..") return false;
            const std::wstring wideSegment = Utf8ToWide(segment);
            if (wideSegment.empty()) return false;
            relative /= wideSegment;
        }
        if (relative.empty()) return false;

        std::error_code ec;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical(root_directory_, ec);
        if (ec) return false;
        file = std::filesystem::weakly_canonical(root_directory_ / relative, ec);
        if (ec) return false;

        auto lowerPath = [](std::wstring value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return value;
        };
        std::wstring rootText = lowerPath(canonicalRoot.wstring());
        std::wstring fileText = lowerPath(file.wstring());
        if (!rootText.empty() &&
            rootText.back() != L'\\' && rootText.back() != L'/') {
            rootText.push_back(std::filesystem::path::preferred_separator);
        }
        if (fileText.rfind(rootText, 0) != 0) return false;
        return std::filesystem::is_regular_file(file, ec) && !ec;
    }

    std::string MissingIndexPage() const {
        const std::string root = WideToUtf8(root_directory_.wstring());
        std::ostringstream html;
        html << "<!doctype html><html><head><meta charset=\"utf-8\">"
             << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             << "<title>r800zz WebXR HTTPS Server</title></head>"
             << "<body><h1>r800zz WebXR HTTPS Server</h1>"
             << "<p>HTTPS is running.</p>"
             << "<p>Place <code>index.html</code> in:</p><pre>" << HtmlEscape(root) << "</pre>"
             << "<p id=\"status\"></p>"
             << "<script>document.getElementById('status').textContent="
                "'Secure context: '+window.isSecureContext+' / WebXR API: '+('xr' in navigator);</script>"
             << "</body></html>";
        return html.str();
    }

    static std::string HtmlEscape(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 32);
        for (char c : value) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c); break;
            }
        }
        return out;
    }

    bool SendResponseHeaders(SOCKET client, CtxtHandle& context,
                             int code, const char* reason,
                             const std::string& contentType,
                             uint64_t contentLength,
                             bool partial = false,
                             uint64_t rangeStart = 0,
                             uint64_t rangeEnd = 0,
                             uint64_t total = 0) {
        std::ostringstream headers;
        headers << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
                << "Server: r800zz-WebXR/1.0\r\n"
                << "Content-Type: " << contentType << "\r\n"
                << "Content-Length: " << contentLength << "\r\n"
                << "Accept-Ranges: bytes\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                << "Access-Control-Allow-Headers: Range, Content-Type\r\n"
                << "Cache-Control: no-cache\r\n";
        if (partial) {
            headers << "Content-Range: bytes " << rangeStart << '-'
                    << rangeEnd << '/' << total << "\r\n";
        }
        headers << "Connection: close\r\n\r\n";
        return SendTlsString(client, context, headers.str());
    }

    void HandleHttp(SOCKET client, CtxtHandle& context,
                    const std::string& request) {
        const size_t lineEnd = request.find("\r\n");
        if (lineEnd == std::string::npos) return;
        std::istringstream firstLine(request.substr(0, lineEnd));
        std::string method;
        std::string path;
        std::string version;
        firstLine >> method >> path >> version;

        if (method == "OPTIONS") {
            std::ostringstream response;
            response << "HTTP/1.1 204 No Content\r\n"
                     << "Access-Control-Allow-Origin: *\r\n"
                     << "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                     << "Access-Control-Allow-Headers: Range, Content-Type\r\n"
                     << "Content-Length: 0\r\nConnection: close\r\n\r\n";
            SendTlsString(client, context, response.str());
            return;
        }
        if (method != "GET" && method != "HEAD") {
            const std::string body = "405 Method Not Allowed\n";
            if (SendResponseHeaders(client, context, 405, "Method Not Allowed",
                                    "text/plain; charset=utf-8", body.size()) &&
                method != "HEAD") {
                SendTlsString(client, context, body);
            }
            return;
        }

        std::filesystem::path file;
        if (!ResolvePath(path, file)) {
            std::string route = path;
            const size_t query = route.find('?');
            if (query != std::string::npos) route.resize(query);
            if (route == "/" || route == "/index.html") {
                const std::string body = MissingIndexPage();
                if (SendResponseHeaders(client, context, 200, "OK",
                                        "text/html; charset=utf-8", body.size()) &&
                    method != "HEAD") {
                    SendTlsString(client, context, body);
                }
                return;
            }
            const std::string body = "404 Not Found\n";
            if (SendResponseHeaders(client, context, 404, "Not Found",
                                    "text/plain; charset=utf-8", body.size()) &&
                method != "HEAD") {
                SendTlsString(client, context, body);
            }
            return;
        }

        std::error_code ec;
        const uint64_t total = std::filesystem::file_size(file, ec);
        if (ec) return;
        uint64_t start = 0;
        uint64_t end = total > 0 ? total - 1 : 0;
        const bool partial = ParseRange(request, total, start, end);
        const uint64_t length = total == 0 ? 0 : end - start + 1;

        if (!SendResponseHeaders(
                client, context,
                partial ? 206 : 200,
                partial ? "Partial Content" : "OK",
                MimeType(file), length,
                partial, start, end, total)) {
            return;
        }
        if (method == "HEAD" || length == 0) return;

        std::ifstream input(file, std::ios::binary);
        if (!input) return;
        input.seekg(static_cast<std::streamoff>(start));
        uint64_t remaining = length;
        std::array<char, 64 * 1024> buffer{};
        while (remaining > 0 && input) {
            const size_t wanted = static_cast<size_t>(
                std::min<uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), static_cast<std::streamsize>(wanted));
            const std::streamsize got = input.gcount();
            if (got <= 0) break;
            if (!SendTlsData(client, context, buffer.data(),
                             static_cast<size_t>(got))) {
                break;
            }
            remaining -= static_cast<uint64_t>(got);
        }
    }

    void HandleClient(SOCKET client) {
        SecurityContextGuard context;
        std::vector<char> encryptedExtra;
        if (!PerformHandshake(client, context, encryptedExtra)) return;
        std::string request;
        if (!ReceiveHttpRequest(client, context.handle,
                                std::move(encryptedExtra), request)) {
            return;
        }
        HandleHttp(client, context.handle, request);
    }

    void SetStatus(const std::string& value) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_ = value;
    }

    std::atomic<bool> running_{false};
    std::atomic<SOCKET> listen_socket_{INVALID_SOCKET};
    bool wsa_started_{false};
    uint16_t https_port_{0};
    std::string advertised_ip_;
    std::filesystem::path root_directory_;
    std::filesystem::path root_certificate_file_;

    CredHandle credential_handle_{};
    bool credential_acquired_{false};
    PCCERT_CONTEXT server_certificate_{nullptr};

    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::condition_variable clients_cv_;
    std::vector<SOCKET> active_clients_;

    mutable std::mutex state_mutex_;
    std::string status_{"Stopped"};
};
