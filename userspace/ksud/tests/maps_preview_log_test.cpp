// Compile the real logger with only its /dev/log/main fopen replaced. No
// device file is opened. Full CLI tests separately assert OFF is inherited by
// the query callback and restored on success, denial, malformed input/error.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include "log.hpp"

int opened = 0;
FILE* preview_log_open(const char* path, const char* mode) {
    assert(std::strcmp(path, "/dev/log/main") == 0 && std::strcmp(mode, "w") == 0);
    ++opened;
    return nullptr;
}
int main() {
    ksud::log_init("preview-test");
    ksud::log_set_level(ksud::LogLevel::INFO);
    const auto saved = ksud::log_get_level();
    ksud::log_set_level(ksud::LogLevel::OFF);
    ksud::log_e("injected failing ioctl");
    ksud::log_w("injected failed SIGSYS setup");
    ksud::log_i("injected info");
    ksud::log_d("injected driver discovery");
    ksud::log_v("injected detail");
    assert(opened == 0 && ksud::log_get_level() == ksud::LogLevel::OFF);
    ksud::log_set_level(saved);
    ksud::log_e("normal logger restored (expected test diagnostic)");
    assert(opened == 1 && ksud::log_get_level() == ksud::LogLevel::INFO);
    std::cout << "Production logger: silent driver/error paths make zero log-file opens; "
                 "restoration passed\n";
}
