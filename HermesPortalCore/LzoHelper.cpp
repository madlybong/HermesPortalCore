#include "includes/hermes_core.h"

extern "C" {
    // Adjust include path if needed to find lzo1z.h
    #include <lzo/lzo1z.h>
}

namespace Lzo {

bool Init() {
    return lzo_init() == LZO_E_OK;
}

bool Decompress(const unsigned char* in, std::size_t inLen,
                unsigned char* out, std::size_t outCap, std::size_t& outLen) {
    if (!in || !out || inLen == 0 || outCap == 0) return false;
    lzo_uint dst_len = static_cast<lzo_uint>(outCap);
    int rc = lzo1z_decompress(reinterpret_cast<const lzo_bytep>(in),
                              static_cast<lzo_uint>(inLen),
                              reinterpret_cast<lzo_bytep>(out),
                              &dst_len, nullptr);
    if (rc != LZO_E_OK) return false;
    outLen = static_cast<std::size_t>(dst_len);
    return true;
}

} // namespace Lzo
