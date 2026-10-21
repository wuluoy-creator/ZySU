#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "json.hpp"

namespace sumhp::hide {
inline constexpr std::size_t HIDE_MAX_RULES = 4096;
inline constexpr std::size_t HIDE_MAX_BYTES = 4 * 1024 * 1024;
inline constexpr std::size_t HIDE_EXCHANGE_MAX_BYTES = HIDE_MAX_BYTES + 256;
inline constexpr std::size_t HIDE_NAME_MAX_BYTES = 128;
inline constexpr std::size_t HIDE_GROUP_MAX_BYTES = 128;
inline constexpr std::size_t HIDE_NOTE_MAX_BYTES = 1024;

struct Definition {
    std::string id, path;
    bool enabled = true;
    std::string name, note, group;
};
struct Document {
    std::vector<Definition> rules;
    int source_schema = 1;  // 0: legacy path array, 1: versioned definitions.
};
struct Stored {
    Document document;
    std::string revision, bytes;
    bool missing = false;
};
bool valid_id(const std::string& id);
bool valid_path(const std::string& path);
bool parse_document(const std::string& bytes, Document& document, std::string& error);
JsonValue definition_json(const Definition& value);
JsonValue document_json(const Document& document);
std::string encode_document(const Document& document);
bool parse_portable_document(const std::string& bytes, Document& document, std::string& error);
bool encode_portable_document(const Document& document, std::string& bytes, std::string& error);
bool read_store(Stored& stored, std::string& error);
std::vector<std::string> enabled_paths(const Document& document);

enum class SaveResult { Unchanged, Saved, NotSaved, Unknown };
class Transaction {
public:
    Transaction();  // No filesystem or kernel access.
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;
    bool begin(std::string& error);
    const Stored& stored() const;
    SaveResult save(const Document& document, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace sumhp::hide
