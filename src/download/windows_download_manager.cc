// ============================================================
// download/windows_download_manager.cc
// ============================================================
// 见 windows_download_manager.h 的总体说明。
//
// 下载流程（对齐 Android resumableDownload）：
//   1. 解析最终路径 dest，临时文件 dest + ".part"
//   2. 若 .part 存在 → existingSize = 大小，发起带 Range: bytes=existingSize- 的 GET
//   3. 响应码处理：
//        404            → 失败（清 .part）
//        416            → 丢 .part，重下整包（restart from 0）
//        200            → 服务端忽略 Range，从 0 重下
//        206 Partial    → 校验 Content-Range 起点 == existingSize，否则 restart
//   4. 续传时先把已有 .part 喂进 SHA256（seed digest），再追加写
//   5. 边下边算 SHA256，按字节/时间节流回调进度
//   6. 完成后校验 file_size（若给）+ sha256（若给），通过则原子改名 .part -> dest
//   7. 遥测：completion_path = WebSeedPrimary（中心源站/web-seed 兜底层）、
//      peer_bytes = 0、web_seed_bytes = 本次会话从网络读取的字节
//
// 自更新 apply（设计 + 落地，见文件末尾 apply_self_update）：
//   运行中的 device-agent.exe 不能被直接覆盖。采用「helper 批处理 + 服务重启」
//   原子替换方案。本批为升级 device-agent 自身可执行文件（file_type==exe/
//   self_update）落地可运行实现；apk/firmware 等业务文件仅下载+校验+上报，
//   安装/应用由后续业务侧处理。
// ============================================================

#ifdef _WIN32

#include "download/windows_download_manager.h"

#include "logger/logger.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace device_agent {
namespace {

constexpr DWORD kConnectTimeoutMs = 10000;
constexpr DWORD kSendTimeoutMs = 30000;
constexpr DWORD kReceiveTimeoutMs = 30000;
constexpr std::size_t kBufferSize = 64 * 1024;

// 进度回调节流：≥30s 或 ≥1MB 增量才回调，对齐 Android PROGRESS_* 常量。
constexpr int64_t kProgressMinBytes = 1LL * 1024 * 1024;
constexpr DWORD kProgressIntervalMs = 30000;

std::wstring to_wide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    const int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                          static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          out.data(), len);
    return out;
}

std::string last_error_string(const std::string& prefix) {
    const DWORD code = ::GetLastError();
    return prefix + " (win32=" + std::to_string(code) + ")";
}

std::string basename_from_url(const std::string& url) {
    std::string path = url;
    const auto query = path.find_first_of("?#");
    if (query != std::string::npos) {
        path = path.substr(0, query);
    }
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos && slash + 1 < path.size()) {
        return path.substr(slash + 1);
    }
    return "download.bin";
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") {
        return ".\\" + name;
    }
    const char back = dir.back();
    if (back == '\\' || back == '/') {
        return dir + name;
    }
    return dir + "\\" + name;
}

bool has_file_extension(const std::string& name) {
    const auto slash = name.find_last_of("/\\");
    const auto dot = name.find_last_of('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash + 1) &&
           dot + 1 < name.size();
}

// RAII：WinHTTP 句柄。
struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() {
        if (h != nullptr) {
            ::WinHttpCloseHandle(h);
        }
    }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    HINTERNET get() const { return h; }
    void reset(HINTERNET handle = nullptr) {
        if (h != nullptr) {
            ::WinHttpCloseHandle(h);
        }
        h = handle;
    }
    HINTERNET release() {
        HINTERNET out = h;
        h = nullptr;
        return out;
    }
};

