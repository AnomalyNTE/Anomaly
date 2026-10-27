#include "anomaly/signature_updater.hpp"

#include "anomaly/reliable_storage.hpp"

extern "C" {
#include "hde64.h"
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <tuple>

namespace anomaly {
namespace {

constexpr std::size_t kWindowBytes = 128;
constexpr std::size_t kMinimumFixedBytes = 24;
constexpr std::size_t kMaximumCacheBytes = 1024 * 1024;
constexpr std::size_t kMaximumRecords = 1024;
constexpr auto kCacheFile = L"signatures.json";
using Key = std::tuple<std::wstring, std::string, std::string, std::string>;

Key RecipeKey(const SignatureScanContext& context, std::string_view contract,
              std::string_view original, std::string_view kind = "relocation:") {
    auto module = context.module.name;
    std::ranges::transform(module, module.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    // Canonicalize whitespace and hex case using the same parser as CLI scan.
    std::string pattern;
    constexpr char hex[] = "0123456789ABCDEF";
    const auto parsed = ue5mem::Pattern::Parse(original);
    for (const auto byte : parsed.Bytes()) {
        if (!pattern.empty()) pattern.push_back(' ');
        pattern.push_back(byte.mask & 0xF0 ? hex[byte.value >> 4] : '?');
        pattern.push_back(byte.mask & 0x0F ? hex[byte.value & 15] : '?');
    }
    return {std::move(module), context.section.name, std::move(pattern),
        std::string(kind) + std::string(contract)};
}

struct DecodedInstruction {
    hde64s code{};
    std::size_t immediate{};
    std::size_t displacement{};
    std::size_t operands{};
};

std::optional<DecodedInstruction> Decode(
    std::span<const ue5mem::PatternByte> source, std::size_t offset) {
    std::array<std::uint8_t, 32> bytes{};
    const auto remaining = (std::min)(bytes.size(), source.size() - offset);
    for (std::size_t i{}; i < remaining; ++i) bytes[i] = source[offset + i].value;
    DecodedInstruction result;
    auto& decoded = result.code;
    hde64_disasm(bytes.data(), &decoded);
    if ((decoded.flags & F_ERROR) || !decoded.len || decoded.len > remaining ||
        decoded.opcode == 0xCC || decoded.opcode == 0xC4 ||
        decoded.opcode == 0xC5 || decoded.opcode == 0x62) return std::nullopt;
    const auto flags = decoded.flags & (F_IMM8 | F_IMM16 | F_IMM32 | F_IMM64);
    if (flags && (flags & (flags - 1))) return std::nullopt;
    result.immediate = decoded.flags & F_IMM64 ? 8 :
        decoded.flags & F_IMM32 ? 4 : decoded.flags & F_IMM16 ? 2 :
        decoded.flags & F_IMM8 ? 1 : 0;
    result.displacement = decoded.flags & F_DISP32 ? 4 :
        decoded.flags & F_DISP16 ? 2 : decoded.flags & F_DISP8 ? 1 : 0;
    if (result.immediate + result.displacement > decoded.len) return std::nullopt;
    result.operands = decoded.len - result.immediate - result.displacement;
    if (std::ranges::any_of(source.subspan(offset, result.operands), [](auto byte) {
            return byte.mask != 0xFF;
        })) return std::nullopt;
    return result;
}

bool EndsBlock(const hde64s& code) {
    return code.opcode == 0xC3 || code.opcode == 0xC2 ||
        code.opcode == 0xE9 || code.opcode == 0xEB ||
        (code.opcode == 0xFF && (code.modrm_reg == 4 || code.modrm_reg == 5));
}

struct MemberSite {
    std::size_t instruction{};
    std::size_t binding{};
    bool operator==(const MemberSite&) const = default;
};

struct MemberRecipe {
    std::string pattern;
    std::vector<MemberSite> sites;
};

std::string Normalize(std::span<const ue5mem::PatternByte> source,
                      std::span<const MemberSite> members = {},
                      std::size_t minimum_fixed = kMinimumFixedBytes) {
    std::vector<ue5mem::PatternByte> result;
    std::size_t offset{};
    std::size_t instructions{};
    while (offset < source.size()) {
        const auto instruction = Decode(source, offset);
        if (!instruction) break;
        const auto& decoded = instruction->code;
        const bool rip_relative = (decoded.flags & F_MODRM) &&
            decoded.modrm_mod == 0 && decoded.modrm_rm == 5 && !decoded.p_67;
        const bool member = std::ranges::any_of(members, [&](const auto& site) {
            return site.instruction == offset;
        });
        for (std::size_t i{}; i < decoded.len; ++i) {
            auto byte = source[offset + i];
            if (((rip_relative || member) && i >= instruction->operands &&
                    i < instruction->operands + instruction->displacement) ||
                ((decoded.flags & F_RELATIVE) && i >= decoded.len - instruction->immediate)) {
                byte = {};
            }
            result.push_back(byte);
        }
        ++instructions;
        offset += decoded.len;
        if (EndsBlock(decoded)) break;
    }
    if (instructions < 4 || static_cast<std::size_t>(std::ranges::count_if(result, [](auto byte) {
            return byte.mask == 0xFF;
        })) < minimum_fixed) return {};
    constexpr char hex[] = "0123456789ABCDEF";
    std::string pattern;
    for (const auto byte : result) {
        if (!pattern.empty()) pattern.push_back(' ');
        pattern.push_back(byte.mask & 0xF0 ? hex[byte.value >> 4] : '?');
        pattern.push_back(byte.mask & 0x0F ? hex[byte.value & 15] : '?');
    }
    return pattern;
}

bool ValidCachedPattern(std::string_view original,
    std::span<const ue5mem::PatternByte> learned, std::span<const MemberSite> sites = {},
    std::size_t minimum_fixed = kMinimumFixedBytes) {
    auto fixed = std::vector<ue5mem::PatternByte>(learned.begin(), learned.end());
    for (auto& byte : fixed) byte.mask = 0xFF;
    const auto normalized = ue5mem::Pattern::Parse(Normalize(fixed, sites, minimum_fixed));
    const auto declaration = ue5mem::Pattern::Parse(original);
    if (normalized.Size() != learned.size()) return false;
    for (std::size_t i{}; i < learned.size(); ++i) {
        auto required = normalized.Bytes()[i].mask;
        if (i < declaration.Size()) {
            required &= declaration.Bytes()[i].mask;
            if ((learned[i].value ^ declaration.Bytes()[i].value) & required) return false;
        }
        if ((learned[i].mask & required) != required) return false;
    }
    return true;
}

struct ObjectOperand {
    std::size_t instruction{};
    std::int64_t offset{};
    std::uint32_t width{};
    std::uint32_t access{};
};

// Track the function's RCX object through 64-bit MOV/LEA and invalidate register
// writes/call-clobbered registers. Unsupported instructions end this evidence
// window; no register provenance is guessed across them.
std::vector<ObjectOperand> ObjectOperands(std::span<const ue5mem::PatternByte> source) {
    std::array<std::optional<std::int64_t>, 16> objects{};
    objects[1] = 0; // RCX
    bool conditional_path{};
    std::vector<ObjectOperand> result;
    for (std::size_t offset{}; offset < source.size();) {
        const auto instruction = Decode(source, offset);
        if (!instruction) break;
        const auto& code = instruction->code;
        const auto reg = code.modrm_reg + 8 * code.rex_r;
        const auto rm = code.modrm_rm + 8 * code.rex_b;
        const auto byte_register = [&](unsigned value, unsigned extension) {
            return !code.rex && value >= 4 ? value - 4 : value + 8 * extension;
        };
        const auto assign_object = [&](unsigned target, std::optional<std::int64_t> value) {
            // After a conditional jump, a newly established alias may be skipped.
            // Keep only provenance already true before that assignment.
            if (conditional_path && objects[target] != value) value.reset();
            objects[target] = value;
        };
        const auto width = code.rex_w ? 8u : code.p_66 ? 2u : 4u;
        std::uint32_t access{};
        std::uint32_t bytes = width;
        bool recognized = true;
        switch (code.opcode) {
        case 0x88: bytes = 1; [[fallthrough]];
        case 0x89: access = 2; break;
        case 0x8A: bytes = 1; [[fallthrough]];
        case 0x8B: access = 1; break;
        case 0x63: bytes = 4; access = 1; break;
        case 0xC6: bytes = 1; [[fallthrough]];
        case 0xC7: access = 2; break;
        case 0x38: case 0x3A: case 0x84: bytes = 1; [[fallthrough]];
        case 0x39: case 0x3B: case 0x85: access = 1; break;
        case 0x8D: bytes = 0; access = 4; break;
        case 0x0F:
            if (code.opcode2 == 0xB6 || code.opcode2 == 0xBE) { bytes = 1; access = 1; }
            else if (code.opcode2 == 0xB7 || code.opcode2 == 0xBF) { bytes = 2; access = 1; }
            // MOVAPS touches XMM registers, never the tracked general registers.
            else if (code.opcode2 == 0x28 || code.opcode2 == 0x29) {}
            else if (code.opcode2 < 0x80 || code.opcode2 > 0x8F) recognized = false;
            break;
        case 0x80: case 0x81: case 0x83:
            if (code.modrm_reg == 7) { bytes = code.opcode == 0x80 ? 1u : width; access = 1; }
            break;
        case 0x31: case 0x33: case 0xE8: case 0xE9: case 0xEB:
        case 0xC2: case 0xC3: case 0xA8: case 0xA9:
            break;
        case 0x90: recognized = !code.rex_b; break; // REX.B encodes XCHG with R8.
        default:
            recognized = (code.opcode >= 0x50 && code.opcode <= 0x5F) ||
                (code.opcode >= 0x70 && code.opcode <= 0x7F) ||
                (code.opcode >= 0xB0 && code.opcode <= 0xBF);
            break;
        }
        if (!recognized) break;
        std::optional<std::int64_t> member;
        if ((code.flags & F_MODRM) && code.modrm_mod != 3 && !code.p_67 && !code.p_seg) {
            int base = rm;
            if (code.flags & F_SIB) {
                if (code.sib_index != 4 || code.rex_x ||
                    (code.modrm_mod == 0 && code.sib_base == 5)) base = -1;
                else base = code.sib_base + 8 * code.rex_b;
            } else if (code.modrm_mod == 0 && code.modrm_rm == 5) base = -1;
            const auto displacement = source.subspan(offset + instruction->operands, instruction->displacement);
            if (base >= 0 && base != 4 && base != 5 && objects[base] &&
                std::ranges::all_of(displacement, [](auto byte) { return byte.mask == 0xFF; })) {
                const auto value = code.flags & F_DISP32 ? static_cast<std::int32_t>(code.disp.disp32) :
                    code.flags & F_DISP16 ? static_cast<std::int16_t>(code.disp.disp16) :
                    code.flags & F_DISP8 ? static_cast<std::int8_t>(code.disp.disp8) : 0;
                member = *objects[base] + value;
                if (access) result.push_back({offset, *member, bytes, access});
            }
        }

        if (code.opcode == 0x89 && code.modrm_mod == 3)
            assign_object(rm, code.rex_w ? objects[reg] : std::nullopt);
        else if (code.opcode == 0x8B)
            assign_object(reg, code.modrm_mod == 3 && code.rex_w ? objects[rm] : std::nullopt);
        else if (code.opcode == 0x8D) assign_object(reg, code.rex_w ? member : std::nullopt);
        else if (code.opcode == 0x8A) objects[byte_register(code.modrm_reg, code.rex_r)].reset();
        else if (code.opcode == 0x63 ||
            (code.opcode == 0x0F && access)) objects[reg].reset();
        else if ((code.opcode == 0x88 || code.opcode == 0xC6) && code.modrm_mod == 3)
            objects[byte_register(code.modrm_rm, code.rex_b)].reset();
        else if (code.opcode == 0xC7 && code.modrm_mod == 3) objects[rm].reset();
        else if (code.opcode == 0x31 && code.modrm_mod == 3) objects[rm].reset();
        else if (code.opcode == 0x33) objects[reg].reset();
        else if ((code.opcode == 0x80 || code.opcode == 0x81 || code.opcode == 0x83) &&
            code.modrm_reg != 7 && code.modrm_mod == 3)
            objects[code.opcode == 0x80 ? byte_register(code.modrm_rm, code.rex_b) : rm].reset();
        else if (code.opcode >= 0x58 && code.opcode <= 0x5F)
            objects[code.opcode - 0x58 + 8 * code.rex_b].reset();
        else if (code.opcode >= 0xB0 && code.opcode <= 0xB7)
            objects[byte_register(code.opcode & 7, code.rex_b)].reset();
        else if (code.opcode >= 0xB8 && code.opcode <= 0xBF)
            objects[(code.opcode & 7) + 8 * code.rex_b].reset();
        else if (code.opcode == 0xE8)
            for (const auto clobbered : {0, 1, 2, 8, 9, 10, 11}) objects[clobbered].reset();
        conditional_path |= (code.opcode >= 0x70 && code.opcode <= 0x7F) ||
            (code.opcode == 0x0F && code.opcode2 >= 0x80 && code.opcode2 <= 0x8F);
        offset += code.len;
        if (EndsBlock(code)) break;
    }
    return result;
}

bool MatchesUse(const ObjectOperand& operand, const SignatureLayoutMember& binding) {
    return (operand.access & binding.access) == operand.access &&
        (operand.access == 4 || operand.width == binding.width);
}

MemberRecipe MakeMemberRecipe(std::span<const ue5mem::PatternByte> source,
    const SignatureLayoutWitness& witness, std::span<const std::uint32_t> offsets) {
    MemberRecipe recipe;
    std::vector<bool> seen(witness.members.size());
    for (const auto& operand : ObjectOperands(source)) {
        std::optional<std::size_t> matched;
        for (std::size_t i{}; i < witness.members.size(); ++i) {
            const auto& binding = witness.members[i];
            if (MatchesUse(operand, binding) &&
                operand.offset == static_cast<std::int64_t>(offsets[binding.field_index]) + binding.addend) {
                if (matched) return {}; // one operand cannot name two fields
                matched = i;
            }
        }
        if (matched) {
            recipe.sites.push_back({operand.instruction, *matched});
            seen[*matched] = true;
        }
    }
    if (std::ranges::any_of(seen, [](bool value) { return !value; })) return {};
    // Short witnesses are accepted only with independent, agreeing witnesses for
    // each field below. The address-only v1 matcher keeps its 24-byte minimum.
    recipe.pattern = Normalize(source, recipe.sites, 16);
    if (recipe.pattern.empty()) return {};
    return recipe;
}

std::vector<ue5mem::PatternByte> ReadInstructions(
    const SignatureScanContext& context, std::uintptr_t address) {
    if (address < context.section.base ||
        address - context.section.base >= context.section.virtual_size) return {};
    const auto size = (std::min)(kWindowBytes,
        context.section.virtual_size - (address - context.section.base));
    std::vector<std::uint8_t> bytes(size);
    if (!context.read(address, bytes.data(), bytes.size())) return {};
    std::vector<ue5mem::PatternByte> source;
    for (auto byte : bytes) source.push_back({byte, 0xFF});
    return source;
}

std::optional<std::vector<std::optional<std::uint32_t>>> ExtractMembers(
    const MemberRecipe& recipe, std::span<const ue5mem::PatternByte> source,
    const SignatureLayoutWitness& witness, const SignatureLayoutDefinition& definition) {
    std::vector<std::optional<std::uint32_t>> values(definition.fields.size());
    std::vector<bool> seen(witness.members.size());
    const auto operands = ObjectOperands(source);
    for (const auto& site : recipe.sites) {
        if (site.binding >= witness.members.size()) return std::nullopt;
        const auto& binding = witness.members[site.binding];
        const auto operand = std::ranges::find_if(operands, [&](const auto& value) {
            return value.instruction == site.instruction;
        });
        if (operand == operands.end() || !MatchesUse(*operand, binding)) return std::nullopt;
        const auto offset = operand->offset - binding.addend;
        if (offset < 0 || offset > UINT32_MAX ||
            offset % definition.fields[binding.field_index].alignment != 0) return std::nullopt;
        auto& value = values[binding.field_index];
        if (value && *value != offset) return std::nullopt;
        value = static_cast<std::uint32_t>(offset);
        seen[site.binding] = true;
    }
    if (std::ranges::any_of(seen, [](bool value) { return !value; })) return std::nullopt;
    return values;
}

bool ExecutableSection(const SignatureScanContext& context) {
    const auto& section = context.section;
    const auto& module = context.module;
    return (section.characteristics & IMAGE_SCN_MEM_EXECUTE) && section.virtual_size &&
        section.base >= module.base && section.base - module.base <= module.size &&
        section.virtual_size <= module.size - (section.base - module.base);
}

}  // namespace

class SignatureUpdater::Impl final {
public:
    explicit Impl(std::filesystem::path directory) : directory_(std::move(directory)) {}

    void Load() {
        if (loaded_) return;
        loaded_ = true;
        if (directory_.empty()) return;
        ReliableStorage storage(directory_.wstring());
        const auto data = storage.Read(kCacheFile, kMaximumCacheBytes);
        if (!data || data.bytes.empty()) return;
        const auto* begin = reinterpret_cast<const char*>(data.bytes.data());
        const auto document = nlohmann::json::parse(begin, begin + data.bytes.size(), nullptr, false);
        if (!document.is_object() || !document.contains("version") || document["version"] != 1 ||
            !document.contains("records") || !document["records"].is_array() ||
            document["records"].size() > kMaximumRecords) return;
        for (const auto& row : document["records"]) {
            if (!row.is_object()) continue;
            try {
                const auto module_utf8 = row.at("module").get<std::string>();
                const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    module_utf8.data(), static_cast<int>(module_utf8.size()), nullptr, 0);
                if (size <= 0) continue;
                std::wstring module(size, L'\0');
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, module_utf8.data(),
                    static_cast<int>(module_utf8.size()), module.data(), size);
                const auto pattern = row.at("learned").get<std::string>();
                const auto parsed = ue5mem::Pattern::Parse(pattern);
                // Only accept records this version of the instruction normalizer
                // can reproduce; corrupted cache data never loosens the matcher.
                const auto contract = row.at("contract").get<std::string>();
                const auto original = row.at("original").get<std::string>();
                if (!contract.starts_with("relocation:") || parsed.Size() > kWindowBytes ||
                    Normalize(parsed.Bytes()) != pattern || !ValidCachedPattern(original, parsed.Bytes())) continue;
                records_.emplace(Key{module, row.at("section").get<std::string>(),
                    original, contract}, pattern);
            } catch (const nlohmann::json::exception&) {
            } catch (const std::invalid_argument&) {
            }
        }
        if (!document.contains("layouts") || !document["layouts"].is_array() ||
            document["layouts"].size() + records_.size() > kMaximumRecords) return;
        for (const auto& row : document["layouts"]) {
            try {
                const auto module_utf8 = row.at("module").get<std::string>();
                const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                    module_utf8.data(), static_cast<int>(module_utf8.size()), nullptr, 0);
                if (size <= 0) continue;
                std::wstring module(size, L'\0');
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, module_utf8.data(),
                    static_cast<int>(module_utf8.size()), module.data(), size);
                MemberRecipe recipe;
                recipe.pattern = row.at("learned").get<std::string>();
                const auto parsed = ue5mem::Pattern::Parse(recipe.pattern);
                const auto contract = row.at("contract").get<std::string>();
                if (!contract.starts_with("members:") || parsed.Size() > kWindowBytes ||
                    Normalize(parsed.Bytes(), {}, 16) != recipe.pattern ||
                    !row.at("sites").is_array() || row.at("sites").size() > kWindowBytes) continue;
                for (const auto& entry : row.at("sites")) {
                    const auto instruction = entry.at(0).get<std::size_t>();
                    const auto binding = entry.at(1).get<std::size_t>();
                    if (instruction >= parsed.Size() || binding >= 32) { recipe.sites.clear(); break; }
                    recipe.sites.push_back({instruction, binding});
                }
                if (recipe.sites.empty()) continue;
                const auto original = row.at("original").get<std::string>();
                if (!ValidCachedPattern(original, parsed.Bytes(), recipe.sites, 16)) continue;
                member_records_.emplace(Key{module, row.at("section").get<std::string>(),
                    original, contract}, std::move(recipe));
            } catch (const nlohmann::json::exception&) {
            } catch (const std::invalid_argument&) {
            }
        }
    }

    std::string Save() {
        if (directory_.empty()) return {};
        nlohmann::json document{{"version", 1}, {"records", nlohmann::json::array()},
            {"layouts", nlohmann::json::array()}};
        for (const auto& [key, learned] : records_) {
            const auto& [module, section, original, contract] = key;
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                module.data(), static_cast<int>(module.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) continue;
            std::string module_utf8(count, '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, module.data(),
                static_cast<int>(module.size()), module_utf8.data(), count, nullptr, nullptr);
            document["records"].push_back({{"module", module_utf8}, {"section", section},
                {"original", original}, {"contract", contract}, {"learned", learned}});
        }
        for (const auto& [key, recipe] : member_records_) {
            const auto& [module, section, original, contract] = key;
            const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                module.data(), static_cast<int>(module.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) continue;
            std::string module_utf8(count, '\0');
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, module.data(),
                static_cast<int>(module.size()), module_utf8.data(), count, nullptr, nullptr);
            auto sites = nlohmann::json::array();
            for (const auto& site : recipe.sites) sites.push_back({site.instruction, site.binding});
            document["layouts"].push_back({{"module", module_utf8}, {"section", section},
                {"original", original}, {"contract", contract}, {"learned", recipe.pattern},
                {"sites", std::move(sites)}});
        }
        const auto text = document.dump();
        if (text.size() > kMaximumCacheBytes) return "signature knowledge exceeds size limit";
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        if (error) return "cannot create signature knowledge directory";
        ReliableStorage storage(directory_.wstring());
        const auto result = storage.WriteAtomic(kCacheFile, std::as_bytes(std::span(text)));
        return result ? std::string{} : "cannot persist signature knowledge: " +
            std::to_string(result.win32_error);
    }

    std::filesystem::path directory_;
    std::mutex mutex_;
    bool loaded_{};
    std::map<Key, std::string> records_;
    std::map<Key, MemberRecipe> member_records_;
};

