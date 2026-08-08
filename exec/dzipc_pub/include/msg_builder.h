#ifndef DZIPC_PUB_MSG_BUILDER_H
#define DZIPC_PUB_MSG_BUILDER_H

#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "msg_schema.h"

namespace dzipc_pub {

// =====================================================================
// Default message construction (recursive over nested schemas)
// =====================================================================

/// Populate `msg` with default values for every field of `schema`.
/// Nested fields are built recursively from their own .msg schemas.
/// Returns false if any nested schema is missing (an empty nested message
/// is still inserted so the result remains publishable).
inline bool build_default_message(dzIPC::GenericMessage& msg, const MsgSchema& schema,
                                  SchemaRegistry& registry, int depth = 0)
{
    using namespace dzIPC;
    if (depth > 16) return false;

    bool ok = true;
    for (const auto& f : schema.fields)
    {
        switch (f.type)
        {
        case FT_BOOL:    msg.set_bool(f.name, false); break;
        case FT_INT8:    msg.set_int8(f.name, 0); break;
        case FT_UINT8:   msg.set_uint8(f.name, 0); break;
        case FT_INT16:   msg.set_int16(f.name, 0); break;
        case FT_UINT16:  msg.set_uint16(f.name, 0); break;
        case FT_INT32:   msg.set_int32(f.name, 0); break;
        case FT_UINT32:  msg.set_uint32(f.name, 0); break;
        case FT_INT64:   msg.set_int64(f.name, 0); break;
        case FT_UINT64:  msg.set_uint64(f.name, 0); break;
        case FT_FLOAT32: msg.set_float32(f.name, 0.0f); break;
        case FT_FLOAT64: msg.set_float64(f.name, 0.0); break;
        case FT_STRING:  msg.set_string(f.name, ""); break;
        case FT_BOOL_ARRAY:    msg.set_bool_array(f.name, {}); break;
        case FT_INT8_ARRAY:    msg.set_int8_array(f.name, {}); break;
        case FT_UINT8_ARRAY:   msg.set_uint8_array(f.name, {}); break;
        case FT_INT16_ARRAY:   msg.set_int16_array(f.name, {}); break;
        case FT_UINT16_ARRAY:  msg.set_uint16_array(f.name, {}); break;
        case FT_INT32_ARRAY:   msg.set_int32_array(f.name, {}); break;
        case FT_UINT32_ARRAY:  msg.set_uint32_array(f.name, {}); break;
        case FT_INT64_ARRAY:   msg.set_int64_array(f.name, {}); break;
        case FT_UINT64_ARRAY:  msg.set_uint64_array(f.name, {}); break;
        case FT_FLOAT32_ARRAY: msg.set_float32_array(f.name, {}); break;
        case FT_FLOAT64_ARRAY: msg.set_float64_array(f.name, {}); break;
        case FT_STRING_ARRAY:  msg.set_string_array(f.name, {}); break;
        case FT_NESTED: {
            dzIPC::GenericMessage nested;
            const MsgSchema* nested_schema =
                f.nested_type.empty() ? nullptr : registry.load(f.nested_type);
            if (nested_schema != nullptr)
            {
                ok = build_default_message(nested, *nested_schema, registry, depth + 1) && ok;
            }
            else
            {
                ok = false;
            }
            msg.set_nested(f.name, nested);
            break;
        }
        case FT_NESTED_ARRAY:
            msg.set_nested_array(f.name, {});
            break;
        default:
            break;
        }
    }
    return ok;
}

// =====================================================================
// Field overrides
//
// Accepted forms (rostopic-style, type inferred from schema):
//   name=value            e.g. x=42   greeting=hello world
//   arr=[1,2,3]           array field (brackets optional: arr=1,2,3)
//   pose.x=1.5            dotted path into nested messages
// Legacy form (type validated against schema):
//   name:type:value       e.g. x:int32:42
// =====================================================================

struct FieldOverride
{
    std::string path;           // possibly dotted, e.g. "pose.x"
    std::string value;
    std::string declared_type;  // only set for the legacy name:type:value form
};

/// Parse an override spec into path/value. Returns error text, empty on success.
inline std::string parse_override_spec(const std::string& spec, FieldOverride& out)
{
    auto eq = spec.find('=');
    if (eq != std::string::npos)
    {
        out.path = spec.substr(0, eq);
        out.value = spec.substr(eq + 1);
        out.declared_type.clear();
        if (out.path.empty()) return "empty field name in '" + spec + "'";
        return {};
    }

    // Legacy name:type:value
    auto colon1 = spec.find(':');
    if (colon1 == std::string::npos)
        return "invalid field spec '" + spec + "' (expected name=value or name:type:value)";
    auto colon2 = spec.find(':', colon1 + 1);
    if (colon2 == std::string::npos)
        return "invalid field spec '" + spec + "' (expected name=value or name:type:value)";

    out.path = spec.substr(0, colon1);
    out.declared_type = spec.substr(colon1 + 1, colon2 - colon1 - 1);
    out.value = spec.substr(colon2 + 1);
    if (out.path.empty()) return "empty field name in '" + spec + "'";
    return {};
}

namespace detail {

inline std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

/// Split an array literal "[1, 2, 3]" or "1,2,3" into trimmed elements.
/// An empty (or "[]") value yields an empty array.
inline std::vector<std::string> split_array_literal(const std::string& raw)
{
    std::string body = trim(raw);
    if (body.size() >= 2 && body.front() == '[' && body.back() == ']')
        body = body.substr(1, body.size() - 2);
    body = trim(body);

    std::vector<std::string> items;
    if (body.empty()) return items;
    size_t start = 0;
    while (true)
    {
        size_t comma = body.find(',', start);
        if (comma == std::string::npos)
        {
            items.push_back(trim(body.substr(start)));
            break;
        }
        items.push_back(trim(body.substr(start, comma - start)));
        start = comma + 1;
    }
    return items;
}

inline bool parse_bool(const std::string& s, std::string& err)
{
    if (s == "true" || s == "1" || s == "True") return true;
    if (s == "false" || s == "0" || s == "False") return false;
    err = "invalid bool value '" + s + "' (expected true/false/1/0)";
    return false;
}

inline int64_t parse_signed(const std::string& s, int64_t lo, int64_t hi, std::string& err)
{
    try
    {
        size_t pos = 0;
        int64_t v = std::stoll(s, &pos);
        if (pos != s.size()) throw std::invalid_argument("trailing chars");
        if (v < lo || v > hi)
        {
            err = "value " + s + " out of range [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
            return 0;
        }
        return v;
    }
    catch (const std::exception&)
    {
        err = "invalid integer value '" + s + "'";
        return 0;
    }
}

inline uint64_t parse_unsigned(const std::string& s, uint64_t hi, std::string& err)
{
    try
    {
        if (!s.empty() && s[0] == '-')
        {
            err = "negative value '" + s + "' for unsigned field";
            return 0;
        }
        size_t pos = 0;
        uint64_t v = std::stoull(s, &pos);
        if (pos != s.size()) throw std::invalid_argument("trailing chars");
        if (v > hi)
        {
            err = "value " + s + " out of range [0, " + std::to_string(hi) + "]";
            return 0;
        }
        return v;
    }
    catch (const std::exception&)
    {
        err = "invalid unsigned value '" + s + "'";
        return 0;
    }
}

inline double parse_double(const std::string& s, std::string& err)
{
    try
    {
        size_t pos = 0;
        double v = std::stod(s, &pos);
        if (pos != s.size()) throw std::invalid_argument("trailing chars");
        return v;
    }
    catch (const std::exception&)
    {
        err = "invalid floating-point value '" + s + "'";
        return 0.0;
    }
}

template<typename T, typename Parse>
inline std::vector<T> parse_array(const std::string& raw, Parse parse_one, std::string& err)
{
    std::vector<T> out;
    for (const auto& item : split_array_literal(raw))
    {
        T v = parse_one(item, err);
        if (!err.empty()) return {};
        out.push_back(v);
    }
    return out;
}

/// Set a leaf field on `msg` according to the schema-declared type.
inline std::string set_leaf(dzIPC::GenericMessage& msg, const MsgField& field, const std::string& value)
{
    using namespace dzIPC;
    std::string err;
    switch (field.type)
    {
    case FT_BOOL:    { bool v = parse_bool(value, err); if (err.empty()) msg.set_bool(field.name, v); break; }
    case FT_INT8:    { auto v = parse_signed(value, INT8_MIN, INT8_MAX, err); if (err.empty()) msg.set_int8(field.name, static_cast<int8_t>(v)); break; }
    case FT_UINT8:   { auto v = parse_unsigned(value, UINT8_MAX, err); if (err.empty()) msg.set_uint8(field.name, static_cast<uint8_t>(v)); break; }
    case FT_INT16:   { auto v = parse_signed(value, INT16_MIN, INT16_MAX, err); if (err.empty()) msg.set_int16(field.name, static_cast<int16_t>(v)); break; }
    case FT_UINT16:  { auto v = parse_unsigned(value, UINT16_MAX, err); if (err.empty()) msg.set_uint16(field.name, static_cast<uint16_t>(v)); break; }
    case FT_INT32:   { auto v = parse_signed(value, INT32_MIN, INT32_MAX, err); if (err.empty()) msg.set_int32(field.name, static_cast<int32_t>(v)); break; }
    case FT_UINT32:  { auto v = parse_unsigned(value, UINT32_MAX, err); if (err.empty()) msg.set_uint32(field.name, static_cast<uint32_t>(v)); break; }
    case FT_INT64:   { auto v = parse_signed(value, INT64_MIN, INT64_MAX, err); if (err.empty()) msg.set_int64(field.name, v); break; }
    case FT_UINT64:  { auto v = parse_unsigned(value, UINT64_MAX, err); if (err.empty()) msg.set_uint64(field.name, v); break; }
    case FT_FLOAT32: { auto v = parse_double(value, err); if (err.empty()) msg.set_float32(field.name, static_cast<float>(v)); break; }
    case FT_FLOAT64: { auto v = parse_double(value, err); if (err.empty()) msg.set_float64(field.name, v); break; }
    case FT_STRING:  msg.set_string(field.name, value); break;

    case FT_BOOL_ARRAY: {
        auto v = parse_array<uint8_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<uint8_t>(parse_bool(s, e) ? 1 : 0); }, err);
        if (err.empty()) msg.set_bool_array(field.name, v);
        break;
    }
    case FT_INT8_ARRAY: {
        auto v = parse_array<int8_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<int8_t>(parse_signed(s, INT8_MIN, INT8_MAX, e)); }, err);
        if (err.empty()) msg.set_int8_array(field.name, v);
        break;
    }
    case FT_UINT8_ARRAY: {
        auto v = parse_array<uint8_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<uint8_t>(parse_unsigned(s, UINT8_MAX, e)); }, err);
        if (err.empty()) msg.set_uint8_array(field.name, v);
        break;
    }
    case FT_INT16_ARRAY: {
        auto v = parse_array<int16_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<int16_t>(parse_signed(s, INT16_MIN, INT16_MAX, e)); }, err);
        if (err.empty()) msg.set_int16_array(field.name, v);
        break;
    }
    case FT_UINT16_ARRAY: {
        auto v = parse_array<uint16_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<uint16_t>(parse_unsigned(s, UINT16_MAX, e)); }, err);
        if (err.empty()) msg.set_uint16_array(field.name, v);
        break;
    }
    case FT_INT32_ARRAY: {
        auto v = parse_array<int32_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<int32_t>(parse_signed(s, INT32_MIN, INT32_MAX, e)); }, err);
        if (err.empty()) msg.set_int32_array(field.name, v);
        break;
    }
    case FT_UINT32_ARRAY: {
        auto v = parse_array<uint32_t>(value, [](const std::string& s, std::string& e) {
            return static_cast<uint32_t>(parse_unsigned(s, UINT32_MAX, e)); }, err);
        if (err.empty()) msg.set_uint32_array(field.name, v);
        break;
    }
    case FT_INT64_ARRAY: {
        auto v = parse_array<int64_t>(value, [](const std::string& s, std::string& e) {
            return parse_signed(s, INT64_MIN, INT64_MAX, e); }, err);
        if (err.empty()) msg.set_int64_array(field.name, v);
        break;
    }
    case FT_UINT64_ARRAY: {
        auto v = parse_array<uint64_t>(value, [](const std::string& s, std::string& e) {
            return parse_unsigned(s, UINT64_MAX, e); }, err);
        if (err.empty()) msg.set_uint64_array(field.name, v);
        break;
    }
    case FT_FLOAT32_ARRAY: {
        auto v = parse_array<float>(value, [](const std::string& s, std::string& e) {
            return static_cast<float>(parse_double(s, e)); }, err);
        if (err.empty()) msg.set_float32_array(field.name, v);
        break;
    }
    case FT_FLOAT64_ARRAY: {
        auto v = parse_array<double>(value, [](const std::string& s, std::string& e) {
            return parse_double(s, e); }, err);
        if (err.empty()) msg.set_float64_array(field.name, v);
        break;
    }
    case FT_STRING_ARRAY: {
        std::vector<std::string> v = split_array_literal(value);
        msg.set_string_array(field.name, v);
        break;
    }

    case FT_NESTED:
    case FT_NESTED_ARRAY:
        err = std::string("field '") + field.name
              + "' is a nested message; override its members with dotted paths (e.g. "
              + field.name + ".x=1.5)";
        break;
    default:
        err = "unsupported field type for override";
        break;
    }
    return err;
}

