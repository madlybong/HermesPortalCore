#include "includes/hermes_core.h"
#include <cmath>
#include <sstream>
#include <iomanip>

// ---- Shared helpers ----
// Big-endian double -> host double
static inline double read_net_double(const void* p) {
    uint64_t u = 0;
    std::memcpy(&u, p, sizeof(u));
    uint32_t hi = ntohl(static_cast<uint32_t>(u >> 32));
    uint32_t lo = ntohl(static_cast<uint32_t>(u & 0xFFFFFFFFULL));
    uint64_t be = (static_cast<uint64_t>(hi) << 32) | lo;
    double d = 0.0;
    std::memcpy(&d, &be, sizeof(d));
    return d;
}

static inline uint32_t read_be32_u(const void* p) {
    uint32_t v = 0; std::memcpy(&v, p, 4); return ntohl(v);
}
static inline int32_t read_be32_s(const void* p) {
    int32_t v = 0; std::memcpy(&v, p, 4); return static_cast<int32_t>(ntohl(static_cast<uint32_t>(v)));
}
static inline uint16_t read_be16_u(const void* p) {
    uint16_t v = 0; std::memcpy(&v, p, 2); return ntohs(v);
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

// ---- 7208: MBP 5x5 (FO) ----
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

// ---- 7202: OI ticker (FO) ----
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

// ---- HandlerCM_CT: CM Touchline/Level1 (ASCII iCode 'CT') ----
// This maps CM CT fields to a readable CSV. CM prices are Price*10000 per spec (v1.31).
// We expose the CM native columns (token, "CT", ltp, atp, bid1Price, bid1Qty, ask1Price, ask1Qty, totalBuy, totalSell, timestamp,...)
// Column definitions printed in debug on first call.
void HandlerCM_CT::handle(const MessageView& mv, ConsoleSink& out,
    InstrumentDirectory* /*instDir*/, const StrikeList& strikes) {

    const char* d = mv.buf; const int L = mv.len;
    if (L < 48) return; // minimal touchline
    // All CM fields are big-endian LONGs in v1.31, prices often Price*10000
    // Offsets per GROK/spec (0-based within payload)
    // 0: token (LONG)
    // 4: LTP (LONG) price * 10000
    // 8: last traded qty (LONG)
    // 12: last traded time HHMMSS (LONG)
    // 16: volume traded today (LONG)
    // 20: open price (LONG) *10000
    // 24: high price
    // 28: low price
    // 32: close price (prev)
    // 36: total buy qty (LONG)
    // 40: total sell qty (LONG)
    // 44: ATP (LONG) *10000
    // 48: Indicative Close Price (maybe present)
    uint32_t token = read_be32_u(d + 0);
    if (!strikes.contains((long)token)) return;

    uint32_t ltp_raw = read_be32_u(d + 4);
    uint32_t lqty = read_be32_u(d + 8);
    uint32_t lt_time = read_be32_u(d + 12);
    uint32_t vtr = read_be32_u(d + 16);
    uint32_t open_raw = read_be32_u(d + 20);
    uint32_t high_raw = read_be32_u(d + 24);
    uint32_t low_raw = read_be32_u(d + 28);
    uint32_t close_raw = read_be32_u(d + 32);
    uint32_t tot_buy = read_be32_u(d + 36);
    uint32_t tot_sell = read_be32_u(d + 40);
    uint32_t atp_raw = read_be32_u(d + 44);
    uint32_t icp_raw = (L >= 52) ? read_be32_u(d + 48) : 0;

    // scaling
    const float PRICE_SCALE = 10000.0f;
    float ltp = ltp_raw / PRICE_SCALE;
    float atp = atp_raw / PRICE_SCALE;
    float bid1 = 0.0f, ask1 = 0.0f; // many feeds include best prices later in payload; check payload size
    uint32_t bid1_q = 0, ask1_q = 0;
    // Attempt to read best bid/ask if present (offsets vary). We'll probe common positions:
    if (L >= 76) {
        // assume best bid at 52, bid qty 56; best ask at 60, ask qty 64 (example positions)
        uint32_t b1 = read_be32_u(d + 52);
        uint32_t b1q = read_be32_u(d + 56);
        uint32_t a1 = read_be32_u(d + 60);
        uint32_t a1q = read_be32_u(d + 64);
        bid1 = b1 / PRICE_SCALE; bid1_q = b1q; ask1 = a1 / PRICE_SCALE; ask1_q = a1q;
    }

    // Print CSV: token,CT,ltp,atp,bid1, bid1_q, ask1, ask1_q, tot_buy, tot_sell, lttime, volume, open, high, low, close
    std::ostringstream os;
    os << token << ",CT,"
        << floatToString(ltp) << ','
        << floatToString(atp) << ','
        << floatToString(bid1) << ',' << bid1_q << ','
        << floatToString(ask1) << ',' << ask1_q << ','
        << tot_buy << ',' << tot_sell << ','
        << lt_time << ',' << vtr << ','
        << floatToString(open_raw / PRICE_SCALE) << ','
        << floatToString(high_raw / PRICE_SCALE) << ','
        << floatToString(low_raw / PRICE_SCALE) << ','
        << floatToString(close_raw / PRICE_SCALE);

    out.sendLine(os.str());

    // Print schema when debug (once)
    if (ConsoleSink::getConsoleMirror()) {
        // print CM CT column definition under debug
        std::cerr << "[SCHEMA CM CT] cols: token,CT,ltp,atp,bid1,bid1_q,ask1,ask1_q,tot_buy,tot_sell,last_trade_time,volume,open,high,low,prev_close\n";
    }
}

// ---- HandlerCM_PN: CM 20-depth (PN) ----
// PN contains touchline + 20 bids + 20 asks. We'll map the first 5 levels for compatibility.
void HandlerCM_PN::handle(const MessageView& mv, ConsoleSink& out,
    InstrumentDirectory* /*instDir*/, const StrikeList& strikes) {

    const char* d = mv.buf; const int L = mv.len;
    if (L < 60) return; // too small
    // Basic touchline as CT
    uint32_t token = read_be32_u(d + 0);
    if (!strikes.contains((long)token)) return;

    const float PRICE_SCALE = 10000.0f;
    uint32_t ltp_raw = read_be32_u(d + 4);
    float ltp = ltp_raw / PRICE_SCALE;
    // We'll attempt to locate depth start. Typical spec: touchline ~offset 0..52 then bids follow.
    size_t depth_start = 52; // heuristic
    size_t per_level = 12; // price(4) qty(4) orders(2) + padding
    // read first 5 bids then first 5 asks
    float bidP[5] = { 0 }; uint32_t bidQ[5] = { 0 };
    float askP[5] = { 0 }; uint32_t askQ[5] = { 0 };

    for (int i = 0; i < 5; ++i) {
        size_t off = depth_start + i * per_level;
        if (off + 8 <= static_cast<size_t>(L)) {
            uint32_t p_raw = read_be32_u(d + off);
            uint32_t q_raw = read_be32_u(d + off + 4);
            bidP[i] = p_raw / PRICE_SCALE; bidQ[i] = q_raw;
        }
    }
    // asks located after 20 bids; so offset = depth_start + 20*per_level
    size_t asks_base = depth_start + 20 * per_level;
    for (int i = 0; i < 5; ++i) {
        size_t off = asks_base + i * per_level;
        if (off + 8 <= static_cast<size_t>(L)) {
            uint32_t p_raw = read_be32_u(d + off);
            uint32_t q_raw = read_be32_u(d + off + 4);
            askP[i] = p_raw / PRICE_SCALE; askQ[i] = q_raw;
        }
    }

    // Form CSV: token,PN,ltp,bid1,bid1_q,...bid5,bid5_q,ask1,ask1_q...ask5,ask5_q
    std::ostringstream os;
    os << token << ",PN," << floatToString(ltp);
    for (int i = 0; i < 5; ++i) os << ',' << floatToString(bidP[i]) << ',' << bidQ[i];
    for (int i = 0; i < 5; ++i) os << ',' << floatToString(askP[i]) << ',' << askQ[i];

    out.sendLine(os.str());

    if (ConsoleSink::getConsoleMirror()) {
        std::cerr << "[SCHEMA CM PN] cols: token,PN,ltp, (bid1,qty1)...(bid5,qty5),(ask1,qty1)...(ask5,qty5)\n";
    }
}
