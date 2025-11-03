//#include "includes/hermes_core.h"
//
//// ---- Shared helpers ----
//
//// Big-endian double -> host double
//static inline double read_net_double(const void* p) {
//    uint64_t u = 0;
//    std::memcpy(&u, p, sizeof(u));
//    uint32_t hi = ntohl(static_cast<uint32_t>(u & 0xFFFFFFFFULL));
//    uint32_t lo = ntohl(static_cast<uint32_t>(u >> 32));
//    uint64_t be = (static_cast<uint64_t>(hi) << 32) | lo;
//    double d = 0.0;
//    std::memcpy(&d, &be, sizeof(d));
//    return d;
//}
//
//// Detect base: either header starts at 0 or has 8-byte preamble
//static inline int detect_header_base(const char* d, int L, uint16_t want) {
//    if (L < 60) return -1;
//    uint16_t tc0 = 0, tc8 = 0;
//    std::memcpy(&tc0, d + 10, 2); tc0 = ntohs(tc0);
//    std::memcpy(&tc8, d + 18, 2); tc8 = ntohs(tc8);
//    if (tc0 == want) return 0;
//    if (tc8 == want) return 8;
//    uint16_t n0 = 0, n8 = 0;
//    std::memcpy(&n0, d + 40, 2); n0 = ntohs(n0);
//    std::memcpy(&n8, d + 48, 2); n8 = ntohs(n8);
//    if (n0 > 0 && n0 < 1024) return 0;
//    if (n8 > 0 && n8 < 1024) return 8;
//    return -1;
//}
//
//// ---- 7208: MBP 5x5 ----
//void Handler7208::handle(const MessageView& mv, ConsoleSink& out,
//    InstrumentDirectory* /*instDir*/, const StrikeList& strikes) {
//    const char* d = mv.buf; const int L = mv.len;
//    const int BASE = detect_header_base(d, L, 7208);
//    if (BASE < 0) return;
//
//    uint16_t n = 0; std::memcpy(&n, d + BASE + 40, 2); n = ntohs(n);
//    if (n == 0) return;
//
//    static constexpr int REC_BYTES = 214;
//    const int REC0 = BASE + 42;
//
//    static constexpr int OFF_TOKEN = 0;
//    static constexpr int OFF_LTP = 12;
//    static constexpr int OFF_TIME = 26;
//    static constexpr int OFF_ATP = 30;
//
//    static constexpr int MBP_OFF = 44;
//    static constexpr int MBP_SIZE = 12;
//    static constexpr int MBP_CNT = 10;
//
//    static constexpr int OFF_TOT_BUY = 180;
//    static constexpr int OFF_TOT_SELL = 188;
//
//    for (uint16_t i = 0; i < n; ++i) {
//        const int rec = REC0 + i * REC_BYTES;
//        if (rec + REC_BYTES > L) break;
//
//        uint32_t token = 0, ltp = 0, atp = 0, t = 0;
//        std::memcpy(&token, d + rec + OFF_TOKEN, 4);
//        std::memcpy(&ltp, d + rec + OFF_LTP, 4);
//        std::memcpy(&t, d + rec + OFF_TIME, 4);
//        std::memcpy(&atp, d + rec + OFF_ATP, 4);
//
//        token = ntohl(token);
//        if (!strikes.contains((long)token)) continue;
//
//        ltp = ntohl(ltp);
//        atp = ntohl(atp);
//        t = ntohl(t);
//        const uint32_t unixTime = t + 315513000u;
//
//        float bidP[5] = { 0 }, askP[5] = { 0 };
//        int   bidQ[5] = { 0 }, askQ[5] = { 0 };
//
//        for (int k = 0; k < MBP_CNT; ++k) {
//            const char* e = d + rec + MBP_OFF + k * MBP_SIZE;
//            int32_t qty = 0, price = 0;
//            std::memcpy(&qty, e + 0, 4);
//            std::memcpy(&price, e + 4, 4);
//            qty = (int32_t)ntohl((uint32_t)qty);
//            price = (int32_t)ntohl((uint32_t)price);
//
//            if (k < 5) { bidQ[k] = qty; bidP[k] = price / 100.0f; }
//            else { int a = k - 5; askQ[a] = qty; askP[a] = price / 100.0f; }
//        }
//
//        const float bdpR = bidP[0];
//        const int   bdq = bidQ[0];
//        const float aspR = askP[0];
//        const int   asq = askQ[0];
//
//        const double buyQ = read_net_double(d + rec + OFF_TOT_BUY);
//        const double sellQ = read_net_double(d + rec + OFF_TOT_SELL);
//
//        const float ltpR = ltp / 100.0f;
//        const float atpR = atp / 100.0f;
//
//        std::ostringstream os;
//        os << token << ",7208,"
//            << floatToString(ltpR) << ','
//            << floatToString(atpR) << ','
//            << floatToString(bdpR) << ','
//            << bdq << ','
//            << floatToString(aspR) << ','
//            << asq << ','
//            << (long long)(buyQ + 0.5) << ','
//            << (long long)(sellQ + 0.5) << ','
//            << unixTime;
//
//        for (int l = 0; l < 5; ++l) os << ',' << floatToString(bidP[l]) << ',' << bidQ[l];
//        for (int l = 0; l < 5; ++l) os << ',' << floatToString(askP[l]) << ',' << askQ[l];
//
//        out.sendLine(os.str());
//    }
//}
//
//// ---- 7202: OI ticker (compact) ----
//void Handler7202::handle(const MessageView& mv, ConsoleSink& out,
//    InstrumentDirectory* /*instDir*/, const StrikeList& strikes) {
//    const char* d = mv.buf; const int L = mv.len;
//    const int BASE = detect_header_base(d, L, 7202);
//    if (BASE < 0) return;
//
//    uint16_t n = 0; std::memcpy(&n, d + BASE + 40, 2); n = ntohs(n);
//    if (n == 0) return;
//
//    static constexpr int REC_BYTES = 26;
//    const int REC0 = BASE + 42;
//
//    for (uint16_t i = 0; i < n; ++i) {
//        const int rec = REC0 + i * REC_BYTES;
//        if (rec + REC_BYTES > L) break;
//
//        uint32_t token = 0, oi = 0;
//        uint16_t mkt = 0;
//
//        std::memcpy(&token, d + rec + 0, 4);
//        std::memcpy(&mkt, d + rec + 4, 2);
//        std::memcpy(&oi, d + rec + 14, 4);
//
//        token = ntohl(token);
//        if (!strikes.contains((long)token)) continue;
//
//        mkt = ntohs(mkt);
//        oi = ntohl(oi);
//
//        std::ostringstream os;
//        os << token << ",7202," << mkt << "," << oi;
//        out.sendLine(os.str());
//    }
//}




