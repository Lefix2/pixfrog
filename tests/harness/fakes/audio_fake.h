#pragma once
#include <vector>

#include "audio.h"

namespace fake {
std::vector<pixfrog::audio::Feedback>& feedbacks();  // audio::feedback() calls, in order
int& boot_sounds();
std::vector<float>& levels();  // the gauge level passed with each feedback // audio::play_boot()
                               // calls
}  // namespace fake
