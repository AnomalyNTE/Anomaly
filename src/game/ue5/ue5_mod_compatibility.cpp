#include "anomaly/ue5_mod_compatibility.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <blake3.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace anomaly {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint32_t MaxEntries = 1000000;
constexpr std::size_t MaxMetadata = 128 * 1024 * 1024;
constexpr std::size_t MaxOverlayBytes = 512 * 1024 * 1024;
constexpr std::uint64_t NullObject = ~std::uint64_t{};
struct Invalid : std::runtime_error { using std::runtime_error::runtime_error; };

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos{};
    void Skip(std::size_t n) {
        if (pos > bytes.size() || n > bytes.size() - pos) throw Invalid("truncated cooked data");
        pos += n;
    }
    template<class T> T Get() {
        const auto at = pos; Skip(sizeof(T)); T value{};
        std::memcpy(&value, bytes.data() + at, sizeof(T)); return value;
    }
    template<class T> T At(std::size_t at) const { Reader r{bytes, at}; return r.Get<T>(); }
    std::uint32_t Count() {
        const auto n = Get<std::uint32_t>();
        if (n > MaxEntries) throw Invalid("cooked data count exceeds limit");
        return n;
    }
    std::uint64_t Integer(std::size_t n, bool big = false) {
        if (n > 8) throw Invalid("invalid packed integer");
        const auto at = pos; Skip(n); std::uint64_t value{};
        for (std::size_t i{}; i < n; ++i)
            value |= std::uint64_t(bytes[at + i]) << (8 * (big ? n - 1 - i : i));
        return value;
    }
};
template<class T> requires std::is_arithmetic_v<T>
void Append(Bytes& out, const T& value) {
    const auto at = out.size(); out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}
template<class T, std::size_t N> requires std::is_same_v<std::remove_const_t<T>, std::uint8_t>
void Append(Bytes& out, std::span<T, N> value) { out.insert(out.end(), value.begin(), value.end()); }
void Append(Bytes& out, const Bytes& value) { Append(out, std::span(value)); }
template<class T> void Put(Bytes& out, std::size_t at, const T& value) {
    if (at > out.size() || sizeof(T) > out.size() - at) throw Invalid("invalid output range");
    std::memcpy(out.data() + at, &value, sizeof(T));
}
void Packed(Bytes& out, std::uint64_t value, std::size_t n, bool big = false) {
    if (n < 8 && (value >> (n * 8))) throw Invalid("container exceeds packed integer range");
    for (std::size_t i{}; i < n; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * (big ? n - 1 - i : i))));
}
Bytes Range(const std::filesystem::path& path, std::uint64_t at, std::size_t n) {
    const auto size = std::filesystem::file_size(path);
    if (n > MaxMetadata || at > size || n > size - at) throw Invalid("invalid container range");
    std::ifstream file(path, std::ios::binary);
    Bytes out(n);
    if (!file || !file.seekg(static_cast<std::streamoff>(at)) ||
        !file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n)))
        throw Invalid("cannot read container");
    return out;
}
void Write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    // Never truncate a content-addressed file that an existing mount may still hold.
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw Invalid("cannot create compatibility container");
    std::size_t offset{};
    while (offset < bytes.size()) {
        const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{1024 * 1024}));
        DWORD written{};
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written != count) break;
        offset += written;
    }
    const bool complete = offset == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!complete) {
        std::error_code error;
        std::filesystem::remove(path, error);
        throw Invalid("cannot write compatibility container");
    }
}
void WriteImmutable(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        if (error || std::filesystem::file_size(path, error) != bytes.size() || error)
            throw Invalid("compatibility container cache entry does not match its content key");
        std::ifstream file(path, std::ios::binary);
        if (!file) throw Invalid("cannot verify compatibility container cache entry");
        std::array<std::uint8_t, 64 * 1024> buffer{};
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto count = (std::min)(buffer.size(), bytes.size() - offset);
            if (!file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(count)) ||
                std::memcmp(buffer.data(), bytes.data() + offset, count) != 0)
                throw Invalid("compatibility container cache key collision");
            offset += count;
        }
        return;
    }
    if (error) throw Invalid("cannot inspect compatibility container cache");
    Write(path, bytes);
}
std::string Utf8(std::span<const std::uint8_t> text, bool wide) {
    if (!wide) return {reinterpret_cast<const char*>(text.data()), text.size()};
    std::wstring value(text.size() / 2, L'\0');
    std::memcpy(value.data(), text.data(), text.size());
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n && !value.empty()) throw Invalid("invalid cooked name");
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}
std::vector<std::string> Names(Reader& r) {
    const auto count = r.Count();
    std::vector<std::string> names;
    if (!count) return names;
    const auto byte_count = r.Get<std::uint32_t>();
    r.Skip(8 + std::size_t(count) * 8);
    std::vector<std::uint16_t> lengths;
    lengths.reserve(count);
    for (std::uint32_t i{}; i < count; ++i) lengths.push_back(static_cast<std::uint16_t>(r.Integer(2, true)));
    const auto start = r.pos;
    names.reserve(count);
    for (const auto size : lengths) {
        const auto length = std::size_t(size & 0x7fff) * (size & 0x8000 ? 2 : 1);
        const auto at = r.pos; r.Skip(length);
        names.push_back(Utf8(r.bytes.subspan(at, length), (size & 0x8000) != 0));
    }
    if (r.pos - start != byte_count) throw Invalid("invalid name batch");
    return names;
}
std::array<std::uint8_t, 32> Hash(std::span<const std::uint8_t> bytes) {
    blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, bytes.data(), bytes.size());
    std::array<std::uint8_t, 32> out{}; blake3_hasher_finalize(&h, out.data(), out.size()); return out;
}
void Decrypt(Bytes& bytes, const std::array<std::uint8_t, 32>& key) {
    if (bytes.size() % 16) throw Invalid("unaligned encrypted container block");
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_KEY_HANDLE handle{};
    auto status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)), sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    if (status >= 0) status = BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    ULONG written{};
    if (status >= 0) status = BCryptDecrypt(handle, bytes.data(), static_cast<ULONG>(bytes.size()),
        nullptr, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()), &written, 0);
    if (handle) BCryptDestroyKey(handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0 || written != bytes.size()) throw Invalid("container decryption failed");
}