#include "includes/hermes_core.h"

#include <cctype>
#include <string>
#include <sstream>
#include <algorithm>
#include <iomanip>

// ---- Shared helpers ----

// Big-endian double -> host double
static inline double read_net_double(const void* p) {
    uint64_t u = 0;
    std::memcpy(&u, p, sizeof(u));
    uint32_t hi = ntohl(static_cast<uint32_t>(u & 0xFFFFFFFFULL));
    uint32_t lo = ntohl(static_cast<uint32_t>(u >> 32));
    uint64_t be = (static_cast<uint64_t>(hi) << 32) | lo;
    double d = 0.0;
    std::memcpy(&d, &be, sizeof(d));
    return d;
}

// Detect base: either header starts at 0 or has 8-byte preamble
static inline int detect_header_base(const char* d, int L, uint16_t want) {
    if (L < 60) return -1;
    uint16_t tc0 = 0, tc8 = 0;
    std::memcpy(&tc0, d + 10, 2); tc0 = ntohs(tc0);
    std::memcpy(&tc8, d + 18, 2); tc8 = ntohs(tc8);
    if (tc0 == want) return 0;
    if (tc8 == want) return 8;
    uint16_t n0 = 0, n8 = 0;
    std::memcpy(&n0, d + 40, 2); n0 = ntohs(n0);
    std::memcpy(&n8, d + 48, 2); n8 = ntohs(n8);
    if (n0 > 0 && n0 < 1024) return 0;
    if (n8 > 0 && n8 < 1024) return 8;
    return -1;
}