SignatureUpdater::SignatureUpdater(std::filesystem::path directory)
    : impl_(std::make_unique<Impl>(std::move(directory))) {}
SignatureUpdater::~SignatureUpdater() = default;

SignatureRecovery SignatureUpdater::Recover(
    const SignatureScanContext& context, std::string_view contract,
    std::string_view original_pattern, std::stop_token stop) {
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    if (!ExecutableSection(context)) return {0, {}, "signature recovery requires an executable section"};
    const auto key = RecipeKey(context, contract, original_pattern);
    std::string learned;
    {
        std::scoped_lock lock(impl_->mutex_);
        impl_->Load();
        const auto found = impl_->records_.find(key);
        if (found != impl_->records_.end()) learned = found->second;
    }
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    std::vector<std::uintptr_t> matches;
    if (!learned.empty()) matches = context.scan(learned, 2);
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    // A learned window can include instructions beyond the caller's original
    // signature. If that surrounding code changes, derive fresh evidence from
    // the original declaration instead of requiring a manually supplied backup.
    if (matches.empty()) {
        const auto parsed = ue5mem::Pattern::Parse(original_pattern);
        auto derived = Normalize(parsed.Bytes());
        if (derived.empty()) return {0, {}, "insufficient instruction evidence for signature recovery"};
        if (derived != learned) {
            if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
            learned = std::move(derived);
            matches = context.scan(learned, 2);
        }
    }
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    if (matches.size() != 1) return {0, {}, matches.empty()
        ? "instruction signature not found" : "instruction signature is ambiguous"};
    return {matches.front(), std::move(learned), {}};
}