struct Chunk { std::array<std::uint8_t, 12> id{}; std::uint64_t offset{}, size{}; };
struct StoreEntry { std::vector<std::uint64_t> imports; Bytes shaders; };
struct Container {
    std::filesystem::path path;
    Bytes toc;
    std::vector<Chunk> chunks;
    std::map<std::uint64_t, std::size_t> packages;
    std::map<std::uint64_t, StoreEntry> store;
    std::set<std::uint64_t> optional_packages;
    std::vector<std::string> methods;
    std::uint32_t block_size{}, block_count{}, flags{};
    std::uint64_t partition_size{};
    std::size_t blocks_at{};
    std::array<std::uint8_t, 32> key{};

    Container(std::filesystem::path file, const Ue5ContainerCodecs& codecs) : path(std::move(file)) {
        toc = Range(path, 0, static_cast<std::size_t>(std::filesystem::file_size(path)));
        Reader r{toc};
        constexpr char magic[] = "-==--==--==--==-";
        if (toc.size() < 144 || std::memcmp(toc.data(), magic, 16)) throw Invalid("invalid IoStore header");
        const auto version = r.At<std::uint8_t>(16);
        if (version < 5 || version > 8 || r.At<std::uint32_t>(20) != 144 || r.At<std::uint32_t>(32) != 12)
            throw Invalid("unsupported IoStore serialization version");
        const auto count = r.At<std::uint32_t>(24), methods_count = r.At<std::uint32_t>(36);
        const auto method_size = r.At<std::uint32_t>(40), seeds = r.At<std::uint32_t>(84), overflow = r.At<std::uint32_t>(96);
        block_count = r.At<std::uint32_t>(28); block_size = r.At<std::uint32_t>(44);
        partition_size = r.At<std::uint64_t>(88); flags = r.At<std::uint8_t>(80);
        if (count > MaxEntries || !block_size || block_size > 16 * 1024 * 1024 || !partition_size ||
            methods_count > 64 || method_size > 256 || (methods_count && !method_size)) throw Invalid("invalid IoStore dimensions");
        if (flags & 2) {
            std::array<std::uint8_t, 16> guid{}; std::memcpy(guid.data(), toc.data() + 64, guid.size());
            if (!codecs.key || !codecs.key(guid, key)) throw Invalid("engine encryption key unavailable");
        }
        r.pos = 144; chunks.resize(count);
        for (auto& c : chunks) c.id = r.Get<std::array<std::uint8_t, 12>>();
        for (auto& c : chunks) { c.offset = r.Integer(5, true); c.size = r.Integer(5, true); }
        r.Skip(std::size_t(seeds) * 4 + std::size_t(overflow) * 4);
        blocks_at = r.pos; r.Skip(std::size_t(block_count) * 12);
        methods.push_back("None");
        for (std::uint32_t i{}; i < methods_count; ++i) {
            const auto at = r.pos; r.Skip(method_size);
            const auto* start = reinterpret_cast<const char*>(toc.data() + at);
            const auto* end = static_cast<const char*>(std::memchr(start, 0, method_size));
            if (!end) throw Invalid("unterminated compression method");
            methods.emplace_back(start, end);
        }
        std::size_t header = count;
        for (std::size_t i{}; i < chunks.size(); ++i) {
            const auto& c = chunks[i];
            if (c.id.back() == 1) {
                const auto id = Reader{c.id}.Get<std::uint64_t>();
                if (!packages.emplace(id, i).second) throw Invalid("duplicate package chunk");
            } else if (c.id.back() == 6) {
                if (header != count) throw Invalid("multiple container headers");
                header = i;
            }
        }
        if (header == count) { if (!packages.empty()) throw Invalid("container package store absent"); return; }
        const auto data = Read(header, codecs); Reader s{data};
        if (s.Get<std::uint32_t>() != 0x496f436e) throw Invalid("invalid package store signature");
        const auto sv = s.Get<std::uint32_t>();
        if (sv < 3 || sv > 5) throw Invalid("unsupported package store version");
        s.Skip(8); const auto n = s.Count(); std::vector<std::uint64_t> ids(n);
        for (auto& id : ids) id = s.Get<std::uint64_t>();
        const auto bytes = s.Get<std::uint32_t>(); const auto begin = s.pos; s.Skip(bytes);
        if (std::uint64_t(n) * 16 > bytes) throw Invalid("truncated package store entries");
        Reader entries{std::span(data).subspan(begin, bytes)};
        for (std::size_t i{}; i < ids.size(); ++i) {
            StoreEntry value;
            for (unsigned kind{}; kind < 2; ++kind) {
                const auto at = i * 16 + kind * 8; entries.pos = at;
                const auto elements = entries.Count(), relative = entries.Get<std::uint32_t>();
                entries.pos = at + relative;
                if (!kind) for (std::uint32_t j{}; j < elements; ++j) value.imports.push_back(entries.Get<std::uint64_t>());
                else {
                    const auto start = entries.pos; entries.Skip(std::size_t(elements) * 20);
                    value.shaders.assign(entries.bytes.begin() + start, entries.bytes.begin() + entries.pos);
                }
            }
            if (!store.emplace(ids[i], std::move(value)).second) throw Invalid("duplicate package store entry");
        }
        const auto optional_count = s.Count();
        for (std::uint32_t i{}; i < optional_count; ++i) optional_packages.insert(s.Get<std::uint64_t>());
        s.Skip(s.Get<std::uint32_t>());
        for (const auto& [id, index] : packages) {
            static_cast<void>(index);
            if (!store.contains(id)) throw Invalid("package chunk has no store entry");
        }
    }
    ~Container() { SecureZeroMemory(key.data(), key.size()); }

