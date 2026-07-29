#include "anomaly/npcap_capture.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

namespace anomaly {
namespace {

constexpr std::size_t kPcapErrorBufferSize = 256;
constexpr int kPcapSuccess = 0;
constexpr int kPcapBreak = -2;
constexpr std::uint32_t kPcapNetmaskUnknown = 0xFFFFFFFFU;
constexpr int kCaptureReadTimeoutMilliseconds = 1;

// These declarations mirror the stable libpcap ABI used by Npcap. They stay
// private so consumers do not need the Npcap SDK at compile or link time.
struct PcapHandle;

struct PcapAddress final {
    PcapAddress* next;
    sockaddr* address;
    sockaddr* netmask;
    sockaddr* broadcast_address;
    sockaddr* destination_address;
};

struct PcapInterface final {
    PcapInterface* next;
    char* name;
    char* description;
    PcapAddress* addresses;
    std::uint32_t flags;
};

struct PcapPacketHeader final {
    timeval timestamp;
    std::uint32_t captured_length;
    std::uint32_t original_length;
};

struct PcapBpfInstruction final {
    std::uint16_t code;
    std::uint8_t jump_true;
    std::uint8_t jump_false;
    std::uint32_t value;
};

struct PcapBpfProgram final {
    std::uint32_t length;
    PcapBpfInstruction* instructions;
};

struct PcapStatistics final {
    std::uint32_t received;
    std::uint32_t dropped;
    std::uint32_t interface_dropped;
};

static_assert(sizeof(PcapPacketHeader) == 16U);
static_assert(sizeof(PcapBpfInstruction) == 8U);
static_assert(sizeof(PcapBpfProgram) == 16U);
static_assert(sizeof(PcapStatistics) == 12U);

using PcapFindAllDevicesFn = int(__cdecl *)(PcapInterface**, char*);
using PcapFreeAllDevicesFn = void(__cdecl *)(PcapInterface*);
using PcapCreateFn = PcapHandle*(__cdecl *)(const char*, char*);
using PcapSetSnapLengthFn = int(__cdecl *)(PcapHandle*, int);
using PcapSetPromiscuousFn = int(__cdecl *)(PcapHandle*, int);
using PcapSetTimeoutFn = int(__cdecl *)(PcapHandle*, int);
using PcapSetImmediateModeFn = int(__cdecl *)(PcapHandle*, int);
using PcapActivateFn = int(__cdecl *)(PcapHandle*);
using PcapSetNonBlockingFn = int(__cdecl *)(PcapHandle*, int, char*);
using PcapCompileFn = int(__cdecl *)(
    PcapHandle*, PcapBpfProgram*, const char*, int, std::uint32_t);
using PcapSetFilterFn = int(__cdecl *)(PcapHandle*, PcapBpfProgram*);
using PcapFreeCodeFn = void(__cdecl *)(PcapBpfProgram*);
using PcapNextExFn = int(__cdecl *)(
    PcapHandle*, PcapPacketHeader**, const unsigned char**);
using PcapGetErrorFn = char*(__cdecl *)(PcapHandle*);
using PcapCloseFn = void(__cdecl *)(PcapHandle*);
using PcapStatisticsFn = int(__cdecl *)(PcapHandle*, PcapStatistics*);

void ClearError(std::string& error) noexcept {
    error.clear();
}

void SetError(std::string& error, std::string_view message) noexcept {
    try {
        error.assign(message);
    } catch (...) {
        error.clear();
    }
}

void SetWin32Error(std::string& error, std::string_view operation) noexcept {
    const DWORD code = GetLastError();
    char message[192]{};
    const int written = std::snprintf(
        message, sizeof(message), "%.*s failed (Windows error %lu)",
        static_cast<int>(operation.size()), operation.data(),
        static_cast<unsigned long>(code));
    const std::size_t length = written > 0
        ? std::min(static_cast<std::size_t>(written), sizeof(message) - 1U)
        : 0U;
    SetError(error, std::string_view(message, length));
}

void SetPcapError(
    std::string& error, std::string_view operation, const char* pcap_message) noexcept {
    try {
        error.assign(operation);
        error.append(" failed");
        if (pcap_message != nullptr && pcap_message[0] != '\0') {
            error.append(": ");
            error.append(pcap_message);
        }
    } catch (...) {
        error.clear();
    }
}

void SetPcapStatusError(
    std::string& error, std::string_view operation, int status,
    const char* pcap_message) noexcept {
    char prefix[128]{};
    const int written = std::snprintf(
        prefix, sizeof(prefix), "%.*s (status %d)",
        static_cast<int>(operation.size()), operation.data(), status);
    const std::size_t length = written > 0
        ? std::min(static_cast<std::size_t>(written), sizeof(prefix) - 1U)
        : 0U;
    SetPcapError(error, std::string_view(prefix, length), pcap_message);
}

class PcapApi final {
public:
    PcapApi() = default;

