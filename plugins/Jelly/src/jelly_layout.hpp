#pragma once
//
// 布局资料 (layout resource).
//
// The plugin carries its own layout resource instead of inheriting offsets from
// another plugin. Every entry records the field type it describes and where the
// number came from, and the resource is only ever a set of *expectations*: the
// model module reads the class layout, property offsets and reflected function
// signature back from the live process and refuses the target when the two do
// not agree. Nothing in here is treated as a compatibility proof by itself.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace jelly {

struct OffsetEntry {
    std::int64_t value{};
    std::string type;
    std::string source;
    bool present{};
};

struct SymbolSpec {
    std::string type;
    std::string source;
    std::string module;
    std::string section;
    std::string pattern;
    std::string resolve;  // "rip-rel32" or "direct"
    std::int64_t resolve_offset{};
    std::int64_t instruction_size{};
    std::int64_t addend{};
    // How many leading bytes of the function Anomaly's own observer may have
    // replaced at this entry, which is also how far the surviving bytes start
    // into it. Zero for a symbol nothing hooks.
    std::int64_t detour_bytes{};
    bool present{};

    [[nodiscard]] bool IsRipRelative() const noexcept { return resolve == "rip-rel32"; }
};

struct PropertySpec {
    std::string owner;
    std::string name;
    std::string type;
    std::string struct_name;
    std::string class_name;
    std::string inner_type;
    std::string inner_class_name;
    std::string source;
    std::int64_t element_size{};
    bool present{};
};

struct FunctionParamSpec {
    std::string name;
    std::string type;
    std::string struct_name;
    std::int64_t element_size{};
    std::int64_t offset{};
};

struct FunctionSpec {
    std::string owner;
    std::string name;
    std::string source;
    std::uint32_t num_parms{};
    std::uint32_t parms_size{};
    // Zero means "read the parameter buffer size from the live signature"; the
    // parameter list is compared either way, so a variadic wrapper such as
    // K2_SetRelativeScale3D is accepted only when its first parameter is the one
    // the plugin writes and the buffer is large enough for it.
    bool exact_size{true};
    std::uint32_t return_value_offset{};
    std::vector<FunctionParamSpec> params;
};

struct Layout {
    std::int64_t version{};
    std::string game;
    std::string source_note;
    std::map<std::string, OffsetEntry, std::less<>> offsets;
    std::map<std::string, SymbolSpec, std::less<>> symbols;
    std::map<std::string, std::string, std::less<>> class_names;
    std::map<std::string, std::vector<std::string>, std::less<>> class_families;
    std::map<std::string, PropertySpec, std::less<>> properties;
    std::map<std::string, std::vector<FunctionSpec>, std::less<>> functions;
    std::vector<std::uint8_t> process_event_prologue;
    // Carried beside the prologue because the model module validates the entry as
    // one of the two together: the prologue itself, or this much detour in front of
    // the same prologue.
    std::int64_t process_event_detour_bytes{};
};

inline constexpr std::string_view kLayoutSchemaId = "anomaly.plugin.jelly.layout";

// The detour Anomaly's public ProcessEvent observer installs on UObject::ProcessEvent
// is a five byte E9 jump, so that is the only form the model module accepts and the
// only value `detourBytes` may carry.
inline constexpr std::int64_t kProcessEventDetourBytes = 5;

// Parses the resource document into `out`. A syntax error is reported in
// `error`; a syntactically valid document whose entries are incomplete still
// parses, and LayoutProblems() then lists what is missing.
bool ParseLayout(
    const std::string_view document, Layout& out, std::string& error) noexcept;

// The same signature with its first `skip` bytes dropped, which is how a symbol is
// found once something has overwritten them: the bytes that survive the patch are
// still unique to the function. Returns false when the pattern holds fewer tokens
// than `skip`, or when a dropped token is not one whole byte, because the remaining
// bytes would then not be a byte boundary of the recorded signature.
[[nodiscard]] bool DropLeadingBytes(
    std::string_view pattern, std::int64_t skip, std::string& out);

// Every entry the model module needs. Empty means the resource describes a layout
// this build can use.
[[nodiscard]] std::vector<std::string> LayoutProblems(const Layout& layout);

[[nodiscard]] std::int64_t LayoutOffset(
    const Layout& layout, const std::string_view key, const std::int64_t fallback) noexcept;

[[nodiscard]] std::string_view LayoutName(
    const Layout& layout, const std::string_view key) noexcept;

}  // namespace jelly
