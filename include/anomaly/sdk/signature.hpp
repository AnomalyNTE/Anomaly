#pragma once

#include "anomaly/sdk/cpp.hpp"

#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace anomaly::sdk {

[[nodiscard]] constexpr AnomalySignatureFieldV2 FieldSeed(
    std::string_view id, std::uint32_t offset, std::uint32_t alignment = 1) noexcept {
    return {sizeof(AnomalySignatureFieldV2), StringView(id), offset, alignment};
}

[[nodiscard]] constexpr AnomalySignatureMemberV2 MemberUse(
    std::uint32_t field, std::uint32_t width, std::uint32_t access,
    std::int32_t addend = 0) noexcept {
    return {sizeof(AnomalySignatureMemberV2), field, addend, width, access};
}

[[nodiscard]] constexpr AnomalySignatureWitnessV2 Witness(
    std::string_view id, std::string_view pattern,
    std::span<const AnomalySignatureMemberV2> members) noexcept {
    return {sizeof(AnomalySignatureWitnessV2), StringView(id), StringView(pattern),
        members.data(), static_cast<std::uint32_t>(members.size())};
}

struct LayoutSchema {
    std::string_view module;
    std::string_view section;
    std::span<const AnomalySignatureFieldV2> fields;
    std::span<const AnomalySignatureWitnessV2> witnesses;
};

class LayoutResult;

// Owned entirely by the plugin. Lookups never call the host, scan or perform I/O.
class ResolvedLayout final {
public:
    [[nodiscard]] std::uintptr_t address(std::string_view id) const {
        for (std::size_t i{}; i < witnesses_.size(); ++i)
            if (witnesses_[i] == id) return addresses_[i];
        throw std::out_of_range("undeclared signature witness");
    }
    [[nodiscard]] std::uint32_t offset(std::string_view id) const {
        for (std::size_t i{}; i < fields_.size(); ++i)
            if (fields_[i] == id) return offsets_[i];
        throw std::out_of_range("undeclared signature field");
    }

private:
    friend LayoutResult ResolveLayout(const AnomalySignatureServiceV2*, const LayoutSchema&);
    std::vector<std::string> fields_;
    std::vector<std::string> witnesses_;
    std::vector<std::uint32_t> offsets_;
    std::vector<std::uintptr_t> addresses_;
};

class LayoutResult final {
public:
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] ResolvedLayout& value() & {
        if (!ok_) throw std::logic_error(error_);
        return layout_;
    }
    [[nodiscard]] const ResolvedLayout& value() const & {
        if (!ok_) throw std::logic_error(error_);
        return layout_;
    }
    [[nodiscard]] ResolvedLayout&& value() && {
        if (!ok_) throw std::logic_error(error_);
        return std::move(layout_);
    }

private:
    friend LayoutResult ResolveLayout(const AnomalySignatureServiceV2*, const LayoutSchema&);
    bool ok_{};
    std::string error_;
    ResolvedLayout layout_;
};

[[nodiscard]] inline LayoutResult ResolveLayout(
    const AnomalySignatureServiceV2* service, const LayoutSchema& schema) {
    LayoutResult result;
    constexpr auto required = offsetof(AnomalySignatureServiceV2, resolve_layout) +
        sizeof(AnomalySignatureServiceV2::resolve_layout);
    if (service == nullptr || service->struct_size < required ||
        service->service_version < ANOMALY_SIGNATURE_SERVICE_V2_VERSION ||
        service->resolve_layout == nullptr) {
        result.error_ = "signature layout service v2 is unavailable";
        return result;
    }
    if (schema.fields.empty() || schema.fields.size() > ANOMALY_SIGNATURE_LAYOUT_MAX_FIELDS_V2 ||
        schema.witnesses.empty() || schema.witnesses.size() > ANOMALY_SIGNATURE_LAYOUT_MAX_WITNESSES_V2) {
        result.error_ = "signature layout declaration exceeds service limits";
        return result;
    }
    auto& layout = result.layout_;
    layout.offsets_.resize(schema.fields.size());
    layout.addresses_.resize(schema.witnesses.size());
    const AnomalySignatureLayoutRequestV2 request{
        sizeof(request), StringView(schema.module), StringView(schema.section),
        schema.fields.data(), static_cast<std::uint32_t>(schema.fields.size()),
        schema.witnesses.data(), static_cast<std::uint32_t>(schema.witnesses.size())};
    const auto status = service->resolve_layout(service->user, &request,
        layout.addresses_.data(), request.witness_count,
        layout.offsets_.data(), request.field_count);
    if (!Succeeded(status)) {
        result.error_ = status.message.data != nullptr
            ? std::string(status.message.data, status.message.size) : "signature layout resolution failed";
        return result;
    }
    for (const auto& field : schema.fields)
        layout.fields_.emplace_back(field.id.data, field.id.size);
    for (const auto& witness : schema.witnesses)
        layout.witnesses_.emplace_back(witness.id.data, witness.id.size);
    result.ok_ = true;
    return result;
}

}  // namespace anomaly::sdk
