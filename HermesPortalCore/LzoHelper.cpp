//#include "includes/hermes_core.h"
//
//extern "C" {
//    // Adjust include path if needed to find lzo1z.h
//    #include <lzo/lzo1z.h>
//}
//
//namespace Lzo {
//
//bool Init() {
//    return lzo_init() == LZO_E_OK;
//}
//
//bool Decompress(const unsigned char* in, std::size_t inLen,
//                unsigned char* out, std::size_t outCap, std::size_t& outLen) {
//    if (!in || !out || inLen == 0 || outCap == 0) return false;
//    lzo_uint dst_len = static_cast<lzo_uint>(outCap);
//    int rc = lzo1z_decompress(reinterpret_cast<const lzo_bytep>(in),
//                              static_cast<lzo_uint>(inLen),
//                              reinterpret_cast<lzo_bytep>(out),
//                              &dst_len, nullptr);
//    if (rc != LZO_E_OK) return false;
//    outLen = static_cast<std::size_t>(dst_len);
//    return true;
//}
//
//} // namespace Lzo




// LzoHelper.cpp — robust + length-prefix probing + cached offset/variant
// Paste this file replacing your existing LzoHelper.cpp

#include "includes/hermes_core.h"

extern "C" {
#include <lzo/lzo1z.h>
#include <lzo/lzo1x.h>
}

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <fstream>

extern bool g_cm_debug; // declared in includes/hermes_core.h, defined in HermesPortalCore.cpp

namespace Lzo {

    enum class Variant { Unknown = 0, Lzo1z, Lzo1x };

    // cached offset & variant (per-process)
    static std::atomic<int> cached_offset{ -2 }; // -2 = unknown, -1 = discovery failed, >=0 = offset
    static std::atomic<int> cached_variant{ 0 }; // 0 = unknown, 1 = lzo1z, 2 = lzo1x

    bool Init() {
        return lzo_init() == LZO_E_OK;
    }

    static void debug_preview(const unsigned char* p, size_t len, size_t maxp = 16) {
        if (!g_cm_debug) return;
        size_t pv = std::min<size_t>(maxp, len);
        for (size_t i = 0; i < pv; ++i) {
            std::cerr << std::hex << std::setw(2) << std::setfill('0') << (int)p[i];
        }
        std::cerr << std::dec;
    }

    static int try_lzo1z_internal(const unsigned char* in, size_t avail, unsigned char* out, size_t outCap, size_t& outLen) {
        lzo_uint dst_len = static_cast<lzo_uint>(outCap);
        int rc = lzo1z_decompress(reinterpret_cast<const lzo_bytep>(in),
            static_cast<lzo_uint>(avail),
            reinterpret_cast<lzo_bytep>(out),
            &dst_len, nullptr);
        if (rc == LZO_E_OK) outLen = static_cast<size_t>(dst_len);
        return rc;
    }
    static int try_lzo1x_internal(const unsigned char* in, size_t avail, unsigned char* out, size_t outCap, size_t& outLen) {
        lzo_uint dst_len = static_cast<lzo_uint>(outCap);
        int rc = lzo1x_decompress(reinterpret_cast<const lzo_bytep>(in),
            static_cast<lzo_uint>(avail),
            reinterpret_cast<lzo_bytep>(out),
            &dst_len, nullptr);
        if (rc == LZO_E_OK) outLen = static_cast<size_t>(dst_len);
        return rc;
    }

    // Try both variants at given offset; on success set outLen and variantUsed and return true.
    static bool try_variants_at(const unsigned char* in, size_t inLen, size_t off,
        unsigned char* out, size_t outCap, size_t& outLen, Variant& variantUsed) {
        if (off >= inLen) return false;
        size_t avail = inLen - off;
        if (g_cm_debug) {
            std::cerr << "[LZO-DBG] try offset=" << off << " avail=" << avail << " preview=";
            debug_preview(in + off, avail, 16);
            std::cerr << "\n";
        }

        // lzo1z
        int rc1 = try_lzo1z_internal(in + off, avail, out, outCap, outLen);
        if (g_cm_debug) std::cerr << "[LZO-DBG] off=" << off << " lzo1z rc=" << rc1 << "\n";
        if (rc1 == LZO_E_OK) { variantUsed = Variant::Lzo1z; return true; }

        // lzo1x
        int rc2 = try_lzo1x_internal(in + off, avail, out, outCap, outLen);
        if (g_cm_debug) std::cerr << "[LZO-DBG] off=" << off << " lzo1x rc=" << rc2 << "\n";
        if (rc2 == LZO_E_OK) { variantUsed = Variant::Lzo1x; return true; }

        return false;
    }