    Bytes Read(std::size_t index, const Ue5ContainerCodecs& codecs, std::size_t limit = MaxMetadata) const {
        const auto& c = chunks.at(index);
        const auto length = (std::min)(c.size, std::uint64_t(limit));
        if (length > MaxMetadata) throw Invalid("cooked metadata too large");
        Bytes out; out.reserve(static_cast<std::size_t>(length));
        for (auto cursor = c.offset; cursor < c.offset + length;) {
            const auto block = cursor / block_size;
            if (block >= block_count) throw Invalid("invalid IoStore block index");
            Reader r{toc, blocks_at + static_cast<std::size_t>(block) * 12};
            const auto offset = r.Integer(5), compressed = r.Integer(3), raw_size = r.Integer(3), method = r.Integer(1);
            if (!compressed || !raw_size || raw_size > block_size || method >= methods.size()) throw Invalid("invalid IoStore block");
            auto data_path = path; data_path.replace_extension(L".ucas");
            const auto partition = offset / partition_size;
            if (partition) data_path = path.parent_path() / (path.stem().wstring() + L"_s" + std::to_wstring(partition) + L".ucas");
            auto bytes = Range(data_path, offset % partition_size,
                static_cast<std::size_t>((flags & 2) ? (compressed + 15) & ~15ull : compressed));
            if (flags & 2) Decrypt(bytes, key);
            bytes.resize(static_cast<std::size_t>(compressed));
            if (method) {
                Bytes decoded(static_cast<std::size_t>(raw_size));
                if (!codecs.decompress || !codecs.decompress(methods[method], bytes, decoded))
                    throw Invalid("engine could not decompress cooked metadata");
                bytes.swap(decoded);
            } else if (compressed != raw_size) throw Invalid("invalid uncompressed block size");
            const auto skip = cursor % block_size;
            if (skip >= raw_size) throw Invalid("invalid package block range");
            const auto n = (std::min)(raw_size - skip, c.offset + length - cursor);
            out.insert(out.end(), bytes.begin() + static_cast<std::size_t>(skip), bytes.begin() + static_cast<std::size_t>(skip + n));
            cursor += n;
        }
        return out;
    }
};

enum class DefinitionKind : std::uint8_t { Struct, Enum };
using ScriptNames = std::map<std::uint64_t, std::string>;
ScriptNames ReadScriptNames(const Container& container, const Ue5ContainerCodecs& codecs) {
    ScriptNames result;
    struct Entry { std::string name; std::uint64_t outer; };
    for (std::size_t i{}; i < container.chunks.size(); ++i) if (container.chunks[i].id.back() == 5) {
        const auto data = container.Read(i, codecs);
        Reader r{data}; const auto names = Names(r); const auto count = r.Count();
        std::map<std::uint64_t, Entry> entries;
        for (std::uint32_t j{}; j < count; ++j) {
            const auto mapped = r.Get<std::uint32_t>(), number = r.Get<std::uint32_t>();
            const auto id = r.Get<std::uint64_t>(), outer = r.Get<std::uint64_t>(); r.Skip(8);
            const auto index = mapped & 0x3fffffff;
            if ((mapped >> 30) != 2 || index >= names.size() || number > MaxEntries || (id >> 62) != 1)
                throw Invalid("invalid script object table");
            if (!entries.emplace(id, Entry{names[index] + (number ? "_" + std::to_string(number - 1) : ""), outer}).second)
                throw Invalid("duplicate script object");
        }
        if (r.pos != data.size()) throw Invalid("unsupported script object table extension");
        for (const auto& [id, entry] : entries) {
            auto name = entry.name; auto outer = entry.outer;
            for (unsigned depth{}; outer != NullObject; ++depth) {
                if (depth >= 64 || !entries.contains(outer)) throw Invalid("invalid script object outer");
                const auto& parent = entries.at(outer); name = parent.name + "/" + name; outer = parent.outer;
            }
            result.emplace(id, std::move(name));
        }
    }
    return result;
}

struct Export {
    std::uint64_t offset{}, size{}, outer{}, type{}, super{}, public_hash{};
    std::string name;
    std::uint32_t flags{};
};
struct Package {
    Bytes bytes;
    std::uint64_t id{};
    std::uint32_t header{};
    std::vector<std::string> names;
    std::vector<std::uint64_t> imports, hashes, dependencies;
    std::vector<Export> exports;
    std::string name;

