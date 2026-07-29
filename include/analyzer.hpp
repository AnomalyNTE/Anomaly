#pragma once

#include "anomaly/pattern_service.hpp"
#include "config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace ue5mem {

class Analyzer {
public:
    using RuntimeStatusProvider = std::function<std::string()>;
    using ProfileStatusProvider = std::function<std::string()>;
    using Ue5ReflectionQueryProvider = std::function<std::string(std::string_view)>;

    Analyzer(
        std::filesystem::path root,
        AnalyzerConfig config,
        RuntimeStatusProvider runtime_status = {},
        anomaly::CoreMemoryServices memory_services = {},
        ProfileStatusProvider profile_status = {},
        Ue5ReflectionQueryProvider ue5_reflection_query = {});

    [[nodiscard]] std::string Execute(std::string_view command) const;
    [[nodiscard]] std::string BuildSnapshot() const;
    [[nodiscard]] const std::filesystem::path& Root() const noexcept { return root_; }
    [[nodiscard]] const anomaly::CoreMemoryServices& MemoryServices() const noexcept {
        return memory_services_;
    }

private:
    std::string ModulesJson() const;
    std::string SectionsJson(std::wstring_view module_name) const;
    std::string RegionsJson(std::wstring_view module_name) const;
    std::string ScanJson(
        std::wstring_view module_name,
        std::string_view section,
        std::string_view pattern_text) const;
    std::string XrefsJson(
        std::wstring_view module_name,
        std::uintptr_t target) const;
    std::string UnrealJson() const;

    std::filesystem::path root_;
    AnalyzerConfig config_;
    RuntimeStatusProvider runtime_status_;
    anomaly::CoreMemoryServices memory_services_;
    ProfileStatusProvider profile_status_;
    Ue5ReflectionQueryProvider ue5_reflection_query_;
};

}  // namespace ue5mem