    // Additional helper: treat the payload as possibly starting with a BE 2-byte length or 4-byte length
    // and attempt to decompress after those length fields.
    static bool try_length_prefixed(const unsigned char* in, size_t inLen,
        unsigned char* out, size_t outCap, size_t& outLen, Variant& variantUsed) {
        // 2-byte BE length
        if (inLen > 2) {
            uint16_t len2 = 0;
            std::memcpy(&len2, in, 2);
            len2 = ntohs(len2);
            if (len2 > 0 && static_cast<size_t>(len2) <= inLen - 2) {
                if (g_cm_debug) std::cerr << "[LZO-DBG] trying 2-byte length prefix -> compressed_len=" << len2 << "\n";
                if (try_variants_at(in, inLen, 2, out, outCap, outLen, variantUsed)) return true;
            }
        }
        // 4-byte BE length
        if (inLen > 4) {
            uint32_t len4 = 0;
            std::memcpy(&len4, in, 4);
            len4 = ntohl(len4);
            if (len4 > 0 && static_cast<size_t>(len4) <= inLen - 4) {
                if (g_cm_debug) std::cerr << "[LZO-DBG] trying 4-byte length prefix -> compressed_len=" << len4 << "\n";
                if (try_variants_at(in, inLen, 4, out, outCap, outLen, variantUsed)) return true;
            }
        }
        return false;
    }

