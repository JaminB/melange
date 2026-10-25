// The shared media protocol headers compiled for x64: the cases the core's media_selftest runs on x86
// (tests/media_shared_cases.inc), so a layout or codec difference between the two builds fails here.
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

static_assert(sizeof(void*) == 8, "this test is the x64 half");

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
#include "../../../tests/media_shared_cases.inc"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    shared_cases::All();
    printf("holo_media_shared_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
