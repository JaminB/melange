// Offline self-test for the media side of Melange 0.12 (Holo): the protocol headers shared with the x64 helper
// (url_logic.h, shm_logic.h, pipe_codec.h, the same cases the helper's own test runs), the pack installer
// (media_selftest_pack.inc, C4a) and the media host (media_selftest_host.inc, C4b). No game, no network.
// Exit code 0 = all passed.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "media/pipe_codec.h"
#include "media/shm_logic.h"
#include "media/url_logic.h"

namespace {
int g_fail = 0, g_pass = 0;
void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}
}  // namespace
#define CHECK(c, w) Check((c), (w))
#include "media_shared_cases.inc"

#include "media_selftest_pack.inc"
#include "media_selftest_host.inc"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::pair<const char*, void (*)()> parts[] = {
        {"url", shared_cases::Url},       {"shm", shared_cases::ShmRules}, {"shm threads", shared_cases::ShmThreads},
        {"pipe", shared_cases::Pipe},     {"pack", pack_cases::Run},       {"host", host_cases::Run}};
    for (const auto& [name, fn] : parts) {
        const int before = g_fail;
        fn();
        printf("  %-12s %s\n", name, g_fail == before ? "ok" : "FAILED");
    }
    printf("media_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
