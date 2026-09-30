// Every pure wire parser on one input: Art-Net, sACN, FPP sync and the FSEQ
// header with its sparse-range helpers. A parser that accepts a packet must
// report fields inside the buffer it was given. (Lengths above 512 slots are
// accepted here; dmx_manager clamps them — fuzz_receivers covers that path.)
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "artnet_parser.h"
#include "fpp_sync_parser.h"
#include "fseq_format.h"
#include "sacn_parser.h"

using namespace pixfrog;

#define CHECK(c)                                                                                   \
    if (!(c)) std::abort()

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    namespace ap = artnet::parser;
    uint16_t op  = 0;
    if (ap::parse_header(data, size, &op)) {
        ap::DmxFields dmx{};
        if (ap::parse_dmx(data, size, &dmx)) CHECK(dmx.data + dmx.data_len <= data + size);
        ap::NzsFields nzs{};
        if (ap::parse_nzs(data, size, &nzs)) CHECK(nzs.data + nzs.data_len <= data + size);
        ap::AddressFields addr{};
        ap::parse_address(data, size, &addr);
        ap::IpProgFields ipp{};
        ap::parse_ip_prog(data, size, &ipp);
        ap::TimeCodeFields tc{};
        ap::parse_time_code(data, size, &tc);
    }

    namespace sp = sacn::parser;
    sp::DataFields sd{};
    if (sp::parse_data(data, size, &sd))
        CHECK(sd.data + sd.data_len <= data + size && sd.cid + 16 <= data + size);
    uint16_t sync_addr = 0;
    sp::parse_sync(data, size, &sync_addr);

    fpp::parser::SyncFields fs{};
    if (fpp::parser::parse_sync(data, size, &fs))
        CHECK(std::memchr(fs.filename, '\0', sizeof(fs.filename)) != nullptr);

    fseq::Header h{};
    if (fseq::parse_header(data, size, h) == fseq::ParseResult::Ok) {
        const size_t off = fseq::sparse_range_file_offset(h);
        const size_t n   = h.num_sparse_ranges;
        if (off <= size && n * sizeof(fseq::SparseRange) <= size - off) {
            fseq::SparseRange ranges[256];
            std::memcpy(ranges, data + off, n * sizeof(fseq::SparseRange));
            const uint32_t bytes = fseq::sparse_frame_bytes(ranges, static_cast<uint8_t>(n));
            for (uint32_t o = 0; o < bytes && o < 4096; o += 97)
                fseq::sparse_to_absolute(ranges, static_cast<uint8_t>(n), o);
        }
        fseq::frame_universe_count(h.channel_count);
        uint16_t uni = 0;
        for (uint32_t ch = 0; ch < h.channel_count && ch < 4096; ch += 131)
            if (fseq::channel_to_universe_checked(ch, 1, 63999, &uni)) CHECK(uni >= 1);
    }
    return 0;
}