    // Discover working offset & variant. Uses quiet probing unless g_cm_debug true.
    static bool discover_offset_and_variant(const unsigned char* in, size_t inLen,
        unsigned char* out, size_t outCap, size_t& outLen) {
        // fast path: try off=0 both variants
            {
                Variant v = Variant::Unknown;
                if (try_variants_at(in, inLen, 0, out, outCap, outLen, v)) {
                    cached_offset.store(0);
                    cached_variant.store((v == Variant::Lzo1z) ? 1 : 2);
                    if (g_cm_debug) std::cerr << "[LZO-DBG] discovered offset=0 variant=" << ((v == Variant::Lzo1z) ? "lzo1z" : "lzo1x") << "\n";
                    return true;
                }
            }

            // scan for 0x1A 0x04 sequence and try -2..+1 around it (common)
            for (size_t pos = 0; pos + 1 < std::min<size_t>(inLen, 256); ++pos) {
                if (in[pos] == 0x1A && in[pos + 1] == 0x04) {
                    for (int delta = -2; delta <= 1; ++delta) {
                        long off_long = static_cast<long>(pos) + delta;
                        if (off_long < 0) continue;
                        size_t off = static_cast<size_t>(off_long);
                        Variant v = Variant::Unknown;
                        if (try_variants_at(in, inLen, off, out, outCap, outLen, v)) {
                            cached_offset.store(static_cast<int>(off));
                            cached_variant.store((v == Variant::Lzo1z) ? 1 : 2);
                            if (g_cm_debug) std::cerr << "[LZO-DBG] discovered offset=" << off << " variant=" << ((v == Variant::Lzo1z) ? "lzo1z" : "lzo1x") << "\n";
                            return true;
                        }
                    }
                }
            }

            // fallback: scan small prefix 1..search_limit for any plausible offset
            size_t search_limit = (inLen > 3) ? std::min<size_t>(128, inLen - 3) : 0;
            for (size_t off = 1; off <= search_limit; ++off) {
                Variant v = Variant::Unknown;
                if (try_variants_at(in, inLen, off, out, outCap, outLen, v)) {
                    cached_offset.store(static_cast<int>(off));
                    cached_variant.store((v == Variant::Lzo1z) ? 1 : 2);
                    if (g_cm_debug) std::cerr << "[LZO-DBG] discovered offset=" << off << " variant=" << ((v == Variant::Lzo1z) ? "lzo1z" : "lzo1x") << "\n";
                    return true;
                }
            }

            // Try length-prefixed forms
            {
                Variant v = Variant::Unknown;
                if (try_length_prefixed(in, inLen, out, outCap, outLen, v)) {
                    cached_offset.store(-3); // indicate length-prefixed discovered (special marker)
                    cached_variant.store((v == Variant::Lzo1z) ? 1 : 2);
                    if (g_cm_debug) std::cerr << "[LZO-DBG] discovered length-prefixed block variant=" << ((v == Variant::Lzo1z) ? "lzo1z" : "lzo1x") << "\n";
                    return true;
                }
            }

            // none worked
            cached_offset.store(-1);
            cached_variant.store(0);

            if (g_cm_debug) {
                // write failed payload for offline inspection
                const char* dump_file = "lzo_failed_payload.bin";
                std::ofstream ofs(dump_file, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(in), static_cast<std::streamsize>(inLen));
                    ofs.close();
                    std::cerr << "[LZO-DBG] discovery failed; wrote failed payload to " << dump_file << "\n";
                }
                else {
                    std::cerr << "[LZO-DBG] discovery failed; could not write failed payload\n";
                }
            }
            return false;
    }

    bool Decompress(const unsigned char* in, std::size_t inLen,
        unsigned char* out, std::size_t outCap, std::size_t& outLen) {
        if (!in || !out || inLen == 0 || outCap == 0) return false;

        int off_cached = cached_offset.load();
        int var_cached = cached_variant.load();

        // If we discovered earlier a length-prefixed case (-3), handle it specially: read length and attempt decompression after length field.
        if (off_cached == -3) {
            // try 2-byte length
            if (inLen > 2) {
                uint16_t len2 = 0; std::memcpy(&len2, in, 2); len2 = ntohs(len2);
                if (len2 > 0 && static_cast<size_t>(len2) <= inLen - 2) {
                    Variant v = (var_cached == 1) ? Variant::Lzo1z : Variant::Lzo1x;
                    size_t tmp = 0;
                    if (try_variants_at(in, inLen, 2, out, outCap, tmp, v)) { outLen = tmp; return true; }
                }
            }
            // try 4-byte length
            if (inLen > 4) {
                uint32_t len4 = 0; std::memcpy(&len4, in, 4); len4 = ntohl(len4);
                if (len4 > 0 && static_cast<size_t>(len4) <= inLen - 4) {
                    Variant v = (var_cached == 1) ? Variant::Lzo1z : Variant::Lzo1x;
                    size_t tmp = 0;
                    if (try_variants_at(in, inLen, 4, out, outCap, tmp, v)) { outLen = tmp; return true; }
                }
            }
            // fallthrough to fresh discovery if this didn't work
        }

        // If cached offset + variant available and >=0: try it first
        if (off_cached >= 0 && (var_cached == 1 || var_cached == 2)) {
            Variant exp = (var_cached == 1) ? Variant::Lzo1z : Variant::Lzo1x;
            size_t tmp = 0;
            Variant found = Variant::Unknown;
            if (try_variants_at(in, inLen, static_cast<size_t>(off_cached), out, outCap, tmp, found)) {
                // update variant if different
                cached_variant.store((found == Variant::Lzo1z) ? 1 : 2);
                outLen = tmp;
                if (g_cm_debug) std::cerr << "[LZO-DBG] cached offset success off=" << off_cached << "\n";
                return true;
            }
            // cached failed -> fallthrough to rediscover
            if (g_cm_debug) std::cerr << "[LZO-DBG] cached offset failed; rediscovering\n";
        }

        // otherwise discover (quiet unless g_cm_debug)
        return discover_offset_and_variant(in, inLen, out, outCap, outLen);
    }

} // namespace Lzo
