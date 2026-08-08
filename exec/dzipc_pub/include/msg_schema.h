#ifndef DZIPC_PUB_MSG_SCHEMA_H
#define DZIPC_PUB_MSG_SCHEMA_H

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "ipc_msg/ipc_msg_base/generic_message.hpp"

namespace dzipc_pub {

// =====================================================================
// .msg file schema model
// =====================================================================

struct MsgField
{
    std::string name;
    uint8_t type{0};          // dzIPC::FieldType byte
    std::string nested_type;  // PascalCase name of nested message type (empty if primitive)
};

struct MsgSchema
{
    std::string type_name;  // PascalCase class name, e.g. "StdVector"
    std::vector<MsgField> fields;

    const MsgField* find_field(const std::string& name) const
    {
        for (const auto& f : fields)
        {
            if (f.name == name) return &f;
        }
        return nullptr;
    }
};

/// Convert snake_case to PascalCase: "std_vector" → "StdVector".
/// Already-PascalCase input ("StdHeader") passes through unchanged.
inline std::string snake_to_pascal(const std::string& snake)
{
    std::string result;
    bool capitalize = true;
    for (char c : snake)
    {
        if (c == '_')
        {
            capitalize = true;
        }
        else if (capitalize)
        {
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalize = false;
        }
        else
        {
            result += c;
        }
    }
    return result;
}

/// Map msg-type-string (e.g. "int32", "float64[]", "string") → FieldType byte
inline uint8_t msg_field_type_byte(const std::string& type_str, bool& is_nested)
{
    using namespace dzIPC;
    is_nested = false;
    bool is_array = false;
    std::string base = type_str;
    if (base.size() > 2 && base[base.size() - 2] == '[' && base[base.size() - 1] == ']')
    {
        is_array = true;
        base = base.substr(0, base.size() - 2);
    }

    if (base == "bool")    return is_array ? FT_BOOL_ARRAY : FT_BOOL;
    if (base == "int8")    return is_array ? FT_INT8_ARRAY : FT_INT8;
    if (base == "uint8")   return is_array ? FT_UINT8_ARRAY : FT_UINT8;
    if (base == "int16")   return is_array ? FT_INT16_ARRAY : FT_INT16;
    if (base == "uint16")  return is_array ? FT_UINT16_ARRAY : FT_UINT16;
    if (base == "int32")   return is_array ? FT_INT32_ARRAY : FT_INT32;
    if (base == "uint32")  return is_array ? FT_UINT32_ARRAY : FT_UINT32;
    if (base == "int64")   return is_array ? FT_INT64_ARRAY : FT_INT64;
    if (base == "uint64")  return is_array ? FT_UINT64_ARRAY : FT_UINT64;
    if (base == "float32") return is_array ? FT_FLOAT32_ARRAY : FT_FLOAT32;
    if (base == "float64") return is_array ? FT_FLOAT64_ARRAY : FT_FLOAT64;
    if (base == "string")  return is_array ? FT_STRING_ARRAY : FT_STRING;

    is_nested = true;
    return is_array ? FT_NESTED_ARRAY : FT_NESTED;
}

inline const char* field_type_name(uint8_t type)
{
    using namespace dzIPC;
    switch (type)
    {
    case FT_BOOL: return "bool";
    case FT_INT8: return "int8";
    case FT_UINT8: return "uint8";
    case FT_INT16: return "int16";
    case FT_UINT16: return "uint16";
    case FT_INT32: return "int32";
    case FT_UINT32: return "uint32";
    case FT_INT64: return "int64";
    case FT_UINT64: return "uint64";
    case FT_FLOAT32: return "float32";
    case FT_FLOAT64: return "float64";
    case FT_STRING: return "string";
    case FT_BOOL_ARRAY: return "bool[]";
    case FT_INT8_ARRAY: return "int8[]";
    case FT_UINT8_ARRAY: return "uint8[]";
    case FT_INT16_ARRAY: return "int16[]";
    case FT_UINT16_ARRAY: return "uint16[]";
    case FT_INT32_ARRAY: return "int32[]";
    case FT_UINT32_ARRAY: return "uint32[]";
    case FT_INT64_ARRAY: return "int64[]";
    case FT_UINT64_ARRAY: return "uint64[]";
    case FT_FLOAT32_ARRAY: return "float32[]";
    case FT_FLOAT64_ARRAY: return "float64[]";
    case FT_STRING_ARRAY: return "string[]";
    case FT_NESTED: return "nested";
    case FT_NESTED_ARRAY: return "nested[]";
    default: return "unknown";
    }
}

/// Parse a single .msg file and return its schema.
/// Lines: "<type> <name>"; '#' comments and blank lines are skipped.
inline MsgSchema parse_msg_file(const std::string& file_path)
{
    MsgSchema schema;
    // Derive type_name from filename: "path/std_vector.msg" → "StdVector"
    auto slash = file_path.find_last_of("/\\");
    std::string basename = (slash != std::string::npos) ? file_path.substr(slash + 1) : file_path;
    auto dot = basename.find_last_of('.');
    if (dot != std::string::npos) basename = basename.substr(0, dot);
    schema.type_name = snake_to_pascal(basename);

    std::ifstream file(file_path);
    if (!file.is_open())
    {
        std::fprintf(stderr, "Warning: cannot open .msg file '%s'\n", file_path.c_str());
        return schema;
    }

    std::string line;
    while (std::getline(file, line))
    {
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start >= line.size()) continue;  // empty line
        if (line[start] == '#') continue;    // comment

        size_t end = line.size();
        while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' || line[end - 1] == '\r')) --end;

        std::string content = line.substr(start, end - start);

        // Split at first whitespace: "<type> <name>"
        auto space = content.find_first_of(" \t");
        if (space == std::string::npos) continue;  // malformed line

        std::string type_str = content.substr(0, space);
        std::string field_name = content.substr(space + 1);

        size_t ns = 0;
        while (ns < field_name.size() && (field_name[ns] == ' ' || field_name[ns] == '\t')) ++ns;
        field_name = field_name.substr(ns);

        if (field_name.empty() || type_str.empty()) continue;

        MsgField f;
        f.name = field_name;
        bool is_nested = false;
        f.type = msg_field_type_byte(type_str, is_nested);
        if (is_nested)
        {
            std::string base = type_str;
            if (base.size() > 2 && base[base.size() - 2] == '[' && base[base.size() - 1] == ']')
                base = base.substr(0, base.size() - 2);
            // Normalize: .msg files declare nested types either as snake_case
            // ("pose") or PascalCase ("StdHeader"); both map to PascalCase.
            f.nested_type = snake_to_pascal(base);
        }
        schema.fields.push_back(f);
    }
    return schema;
}

