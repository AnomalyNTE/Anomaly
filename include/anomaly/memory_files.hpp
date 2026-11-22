#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace anomaly {

// A read-only process-local file. The owner keeps the borrowed bytes alive.
struct MemoryFile {
    std::filesystem::path path;
    std::shared_ptr<const void> owner;
    std::span<const std::uint8_t> bytes;
};

// Registers names, never creates directories or files. Removing the registration prevents
// new opens; existing handles retain their bytes until close. Windows read hooks remain
// resident with Core, since engine I/O can outlive a RuntimeSession.
class MemoryFileSet final {
public:
    explicit MemoryFileSet(std::vector<MemoryFile> files);
    ~MemoryFileSet();
    MemoryFileSet(const MemoryFileSet&) = delete;
    MemoryFileSet& operator=(const MemoryFileSet&) = delete;
private:
    std::vector<std::shared_ptr<const MemoryFile>> files_;
};

std::optional<MemoryFile> FindMemoryFile(const std::filesystem::path& path);
// A fresh namespace for each mount generation; this does not touch the filesystem.
std::filesystem::path MemoryFileDirectory(const std::filesystem::path& parent);
// Failed engine unmounts must retain their registrations until process exit.
void RetainMemoryFiles(std::shared_ptr<const MemoryFileSet> files);

} // namespace anomaly
