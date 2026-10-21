#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include "core/rule_inventory.hpp"

namespace {
using namespace sumhp;
using namespace sumhp::sumh;
struct Call {
    unsigned long cmd;
    std::function<int(void*)> run;
};
std::deque<Call> calls;
constexpr std::uint64_t instance = 0xf123456789abcdefULL;
sumh_rule_inventory_arg sample(std::uint32_t category = 1, std::uint64_t cursor = 0) {
    sumh_rule_inventory_arg arg{};
    arg.size = sizeof(arg);
    arg.version = 1;
    arg.category = category;
    arg.count = category ? 1 : 0;
    arg.instance_id = instance;
    arg.generation = 5;
    arg.cursor = cursor;
    arg.next_cursor = cursor + arg.count;
    arg.flags = 1;
    arg.complete = 1;
    arg.supported_categories = 0x7f;
    if (category) {
        auto& entry = arg.entry;
        entry.category = category;
        entry.id = cursor + 1;
        entry.id_kind = 1;
        if (category == 4 || category == 5) {
            entry.target_ino = UINT64_MAX;
            entry.target_dev = 123;
            entry.spoofed_ino = 11;
            entry.spoofed_dev = 22;
            entry.owner = category == 4 ? 2 : 0;
        } else {
            entry.path1_len = 2;
            std::memcpy(entry.path1, "/a", 2);
        }
        if (category == 1 || category == 3 || category == 4 || category == 5) {
            entry.path2_len = 2;
            std::memcpy(entry.path2, "/b", 2);
        }
        if (category == 7) {
            entry.id = 44;
            entry.definition_generation = 9;
            entry.id_kind = 2;
            entry.owner = 1;
        }
    }
    return arg;
}
void get(const InventoryRequest& request, const sumh_rule_inventory_arg& reply, int error = 0) {
    calls.push_back({SUMH_IOC_GET_RULE_INVENTORY, [=](void* pointer) {
                         sumh_rule_inventory_arg expected{};
                         expected.size = sizeof(expected);
                         expected.version = 1;
                         expected.category = request.category;
                         expected.cursor = request.cursor;
                         expected.expected_instance_id = request.instance_id;
                         expected.expected_generation = request.generation;
                         assert(!std::memcmp(pointer, &expected, sizeof(expected)));
                         *static_cast<sumh_rule_inventory_arg*>(pointer) = reply;
                         errno = error;
                         return error ? -1 : 0;
                     }});
}
void capabilities(int bits = SUMH_FEATURE_RULE_INVENTORY) {
    calls.push_back({SUMH_IOC_GET_VERSION, [](void* p) {
                         *static_cast<int*>(p) = 1;
                         return 0;
                     }});
    calls.push_back({SUMH_IOC_GET_FEATURES, [=](void* p) {
                         *static_cast<int*>(p) = bits;
                         return 0;
                     }});
}
void test_client() {
    InventoryPage page;
    for (std::uint32_t category = 0; category <= 7; ++category) {
        get({category}, sample(category));
        assert(rule_inventory({category}, page));
        assert(page.category == category && page.entries.size() == (category ? 1U : 0U));
    }
    for (int bad = 0; bad < 31; ++bad) {
        auto reply = sample();
        switch (bad) {
        case 0:
            reply.size--;
            break;
        case 1:
            reply.version++;
            break;
        case 2:
            reply.category++;
            break;
        case 3:
            reply.count++;
            break;
        case 4:
            reply.instance_id = 0;
            break;
        case 5:
            reply.generation = 0;
            break;
        case 6:
            reply.cursor++;
            break;
        case 7:
            reply.next_cursor++;
            break;
        case 8:
            reply.flags = 3;
            break;
        case 9:
            reply.complete++;
            break;
        case 10:
            reply.reserved++;
            break;
        case 11:
            reply.expected_instance_id++;
            break;
        case 12:
            reply.expected_generation++;
            break;
        case 13:
            reply.supported_categories = 0;
            break;
        case 14:
            reply.entry.category++;
            break;
        case 15:
            reply.entry.owner++;
            break;
        case 16:
            reply.entry.id_kind++;
            break;
        case 17:
            reply.entry.id++;
            break;
        case 18:
            reply.entry.definition_generation++;
            break;
        case 19:
            reply.entry.flags++;
            break;
        case 20:
            reply.entry.reserved++;
            break;
        case 21:
            reply.entry.target_ino++;
            break;
        case 22:
            reply.entry.target_dev++;
            break;
        case 23:
            reply.entry.spoofed_ino++;
            break;
        case 24:
            reply.entry.spoofed_dev++;
            break;
        case 25:
            reply.entry.path1_len = 4096;
            break;
        case 26:
            reply.entry.path2_len = 4096;
            break;
        case 27:
            reply.entry.path1[1] = 0;
            break;
        case 28:
            reply.entry.path2[2] = 'x';
            break;
        case 29:
            reply.entry.path1[4095] = 'x';
            break;
        case 30:
            reply.entry.path2[4095] = 'x';
            break;
        }
        get({1}, reply);
        assert(!rule_inventory({1}, page) && errno == EPROTO);
        assert(page.category == 7 && page.entries.front().id == 44);  // Output untouched.
    }
    for (std::uint32_t category : {2U, 3U, 4U, 5U, 6U, 7U}) {
        auto reply = sample(category);
        reply.entry.type = 1;
        get({category}, reply);
        assert(!rule_inventory({category}, page) && errno == EPROTO);
    }
    auto reply = sample(7);
    reply.entry.definition_generation = 0;
    get({7}, reply);
    assert(!rule_inventory({7}, page));
    reply = sample(4);
    reply.entry.path1_len = 1;
    reply.entry.path1[0] = '/';
    get({4}, reply);
    assert(!rule_inventory({4}, page));
    reply = sample(2);
    reply.entry.path2_len = 1;
    reply.entry.path2[0] = '/';
    get({2}, reply);
    assert(!rule_inventory({2}, page));
    reply = sample(2);
    reply.entry.flags = 1;
    get({2}, reply);
    assert(rule_inventory({2}, page) && page.entries[0].owner == 0);
    reply = sample(1);
    reply.count = 0;
    reply.next_cursor = 0;
    get({1}, reply);
    assert(!rule_inventory({1}, page));  // Empty must clear entry.
    reply.entry = {};
    reply.complete = 0;
    get({1}, reply);
    assert(!rule_inventory({1}, page));
    reply.complete = 1;
    get({1}, reply);
    assert(rule_inventory({1}, page) && page.entries.empty());
    get({1, instance, 5, 0}, sample());
    assert(rule_inventory({1, instance, 5, 0}, page));
    get({3, instance, 5, 0}, sample(3));
    assert(rule_inventory({3, instance, 5, 0}, page));
    reply = sample();
    reply.generation++;
    get({1, instance, 5, 0}, reply);
    assert(!rule_inventory({1, instance, 5, 0}, page));
    get({1, instance, 5, 1}, {}, ESTALE);
    assert(!rule_inventory({1, instance, 5, 1}, page) && errno == ESTALE);
    assert(!rule_inventory({1, 0, 0, 1}, page) && errno == EINVAL);
    assert(!rule_inventory({0, instance, 5, 1}, page) && errno == EINVAL);
    assert(!rule_inventory({1, instance, 0, 0}, page) && errno == EINVAL);
    assert(!rule_inventory({11}, page) && errno == EINVAL);
    assert(calls.empty());  // No mutation or automatic retry.
}
void test_command() {
    for (auto invalid : {std::vector<std::string>{"api", "inventory", "bad"},
                         {"api", "inventory", "mapping", "1"},
                         {"api", "inventory", "mapping", "0", "5", "0"},
                         {"api", "inventory", "metadata", "1", "5", "1"},
                         {"api", "inventory", "mapping", "1", "5", "18446744073709551616"},
                         {"api", "inventory", "mapping", "1", "5", "+1"}}) {
        std::ostringstream out;
        assert(inventory::command(invalid, "boot", out) == 1);
        assert(json::parse(out.str()).find("reason")->s == "invalid_request");
    }
    std::size_t total = 0;
    // More than 64 KiB of raw paths, each response bounded independently.
    for (std::uint64_t cursor = 0; cursor < 20; ++cursor) {
        auto reply = sample(1, cursor);
        reply.complete = cursor == 19;
        std::memset(reply.entry.path1, '\n', 4095);
        reply.entry.path1_len = 4095;
        std::memset(reply.entry.path2, 'b', 4095);
        reply.entry.path2_len = 4095;
        capabilities();
        const InventoryRequest request{1, cursor ? instance : 0, cursor ? 5U : 0U, cursor};
        get(request, reply);
        std::vector<std::string> args{"api", "inventory", "mapping"};
        if (cursor) {
            args.push_back(std::to_string(instance));
            args.push_back("5");
            args.push_back(std::to_string(cursor));
        }
        std::ostringstream out;
        assert(inventory::command(args, "boot-\"test", out) == 0);
        const auto parsed = json::parse(out.str());
        assert(!parsed.find("complete")->b);
        const auto& p = *parsed.find("page");
        assert(p.find("instance_id")->s == std::to_string(instance));
        assert(p.find("at_end")->b == (cursor == 19));
        assert(p.find("entries")->a[0].find("visible_path")->find("value")->s ==
               std::string(4095, '\n'));
        total += 8190;
        assert(out.str().size() < 64 * 1024);
    }
    assert(total > 64 * 1024);
    capabilities();
    get({4}, sample(4));
    std::ostringstream out;
    assert(inventory::command({"api", "inventory", "maps_managed"}, "", out) == 0);
    auto parsed = json::parse(out.str());
    assert(parsed.find("complete")->b);
    const auto& entry = parsed.find("page")->find("entries")->a[0];
    assert(entry.find("owner")->s == "managed_maps");
    assert(entry.find("target_ino")->s == "18446744073709551615");
    capabilities();
    get({1, instance, 5, 1}, {}, ESTALE);
    out.str("");
    out.clear();
    assert(inventory::command({"api", "inventory", "mapping", std::to_string(instance), "5", "1"},
                              "", out) == 1);
    parsed = json::parse(out.str());
    assert(parsed.find("status")->s == "stale" && !parsed.find("complete")->b);
    capabilities(0);
    calls.push_back({SUMH_IOC_LIST_RULES, [](void* p) {
                         auto& arg = *static_cast<sumh_syscall_list_arg*>(p);
                         assert(arg.size == 65536);
                         std::memcpy(arg.buf, "abc\n", 4);
                         arg.size = 4;
                         return 0;
                     }});
    out.str("");
    out.clear();
    assert(inventory::command({"api", "inventory"}, "", out) == 0);
    parsed = json::parse(out.str());
    assert(parsed.find("status")->s == "unsupported" && !parsed.find("complete")->b);
    assert(!parsed.find("legacy")->find("complete")->b);
    capabilities();
    get({9}, {}, EOPNOTSUPP);
    out.str("");
    out.clear();
    assert(inventory::command({"api", "inventory", "kstat"}, "", out) == 1);
    assert(json::parse(out.str()).find("reason")->s == "category_not_supported");
    assert(calls.empty());
}
void test_bytes() {
    const std::string legal = "/\xe4\xb8\xad\xf0\x9f\x98\x80\r\n\t\"\\ ";
    assert(inventory::valid_utf8(legal));
    auto parsed = json::parse(inventory::bytes_json(legal));
    assert(parsed.find("value")->s == legal && parsed.find("encoding")->s == "utf-8");
    for (const auto* invalid :
         {"\xff", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf0\x9f"})
        assert(!inventory::valid_utf8(invalid));
    parsed = json::parse(inventory::bytes_json("/\xff\n"));
    assert(parsed.find("encoding")->s == "hex" && parsed.find("value")->s == "2fff0a");
}
void test_output_budget() {
    auto reply = sample();
    std::memset(reply.entry.path1, 1, 4095);
    reply.entry.path1_len = 4095;
    std::memset(reply.entry.path2, 1, 4095);
    reply.entry.path2_len = 4095;
    capabilities();
    get({1}, reply);
    std::ostringstream out;
    assert(inventory::command({"api", "inventory", "mapping"}, "", out) == 0);
    assert(out.str().size() < 56 * 1024);  // Worst six-byte JSON escape per path byte.
    assert(json_quote(out.str()).size() + 1024 < 1024 * 1024);  // Existing daemon response budget.
    // Full ioctl -> strict client -> serializer preserves a non-UTF-8 path.
    reply = sample();
    reply.entry.path1[1] = static_cast<char>(0xff);
    capabilities();
    get({1}, reply);
    out.str("");
    out.clear();
    assert(inventory::command({"api", "inventory", "mapping"}, "", out) == 0);
    const auto path =
        *json::parse(out.str()).find("page")->find("entries")->a[0].find("visible_path");
    assert(path.find("encoding")->s == "hex" && path.find("value")->s == "2fff");
    // Legacy raw text may encode larger than a structured page, but is bounded
    // under the existing envelope limit and never claims collection completeness.
    capabilities(0);
    calls.push_back({SUMH_IOC_LIST_RULES, [](void* p) {
                         auto& arg = *static_cast<sumh_syscall_list_arg*>(p);
                         assert(arg.size == 65536);
                         std::memset(arg.buf, 1, arg.size);
                         return 0;
                     }});
    out.str("");
    out.clear();
    assert(inventory::command({"api", "inventory"}, "", out) == 0);
    assert(json_quote(out.str()).size() + 1024 < 1024 * 1024);
    assert(!json::parse(out.str()).find("legacy")->find("complete")->b);
    assert(calls.empty());
}
}  // namespace
extern "C" int ksu_sumh_ioctl(unsigned long cmd, void* arg) {
    assert(!calls.empty());
    auto call = std::move(calls.front());
    calls.pop_front();
    assert(call.cmd == cmd);
    return call.run(arg);
}
int main() {
    test_client();
    test_command();
    test_bytes();
    test_output_budget();
    std::cout << "SUMH inventory client, pagination and JSON tests passed\n";
}
