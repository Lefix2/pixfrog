#pragma once
#include "fseq_player.h"
#include <vector>
namespace pixfrog::fseq::fake {
void set(Status s, uint32_t pos_ms);
std::vector<uint32_t>& seeks();
}  // namespace pixfrog::fseq::fake