std::string read_response_header(HINTERNET request, DWORD info_flag) {
    DWORD size = 0;
    ::WinHttpQueryHeaders(request, info_flag, WINHTTP_HEADER_NAME_BY_INDEX,
                          WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return std::string();
    }
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(request, info_flag, WINHTTP_HEADER_NAME_BY_INDEX,
                               buf.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return std::string();
    }
    buf.resize(size / sizeof(wchar_t));
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// 解析 "bytes 1024-2047/4096" 的起点；解析失败返回 -1。
int64_t parse_content_range_start(const std::string& content_range) {
    const auto sp = content_range.find(' ');
    if (sp == std::string::npos) {
        return -1;
    }
    const auto dash = content_range.find('-', sp + 1);
    if (dash == std::string::npos) {
        return -1;
    }
    const std::string num = content_range.substr(sp + 1, dash - sp - 1);
    try {
        return std::stoll(num);
    } catch (...) {
        return -1;
    }
}

// 解析 "bytes 1024-2047/4096" 的总长度；解析失败返回 -1。
int64_t parse_content_range_total(const std::string& content_range) {
    const auto slash = content_range.find_last_of('/');
    if (slash == std::string::npos) {
        return -1;
    }
    try {
        return std::stoll(content_range.substr(slash + 1));
    } catch (...) {
        return -1;
    }
}

}  // namespace

std::string windows_resolve_dest_path(const DownloadRequest& req) {
    std::string filename = req.file_id.empty() ? basename_from_url(req.url) : req.file_id;
    if (req.file_type == "windows_app" && !has_file_extension(filename)) {
        filename += ".exe";
    }
    const std::string dir = req.dest_path.empty() ? "." : req.dest_path;
    return join_path(dir, filename);
}

// 以 BCrypt/CNG 计算文件 SHA256（小写 hex）。
bool windows_sha256_file(const std::string& path, std::string& out_hex, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "failed to open file for sha256: " + path;
        return false;
    }

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        error = "BCryptOpenAlgorithmProvider failed";
        return false;
    }
    struct AlgGuard {
        BCRYPT_ALG_HANDLE a;
        ~AlgGuard() { if (a) ::BCryptCloseAlgorithmProvider(a, 0); }
    } alg_guard{alg};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        error = "BCryptCreateHash failed";
        return false;
    }
    struct HashGuard {
        BCRYPT_HASH_HANDLE h;
        ~HashGuard() { if (h) ::BCryptDestroyHash(h); }
    } hash_guard{hash};

    std::array<unsigned char, kBufferSize> buffer{};
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = input.gcount();
        if (n > 0) {
            if (::BCryptHashData(hash, buffer.data(), static_cast<ULONG>(n), 0) != 0) {
                error = "BCryptHashData failed";
                return false;
            }
        }
    }
    if (input.bad()) {
        error = "failed to read file for sha256: " + path;
        return false;
    }

    std::array<unsigned char, 32> digest{};
    if (::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
        error = "BCryptFinishHash failed";
        return false;
    }

    static const char* kHex = "0123456789abcdef";
    out_hex.clear();
    out_hex.reserve(digest.size() * 2);
    for (unsigned char b : digest) {
        out_hex.push_back(kHex[b >> 4]);
        out_hex.push_back(kHex[b & 0x0F]);
    }
    return true;
}

namespace {

// 把已有 .part 内容喂进 SHA256（续传时使用）。返回喂入字节数，失败返回 -1。
int64_t seed_digest_from_part(BCRYPT_HASH_HANDLE hash, const std::string& part_path) {
    std::ifstream input(part_path, std::ios::binary);
    if (!input.is_open()) {
        return -1;
    }
    std::array<unsigned char, kBufferSize> buffer{};
    int64_t seeded = 0;
    while (input.good()) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const std::streamsize n = input.gcount();
        if (n > 0) {
            if (::BCryptHashData(hash, buffer.data(), static_cast<ULONG>(n), 0) != 0) {
                return -1;
            }
            seeded += static_cast<int64_t>(n);
        }
    }
    if (input.bad()) {
        return -1;
    }
    return seeded;
}

int64_t file_size_on_disk(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    LARGE_INTEGER li;
    li.HighPart = static_cast<LONG>(data.nFileSizeHigh);
    li.LowPart = data.nFileSizeLow;
    return static_cast<int64_t>(li.QuadPart);
}