    Package(const Container& container, std::uint64_t package, const Ue5ContainerCodecs& codecs, bool full)
        : id(package), dependencies(container.store.at(package).imports) {
        const auto chunk = container.packages.at(id);
        bytes = container.Read(chunk, codecs, full ? MaxMetadata : 65536);
        Reader r{bytes}; header = r.At<std::uint32_t>(4);
        if (header < 60 || header > MaxMetadata || header > container.chunks[chunk].size) throw Invalid("invalid Zen package header");
        if (bytes.size() < header) bytes = container.Read(chunk, codecs, header);
        r = {bytes};
        const auto public_at = r.At<std::uint32_t>(24), import_at = r.At<std::uint32_t>(28);
        const auto export_at = r.At<std::uint32_t>(32), bundle_at = r.At<std::uint32_t>(36);
        if (!(public_at <= import_at && import_at <= export_at && export_at <= bundle_at && bundle_at <= header) ||
            (import_at - public_at) % 8 || (export_at - import_at) % 8)
            throw Invalid("invalid Zen export tables");
        r.pos = 52;
        const auto versioned = r.At<std::uint32_t>(0);
        if (versioned > 1) throw Invalid("invalid Zen version flag");
        std::uint32_t ue5_version = 1017; // UE 5.6 serialization contract for unversioned packages.
        if (versioned) {
            if (r.Get<std::uint32_t>() != 3) throw Invalid("unsupported Zen package version");
            r.Skip(4); ue5_version = r.Get<std::uint32_t>(); r.Skip(4);
            if (ue5_version < 1009 || ue5_version > 1017) throw Invalid("unsupported cooked package version");
            const auto versions = r.Count(); r.Skip(std::size_t(versions) * 20);
        }
        if (ue5_version >= 1015) { // VERSE_CELLS
            const auto cell_import = r.Get<std::uint32_t>(), cell_export = r.Get<std::uint32_t>();
            if (cell_import != bundle_at || cell_export != bundle_at)
                throw Invalid("Verse cell exports require a different compatibility contract");
        }
        if ((bundle_at - export_at) % 72) throw Invalid("invalid Zen export map size");
        names = Names(r);
        if (r.pos > public_at) throw Invalid("invalid Zen name batch boundary");
        name = Mapped(r.At<std::uint32_t>(8), r.At<std::uint32_t>(12));
        for (auto at = public_at; at < import_at; at += 8) hashes.push_back(r.At<std::uint64_t>(at));
        for (auto at = import_at; at < export_at; at += 8) imports.push_back(r.At<std::uint64_t>(at));
        for (auto at = export_at; at < bundle_at; at += 72) {
            Export e;
            e.offset = r.At<std::uint64_t>(at); e.size = r.At<std::uint64_t>(at + 8);
            e.name = Mapped(r.At<std::uint32_t>(at + 16), r.At<std::uint32_t>(at + 20));
            e.outer = r.At<std::uint64_t>(at + 24); e.type = r.At<std::uint64_t>(at + 32);
            e.super = r.At<std::uint64_t>(at + 40); e.public_hash = r.At<std::uint64_t>(at + 56);
            e.flags = r.At<std::uint32_t>(at + 64);
            if (e.offset > container.chunks[chunk].size - header || e.size > container.chunks[chunk].size - header - e.offset)
                throw Invalid("export outside package");
            exports.push_back(std::move(e));
        }
    }
    std::string Mapped(std::uint32_t index, std::uint32_t number) const {
        if (index >= names.size() || number > MaxEntries) throw Invalid("invalid package name reference");
        return names[index] + (number ? "_" + std::to_string(number - 1) : "");
    }
    std::string Name(Reader& r) const { const auto index = r.Get<std::uint32_t>(); return Mapped(index, r.Get<std::uint32_t>()); }
    std::uint64_t Index(std::int32_t index) const {
        if (!index) return NullObject;
        if (index > 0) {
            if (static_cast<std::size_t>(index) > exports.size()) throw Invalid("invalid local export reference");
            return static_cast<std::uint32_t>(index - 1);
        }
        const auto i = -std::int64_t(index) - 1;
        if (static_cast<std::uint64_t>(i) >= imports.size()) throw Invalid("invalid local import reference");
        return imports[static_cast<std::size_t>(i)];
    }
    std::string Identity(std::uint64_t object, unsigned depth = 0) const {
        if (object == NullObject) return "null";
        if (depth > 32) throw Invalid("cyclic export outer");
        const auto kind = object >> 62;
        if (kind == 1) return "script:" + std::to_string(object);
        if (kind == 2) {
            const auto package = (object >> 32) & 0x3fffffff, hash = object & 0xffffffff;
            if (package >= dependencies.size() || hash >= hashes.size()) throw Invalid("invalid public export reference");
            return std::to_string(dependencies[package]) + ":" + std::to_string(hashes[hash]);
        }
        if (kind || object >= exports.size()) throw Invalid("invalid package object index");
        const auto& e = exports[static_cast<std::size_t>(object)];
        return (e.outer == NullObject ? std::to_string(id) : Identity(e.outer, depth + 1)) + "/" + e.name;
    }
    struct TypeReference {
        std::string identity;
        std::uint64_t package_id{}, public_hash{};
        bool has_public_hash{};
        bool operator==(const TypeReference& other) const { return identity == other.identity; }
    };
    TypeReference Reference(std::uint64_t object) const {
        TypeReference result;
        result.identity = Identity(object);
        if (object == NullObject) return result;
        const auto kind = object >> 62;
        if (kind == 0) {
            result.package_id = id;
        } else if (kind == 2) {
            const auto package = (object >> 32) & 0x3fffffff;
            const auto hash = object & 0xffffffff;
            if (package >= dependencies.size() || hash >= hashes.size()) throw Invalid("invalid public export reference");
            result.package_id = dependencies[static_cast<std::size_t>(package)];
            result.public_hash = hashes[static_cast<std::size_t>(hash)];
            result.has_public_hash = true;
        }
        return result;
    }
    TypeReference Reference(std::int32_t index) const { return Reference(Index(index)); }
    std::optional<std::size_t> FindExport(const TypeReference& reference) const {
        if (reference.package_id != id) return std::nullopt;
        for (std::size_t i{}; i < exports.size(); ++i) {
            if (reference.has_public_hash ? exports[i].public_hash == reference.public_hash
                                         : Identity(i) == reference.identity) return i;
        }
        return std::nullopt;
    }
    std::map<std::size_t, DefinitionKind> Definitions(const ScriptNames& scripts) const {
        std::map<std::size_t, DefinitionKind> result;
        for (const auto& e : exports) if ((e.flags & 0x10u) && e.type < exports.size())
            result.emplace(static_cast<std::size_t>(e.type), DefinitionKind::Struct);
        for (std::size_t i{}; i < exports.size(); ++i) {
            const auto type = scripts.find(exports[i].type);
            if (type == scripts.end()) continue;
            if (type->second == "/Script/CoreUObject/ScriptStruct" || type->second == "/Script/Engine/UserDefinedStruct")
                result.emplace(i, DefinitionKind::Struct);
            else if (type->second == "/Script/CoreUObject/Enum" || type->second == "/Script/Engine/UserDefinedEnum")
                result.emplace(i, DefinitionKind::Enum);
        }
        return result;
    }
    std::span<const std::uint8_t> Data(std::size_t index) const {
        const auto& e = exports.at(index); Reader r{bytes, header + static_cast<std::size_t>(e.offset)};
        const auto at = r.pos; r.Skip(static_cast<std::size_t>(e.size)); return r.bytes.subspan(at, static_cast<std::size_t>(e.size));
    }
};

