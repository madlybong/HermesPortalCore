#include "includes/hermes_core.h"

static uint16_t peek_code(const char* d, int L, int base) {
    if (L < base + 20) return 0;
    uint16_t code = 0; std::memcpy(&code, d + base + 10, 2); return ntohs(code);
}

void PacketParser::parse(const char* buf, int len, ConsoleSink& out,
    InstrumentDirectory* instDir, const StrikeList& strikes) {
    if (len < 4) return;

    uint16_t NOP = 0;
    std::memcpy(&NOP, buf + 2, 2);
    NOP = ntohs(NOP);
    int off = 4;

    static thread_local unsigned char dcmp[65536];

    for (int i = 0; i < NOP; ++i) {
        if (off + 2 > len) break;

        uint16_t compLen = 0;
        std::memcpy(&compLen, buf + off, 2);
        off += 2;
        compLen = ntohs(compLen);
        if (compLen == 0 || off + compLen > len) { off += compLen; continue; }

        const unsigned char* src = reinterpret_cast<const unsigned char*>(buf + off);
        off += compLen;

        std::size_t outLen = 0;
        if (!Lzo::Decompress(src, compLen, dcmp, sizeof(dcmp), outLen)) {
            continue;
        }
        const char* dst = reinterpret_cast<const char*>(dcmp);
        int dst_len = static_cast<int>(outLen);

        // Try base 0 or 8
        int base = -1;
        uint16_t code0 = peek_code(dst, dst_len, 0);
        uint16_t code8 = peek_code(dst, dst_len, 8);
        int code = 0;

        if (code0) { base = 0; code = code0; }
        if (code8 && (code == 0 || code8 == 7208 || code8 == 7202)) { base = 8; code = code8; }

        if (base >= 0 && code != 0) {
            MessageView mv{ dst, dst_len };
            if (auto* h = disp_.find(static_cast<uint16_t>(code))) {
                h->handle(mv, out, instDir, strikes);
            }
        }
    }
}
