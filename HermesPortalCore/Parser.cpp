//#include "includes/hermes_core.h"
//
//static uint16_t peek_code(const char* d, int L, int base) {
//    if (L < base + 20) return 0;
//    uint16_t code = 0; std::memcpy(&code, d + base + 10, 2); return ntohs(code);
//}
//
//void PacketParser::parse(const char* buf, int len, ConsoleSink& out,
//    InstrumentDirectory* instDir, const StrikeList& strikes) {
//    if (len < 4) return;
//
//    uint16_t NOP = 0;
//    std::memcpy(&NOP, buf + 2, 2);
//    NOP = ntohs(NOP);
//    int off = 4;
//
//    static thread_local unsigned char dcmp[65536];
//
//    for (int i = 0; i < NOP; ++i) {
//        if (off + 2 > len) break;
//
//        uint16_t compLen = 0;
//        std::memcpy(&compLen, buf + off, 2);
//        off += 2;
//        compLen = ntohs(compLen);
//        if (compLen == 0 || off + compLen > len) { off += compLen; continue; }
//
//        const unsigned char* src = reinterpret_cast<const unsigned char*>(buf + off);
//        off += compLen;
//
//        std::size_t outLen = 0;
//        if (!Lzo::Decompress(src, compLen, dcmp, sizeof(dcmp), outLen)) {
//            continue;
//        }
//        const char* dst = reinterpret_cast<const char*>(dcmp);
//        int dst_len = static_cast<int>(outLen);
//
//        // Try base 0 or 8
//        int base = -1;
//        uint16_t code0 = peek_code(dst, dst_len, 0);
//        uint16_t code8 = peek_code(dst, dst_len, 8);
//        int code = 0;
//
//        if (code0) { base = 0; code = code0; }
//        if (code8 && (code == 0 || code8 == 7208 || code8 == 7202)) { base = 8; code = code8; }
//
//        if (base >= 0 && code != 0) {
//            MessageView mv{ dst, dst_len };
//            if (auto* h = disp_.find(static_cast<uint16_t>(code))) {
//                h->handle(mv, out, instDir, strikes);
//            }
//        }
//    }
//}



// Parser.cpp
// Packet parsing for FO/CM. Includes robust CM concatenated-LZO handling.

#include "includes/hermes_core.h"

#include <cstring>
#include <sstream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <algorithm>
#include <iostream>

extern bool g_cm_debug; // declared in header, defined in HermesPortalCore.cpp

// ---------- helpers ----------
static uint16_t peek_code(const char* d, int L, int base) {
    if (L < base + 20) return 0;
    uint16_t code = 0; std::memcpy(&code, d + base + 10, 2); return ntohs(code);
}

static inline uint16_t read_be16(const uint8_t* p) {
    uint16_t v = 0; std::memcpy(&v, p, 2); return ntohs(v);
}
static inline uint32_t read_be32(const uint8_t* p) {
    uint32_t v = 0; std::memcpy(&v, p, 4); return ntohl(v);
}

// ---------- original parse (FO-style, decompress per compressed chunk) ----------
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

