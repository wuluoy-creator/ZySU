#include "preinit.hpp"
#include "preinit_platform.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ksud::preinit {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t BLOCK = 4096;
constexpr const char* BOOTCOUNT = ".preinit_bootcount";
constexpr const char* MANIFEST = ".manifest";

struct Entry {
    fs::path relative;
    bool directory;
    uint64_t size;
    unsigned mode;
};

struct Candidate {
    std::string id;
    fs::path source;
    std::vector<Entry> entries;
    uint64_t usage = BLOCK;
    std::string manifest;
};

uint64_t blocks(uint64_t size) {
    return ((size + BLOCK - 1) / BLOCK) * BLOCK;
}

bool valid_id(std::string_view id) {
    return !id.empty() && id.front() != '.' &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}

bool real_directory(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(path, ec));
}

std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("cannot read " + path.string());
    std::string data{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad())
        throw std::runtime_error("read failed " + path.string());
    return data;
}

std::string read_optional(const fs::path& path) {
    try {
        return read(path);
    } catch (const std::exception&) {
        return {};
    }
}

void write_atomic(const fs::path& path, const std::string& data, const char* context = nullptr) {
    if (fs::is_regular_file(fs::symlink_status(path)) && read_optional(path) == data)
        return;
    const fs::path temporary = path.string() + ".tmp";
    fs::remove(temporary);  // Do not follow a stale symlink when opening the temporary.
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        stream.close();
        if (!stream)
            throw std::runtime_error("write failed " + temporary.string());
    }
    if (context)
        platform::secure(temporary, 0644, context);
    platform::sync_path(temporary);
    platform::replace_file(temporary, path);
    platform::sync_path(path.parent_path());
}

void scan(const fs::path& root, const fs::path& relative, Candidate& candidate) {
    std::vector<fs::directory_entry> children;
    for (const auto& entry : fs::directory_iterator(root / relative))
        children.push_back(entry);
    std::sort(children.begin(), children.end(), [](const auto& a, const auto& b) {
        return a.path().filename() < b.path().filename();
    });
    for (const auto& child : children) {
        const auto rel = relative / child.path().filename();
        if (rel.generic_string().find_first_of("\t\r\n") != std::string::npos)
            throw std::runtime_error("invalid preinit filename: " + rel.string());
        const auto status = child.symlink_status();
        const bool directory = fs::is_directory(status);
        if (!directory && !fs::is_regular_file(status))
            throw std::runtime_error("unsupported preinit file type: " + rel.string());
        const uint64_t size = directory ? 0 : child.file_size();
        if (size > LIMIT_TOTAL)
            throw std::length_error("preinit file exceeds 4 MiB limit");
        // Charge a block for the directory entry as well as the payload. Empty
        // files still consume metadata, and long names grow the parent directory.
        const uint64_t need = BLOCK + (directory ? BLOCK : blocks(size));
        if (need > LIMIT_TOTAL || candidate.usage > LIMIT_TOTAL - need)
            throw std::length_error("preinit directory exceeds 4 MiB limit");
        candidate.usage += need;
        candidate.entries.push_back({rel, directory, size, platform::mode(child.path())});
        const auto& saved = candidate.entries.back();
        std::ostringstream line;
        line << candidate.id << '\t' << (directory ? 'd' : 'f') << '\t' << size << '\t' << std::oct
             << (saved.mode & 0755) << '\t' << rel.generic_string() << '\n';
        candidate.manifest += line.str();
        if (directory)
            scan(root, rel, candidate);
    }
}

uint64_t disk_usage(const fs::path& path) {
    const auto status = fs::symlink_status(path);
    if (!fs::exists(status))
        return 0;
    uint64_t result = platform::allocated(path);
    if (fs::is_directory(status)) {
        for (const auto& entry : fs::directory_iterator(path))
            result += disk_usage(entry.path());
    }
    return result;
}