// ---- 7208: MBP 5x5 ----
void Handler7208::handle(const MessageView& mv, ConsoleSink& out,
    InstrumentDirectory* /*instDir*/, const StrikeList& strikes) {
    const char* d = mv.buf; const int L = mv.len;
    const int BASE = detect_header_base(d, L, 7208);
    if (BASE < 0) return;

    uint16_t n = 0; std::memcpy(&n, d + BASE + 40, 2); n = ntohs(n);
    if (n == 0) return;

    static constexpr int REC_BYTES = 214;
    const int REC0 = BASE + 42;

    static constexpr int OFF_TOKEN = 0;
    static constexpr int OFF_LTP = 12;
    static constexpr int OFF_TIME = 26;
    static constexpr int OFF_ATP = 30;

    static constexpr int MBP_OFF = 44;
    static constexpr int MBP_SIZE = 12;
    static constexpr int MBP_CNT = 10;

    static constexpr int OFF_TOT_BUY = 180;
    static constexpr int OFF_TOT_SELL = 188;

    for (uint16_t i = 0; i < n; ++i) {
        const int rec = REC0 + i * REC_BYTES;
        if (rec + REC_BYTES > L) break;

        uint32_t token = 0, ltp = 0, atp = 0, t = 0;
        std::memcpy(&token, d + rec + OFF_TOKEN, 4);
        std::memcpy(&ltp, d + rec + OFF_LTP, 4);
        std::memcpy(&t, d + rec + OFF_TIME, 4);
        std::memcpy(&atp, d + rec + OFF_ATP, 4);

        token = ntohl(token);
        if (!strikes.contains((long)token)) continue;

        ltp = ntohl(ltp);
        atp = ntohl(atp);
        t = ntohl(t);
        const uint32_t unixTime = t + 315513000u;

        float bidP[5] = { 0 }, askP[5] = { 0 };
        int   bidQ[5] = { 0 }, askQ[5] = { 0 };

        for (int k = 0; k < MBP_CNT; ++k) {
            const char* e = d + rec + MBP_OFF + k * MBP_SIZE;
            int32_t qty = 0, price = 0;
            std::memcpy(&qty, e + 0, 4);
            std::memcpy(&price, e + 4, 4);
            qty = (int32_t)ntohl((uint32_t)qty);
            price = (int32_t)ntohl((uint32_t)price);

            if (k < 5) { bidQ[k] = qty; bidP[k] = price / 100.0f; }
            else { int a = k - 5; askQ[a] = qty; askP[a] = price / 100.0f; }
        }

        const float bdpR = bidP[0];
        const int   bdq = bidQ[0];
        const float aspR = askP[0];
        const int   asq = askQ[0];

        const double buyQ = read_net_double(d + rec + OFF_TOT_BUY);
        const double sellQ = read_net_double(d + rec + OFF_TOT_SELL);

        const float ltpR = ltp / 100.0f;
        const float atpR = atp / 100.0f;

        std::ostringstream os;
        os << token << ",7208,"
            << floatToString(ltpR) << ','
            << floatToString(atpR) << ','
            << floatToString(bdpR) << ','
            << bdq << ','
            << floatToString(aspR) << ','
            << asq << ','
            << (long long)(buyQ + 0.5) << ','
            << (long long)(sellQ + 0.5) << ','
            << unixTime;

        for (int l = 0; l < 5; ++l) os << ',' << floatToString(bidP[l]) << ',' << bidQ[l];
        for (int l = 0; l < 5; ++l) os << ',' << floatToString(askP[l]) << ',' << askQ[l];

        out.sendLine(os.str());
    }
}

// ---- 7202: OI ticker (compact) ----
void Handler7202::handle(const MessageView& mv, ConsoleSink& out,
    InstrumentDirectory* /*instDir*/, const StrikeList& strikes) {
    const char* d = mv.buf; const int L = mv.len;
    const int BASE = detect_header_base(d, L, 7202);
    if (BASE < 0) return;

    uint16_t n = 0; std::memcpy(&n, d + BASE + 40, 2); n = ntohs(n);
    if (n == 0) return;

    static constexpr int REC_BYTES = 26;
    const int REC0 = BASE + 42;

    for (uint16_t i = 0; i < n; ++i) {
        const int rec = REC0 + i * REC_BYTES;
        if (rec + REC_BYTES > L) break;

        uint32_t token = 0, oi = 0;
        uint16_t mkt = 0;

        std::memcpy(&token, d + rec + 0, 4);
        std::memcpy(&mkt, d + rec + 4, 2);
        std::memcpy(&oi, d + rec + 14, 4);

        token = ntohl(token);
        if (!strikes.contains((long)token)) continue;

        mkt = ntohs(mkt);
        oi = ntohl(oi);

        std::ostringstream os;
        os << token << ",7202," << mkt << "," << oi;
        out.sendLine(os.str());
    }
}

