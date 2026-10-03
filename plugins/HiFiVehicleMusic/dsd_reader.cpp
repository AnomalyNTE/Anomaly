#include "plugins/HiFiVehicleMusic/dsd_reader.hpp"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>

namespace hifi_vehicle_music {
namespace {

std::uint64_t LittleEndian(const std::uint8_t* bytes, const unsigned size) noexcept {
    std::uint64_t value{};
    for (unsigned i = size; i-- > 0;) value = value << 8 | bytes[i];
    return value;
}

std::uint64_t BigEndian(const std::uint8_t* bytes, const unsigned size) noexcept {
    std::uint64_t value{};
    for (unsigned i = 0; i < size; ++i) value = value << 8 | bytes[i];
    return value;
}

bool ReadExact(std::ifstream& file, void* out, const std::size_t size) {
    file.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return static_cast<std::size_t>(file.gcount()) == size;
}

// Channel counts and rates that DSD hardware accepts (DSD64..DSD512).
bool Plausible(const unsigned channels, const unsigned rate) noexcept {
    return channels >= 1 && channels <= 8 && rate >= 2822400 && rate % 44100 == 0;
}

} // namespace

bool IsDsdPath(const std::wstring& path) {
    std::wstring extension = std::filesystem::path(path).extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".dsf" || extension == L".dff";
}

std::unique_ptr<DsdReader> DsdReader::Open(const std::wstring& path) {
    auto reader = std::unique_ptr<DsdReader>(new DsdReader());
    reader->file_.open(std::filesystem::path(path), std::ios::binary);
    if (!reader->file_) return {};
    char magic[4]{};
    if (!ReadExact(reader->file_, magic, sizeof(magic))) return {};
    reader->file_.seekg(0);
    const bool opened = std::memcmp(magic, "DSD ", 4) == 0 ? reader->OpenDsf()
        : std::memcmp(magic, "FRM8", 4) == 0               ? reader->OpenDff()
                                                            : false;
    if (!opened || !Plausible(reader->channels_, reader->rate_) || reader->frames_ == 0) return {};
    reader->Seek(0);
    return reader;
}

// DSF (Sony): "DSD " (28 bytes), "fmt " (52 bytes), "data" (12-byte header),
// little-endian. Channel blocks of block_size_ bytes follow one another.
bool DsdReader::OpenDsf() {
    std::uint8_t header[28 + 52 + 12]{};
    if (!ReadExact(file_, header, sizeof(header))) return false;
    const std::uint8_t* const fmt = header + 28;
    const std::uint8_t* const data = fmt + 52;
    if (std::memcmp(fmt, "fmt ", 4) != 0 || std::memcmp(data, "data", 4) != 0) return false;
    if (LittleEndian(fmt + 16, 4) != 0) return false;  // format id 0 = DSD raw
    channels_ = static_cast<unsigned>(LittleEndian(fmt + 24, 4));
    rate_ = static_cast<unsigned>(LittleEndian(fmt + 28, 4));
    const std::uint64_t bits = LittleEndian(fmt + 32, 4);
    if (bits != 1 && bits != 8) return false;
    lsb_first_ = bits == 1;
    frames_ = LittleEndian(fmt + 36, 8) / 8;  // sample count per channel
    block_size_ = static_cast<std::uint32_t>(LittleEndian(fmt + 44, 4));
    if (block_size_ == 0 || block_size_ > (1U << 20)) return false;
    data_offset_ = sizeof(header);
    dsf_ = true;
    return true;
}

// DSDIFF (Philips): big-endian FRM8 form of chunks padded to even sizes.
// PROP/SND holds "FS  ", "CHNL" and "CMPR"; "DSD " holds the interleaved
// bytes. DST-compressed files are not supported.
bool DsdReader::OpenDff() {
    std::uint8_t form[16]{};
    if (!ReadExact(file_, form, sizeof(form)) || std::memcmp(form + 12, "DSD ", 4) != 0) return false;
    const std::uint64_t end = 12 + BigEndian(form + 4, 8);
    std::uint64_t offset = 16;
    bool compressed_ok = false;
    while (offset + 12 <= end) {
        std::uint8_t chunk[12]{};
        file_.seekg(static_cast<std::streamoff>(offset));
        if (!ReadExact(file_, chunk, sizeof(chunk))) return false;
        const std::uint64_t size = BigEndian(chunk + 4, 8);
        const std::uint64_t body = offset + 12;
        if (std::memcmp(chunk, "PROP", 4) == 0) {
            std::uint8_t type[4]{};
            if (!ReadExact(file_, type, sizeof(type)) || std::memcmp(type, "SND ", 4) != 0) return false;
            std::uint64_t sub = body + 4;
            while (sub + 12 <= body + size) {
                std::uint8_t entry[12 + 4]{};
                file_.seekg(static_cast<std::streamoff>(sub));
                if (!ReadExact(file_, entry, sizeof(entry))) return false;
                if (std::memcmp(entry, "FS  ", 4) == 0) {
                    rate_ = static_cast<unsigned>(BigEndian(entry + 12, 4));
                } else if (std::memcmp(entry, "CHNL", 4) == 0) {
                    channels_ = static_cast<unsigned>(BigEndian(entry + 12, 2));
                } else if (std::memcmp(entry, "CMPR", 4) == 0) {
                    compressed_ok = std::memcmp(entry + 12, "DSD ", 4) == 0;
                }
                const std::uint64_t entry_size = BigEndian(entry + 4, 8);
                sub += 12 + entry_size + (entry_size & 1);
            }
        } else if (std::memcmp(chunk, "DSD ", 4) == 0) {
            if (!compressed_ok || channels_ == 0) return false;
            data_offset_ = body;
            frames_ = size / channels_;
            return true;
        }
        offset = body + size + (size & 1);
    }
    return false;
}

void DsdReader::Seek(const std::uint64_t frame) noexcept {
    position_ = (std::min)(frame, frames_);
    cached_group_ = UINT64_MAX;
    file_.clear();
}

std::size_t DsdReader::Read(std::uint8_t* out, const std::size_t frames) {
    const std::size_t wanted = static_cast<std::size_t>((std::min<std::uint64_t>)(frames, frames_ - position_));
    if (!dsf_) {
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(data_offset_ + position_ * channels_));
        file_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(wanted * channels_));
        const std::size_t read = static_cast<std::size_t>(file_.gcount()) / channels_;
        position_ += read;
        return read;
    }
    // DSF: one group = block_size_ bytes for every channel.
    std::size_t done = 0;
    while (done < wanted) {
        const std::uint64_t group = position_ / block_size_;
        if (group != cached_group_) {
            group_.assign(static_cast<std::size_t>(block_size_) * channels_, 0);
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(data_offset_ + group * group_.size()));
            file_.read(reinterpret_cast<char*>(group_.data()), static_cast<std::streamsize>(group_.size()));
            if (file_.gcount() <= 0) break;
            if (lsb_first_) {
                for (std::uint8_t& byte : group_) byte = ReverseBits(byte);
            }
            cached_group_ = group;
        }
        const std::size_t within = static_cast<std::size_t>(position_ % block_size_);
        const std::size_t count = (std::min)(wanted - done, static_cast<std::size_t>(block_size_) - within);
        for (std::size_t i = 0; i < count; ++i) {
            for (unsigned c = 0; c < channels_; ++c) {
                out[(done + i) * channels_ + c] = group_[static_cast<std::size_t>(c) * block_size_ + within + i];
            }
        }
        done += count;
        position_ += count;
    }
    return done;
}

} // namespace hifi_vehicle_music