inline std::string apply_override_recursive(dzIPC::GenericMessage& msg, const MsgSchema& schema,
                                            SchemaRegistry& registry, const FieldOverride& ov,
                                            const std::string& path_left)
{
    using namespace dzIPC;
    auto dot = path_left.find('.');
    std::string head = (dot == std::string::npos) ? path_left : path_left.substr(0, dot);

    const MsgField* field = schema.find_field(head);
    if (field == nullptr)
    {
        std::string err = "message type '" + schema.type_name + "' has no field '" + head + "'. Fields:";
        for (const auto& f : schema.fields) err += " " + f.name;
        return err;
    }

    if (dot == std::string::npos)
    {
        // Leaf: validate legacy declared type against the schema, then set.
        if (!ov.declared_type.empty() && ov.declared_type != field_type_name(field->type))
        {
            return "field '" + head + "' is declared as '" + field_type_name(field->type)
                   + "' in the .msg schema, but override says '" + ov.declared_type + "'";
        }
        return set_leaf(msg, *field, ov.value);
    }

    // Intermediate path segment must be a (non-array) nested message.
    if (field->type != FT_NESTED)
    {
        return "field '" + head + "' is '" + field_type_name(field->type)
               + "', dotted paths can only traverse nested message fields";
    }
    const MsgSchema* nested_schema = registry.load(field->nested_type);
    if (nested_schema == nullptr)
    {
        return "no .msg schema found for nested type '" + field->nested_type + "'";
    }

    dzIPC::GenericMessage child;
    try
    {
        child = msg.get_nested(head);
    }
    catch (const std::exception&)
    {
        // Field missing on the message (defaults not built) — start from defaults.
        build_default_message(child, *nested_schema, registry);
    }
    std::string err =
        apply_override_recursive(child, *nested_schema, registry, ov, path_left.substr(dot + 1));
    if (err.empty())
    {
        msg.set_nested(head, child);
    }
    return err;
}

}  // namespace detail

/// Apply a parsed override to `msg`, validating the path and value against
/// the schema. Returns error text, empty on success.
inline std::string apply_field_override(dzIPC::GenericMessage& msg, const MsgSchema& schema,
                                        SchemaRegistry& registry, const FieldOverride& ov)
{
    if (ov.path.empty()) return "empty field path";
    return detail::apply_override_recursive(msg, schema, registry, ov, ov.path);
}

/// Convenience: parse + apply a raw spec string.
inline std::string apply_field_override(dzIPC::GenericMessage& msg, const MsgSchema& schema,
                                        SchemaRegistry& registry, const std::string& spec)
{
    FieldOverride ov;
    std::string err = parse_override_spec(spec, ov);
    if (!err.empty()) return err;
    return apply_field_override(msg, schema, registry, ov);
}

}  // namespace dzipc_pub

#endif  // DZIPC_PUB_MSG_BUILDER_H