    ~PcapApi() { Reset(); }

    PcapApi(const PcapApi&) = delete;
    PcapApi& operator=(const PcapApi&) = delete;

    [[nodiscard]] bool Load(std::string& error) noexcept {
        if (module_ != nullptr) {
            ClearError(error);
            return true;
        }

        module_ = LoadLibraryW(L"wpcap.dll");
        if (module_ == nullptr) {
            SetWin32Error(error, "LoadLibraryW(wpcap.dll)");
            return false;
        }

        if (!Resolve("pcap_findalldevs", find_all_devices_, error) ||
            !Resolve("pcap_freealldevs", free_all_devices_, error) ||
            !Resolve("pcap_create", create_, error) ||
            !Resolve("pcap_set_snaplen", set_snap_length_, error) ||
            !Resolve("pcap_set_promisc", set_promiscuous_, error) ||
            !Resolve("pcap_set_timeout", set_timeout_, error) ||
            !Resolve("pcap_activate", activate_, error) ||
            !Resolve("pcap_setnonblock", set_non_blocking_, error) ||
            !Resolve("pcap_compile", compile_, error) ||
            !Resolve("pcap_setfilter", set_filter_, error) ||
            !Resolve("pcap_freecode", free_code_, error) ||
            !Resolve("pcap_next_ex", next_ex_, error) ||
            !Resolve("pcap_geterr", get_error_, error) ||
            !Resolve("pcap_close", close_, error) ||
            !Resolve("pcap_stats", statistics_, error)) {
            Reset();
            return false;
        }

        set_immediate_mode_ = ResolveOptional<PcapSetImmediateModeFn>("pcap_set_immediate_mode");
        ClearError(error);
        return true;
    }

    void Reset() noexcept {
        if (module_ != nullptr) FreeLibrary(module_);
        module_ = nullptr;
        find_all_devices_ = nullptr;
        free_all_devices_ = nullptr;
        create_ = nullptr;
        set_snap_length_ = nullptr;
        set_promiscuous_ = nullptr;
        set_timeout_ = nullptr;
        set_immediate_mode_ = nullptr;
        activate_ = nullptr;
        set_non_blocking_ = nullptr;
        compile_ = nullptr;
        set_filter_ = nullptr;
        free_code_ = nullptr;
        next_ex_ = nullptr;
        get_error_ = nullptr;
        close_ = nullptr;
        statistics_ = nullptr;
    }

    PcapFindAllDevicesFn find_all_devices_{};
    PcapFreeAllDevicesFn free_all_devices_{};
    PcapCreateFn create_{};
    PcapSetSnapLengthFn set_snap_length_{};
    PcapSetPromiscuousFn set_promiscuous_{};
    PcapSetTimeoutFn set_timeout_{};
    PcapSetImmediateModeFn set_immediate_mode_{};
    PcapActivateFn activate_{};
    PcapSetNonBlockingFn set_non_blocking_{};
    PcapCompileFn compile_{};
    PcapSetFilterFn set_filter_{};
    PcapFreeCodeFn free_code_{};
    PcapNextExFn next_ex_{};
    PcapGetErrorFn get_error_{};
    PcapCloseFn close_{};
    PcapStatisticsFn statistics_{};

private:
    template <typename Function>
    [[nodiscard]] bool Resolve(
        const char* name, Function& function, std::string& error) noexcept {
        const FARPROC address = GetProcAddress(module_, name);
        if (address == nullptr) {
            char message[160]{};
            const int written = std::snprintf(
                message, sizeof(message), "Npcap wpcap.dll does not export %s", name);
            const std::size_t length = written > 0
                ? std::min(static_cast<std::size_t>(written), sizeof(message) - 1U)
                : 0U;
            SetError(error, std::string_view(message, length));
            return false;
        }
        function = reinterpret_cast<Function>(address);
        return true;
    }