/// Search msg_dir recursively for a .msg file whose PascalCase name matches type_name
inline std::string find_msg_file(const std::string& msg_dir, const std::string& type_name)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(msg_dir, ec))
    {
        std::fprintf(stderr, "Warning: '%s' is not a directory (ec=%s)\n",
                     msg_dir.c_str(), ec.message().c_str());
        return {};
    }

    for (auto it = fs::recursive_directory_iterator(msg_dir, ec);
         it != fs::recursive_directory_iterator(); ++it)
    {
        if (ec)
        {
            std::fprintf(stderr, "Warning: error scanning '%s': %s\n",
                         msg_dir.c_str(), ec.message().c_str());
            ec.clear();
            continue;
        }
        if (!it->is_regular_file()) continue;
        std::string path = it->path().string();
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".msg") != 0) continue;

        std::string basename = it->path().stem().string();
        if (snake_to_pascal(basename) == type_name)
        {
            return path;
        }
    }
    return {};
}

/// Loads and caches .msg schemas by PascalCase type name, resolving nested
/// types recursively so that default messages can be built depth-first.
class SchemaRegistry
{
public:
    explicit SchemaRegistry(std::string msg_dir)
        : msg_dir_(std::move(msg_dir))
    {}

    /// Load a schema (and, best-effort, all nested schemas it references).
    /// Returns nullptr if the root .msg file cannot be found.
    const MsgSchema* load(const std::string& type_name)
    {
        return load_recursive(type_name, 0);
    }

    /// Lookup without loading.
    const MsgSchema* get(const std::string& type_name) const
    {
        auto it = cache_.find(type_name);
        return (it != cache_.end()) ? &it->second : nullptr;
    }

    const std::string& msg_dir() const { return msg_dir_; }

private:
    static constexpr int kMaxDepth = 16;  // guards against recursive .msg definitions

    const MsgSchema* load_recursive(const std::string& type_name, int depth)
    {
        auto it = cache_.find(type_name);
        if (it != cache_.end()) return &it->second;
        if (depth > kMaxDepth)
        {
            std::fprintf(stderr, "Warning: nested .msg depth exceeds %d at type '%s'\n",
                         kMaxDepth, type_name.c_str());
            return nullptr;
        }

        std::string path = find_msg_file(msg_dir_, type_name);
        if (path.empty()) return nullptr;

        MsgSchema schema = parse_msg_file(path);
        auto [pos, inserted] = cache_.emplace(type_name, std::move(schema));
        // Resolve nested types after insertion so self-references terminate.
        for (const auto& f : pos->second.fields)
        {
            if (!f.nested_type.empty())
            {
                if (load_recursive(f.nested_type, depth + 1) == nullptr)
                {
                    std::fprintf(stderr,
                                 "Warning: nested type '%s' (field '%s' of '%s') has no .msg file in '%s'\n",
                                 f.nested_type.c_str(), f.name.c_str(), type_name.c_str(), msg_dir_.c_str());
                }
            }
        }
        return &pos->second;
    }

    std::string msg_dir_;
    std::unordered_map<std::string, MsgSchema> cache_;
};

}  // namespace dzipc_pub

#endif  // DZIPC_PUB_MSG_SCHEMA_H