std::string SignatureUpdater::Learn(
    const SignatureScanContext& context, std::string_view contract,
    std::string_view original_pattern, std::uintptr_t instruction, std::stop_token stop) {
    if (stop.stop_requested()) return "signature learning cancelled";
    if (!ExecutableSection(context) || instruction < context.section.base ||
        instruction - context.section.base >= context.section.virtual_size) {
        return "signature learning requires an instruction in the declared executable section";
    }
    const auto size = (std::min)(kWindowBytes,
        context.section.virtual_size - (instruction - context.section.base));
    std::vector<std::uint8_t> bytes(size);
    if (!context.read(instruction, bytes.data(), bytes.size())) return "signature learning memory is unreadable";
    std::vector<ue5mem::PatternByte> source;
    for (auto byte : bytes) source.push_back({byte, 0xFF});
    const auto learned = Normalize(source);
    if (learned.empty()) return "insufficient instruction evidence for signature learning";
    if (stop.stop_requested()) return "signature learning cancelled";
    const auto matches = context.scan(learned, 2);
    if (matches.size() != 1 || matches.front() != instruction) return "learned instruction signature is ambiguous";
    if (stop.stop_requested()) return "signature learning cancelled";
    const auto key = RecipeKey(context, contract, original_pattern);
    std::scoped_lock lock(impl_->mutex_);
    impl_->Load();
    const auto found = impl_->records_.find(key);
    if (found != impl_->records_.end() && found->second == learned) return {};
    if (found == impl_->records_.end() &&
        impl_->records_.size() + impl_->member_records_.size() >= kMaximumRecords)
        return "signature knowledge record limit reached";
    impl_->records_.insert_or_assign(key, learned);
    return impl_->Save();
}

