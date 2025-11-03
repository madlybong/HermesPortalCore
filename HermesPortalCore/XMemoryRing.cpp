#include <cstring>
#include <algorithm>
#include <cerrno>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include "XMemoryRing.hpp"

namespace xmr {

    static constexpr uint64_t kMinCapacity = 64ull * 1024ull;

    static inline bool region_valid(void* base) { return base != nullptr; }

    // --------- time helpers ---------
    uint64_t Writer::now_nanos() {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count());
    }
    uint64_t Reader::now_nanos() {
        using namespace std::chrono;
        return static_cast<uint64_t>(duration_cast<nanoseconds>(
            system_clock::now().time_since_epoch()).count());
    }

    // --------- header field access (volatile loads/stores) ---------
    uint64_t Writer::load64(size_t off) const {
        return *reinterpret_cast<volatile const uint64_t*>((uint8_t*)base_ + off);
    }
    void Writer::store64(size_t off, uint64_t v) const {
        *reinterpret_cast<volatile uint64_t*>((uint8_t*)base_ + off) = v;
    }
    uint32_t Writer::load32(size_t off) const {
        return *reinterpret_cast<volatile const uint32_t*>((uint8_t*)base_ + off);
    }
    void Writer::store32(size_t off, uint32_t v) const {
        *reinterpret_cast<volatile uint32_t*>((uint8_t*)base_ + off) = v;
    }
    uint64_t Reader::load64(size_t off) const {
        return *reinterpret_cast<volatile const uint64_t*>((uint8_t*)base_ + off);
    }
    void Reader::store64(size_t off, uint64_t v) const {
        *reinterpret_cast<volatile uint64_t*>((uint8_t*)base_ + off) = v;
    }
    uint32_t Reader::load32(size_t off) const {
        return *reinterpret_cast<volatile const uint32_t*>((uint8_t*)base_ + off);
    }

    // --------- Windows helpers ---------
    std::wstring Writer::utf8_to_wide(const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
    std::wstring Reader::utf8_to_wide(const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
        return w;
    }
    std::wstring Writer::compose_map_name_w(const std::wstring& name, const std::wstring& token) {
        std::wstring pair = name + L"." + token;
        if (pair.rfind(L"Local:", 0) == 0 || pair.rfind(L"Local\\", 0) == 0) return pair;
        return L"Local\\" + pair;
    }
    std::wstring Reader::compose_map_name_w(const std::wstring& name, const std::wstring& token) {
        std::wstring pair = name + L"." + token;
        if (pair.rfind(L"Local:", 0) == 0 || pair.rfind(L"Local\\", 0) == 0) return pair;
        return L"Local\\" + pair;
    }

    // --------- Writer header ops ---------
    bool Writer::validate_header(uint64_t& outCap) const {
        if (!region_valid(base_)) return false;
        if (load64(oMagic) != kMagic64) return false;
        if (load32(oABIVer) != kAbiVersion) return false;
        uint64_t c = load64(oCapacity);
        if (!c) return false;
        outCap = c;
        return true;
    }
    void Writer::init_header(uint64_t cap) const {
        store64(oMagic, kMagic64);
        store32(oABIVer, kAbiVersion);
        store32(oFlags, 0);
        store64(oCapacity, cap);
        store64(oHeartbeat, now_nanos());
    }

    // --------- Writer open/close ---------
    void Writer::open(const Config& cfg) {
        if (is_open()) close();
        if (cfg.capacity_bytes < kMinCapacity)
            throw std::runtime_error("XMemoryRing: capacity too small (min 64 KiB)");

        cfg_ = cfg;

        std::wstring nameW = !cfg.nameW.empty() ? cfg.nameW : utf8_to_wide(cfg.name);
        std::wstring tokenW = !cfg.tokenW.empty() ? cfg.tokenW : utf8_to_wide(cfg.token);
        if (nameW.empty() || tokenW.empty()) throw std::runtime_error("XMemoryRing: Name/Token required");
        map_name_w_ = compose_map_name_w(nameW, tokenW);
        size_ = static_cast<size_t>(oHdrEnd + cfg.capacity_bytes);

        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            (DWORD)((uint64_t)size_ >> 32),
            (DWORD)((uint64_t)size_ & 0xffffffffu),
            map_name_w_.c_str());
        if (!h) {
            DWORD e = GetLastError();
            if (cfg.on_event) cfg.on_event(EventType::Error, "CreateFileMappingW failed");
            throw std::system_error((int)e, std::system_category(), "CreateFileMappingW");
        }
        void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!v) {
            DWORD e = GetLastError();
            CloseHandle(h);
            if (cfg.on_event) cfg.on_event(EventType::Error, "MapViewOfFile failed");
            throw std::system_error((int)e, std::system_category(), "MapViewOfFile");
        }
        hmap_ = h;
        base_ = v;

        uint64_t cap = 0;
        if (!validate_header(cap) || cap != cfg.capacity_bytes) {
            init_header(cfg.capacity_bytes);
        }
        if (!validate_header(cap)) {
            close();
            if (cfg.on_event) cfg.on_event(EventType::Error, "header validation failed");
            throw std::runtime_error("XMemoryRing: header validation failed");
        }

        cap_ = cap;
        data_ = reinterpret_cast<uint8_t*>(base_) + oHdrEnd;

        start_hb(cfg.heartbeat_interval.count() ? cfg.heartbeat_interval : std::chrono::nanoseconds(500'000'000));

        if (cfg.on_event) cfg.on_event(EventType::Opened, mapping_name());
    }

    void Writer::close() noexcept {
        stop_hb();
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (hmap_) { CloseHandle(hmap_); hmap_ = nullptr; }
        map_name_w_.clear();
        data_ = nullptr; cap_ = 0; size_ = 0;
        if (cfg_.on_event) cfg_.on_event(EventType::Closed, "");
    }

    Writer::~Writer() { close(); }
    Writer::Writer(Writer&& o) noexcept { *this = std::move(o); }
    Writer& Writer::operator=(Writer&& o) noexcept {
        if (this == &o) return *this;
        close();
        base_ = o.base_; o.base_ = nullptr;
        data_ = o.data_; o.data_ = nullptr;
        cap_ = o.cap_;  o.cap_ = 0;
        size_ = o.size_; o.size_ = 0;
        hmap_ = o.hmap_; o.hmap_ = nullptr;
        map_name_w_ = std::move(o.map_name_w_);
        hb_stop_.store(o.hb_stop_.load());
        cfg_ = std::move(o.cfg_);
        if (o.hb_.joinable()) hb_ = std::move(o.hb_);
        return *this;
    }

    // --------- Writer ops ---------
    void Writer::start_hb(std::chrono::nanoseconds iv) {
        hb_stop_.store(false, std::memory_order_relaxed);
        hb_ = std::thread([this, iv]() {
            auto dur = iv.count() ? iv : std::chrono::nanoseconds(500'000'000);
            while (!hb_stop_.load(std::memory_order_relaxed)) {
                store64(oHeartbeat, now_nanos());
                std::this_thread::sleep_for(dur);
            }
            });
    }
    void Writer::stop_hb() {
        if (hb_.joinable()) { hb_stop_.store(true, std::memory_order_relaxed); hb_.join(); }
    }

    bool Writer::write(const std::string& s) {
        return write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    bool Writer::write(const char* s) {
        return s ? write(reinterpret_cast<const uint8_t*>(s), std::strlen(s)) : false;
    }

    bool Writer::write(const uint8_t* data, size_t len) {
        if (!base_) return false;
        if (len == 0) { store64(oHeartbeat, now_nanos()); return true; }

        // framing
        bool newline_mode = (cfg_.frame_mode == FrameMode::Newline);
        size_t n = len;
        if (newline_mode && data[len - 1] != '\n') n++;
        size_t need = newline_mode ? n : n + sizeof(uint32_t);
        if (need > cap_) { store64(oOver, load64(oOver) + 1); return false; }

        uint64_t head = load64(oHead);
        uint64_t tail = load64(oTail);
        uint64_t used = head - tail;

        if (cap_ - used < need) {
            if (cfg_.drop_policy == DropPolicy::DropNewest) {
                store64(oDrops, load64(oDrops) + 1);
                return false;
            }
            else {
                // drop-oldest: advance tail enough
                uint64_t advance = need - (cap_ - used);
                store64(oTail, tail + advance);
                store64(oDrops, load64(oDrops) + 1);
            }
        }

        uint64_t pos = head % cap_;
        size_t first = static_cast<size_t>(cap_ - pos);
        if (first > need) first = need;

        if (cfg_.frame_mode == FrameMode::LengthPrefix) {
            uint32_t le = (uint32_t)len;
            std::memcpy(&data_[pos], &le, (std::min)(first, sizeof(uint32_t)));
            if (first < sizeof(uint32_t)) {
                std::memcpy(&data_[0], ((uint8_t*)&le) + first, sizeof(uint32_t) - first);
                size_t rem = need - sizeof(uint32_t);
                std::memcpy(&data_[sizeof(uint32_t) - first], data, (std::min)(len, rem));
            }
            else {
                std::memcpy(&data_[(pos + sizeof(uint32_t)) % cap_], data, len);
            }
        }
        else {
            if (first) {
                std::memcpy(&data_[pos], data, (std::min)(len, first));
                if (n > len && first == n) data_[pos + first - 1] = '\n';
            }
            if (first < n) {
                size_t rem = n - first;
                std::memcpy(&data_[0], data + first, (std::min)(len - first, rem));
                if (n > len) data_[rem - 1] = '\n';
            }
        }

        store64(oHead, head + need);
        store64(oBWrite, load64(oBWrite) + need);
        store64(oHeartbeat, now_nanos());
        return true;
    }

    void Writer::heartbeat() { if (base_) store64(oHeartbeat, now_nanos()); }
    WriterStats Writer::stats() const {
        WriterStats s{};
        if (!base_) return s;
        s.drops_newest = load64(oDrops);
        s.drops_oldest = 0;
        s.oversize = load64(oOver);
        s.bytes_written = load64(oBWrite);
        return s;
    }
    std::string Writer::mapping_name() const {
        std::string out; out.reserve(map_name_w_.size());
        for (wchar_t c : map_name_w_) out.push_back(static_cast<char>(c & 0x7F));
        return out;
    }

    // --------- Reader ops ---------
    bool Reader::validate_header(uint64_t& outCap) const {
        if (!region_valid(base_)) return false;
        if (load64(oMagic) != kMagic64) return false;
        if (load32(oABIVer) != kAbiVersion) return false;
        uint64_t c = load64(oCapacity);
        if (!c) return false;
        outCap = c;
        return true;
    }

    void Reader::open(const Config& cfg) {
        if (is_open()) close();
        cfg_ = cfg;

        std::wstring nameW = !cfg.nameW.empty() ? cfg.nameW : utf8_to_wide(cfg.name);
        std::wstring tokenW = !cfg.tokenW.empty() ? cfg.tokenW : utf8_to_wide(cfg.token);
        if (nameW.empty() || tokenW.empty()) throw std::runtime_error("XMemoryRing: Name/Token required");
        map_name_w_ = compose_map_name_w(nameW, tokenW);

        HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, map_name_w_.c_str());
        if (!h) { DWORD e = GetLastError(); throw std::system_error((int)e, std::system_category(), "OpenFileMappingW"); }
        void* v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!v) { DWORD e = GetLastError(); CloseHandle(h); throw std::system_error((int)e, std::system_category(), "MapViewOfFile"); }
        hmap_ = h; base_ = v;

        uint64_t cap = 0;
        if (!validate_header(cap)) { close(); throw std::runtime_error("XMemoryRing: header validation failed"); }
        cap_ = cap;
        data_ = reinterpret_cast<uint8_t*>(base_) + oHdrEnd;
    }

    void Reader::close() noexcept {
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (hmap_) { CloseHandle(hmap_); hmap_ = nullptr; }
        map_name_w_.clear();
        data_ = nullptr; cap_ = 0;
    }

    Reader::~Reader() { close(); }
    Reader::Reader(Reader&& o) noexcept { *this = std::move(o); }
    Reader& Reader::operator=(Reader&& o) noexcept {
        if (this == &o) return *this;
        close();
        base_ = o.base_; o.base_ = nullptr;
        data_ = o.data_; o.data_ = nullptr;
        cap_ = o.cap_;  o.cap_ = 0;
        hmap_ = o.hmap_; o.hmap_ = nullptr;
        map_name_w_ = std::move(o.map_name_w_);
        rs_ = o.rs_;
        cfg_ = std::move(o.cfg_);
        return *this;
    }

    bool Reader::read(std::string& out) {
        for (;;) {
            if (read_nonblocking(out)) return true;
            std::this_thread::sleep_for(cfg_.idle_sleep_us);
            rs_.sleeps++;
        }
    }
    bool Reader::read_nonblocking(std::string& out) {
        if (!base_) return false;
        uint64_t head = load64(oHead);
        uint64_t tail = load64(oTail);
        if (tail == head) return false;

        uint64_t used = head - tail;
        uint64_t pos = tail % cap_;

        if (cfg_.frame_mode == FrameMode::LengthPrefix) {
            if (used < sizeof(uint32_t)) return false;
            uint32_t le = 0;
            std::memcpy(&le, &data_[pos], (std::min)((uint64_t)sizeof(uint32_t), used));
            if (used < sizeof(uint32_t) + le) return false;
            out.resize(le);
            std::memcpy(&out[0], &data_[(pos + sizeof(uint32_t)) % cap_], le);
            store64(oTail, tail + sizeof(uint32_t) + le);
            store64(oBRead, load64(oBRead) + sizeof(uint32_t) + le);
            rs_.bytes_read += sizeof(uint32_t) + le;
            return true;
        }
        else {
            size_t first = static_cast<size_t>(cap_ - pos);
            if (first > used) first = (size_t)used;
            const uint8_t* p1 = &data_[pos];
            const uint8_t* nl = (const uint8_t*)memchr(p1, '\n', first);
            if (nl) {
                size_t n = (size_t)(nl - p1) + 1;
                out.assign(reinterpret_cast<const char*>(p1), n);
                store64(oTail, tail + n);
                store64(oBRead, load64(oBRead) + n);
                rs_.bytes_read += n;
                return true;
            }
            size_t second = (size_t)(used - first);
            if (second) {
                const uint8_t* p2 = &data_[0];
                const uint8_t* nl2 = (const uint8_t*)memchr(p2, '\n', second);
                if (nl2) {
                    size_t n2 = (size_t)(nl2 - p2) + 1;
                    out.reserve(first + n2);
                    out.assign(reinterpret_cast<const char*>(p1), first);
                    out.append(reinterpret_cast<const char*>(p2), n2);
                    store64(oTail, tail + first + n2);
                    store64(oBRead, load64(oBRead) + first + n2);
                    rs_.bytes_read += first + n2;
                    return true;
                }
            }
        }
        return false;
    }
    bool Reader::read_timeout(std::string& out, std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (read_nonblocking(out)) return true;
            std::this_thread::sleep_for(cfg_.idle_sleep_us);
            rs_.sleeps++;
        }
        return false;
    }

    bool Reader::alive() const {
        if (!base_) return false;
        uint64_t hb = load64(oHeartbeat);
        uint64_t now = now_nanos();
        auto grace = cfg_.heartbeat_grace.count() ? cfg_.heartbeat_grace.count() : (cfg_.heartbeat_interval.count() * 3);
        return now - hb <= (uint64_t)grace;
    }
    std::chrono::nanoseconds Reader::heartbeat_age() const {
        if (!base_)
            return (std::chrono::duration_values<std::chrono::nanoseconds>::max)();

        uint64_t hb = load64(oHeartbeat);
        uint64_t now = now_nanos();
        return std::chrono::nanoseconds(now - hb);
    }
    ReaderStats Reader::stats() const { return rs_; }
    std::string Reader::mapping_name() const {
        std::string out; out.reserve(map_name_w_.size());
        for (wchar_t c : map_name_w_) out.push_back(static_cast<char>(c & 0x7F));
        return out;
    }
} // namespace xmr
#endif