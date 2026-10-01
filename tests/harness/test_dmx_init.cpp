// dmx::init() failure paths, one allocation at a time. A failed init leaves
// the module half-built, so these run in their own process; the last case
// proves a clean init still works after all of them.

#include "config_store.h"
#include "dmx_manager.h"
#include "harness.h"
#include "shim_control.h"

using namespace pixfrog;

namespace {
void setup() {
    static bool once = false;
    if (!once) {
        shim::nvs_wipe();
        config::init();
        once = true;
    }
    shim::faults_clear();
}
}  // namespace

// Allocation order in init(): bank A, bank B, merge staging, crossfade scratch,
// then 2 buffers per channel (heap_caps); swap mutex, then sync semaphore;
// one event group.
TEST(every_allocation_failure_is_reported) {
    shim::fail_next(shim::Fault::HeapCaps);  // universe bank
    EXPECT_FALSE(dmx::init());
    shim::fail_next(shim::Fault::Semaphore);  // swap mutex
    EXPECT_FALSE(dmx::init());
    shim::fail_next(shim::Fault::HeapCaps, 1, 2);  // merge staging
    EXPECT_FALSE(dmx::init());
    shim::fail_next(shim::Fault::HeapCaps, 1, 3);  // crossfade scratch
    EXPECT_FALSE(dmx::init());
    shim::fail_next(shim::Fault::HeapCaps, 1, 4);  // channel 0's pixel buffer
    EXPECT_FALSE(dmx::init());
    shim::fail_next(shim::Fault::EventGroup);
    EXPECT_FALSE(dmx::init());
    shim::fail_next(shim::Fault::Semaphore, 1, 1);  // ArtSync semaphore
    EXPECT_FALSE(dmx::init());
}

TEST(a_clean_init_still_works) {
    EXPECT_TRUE(dmx::init());
    EXPECT_TRUE(dmx::pixel_back_buffer(0) != nullptr);
}

TEST(wait_for_sync_times_out_or_wakes_on_sync) {
    const int64_t t0 = shim::now_us();
    EXPECT_FALSE(dmx::wait_for_sync_or_period(5));
    EXPECT_TRUE(shim::now_us() - t0 >= 5000);
    dmx::note_sync();
    EXPECT_TRUE(dmx::wait_for_sync_or_period(5));
}

int main(int argc, char** argv) {
    return harness::run_all(argc, argv, setup);
}
