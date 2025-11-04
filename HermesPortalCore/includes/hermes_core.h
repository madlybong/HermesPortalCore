#pragma once
// HermesPortal - core umbrella header (updated for CM ascii iCode handlers)

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <cstring>
#include <algorithm>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#endif

extern bool g_cm_debug;

// ---------------- Utilities ----------------
inline std::string floatToString(float value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

// helper to form big-endian 2-char iCode value
constexpr uint16_t MAKE_ICODE(char a, char b) {
    return static_cast<uint16_t>((static_cast<uint8_t>(a) << 8) | static_cast<uint8_t>(b));
}

// common CM codes (examples)
constexpr uint16_t ICODE_CT = MAKE_ICODE('C', 'T'); // Touchline (Level 1)
constexpr uint16_t ICODE_PN = MAKE_ICODE('P', 'N'); // 20-depth (Level 3)

// ------------- Common message view ----------
struct MessageView { const char* buf; int len; };

// ------------- Console/Pluggable sink -------------
class ConsoleSink {
public:
    // If set, all lines go to this writer (e.g., SHM). Should return true on success.
    static inline void setExternal(std::function<bool(const std::string&)> fn) { s_extWriter = std::move(fn); }
    static inline void setConsoleMirror(bool on) { s_consoleMirror = on; }
    static inline bool getConsoleMirror() { return s_consoleMirror; }

    inline void sendLine(const std::string& line) {
        if (s_extWriter) {
            (void)s_extWriter(line);    // ignore failure; DROP-NEWEST expected policy
            return;
        }
        if (s_consoleMirror) std::cout << line << "\n";
    }
private:
    inline static bool s_consoleMirror = true; // C++17 inline var
    inline static std::function<bool(const std::string&)> s_extWriter{}; // null => console
};

// ------------- Strike filter ----------------
class StrikeList {
public:
    bool loadFromArgs(const std::string& csv) {
        tokens_.clear();
        std::stringstream ss(csv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            try { tokens_.insert(std::stol(item)); }
            catch (...) {}
        }
        return !tokens_.empty();
    }
    bool contains(long token) const {
        return tokens_.empty() ? true : (tokens_.count(token) > 0);
    }
private:
    std::set<long> tokens_;
};

// ------------- Instrument directory (stub) --
struct InstrumentDirectory {};

// ------------- Handler interface -------------
class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;
    virtual const std::vector<uint16_t>& transcodes() const = 0;
    virtual void handle(const MessageView& mv,
        ConsoleSink& out,
        InstrumentDirectory* instDir,
        const StrikeList& strikes) = 0;
};

// ------------- Dispatcher --------------------
class PacketDispatcher {
public:
    void registerHandler(std::unique_ptr<IMessageHandler> h) {
        for (auto c : h->transcodes()) handlers_[c] = std::move(h);
    }
    IMessageHandler* find(uint16_t code) const {
        auto it = handlers_.find(code);
        return (it == handlers_.end()) ? nullptr : it->second.get();
    }
private:
    std::map<uint16_t, std::unique_ptr<IMessageHandler>> handlers_;
};

// ------------- LZO (decl only; impl in lzo_helper.cpp) ---
namespace Lzo {
    bool Init();
    bool Decompress(const unsigned char* in, std::size_t inLen,
        unsigned char* out, std::size_t outCap, std::size_t& outLen);
}

// ------------- PacketParser (decl; impl in parser.cpp) ---
class PacketParser {
public:
    explicit PacketParser(PacketDispatcher& d) : disp_(d) {}
    void parse(const char* buf, int len, ConsoleSink& out,
        InstrumentDirectory* instDir, const StrikeList& strikes);
    void parseCM(const uint8_t* buf, size_t len, ConsoleSink& out,
        InstrumentDirectory* instDir, const StrikeList& strikes);
private:
    PacketDispatcher& disp_;
    // working buffer for CM decompression
    std::vector<unsigned char> cm_decomp_buf_;
};

// ------------- Schemas (decl; impl in schemas.cpp) -------
void PrintSchemas();

// ------------- Handlers (decl; impl in handlers_market.cpp) ---------
class Handler7208 : public IMessageHandler {
public:
    Handler7208() : codes_{ 7208 } {}
    const std::vector<uint16_t>& transcodes() const override { return codes_; }
    void handle(const MessageView& mv, ConsoleSink& out,
        InstrumentDirectory* instDir, const StrikeList& strikes) override;
private:
    std::vector<uint16_t> codes_;
};

class Handler7202 : public IMessageHandler {
public:
    Handler7202() : codes_{ 7202 } {}
    const std::vector<uint16_t>& transcodes() const override { return codes_; }
    void handle(const MessageView& mv, ConsoleSink& out,
        InstrumentDirectory* instDir, const StrikeList& strikes) override;
private:
    std::vector<uint16_t> codes_;
};

// New CM handlers declarations
class HandlerCM_CT : public IMessageHandler {
public:
    HandlerCM_CT() : codes_{ ICODE_CT } {}
    const std::vector<uint16_t>& transcodes() const override { return codes_; }
    void handle(const MessageView& mv, ConsoleSink& out,
        InstrumentDirectory* instDir, const StrikeList& strikes) override;
private:
    std::vector<uint16_t> codes_;
};

class HandlerCM_PN : public IMessageHandler {
public:
    HandlerCM_PN() : codes_{ ICODE_PN } {}
    const std::vector<uint16_t>& transcodes() const override { return codes_; }
    void handle(const MessageView& mv, ConsoleSink& out,
        InstrumentDirectory* instDir, const StrikeList& strikes) override;
private:
    std::vector<uint16_t> codes_;
};
