#include "anomaly/websocket_server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr std::string_view kWebSocketMagic =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaximumHandshakeBytes = 16U * 1024U;
constexpr std::size_t kMaximumTextBytes = 1024U * 1024U;
constexpr std::size_t kMaximumQueuedMessages = 16U;
constexpr std::size_t kMaximumClients = 8U;
constexpr std::size_t kMaximumQueuedFramesPerClient = kMaximumQueuedMessages;
constexpr std::size_t kMaximumQueuedBytesPerClient =
    kMaximumQueuedFramesPerClient * (kMaximumTextBytes + 10U);
constexpr auto kFinalDrainTimeout = std::chrono::milliseconds(1000);
constexpr std::uint64_t kPortRequestPortMask = 0xFFFFU;
constexpr unsigned kPortRequestGenerationShift = 16U;

AnomalyStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

bool CaseInsensitiveEquals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index{}; index != left.size(); ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if ((a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) !=
            (b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b)) {
            return false;
        }
    }
    return true;
}

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::optional<std::string> HeaderValue(
    const std::string_view request, const std::string_view wanted) {
    std::size_t cursor{};
    while (cursor < request.size()) {
        const std::size_t line_end = request.find("\r\n", cursor);
        const std::string_view line = request.substr(
            cursor, line_end == std::string_view::npos ? std::string_view::npos : line_end - cursor);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos &&
            CaseInsensitiveEquals(Trim(line.substr(0, colon)), wanted)) {
            const std::string_view value = Trim(line.substr(colon + 1U));
            if (!value.empty()) return std::string(value);
        }
        if (line_end == std::string_view::npos) break;
        cursor = line_end + 2U;
    }
    return std::nullopt;
}

std::string Base64Encode(const std::span<const std::uint8_t> bytes) {
    constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t index{}; index < bytes.size(); index += 3U) {
        const std::uint32_t first = bytes[index];
        const bool has_second = index + 1U < bytes.size();
        const bool has_third = index + 2U < bytes.size();
        const std::uint32_t second = has_second ? bytes[index + 1U] : 0U;
        const std::uint32_t third = has_third ? bytes[index + 2U] : 0U;
        const std::uint32_t block = (first << 16U) | (second << 8U) | third;
        result.push_back(kAlphabet[(block >> 18U) & 0x3FU]);
        result.push_back(kAlphabet[(block >> 12U) & 0x3FU]);
        result.push_back(has_second ? kAlphabet[(block >> 6U) & 0x3FU] : '=');
        result.push_back(has_third ? kAlphabet[block & 0x3FU] : '=');
    }
    return result;
}

std::optional<std::string> WebSocketAcceptValue(const std::string_view key) {
    const std::string source = std::string(key) + std::string(kWebSocketMagic);
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) {
        return std::nullopt;
    }
    DWORD object_size{};
    DWORD returned{};
    const NTSTATUS object_status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
        &returned, 0);
    if (object_status < 0 || returned != sizeof(object_size)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::vector<std::uint8_t> object(object_size);
    BCRYPT_HASH_HANDLE hash{};
    const NTSTATUS create_status = BCryptCreateHash(
        algorithm, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0);
    if (create_status < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::array<std::uint8_t, 20> digest{};
    const NTSTATUS hash_status = BCryptHashData(
        hash, reinterpret_cast<PUCHAR>(const_cast<char*>(source.data())),
        static_cast<ULONG>(source.size()), 0);
    const NTSTATUS finish_status = hash_status < 0
        ? hash_status
        : BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finish_status < 0) return std::nullopt;
    return Base64Encode(digest);
}

