#include "anomaly/sdk/cpp.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifndef ANOMALY_WINSOCK_TRACE_OPERATION
#error "ANOMALY_WINSOCK_TRACE_OPERATION is required"
#endif

#ifndef ANOMALY_WINSOCK_TRACE_PLUGIN_ID
#error "ANOMALY_WINSOCK_TRACE_PLUGIN_ID is required"
#endif

namespace {

constexpr std::size_t kTraceRingCapacity = 512;
// A v2 event includes up to 16 return addresses; this stays below the host's 4 MiB storage limit.
constexpr std::size_t kMaximumHistoryRecords = 2048;
constexpr std::size_t kMaximumCallsites = 128;
constexpr std::size_t kTraceStackDepth = 16;
constexpr DWORD kMaximumScatterGatherBuffers = 64;
constexpr std::uint32_t kFlushIntervalMilliseconds = 250;
constexpr std::uint64_t kUnixEpochFileTimeTicks = 116444736000000000ULL;
constexpr std::uint64_t kFileTimeTicksPerMillisecond = 10000ULL;

enum class TraceOperation : std::uint8_t {
    Send = 1,
    SendTo = 2,
    WsaSend = 3,
    WsaSendTo = 4,
    Recv = 5,
    RecvFrom = 6,
    WsaRecv = 7,
    WsaRecvFrom = 8,
};

constexpr TraceOperation kOperation =
    static_cast<TraceOperation>(ANOMALY_WINSOCK_TRACE_OPERATION);

#if ANOMALY_WINSOCK_TRACE_OPERATION == 1
constexpr std::string_view kOperationName = "send";
constexpr bool kOutbound = true;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 2
constexpr std::string_view kOperationName = "sendto";
constexpr bool kOutbound = true;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 3
constexpr std::string_view kOperationName = "WSASend";
constexpr bool kOutbound = true;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 4
constexpr std::string_view kOperationName = "WSASendTo";
constexpr bool kOutbound = true;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 5
constexpr std::string_view kOperationName = "recv";
constexpr bool kOutbound = false;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 6
constexpr std::string_view kOperationName = "recvfrom";
constexpr bool kOutbound = false;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 7
constexpr std::string_view kOperationName = "WSARecv";
constexpr bool kOutbound = false;
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 8
constexpr std::string_view kOperationName = "WSARecvFrom";
constexpr bool kOutbound = false;
#else
#error "unsupported Winsock trace operation"
#endif

enum TraceFlags : std::uint32_t {
    TraceSuccess = 1U << 0U,
    TracePending = 1U << 1U,
    TraceOverlapped = 1U << 2U,
    TraceScatterGather = 1U << 3U,
    TraceScatterGatherTruncated = 1U << 4U,
};

enum class EndpointProvenance : std::uint8_t {
    Unavailable = 0,
    GetSockName = 1,
    GetPeerName = 2,
    SendToArgument = 3,
    RecvFromResult = 4,
};

struct EndpointMetadata final {
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port{};
    std::uint8_t family{};
    std::uint8_t address_size{};
    EndpointProvenance provenance{EndpointProvenance::Unavailable};
};

struct TraceRecord final {
    std::uint64_t sequence{};
    std::uint64_t qpc{};
    std::uint64_t socket{};
    std::uint64_t return_address{};
    std::array<std::uint64_t, kTraceStackDepth> stack{};
    std::uint32_t thread_id{};
    std::uint32_t requested_bytes{};
    std::uint32_t completed_bytes{};
    std::uint32_t buffer_count{};
    std::uint32_t flags{};
    std::int32_t result{};
    std::int32_t wsa_error{};
    std::int32_t socket_type{};
    std::uint32_t route_interface_index{};
    EndpointMetadata local_endpoint{};
    EndpointMetadata peer_endpoint{};
    std::uint8_t stack_depth{};
};

static_assert(std::is_trivially_copyable_v<TraceRecord>);

template <typename T, std::size_t Capacity>
class BoundedMpmcRing final {
    static_assert(Capacity != 0 && (Capacity & (Capacity - 1U)) == 0);

