#include "kernelsu_loader.hpp"

#include "core/ksucalls.hpp"
#include "elf_symbols.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <elf.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace ksud::kernelsu_loader {

namespace {

class KptrGuard {
public:
    KptrGuard() {
        if (const auto current = read_file(kPath)) {
            original_value_.assign(trim_view(*current));
        }
        if (!original_value_.empty()) {
            (void)write_file(kPath, "1");
        }
    }

    ~KptrGuard() {
        if (!original_value_.empty()) {
            (void)write_file(kPath, original_value_);
        }
    }

    KptrGuard(const KptrGuard&) = delete;
    KptrGuard& operator=(const KptrGuard&) = delete;
    KptrGuard(KptrGuard&&) = delete;
    KptrGuard& operator=(KptrGuard&&) = delete;

private:
    static constexpr const char* kPath = "/proc/sys/kernel/kptr_restrict";
    std::string original_value_;
};

void normalize_symbol_name(std::string* name) {
    size_t pos = name->find('$');
    if (pos == std::string::npos) {
        pos = name->find(".llvm.");
    }
    if (pos != std::string::npos) {
        name->resize(pos);
    }
}

bool read_file_bytes(const char* path, std::vector<uint8_t>* buffer) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("loader: cannot open %s", path);
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        LOGE("loader: cannot stat %s", path);
        close(fd);
        return false;
    }

    buffer->resize(static_cast<size_t>(st.st_size));
    size_t filled = 0;
    while (filled < buffer->size()) {
        const ssize_t n = read(fd, buffer->data() + filled, buffer->size() - filled);
        if (n > 0) {
            filled += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        LOGE("loader: cannot read %s", path);
        close(fd);
        return false;
    }
    close(fd);

    return true;
}

int init_module_syscall(void* module_image, unsigned long len, const char* param_values) {
    return syscall(__NR_init_module, module_image, len, param_values);
}

template <typename Ehdr, typename Shdr, typename Sym>
bool patch_undefined_symbols(std::vector<uint8_t>* buffer) {
    auto symbols = collect_undefined_symbols<Ehdr, Shdr, Sym>(*buffer);
    if (!symbols) {
        LOGE("loader: invalid ELF symbol or string table");
        return false;
    }
    auto& unresolved_symbols = *symbols;

    if (unresolved_symbols.empty()) {
        return true;
    }

    // The scoped kernel interface also works when /proc/kallsyms is unavailable.
    if (get_uapi_version() >= 6) {
        std::vector<std::string> names;
        names.reserve(unresolved_symbols.size());
        for (const auto& [name, offsets] : unresolved_symbols) {
            (void)offsets;
            names.push_back(name);
        }
        if (const auto addresses = lookup_symbols(names)) {
            for (size_t i = 0; i < names.size(); ++i) {
                const auto address = (*addresses)[i];
                if (address == 0)
                    continue;
                for (const size_t offset : unresolved_symbols.at(names[i])) {
                    Sym symbol{};
                    std::memcpy(&symbol, buffer->data() + offset, sizeof(symbol));
                    symbol.st_shndx = SHN_ABS;
                    symbol.st_value = static_cast<decltype(symbol.st_value)>(address);
                    std::memcpy(buffer->data() + offset, &symbol, sizeof(symbol));
                }
            }
            return true;
        }
        LOGW("loader: kernel symbol lookup failed, using /proc/kallsyms");
    }

    KptrGuard const guard;
    // Streamed, not read whole: /proc/kallsyms is several MB and this scan stops
    // at the first module-owned symbol.
    std::string name;
    const bool opened = for_each_file_line("/proc/kallsyms", [&](std::string_view line) {
        if (unresolved_symbols.empty()) {
            return false;
        }
        std::string_view rest = line;
        const std::string_view addr_str = next_token(&rest);
        const std::string_view type = next_token(&rest);
        const std::string_view sym_name = next_token(&rest);
        (void)type;
        if (addr_str.empty() || sym_name.empty()) {
            return true;
        }

        // Kernel symbols come before module symbols in /proc/kallsyms. Stop scanning once we
        // reach module-owned entries so we don't accidentally relocate against them.
        if (!next_token(&rest).empty()) {
            return false;
        }

        // Parse the bounded view directly; hidden addresses must remain
        // unresolved so the kernel can handle them normally.
        uint64_t addr = 0;
        const auto [end, error] =
            std::from_chars(addr_str.data(), addr_str.data() + addr_str.size(), addr, 16);
        if (error != std::errc{} || end != addr_str.data() + addr_str.size() || addr == 0) {
            return true;
        }

        name.assign(sym_name);
        normalize_symbol_name(&name);

        const auto it = unresolved_symbols.find(name);
        if (it == unresolved_symbols.end()) {
            return true;
        }

        for (const size_t offset : it->second) {
            Sym symbol{};
            std::memcpy(&symbol, buffer->data() + offset, sizeof(symbol));
            symbol.st_shndx = SHN_ABS;
            symbol.st_value = static_cast<decltype(symbol.st_value)>(addr);
            std::memcpy(buffer->data() + offset, &symbol, sizeof(symbol));
        }
        unresolved_symbols.erase(it);
        return true;
    });
    if (!opened) {
        LOGE("loader: cannot open /proc/kallsyms");
        return false;
    }

    for (const auto& [name, _] : unresolved_symbols) {
        LOGW("loader: cannot find symbol: %s", name.c_str());
    }

    return true;
}

}  // namespace

bool load_module(const char* path, const std::string& param_values) {
    std::vector<uint8_t> buffer;
    if (!read_file_bytes(path, &buffer)) {
        return false;
    }

    if (buffer.size() < EI_NIDENT || memcmp(buffer.data(), ELFMAG, SELFMAG) != 0) {
        LOGE("loader: invalid ELF image");
        return false;
    }

    const unsigned char elf_class = buffer[EI_CLASS];
    bool patched = false;
    if (elf_class == ELFCLASS64) {
        patched = patch_undefined_symbols<Elf64_Ehdr, Elf64_Shdr, Elf64_Sym>(&buffer);
    } else if (elf_class == ELFCLASS32) {
        patched = patch_undefined_symbols<Elf32_Ehdr, Elf32_Shdr, Elf32_Sym>(&buffer);
    } else {
        LOGE("loader: unsupported ELF class %u", elf_class);
        return false;
    }

    if (!patched) {
        return false;
    }

    if (init_module_syscall(buffer.data(), buffer.size(), param_values.c_str()) != 0) {
        if (errno == EEXIST) {
            return true;
        }
        LOGE("loader: init_module failed: %s", strerror(errno));
        return false;
    }

    LOGI("loader: module loaded successfully");
    return true;
}

}  // namespace ksud::kernelsu_loader
