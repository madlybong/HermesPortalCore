#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <system_error>
#include <functional>

namespace xmr {

    // ---- ABI ----
    static constexpr uint64_t kMagic64 = 0x4845524D53524E47ULL; // "HERMSRNG"
    static constexpr uint32_t kAbiVersion = 2;

    static constexpr size_t oMagic = 0;
    static constexpr size_t oABIVer = 8;
    static constexpr size_t oFlags = 12;
    static constexpr size_t oCapacity = 16;
    static constexpr size_t oHeartbeat = 24;

    static constexpr size_t oHead = 128;
    static constexpr size_t oTail = oHead + 64;
    static constexpr size_t oDrops = oTail + 64;
    static constexpr size_t oOver = oDrops + 8;
    static constexpr size_t oSleepC = oOver + 8;
    static constexpr size_t oBWrite = oSleepC + 8;
    static constexpr size_t oBRead = oBWrite + 8;
    static constexpr size_t oHdrEnd = oBRead + 8 + 56; // 352

    // ---- enums ----
    enum class DropPolicy { DropNewest, DropOldest };
    enum class FrameMode { Newline, LengthPrefix };
    enum class EventType { Opened, Closed, Reconnected, Dead, Error };

    // ---- stats ----
    struct WriterStats {
        uint64_t drops_newest = 0;
        uint64_t drops_oldest = 0;
        uint64_t oversize = 0;
        uint64_t bytes_written = 0;
    };
    struct ReaderStats {
        uint64_t sleeps = 0;
        uint64_t bytes_read = 0;
    };

    // ---- config ----
    struct Config {
        std::string name, token;
#ifdef _WIN32
        std::wstring nameW, tokenW;
#endif

        uint64_t capacity_bytes = 4ull << 20;

        DropPolicy drop_policy = DropPolicy::DropNewest;
        FrameMode  frame_mode = FrameMode::Newline;

        std::chrono::nanoseconds heartbeat_interval{ 500'000'000 };
        std::chrono::nanoseconds heartbeat_grace{ 0 };
        std::chrono::microseconds idle_sleep_us{ 200 };

        std::function<void(EventType, const std::string&)> on_event;
    };

    // ---- Writer ----
    class Writer {
    public:
        Writer() = default; ~Writer();
        Writer(const Writer&) = delete; Writer& operator=(const Writer&) = delete;
        Writer(Writer&&) noexcept;      Writer& operator=(Writer&&) noexcept;

        void open(const Config& cfg);
        void close() noexcept;
        bool is_open() const noexcept { return base_ != nullptr; }

        bool write(const std::string& s);
        bool write(const char* s);
        bool write(const uint8_t* data, size_t len);

        void heartbeat();
        WriterStats stats() const;
        std::string mapping_name() const;

    private:
        static uint64_t now_nanos();
#ifdef _WIN32
        static std::wstring utf8_to_wide(const std::string& s);
        static std::wstring compose_map_name_w(const std::wstring& name, const std::wstring& token);
#endif
        bool validate_header(uint64_t& outCap) const;
        void init_header(uint64_t cap) const;
        void start_hb(std::chrono::nanoseconds iv);
        void stop_hb();

        uint64_t load64(size_t off) const; void store64(size_t off, uint64_t v) const;
        uint32_t load32(size_t off) const; void store32(size_t off, uint32_t v) const;

    private:
        void* base_ = nullptr;
        uint8_t* data_ = nullptr;
        uint64_t cap_ = 0;
        size_t   size_ = 0;

#ifdef _WIN32
        void* hmap_ = nullptr;
        std::wstring map_name_w_;
#endif

        std::thread hb_;
        std::atomic<bool> hb_stop_{ true };

        Config cfg_;
    };

    // ---- Reader ----
    class Reader {
    public:
        Reader() = default; ~Reader();
        Reader(const Reader&) = delete; Reader& operator=(const Reader&) = delete;
        Reader(Reader&&) noexcept;      Reader& operator=(Reader&&) noexcept;

        void open(const Config& cfg);
        void close() noexcept;
        bool is_open() const noexcept { return base_ != nullptr; }

        bool read(std::string& out);
        bool read_nonblocking(std::string& out);
        bool read_timeout(std::string& out, std::chrono::milliseconds timeout);

        bool alive() const;
        std::chrono::nanoseconds heartbeat_age() const;

        ReaderStats stats() const;
        std::string mapping_name() const;

    private:
        static uint64_t now_nanos();
#ifdef _WIN32
        static std::wstring utf8_to_wide(const std::string& s);
        static std::wstring compose_map_name_w(const std::wstring& name, const std::wstring& token);
#endif
        bool validate_header(uint64_t& outCap) const;

        uint64_t load64(size_t off) const; void store64(size_t off, uint64_t v) const;
        uint32_t load32(size_t off) const;

    private:
        void* base_ = nullptr;
        uint8_t* data_ = nullptr;
        uint64_t cap_ = 0;

#ifdef _WIN32
        void* hmap_ = nullptr;
        std::wstring map_name_w_;
#endif

        mutable ReaderStats rs_{};
        Config cfg_;
    };

} // namespace xmr
