// Windows models child and pipe faults; Linux also executes actual fork and pipe calls.
#include "sumh_namespace_test_support.hpp"
using namespace namespace_fixture;

int main() {
    std::string error;
    NamespaceOperationState state;
#ifdef _WIN32
    interrupted_read = interrupted_write = true;
#endif
    const auto invoke = [&](const std::function<bool()>& fn) {
        return run_in_init_mount_ns(fn, error, &state);
    };
    assert(!invoke([&]() {
        error = "missing /target";
        errno = ENOENT;
        return false;
    }));
    assert(errno == ENOENT && error == "missing /target");
    assert(state == NamespaceOperationState::CallbackFailed);
    assert(!invoke([&]() {
        error.assign(5000, 'x');
        errno = EEXIST;
        return false;
    }));
    assert(errno == EEXIST && error == std::string(1023, 'x'));
    assert(invoke([]() { return true; }));
    assert(error.empty());
    assert(state == NamespaceOperationState::Completed);
    switch_ok = false;
    assert(!invoke([]() {
        assert(false);
        return true;
    }));
    assert(!error.empty());
    assert(state == NamespaceOperationState::Unknown);
    switch_ok = true;
#ifdef _WIN32
    pipe_ok = false;
    assert(!invoke([]() { return true; }));
    assert(errno == EMFILE && error.find("pipe") != std::string::npos);
    assert(state == NamespaceOperationState::NotStarted);
    pipe_ok = true;
    for (bool* fault : {&drop_reply, &truncate_reply, &failed_read, &lost_child_completion}) {
        *fault = true;
        bool committed = false;
        assert(!invoke([&]() {
            committed = true;
            return true;
        }));
        assert(committed && state == NamespaceOperationState::Unknown);
        *fault = false;
    }
#else
    (void)pipe_ok;
#endif
    std::cout << "sumh_namespace_result_test: all checks passed\n";
}