SignatureLayoutResolution SignatureUpdater::ResolveLayout(
    const SignatureScanContext& context, const SignatureLayoutDefinition& definition,
    std::stop_token stop) {
    const auto fail = [](std::string message, bool conflict = false) {
        return SignatureLayoutResolution{{}, {}, std::move(message), conflict};
    };
    if (stop.stop_requested()) return fail("signature layout resolution cancelled");
    if (!ExecutableSection(context)) return fail("signature layout requires an executable section");

    // Include the semantic declaration in the cache identity, not just the byte
    // pattern: changing a field binding must never reuse an old extraction site.
    nlohmann::json contract_data = nlohmann::json::array();
    std::vector<std::uint32_t> seeds;
    for (const auto& field : definition.fields) {
        contract_data.push_back({field.id, field.seed_offset, field.alignment});
        seeds.push_back(field.seed_offset);
    }
    for (const auto& witness : definition.witnesses) {
        auto row = nlohmann::json::array({witness.id});
        for (const auto& member : witness.members)
            row.push_back({member.field_index, member.addend, member.width, member.access});
        contract_data.push_back(std::move(row));
    }
    const auto contract = contract_data.dump();
    struct Winner {
        std::uintptr_t address{};
        MemberRecipe recipe;
        std::vector<ue5mem::PatternByte> source;
        std::vector<std::optional<std::uint32_t>> fields;
        bool original{};
    };
    std::vector<Winner> winners;
    std::vector<Key> keys;
    for (const auto& witness : definition.witnesses) {
        if (stop.stop_requested()) return fail("signature layout resolution cancelled");
        auto key = RecipeKey(context, contract, witness.pattern, "members:");
        MemberRecipe cached;
        {
            std::scoped_lock lock(impl_->mutex_);
            impl_->Load();
            if (const auto found = impl_->member_records_.find(key); found != impl_->member_records_.end())
                cached = found->second;
        }
        Winner winner;
        const auto originals = context.scan(witness.pattern, 2);
        if (stop.stop_requested()) return fail("signature layout resolution cancelled");
        if (originals.size() > 1) return fail(witness.id + ": original signature is ambiguous", true);
        if (originals.size() == 1) {
            winner.source = ReadInstructions(context, originals.front());
            winner.recipe = MakeMemberRecipe(winner.source, witness, seeds);
            if (!winner.recipe.pattern.empty()) {
                winner.address = originals.front();
                winner.original = true;
            }
        }
        if (winner.address == 0) {
            auto recipe = std::move(cached);
            std::vector<std::uintptr_t> matches;
            if (!recipe.pattern.empty()) matches = context.scan(recipe.pattern, 2);
            if (stop.stop_requested()) return fail("signature layout resolution cancelled");
            if (matches.empty()) {
                const auto parsed = ue5mem::Pattern::Parse(witness.pattern);
                auto derived = MakeMemberRecipe(parsed.Bytes(), witness, seeds);
                if (derived.pattern.empty()) return fail(witness.id + ": insufficient member instruction evidence");
                if (derived.pattern != recipe.pattern) {
                    recipe = std::move(derived);
                    matches = context.scan(recipe.pattern, 2);
                }
            }
            if (stop.stop_requested()) return fail("signature layout resolution cancelled");
            if (matches.size() != 1) return fail(witness.id + (matches.empty()
                ? ": member signature not found" : ": member signature is ambiguous"), !matches.empty());
            winner.address = matches.front();
            winner.recipe = std::move(recipe);
            winner.source = ReadInstructions(context, winner.address);
        }
        const auto values = ExtractMembers(winner.recipe, winner.source, witness, definition);
        if (!values) return fail(witness.id + ": member access evidence is inconsistent", true);
        winner.fields = *values;
        keys.push_back(std::move(key));
        winners.push_back(std::move(winner));
    }

    std::vector<std::optional<std::uint32_t>> fields(definition.fields.size());
    std::vector<std::vector<std::uintptr_t>> observations(definition.fields.size());
    for (const auto& winner : winners) {
        for (std::size_t i{}; i < winner.fields.size(); ++i) {
            if (!winner.fields[i]) continue;
            if (fields[i] && fields[i] != winner.fields[i])
                return fail(definition.fields[i].id + ": independent witnesses disagree", true);
            fields[i] = winner.fields[i];
            auto& addresses = observations[i];
            if (std::ranges::find(addresses, winner.address) == addresses.end())
                addresses.push_back(winner.address);
        }
    }
    for (const auto& winner : winners) {
        const auto parsed = ue5mem::Pattern::Parse(winner.recipe.pattern);
        const auto fixed = std::ranges::count_if(parsed.Bytes(), [](auto byte) { return byte.mask == 0xFF; });
        if (fixed >= kMinimumFixedBytes) continue;
        for (std::size_t i{}; i < winner.fields.size(); ++i)
            if (winner.fields[i] && observations[i].size() < 2)
                return fail(definition.fields[i].id + ": short witness requires independent confirmation");
    }

    SignatureLayoutResolution result;
    for (std::size_t i{}; i < fields.size(); ++i) {
        if (!fields[i]) return fail(definition.fields[i].id + ": field has no verified witness");
        result.offsets.push_back(*fields[i]);
    }
    for (const auto& winner : winners) result.addresses.push_back(winner.address);
    if (stop.stop_requested()) return fail("signature layout resolution cancelled");

    // Learn only after the complete layout agrees. Keep the winning evidence if
    // an extended window is not unique; persistence is optional for this result.
    std::vector<MemberRecipe> recipes;
    for (std::size_t i{}; i < winners.size(); ++i) {
        if (stop.stop_requested()) return fail("signature layout resolution cancelled");
        auto learned = MakeMemberRecipe(winners[i].source, definition.witnesses[i], result.offsets);
        if (!learned.pattern.empty() && (winners[i].original || learned.pattern != winners[i].recipe.pattern)) {
            const auto matches = context.scan(learned.pattern, 2);
            if (stop.stop_requested()) return fail("signature layout resolution cancelled");
            if (matches.size() != 1 || matches.front() != winners[i].address) learned = {};
        }
        if (learned.pattern.empty() && !winners[i].original) learned = winners[i].recipe;
        recipes.push_back(std::move(learned));
    }
    if (stop.stop_requested()) return fail("signature layout resolution cancelled");
    try {
        std::scoped_lock lock(impl_->mutex_);
        bool changed{};
        for (std::size_t i{}; i < recipes.size(); ++i) {
            if (recipes[i].pattern.empty()) continue;
            const auto found = impl_->member_records_.find(keys[i]);
            if (found != impl_->member_records_.end() &&
                found->second.pattern == recipes[i].pattern && found->second.sites == recipes[i].sites) continue;
            if (found == impl_->member_records_.end() &&
                impl_->records_.size() + impl_->member_records_.size() >= kMaximumRecords) {
                result.diagnostic = "signature knowledge record limit reached";
                continue;
            }
            impl_->member_records_.insert_or_assign(keys[i], std::move(recipes[i]));
            changed = true;
        }
        if (changed) result.diagnostic = impl_->Save();
    } catch (const std::exception& error) {
        result.diagnostic = "cannot persist signature layout knowledge: " + std::string(error.what());
    }
    return result;
}

}  // namespace anomaly