    template <typename Function>
    [[nodiscard]] Function ResolveOptional(const char* name) const noexcept {
        return reinterpret_cast<Function>(GetProcAddress(module_, name));
    }

    HMODULE module_{};
};

class PcapDeviceList final {
public:
    PcapDeviceList(PcapApi& api, PcapInterface* devices) noexcept
        : api_(api), devices_(devices) {}

    ~PcapDeviceList() {
        if (devices_ != nullptr) api_.free_all_devices_(devices_);
    }

    PcapDeviceList(const PcapDeviceList&) = delete;
    PcapDeviceList& operator=(const PcapDeviceList&) = delete;

private:
    PcapApi& api_;
    PcapInterface* devices_{};
};

class PcapProgram final {
public:
    explicit PcapProgram(PcapApi& api) noexcept : api_(api) {}

    ~PcapProgram() {
        if (compiled_) api_.free_code_(&program_);
    }

    PcapProgram(const PcapProgram&) = delete;
    PcapProgram& operator=(const PcapProgram&) = delete;

    [[nodiscard]] PcapBpfProgram* get() noexcept { return &program_; }
    void MarkCompiled() noexcept { compiled_ = true; }

private:
    PcapApi& api_;
    PcapBpfProgram program_{};
    bool compiled_{};
};

std::string FormatIpv4Address(const sockaddr_in& address) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&address.sin_addr);
    char text[16]{};
    const int written = std::snprintf(
        text, sizeof(text), "%u.%u.%u.%u", static_cast<unsigned int>(bytes[0]),
        static_cast<unsigned int>(bytes[1]), static_cast<unsigned int>(bytes[2]),
        static_cast<unsigned int>(bytes[3]));
    if (written <= 0) return {};
    return std::string(text, static_cast<std::size_t>(written));
}

std::string FormatIpv6Address(const sockaddr_in6& address) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&address.sin6_addr);
    std::array<std::uint16_t, 8> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        words[index] = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(bytes[index * 2U]) << 8U) |
            static_cast<std::uint16_t>(bytes[index * 2U + 1U]));
    }

    std::size_t best_start = words.size();
    std::size_t best_length{};
    for (std::size_t index = 0; index < words.size();) {
        if (words[index] != 0U) {
            ++index;
            continue;
        }
        const std::size_t start = index;
        while (index < words.size() && words[index] == 0U) ++index;
        const std::size_t length = index - start;
        if (length >= 2U && length > best_length) {
            best_start = start;
            best_length = length;
        }
    }

    std::string text;
    text.reserve(40U);
    for (std::size_t index = 0; index < words.size(); ++index) {
        if (index == best_start) {
            text.append("::");
            index += best_length - 1U;
            continue;
        }
        if (!text.empty() && text.back() != ':') text.push_back(':');
        char segment[5]{};
        const int written = std::snprintf(
            segment, sizeof(segment), "%x", static_cast<unsigned int>(words[index]));
        if (written <= 0) return {};
        text.append(segment, static_cast<std::size_t>(written));
    }
    if (address.sin6_scope_id != 0U) {
        text.push_back('%');
        text.append(std::to_string(address.sin6_scope_id));
    }
    return text;
}

std::string FormatAddress(const sockaddr* address) {
    if (address == nullptr) return {};
    switch (address->sa_family) {
    case AF_INET:
        return FormatIpv4Address(*reinterpret_cast<const sockaddr_in*>(address));
    case AF_INET6:
        return FormatIpv6Address(*reinterpret_cast<const sockaddr_in6*>(address));
    default:
        return {};
    }
}

void AppendAddress(const sockaddr* address, std::vector<std::string>& addresses) {
    std::string formatted = FormatAddress(address);
    if (!formatted.empty() &&
        std::find(addresses.begin(), addresses.end(), formatted) == addresses.end()) {
        addresses.push_back(std::move(formatted));
    }
}

const char* CaptureError(const PcapApi& api, PcapHandle* handle) noexcept {
    return handle == nullptr || api.get_error_ == nullptr ? nullptr : api.get_error_(handle);
}

}  // namespace

class NpcapCapture::Impl final {
public:
    ~Impl() { Close(); }