// 原子改名 .part -> dest（先删旧 dest）。
bool move_part_to_dest(const std::string& part_path, const std::string& dest_path) {
    ::DeleteFileA(dest_path.c_str());
    if (::MoveFileExA(part_path.c_str(), dest_path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    LOG_WARN("WindowsDownloadManager: MoveFileEx .part -> dest failed: " +
             last_error_string(dest_path));
    return false;
}

}  // namespace

WindowsDownloadManager::~WindowsDownloadManager() {
    cancel_requested_.store(true);
    join_worker();
}

void WindowsDownloadManager::join_worker() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!worker_.joinable()) {
            return;
        }
        worker = std::move(worker_);
    }
    worker.join();
}

bool WindowsDownloadManager::is_downloading() const {
    return downloading_.load();
}

void WindowsDownloadManager::cancel() {
    cancel_requested_.store(true);
    join_worker();
    cancel_requested_.store(false);
}

void WindowsDownloadManager::download(const DownloadRequest& req,
                                      ProgressCallback on_progress,
                                      CompleteCallback on_complete) {
    join_worker();
    cancel_requested_.store(false);
    downloading_.store(true);
    std::lock_guard<std::mutex> lock(mu_);
    worker_ = std::thread(&WindowsDownloadManager::run_download, this, req,
                          std::move(on_progress), std::move(on_complete));
}