// ---------- CM parsing: concatenated LZO-aware parser ----------
// Note: This function expects to be called from HermesPortalCore when feed = CM.
// Signature uses uint8_t* and size_t to be clear.
void PacketParser::parseCM(const uint8_t* buf, size_t len, ConsoleSink& out,
    InstrumentDirectory* instDir, const StrikeList& strikes) {

    if (!buf || len < 5) return;

    size_t pos = 0;
    uint8_t cCompOrNot = buf[pos]; pos += 1;
    if (pos + 4 > len) {
        std::cerr << "[CM parser] truncated batch header\n";
        return;
    }
    uint16_t nDataSize = read_be16(buf + pos); pos += 2;
    uint16_t iNoOfPackets = read_be16(buf + pos); pos += 2;

    const uint8_t* payload_ptr = buf + pos;
    size_t payload_len = 0;
    if (nDataSize > 0) payload_len = std::min<size_t>(nDataSize, (len - pos));
    else payload_len = len - pos;

    // header summary
    {
        std::ostringstream dbg;
        dbg << "[CM parser] batch header cCompOrNot=0x" << std::hex << (int)cCompOrNot << std::dec
            << " nDataSize=" << nDataSize << " iNoOfPackets=" << iNoOfPackets
            << " payload_len=" << payload_len << " preview=";
        for (size_t i = 0; i < std::min<size_t>(payload_len, 24); ++i)
            dbg << std::hex << std::setw(2) << std::setfill('0') << (int)payload_ptr[i];
        dbg << std::dec;
        std::cerr << dbg.str() << "\n";
    }

    if (payload_len == 0) {
        std::cerr << "[CM parser] empty payload\n";
        return;
    }

    bool is_uncompressed = (cCompOrNot == 'N' || cCompOrNot == 'n' || cCompOrNot == 0);
    bool is_compressed = !is_uncompressed;

    if (!is_compressed) {
        // parse payload directly (unchanged)
        const uint8_t* p = payload_ptr;
        size_t rem = payload_len;
        const size_t INFO_HEADER_SZ = 2 + 2 + 4;
        const size_t INFO_TRAILER_SZ = 2 + 1;
        for (uint16_t pkt_idx = 0; pkt_idx < iNoOfPackets; ++pkt_idx) {
            if (rem < INFO_HEADER_SZ + INFO_TRAILER_SZ) break;
            uint16_t iCode_be = read_be16(p); p += 2; rem -= 2;
            uint16_t iLen_be = read_be16(p); p += 2; rem -= 2;
            uint32_t seqNo_be = read_be32(p); p += 4; rem -= 4;

            uint16_t iLen = iLen_be;
            if (iLen < INFO_HEADER_SZ + INFO_TRAILER_SZ) continue;

            size_t payload_bytes = static_cast<size_t>(iLen) - INFO_HEADER_SZ - INFO_TRAILER_SZ;
            if (rem < payload_bytes + INFO_TRAILER_SZ) break;

            const uint8_t* payload = p;
            p += payload_bytes; rem -= payload_bytes;

            uint16_t recv_checksum = 0; std::memcpy(&recv_checksum, p, 2); recv_checksum = ntohs(recv_checksum); p += 2; rem -= 2;
            uint8_t cEOT = *p; p += 1; rem -= 1; (void)cEOT;

            if (auto* h = disp_.find(iCode_be)) {
                MessageView mv{ reinterpret_cast<const char*>(payload), static_cast<int>(payload_bytes) };
                h->handle(mv, out, instDir, strikes);
            }
        }
        return;
    }

    // Compressed: find markers 0x1A 0x04 and try decompressing at the marker with full remaining bytes.
    // We'll build a full_decomp buffer by appending decompressed sub-blocks.
    std::vector<size_t> markers;
    markers.reserve(256);
    for (size_t i = 0; i + 1 < payload_len; ++i) {
        if (payload_ptr[i] == 0x1A && payload_ptr[i + 1] == 0x04) markers.push_back(i);
    }

    std::vector<unsigned char> full_decomp;
    std::vector<unsigned char> workbuf; // temp buffer for LZO output
    full_decomp.reserve(payload_len * 4);

    auto dump_payload_for_analysis = [&](const uint8_t* data, size_t len, const char* path = "cm_failed_payload.bin") {
        std::ofstream ofs(path, std::ios::binary);
        if (ofs) {
            ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
            ofs.close();
            std::cerr << "[CM DUMP] wrote " << len << " bytes to " << path << "\n";
        }
        else {
            std::cerr << "[CM DUMP] failed to write " << path << "\n";
        }
        // print hex preview (first 256 bytes)
        size_t hp = std::min<size_t>(256, len);
        std::ostringstream ss;
        ss << "[CM DUMP HEX] first " << hp << " bytes:\n";
        for (size_t i = 0; i < hp; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
            if ((i & 0x0F) == 0x0F) ss << '\n';
            else ss << ' ';
        }
        ss << std::dec << '\n';
        std::cerr << ss.str();
        };

    auto try_decompress_slice = [&](const uint8_t* payload_ptr_inner, size_t payload_len_inner,
        size_t off, size_t avail,
        std::vector<unsigned char>& workbuf_inner,
        std::vector<unsigned char>& dest,
        size_t& outLen) -> bool {
            if (off >= payload_len_inner || avail == 0) return false;
            const uint8_t* start = payload_ptr_inner + off;
            size_t want = std::max<size_t>(64 * 1024, avail * 8);
            if (want > (8ull << 20)) want = (8ull << 20);
            if (workbuf_inner.size() < want) workbuf_inner.resize(want);

            if (g_cm_debug) {
                std::ostringstream ss;
                ss << "[CM-TRY] off=" << off << " avail=" << avail << " cap=" << workbuf_inner.size() << " pv=";
                size_t pv = std::min<size_t>(16, avail);
                for (size_t i = 0; i < pv; ++i) ss << std::hex << std::setw(2) << std::setfill('0') << (int)start[i];
                ss << std::dec;
                std::cerr << ss.str() << "\n";
            }

            size_t tmpOut = 0;
            bool ok = Lzo::Decompress(start, static_cast<std::size_t>(avail),
                workbuf_inner.data(), workbuf_inner.size(), tmpOut);
            if (!ok) {
                if (g_cm_debug) std::cerr << "[CM-TRY] decompress failed at off=" << off << " avail=" << avail << "\n";
                return false;
            }
            dest.insert(dest.end(), workbuf_inner.data(), workbuf_inner.data() + tmpOut);
            outLen = tmpOut;
            if (g_cm_debug) std::cerr << "[CM-TRY] decompress OK off=" << off << " out=" << tmpOut << "\n";
            return true;
        };

    bool any_success = false;

    if (!markers.empty()) {
        // Try decompressing at each marker using the full remaining payload from that marker.
        for (size_t mi = 0; mi < markers.size(); ++mi) {
            size_t marker_pos = markers[mi];
            size_t off = marker_pos;
            size_t avail = (off < payload_len) ? (payload_len - off) : 0;
            if (avail == 0) continue;

            size_t outLen = 0;
            if (try_decompress_slice(payload_ptr, payload_len, off, avail, workbuf, full_decomp, outLen)) {
                any_success = true;
                if (g_cm_debug) std::cerr << "[CM parser] decompressed at marker " << marker_pos << " out=" << outLen << "\n";
                // continue trying next markers; append-only semantics
                continue;
            }
            else {
                if (g_cm_debug) std::cerr << "[CM parser] decompress failed at marker " << marker_pos << "; trying small adj window\n";
                // small adjacency fallback: try marker-1, marker+1
                for (int delta = -1; delta <= 1; ++delta) {
                    long o = static_cast<long>(marker_pos) + delta;
                    if (o < 0 || static_cast<size_t>(o) >= payload_len) continue;
                    size_t off2 = static_cast<size_t>(o);
                    size_t avail2 = payload_len - off2;
                    size_t out2 = 0;
                    if (try_decompress_slice(payload_ptr, payload_len, off2, avail2, workbuf, full_decomp, out2)) {
                        any_success = true;
                        if (g_cm_debug) std::cerr << "[CM parser] decompressed at marker_adj " << off2 << " out=" << out2 << "\n";
                        break;
                    }
                }
            }
        }
    }
    else {
        // No markers found: conservative fallback scan across first 0..min(256,payload_len-3)
        size_t max_probe = (payload_len > 3) ? std::min<size_t>(256, payload_len - 3) : 0;
        for (size_t off = 0; off <= max_probe; ++off) {
            size_t avail = payload_len - off;
            size_t outLen = 0;
            if (try_decompress_slice(payload_ptr, payload_len, off, avail, workbuf, full_decomp, outLen)) {
                any_success = true;
                // Keep scanning — there may be more blocks.
            }
        }
    }

    if (!any_success) {
        std::cerr << "[CM parser] decompression failed — dumping payload for analysis\n";
        dump_payload_for_analysis(payload_ptr, payload_len);
        return;
    }

    if (g_cm_debug) std::cerr << "[CM parser] concat decompressed -> " << full_decomp.size() << " bytes\n";

    // Parse the decompressed buffer into info packets
    const uint8_t* process_buf = full_decomp.data();
    size_t process_len = full_decomp.size();

    const uint8_t* p = process_buf;
    size_t rem = process_len;
    const size_t INFO_HEADER_SZ = 2 + 2 + 4; // iCode + iLen + seq
    const size_t INFO_TRAILER_SZ = 2 + 1;    // checksum + EOT

    for (uint16_t pkt_idx = 0; pkt_idx < iNoOfPackets; ++pkt_idx) {
        if (rem < INFO_HEADER_SZ + INFO_TRAILER_SZ) {
            if (g_cm_debug) std::cerr << "[CM parser] not enough bytes for packet header/trailer (pkt " << pkt_idx << ", rem=" << rem << ")\n";
            break;
        }
        uint16_t iCode_be = read_be16(p); p += 2; rem -= 2;
        uint16_t iLen_be = read_be16(p); p += 2; rem -= 2;
        uint32_t seqNo_be = read_be32(p); p += 4; rem -= 4;

        uint16_t iLen = iLen_be;
        if (iLen < INFO_HEADER_SZ + INFO_TRAILER_SZ) {
            if (g_cm_debug) std::cerr << "[CM parser] malformed iLen: " << iLen << "\n";
            continue;
        }

        size_t payload_bytes = static_cast<size_t>(iLen) - INFO_HEADER_SZ - INFO_TRAILER_SZ;
        if (rem < payload_bytes + INFO_TRAILER_SZ) {
            std::cerr << "[CM parser] truncated packet payload (need " << payload_bytes + INFO_TRAILER_SZ
                << ", have " << rem << ")\n";
            break;
        }

        const uint8_t* payload = p;
        p += payload_bytes; rem -= payload_bytes;

        uint16_t recv_checksum = 0; std::memcpy(&recv_checksum, p, 2); recv_checksum = ntohs(recv_checksum); p += 2; rem -= 2;
        uint8_t cEOT = *p; p += 1; rem -= 1; (void)cEOT;

        if (auto* h = disp_.find(iCode_be)) {
            MessageView mv{ reinterpret_cast<const char*>(payload), static_cast<int>(payload_bytes) };
            h->handle(mv, out, instDir, strikes);
        }
        else {
            if (g_cm_debug) {
                std::ostringstream oss;
                oss << "[CM parser] unhandled iCode=0x" << std::hex << std::setw(4) << std::setfill('0') << iCode_be << std::dec
                    << " seq=" << seqNo_be << " payload_bytes=" << payload_bytes;
                std::cerr << oss.str() << "\n";
            }
        }
    } // per-packet loop
}