bool SendAll(const SOCKET socket, const std::string_view bytes) noexcept {
    std::size_t sent{};
    while (sent < bytes.size()) {
        const std::size_t remaining = bytes.size() - sent;
        const int chunk = static_cast<int>((std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int result = send(socket, bytes.data() + sent, chunk, 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool SetNonBlocking(const SOCKET socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

std::string BuildTextFrame(const std::string_view message) {
    std::string frame;
    frame.reserve(message.size() + 10U);
    frame.push_back(static_cast<char>(0x81U));
    if (message.size() <= 125U) {
        frame.push_back(static_cast<char>(message.size()));
    } else if (message.size() <= 0xFFFFU) {
        frame.push_back(static_cast<char>(126U));
        frame.push_back(static_cast<char>((message.size() >> 8U) & 0xFFU));
        frame.push_back(static_cast<char>(message.size() & 0xFFU));
    } else {
        frame.push_back(static_cast<char>(127U));
        const std::uint64_t size = message.size();
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((size >> shift) & 0xFFU));
        }
    }
    frame.append(message);
    return frame;
}

void CloseSocket(const SOCKET socket) noexcept {
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

[[nodiscard]] SOCKET CreateListener(const std::uint16_t port) noexcept {
    SOCKET listening = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listening == INVALID_SOCKET) return INVALID_SOCKET;

    const BOOL reuse = TRUE;
    static_cast<void>(setsockopt(
        listening, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listening, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listening, SOMAXCONN) == SOCKET_ERROR || !SetNonBlocking(listening)) {
        CloseSocket(listening);
        return INVALID_SOCKET;
    }
    return listening;
}

[[nodiscard]] constexpr std::uint64_t PackPortRequest(
    const std::uint64_t generation, const std::uint16_t port) noexcept {
    return (generation << kPortRequestGenerationShift) | port;
}

[[nodiscard]] constexpr std::uint64_t PortRequestGeneration(
    const std::uint64_t request) noexcept {
    return request >> kPortRequestGenerationShift;
}

[[nodiscard]] constexpr std::uint16_t PortRequestPort(const std::uint64_t request) noexcept {
    return static_cast<std::uint16_t>(request & kPortRequestPortMask);
}

}  // namespace

struct WebSocketServer::Impl final {
    explicit Impl(const std::uint16_t requested_port) : port(requested_port) {}

    struct Client final {
        SOCKET socket{INVALID_SOCKET};
        std::deque<std::string> frames;
        std::size_t front_offset{};
        std::size_t queued_bytes{};
    };

    std::atomic<std::uint16_t> port{};
    std::atomic<std::uint64_t> generation{};
    std::atomic<std::uint64_t> pending_port_request{};
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<SOCKET> handshaking_client{INVALID_SOCKET};
    std::atomic_bool running{};
    std::atomic_bool winsock_ready{};
    std::atomic<std::uint32_t> connected_clients{};
    std::atomic<std::uint64_t> published_messages{};
    std::atomic<std::uint64_t> dropped_messages{};
    mutable std::mutex lifecycle_mutex;
    std::thread worker;
    std::mutex queue_mutex;
    std::deque<std::string> outbound;
    AnomalyWebSocketServiceV1 service{};

    void RebindPendingListener() noexcept {
        const std::uint64_t request = pending_port_request.exchange(0U, std::memory_order_acq_rel);
        const std::uint16_t requested = PortRequestPort(request);
        const std::uint64_t request_generation = PortRequestGeneration(request);
        if (requested == 0U || request_generation != generation.load(std::memory_order_acquire) ||
            requested == port.load(std::memory_order_acquire)) {
            return;
        }

        std::scoped_lock lock(lifecycle_mutex);
        if (!running.load(std::memory_order_acquire) ||
            request_generation != generation.load(std::memory_order_acquire) ||
            requested == port.load(std::memory_order_acquire)) {
            return;
        }
        const SOCKET replacement = CreateListener(requested);
        if (replacement == INVALID_SOCKET) return;

        const SOCKET previous = listener.exchange(replacement, std::memory_order_acq_rel);
        port.store(requested, std::memory_order_release);
        CloseSocket(previous);
    }

    [[nodiscard]] bool Handshake(const SOCKET client) noexcept {
        try {
            DWORD receive_timeout = 1000U;
            u_long blocking = 0;
            if (ioctlsocket(client, FIONBIO, &blocking) != 0) return false;
            static_cast<void>(setsockopt(
                client, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)));
            static_cast<void>(setsockopt(
                client, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)));
            std::string request;
            request.reserve(1024U);
            std::array<char, 1024> buffer{};
            while (request.size() < kMaximumHandshakeBytes) {
                const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (received <= 0) return false;
                request.append(buffer.data(), static_cast<std::size_t>(received));
                const std::size_t end = request.find("\r\n\r\n");
                if (end == std::string::npos) continue;
                const std::string_view header(request.data(), end + 2U);
                if (!header.starts_with("GET ")) return false;
                const auto key = HeaderValue(header, "Sec-WebSocket-Key");
                if (!key) return false;
                const auto accept = WebSocketAcceptValue(*key);
                if (!accept) return false;
                const std::string response =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + *accept + "\r\n\r\n";
                if (!SendAll(client, response)) return false;
                return SetNonBlocking(client);
            }
            return false;
        } catch (...) {
            return false;
        }
    }

    void AcceptClients(std::vector<Client>& clients) noexcept {
        const SOCKET listening = listener.load(std::memory_order_acquire);
        if (listening == INVALID_SOCKET) return;
        for (;;) {
            SOCKET client = accept(listening, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                return;
            }
            if (clients.size() >= kMaximumClients || !running.load(std::memory_order_acquire)) {
                CloseSocket(client);
                continue;
            }
            handshaking_client.store(client, std::memory_order_release);
            const bool accepted = Handshake(client);
            const SOCKET completed = handshaking_client.exchange(
                INVALID_SOCKET, std::memory_order_acq_rel);
            if (completed != client) continue;
            if (!accepted || !running.load(std::memory_order_acquire)) {
                CloseSocket(client);
                continue;
            }
            try {
                clients.push_back({client});
            } catch (...) {
                CloseSocket(client);
                return;
            }
            connected_clients.store(
                static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
        }
    }

    [[nodiscard]] bool DrainClient(const SOCKET client) noexcept {
        std::array<char, 2048> buffer{};
        for (;;) {
            const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received > 0) continue;
            if (received == 0) return false;
            const int error = WSAGetLastError();
            return error == WSAEWOULDBLOCK;
        }
    }

    [[nodiscard]] bool QueueFrame(Client& client, const std::string& frame) noexcept {
        if (client.frames.size() >= kMaximumQueuedFramesPerClient ||
            frame.size() > kMaximumQueuedBytesPerClient - client.queued_bytes) {
            return false;
        }
        try {
            client.frames.push_back(frame);
            client.queued_bytes += frame.size();
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool FlushClient(Client& client) noexcept {
        while (!client.frames.empty()) {
            std::string& frame = client.frames.front();
            const std::size_t remaining = frame.size() - client.front_offset;
            const int chunk = static_cast<int>((std::min)(
                remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
            const int sent = send(client.socket, frame.data() + client.front_offset, chunk, 0);
            if (sent > 0) {
                const std::size_t sent_bytes = static_cast<std::size_t>(sent);
                client.front_offset += sent_bytes;
                client.queued_bytes -= sent_bytes;
                if (client.front_offset == frame.size()) {
                    client.frames.pop_front();
                    client.front_offset = 0;
                }
                continue;
            }
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return true;
            return false;
        }
        return true;
    }

    void FlushWritableClients(std::vector<Client>& clients, fd_set& writable) noexcept {
        for (auto iterator = clients.begin(); iterator != clients.end();) {
            if (FD_ISSET(iterator->socket, &writable) && !FlushClient(*iterator)) {
                CloseSocket(iterator->socket);
                iterator = clients.erase(iterator);
            } else {
                ++iterator;
            }
        }
        connected_clients.store(
            static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
    }

    [[nodiscard]] static bool HasPendingFrames(const std::vector<Client>& clients) noexcept {
        return std::any_of(clients.begin(), clients.end(), [](const Client& client) {
            return !client.frames.empty();
        });
    }

    void DrainPendingClients(std::vector<Client>& clients) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kFinalDrainTimeout;
        while (!clients.empty() && HasPendingFrames(clients)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return;
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000000LL);
            timeout.tv_usec = static_cast<long>(remaining.count() % 1000000LL);
            fd_set writable{};
            for (const Client& client : clients) {
                if (!client.frames.empty()) FD_SET(client.socket, &writable);
            }
            const int ready = select(0, nullptr, &writable, nullptr, &timeout);
            if (ready <= 0) return;
            FlushWritableClients(clients, writable);
        }
    }

    void DeliverQueued(std::vector<Client>& clients) noexcept {
        std::deque<std::string> pending;
        {
            std::scoped_lock lock(queue_mutex);
            pending.swap(outbound);
        }
        if (pending.empty() || clients.empty()) return;
        for (const std::string& message : pending) {
            std::string frame;
            try {
                frame = BuildTextFrame(message);
            } catch (...) {
                dropped_messages.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (Client& client : clients) {
                if (!QueueFrame(client, frame)) {
                    dropped_messages.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        connected_clients.store(
            static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
    }

    void Run() noexcept {
        std::vector<Client> clients;
        while (running.load(std::memory_order_acquire)) {
            RebindPendingListener();
            fd_set read_set{};
            fd_set write_set{};
            const SOCKET listening = listener.load(std::memory_order_acquire);
            if (listening != INVALID_SOCKET) FD_SET(listening, &read_set);
            for (const Client& client : clients) {
                FD_SET(client.socket, &read_set);
                if (!client.frames.empty()) FD_SET(client.socket, &write_set);
            }
            timeval timeout{};
            timeout.tv_usec = 50000;
            const int ready = select(0, &read_set, &write_set, nullptr, &timeout);
            if (ready > 0) {
                if (listening != INVALID_SOCKET && FD_ISSET(listening, &read_set)) {
                    AcceptClients(clients);
                }
                for (auto iterator = clients.begin(); iterator != clients.end();) {
                    if (FD_ISSET(iterator->socket, &read_set) && !DrainClient(iterator->socket)) {
                        CloseSocket(iterator->socket);
                        iterator = clients.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
                connected_clients.store(
                    static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
                FlushWritableClients(clients, write_set);
            }
            DeliverQueued(clients);
        }
        DeliverQueued(clients);
        DrainPendingClients(clients);
        for (const Client& client : clients) CloseSocket(client.socket);
        connected_clients.store(0, std::memory_order_release);
    }
};

namespace {

AnomalyStatusV1 ANOMALY_CALL PublishTextV1(
    void* user, const AnomalyStringViewV1 message) noexcept {
    auto* const server = static_cast<WebSocketServer*>(user);
    if (server == nullptr || (message.data == nullptr && message.size != 0U)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    if (message.size > kMaximumTextBytes) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (!server->PublishText({message.data, message.size})) {
        const AnomalyWebSocketServerInfoV1 info = server->Snapshot();
        return Status(info.port == 0U ? ANOMALY_STATUS_V1_UNAVAILABLE
                                      : ANOMALY_STATUS_V1_CONFLICT);
    }
    return Status(ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL ServerInfoV1(
    void* user, AnomalyWebSocketServerInfoV1* info) noexcept {
    auto* const server = static_cast<WebSocketServer*>(user);
    if (server == nullptr || info == nullptr || info->struct_size < sizeof(*info)) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    *info = server->Snapshot();
    return Status(info->port == 0U ? ANOMALY_STATUS_V1_UNAVAILABLE
                                   : ANOMALY_STATUS_V1_OK);
}

AnomalyStatusV1 ANOMALY_CALL SetPortV1(void* user, const std::uint16_t port) noexcept {
    auto* const server = static_cast<WebSocketServer*>(user);
    if (server == nullptr || port == 0U) {
        return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    }
    return Status(server->RequestPortChange(port)
        ? ANOMALY_STATUS_V1_OK
        : ANOMALY_STATUS_V1_UNAVAILABLE);
}

}  // namespace

WebSocketServer::WebSocketServer(const std::uint16_t port)
    : impl_(std::make_unique<Impl>(port)) {
    impl_->service = {
        sizeof(AnomalyWebSocketServiceV1), ANOMALY_WEBSOCKET_SERVICE_V1_VERSION,
        this, PublishTextV1, ServerInfoV1, SetPortV1};
}

WebSocketServer::~WebSocketServer() {
    Stop();
}

bool WebSocketServer::Start() noexcept {
    if (impl_ == nullptr) return false;
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (impl_->running.load(std::memory_order_acquire)) return true;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    impl_->winsock_ready.store(true, std::memory_order_release);
    impl_->generation.fetch_add(1U, std::memory_order_acq_rel);
    SOCKET listening = CreateListener(impl_->port.load(std::memory_order_acquire));
    if (listening == INVALID_SOCKET) {
        WSACleanup();
        impl_->winsock_ready.store(false, std::memory_order_release);
        return false;
    }
    impl_->listener.store(listening, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        CloseSocket(impl_->listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel));
        WSACleanup();
        impl_->winsock_ready.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void WebSocketServer::Stop() noexcept {
    if (impl_ == nullptr) return;
    std::thread worker;
    bool cleanup{};
    {
        std::scoped_lock lock(impl_->lifecycle_mutex);
        if (!impl_->running.exchange(false, std::memory_order_acq_rel)) return;
        CloseSocket(impl_->listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel));
        CloseSocket(impl_->handshaking_client.exchange(INVALID_SOCKET, std::memory_order_acq_rel));
        worker = std::move(impl_->worker);
        cleanup = impl_->winsock_ready.exchange(false, std::memory_order_acq_rel);
    }
    if (worker.joinable()) worker.join();
    {
        std::scoped_lock lock(impl_->queue_mutex);
        impl_->outbound.clear();
    }
    if (cleanup) WSACleanup();
}

bool WebSocketServer::PublishText(const std::string_view message) noexcept {
    if (impl_ == nullptr || message.size() > kMaximumTextBytes ||
        !impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    try {
        std::scoped_lock lock(impl_->queue_mutex);
        if (!impl_->running.load(std::memory_order_acquire)) return false;
        if (impl_->outbound.size() >= kMaximumQueuedMessages) {
            impl_->dropped_messages.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        impl_->outbound.emplace_back(message);
        impl_->published_messages.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (...) {
        impl_->dropped_messages.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool WebSocketServer::RequestPortChange(const std::uint16_t port) noexcept {
    if (impl_ == nullptr || port == 0U) return false;
    const std::uint64_t request_generation = impl_->generation.load(std::memory_order_acquire);
    if (request_generation == 0U || !impl_->running.load(std::memory_order_acquire) ||
        impl_->generation.load(std::memory_order_acquire) != request_generation) {
        return false;
    }
    const std::uint64_t request = PackPortRequest(request_generation, port);
    std::uint64_t pending = impl_->pending_port_request.load(std::memory_order_acquire);
    do {
        if (PortRequestGeneration(pending) > request_generation) return false;
    } while (!impl_->pending_port_request.compare_exchange_weak(
        pending, request, std::memory_order_release, std::memory_order_acquire));

    if (impl_->running.load(std::memory_order_acquire) &&
        impl_->generation.load(std::memory_order_acquire) == request_generation) {
        return true;
    }
    std::uint64_t expected = request;
    static_cast<void>(impl_->pending_port_request.compare_exchange_strong(
        expected, 0U, std::memory_order_acq_rel, std::memory_order_acquire));
    return false;
}

AnomalyWebSocketServerInfoV1 WebSocketServer::Snapshot() const noexcept {
    if (impl_ == nullptr) return {sizeof(AnomalyWebSocketServerInfoV1)};
    const bool running = impl_->running.load(std::memory_order_acquire);
    return {
        sizeof(AnomalyWebSocketServerInfoV1),
        running ? impl_->port.load(std::memory_order_acquire) : 0U,
        0,
        impl_->connected_clients.load(std::memory_order_acquire),
        impl_->published_messages.load(std::memory_order_acquire),
        impl_->dropped_messages.load(std::memory_order_acquire)};
}

const AnomalyWebSocketServiceV1* WebSocketServer::Service() const noexcept {
    return impl_ == nullptr ? nullptr : &impl_->service;
}

}  // namespace anomaly