struct DefinitionReference {
    Package::TypeReference type;
    DefinitionKind kind{};
    bool operator==(const DefinitionReference& other) const { return kind == other.kind && type == other.type; }
};
struct Field {
    std::string type, name;
    std::uint32_t dimension{};
    std::uint64_t serialized_flags{};
    std::vector<std::string> attributes;
    std::vector<Package::TypeReference> references;
    std::vector<DefinitionReference> definitions;
    std::vector<Field> inner;
    bool operator==(const Field&) const = default;
};
Field ReadField(const Package& p, Reader& r, unsigned depth = 0) {
    if (depth > 16) throw Invalid("nested field depth exceeds limit");
    Field f; f.type = p.Name(r); f.name = p.Name(r); r.Skip(4);
    f.dimension = r.Get<std::uint32_t>(); const auto size = r.Get<std::uint32_t>();
    const auto flags = r.Get<std::uint64_t>();
    // UE's game schema hashes only whether a property is editor-only from the serialized flags.
    f.serialized_flags = flags & 0x0000000800000000ull;
    r.Skip(2); static_cast<void>(p.Name(r)); const auto condition = r.Get<std::uint8_t>();
    if (!f.dimension || f.dimension > 65536 || size >= 0x1000000 || condition >= 64) throw Invalid("invalid field declaration");
    const auto ref = [&](const std::optional<DefinitionKind> kind = std::nullopt) {
        auto reference = p.Reference(r.Get<std::int32_t>());
        f.references.push_back(reference);
        if (kind) f.definitions.push_back({std::move(reference), *kind});
    };
    const auto& t = f.type;
    if (t == "Int8Property" || t == "Int16Property" || t == "IntProperty" || t == "Int64Property" ||
        t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property" || t == "FloatProperty" ||
        t == "DoubleProperty" || t == "NameProperty" || t == "StrProperty" || t == "TextProperty") {}
    else if (t == "ObjectProperty" || t == "SoftObjectProperty" || t == "WeakObjectProperty" ||
        t == "LazyObjectProperty" || t == "InterfaceProperty" || t == "DelegateProperty" ||
        t == "MulticastDelegateProperty" || t == "MulticastInlineDelegateProperty" || t == "MulticastSparseDelegateProperty") ref();
    else if (t == "StructProperty") ref(DefinitionKind::Struct);
    else if (t == "ByteProperty") ref(DefinitionKind::Enum);
    else if (t == "ClassProperty" || t == "SoftClassProperty") { ref(); ref(); }
    else if (t == "BoolProperty") {
        std::array<std::uint8_t, 6> values{};
        for (auto& value : values) {
            value = r.Get<std::uint8_t>();
            f.attributes.push_back(std::to_string(value));
        }
        const auto size_bool = values[4], native = values[5];
        if (native > 1 || !size_bool || size_bool > 8) throw Invalid("invalid bool declaration");
    } else if (t == "ArrayProperty" || t == "SetProperty" || t == "OptionalProperty") f.inner.push_back(ReadField(p, r, depth + 1));
    else if (t == "MapProperty") { f.inner.push_back(ReadField(p, r, depth + 1)); f.inner.push_back(ReadField(p, r, depth + 1)); }
    else if (t == "EnumProperty") { ref(DefinitionKind::Enum); f.inner.push_back(ReadField(p, r, depth + 1)); }
    else if (t == "FieldPathProperty") f.attributes.push_back(p.Name(r));
    else throw Invalid("unsupported field declaration: " + t);
    return f;
}
struct Schema { Package::TypeReference parent; std::vector<Field> fields; bool operator==(const Schema&) const = default; };
Schema ReadSchema(const Package& p, std::size_t index) {
    const auto& e = p.exports[index]; const auto bytes = p.Data(index);
    Schema out; out.parent = p.Reference(e.super); unsigned matches{};
    // UObject's native property prefix is variable-sized. Locate the UStruct record
    // by its declared superclass, local child exports, recursive FField records and
    // bounded bytecode section. Accept exactly one structurally valid record.
    for (std::size_t at{}; at + 20 <= bytes.size(); ++at) {
        try {
            Reader r{bytes, at};
            if (p.Index(r.Get<std::int32_t>()) != e.super) continue;
            const auto children = r.Count();
            if (children > p.exports.size()) continue;
            std::set<std::uint32_t> seen;
            for (std::uint32_t i{}; i < children; ++i) {
                const auto child = r.Get<std::uint32_t>();
                if (!child || child > p.exports.size() || p.exports[child - 1].outer != index || !seen.insert(child).second)
                    throw Invalid("invalid class child export");
            }
            const auto count = r.Count();
            if (count > 65536 || count > (bytes.size() - r.pos) / 47) continue;
            std::vector<Field> fields; fields.reserve(count);
            for (std::uint32_t i{}; i < count; ++i) fields.push_back(ReadField(p, r));
            const auto code_size = r.Get<std::uint32_t>(), storage_size = r.Get<std::uint32_t>();
            if (code_size > MaxMetadata || storage_size > bytes.size() - r.pos) continue;
            r.Skip(storage_size);
            out.fields = std::move(fields); ++matches;
        } catch (const Invalid&) {}
    }
    if (matches != 1) throw Invalid("class schema is not uniquely readable: " + p.name + "." + e.name);
    return out;
}

std::vector<std::string> EnumNames(const Package& p, std::size_t index) {
    const auto bytes = p.Data(index);
    unsigned matches{};
    std::vector<std::string> names;
    for (std::size_t at{}; at + 5 <= bytes.size(); ++at) {
        try {
            Reader r{bytes, at};
            const auto count = r.Count();
            if (!count || count > 65536 || std::uint64_t(count) * 16 + 1 > bytes.size() - r.pos) continue;
            std::vector<std::string> candidate; candidate.reserve(count);
            for (std::uint32_t i{}; i < count; ++i) {
                candidate.push_back(p.Name(r));
                r.Skip(sizeof(std::int64_t));
            }
            if (r.Get<std::uint8_t>() > 2 || r.pos != bytes.size()) continue;
            names = std::move(candidate); ++matches;
        } catch (const Invalid&) {}
    }
    if (matches != 1) throw Invalid("enum schema is not uniquely readable: " + p.name);
    return names;
}

struct RetainedPackage {
    std::uint64_t id{};
    Bytes data;
    StoreEntry store;
    std::string name;
};
struct RetainedChunk { std::array<std::uint8_t, 12> id; Bytes data; };