    struct Cell final {
        std::atomic<std::size_t> sequence{};
        T value{};
    };

public:
    BoundedMpmcRing() noexcept {
        for (std::size_t index = 0; index < Capacity; ++index) {
            cells_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool Push(const T& value) noexcept {
        std::size_t position = enqueue_position_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position & (Capacity - 1U)];
            const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const std::intptr_t difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position);
            if (difference == 0) {
                if (enqueue_position_.compare_exchange_weak(
                        position, position + 1U, std::memory_order_relaxed)) {
                    cell.value = value;
                    cell.sequence.store(position + 1U, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = enqueue_position_.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] bool Pop(T& value) noexcept {
        std::size_t position = dequeue_position_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = cells_[position & (Capacity - 1U)];
            const std::size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const std::intptr_t difference = static_cast<std::intptr_t>(sequence) -
                static_cast<std::intptr_t>(position + 1U);
            if (difference == 0) {
                if (dequeue_position_.compare_exchange_weak(
                        position, position + 1U, std::memory_order_relaxed)) {
                    value = cell.value;
                    cell.sequence.store(position + Capacity, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = dequeue_position_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    std::array<Cell, Capacity> cells_{};
    std::atomic<std::size_t> enqueue_position_{};
    std::atomic<std::size_t> dequeue_position_{};
};

struct CallsiteStats final {
    std::uint64_t calls{};
    std::uint64_t successes{};
    std::uint64_t errors{};
    std::uint64_t pending{};
    std::uint64_t requested_bytes{};
    std::uint64_t completed_bytes{};
};

struct Context final {
    const AnomalyHookServiceV1* hook{};
    const AnomalyStorageServiceV1* storage{};
    const AnomalySchedulerServiceV1* scheduler{};
    AnomalyGenerationHandleV1 hook_handle{};
    std::array<std::uint8_t, 16> capture_id{};
    std::uint64_t qpc_frequency{};
    std::uint64_t started_qpc{};
    std::uint64_t started_utc_unix_milliseconds{};
    std::uint64_t ended_qpc{};
    std::uint64_t ended_utc_unix_milliseconds{};
    std::atomic_bool capture_open{};
    std::atomic_bool stop_started{};
    BoundedMpmcRing<TraceRecord, kTraceRingCapacity> ring;
    std::atomic<std::uint64_t> next_sequence{};
    std::atomic<std::uint64_t> records_enqueued{};
    std::atomic<std::uint64_t> ring_dropped{};
    std::atomic<std::uint64_t> scheduling_failures{};
    std::mutex persistence_mutex;
    std::mutex flush_mutex;
    AnomalyGenerationHandleV1 flush_task{};
    std::vector<TraceRecord> history;
    std::map<std::uint64_t, CallsiteStats> callsites;
    std::uint64_t records_drained{};
    std::uint64_t records_recorded{};
    std::uint64_t history_dropped{};
    std::uint64_t aggregation_dropped{};
    std::uint64_t persistence_attempts{};
    std::uint64_t persistence_successes{};
    std::uint64_t persistence_failures{};
    bool finalization_requested{};
    bool hook_quiesced{};

    void Record(TraceRecord record) noexcept {
        record.sequence = next_sequence.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if (ring.Push(record)) {
            records_enqueued.fetch_add(1U, std::memory_order_relaxed);
        } else {
            ring_dropped.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    void Drain() {
        TraceRecord record;
        while (ring.Pop(record)) {
            ++records_drained;
            if (history.size() < kMaximumHistoryRecords) {
                history.push_back(record);
                ++records_recorded;
            } else {
                ++history_dropped;
            }
            auto found = callsites.find(record.return_address);
            if (found == callsites.end() && callsites.size() >= kMaximumCallsites) {
                ++aggregation_dropped;
                continue;
            }
            CallsiteStats& stats = callsites[record.return_address];
            ++stats.calls;
            stats.requested_bytes += record.requested_bytes;
            stats.completed_bytes += record.completed_bytes;
            if ((record.flags & TraceSuccess) != 0) ++stats.successes;
            if ((record.flags & TracePending) != 0) ++stats.pending;
            if (record.result == SOCKET_ERROR) ++stats.errors;
        }
    }
};

std::atomic<Context*> g_active{};
std::atomic<const AnomalyHookServiceV1*> g_hook_service{};
std::atomic<std::uint64_t> g_hook_id{};
std::atomic<std::uint64_t> g_hook_generation{};

using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using WsaSendFn = int(WSAAPI*)(
    SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WsaSendToFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using RecvFn = int(WSAAPI*)(SOCKET, char*, int, int);
using RecvFromFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using WsaRecvFn = int(WSAAPI*)(
    SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WsaRecvFromFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

std::atomic<SendFn> g_send_original{};
std::atomic<SendToFn> g_sendto_original{};
std::atomic<WsaSendFn> g_wsa_send_original{};
std::atomic<WsaSendToFn> g_wsa_sendto_original{};
std::atomic<RecvFn> g_recv_original{};
std::atomic<RecvFromFn> g_recvfrom_original{};
std::atomic<WsaRecvFn> g_wsa_recv_original{};
std::atomic<WsaRecvFromFn> g_wsa_recvfrom_original{};

constexpr AnomalyStatusV1 Status(const std::uint32_t code, const char* message = nullptr) noexcept {
    return {code, 0, {message, message == nullptr ? 0U : std::strlen(message)}};
}

template <typename Service>
const Service* Query(const AnomalyHostApiV1* host, const char* id) noexcept {
    return anomaly::sdk::Host(host).Query<Service>(id, 1).get();
}

std::uint64_t QueryPerformanceCounterValue() noexcept {
    LARGE_INTEGER value{};
    static_cast<void>(QueryPerformanceCounter(&value));
    return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QueryPerformanceFrequencyValue() noexcept {
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value) == FALSE
        ? 0U
        : static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t QueryUnixTimeMilliseconds() noexcept {
    FILETIME file_time{};
    GetSystemTimePreciseAsFileTime(&file_time);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    if (ticks.QuadPart < kUnixEpochFileTimeTicks) return 0;
    return (ticks.QuadPart - kUnixEpochFileTimeTicks) / kFileTimeTicksPerMillisecond;
}

void CaptureStack(TraceRecord& record) noexcept {
    void* addresses[kTraceStackDepth]{};
    const USHORT captured = RtlCaptureStackBackTrace(
        1U, static_cast<ULONG>(record.stack.size()), addresses, nullptr);
    record.stack_depth = static_cast<std::uint8_t>(captured);
    for (USHORT index = 0; index < captured; ++index) {
        record.stack[index] = reinterpret_cast<std::uintptr_t>(addresses[index]);
    }
}

std::uint32_t ClampByteCount(const int value) noexcept {
    return value <= 0 ? 0U : static_cast<std::uint32_t>(value);
}

std::uint32_t SafeDword(const LPDWORD value) noexcept {
    if (value == nullptr) return 0;
    __try {
        return *value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uint32_t SafeScatterGatherBytes(
    const LPWSABUF buffers, const DWORD count, bool& truncated) noexcept {
    truncated = count > kMaximumScatterGatherBuffers;
    if (buffers == nullptr || count == 0) return 0;
    const DWORD bounded_count = (std::min)(count, kMaximumScatterGatherBuffers);
    std::uint64_t result{};
    __try {
        for (DWORD index = 0; index < bounded_count; ++index) {
            result += buffers[index].len;
            if (result > (std::numeric_limits<std::uint32_t>::max)()) {
                return (std::numeric_limits<std::uint32_t>::max)();
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        truncated = true;
        return 0;
    }
    return static_cast<std::uint32_t>(result);
}

void CaptureEndpoint(
    const sockaddr* address, const int address_length, const EndpointProvenance provenance,
    EndpointMetadata& endpoint) noexcept {
    endpoint = {};
    if (address == nullptr || address_length < static_cast<int>(sizeof(address->sa_family))) return;
    __try {
        if (address->sa_family == AF_INET && address_length >= static_cast<int>(sizeof(sockaddr_in))) {
            const auto* const ipv4 = reinterpret_cast<const sockaddr_in*>(address);
            endpoint.family = static_cast<std::uint8_t>(AF_INET);
            endpoint.port = ntohs(ipv4->sin_port);
            endpoint.address_size = static_cast<std::uint8_t>(sizeof(ipv4->sin_addr));
            endpoint.provenance = provenance;
            std::memcpy(endpoint.address.data(), &ipv4->sin_addr, endpoint.address_size);
        } else if (address->sa_family == AF_INET6 &&
                   address_length >= static_cast<int>(sizeof(sockaddr_in6))) {
            const auto* const ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
            endpoint.family = static_cast<std::uint8_t>(AF_INET6);
            endpoint.port = ntohs(ipv6->sin6_port);
            endpoint.address_size = static_cast<std::uint8_t>(sizeof(ipv6->sin6_addr));
            endpoint.provenance = provenance;
            std::memcpy(endpoint.address.data(), &ipv6->sin6_addr, endpoint.address_size);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        endpoint = {};
    }
}

void SocketMetadata(
    const SOCKET socket, EndpointMetadata& local, EndpointMetadata& peer,
    std::int32_t& socket_type) noexcept {
    sockaddr_storage local_address{};
    sockaddr_storage peer_address{};
    int local_address_length = sizeof(local_address);
    int peer_address_length = sizeof(peer_address);
    int option_length = sizeof(socket_type);
    const int saved_error = WSAGetLastError();
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local_address), &local_address_length) == 0) {
        CaptureEndpoint(reinterpret_cast<const sockaddr*>(&local_address), local_address_length,
            EndpointProvenance::GetSockName, local);
    }
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer_address), &peer_address_length) == 0) {
        CaptureEndpoint(reinterpret_cast<const sockaddr*>(&peer_address), peer_address_length,
            EndpointProvenance::GetPeerName, peer);
    }
    if (getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socket_type),
            &option_length) != 0 || option_length != sizeof(socket_type)) {
        socket_type = 0;
    }
    WSASetLastError(saved_error);
}

std::uint32_t RouteInterfaceIndex(
    const sockaddr* address, const int address_length) noexcept {
    if (address == nullptr || address_length < static_cast<int>(sizeof(address->sa_family))) return 0;
    sockaddr_storage copy{};
    const int copy_length = (std::min)(address_length, static_cast<int>(sizeof(copy)));
    DWORD index{};
    __try {
        std::memcpy(&copy, address, static_cast<std::size_t>(copy_length));
        return GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&copy), &index) == NO_ERROR ? index : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

Context* BeginCapture(
    const AnomalyHookServiceV1*& service, AnomalyGenerationHandleV1& lease) noexcept {
    Context* const context = g_active.load(std::memory_order_acquire);
    service = g_hook_service.load(std::memory_order_acquire);
    if (context == nullptr || !context->capture_open.load(std::memory_order_acquire) ||
        service == nullptr || service->begin_callback == nullptr ||
        service->end_callback == nullptr) {
        return nullptr;
    }
    lease = {g_hook_id.load(std::memory_order_acquire),
        g_hook_generation.load(std::memory_order_acquire)};
    AnomalyGenerationHandleV1 callback_lease{};
    if (lease.id == 0 || service->begin_callback(service->user, lease, &callback_lease).code !=
            ANOMALY_STATUS_V1_OK) {
        return nullptr;
    }
    lease = callback_lease;
    if (!context->capture_open.load(std::memory_order_acquire)) {
        static_cast<void>(service->end_callback(service->user, lease));
        lease = {};
        return nullptr;
    }
    return context;
}

void EndCapture(
    const AnomalyHookServiceV1* service, const AnomalyGenerationHandleV1 lease) noexcept {
    if (service != nullptr && service->end_callback != nullptr && lease.id != 0) {
        static_cast<void>(service->end_callback(service->user, lease));
    }
}

TraceRecord NewRecord(
    const SOCKET socket, const void* return_address, const std::uint64_t qpc,
    const std::uint32_t requested_bytes, const std::uint32_t buffer_count,
    std::uint32_t flags) noexcept {
    TraceRecord record;
    record.qpc = qpc;
    record.socket = static_cast<std::uint64_t>(socket);
    record.return_address = reinterpret_cast<std::uintptr_t>(return_address);
    record.thread_id = GetCurrentThreadId();
    record.requested_bytes = requested_bytes;
    record.buffer_count = buffer_count;
    record.flags = flags;
    SocketMetadata(socket, record.local_endpoint, record.peer_endpoint, record.socket_type);
    CaptureStack(record);
    return record;
}

void FinishRecord(Context& context, TraceRecord& record) noexcept {
    context.Record(record);
}

int WSAAPI SendDetour(const SOCKET socket, const char* buffer, const int length, const int flags) {
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const SendFn original = g_send_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(socket, buffer, length, flags);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        TraceRecord record = NewRecord(socket, return_address, qpc, ClampByteCount(length), 1U, 0U);
        record.result = result;
        record.wsa_error = error;
        if (result != SOCKET_ERROR) {
            record.flags |= TraceSuccess;
            record.completed_bytes = ClampByteCount(result);
            FinishRecord(*context, record);
        } else {
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI SendToDetour(
    const SOCKET socket, const char* buffer, const int length, const int flags,
    const sockaddr* address, const int address_length) {
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const SendToFn original = g_sendto_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(socket, buffer, length, flags, address, address_length);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        TraceRecord record = NewRecord(socket, return_address, qpc, ClampByteCount(length), 1U, 0U);
        record.result = result;
        record.wsa_error = error;
        CaptureEndpoint(address, address_length, EndpointProvenance::SendToArgument,
            record.peer_endpoint);
        record.route_interface_index = RouteInterfaceIndex(address, address_length);
        if (result != SOCKET_ERROR) {
            record.flags |= TraceSuccess;
            record.completed_bytes = ClampByteCount(result);
            FinishRecord(*context, record);
        } else {
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI WsaSendDetour(
    const SOCKET socket, LPWSABUF buffers, const DWORD buffer_count, LPDWORD bytes_sent,
    const DWORD flags, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    bool buffers_truncated{};
    const std::uint32_t requested = SafeScatterGatherBytes(buffers, buffer_count, buffers_truncated);
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const WsaSendFn original = g_wsa_send_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(socket, buffers, buffer_count, bytes_sent, flags, overlapped, completion);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        std::uint32_t record_flags = TraceScatterGather;
        if (overlapped != nullptr || completion != nullptr) record_flags |= TraceOverlapped;
        if (buffers_truncated) record_flags |= TraceScatterGatherTruncated;
        TraceRecord record = NewRecord(socket, return_address, qpc, requested, buffer_count, record_flags);
        record.result = result;
        record.wsa_error = error;
        if (result == 0) {
            record.flags |= TraceSuccess;
            if (overlapped == nullptr && completion == nullptr && bytes_sent != nullptr) {
                record.completed_bytes = SafeDword(bytes_sent);
                FinishRecord(*context, record);
            } else {
                record.flags |= TracePending;
                FinishRecord(*context, record);
            }
        } else {
            if (error == WSA_IO_PENDING) record.flags |= TracePending;
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI WsaSendToDetour(
    const SOCKET socket, LPWSABUF buffers, const DWORD buffer_count, LPDWORD bytes_sent,
    const DWORD flags, const sockaddr* address, const int address_length, LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    bool buffers_truncated{};
    const std::uint32_t requested = SafeScatterGatherBytes(buffers, buffer_count, buffers_truncated);
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const WsaSendToFn original = g_wsa_sendto_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(
        socket, buffers, buffer_count, bytes_sent, flags, address, address_length, overlapped, completion);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        std::uint32_t record_flags = TraceScatterGather;
        if (overlapped != nullptr || completion != nullptr) record_flags |= TraceOverlapped;
        if (buffers_truncated) record_flags |= TraceScatterGatherTruncated;
        TraceRecord record = NewRecord(socket, return_address, qpc, requested, buffer_count, record_flags);
        record.result = result;
        record.wsa_error = error;
        CaptureEndpoint(address, address_length, EndpointProvenance::SendToArgument,
            record.peer_endpoint);
        record.route_interface_index = RouteInterfaceIndex(address, address_length);
        if (result == 0) {
            record.flags |= TraceSuccess;
            if (overlapped == nullptr && completion == nullptr && bytes_sent != nullptr) {
                record.completed_bytes = SafeDword(bytes_sent);
                FinishRecord(*context, record);
            } else {
                record.flags |= TracePending;
                FinishRecord(*context, record);
            }
        } else {
            if (error == WSA_IO_PENDING) record.flags |= TracePending;
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI RecvDetour(const SOCKET socket, char* buffer, const int length, const int flags) {
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const RecvFn original = g_recv_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(socket, buffer, length, flags);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        TraceRecord record = NewRecord(socket, return_address, qpc, ClampByteCount(length), 1U, 0U);
        record.result = result;
        record.wsa_error = error;
        if (result != SOCKET_ERROR) {
            record.flags |= TraceSuccess;
            record.completed_bytes = ClampByteCount(result);
            FinishRecord(*context, record);
        } else {
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI RecvFromDetour(
    const SOCKET socket, char* buffer, const int length, const int flags, sockaddr* address,
    int* address_length) {
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const RecvFromFn original = g_recvfrom_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(socket, buffer, length, flags, address, address_length);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        TraceRecord record = NewRecord(socket, return_address, qpc, ClampByteCount(length), 1U, 0U);
        record.result = result;
        record.wsa_error = error;
        int peer_length{};
        __try {
            if (address_length != nullptr) peer_length = *address_length;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            peer_length = 0;
        }
        CaptureEndpoint(address, peer_length, EndpointProvenance::RecvFromResult,
            record.peer_endpoint);
        record.route_interface_index = RouteInterfaceIndex(address, peer_length);
        if (result != SOCKET_ERROR) {
            record.flags |= TraceSuccess;
            record.completed_bytes = ClampByteCount(result);
            FinishRecord(*context, record);
        } else {
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI WsaRecvDetour(
    const SOCKET socket, LPWSABUF buffers, const DWORD buffer_count, LPDWORD bytes_received,
    LPDWORD flags, LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    bool buffers_truncated{};
    const std::uint32_t requested = SafeScatterGatherBytes(buffers, buffer_count, buffers_truncated);
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const WsaRecvFn original = g_wsa_recv_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(socket, buffers, buffer_count, bytes_received, flags, overlapped, completion);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        std::uint32_t record_flags = TraceScatterGather;
        if (overlapped != nullptr || completion != nullptr) record_flags |= TraceOverlapped;
        if (buffers_truncated) record_flags |= TraceScatterGatherTruncated;
        TraceRecord record = NewRecord(socket, return_address, qpc, requested, buffer_count, record_flags);
        record.result = result;
        record.wsa_error = error;
        if (result == 0) {
            record.flags |= TraceSuccess;
            if (overlapped == nullptr && completion == nullptr && bytes_received != nullptr) {
                record.completed_bytes = SafeDword(bytes_received);
                FinishRecord(*context, record);
            } else {
                record.flags |= TracePending;
                FinishRecord(*context, record);
            }
        } else {
            if (error == WSA_IO_PENDING) record.flags |= TracePending;
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

int WSAAPI WsaRecvFromDetour(
    const SOCKET socket, LPWSABUF buffers, const DWORD buffer_count, LPDWORD bytes_received,
    LPDWORD flags, sockaddr* address, LPINT address_length, LPWSAOVERLAPPED overlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    bool buffers_truncated{};
    const std::uint32_t requested = SafeScatterGatherBytes(buffers, buffer_count, buffers_truncated);
    const std::uint64_t qpc = QueryPerformanceCounterValue();
    const void* const return_address = _ReturnAddress();
    const AnomalyHookServiceV1* service{};
    AnomalyGenerationHandleV1 lease{};
    Context* const context = BeginCapture(service, lease);
    const WsaRecvFromFn original = g_wsa_recvfrom_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        if (context != nullptr) EndCapture(service, lease);
        WSASetLastError(WSAEFAULT);
        return SOCKET_ERROR;
    }
    const int result = original(
        socket, buffers, buffer_count, bytes_received, flags, address, address_length, overlapped, completion);
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (context != nullptr) {
        std::uint32_t record_flags = TraceScatterGather;
        if (overlapped != nullptr || completion != nullptr) record_flags |= TraceOverlapped;
        if (buffers_truncated) record_flags |= TraceScatterGatherTruncated;
        TraceRecord record = NewRecord(socket, return_address, qpc, requested, buffer_count, record_flags);
        record.result = result;
        record.wsa_error = error;
        int peer_length{};
        __try {
            if (address_length != nullptr) peer_length = *address_length;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            peer_length = 0;
        }
        CaptureEndpoint(address, peer_length, EndpointProvenance::RecvFromResult,
            record.peer_endpoint);
        record.route_interface_index = RouteInterfaceIndex(address, peer_length);
        if (result == 0) {
            record.flags |= TraceSuccess;
            if (overlapped == nullptr && completion == nullptr && bytes_received != nullptr) {
                record.completed_bytes = SafeDword(bytes_received);
                FinishRecord(*context, record);
            } else {
                record.flags |= TracePending;
                FinishRecord(*context, record);
            }
        } else {
            if (error == WSA_IO_PENDING) record.flags |= TracePending;
            FinishRecord(*context, record);
        }
        EndCapture(service, lease);
    }
    if (result == SOCKET_ERROR) WSASetLastError(error);
    return result;
}

void* DetourAddress() noexcept {
#if ANOMALY_WINSOCK_TRACE_OPERATION == 1
    return reinterpret_cast<void*>(&SendDetour);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 2
    return reinterpret_cast<void*>(&SendToDetour);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 3
    return reinterpret_cast<void*>(&WsaSendDetour);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 4
    return reinterpret_cast<void*>(&WsaSendToDetour);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 5
    return reinterpret_cast<void*>(&RecvDetour);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 6
    return reinterpret_cast<void*>(&RecvFromDetour);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 7
    return reinterpret_cast<void*>(&WsaRecvDetour);
#else
    return reinterpret_cast<void*>(&WsaRecvFromDetour);
#endif
}

void AppendHex(std::string& output, const std::uint64_t value, const std::size_t digits = 16U) {
    static constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < digits; ++index) {
        const unsigned shift = static_cast<unsigned>((digits - index - 1U) * 4U);
        output.push_back(hex[(value >> shift) & 0xfU]);
    }
}

void AppendCaptureId(std::string& output, const std::array<std::uint8_t, 16>& capture_id) {
    static constexpr char hex[] = "0123456789abcdef";
    for (const std::uint8_t value : capture_id) {
        output.push_back(hex[value >> 4U]);
        output.push_back(hex[value & 0x0fU]);
    }
}

std::string_view EndpointProvenanceName(const EndpointProvenance provenance) noexcept {
    switch (provenance) {
    case EndpointProvenance::GetSockName:
        return "getsockname";
    case EndpointProvenance::GetPeerName:
        return "getpeername";
    case EndpointProvenance::SendToArgument:
        return "sendto-argument";
    case EndpointProvenance::RecvFromResult:
        return "recvfrom-result";
    default:
        return "unavailable";
    }
}

std::string EndpointAddress(const EndpointMetadata& endpoint) {
    const std::size_t expected_size = endpoint.family == AF_INET
        ? sizeof(IN_ADDR)
        : endpoint.family == AF_INET6 ? sizeof(IN6_ADDR) : 0U;
    if (expected_size == 0 || endpoint.address_size != expected_size) return {};
    char text[INET6_ADDRSTRLEN]{};
    return InetNtopA(endpoint.family, endpoint.address.data(), text, sizeof(text)) == nullptr
        ? std::string{}
        : std::string(text);
}

void AppendEndpoint(
    std::string& output, const EndpointMetadata& endpoint, const bool include_address) {
    output += "{\"source\":\"";
    output += EndpointProvenanceName(endpoint.provenance);
    output += "\",\"family\":" + std::to_string(endpoint.family);
    output += ",\"address\":";
    const std::string address = include_address ? EndpointAddress(endpoint) : std::string{};
    if (address.empty()) {
        output += "null";
    } else {
        output += "\"" + address + "\"";
    }
    output += ",\"port\":" + std::to_string(endpoint.port) + "}";
}

std::string Serialize(
    const Context& context, const std::uint64_t persisted_records,
    const std::uint64_t persisted_max_sequence, const std::uint64_t persistence_successes,
    const bool final_persist_succeeded) {
    std::string output;
    output.reserve(1024U + context.history.size() * 560U);
    output += "{\"schemaVersion\":2,\"captureId\":\"";
    AppendCaptureId(output, context.capture_id);
    output += "\",\"pluginId\":\"" ANOMALY_WINSOCK_TRACE_PLUGIN_ID;
    output += "\",\"operation\":\"";
    output += kOperationName;
    output += "\",\"direction\":\"";
    output += (kOutbound ? "outbound" : "inbound");
    output += "\",\"clock\":{\"qpcFrequency\":" + std::to_string(context.qpc_frequency);
    output += ",\"startedQpc\":" + std::to_string(context.started_qpc);
    output += ",\"startedUtcUnixMilliseconds\":" +
        std::to_string(context.started_utc_unix_milliseconds);
    output += ",\"endedQpc\":" + std::to_string(context.ended_qpc);
    output += ",\"endedUtcUnixMilliseconds\":" +
        std::to_string(context.ended_utc_unix_milliseconds) + "}";
    output += ",\"state\":{\"finalizationRequested\":";
    output += context.finalization_requested ? "true" : "false";
    output += ",\"finalized\":";
    output += final_persist_succeeded ? "true" : "false";
    output += ",\"hookQuiesced\":";
    output += context.hook_quiesced ? "true" : "false";
    output += ",\"captureOpen\":";
    output += context.capture_open.load(std::memory_order_relaxed) ? "true" : "false";
    output += "}";
    output += ",\"integrity\":{\"recordsObserved\":" +
        std::to_string(context.next_sequence.load(std::memory_order_relaxed));
    output += ",\"recordsEnqueued\":" +
        std::to_string(context.records_enqueued.load(std::memory_order_relaxed));
    output += ",\"recordsDrained\":" + std::to_string(context.records_drained);
    output += ",\"recordsRecorded\":" + std::to_string(context.records_recorded);
    output += ",\"recordsPersisted\":" + std::to_string(persisted_records);
    output += ",\"persistedMaxSequence\":" + std::to_string(persisted_max_sequence);
    output += ",\"ringDropped\":" +
        std::to_string(context.ring_dropped.load(std::memory_order_relaxed));
    output += ",\"historyDropped\":" + std::to_string(context.history_dropped);
    output += ",\"aggregationDropped\":" + std::to_string(context.aggregation_dropped);
    output += ",\"persistenceAttempts\":" + std::to_string(context.persistence_attempts);
    output += ",\"persistenceSuccesses\":" + std::to_string(persistence_successes);
    output += ",\"persistenceFailures\":" + std::to_string(context.persistence_failures);
    output += ",\"schedulingFailures\":" +
        std::to_string(context.scheduling_failures.load(std::memory_order_relaxed)) + "}";
    output += ",\"callsites\":[";
    bool first = true;
    for (const auto& [address, stats] : context.callsites) {
        if (!first) output.push_back(',');
        first = false;
        output += "{\"returnAddress\":\"0x";
        AppendHex(output, address);
        output += "\",\"calls\":" + std::to_string(stats.calls);
        output += ",\"successes\":" + std::to_string(stats.successes);
        output += ",\"errors\":" + std::to_string(stats.errors);
        output += ",\"pending\":" + std::to_string(stats.pending);
        output += ",\"requestedBytes\":" + std::to_string(stats.requested_bytes);
        output += ",\"completedBytes\":" + std::to_string(stats.completed_bytes) + "}";
    }
    output += "],\"events\":[";
    first = true;
    for (const TraceRecord& record : context.history) {
        if (!first) output.push_back(',');
        first = false;
        output += "{\"sequence\":" + std::to_string(record.sequence);
        output += ",\"qpc\":" + std::to_string(record.qpc);
        output += ",\"socket\":\"0x";
        AppendHex(output, record.socket);
        output += "\",\"returnAddress\":\"0x";
        AppendHex(output, record.return_address);
        output += "\",\"threadId\":" + std::to_string(record.thread_id);
        output += ",\"requestedBytes\":" + std::to_string(record.requested_bytes);
        output += ",\"completedBytes\":" + std::to_string(record.completed_bytes);
        output += ",\"bufferCount\":" + std::to_string(record.buffer_count);
        output += ",\"flags\":" + std::to_string(record.flags);
        output += ",\"result\":" + std::to_string(record.result);
        output += ",\"wsaError\":" + std::to_string(record.wsa_error);
        output += ",\"socketType\":" + std::to_string(record.socket_type);
        output += ",\"routeInterfaceIndex\":" +
            std::to_string(record.route_interface_index);
        output += ",\"localEndpoint\":";
        AppendEndpoint(output, record.local_endpoint, true);
        output += ",\"peerEndpoint\":";
        AppendEndpoint(output, record.peer_endpoint, false);
        output += ",\"stack\":[";
        for (std::size_t index = 0; index < record.stack_depth; ++index) {
            if (index != 0) output.push_back(',');
            output += "\"0x";
            AppendHex(output, record.stack[index]);
            output.push_back('\"');
        }
        output += "]}";
    }
    output += "]}";
    return output;
}

bool Persist(Context& context, const bool final_snapshot, const bool hook_quiesced) {
    std::scoped_lock lock(context.persistence_mutex);
    context.Drain();
    if (final_snapshot) {
        const std::uint64_t qpc_before_clock = QueryPerformanceCounterValue();
        context.ended_utc_unix_milliseconds = QueryUnixTimeMilliseconds();
        const std::uint64_t qpc_after_clock = QueryPerformanceCounterValue();
        context.ended_qpc = qpc_before_clock + (qpc_after_clock - qpc_before_clock) / 2U;
        context.finalization_requested = true;
        context.hook_quiesced = hook_quiesced;
    }
    ++context.persistence_attempts;
    const std::uint64_t persisted_max_sequence =
        context.history.empty() ? 0U : context.history.back().sequence;
    const std::uint64_t persistence_successes = context.persistence_successes + 1U;
    const bool final_persist_succeeded = final_snapshot && hook_quiesced;
    const std::string document = Serialize(
        context, context.records_recorded, persisted_max_sequence, persistence_successes,
        final_persist_succeeded);
    const AnomalyByteSpanV1 bytes{
        reinterpret_cast<const std::uint8_t*>(document.data()), document.size()};
    const bool written = context.storage != nullptr && context.storage->write_atomic != nullptr &&
        context.storage->write_atomic(
            context.storage->user, anomaly::sdk::StringView("transport-trace.json"), bytes).code ==
            ANOMALY_STATUS_V1_OK;
    if (written) {
        context.persistence_successes = persistence_successes;
    } else {
        ++context.persistence_failures;
    }
    return written;
}

void ScheduleFlush(Context* context, const std::uint32_t delay_milliseconds);

void ANOMALY_CALL FlushTask(void* user, const AnomalyGenerationHandleV1 task) {
    auto* const context = static_cast<Context*>(user);
    if (context == nullptr) return;
    {
        std::scoped_lock lock(context->flush_mutex);
        if (context->flush_task.id == task.id) context->flush_task = {};
    }
    if (context->stop_started.load(std::memory_order_acquire)) return;
    static_cast<void>(Persist(*context, false, false));
    if (context->stop_started.load(std::memory_order_acquire)) return;
    ScheduleFlush(context, kFlushIntervalMilliseconds);
}

void ScheduleFlush(Context* context, const std::uint32_t delay_milliseconds) {
    if (context == nullptr || context->stop_started.load(std::memory_order_acquire) ||
        context->scheduler == nullptr || context->scheduler->schedule == nullptr) {
        return;
    }
    AnomalyGenerationHandleV1 task{};
    if (context->scheduler->schedule(
            context->scheduler->user, delay_milliseconds, FlushTask, context, &task).code !=
        ANOMALY_STATUS_V1_OK) {
        context->scheduling_failures.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    bool cancel{};
    {
        std::scoped_lock lock(context->flush_mutex);
        if (context->stop_started.load(std::memory_order_acquire)) {
            cancel = true;
        } else {
            context->flush_task = task;
        }
    }
    if (cancel && context->scheduler->cancel != nullptr) {
        static_cast<void>(context->scheduler->cancel(context->scheduler->user, task));
    }
}

void CancelFlush(Context& context) noexcept {
    AnomalyGenerationHandleV1 task{};
    {
        std::scoped_lock lock(context.flush_mutex);
        task = context.flush_task;
        context.flush_task = {};
    }
    if (task.id != 0 && context.scheduler != nullptr && context.scheduler->cancel != nullptr) {
        static_cast<void>(context.scheduler->cancel(context.scheduler->user, task));
    }
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* host, void** plugin_context) {
    if (host == nullptr || plugin_context == nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "host is invalid");
    }
    const auto* const storage = Query<AnomalyStorageServiceV1>(host, ANOMALY_STORAGE_SERVICE_V1_ID);
    const auto* const scheduler = Query<AnomalySchedulerServiceV1>(host, ANOMALY_SCHEDULER_SERVICE_V1_ID);
    const auto* const hook = Query<AnomalyHookServiceV1>(host, ANOMALY_HOOK_SERVICE_V1_ID);
    if (storage == nullptr || scheduler == nullptr || hook == nullptr) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "trace services are unavailable");
    }
    auto* const context = new (std::nothrow) Context();
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_FAILED, "trace context allocation failed");
    context->storage = storage;
    context->scheduler = scheduler;
    context->hook = hook;
    *plugin_context = context;
    return anomaly::sdk::Ok();
}

AnomalyStatusV1 ANOMALY_CALL Start(void* plugin_context) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr || context->hook == nullptr || context->hook->create == nullptr ||
        g_active.load(std::memory_order_acquire) != nullptr) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "trace context is invalid");
    }
    const HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
    const FARPROC target = winsock == nullptr ? nullptr : GetProcAddress(winsock, kOperationName.data());
    if (target == nullptr || BCryptGenRandom(
            nullptr, context->capture_id.data(), static_cast<ULONG>(context->capture_id.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return Status(ANOMALY_STATUS_V1_UNAVAILABLE, "Winsock trace target is unavailable");
    }
    AnomalyHookRequestV1 request{};
    request.struct_size = sizeof(request);
    request.kind = ANOMALY_HOOK_V1_FUNCTION;
    request.target = reinterpret_cast<std::uintptr_t>(target);
    request.detour = DetourAddress();
    request.label = anomaly::sdk::StringView("winsock-transport-trace");
    std::uintptr_t original{};
    if (context->hook->create(
            context->hook->user, &request, &original, &context->hook_handle).code !=
            ANOMALY_STATUS_V1_OK ||
        original == 0 || context->hook_handle.id == 0) {
        return Status(ANOMALY_STATUS_V1_FAILED, "Winsock trace hook creation failed");
    }
    context->qpc_frequency = QueryPerformanceFrequencyValue();
    const std::uint64_t qpc_before_clock = QueryPerformanceCounterValue();
    context->started_utc_unix_milliseconds = QueryUnixTimeMilliseconds();
    const std::uint64_t qpc_after_clock = QueryPerformanceCounterValue();
    context->started_qpc = qpc_before_clock + (qpc_after_clock - qpc_before_clock) / 2U;
    g_hook_service.store(context->hook, std::memory_order_release);
    g_hook_id.store(context->hook_handle.id, std::memory_order_release);
    g_hook_generation.store(context->hook_handle.generation, std::memory_order_release);
#if ANOMALY_WINSOCK_TRACE_OPERATION == 1
    g_send_original.store(reinterpret_cast<SendFn>(original), std::memory_order_release);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 2
    g_sendto_original.store(reinterpret_cast<SendToFn>(original), std::memory_order_release);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 3
    g_wsa_send_original.store(reinterpret_cast<WsaSendFn>(original), std::memory_order_release);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 4
    g_wsa_sendto_original.store(reinterpret_cast<WsaSendToFn>(original), std::memory_order_release);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 5
    g_recv_original.store(reinterpret_cast<RecvFn>(original), std::memory_order_release);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 6
    g_recvfrom_original.store(reinterpret_cast<RecvFromFn>(original), std::memory_order_release);
#elif ANOMALY_WINSOCK_TRACE_OPERATION == 7
    g_wsa_recv_original.store(reinterpret_cast<WsaRecvFn>(original), std::memory_order_release);
#else
    g_wsa_recvfrom_original.store(reinterpret_cast<WsaRecvFromFn>(original), std::memory_order_release);
#endif
    context->capture_open.store(true, std::memory_order_release);
    g_active.store(context, std::memory_order_release);
    ScheduleFlush(context, 0);
    return anomaly::sdk::Ok();
}

void ClearDetourState() noexcept {
    g_send_original.store(nullptr, std::memory_order_release);
    g_sendto_original.store(nullptr, std::memory_order_release);
    g_wsa_send_original.store(nullptr, std::memory_order_release);
    g_wsa_sendto_original.store(nullptr, std::memory_order_release);
    g_recv_original.store(nullptr, std::memory_order_release);
    g_recvfrom_original.store(nullptr, std::memory_order_release);
    g_wsa_recv_original.store(nullptr, std::memory_order_release);
    g_wsa_recvfrom_original.store(nullptr, std::memory_order_release);
    g_hook_id.store(0, std::memory_order_release);
    g_hook_generation.store(0, std::memory_order_release);
    g_hook_service.store(nullptr, std::memory_order_release);
}

bool ReleaseTraceHook(Context& context) noexcept {
    if (context.hook_handle.id == 0) return true;
    if (context.hook == nullptr || context.hook->release == nullptr) return false;
    const AnomalyStatusV1 status = context.hook->release(context.hook->user, context.hook_handle);
    if (status.code != ANOMALY_STATUS_V1_OK && status.code != ANOMALY_STATUS_V1_NOT_FOUND) {
        return false;
    }
    context.hook_handle = {};
    return true;
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* plugin_context, std::uint32_t) {
    auto* const context = static_cast<Context*>(plugin_context);
    if (context == nullptr) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "trace context is invalid");
    if (context->stop_started.exchange(true, std::memory_order_acq_rel)) return anomaly::sdk::Ok();
    context->capture_open.store(false, std::memory_order_release);
    Context* expected = context;
    static_cast<void>(g_active.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel));
    CancelFlush(*context);
    const bool hook_quiesced = ReleaseTraceHook(*context);
    if (hook_quiesced) ClearDetourState();
    const bool persisted = Persist(*context, true, hook_quiesced);
    if (!hook_quiesced) {
        return Status(ANOMALY_STATUS_V1_FAILED, "trace hook did not quiesce");
    }
    return persisted
        ? anomaly::sdk::Ok()
        : Status(ANOMALY_STATUS_V1_FAILED, "trace final persistence failed");
}

void ANOMALY_CALL Unload(void* plugin_context) {
    if (Stop(plugin_context, 0).code == ANOMALY_STATUS_V1_OK) {
        delete static_cast<Context*>(plugin_context);
    }
}

}  // namespace

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(
    AnomalyPluginDescriptorV1* descriptor) {
    if (descriptor == nullptr || descriptor->struct_size < sizeof(*descriptor)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT, "descriptor is invalid");
    }
    *descriptor = {
        sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        anomaly::sdk::StringView(ANOMALY_WINSOCK_TRACE_PLUGIN_ID),
        anomaly::sdk::StringView("Winsock transport trace"),
        anomaly::sdk::StringView("Anomaly"), anomaly::sdk::StringView("0.1.0"),
        Load, Start, Stop, Unload, nullptr, nullptr};
    return anomaly::sdk::Ok();
}