    void Close() noexcept {
        if (handle_ != nullptr) api_.close_(handle_);
        handle_ = nullptr;
    }

    PcapApi api_;
    PcapHandle* handle_{};
};

NpcapCapture::NpcapCapture(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

NpcapCapture::~NpcapCapture() = default;

std::vector<NpcapDevice> NpcapCapture::EnumerateDevices(std::string& error) noexcept {
    std::vector<NpcapDevice> devices;
    try {
        ClearError(error);
        PcapApi api;
        if (!api.Load(error)) return devices;

        std::array<char, kPcapErrorBufferSize> pcap_error{};
        PcapInterface* listed_devices{};
        if (api.find_all_devices_(&listed_devices, pcap_error.data()) != kPcapSuccess) {
            SetPcapError(error, "pcap_findalldevs", pcap_error.data());
            return devices;
        }
        PcapDeviceList list(api, listed_devices);

        for (const PcapInterface* current = listed_devices; current != nullptr;
             current = current->next) {
            if (current->name == nullptr || current->name[0] == '\0') continue;
            NpcapDevice device;
            device.name = current->name;
            if (current->description != nullptr) device.description = current->description;
            for (const PcapAddress* address = current->addresses; address != nullptr;
                 address = address->next) {
                AppendAddress(address->address, device.addresses);
            }
            devices.push_back(std::move(device));
        }
        ClearError(error);
        return devices;
    } catch (const std::exception& exception) {
        devices.clear();
        SetError(error, exception.what());
        return devices;
    } catch (...) {
        devices.clear();
        SetError(error, "Npcap device enumeration failed");
        return devices;
    }
}

std::unique_ptr<NpcapCapture> NpcapCapture::Open(
    const NpcapDevice& device, const std::string_view bpf_expression,
    std::string& error) noexcept {
    return Open(device.name, bpf_expression, error);
}

std::unique_ptr<NpcapCapture> NpcapCapture::Open(
    const std::string_view device_name, const std::string_view bpf_expression,
    std::string& error) noexcept {
    try {
        ClearError(error);
        if (device_name.empty()) {
            SetError(error, "Npcap device name is empty");
            return {};
        }
        if (device_name.find('\0') != std::string_view::npos ||
            bpf_expression.find('\0') != std::string_view::npos) {
            SetError(error, "Npcap device name or BPF expression contains a null byte");
            return {};
        }

        auto impl = std::make_unique<Impl>();
        if (!impl->api_.Load(error)) return {};

        std::string device_name_copy(device_name);
        std::array<char, kPcapErrorBufferSize> pcap_error{};
        impl->handle_ = impl->api_.create_(device_name_copy.c_str(), pcap_error.data());
        if (impl->handle_ == nullptr) {
            SetPcapError(error, "pcap_create", pcap_error.data());
            return {};
        }

        const auto configure = [&](const int status, const std::string_view operation) {
            if (status == kPcapSuccess) return true;
            SetPcapStatusError(
                error, operation, status, CaptureError(impl->api_, impl->handle_));
            return false;
        };
        if (!configure(impl->api_.set_snap_length_(impl->handle_, 65535), "pcap_set_snaplen") ||
            !configure(impl->api_.set_promiscuous_(impl->handle_, 0), "pcap_set_promisc") ||
            !configure(impl->api_.set_timeout_(
                impl->handle_, kCaptureReadTimeoutMilliseconds), "pcap_set_timeout")) {
            return {};
        }
        if (impl->api_.set_immediate_mode_ != nullptr &&
            !configure(impl->api_.set_immediate_mode_(impl->handle_, 1),
                "pcap_set_immediate_mode")) {
            return {};
        }
        const int activation = impl->api_.activate_(impl->handle_);
        if (activation < kPcapSuccess) {
            SetPcapStatusError(
                error, "pcap_activate", activation, CaptureError(impl->api_, impl->handle_));
            return {};
        }

        pcap_error.fill('\0');
        if (impl->api_.set_non_blocking_(impl->handle_, 1, pcap_error.data()) != kPcapSuccess) {
            const char* detail = pcap_error[0] == '\0'
                ? CaptureError(impl->api_, impl->handle_)
                : pcap_error.data();
            SetPcapError(error, "pcap_setnonblock", detail);
            return {};
        }

        if (!bpf_expression.empty()) {
            std::string filter_copy(bpf_expression);
            PcapProgram program(impl->api_);
            if (impl->api_.compile_(
                    impl->handle_, program.get(), filter_copy.c_str(), 1,
                    kPcapNetmaskUnknown) != kPcapSuccess) {
                SetPcapError(
                    error, "pcap_compile", CaptureError(impl->api_, impl->handle_));
                return {};
            }
            program.MarkCompiled();
            if (impl->api_.set_filter_(impl->handle_, program.get()) != kPcapSuccess) {
                SetPcapError(
                    error, "pcap_setfilter", CaptureError(impl->api_, impl->handle_));
                return {};
            }
        }

        ClearError(error);
        return std::unique_ptr<NpcapCapture>(new NpcapCapture(std::move(impl)));
    } catch (const std::exception& exception) {
        SetError(error, exception.what());
        return {};
    } catch (...) {
        SetError(error, "Npcap capture open failed");
        return {};
    }
}

NpcapNextResult NpcapCapture::Next(
    NpcapPacket& packet, const std::chrono::milliseconds timeout,
    std::string& error) noexcept {
    packet = {};
    try {
        ClearError(error);
        if (impl_ == nullptr || impl_->handle_ == nullptr) {
            SetError(error, "Npcap capture session is closed");
            return NpcapNextResult::Error;
        }

        const auto start = std::chrono::steady_clock::now();
        const auto requested_timeout = std::max(timeout, std::chrono::milliseconds::zero());
        for (;;) {
            PcapPacketHeader* header{};
            const unsigned char* bytes{};
            const int result = impl_->api_.next_ex_(impl_->handle_, &header, &bytes);
            if (result == 1) {
                if (header == nullptr || (header->captured_length != 0U && bytes == nullptr)) {
                    SetError(error, "pcap_next_ex returned an invalid packet");
                    return NpcapNextResult::Error;
                }
                packet.timestamp.seconds = static_cast<std::int64_t>(header->timestamp.tv_sec);
                packet.timestamp.microseconds = static_cast<std::int32_t>(header->timestamp.tv_usec);
                packet.captured_length = header->captured_length;
                packet.original_length = header->original_length;
                packet.bytes = std::span<const std::uint8_t>(
                    reinterpret_cast<const std::uint8_t*>(bytes), header->captured_length);
                return NpcapNextResult::Packet;
            }
            if (result < kPcapSuccess) {
                if (result == kPcapBreak) {
                    SetError(error, "pcap_next_ex was interrupted");
                } else {
                    SetPcapError(
                        error, "pcap_next_ex", CaptureError(impl_->api_, impl_->handle_));
                }
                return NpcapNextResult::Error;
            }
            if (requested_timeout == std::chrono::milliseconds::zero() ||
                std::chrono::steady_clock::now() - start >= requested_timeout) {
                return NpcapNextResult::Timeout;
            }
            Sleep(1U);
        }
    } catch (const std::exception& exception) {
        packet = {};
        SetError(error, exception.what());
        return NpcapNextResult::Error;
    } catch (...) {
        packet = {};
        SetError(error, "Npcap packet read failed");
        return NpcapNextResult::Error;
    }
}

bool NpcapCapture::Stats(NpcapStats& stats, std::string& error) const noexcept {
    stats = {};
    try {
        ClearError(error);
        if (impl_ == nullptr || impl_->handle_ == nullptr) {
            SetError(error, "Npcap capture session is closed");
            return false;
        }
        PcapStatistics native_stats{};
        if (impl_->api_.statistics_(impl_->handle_, &native_stats) != kPcapSuccess) {
            SetPcapError(error, "pcap_stats", CaptureError(impl_->api_, impl_->handle_));
            return false;
        }
        stats.received = native_stats.received;
        stats.dropped = native_stats.dropped;
        stats.interface_dropped = native_stats.interface_dropped;
        return true;
    } catch (const std::exception& exception) {
        stats = {};
        SetError(error, exception.what());
        return false;
    } catch (...) {
        stats = {};
        SetError(error, "Npcap statistics query failed");
        return false;
    }
}

bool NpcapCapture::IsOpen() const noexcept {
    return impl_ != nullptr && impl_->handle_ != nullptr;
}

void NpcapCapture::Close() noexcept {
    if (impl_ != nullptr) impl_->Close();
}

}  // namespace anomaly
