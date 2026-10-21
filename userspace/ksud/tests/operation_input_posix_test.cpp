#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include "core/operation_submit.hpp"
int main() {
    using namespace sumhp::state;
    char directory[] = "/tmp/sumhp-input-XXXXXX";
    assert(mkdtemp(directory));
    const std::string path = std::string(directory) + "/input", sym = path + "-sym",
                      hard = path + "-hard";
    const std::string data = "{\"schema_version\":1,\"args\":[\"debug\",\"enable\"]}";
    {
        std::ofstream file(path);
        file << data;
    }
    std::string bytes, error;
    assert(read_operation_input(path, bytes, error) && bytes == data);
    assert(symlink(path.c_str(), sym.c_str()) == 0);
    assert(!read_operation_input(sym, bytes, error));
    assert(link(path.c_str(), hard.c_str()) == 0);
    assert(!read_operation_input(path, bytes, error));
    assert(unlink(hard.c_str()) == 0);
    assert(!read_operation_input(directory, bytes, error));
    assert(!read_operation_input("/dev/null", bytes, error));
    const int saved = dup(0);
    assert(saved >= 0);
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    assert(write(descriptors[1], data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    assert(close(descriptors[1]) == 0 && dup2(descriptors[0], 0) == 0);
    close(descriptors[0]);
    const auto old_flags = fcntl(0, F_GETFL);
    assert(read_operation_input("-", bytes, error) && bytes == data &&
           fcntl(0, F_GETFL) == old_flags);
    assert(pipe(descriptors) == 0 && dup2(descriptors[0], 0) == 0);
    close(descriptors[0]);
    auto start = std::chrono::steady_clock::now();
    assert(!read_operation_input("-", bytes, error) && error == "input_timeout");
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(3));
    close(descriptors[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    assert(write(descriptors[1], data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    assert(shutdown(descriptors[1], SHUT_WR) == 0 && dup2(descriptors[0], 0) == 0);
    close(descriptors[0]);
    assert(read_operation_input("-", bytes, error) && bytes == data);
    close(descriptors[1]);
    assert(dup2(saved, 0) == 0);
    close(saved);
    unlink(sym.c_str());
    unlink(path.c_str());
    rmdir(directory);
    std::cout << "Real POSIX operation input file/pipe/socket regressions passed\n";
}