// ---- CM handler helpers (for CT and PN payload parsing) ----

// trim helpers
static inline std::string cm_str_trim(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// extract first run of digits as token
static inline long cm_extract_token_from_payload(const char* data, int len) {
    int i = 0;
    while (i < len) {
        while (i < len && !std::isdigit(static_cast<unsigned char>(data[i]))) ++i;
        int start = i;
        while (i < len && std::isdigit(static_cast<unsigned char>(data[i]))) ++i;
        int end = i;
        if (end > start) {
            try {
                std::string num(data + start, data + end);
                return std::stol(num);
            }
            catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

// extract first alnum run that contains a letter (likely symbol)
static inline std::string cm_extract_symbol_from_payload(const char* data, int len) {
    int i = 0;
    while (i < len) {
        while (i < len && !std::isalnum(static_cast<unsigned char>(data[i]))) ++i;
        int start = i;
        while (i < len && std::isalnum(static_cast<unsigned char>(data[i]))) ++i;
        int end = i;
        if (end > start) {
            std::string cand(data + start, data + end);
            bool hasAlpha = false;
            for (char c : cand) if (std::isalpha(static_cast<unsigned char>(c))) { hasAlpha = true; break; }
            if (hasAlpha) return cm_str_trim(cand);
            // otherwise keep scanning
        }
    }
    return std::string("<unknown>");
}

// make payload printable snippet
static inline std::string cm_payload_to_printable(const char* data, int len, size_t maxlen = 120) {
    std::string out;
    out.reserve(std::min<size_t>(len, maxlen));
    for (int i = 0; i < len && out.size() < maxlen; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c >= 32 && c <= 126) out.push_back(static_cast<char>(c));
        else out.push_back('.');
    }
    return cm_str_trim(out);
}

// pack two ASCII chars into a big-endian uint16_t
static inline uint16_t cm_pack_code(char a, char b) {
    return static_cast<uint16_t>((static_cast<uint8_t>(a) << 8) | static_cast<uint8_t>(b));
}

// ---- HandlerCM_CT ----
HandlerCM_CT::HandlerCM_CT() {
    codes_.push_back(cm_pack_code('C', 'T')); // "CT"
}

void HandlerCM_CT::handle(const MessageView& mv, ConsoleSink& out,
    InstrumentDirectory* /*instDir*/, const StrikeList& /*strikes*/) {

    const char* data = mv.buf;
    int len = mv.len;

    long token = cm_extract_token_from_payload(data, len);
    std::string symbol = cm_extract_symbol_from_payload(data, len);

    std::string payload_print = cm_payload_to_printable(data, len, 200);

    // Emit CSV: token,7208,symbol,<payload_preview>
    std::ostringstream oss;
    oss << token << "," << 7208 << "," << symbol << "," << payload_print;
    out.sendLine(oss.str());
}

// ---- HandlerCM_PN ----
HandlerCM_PN::HandlerCM_PN() {
    codes_.push_back(cm_pack_code('P', 'N')); // "PN"
}

void HandlerCM_PN::handle(const MessageView& mv, ConsoleSink& out,
    InstrumentDirectory* instDir, const StrikeList& strikes) {

    const char* data = mv.buf;
    int len = mv.len;

    long token = cm_extract_token_from_payload(data, len);
    std::string symbol = cm_extract_symbol_from_payload(data, len);

    std::string payload_print = cm_payload_to_printable(data, len, 200);

    // Emit CSV: token,7202,symbol,<payload_preview>
    std::ostringstream oss;
    oss << token << "," << 7202 << "," << symbol << "," << payload_print;
    out.sendLine(oss.str());

    // Optionally update instrument directory if you have API
    (void)instDir; (void)strikes;
}