bool same_copy(const fs::path& destination, const Candidate& candidate) {
    try {
        if (!real_directory(destination))
            return false;
        Candidate existing;
        scan(destination, {}, existing);
        if (existing.entries.size() != candidate.entries.size())
            return false;
        for (size_t i = 0; i < existing.entries.size(); ++i) {
            const auto& want = candidate.entries[i];
            const auto& have = existing.entries[i];
            if (want.relative != have.relative || want.directory != have.directory ||
                (want.mode & 0755) != have.mode || want.size != have.size ||
                (!want.directory &&
                 read(candidate.source / want.relative) != read(destination / have.relative)))
                return false;
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void install(const fs::path& root, const Candidate& candidate, const char* context) {
    const auto temporary = root / ("." + candidate.id + ".tmp");
    const auto old = root / ("." + candidate.id + ".old");
    const auto destination = root / candidate.id;
    fs::remove_all(temporary);
    fs::create_directory(temporary);
    platform::secure(temporary, 0755, context);
    for (const auto& entry : candidate.entries) {
        const auto target = temporary / entry.relative;
        if (entry.directory) {
            fs::create_directory(target);
        } else {
            const auto source = candidate.source / entry.relative;
            if (!fs::is_regular_file(fs::symlink_status(source)) ||
                fs::file_size(source) != entry.size)
                throw std::runtime_error("preinit source changed while copying");
            fs::copy_file(source, target);
        }
        // Delay restrictive directory modes until every descendant has been copied.
        platform::secure(target, entry.directory ? 0755 : entry.mode, context);
        if (!entry.directory)
            platform::sync_path(target);
    }
    for (auto it = candidate.entries.rbegin(); it != candidate.entries.rend(); ++it) {
        if (it->directory) {
            platform::secure(temporary / it->relative, it->mode, context);
            platform::sync_path(temporary / it->relative);
        }
    }
    platform::sync_path(temporary);
    fs::remove_all(old);
    const bool had_previous = fs::exists(fs::symlink_status(destination));
    if (had_previous)
        fs::rename(destination, old);
    try {
        fs::rename(temporary, destination);
    } catch (...) {
        if (had_previous)
            fs::rename(old, destination);
        throw;
    }
    platform::sync_path(root);
    fs::remove_all(old);
}

void remove_unlisted(const fs::path& root, const std::set<std::string>& keep, bool& changed) {
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (name == MANIFEST || keep.count(name))
            continue;
        fs::remove_all(entry.path());
        if (!name.empty() && name.front() != '.')
            changed = true;
    }
}

void remove_preinit(const fs::path& directory) {
    fs::remove_all(directory / "preinit");
    fs::remove(directory / BOOTCOUNT);
}

uint64_t read_bootcount(const fs::path& directory) {
    std::istringstream input(read_optional(directory / BOOTCOUNT));
    uint64_t count = 0;
    input >> count;
    return input.fail() ? 0 : count;
}

std::string single_line(std::string value) {
    for (auto& c : value) {
        if (c == '\r' || c == '\n')
            c = ' ';
    }
    return value;
}
}  // namespace

int sync(const fs::path& directory, const std::vector<fs::path>& stale_directories,
         const Modules& modules, const char* context, bool supported, const fs::path& status_path) {
    States states;
    std::vector<Candidate> candidates;
    bool changed = false;
    int result = 0;
    try {
        for (const auto& stale : stale_directories)
            remove_preinit(stale);
        for (const auto& [id, module] : modules) {
            if (!valid_id(id))
                continue;
            const auto source = module / "preinit";
            if (!fs::exists(fs::symlink_status(source)))
                continue;
            if (!supported) {
                states[id] = {"unsupported", "kernel does not run preinit"};
                continue;
            }
            try {
                if (!real_directory(source) ||
                    !fs::is_regular_file(fs::symlink_status(source / "preinit.conf")))
                    throw std::runtime_error("preinit.conf not found or invalid file type");
                Candidate candidate{id, source, {}, BLOCK, {}};
                scan(source, {}, candidate);
                candidates.push_back(std::move(candidate));
            } catch (const std::length_error& error) {
                states[id] = {"no_space", error.what()};
            } catch (const std::exception& error) {
                states[id] = {"error", error.what()};
            }
        }
        if (!supported || candidates.empty()) {
            remove_preinit(directory);
        } else {
            fs::create_directories(directory);
            platform::secure(directory, 0755, context);
            const auto root = directory / "preinit";
            if (fs::exists(fs::symlink_status(root)) && !real_directory(root))
                fs::remove_all(root);
            fs::create_directory(root);
            platform::secure(root, 0755, context);
            const uint64_t free = platform::available(directory) + disk_usage(root);
            const uint64_t budget =
                std::min<uint64_t>(LIMIT_TOTAL, free > RESERVE ? free - RESERVE : 0);
            uint64_t used = BLOCK;
            uint64_t manifest_bytes = 0;
            std::set<std::string> wanted;
            for (const auto& candidate : candidates) {
                if (used + candidate.usage + blocks(manifest_bytes + candidate.manifest.size()) >
                    budget) {
                    states[candidate.id] = {"no_space", "metadata space limit or reserve reached"};
                    continue;
                }
                used += candidate.usage;
                manifest_bytes += candidate.manifest.size();
                wanted.insert(candidate.id);
            }
            remove_unlisted(root, wanted, changed);
            std::set<std::string> keep;
            std::ostringstream manifest;
            for (const auto& candidate : candidates) {
                if (!wanted.count(candidate.id))
                    continue;
                try {
                    const auto destination = root / candidate.id;
                    if (!same_copy(destination, candidate)) {
                        if (platform::available(directory) < candidate.usage + RESERVE &&
                            fs::exists(destination)) {
                            fs::remove_all(destination);
                            changed = true;
                        }
                        if (platform::available(directory) < candidate.usage + RESERVE) {
                            states[candidate.id] = {"no_space",
                                                    "metadata reserve would be exhausted"};
                            continue;
                        }
                        install(root, candidate, context);
                        changed = true;
                    }
                    keep.insert(candidate.id);
                    states[candidate.id] = {"ok", ""};
                    manifest << candidate.manifest;
                } catch (const std::exception& error) {
                    states[candidate.id] = {"error", error.what()};
                }
            }
            remove_unlisted(root, keep, changed);
            const auto manifest_content = manifest.str();
            const auto manifest_path = root / MANIFEST;
            if (manifest_content.empty()) {
                fs::remove(manifest_path);
            } else if (read_optional(manifest_path) != manifest_content) {
                // Atomic replacement briefly keeps both files. The manifest is
                // only an index for ksud, so discard the old index if necessary.
                const auto required = blocks(manifest_content.size()) + RESERVE;
                if (platform::available(directory) < required)
                    fs::remove(manifest_path);
                if (platform::available(directory) < required)
                    throw std::runtime_error("metadata reserve would be exhausted by manifest");
                write_atomic(manifest_path, manifest_content, context);
            }
            platform::sync_path(root);
        }
    } catch (const std::exception& error) {
        result = 1;
        for (const auto& candidate : candidates) {
            if (!states.count(candidate.id))
                states[candidate.id] = {"error", error.what()};
        }
        if (fprintf(stderr, "preinit: %s\n", error.what()) < 0)
            (void)0;
    }
    if (changed && reset_bootcount(directory) != 0)
        result = 1;
    try {
        fs::create_directories(status_path.parent_path());
        std::ostringstream output;
        for (const auto& [id, status] : states)
            output << id << ' ' << status.state << ' ' << single_line(status.message) << '\n';
        write_atomic(status_path, output.str());
    } catch (const std::exception& error) {
        if (fprintf(stderr, "preinit: cannot write status: %s\n", error.what()) < 0)
            (void)0;
        result = 1;
    }
    return result;
}

States module_states(const fs::path& directory, const fs::path& status_path) {
    States states;
    const bool bootloop = read_bootcount(directory) > BOOTCOUNT_LIMIT;
    std::istringstream input(read_optional(status_path));
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string id;
        Status status;
        if (!(row >> id >> status.state) || !valid_id(id))
            continue;
        std::getline(row >> std::ws, status.message);
        if (bootloop && status.state == "ok")
            status.state = "bootloop";
        states[id] = std::move(status);
    }
    return states;
}

int reset_bootcount(const fs::path& directory) {
    try {
        if (fs::remove(directory / BOOTCOUNT))
            platform::sync_path(directory);
        return 0;
    } catch (const std::exception& error) {
        if (fprintf(stderr, "preinit: cannot reset bootcount: %s\n", error.what()) < 0)
            (void)0;
        return 1;
    }
}

void on_boot_completed(const fs::path& directory) {
    if (read_bootcount(directory) <= BOOTCOUNT_LIMIT)
        (void)reset_bootcount(directory);
}

void print_status(const fs::path& directory, const fs::path& status_path) {
    const auto count = read_bootcount(directory);
    if (count > BOOTCOUNT_LIMIT)
        printf("preinit: disabled after %u failed boots (reset with `ksud module preinit reset`)\n",
               BOOTCOUNT_LIMIT);
    else
        printf("preinit: bootcount %llu/%u\n", static_cast<unsigned long long>(count),
               BOOTCOUNT_LIMIT);
    for (const auto& [id, status] : module_states(directory, status_path))
        printf("%s %s %s\n", id.c_str(), status.state.c_str(), status.message.c_str());
}
}  // namespace ksud::preinit