void WindowsDownloadManager::run_download(DownloadRequest req,
                                          ProgressCallback on_progress,
                                          CompleteCallback on_complete) {
    DownloadCompletionTelemetry telemetry;
    auto finish = [&](bool ok, const std::string& err, WindowsCompletionPath path,
                      int64_t web_seed_bytes) {
        telemetry.completion_path = static_cast<int>(path);
        telemetry.peer_bytes = 0;  // 本批 Windows = download-only，无 P2P peer
        telemetry.web_seed_bytes = web_seed_bytes;
        downloading_.store(false);
        if (on_complete) {
            on_complete(ok, err, telemetry);
        }
    };

    if (req.url.empty()) {
        LOG_ERROR("WindowsDownloadManager: empty download_url file=" + req.file_id);
        finish(false, "download_url is empty", WindowsCompletionPath::Unspecified, 0);
        return;
    }

    const std::string dest = windows_resolve_dest_path(req);
    const std::string part = dest + ".part";
    LOG_INFO("WindowsDownloadManager: download file=" + req.file_id +
             " dest=" + dest + " size=" + std::to_string(req.file_size));

    // ── URL 解析 ──────────────────────────────────────────
    const std::wstring wurl = to_wide(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    std::array<wchar_t, 256> host{};
    std::array<wchar_t, 2048> urlpath{};
    uc.lpszHostName = host.data();
    uc.dwHostNameLength = static_cast<DWORD>(host.size());
    uc.lpszUrlPath = urlpath.data();
    uc.dwUrlPathLength = static_cast<DWORD>(urlpath.size());
    if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        finish(false, last_error_string("WinHttpCrackUrl failed"),
               WindowsCompletionPath::Unspecified, 0);
        return;
    }
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const std::wstring host_name(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path_query(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) {
        path_query.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    const INTERNET_PORT port = uc.nPort;

    WinHttpHandle session(::WinHttpOpen(L"device-agent/1.0",
                                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.get()) {
        finish(false, last_error_string("WinHttpOpen failed"),
               WindowsCompletionPath::Unspecified, 0);
        return;
    }
    // 显式启用 TLS1.2（Win7 注意点，同 9e81f2d；新系统默认已含）。
    DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    ::WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS,
                       &secure_protocols, sizeof(secure_protocols));
    ::WinHttpSetTimeouts(session.get(), 0, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    WinHttpHandle connect(::WinHttpConnect(session.get(), host_name.c_str(), port, 0));
    if (!connect.get()) {
        finish(false, last_error_string("WinHttpConnect failed"),
               WindowsCompletionPath::Unspecified, 0);
        return;
    }

    int64_t existing_size = file_size_on_disk(part);
    int64_t web_seed_bytes = 0;  // 本会话从网络读取的字节（用于账本 web_seed_bytes）

    // 重试循环：处理 416 / 200-ignore-range / Content-Range mismatch 的整包重下。
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (cancel_requested_.load()) {
            finish(false, "download cancelled", WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }

        const DWORD req_flags = https ? WINHTTP_FLAG_SECURE : 0;
        WinHttpHandle request(::WinHttpOpenRequest(connect.get(), L"GET", path_query.c_str(),
                                                   nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags));
        if (!request.get()) {
            finish(false, last_error_string("WinHttpOpenRequest failed"),
                   WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }

        std::wstring headers = L"Accept: application/octet-stream\r\n";
        if (!req.command_id.empty()) {
            headers += L"X-Device-ID: " + to_wide(req.command_id) + L"\r\n";
        }
        const bool use_range = existing_size > 0;
        if (use_range) {
            headers += L"Range: bytes=" + std::to_wstring(existing_size) + L"-\r\n";
        }

        if (!::WinHttpSendRequest(request.get(), headers.c_str(),
                                  static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(request.get(), nullptr)) {
            finish(false, last_error_string("WinHttp send/receive failed"),
                   WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        ::WinHttpQueryHeaders(request.get(),
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                              WINHTTP_NO_HEADER_INDEX);
        const std::string content_range =
            read_response_header(request.get(), WINHTTP_QUERY_CONTENT_RANGE);
        LOG_INFO("WindowsDownloadManager: HTTP status=" + std::to_string(status) +
                 " Content-Range=" + content_range + " existing=" + std::to_string(existing_size));

        bool append = false;
        if (status == 404) {
            ::DeleteFileA(part.c_str());
            finish(false, "http 404 not found", WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        } else if (status == 416) {
            LOG_WARN("WindowsDownloadManager: 416, dropping .part and restarting");
            ::DeleteFileA(part.c_str());
            existing_size = 0;
            continue;
        } else if (status == 200) {
            if (use_range) {
                LOG_WARN("WindowsDownloadManager: 200 ignoring Range, restarting from 0");
            }
            existing_size = 0;
            append = false;
        } else if (status == 206) {
            const int64_t start = parse_content_range_start(content_range);
            if (start != existing_size) {
                LOG_WARN("WindowsDownloadManager: Content-Range mismatch start=" +
                         std::to_string(start) + " existing=" + std::to_string(existing_size) +
                         ", restarting");
                ::DeleteFileA(part.c_str());
                existing_size = 0;
                continue;
            }
            append = true;
        } else {
            finish(false, "unexpected http status " + std::to_string(status),
                   WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }

        // 计算 total_bytes（用于进度百分比）。
        int64_t total_bytes = -1;
        const std::string content_length =
            read_response_header(request.get(), WINHTTP_QUERY_CONTENT_LENGTH);
        if (status == 206) {
            total_bytes = parse_content_range_total(content_range);
            if (total_bytes <= 0 && !content_length.empty()) {
                try { total_bytes = existing_size + std::stoll(content_length); } catch (...) {}
            }
        } else if (!content_length.empty()) {
            try { total_bytes = std::stoll(content_length); } catch (...) {}
        }
        if (total_bytes <= 0 && req.file_size > 0) {
            total_bytes = req.file_size;
        }

        // SHA256 上下文：续传时先喂入已有 .part。
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            finish(false, "BCryptOpenAlgorithmProvider failed",
                   WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
            ::BCryptCloseAlgorithmProvider(alg, 0);
            finish(false, "BCryptCreateHash failed",
                   WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }
        auto close_hash = [&]() {
            if (hash) { ::BCryptDestroyHash(hash); hash = nullptr; }
            if (alg) { ::BCryptCloseAlgorithmProvider(alg, 0); alg = nullptr; }
        };

        if (append && existing_size > 0) {
            const int64_t seeded = seed_digest_from_part(hash, part);
            if (seeded != existing_size) {
                LOG_WARN("WindowsDownloadManager: seed digest mismatch (" +
                         std::to_string(seeded) + " vs " + std::to_string(existing_size) +
                         "), restarting");
                close_hash();
                ::DeleteFileA(part.c_str());
                existing_size = 0;
                continue;
            }
        }

        // 打开 .part 写入（追加或截断）。
        std::ofstream out(part, std::ios::binary |
                                    (append ? std::ios::app : std::ios::trunc));
        if (!out.is_open()) {
            close_hash();
            finish(false, "failed to open .part for write: " + part,
                   WindowsCompletionPath::Unspecified, web_seed_bytes);
            return;
        }

        int64_t downloaded = existing_size;
        int64_t last_report_bytes = 0;
        DWORD last_report_tick = ::GetTickCount();
        std::vector<char> buf(kBufferSize);
        bool read_failed = false;

        while (true) {
            if (cancel_requested_.load()) {
                out.close();
                close_hash();
                finish(false, "download cancelled", WindowsCompletionPath::Unspecified,
                       web_seed_bytes);
                return;
            }
            DWORD avail = 0;
            if (!::WinHttpQueryDataAvailable(request.get(), &avail)) {
                read_failed = true;
                break;
            }
            if (avail == 0) {
                break;  // 正常结束
            }
            DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(avail, buf.size()));
            DWORD got = 0;
            if (!::WinHttpReadData(request.get(), buf.data(), to_read, &got) || got == 0) {
                read_failed = true;
                break;
            }
            out.write(buf.data(), static_cast<std::streamsize>(got));
            if (!out.good()) {
                read_failed = true;
                break;
            }
            ::BCryptHashData(hash, reinterpret_cast<unsigned char*>(buf.data()), got, 0);
            downloaded += static_cast<int64_t>(got);
            web_seed_bytes += static_cast<int64_t>(got);

            // 进度回调节流（与 Android shouldReportProgress 一致的口径）。
            if (on_progress) {
                const DWORD now = ::GetTickCount();
                const bool by_time = (now - last_report_tick) >= kProgressIntervalMs;
                int64_t threshold = kProgressMinBytes;
                if (total_bytes > 0) {
                    threshold = std::max<int64_t>(total_bytes / 20, kProgressMinBytes);
                }
                const bool by_bytes = (downloaded - last_report_bytes) >= threshold;
                if (by_time || by_bytes) {
                    last_report_tick = now;
                    last_report_bytes = downloaded;
                    DownloadProgress p{};
                    p.downloaded_bytes = downloaded;
                    p.total_bytes = total_bytes > 0 ? total_bytes : 0;
                    p.percent = total_bytes > 0
                                    ? static_cast<int>(std::min<int64_t>(
                                          100, downloaded * 100 / total_bytes))
                                    : 0;
                    on_progress(p);
                }
            }
        }
        out.close();

        if (read_failed) {
            close_hash();
            // .part 保留以便下次续传。
            finish(false, last_error_string("WinHttp read failed"),
                   WindowsCompletionPath::HttpFallbackStall, web_seed_bytes);
            return;
        }

        // ── 校验 size ──
        if (req.file_size > 0) {
            const int64_t actual = file_size_on_disk(part);
            if (actual != req.file_size) {
                LOG_ERROR("WindowsDownloadManager: size mismatch expected=" +
                          std::to_string(req.file_size) + " actual=" + std::to_string(actual));
                close_hash();
                ::DeleteFileA(part.c_str());
                finish(false, "file size mismatch", WindowsCompletionPath::HttpFallbackShaMismatch,
                       web_seed_bytes);
                return;
            }
        }

        // ── 校验 sha256 ──
        std::array<unsigned char, 32> digest{};
        const NTSTATUS fin = ::BCryptFinishHash(hash, digest.data(),
                                                static_cast<ULONG>(digest.size()), 0);
        close_hash();
        if (fin != 0) {
            finish(false, "BCryptFinishHash failed", WindowsCompletionPath::Unspecified,
                   web_seed_bytes);
            return;
        }
        if (!req.expected_sha256.empty()) {
            static const char* kHex = "0123456789abcdef";
            std::string actual;
            actual.reserve(64);
            for (unsigned char b : digest) {
                actual.push_back(kHex[b >> 4]);
                actual.push_back(kHex[b & 0x0F]);
            }
            std::string expected = req.expected_sha256;
            std::transform(expected.begin(), expected.end(), expected.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (actual != expected) {
                LOG_ERROR("WindowsDownloadManager: sha256 mismatch expected=" + expected +
                          " actual=" + actual);
                ::DeleteFileA(part.c_str());
                finish(false, "sha256 mismatch", WindowsCompletionPath::HttpFallbackShaMismatch,
                       web_seed_bytes);
                return;
            }
            LOG_INFO("WindowsDownloadManager: sha256 verified " + actual);
        }

        if (!move_part_to_dest(part, dest)) {
            finish(false, "failed to move .part to dest", WindowsCompletionPath::Unspecified,
                   web_seed_bytes);
            return;
        }

        LOG_INFO("WindowsDownloadManager: download complete file=" + req.file_id +
                 " bytes=" + std::to_string(downloaded) + " path=" + dest);
        finish(true, "", WindowsCompletionPath::WebSeedPrimary, web_seed_bytes);
        return;
    }

    finish(false, "download retries exhausted", WindowsCompletionPath::Unspecified, web_seed_bytes);
}

// ─── 自更新 apply ─────────────────────────────────────────
bool windows_apply_self_update(const std::string& new_exe_path,
                               const std::string& target_exe_path,
                               const std::string& service_name,
                               std::string& error) {
    if (new_exe_path.empty()) {
        error = "new_exe_path is empty";
        return false;
    }
    if (file_size_on_disk(new_exe_path) <= 0) {
        error = "new exe missing or empty: " + new_exe_path;
        return false;
    }

    std::string target = target_exe_path;
    if (target.empty()) {
        std::array<char, MAX_PATH> module_path{};
        const DWORD n = ::GetModuleFileNameA(nullptr, module_path.data(),
                                             static_cast<DWORD>(module_path.size()));
        if (n == 0 || n >= module_path.size()) {
            error = "GetModuleFileName failed";
            return false;
        }
        target.assign(module_path.data(), n);
    }

    const DWORD pid = ::GetCurrentProcessId();
    const char* temp = std::getenv("TEMP");
    std::string temp_dir = (temp && temp[0]) ? std::string(temp) : std::string(".");
    const std::string cmd_path =
        temp_dir + "\\device-agent-update-" + std::to_string(pid) + ".cmd";
    const std::string backup = target + ".bak";

    // 生成 helper 批处理。注意：批处理用 GBK/系统编码，路径不含中文时安全。
    std::ofstream cmd(cmd_path, std::ios::binary | std::ios::trunc);
    if (!cmd.is_open()) {
        error = "failed to write helper cmd: " + cmd_path;
        return false;
    }
    cmd << "@echo off\r\n";
    cmd << "setlocal\r\n";
    // 等待旧进程退出（最多 ~60s）。
    cmd << ":waitloop\r\n";
    cmd << "tasklist /FI \"PID eq " << pid << "\" 2>NUL | find \"" << pid << "\" >NUL\r\n";
    cmd << "if not errorlevel 1 (\r\n";
    cmd << "  timeout /t 1 /nobreak >NUL\r\n";
    cmd << "  goto waitloop\r\n";
    cmd << ")\r\n";
    if (!service_name.empty()) {
        cmd << "net stop \"" << service_name << "\" >NUL 2>&1\r\n";
    }
    // 备份 + 原子替换；失败回滚。
    cmd << "copy /Y \"" << target << "\" \"" << backup << "\" >NUL 2>&1\r\n";
    cmd << "move /Y \"" << new_exe_path << "\" \"" << target << "\" >NUL\r\n";
    cmd << "if errorlevel 1 (\r\n";
    cmd << "  copy /Y \"" << backup << "\" \"" << target << "\" >NUL 2>&1\r\n";
    cmd << ")\r\n";
    if (!service_name.empty()) {
        cmd << "net start \"" << service_name << "\" >NUL 2>&1\r\n";
    } else {
        cmd << "start \"\" \"" << target << "\"\r\n";
    }
    // 自删除（延迟，避免占用自身）。
    cmd << "del \"" << cmd_path << "\" >NUL 2>&1\r\n";
    cmd.close();

    // 以分离、无窗口方式启动 helper。
    std::string command_line = "cmd.exe /c \"" + cmd_path + "\"";
    std::vector<char> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                                     &si, &pi);
    if (!ok) {
        error = last_error_string("CreateProcess(helper) failed");
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    LOG_INFO("WindowsDownloadManager: self-update helper scheduled cmd=" + cmd_path +
             " target=" + target + (service_name.empty() ? "" : " service=" + service_name));
    return true;
}

}  // namespace device_agent

#endif  // _WIN32
