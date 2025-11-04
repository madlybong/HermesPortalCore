// Parser.cpp
#include "includes/hermes_core.h"
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>

// existing FO parser (unchanged)
static uint16_t peek_code(const char* d, int L, int base) {
    if (L < base + 20) return 0;
    uint16_t code = 0;
    std::memcpy(&code, d + base + 10, 2);
    return ntohs(code);
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

// --------------------------- CM parser ---------------------------
// CM payloads frequently contain concatenated small LZO streams.
// This function scans for likely LZO start markers (0x1A 0x04) and
// attempts to decompress each candidate chunk. Appends successful
// decompressed outputs into a single buffer which is then parsed
// as a sequence of ST_INFO_HEADER + payload records.
void PacketParser::parseCM(const uint8_t* buf, size_t len, ConsoleSink& out,
    InstrumentDirectory* instDir, const StrikeList& strikes) {

    if (!buf || len < 5) return;

    // ST_COMP_BATCH_HEADER (per spec): CHAR cCompOrNot; SHORT nDataSize; SHORT iNoOfPackets;
    // total 5 bytes
    uint8_t cCompOrNot = buf[0];
    uint16_t nDataSize_be = 0;
    uint16_t iNoOfPackets_be = 0;
    std::memcpy(&nDataSize_be, buf + 1, 2);
    std::memcpy(&iNoOfPackets_be, buf + 3, 2);
    uint16_t nDataSize = ntohs(nDataSize_be);
    uint16_t iNoOfPackets = ntohs(iNoOfPackets_be);

    size_t payload_off = 5;
    if (payload_off > len) return;
    size_t payload_len = (nDataSize == 0) ? (len - payload_off) : std::min<size_t>(nDataSize, len - payload_off);
    const uint8_t* payload_ptr = buf + payload_off;

    if (g_cm_debug) {
        std::ostringstream ss;
        ss << "[CM parser] batch header cCompOrNot=0x" << std::hex << (int)cCompOrNot << std::dec
            << " nDataSize=" << nDataSize << " iNoOfPackets=" << iNoOfPackets
            << " payload_len=" << payload_len << " preview=";
        // preview first 16 bytes
        size_t pp = std::min<size_t>(16, payload_len);
        for (size_t i = 0; i < pp; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)payload_ptr[i];
        }
        ss << std::dec;
        std::cerr << ss.str() << "\n";
    }

    bool is_compressed = !(cCompOrNot == 0 || cCompOrNot == 'N' || cCompOrNot == 'n');

    // If not compressed, feed the payload directly to the same ST_INFO parser
    if (!is_compressed) {
        // use the decompressed payload as-is
        const char* proc = reinterpret_cast<const char*>(payload_ptr);
        int proc_len = static_cast<int>(payload_len);

        // iterate ST_INFO_HEADER records
        size_t pos = 0;
        while (pos + 8 <= static_cast<size_t>(proc_len)) {
            // ST_INFO_HEADER: SHORT iCode; SHORT iLen; LONG lSeqNo; (big-endian)
            uint16_t iCode_be = 0, iLen_be = 0;
            uint32_t lSeqNo_be = 0;
            std::memcpy(&iCode_be, proc + pos + 0, 2);
            std::memcpy(&iLen_be, proc + pos + 2, 2);
            std::memcpy(&lSeqNo_be, proc + pos + 4, 4);
            uint16_t iCode = ntohs(iCode_be);
            uint16_t iLen = ntohs(iLen_be);
            uint32_t lSeqNo = ntohl(lSeqNo_be);

            if (iLen < 8) break; // invalid
            if (pos + iLen > static_cast<size_t>(proc_len)) break;

            MessageView mv{ proc + pos, static_cast<int>(iLen) };
            if (auto* h = disp_.find(iCode)) {
                h->handle(mv, out, instDir, strikes);
            }
            pos += iLen;
        }
        return;
    }

    // Compressed path: try to split by likely LZO start markers (0x1A 0x04)
    std::vector<size_t> markers;
    for (size_t i = 0; i + 1 < payload_len; ++i) {
        if (payload_ptr[i] == 0x1A && payload_ptr[i + 1] == 0x04) {
            markers.push_back(i);
        }
    }
    // If no markers found, try marker at 0 as heuristic
    if (markers.empty()) markers.push_back(0);

    // We'll try to decompress each chunk between markers (marker..next_marker or marker..end)
    std::vector<unsigned char> decomp_all;
    decomp_all.reserve(payload_len * 4); // guess expansion

    // per-chunk buffer (thread-local)
    static thread_local std::vector<unsigned char> workbuf;
    workbuf.resize(65536); // initial

    bool any_ok = false;

    for (size_t mi = 0; mi < markers.size(); ++mi) {
        size_t start = markers[mi];
        size_t end = (mi + 1 < markers.size()) ? markers[mi + 1] : payload_len;
        if (start >= payload_len) continue;
        if (end <= start) continue;

        size_t comp_len = end - start;
        const unsigned char* comp_ptr = payload_ptr + start;

        // try decompress comp_ptr[0..comp_len)
        std::size_t outLen = 0;
        bool ok = false;

        // ensure workbuf large enough
        if (workbuf.size() < payload_len * 4) workbuf.resize(std::max<size_t>(workbuf.size(), payload_len * 4));

        if (g_cm_debug) {
            std::cerr << "[CM] trying marker-chunk at offset=" << start << " len=" << comp_len << "\n";
        }

        if (Lzo::Decompress(comp_ptr, comp_len, workbuf.data(), workbuf.size(), outLen)) {
            if (outLen > 0) {
                // success
                decomp_all.insert(decomp_all.end(), workbuf.data(), workbuf.data() + outLen);
                any_ok = true;
                ok = true;
                if (g_cm_debug) std::cerr << "[CM] decompressed chunk at " << start << " -> " << outLen << " bytes\n";
                // continue to next chunk
                continue;
            }
        }

        // fallback: try decompressing from start to end-of-payload (in case comp block goes beyond
        // next marker or markers are noisy)
        size_t fallback_len = payload_len - start;
        std::size_t outLen2 = 0;
        if (fallback_len > comp_len) {
            if (g_cm_debug) std::cerr << "[CM] fallback: trying full-suffix at " << start << " len=" << fallback_len << "\n";
            if (Lzo::Decompress(comp_ptr, fallback_len, workbuf.data(), workbuf.size(), outLen2)) {
                if (outLen2 > 0) {
                    decomp_all.insert(decomp_all.end(), workbuf.data(), workbuf.data() + outLen2);
                    any_ok = true;
                    ok = true;
                    if (g_cm_debug) std::cerr << "[CM] decompressed suffix at " << start << " -> " << outLen2 << " bytes\n";
                    // we can't reliably tell consumed compressed bytes; assume remainder consumed and break
                    break;
                }
            }
        }

        if (!ok && g_cm_debug) {
            std::cerr << "[CM] chunk at " << start << " failed to decompress\n";
            // continue to next marker
        }
    }

    // If nothing worked, try one more attempt: decompress entire payload
    if (!any_ok) {
        std::size_t outLen3 = 0;
        if (g_cm_debug) std::cerr << "[CM] final attempt: decompress entire payload len=" << payload_len << "\n";
        if (Lzo::Decompress(payload_ptr, payload_len, workbuf.data(), workbuf.size(), outLen3) && outLen3 > 0) {
            decomp_all.insert(decomp_all.end(), workbuf.data(), workbuf.data() + outLen3);
            any_ok = true;
            if (g_cm_debug) std::cerr << "[CM] decompressed entire payload -> " << outLen3 << " bytes\n";
        }
    }

    if (!any_ok) {
        std::cerr << "[CM parser] decompression failed \xC3\xB9 dumping payload\n"; // 'ù' from your logs; keep similar
        // dump file for offline analysis
        try {
            std::ofstream ofs("cm_failed_payload.bin", std::ios::binary);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(payload_ptr), static_cast<std::streamsize>(payload_len));
                ofs.close();
                std::cerr << "[CM DUMP] wrote " << payload_len << " bytes to cm_failed_payload.bin\n";
            }
        }
        catch (...) {}
        // print a small hex preview
        if (g_cm_debug) {
            std::ostringstream ss;
            ss << "[CM DUMP HEX] first " << std::min<size_t>(256, payload_len) << " bytes:\n";
            size_t hex_print = std::min<size_t>(256, payload_len);
            for (size_t i = 0; i < hex_print; ++i) {
                ss << std::hex << std::setw(2) << std::setfill('0') << (int)payload_ptr[i] << ' ';
                if ((i & 0x0F) == 0x0F) ss << '\n';
            }
            ss << std::dec << '\n';
            std::cerr << ss.str();
        }
        return;
    }

    // Now parse the accumulated decompressed buffer as sequence of ST_INFO_HEADER + payload
    const unsigned char* proc_ptr = decomp_all.data();
    size_t proc_len = decomp_all.size();
    if (g_cm_debug) std::cerr << "[CM parser] concat decompressed -> " << proc_len << " bytes\n";

    size_t pos = 0;
    while (pos + 8 <= proc_len) {
        // ST_INFO_HEADER: SHORT iCode; SHORT iLen; LONG lSeqNo;
        uint16_t iCode_be = 0, iLen_be = 0;
        uint32_t lSeqNo_be = 0;
        std::memcpy(&iCode_be, proc_ptr + pos + 0, 2);
        std::memcpy(&iLen_be, proc_ptr + pos + 2, 2);
        std::memcpy(&lSeqNo_be, proc_ptr + pos + 4, 4);
        uint16_t iCode = ntohs(iCode_be);
        uint16_t iLen = ntohs(iLen_be);
        uint32_t lSeqNo = ntohl(lSeqNo_be);

        if (iLen < 8) { // invalid header - abort
            if (g_cm_debug) std::cerr << "[CM parser] invalid iLen=" << iLen << " at pos=" << pos << "\n";
            break;
        }

        // iLen refers to the total packet length (header + payload). Some vendors include trailer; be defensive:
        size_t rec_total = static_cast<size_t>(iLen);
        if (pos + rec_total > proc_len) {
            // if it doesn't fit, try to be tolerant: if remaining bytes look like a header + trailer skip
            if (g_cm_debug) std::cerr << "[CM parser] incomplete record (need " << rec_total << " have " << (proc_len - pos) << ") at pos=" << pos << "\n";
            break;
        }

        // dispatch: construct MessageView with pointer to header start (so handlers can detect offsets themselves)
        MessageView mv{ reinterpret_cast<const char*>(proc_ptr + pos), static_cast<int>(rec_total) };
        if (auto* h = disp_.find(iCode)) {
            h->handle(mv, out, instDir, strikes);
        }
        else {
            if (g_cm_debug) {
                // print iCode as two ascii chars as well — helpful for CM (CT/PN etc)
                char c1 = static_cast<char>((iCode >> 8) & 0xFF);
                char c2 = static_cast<char>(iCode & 0xFF);
                std::ostringstream ss;
                ss << "[CM parser] no handler for iCode=0x" << std::hex << iCode << std::dec
                    << " (" << c1 << c2 << ") len=" << rec_total << " seq=" << lSeqNo << "\n";
                std::cerr << ss.str();
            }
        }

        pos += rec_total;
    }

    // done
}