Bytes ContainerHeader(std::uint64_t container, const std::vector<RetainedPackage>& packages) {
    Bytes out;
    Append(out, std::uint32_t{0x496f436e}); Append(out, std::uint32_t{5}); Append(out, container);
    Append(out, static_cast<std::uint32_t>(packages.size()));
    for (const auto& p : packages) Append(out, p.id);
    Bytes entries(packages.size() * 16);
    for (std::size_t i{}; i < packages.size(); ++i) {
        const auto& p = packages[i]; const auto at = i * 16;
        Put(entries, at, static_cast<std::uint32_t>(p.store.imports.size()));
        Put(entries, at + 4, static_cast<std::uint32_t>(entries.size() - at));
        for (const auto id : p.store.imports) Append(entries, id);
        Put(entries, at + 8, static_cast<std::uint32_t>(p.store.shaders.size() / 20));
        Put(entries, at + 12, static_cast<std::uint32_t>(entries.size() - at - 8));
        Append(entries, p.store.shaders);
    }
    Append(out, static_cast<std::uint32_t>(entries.size())); Append(out, entries);
    // No optional segments, redirects, localized packages or soft-reference hints.
    for (unsigned i{}; i < 5; ++i) Append(out, std::uint32_t{});
    Append(out, std::uint64_t(out.size() + 16)); Append(out, std::uint64_t{4}); Append(out, std::uint32_t{});
    return out;
}

std::array<std::uint8_t, 20> Sha1(std::span<const std::uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm{}; std::array<std::uint8_t, 20> out{};
    auto status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    if (status >= 0) status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), out.data(), static_cast<ULONG>(out.size()));
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw Invalid("cannot hash compatibility pak index");
    return out;
}
Bytes EmptyPak() {
    // FPakInfo v8, with an empty legacy index. IoStore supplies the package data.
    const std::string mount = "../../../";
    Bytes index; Append(index, static_cast<std::uint32_t>(mount.size() + 1));
    index.insert(index.end(), mount.begin(), mount.end()); index.push_back(0); Append(index, std::uint32_t{});
    const auto digest = Sha1(index);
    Bytes footer(221);
    Put(footer, 17, std::uint32_t{0x5a6f12e1}); Put(footer, 21, std::uint32_t{8});
    Put(footer, 25, std::uint64_t{}); Put(footer, 33, std::uint64_t(index.size()));
    std::copy(digest.begin(), digest.end(), footer.begin() + 41);
    Append(index, footer); return index;
}
std::filesystem::path WriteOverlay(const std::filesystem::path& candidate,
    const std::filesystem::path& directory, const std::vector<RetainedPackage>& packages,
    const std::vector<RetainedChunk>& bulk) {
    const auto path_text = candidate.generic_u8string();
    Bytes identity(reinterpret_cast<const std::uint8_t*>(path_text.data()),
        reinterpret_cast<const std::uint8_t*>(path_text.data()) + path_text.size());
    Append(identity, std::uint32_t{4});
    Append(identity, static_cast<std::uint32_t>(packages.size()));
    for (const auto& p : packages) {
        Append(identity, p.id);
        const auto hash = Hash(p.data); Append(identity, std::span(hash));
        Append(identity, static_cast<std::uint32_t>(p.store.imports.size()));
        for (const auto imported : p.store.imports) Append(identity, imported);
        Append(identity, static_cast<std::uint64_t>(p.store.shaders.size()));
        Append(identity, std::span(p.store.shaders));
    }
    for (const auto& chunk : bulk) {
        Append(identity, std::span(chunk.id));
        const auto hash = Hash(chunk.data); Append(identity, std::span(hash));
    }
    const auto digest = Hash(identity);
    constexpr char hex[] = "0123456789abcdef";
    std::string stem = "schema-";
    for (std::size_t i{}; i < 12; ++i) { stem += hex[digest[i] >> 4]; stem += hex[digest[i] & 15]; }
    const auto container_id = Reader{digest}.Get<std::uint64_t>();
    const auto container_header = ContainerHeader(container_id, packages);
    Bytes ids, offsets, blocks, metadata, data;
    std::uint64_t logical{};
    const auto append_chunk = [&](const std::array<std::uint8_t, 12>& id, std::span<const std::uint8_t> bytes) {
        Append(ids, std::span(id));
        Packed(offsets, logical, 5, true); Packed(offsets, bytes.size(), 5, true);
        if (id.back() == 4) data.resize((data.size() + 65535) & ~std::size_t{65535});
        for (std::size_t at{}; at < bytes.size();) {
            const auto n = (std::min)(std::size_t{65536}, bytes.size() - at);
            data.resize((data.size() + 15) & ~std::size_t{15});
            Packed(blocks, data.size(), 5); Packed(blocks, n, 3); Packed(blocks, n, 3); blocks.push_back(0);
            Append(data, bytes.subspan(at, n)); at += n;
        }
        const auto hash = Hash(bytes); metadata.insert(metadata.end(), hash.begin(), hash.begin() + 20);
        Append(metadata, std::uint32_t{id.back() == 4 ? 2u : 0u});
        logical += (bytes.size() + 65535) & ~std::uint64_t{65535};
    };
    const auto chunk_id = [](std::uint64_t id, std::uint8_t type) {
        std::array<std::uint8_t, 12> value{};
        std::memcpy(value.data(), &id, sizeof(id)); value.back() = type;
        return value;
    };
    for (const auto& p : packages) append_chunk(chunk_id(p.id, 1), p.data);
    for (const auto& chunk : bulk) append_chunk(chunk.id, chunk.data);
    append_chunk(chunk_id(container_id, 6), container_header);
    // ReadContainerHeader reads Align(logical size, AESBlockSize), including the final block.
    data.resize((data.size() + 15) & ~std::size_t{15});
    Bytes toc(144);
    constexpr char magic[] = "-==--==--==--==-";
    std::memcpy(toc.data(), magic, 16); toc[16] = 8;
    Put(toc, 20, std::uint32_t{144}); Put(toc, 24, static_cast<std::uint32_t>(packages.size() + bulk.size() + 1));
    Put(toc, 28, static_cast<std::uint32_t>(blocks.size() / 12)); Put(toc, 32, std::uint32_t{12});
    Put(toc, 40, std::uint32_t{32}); Put(toc, 44, std::uint32_t{65536});
    Put(toc, 52, std::uint32_t{1}); Put(toc, 56, container_id); Put(toc, 88, ~std::uint64_t{});
    Append(toc, ids); Append(toc, offsets); Append(toc, blocks); Append(toc, metadata);
    std::filesystem::create_directories(directory);
    const auto pak = directory / (stem + ".pak");
    const auto empty_pak = EmptyPak();
    WriteImmutable(directory / (stem + ".ucas"), data);
    WriteImmutable(directory / (stem + ".utoc"), toc);
    WriteImmutable(pak, empty_pak);
    return pak;
}

} // namespace

