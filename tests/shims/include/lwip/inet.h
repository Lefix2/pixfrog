// Host shim: byte-order helpers.
#pragma once
#include <arpa/inet.h>
#include <cstdint>

inline uint32_t lwip_htonl(uint32_t v) {
    return htonl(v);
}
inline uint32_t lwip_ntohl(uint32_t v) {
    return ntohl(v);
}
