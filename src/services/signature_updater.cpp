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
    if (offset >= source.size()) return std::nullopt;
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

// A callee anchor re-identifies a recipe whose own instruction bytes are shared with
// other functions. Whole families of compiler-generated wrappers (exec thunks, row
// getters, flag setters) differ only in the rel32 of the callee they call, and that
// rel32 is exactly the byte range Normalize() has to wildcard, so the learned window
// alone can never separate them. The anchor stores the normalized fingerprint of the
// call target instead: the callee's own body is unique even when the caller template
// is not, and it survives the next build because only its relative displacement moved.
struct CalleeAnchor {
    std::size_t instruction{};
    std::string callee;
};

constexpr std::size_t kMaximumCalleeAnchors = 2;
constexpr std::size_t kAnchorReadBytes = 48;
constexpr std::size_t kAnchorMinimumFixed = 8;
constexpr std::size_t kMaximumAnchorCandidates = 256;

std::string Normalize(std::span<const ue5mem::PatternByte> source,
                      std::span<const MemberSite> members = {},
                      std::size_t minimum_fixed = kMinimumFixedBytes,
                      std::size_t minimum_instructions = 4) {
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
    if (instructions < minimum_instructions || static_cast<std::size_t>(std::ranges::count_if(result, [](auto byte) {
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
    // Never omit declared member/stack operands when compacting an address-only
    // recipe. Its consumers still use those original offsets after recovery.
    if (learned.size() < ue5mem::Pattern::Parse(Normalize(declaration.Bytes(), {}, 0, 0)).Size()) return false;
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

bool ImpliesPattern(const ue5mem::Pattern& known, const ue5mem::Pattern& required) {
    const auto known_bytes = known.Bytes();
    const auto required_bytes = required.Bytes();
    return known_bytes.size() >= required_bytes.size() &&
        std::equal(required_bytes.begin(), required_bytes.end(), known_bytes.begin(),
            [](const auto& expected, const auto& actual) {
                return (actual.mask & expected.mask) == expected.mask &&
                    ((actual.value ^ expected.value) & expected.mask) == 0;
            });
}

bool RelativeBranch(const hde64s& code) {
    return (code.flags & F_RELATIVE) && (code.flags & (F_IMM8 | F_IMM32));
}

std::uintptr_t BranchTarget(std::uintptr_t address, const hde64s& code) {
    const auto displacement = code.flags & F_IMM32 ? static_cast<std::int32_t>(code.imm.imm32) :
        static_cast<std::int8_t>(code.imm.imm8);
    return static_cast<std::uintptr_t>(static_cast<std::int64_t>(address + code.len) + displacement);
}

std::vector<std::size_t> CallSites(std::span<const std::uint8_t> bytes) {
    std::vector<std::size_t> sites;
    std::size_t offset{};
    while (offset < bytes.size()) {
        std::array<std::uint8_t, 32> chunk{};
        const auto remaining = (std::min)(chunk.size(), bytes.size() - offset);
        std::copy_n(bytes.data() + offset, remaining, chunk.data());
        hde64s decoded{};
        hde64_disasm(chunk.data(), &decoded);
        if ((decoded.flags & F_ERROR) || !decoded.len || decoded.len > remaining) break;
        if (RelativeBranch(decoded)) sites.push_back(offset);
        offset += decoded.len;
        if (EndsBlock(decoded)) break;
    }
    return sites;
}

bool AnchorsFit(std::span<const ue5mem::PatternByte> pattern, std::span<const CalleeAnchor> anchors) {
    std::vector<std::uint8_t> bytes;
    for (auto byte : pattern) bytes.push_back(byte.value);
    const auto sites = CallSites(bytes);
    return std::ranges::all_of(anchors, [&](const auto& anchor) {
        return anchor.instruction < pattern.size() &&
            pattern[anchor.instruction].mask == 0xFF &&
            std::ranges::find(sites, anchor.instruction) != sites.end();
    });
}

bool InSection(const SignatureScanContext& context, std::uintptr_t address, std::size_t size) {
    return address >= context.section.base && address - context.section.base <= context.section.virtual_size &&
        size <= context.section.virtual_size - (address - context.section.base);
}

std::string CalleeFingerprint(const SignatureScanContext& context, std::uintptr_t target) {
    std::array<std::uint8_t, kAnchorReadBytes> bytes{};
    if (!InSection(context, target, bytes.size()) || !context.read(target, bytes.data(), bytes.size())) return {};
    std::vector<ue5mem::PatternByte> source;
    source.reserve(bytes.size());
    for (const auto byte : bytes) source.push_back({byte, 0xFF});
    return Normalize(source, {}, kAnchorMinimumFixed, 1);
}

bool AnchorMatches(const SignatureScanContext& context, std::uintptr_t candidate,
                   const CalleeAnchor& anchor) {
    std::array<std::uint8_t, 32> code{};
    if (!InSection(context, candidate + anchor.instruction, 15) ||
        !context.read(candidate + anchor.instruction, code.data(), 15)) return false;
    hde64s decoded{};
    hde64_disasm(code.data(), &decoded);
    if ((decoded.flags & F_ERROR) || !RelativeBranch(decoded)) return false;
    const auto target = BranchTarget(candidate + anchor.instruction, decoded);
    std::array<std::uint8_t, kAnchorReadBytes> bytes{};
    const auto pattern = ue5mem::Pattern::Parse(anchor.callee);
    if (pattern.Size() == 0 || pattern.Size() > bytes.size()) return false;
    const auto width = pattern.Size();
    if (!InSection(context, target, width) || !context.read(target, bytes.data(), width)) return false;
    for (std::size_t i{}; i < width; ++i) {
        const auto& byte = pattern.Bytes()[i];
        if (((bytes[i] ^ byte.value) & byte.mask) != 0) return false;
    }
    return true;
}

std::size_t AnchorSurvivors(const SignatureScanContext& context,
                            std::span<const std::uintptr_t> candidates,
                            std::span<const CalleeAnchor> anchors) {
    std::size_t survivors{};
    for (const auto candidate : candidates) {
        if (std::ranges::all_of(anchors, [&](const CalleeAnchor& anchor) {
                return AnchorMatches(context, candidate, anchor);
            })) ++survivors;
    }
    return survivors;
}

// Pick the callee evidence that separates `instruction` from the other candidates of a
// shared template. A single decisive call target is preferred; otherwise the two most
// selective targets are combined. Applying anchors can only remove candidates, so a
// learned anchor set never turns a wrong candidate into a match.
std::vector<CalleeAnchor> LearnCalleeAnchors(const SignatureScanContext& context,
                                             std::uintptr_t instruction,
                                             std::span<const std::uintptr_t> matches,
                                             std::size_t window_size,
                                             std::stop_token stop) {
    std::array<std::uint8_t, kWindowBytes> raw{};
    if (!context.read(instruction, raw.data(), window_size)) return {};
    struct Scored {
        CalleeAnchor anchor;
        std::size_t survivors{};
    };
    std::vector<Scored> scored;
    for (const auto site : CallSites(std::span(raw).first(window_size))) {
        if (stop.stop_requested()) return {};
        std::array<std::uint8_t, 32> code{};
        std::copy_n(raw.data() + site, (std::min)(code.size(), window_size - site), code.data());
        hde64s decoded{};
        hde64_disasm(code.data(), &decoded);
        const auto target = BranchTarget(instruction + site, decoded);
        auto fingerprint = CalleeFingerprint(context, target);
        if (fingerprint.empty()) continue;
        CalleeAnchor anchor{site, std::move(fingerprint)};
        const auto survivors = AnchorSurvivors(context, matches, std::span(&anchor, 1));
        if (survivors == 1) return {std::move(anchor)};
        if (survivors < matches.size()) scored.push_back({std::move(anchor), survivors});
    }
    std::ranges::sort(scored, {}, &Scored::survivors);
    for (std::size_t i{}; i < scored.size(); ++i) {
        for (std::size_t j{i + 1}; j < scored.size(); ++j) {
            const std::array<CalleeAnchor, 2> pair{scored[i].anchor, scored[j].anchor};
            if (AnchorSurvivors(context, matches, pair) == 1) return {pair[0], pair[1]};
        }
    }
    return {};
}

// Persist the shortest instruction-aligned prefix that still proves uniqueness.
// Unrelated code near the end of a 128-byte window often changes between builds.
// Prefix match sets only shrink as bytes are added, so binary search bounds the
// extra scans. Callee evidence, when needed, remains inside every tested prefix.
std::string CompactRecipe(const SignatureScanContext& context, std::string_view pattern,
                          std::uintptr_t instruction, std::span<const CalleeAnchor> anchors,
                          std::size_t declaration_size, std::stop_token stop,
                          const ue5mem::Pattern& declaration, bool original_unique) {
    const auto parsed = ue5mem::Pattern::Parse(pattern);
    const auto bytes = parsed.Bytes();
    std::size_t minimum = declaration_size;
    for (const auto& anchor : anchors)
        minimum = (std::max)(minimum, anchor.instruction + Decode(bytes, anchor.instruction)->code.len);
    std::vector<std::size_t> ends;
    std::size_t fixed{}, instructions{};
    for (std::size_t offset{}; offset < bytes.size();) {
        const auto decoded = Decode(bytes, offset);
        if (!decoded) break;
        for (auto byte : bytes.subspan(offset, decoded->code.len)) fixed += byte.mask == 0xFF;
        offset += decoded->code.len;
        if (++instructions >= 4 && fixed >= kMinimumFixedBytes && offset >= minimum) ends.push_back(offset);
    }
    if (ends.empty()) return std::string(pattern);
    const auto prefix = [&](std::size_t index) { return std::string(pattern.substr(0, ends[index] * 3 - 1)); };
    std::size_t low{}, high = ends.size() - 1;
    while (low < high && !stop.stop_requested()) {
        const auto middle = low + (high - low) / 2;
        const auto candidate = prefix(middle);
        bool unique = original_unique &&
            ImpliesPattern(ue5mem::Pattern::Parse(candidate), declaration);
        if (!unique) {
            const auto matches = context.scan(candidate, kMaximumAnchorCandidates);
            unique = matches.size() < kMaximumAnchorCandidates &&
                std::ranges::find(matches, instruction) != matches.end() &&
                AnchorSurvivors(context, matches, anchors) == 1;
        }
        if (unique) high = middle;
        else low = middle + 1;
    }
    return prefix(low);
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
        bool xmm_destination{};
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
            // MOVSS/MOVSD expose scalar float/double fields. Packed vector moves
            // are traversable but do not satisfy a scalar member binding.
            else if (code.opcode2 == 0x10 || code.opcode2 == 0x11) {
                bytes = code.p_rep == 0xF3 ? 4u : code.p_rep == 0xF2 ? 8u : 16u;
                access = code.opcode2 == 0x10 ? 1u : 2u;
                xmm_destination = true;
            }
            else if (code.opcode2 == 0x28 || code.opcode2 == 0x29 || code.opcode2 == 0x57) {
                xmm_destination = true;
            }
            else if (code.opcode2 < 0x80 || code.opcode2 > 0x8F) recognized = false;
            break;
        case 0xF6: case 0xF7:
            // TEST r/m, imm does not overwrite the base register.
            recognized = code.modrm_reg == 0;
            if (recognized) { bytes = code.opcode == 0xF6 ? 1u : width; access = 1; }
            break;
        case 0xFF:
            // Indirect calls clobber the same volatile registers as E8 calls.
            recognized = code.modrm_reg == 2 || code.modrm_reg == 4;
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
            (code.opcode == 0x0F && access && !xmm_destination)) objects[reg].reset();
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
        else if (code.opcode == 0xE8 || (code.opcode == 0xFF && code.modrm_reg == 2))
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
                std::string extended;
                if (row.contains("extended")) {
                    extended = row.at("extended").get<std::string>();
                    const auto full = ue5mem::Pattern::Parse(extended);
                    if (full.Size() <= parsed.Size() || full.Size() > kWindowBytes ||
                        !extended.starts_with(pattern + " ") || Normalize(full.Bytes()) != extended ||
                        !ValidCachedPattern(original, full.Bytes())) continue;
                }
                std::vector<CalleeAnchor> anchors;
                if (row.contains("anchors")) {
                    if (!row["anchors"].is_array() ||
                        row["anchors"].size() > kMaximumCalleeAnchors) continue;
                    bool valid{true};
                    for (const auto& entry : row["anchors"]) {
                        if (!entry.is_object()) { valid = false; break; }
                        const auto instruction = entry.at("instruction").get<std::size_t>();
                        const auto callee = entry.at("callee").get<std::string>();
                        const auto callee_pattern = ue5mem::Pattern::Parse(callee);
                        if (instruction >= parsed.Size() ||
                            callee_pattern.Size() < kAnchorMinimumFixed ||
                            callee_pattern.Size() > kAnchorReadBytes ||
                            Normalize(callee_pattern.Bytes(), {}, kAnchorMinimumFixed, 1) != callee) {
                            valid = false;
                            break;
                        }
                        anchors.push_back({instruction, callee});
                    }
                    if (!valid || !AnchorsFit(parsed.Bytes(), anchors)) continue;
                }
                const auto key = Key{module, row.at("section").get<std::string>(), original, contract};
                if (!records_.emplace(key, pattern).second) continue;
                if (!extended.empty()) extended_records_.emplace(key, std::move(extended));
                if (!anchors.empty()) anchor_records_.insert_or_assign(key, std::move(anchors));
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
            nlohmann::json row{{"module", module_utf8}, {"section", section},
                {"original", original}, {"contract", contract}, {"learned", learned}};
            if (const auto full = extended_records_.find(key); full != extended_records_.end())
                row["extended"] = full->second;
            const auto anchors = anchor_records_.find(key);
            if (anchors != anchor_records_.end() && !anchors->second.empty()) {
                auto encoded = nlohmann::json::array();
                for (const auto& anchor : anchors->second) {
                    encoded.push_back({{"instruction", anchor.instruction},
                        {"callee", anchor.callee}});
                }
                row["anchors"] = std::move(encoded);
            }
            document["records"].push_back(std::move(row));
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
    std::map<Key, std::string> extended_records_;
    std::map<Key, MemberRecipe> member_records_;
    std::map<Key, std::vector<CalleeAnchor>> anchor_records_;
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
    std::string extended;
    std::vector<CalleeAnchor> anchors;
    {
        std::scoped_lock lock(impl_->mutex_);
        impl_->Load();
        const auto found = impl_->records_.find(key);
        if (found != impl_->records_.end()) learned = found->second;
        if (const auto full = impl_->extended_records_.find(key); full != impl_->extended_records_.end()) extended = full->second;
        const auto learned_anchors = impl_->anchor_records_.find(key);
        if (learned_anchors != impl_->anchor_records_.end()) anchors = learned_anchors->second;
    }
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    std::vector<std::uintptr_t> matches;
    if (!learned.empty()) matches = context.scan(learned, kMaximumAnchorCandidates);
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    // A new function can share yesterday's unique prefix. Retain the original
    // full evidence for that case, without requiring it when the prefix is unique.
    if (matches.size() > 1 && !extended.empty()) {
        const auto narrowed = context.scan(extended, kMaximumAnchorCandidates);
        if (!narrowed.empty()) { learned = std::move(extended); matches = narrowed; }
    }
    // A learned window can include instructions beyond the caller's original
    // signature. If that surrounding code changes, derive fresh evidence from
    // the original declaration instead of requiring a manually supplied backup.
    if (matches.empty()) {
        const auto parsed = ue5mem::Pattern::Parse(original_pattern);
        auto derived = Normalize(parsed.Bytes());
        if (derived.empty()) return {0, {}, "insufficient instruction evidence for signature recovery"};
        // Anchors belong to decoded call sites, not arbitrary bytes beyond a new
        // shorter declaration. Never discard mandatory evidence to force a hit.
        if (!AnchorsFit(ue5mem::Pattern::Parse(derived).Bytes(), anchors))
            return {0, {}, "declaration does not cover learned callee evidence"};
        if (derived != learned) {
            if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
            learned = std::move(derived);
            matches = context.scan(learned, kMaximumAnchorCandidates);
        }
    }
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    // Shared instruction templates stay ambiguous until the callee evidence separates
    // them; the anchors only narrow the candidate set, they never invent a match.
    if (matches.size() >= kMaximumAnchorCandidates)
        return {0, {}, "signature candidate limit reached"};
    if (!anchors.empty()) {
        std::vector<std::uintptr_t> kept;
        for (const auto candidate : matches) {
            if (std::ranges::all_of(anchors, [&](const CalleeAnchor& anchor) {
                    return AnchorMatches(context, candidate, anchor);
                })) kept.push_back(candidate);
        }
        matches = std::move(kept);
    }
    if (stop.stop_requested()) return {0, {}, "signature recovery cancelled"};
    if (matches.size() != 1) return {0, {}, matches.empty()
        ? "instruction signature not found" : "instruction signature is ambiguous"};
    return {matches.front(), std::move(learned), {}};
}

std::string SignatureUpdater::Learn(
    const SignatureScanContext& context, std::string_view contract,
    std::string_view original_pattern, std::uintptr_t instruction, std::stop_token stop,
    bool original_unique) {
    if (stop.stop_requested()) return "signature learning cancelled";
    if (!ExecutableSection(context) || instruction < context.section.base ||
        instruction - context.section.base >= context.section.virtual_size) {
        return "signature learning requires an instruction in the declared executable section";
    }
    const auto key = RecipeKey(context, contract, original_pattern);
    std::string cached;
    std::string extended;
    bool anchored{};
    {
        std::scoped_lock lock(impl_->mutex_);
        impl_->Load();
        if (const auto found = impl_->records_.find(key); found != impl_->records_.end())
            cached = found->second;
        if (const auto found = impl_->extended_records_.find(key);
            found != impl_->extended_records_.end()) extended = found->second;
        anchored = impl_->anchor_records_.contains(key);
    }
    // The caller already found the declared signature uniquely. If its prior
    // unanchored recovery evidence still matches here, relearning adds only
    // repeated full-section scans and a possible cache write.
    if (!cached.empty() && !anchored) {
        const auto declaration = ue5mem::Pattern::Parse(original_pattern);
        const auto compact = ue5mem::Pattern::Parse(cached);
        if (original_unique && ImpliesPattern(compact, declaration)) {
            const auto& evidence = extended.empty() ? cached : extended;
            const auto pattern = ue5mem::Pattern::Parse(evidence);
            if (pattern.Size() <= context.section.virtual_size - (instruction - context.section.base)) {
                std::vector<std::uint8_t> current(pattern.Size());
                if (context.read(instruction, current.data(), current.size()) &&
                    !stop.stop_requested() && pattern.FindAll(current, 1) == std::vector<std::size_t>{0})
                    return {};
            }
        }
    }
    const auto size = (std::min)(kWindowBytes,
        context.section.virtual_size - (instruction - context.section.base));
    std::vector<std::uint8_t> bytes(size);
    if (!context.read(instruction, bytes.data(), bytes.size())) return "signature learning memory is unreadable";
    std::vector<ue5mem::PatternByte> source;
    for (auto byte : bytes) source.push_back({byte, 0xFF});
    // Preserve the declaration's intentional operand wildcards. Freezing their
    // observed values would make learned evidence less portable than its seed.
    // Decode still requires concrete instruction prefixes/opcodes/ModRM bytes.
    const auto declaration = ue5mem::Pattern::Parse(original_pattern);
    for (std::size_t i{}; i < (std::min)(source.size(), declaration.Size()); ++i)
        source[i].mask &= declaration.Bytes()[i].mask;
    auto learned = Normalize(source);
    if (learned.empty()) return "insufficient instruction evidence for signature learning";
    if (stop.stop_requested()) return "signature learning cancelled";
    const auto matches = original_unique &&
            ImpliesPattern(ue5mem::Pattern::Parse(learned), declaration)
        ? std::vector<std::uintptr_t>{instruction}
        : context.scan(learned, kMaximumAnchorCandidates);
    if (matches.empty() || matches.size() >= kMaximumAnchorCandidates ||
        std::ranges::find(matches, instruction) == matches.end())
        return "learned instruction signature is ambiguous";
    std::vector<CalleeAnchor> anchors;
    if (matches.size() > 1) {
        anchors = LearnCalleeAnchors(context, instruction, matches,
            ue5mem::Pattern::Parse(learned).Size(), stop);
        if (anchors.empty()) return "learned instruction signature is ambiguous";
    }
    if (stop.stop_requested()) return "signature learning cancelled";
    const auto full = learned;
    learned = CompactRecipe(context, learned, instruction, anchors,
        ue5mem::Pattern::Parse(Normalize(declaration.Bytes(), {}, 0, 0)).Size(), stop,
        declaration, original_unique);
    if (stop.stop_requested()) return "signature learning cancelled";
    std::scoped_lock lock(impl_->mutex_);
    impl_->Load();
    const auto found = impl_->records_.find(key);
    if (anchors.empty() && found != impl_->records_.end() && found->second == learned &&
        !impl_->anchor_records_.contains(key) &&
        ((full == learned && !impl_->extended_records_.contains(key)) ||
         (impl_->extended_records_.contains(key) && impl_->extended_records_.at(key) == full))) return {};
    if (found == impl_->records_.end() &&
        impl_->records_.size() + impl_->member_records_.size() >= kMaximumRecords)
        return "signature knowledge record limit reached";
    impl_->records_.insert_or_assign(key, learned);
    if (full == learned) impl_->extended_records_.erase(key);
    else impl_->extended_records_.insert_or_assign(key, full);
    if (anchors.empty()) impl_->anchor_records_.erase(key);
    else impl_->anchor_records_.insert_or_assign(key, std::move(anchors));
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
        [[nodiscard]] bool Present() const noexcept { return address != 0; }
    };
    // One entry per declared witness, in declaration order. A witness the binary
    // no longer contains publishes a null address and no evidence: the
    // declaration survives as long as every field is still verified by another
    // witness, so one changed prologue cannot take every consumer down with it.
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
        // Two candidate sites cannot be told apart; the declaration is refused
        // rather than one of them being picked.
        if (originals.size() > 1) return fail(witness.id + ": original signature is ambiguous", true);
        if (originals.size() == 1) {
            winner.source = ReadInstructions(context, originals.front());
            winner.recipe = MakeMemberRecipe(winner.source, witness, seeds);
            if (!winner.recipe.pattern.empty()) {
                winner.address = originals.front();
                winner.original = true;
            }
        }
        if (!winner.Present()) {
            auto recipe = std::move(cached);
            std::vector<std::uintptr_t> matches;
            if (!recipe.pattern.empty()) matches = context.scan(recipe.pattern, 2);
            if (stop.stop_requested()) return fail("signature layout resolution cancelled");
            if (matches.size() > 1) return fail(witness.id + ": learned member signature is ambiguous", true);
            if (matches.empty()) {
                const auto parsed = ue5mem::Pattern::Parse(witness.pattern);
                auto derived = MakeMemberRecipe(parsed.Bytes(), witness, seeds);
                if (!derived.pattern.empty() && derived.pattern != recipe.pattern) {
                    recipe = std::move(derived);
                    matches = context.scan(recipe.pattern, 2);
                }
            }
            if (stop.stop_requested()) return fail("signature layout resolution cancelled");
            if (matches.size() > 1) return fail(witness.id + ": member signature is ambiguous", true);
            if (matches.size() == 1) {
                winner.address = matches.front();
                winner.recipe = std::move(recipe);
                winner.source = ReadInstructions(context, winner.address);
            }
        }
        if (winner.Present()) {
            const auto values = ExtractMembers(winner.recipe, winner.source, witness, definition);
            // A site that matches but contradicts the declared members is not
            // evidence for anything: drop this witness instead of refusing the
            // fields the other witnesses still verify.
            if (values) winner.fields = *values;
            else winner.address = 0;
        }
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
        if (!winner.Present() || winner.original) continue;
        // A declaration that still matches is looked up by its own exact pattern,
        // which the scan above already required to be unique. A learned
        // generalization is weaker evidence, so a short one needs a second,
        // independently agreeing witness for every field it names.
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
        if (!winners[i].Present()) {
            // Nothing was located, so there is nothing to remember for it.
            recipes.emplace_back();
            continue;
        }
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