struct Ue5ModCompatibility::Impl {
    struct Cached {
        std::filesystem::file_time_type stamp;
        std::uintmax_t size{};
        std::shared_ptr<Container> value;
    };
    std::map<std::filesystem::path, Cached> official;
    std::shared_ptr<Container> Get(const std::filesystem::path& path, const Ue5ContainerCodecs& codecs) {
        const auto stamp = std::filesystem::last_write_time(path);
        const auto size = std::filesystem::file_size(path);
        if (const auto i = official.find(path); i != official.end() && i->second.stamp == stamp && i->second.size == size)
            return i->second.value;
        auto value = std::make_shared<Container>(path, codecs);
        official.insert_or_assign(path, Cached{stamp, size, value}); return value;
    }
};
Ue5ModCompatibility::Ue5ModCompatibility() : impl_(std::make_unique<Impl>()) {}
Ue5ModCompatibility::~Ue5ModCompatibility() = default;

Ue5ModCompatibilityPlan Ue5ModCompatibility::Prepare(const std::filesystem::path& candidate,
    const std::vector<std::filesystem::path>& official_containers, const std::filesystem::path& output_directory,
    const Ue5ContainerCodecs& codecs) {
    auto toc = candidate; toc.replace_extension(L".utoc");
    if (!std::filesystem::is_regular_file(toc)) return {}; // Legacy paks do not use Zen unversioned export bundles.
    Container mod(toc, codecs);
    if (mod.packages.empty()) return {};
    std::map<std::uint64_t, std::shared_ptr<Container>> base;
    std::vector<std::shared_ptr<Container>> baseline_containers;
    ScriptNames scripts;
    std::set<std::filesystem::path> script_tables;
    bool inspected_baseline{};
    for (const auto& path : official_containers) {
        auto original = path; original.replace_extension(L".utoc");
        if (!std::filesystem::is_regular_file(original)) continue;
        inspected_baseline = true;
        const auto container = impl_->Get(original, codecs);
        baseline_containers.push_back(container);
        const auto global = original.parent_path() / L"global.utoc";
        if (script_tables.insert(global).second && std::filesystem::is_regular_file(global)) {
            const auto names = ReadScriptNames(*impl_->Get(global, codecs), codecs);
            scripts.insert(names.begin(), names.end());
        }
        for (const auto& [id, index] : container->packages) {
            static_cast<void>(index);
            if (mod.packages.contains(id)) base.try_emplace(id, container);
        }
    }
    if (!inspected_baseline) throw Invalid("no mounted game IoStore baseline available for Blueprint compatibility");
    if (scripts.empty()) throw Invalid("game script type table unavailable for cooked schema inspection");
    if (base.empty()) return {};

    std::map<std::uint64_t, std::unique_ptr<Package>> classes;
    for (const auto& [id, index] : mod.packages) {
        static_cast<void>(index);
        Package header(mod, id, codecs, false);
        if (!header.Definitions(scripts).empty()) classes.emplace(id, std::make_unique<Package>(mod, id, codecs, true));
        // A replacement that removes every class/struct must still be compared with its baseline.
        else if (base.contains(id)) {
            Package before(*base.at(id), id, codecs, false);
            if (!before.Definitions(scripts).empty()) classes.emplace(id, std::make_unique<Package>(mod, id, codecs, true));
        }
    }
    if (classes.empty()) return {};

    std::map<std::uint64_t, std::unique_ptr<Package>> mod_type_packages;
    std::map<std::uint64_t, std::unique_ptr<Package>> base_type_packages;
    const auto mod_package = [&](const std::uint64_t id) -> Package& {
        if (const auto found = classes.find(id); found != classes.end()) return *found->second;
        auto [found, inserted] = mod_type_packages.try_emplace(id);
        static_cast<void>(inserted);
        if (!found->second) found->second = std::make_unique<Package>(mod, id, codecs, true);
        return *found->second;
    };
    const auto base_package = [&](const std::uint64_t id) -> Package& {
        auto [found, inserted] = base_type_packages.try_emplace(id);
        static_cast<void>(inserted);
        if (!found->second) found->second = std::make_unique<Package>(*base.at(id), id, codecs, true);
        return *found->second;
    };

    std::set<std::uint64_t> retain;
    std::set<std::uint64_t> incompatible_type_packages;
    std::set<std::string> active_schemas;
    std::function<bool(const Package&, std::size_t, const Package&, std::size_t)> schema_compatible;
    std::function<bool(const Package::TypeReference&, const Package::TypeReference&, DefinitionKind)> definition_compatible;
    std::function<bool(const Field&, const Field&)> field_compatible;

    definition_compatible = [&](const Package::TypeReference& before, const Package::TypeReference& after,
                                const DefinitionKind kind) {
        if (before.identity != after.identity) return false;
        if (!after.package_id || !mod.packages.contains(after.package_id) || !base.contains(before.package_id)) return true;
        auto& old_package = base_package(before.package_id);
        auto& new_package = mod_package(after.package_id);
        const auto old_index = old_package.FindExport(before);
        const auto new_index = new_package.FindExport(after);
        if (!old_index || !new_index) {
            incompatible_type_packages.insert(after.package_id);
            return false;
        }
        const auto key = std::to_string(after.package_id) + ":" + after.identity + ":" +
            std::to_string(static_cast<unsigned>(kind));
        if (active_schemas.contains(key)) return true;
        active_schemas.insert(key);
        const bool compatible = kind == DefinitionKind::Struct
            ? schema_compatible(old_package, *old_index, new_package, *new_index)
            : EnumNames(old_package, *old_index) == EnumNames(new_package, *new_index);
        active_schemas.erase(key);
        if (!compatible) incompatible_type_packages.insert(after.package_id);
        return compatible;
    };
    field_compatible = [&](const Field& before, const Field& after) {
        if (before != after || before.definitions.size() != after.definitions.size() ||
            before.inner.size() != after.inner.size()) return false;
        for (std::size_t i{}; i < before.definitions.size(); ++i) {
            const auto& old_definition = before.definitions[i];
            const auto& new_definition = after.definitions[i];
            if (old_definition.kind != new_definition.kind ||
                !definition_compatible(old_definition.type, new_definition.type, new_definition.kind)) return false;
        }
        for (std::size_t i{}; i < before.inner.size(); ++i)
            if (!field_compatible(before.inner[i], after.inner[i])) return false;
        return true;
    };
    schema_compatible = [&](const Package& before_package, const std::size_t before_index,
                            const Package& after_package, const std::size_t after_index) {
        const auto key = "schema:" + std::to_string(after_package.id) + ":" + after_package.Identity(after_index);
        if (active_schemas.contains(key)) return true;
        active_schemas.insert(key);
        const auto before = ReadSchema(before_package, before_index);
        const auto after = ReadSchema(after_package, after_index);
        bool compatible = before == after;
        if (compatible) compatible = definition_compatible(before.parent, after.parent, DefinitionKind::Struct);
        if (compatible) {
            for (std::size_t i{}; i < before.fields.size(); ++i) {
                if (!field_compatible(before.fields[i], after.fields[i])) { compatible = false; break; }
            }
        }
        active_schemas.erase(key);
        return compatible;
    };

    for (const auto& [id, package] : classes) {
        // New classes have no existing game schema to displace.
        const auto original = base.find(id);
        if (original == base.end()) continue;
        Package before(*original->second, id, codecs, true);
        using Definition = std::pair<std::size_t, DefinitionKind>;
        std::map<std::string, Definition> old_classes, new_classes;
        for (const auto& [c, kind] : before.Definitions(scripts)) old_classes.emplace(before.Identity(c), Definition{c, kind});
        for (const auto& [c, kind] : package->Definitions(scripts)) new_classes.emplace(package->Identity(c), Definition{c, kind});
        // Additional definitions are safe when every existing definition remains compatible.
        bool compatible = true;
        for (const auto& [identity, old_definition] : old_classes) {
            if (!compatible) break;
            const auto replacement = new_classes.find(identity);
            if (replacement == new_classes.end() || old_definition.second != replacement->second.second) {
                compatible = false;
                break;
            }
            compatible = old_definition.second == DefinitionKind::Struct
                ? schema_compatible(before, old_definition.first, *package, replacement->second.first)
                : EnumNames(before, old_definition.first) == EnumNames(*package, replacement->second.first);
            if (!compatible) break;
        }
        if (!compatible) retain.insert(id);
    }
    retain.insert(incompatible_type_packages.begin(), incompatible_type_packages.end());
    if (retain.empty()) return {};
    // A mod child cooked against the rejected parent must not be paired with the
    // game's parent. Retain its baseline too; new dependent packages cannot be
    // safely separated and cause a pre-mount refusal instead of a loader fatal.
    bool changed;
    do {
        changed = false;
        for (const auto& [id, entry] : mod.store) if (!retain.contains(id) &&
            std::any_of(entry.imports.begin(), entry.imports.end(), [&](auto dependency) { return retain.contains(dependency); })) {
            if (!base.contains(id)) throw Invalid("incompatible Blueprint has a mod-only dependent package: " + std::to_string(id));
            retain.insert(id); changed = true;
        }
    } while (changed);
    std::vector<RetainedPackage> retained;
    std::vector<RetainedChunk> bulk;
    Ue5ModCompatibilityPlan plan;
    std::size_t retained_storage{};
    for (const auto id : retain) {
        const auto& container = *base.at(id);
        if (container.optional_packages.contains(id) || mod.optional_packages.contains(id))
            throw Invalid("cannot retain a schema with optional package segments");
        Package package(container, id, codecs, true);
        if (package.bytes.size() != container.chunks[container.packages.at(id)].size)
            throw Invalid("compatibility package exceeds supported metadata size");
        const auto& store = container.store.at(id);
        const auto store_bytes = store.shaders.size() + store.imports.size() * sizeof(std::uint64_t);
        if (package.bytes.size() > MaxOverlayBytes - retained_storage ||
            store_bytes > MaxOverlayBytes - retained_storage - package.bytes.size())
            throw Invalid("compatibility dependency closure exceeds memory budget");
        retained_storage += package.bytes.size() + store_bytes;
        plan.retained_packages.push_back(package.name);
        retained.push_back({id, std::move(package.bytes), container.store.at(id), std::move(package.name)});
    }
    // A retained package must not read mod bulk bytes with the original offsets or sizes.
    // Resolve every colliding chunk against the mounted baseline in the engine's read order.
    for (const auto& chunk : mod.chunks) {
        if (chunk.id.back() < 2 || chunk.id.back() > 4 || !retain.contains(Reader{chunk.id}.Get<std::uint64_t>())) continue;
        bool found{};
        for (const auto& container : baseline_containers) {
            const auto original = std::find_if(container->chunks.begin(), container->chunks.end(),
                [&](const auto& value) { return value.id == chunk.id; });
            if (original == container->chunks.end()) continue;
            if (original->size > MaxMetadata || original->size > MaxOverlayBytes - retained_storage)
                throw Invalid("compatibility bulk dependency exceeds memory budget");
            const auto index = static_cast<std::size_t>(original - container->chunks.begin());
            auto data = container->Read(index, codecs);
            retained_storage += data.size();
            bulk.push_back({chunk.id, std::move(data)}); found = true; break;
        }
        if (!found) throw Invalid("retained package has mod-only bulk data that cannot be masked");
    }
    plan.overlay = WriteOverlay(candidate, output_directory, retained, bulk);
    return plan;
}

} // namespace anomaly
