// Stand-in for audio.cpp under the UI task tests: records the feedback asked.
#include "audio_fake.h"

namespace fake {
std::vector<float>& levels() {
    static std::vector<float> v;
    return v;
}
int& boot_sounds() {
    static int n = 0;
    return n;
}
std::vector<pixfrog::audio::Feedback>& feedbacks() {
    static std::vector<pixfrog::audio::Feedback> v;
    return v;
}
}  // namespace fake

namespace pixfrog::audio {
bool play_boot() {
    ++fake::boot_sounds();
    return true;
}
void feedback(Feedback f, float level) {
    fake::feedbacks().push_back(f);
    fake::levels().push_back(level);
}
}  // namespace pixfrog::audio
