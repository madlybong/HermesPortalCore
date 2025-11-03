// src/FileWriter.cpp
// FileWriter — always-write mode; 7202 file outputs get appended timestamp; 7208 time field converted.
// Replace existing src/FileWriter.cpp with this file.

#include "FileWriter.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#else
#include <unistd.h> // readlink
#endif

// ----------------- internal helpers & types -----------------
namespace {

    struct Task {
        uint16_t type{ 0 };
        uint32_t token{ 0 };
        std::string csv;
        std::string market; // optional market folder
    };

    // return directory of the running executable (no trailing slash)
    static std::string exe_dir() {
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0 || n == MAX_PATH) return std::string(".");
        PathRemoveFileSpecW(buf);
        int sz = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (sz <= 0) return std::string(".");
        std::string s(sz - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s[0], sz, nullptr, nullptr);
        return s;
#else
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return std::string(".");
        buf[n] = '\0';
        std::string p(buf);
        auto pos = p.find_last_of('/');
        if (pos == std::string::npos) return std::string(".");
        return p.substr(0, pos);
#endif
    }

    // simple CSV split/join (no quoting support; matches Hermes CSV)
    static std::vector<std::string> split_csv(const std::string& s) {
        std::vector<std::string> out;
        size_t i = 0;
        while (true) {
            size_t j = s.find(',', i);
            if (j == std::string::npos) { out.emplace_back(s.substr(i)); break; }
            out.emplace_back(s.substr(i, j - i));
            i = j + 1;
        }
        return out;
    }
    static std::string join_csv(const std::vector<std::string>& v) {
        std::string o;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) o.push_back(',');
            o += v[i];
        }
        return o;
    }

    static std::string unix_to_local(uint64_t unixsec) {
        std::time_t tt = static_cast<std::time_t>(unixsec);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }

    // return current local time formatted "YYYY-MM-DD HH:MM:SS"
    static std::string now_local_string() {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t tt = system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
        return std::string(buf);
    }

    // sanitize path component (tokens are numeric but keep it safe)
    static std::string sanitize_component(const std::string& s) {
        std::string out; out.reserve(s.size());
        for (char c : s) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<' || c == '>' || c == '|') out.push_back('_');
            else out.push_back(c);
        }
        return out;
    }

    // ----------------- Impl (hidden) -----------------
    // Not nested in FileWriter; kept in anon namespace to avoid leaking symbols.
    class Impl {
    public:
        Impl()
            : max_queue_(10000), stop_flag_(false) {
        }

        ~Impl() { stop(); }

        void start(const std::string& baseDir) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (worker_.joinable()) return;

            if (baseDir.empty()) {
                std::string ed = exe_dir();
                base_dir_ = (std::filesystem::path(ed) / "data").string();
            }
            else {
                base_dir_ = baseDir;
            }

            std::error_code ec;
            std::filesystem::create_directories(base_dir_, ec);
            stop_flag_ = false;
            worker_ = std::thread(&Impl::thread_main, this);
        }

        void stop() {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (!worker_.joinable()) return;
                stop_flag_ = true;
                cv_.notify_all();
            }
            if (worker_.joinable()) worker_.join();
        }

        // Enqueue: always push; no dedupe
        void enqueue(uint16_t type, uint32_t token, const std::string& csv, const std::string& market) {
            Task t; t.type = type; t.token = token; t.csv = csv; t.market = market;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                q_.push(std::move(t));
                if (q_.size() > max_queue_) {
                    // drop oldest to bound memory
                    q_.pop();
                }
            }
            cv_.notify_one();
        }

        void set_max_queue(size_t m) { max_queue_ = m; }

    private:
        std::string base_dir_;
        std::thread worker_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<Task> q_;
        size_t max_queue_;
        bool stop_flag_;

        void thread_main() {
            while (true) {
                Task t;
                {
                    std::unique_lock<std::mutex> lk(mutex_);
                    cv_.wait(lk, [&] { return stop_flag_ || !q_.empty(); });
                    if (stop_flag_ && q_.empty()) break;
                    t = std::move(q_.front());
                    q_.pop();
                }

                // determine market folder:
                // If caller provided a market string (non-empty), use that (sanitized).
                // Otherwise fall back to "7208" or "7202" based on type.
                std::string market_folder;
                if (!t.market.empty()) market_folder = sanitize_component(t.market);
                else market_folder = (t.type == 7208) ? "7208" : "7202";

                std::filesystem::path pbase(base_dir_);
                std::filesystem::path live_dir = pbase / "live" / market_folder;
                std::filesystem::path hist_dir = pbase / "historical" / market_folder;
                std::error_code ec;
                std::filesystem::create_directories(live_dir, ec);
                std::filesystem::create_directories(hist_dir, ec);

                std::string token_str = std::to_string(t.token);
                std::filesystem::path live_path = live_dir / (token_str + ".txt");
                std::filesystem::path hist_path = hist_dir / (token_str + ".txt");

                // normalize line
                std::string line = t.csv;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();

                // For 7208: convert Time field (index 10) if numeric unix seconds -> human
                if (t.type == 7208) {
                    auto parts = split_csv(line);
                    if (parts.size() > 10) {
                        try {
                            uint64_t tm = std::stoull(parts[10]);
                            parts[10] = unix_to_local(tm);
                            line = join_csv(parts);
                        }
                        catch (...) { /* ignore parse errors */ }
                    }
                }

                // For 7202: append current human-readable timestamp as an extra column,
                // because 7202 CSV schema has no time field. This affects only file outputs,
                // not the console/SHM original CSV.
                if (t.type == 7202) {
                    std::string ts = now_local_string();
                    // Append as new CSV column
                    line += ",";
                    line += ts;
                }

                // write latest atomically
                try {
                    std::string tmp = live_path.string() + ".tmp";
                    {
                        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
                        if (ofs) {
                            ofs << line << "\n";
                            ofs.flush();
                            ofs.close();
                        }
                    }
                    std::error_code rc;
#ifdef _WIN32
                    std::filesystem::remove(live_path, rc);
                    std::filesystem::rename(tmp, live_path, rc);
                    if (rc) {
                        std::remove(tmp.c_str());
                    }
#else
                    std::filesystem::rename(tmp, live_path, rc);
                    if (rc) {
                        std::filesystem::remove(live_path, rc);
                        std::filesystem::rename(tmp, live_path, rc);
                    }
#endif
                }
                catch (...) {
                    // ignore single-write failures
                }

                // append historical
                try {
                    std::ofstream ofs(hist_path.string(), std::ios::binary | std::ios::app);
                    if (ofs) {
                        ofs << line << "\n";
                        ofs.flush();
                    }
                }
                catch (...) {
                    // ignore
                }
            } // while
        }
    };

    // single global impl instance (hidden)
    static Impl* g_impl = nullptr;

} // namespace (anon)

// ----------------- FileWriter public wrapper -----------------
FileWriter::FileWriter() {
    if (!g_impl) g_impl = new Impl();
}
FileWriter::~FileWriter() {
    if (g_impl) {
        delete g_impl;
        g_impl = nullptr;
    }
}

void FileWriter::start(const std::string& baseDir) {
    if (g_impl) g_impl->start(baseDir);
}
void FileWriter::stop() {
    if (g_impl) g_impl->stop();
}
void FileWriter::enqueue(uint16_t type, uint32_t token, const std::string& csvLine, const std::string& market) {
    if (g_impl) g_impl->enqueue(type, token, csvLine, market);
}
void FileWriter::set_max_queue_size(size_t maxq) {
    if (g_impl) g_impl->set_max_queue(maxq);
}
