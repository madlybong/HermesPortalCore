#include "includes/hermes_core.h"

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
