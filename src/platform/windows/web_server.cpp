#include "anomaly/web_server.hpp"

#include "anomaly/reliable_storage.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace anomaly {
namespace {
constexpr std::size_t kMaximumHeader = 16384;
constexpr std::size_t kMaximumAssets = 64;
constexpr std::size_t kMaximumAssetBytes = 8 * 1024 * 1024;
constexpr auto kRequestTimeout = std::chrono::seconds(5);

AnomalyStatusV1 Status(std::uint32_t code) noexcept { return {code, 0, {}}; }

bool SafeRelative(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.front() == '\\' ||
        path.find_first_of(":\\\0", 0, 3) != std::string_view::npos) return false;
    std::size_t offset{};
    while (offset < path.size()) {
        const auto end = path.find('/', offset);
        const auto part = path.substr(offset, end == path.npos ? path.npos : end - offset);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == path.npos) break;
        offset = end + 1;
    }
    return path.back() != '/';
}

void CloseSocket(SOCKET value) noexcept {
    if (value != INVALID_SOCKET) {
        shutdown(value, SD_BOTH);
        closesocket(value);
    }
}

bool SendAll(SOCKET value, std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        const int count = send(value, bytes.data(), static_cast<int>(bytes.size()), 0);
        if (count <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

std::string Lower(std::string value) {
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return value;
}

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

struct Request final {
    std::string method;
    std::string path;
    std::string body;
    std::map<std::string, std::string, std::less<>> headers;
};

std::optional<Request> ReadRequest(SOCKET client) {
    std::string wire;
    std::array<char, 4096> chunk{};
    std::size_t header_end{};
    while ((header_end = wire.find("\r\n\r\n")) == wire.npos) {
        if (wire.size() > kMaximumHeader) return {};
        const int count = recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (count <= 0) return {};
        wire.append(chunk.data(), static_cast<std::size_t>(count));
    }
    if (header_end > kMaximumHeader) return {};
    const auto first_end = wire.find("\r\n");
    if (first_end == wire.npos) return {};
    const std::string_view first(wire.data(), first_end);
    const auto first_space = first.find(' ');
    const auto last_space = first.rfind(' ');
    if (first_space == first.npos || last_space == first_space ||
        first.substr(last_space + 1) != "HTTP/1.1") return {};
    Request result;
    result.method = first.substr(0, first_space);
    result.path = first.substr(first_space + 1, last_space - first_space - 1);
    if (result.path.empty() || result.path.front() != '/' ||
        result.path.find_first_of("\\%#\0", 0, 4) != result.path.npos) return {};
    if (const auto query = result.path.find('?'); query != result.path.npos) result.path.resize(query);
    std::size_t offset = first_end + 2;
    while (offset < header_end) {
        const auto end = wire.find("\r\n", offset);
        const std::string_view line(wire.data() + offset, end - offset);
        const auto colon = line.find(':');
        if (colon == line.npos) return {};
        auto key = Lower(std::string(line.substr(0, colon)));
        if (key.empty() || !result.headers.emplace(std::move(key), Trim(line.substr(colon + 1))).second) return {};
        offset = end + 2;
    }
    if (result.headers.contains("transfer-encoding")) return {};
    std::size_t body_size{};
    if (const auto length = result.headers.find("content-length"); length != result.headers.end()) {
        const auto& text = length->second;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), body_size);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    }
    if (body_size > ANOMALY_WEB_MAX_REQUEST_BYTES_V1) return {};
    const auto body_start = header_end + 4;
    while (wire.size() - body_start < body_size) {
        const int count = recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (count <= 0) return {};
        wire.append(chunk.data(), static_cast<std::size_t>(count));
    }
    result.body.assign(wire.data() + body_start, body_size);
    return result;
}

struct Reply final {
    std::uint32_t status{503};
    std::string content_type{"application/json; charset=utf-8"};
    std::string body{"{\"error\":\"unavailable\"}"};
};

void SendReply(SOCKET client, const Reply& reply, std::string_view extra = {}) {
    std::string headers = "HTTP/1.1 " + std::to_string(reply.status) + " Response\r\n";
    headers += "Content-Type: " + reply.content_type + "\r\nContent-Length: " + std::to_string(reply.body.size());
    headers += "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    headers += "Referrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'self'\r\n";
    headers += extra;
    headers += "\r\n";
    if (SendAll(client, headers)) static_cast<void>(SendAll(client, reply.body));
}

std::string Mime(const std::filesystem::path& path) {
    const auto extension = Lower(path.extension().string());
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

std::string SessionToken() {
    std::array<unsigned char, 24> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        result.push_back(alphabet[value >> 4]);
        result.push_back(alphabet[value & 15]);
    }
    return result;
}
}  // namespace

struct WebServer::Impl final : std::enable_shared_from_this<Impl> {
    struct Application final {
        std::shared_ptr<PluginScope> scope;
        std::uint64_t token{};
        AnomalyGenerationHandleV1 handle{};
        std::string route;
        std::string session_token;
        AnomalyWebCallbackV1 callback{};
        void* callback_user{};
        std::shared_ptr<void> callback_lifetime;
        std::mutex mutex;
        std::condition_variable drained;
        bool active{true};
        bool assets_ready{};
        std::size_t callbacks{};
        std::thread::id callback_thread;
        std::map<std::string, Reply, std::less<>> assets;
    };
    struct PendingReply final {
        std::mutex mutex;
        std::condition_variable ready;
        bool done{};
        bool cancelled{};
        Reply reply;
    };

    explicit Impl(SubmitWorker value) : submit(std::move(value)) {}
    SubmitWorker submit;
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Application>> applications;
    std::uint64_t next_id{1};
    std::atomic_bool running{};
    std::atomic<std::uint32_t> start_error{};
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<SOCKET> connection{INVALID_SOCKET};
    std::jthread network;
    std::uint16_t port{};
    bool winsock_started{};

    std::string Authority() const { return "127.0.0.1:" + std::to_string(port); }

    std::shared_ptr<Application> Find(const std::shared_ptr<PluginScope>& scope,
        AnomalyGenerationHandleV1 handle) {
        std::scoped_lock lock(mutex);
        const auto found = applications.find(handle.id);
        if (found == applications.end() || found->second->scope != scope ||
            found->second->handle.generation != handle.generation) return {};
        return found->second;
    }

    void Remove(const std::shared_ptr<Application>& app) noexcept {
        {
            std::unique_lock lock(app->mutex);
            app->active = false;
            app->drained.wait(lock, [&] { return app->callbacks == 0; });
        }
        std::scoped_lock lock(mutex);
        const auto found = applications.find(app->handle.id);
        if (found != applications.end() && found->second == app) applications.erase(found);
    }

    static void LoadAssets(const std::shared_ptr<Application>& app,
        const std::filesystem::path& package, const std::string& relative) noexcept {
        try {
            ReliableStorage storage(package.native());
            if (!storage.InitializationResult()) return;
            const auto directory = package / std::filesystem::path(relative);
            const auto root_attributes = GetFileAttributesW(directory.c_str());
            if (root_attributes == INVALID_FILE_ATTRIBUTES ||
                (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return;
            std::map<std::string, Reply, std::less<>> assets;
            std::size_t total{};
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                const auto attributes = GetFileAttributesW(entry.path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return;
                if (entry.is_directory()) continue;
                if (!entry.is_regular_file() || assets.size() >= kMaximumAssets) return;
                const auto within = entry.path().lexically_relative(package);
                const auto read = storage.Read(within.native(), kMaximumAssetBytes - total);
                if (!read) return;
                total += read.bytes.size();
                const auto local = entry.path().lexically_relative(directory).generic_string();
                Reply asset{200, Mime(entry.path()), {}};
                asset.body.assign(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
                assets.emplace('/' + local, std::move(asset));
            }
            if (!assets.contains("/index.html")) return;
            std::scoped_lock lock(app->mutex);
            if (app->active) {
                app->assets = std::move(assets);
                app->assets_ready = true;
            }
        } catch (...) {}
    }

    static void Call(const std::shared_ptr<Application>& app, const Request& request,
        const std::string& local_path, const std::shared_ptr<PendingReply>& pending) noexcept {
        {
            std::scoped_lock lock(pending->mutex);
            if (pending->cancelled) return;
        }
        auto lease = app->scope->AcquireCallback(app->handle.generation);
        if (!lease) {
            std::scoped_lock lock(pending->mutex);
            pending->done = true;
            pending->ready.notify_all();
            return;
        }
        {
            std::scoped_lock lock(app->mutex);
            if (!app->active) {
                std::scoped_lock result_lock(pending->mutex);
                pending->done = true;
                pending->ready.notify_all();
                return;
            }
            ++app->callbacks;
            app->callback_thread = std::this_thread::get_id();
        }
        Reply reply;
        try {
            std::vector<std::uint8_t> buffer(ANOMALY_WEB_MAX_RESPONSE_BYTES_V1);
            const AnomalyWebRequestV1 input{sizeof(input), 0,
                {request.method.data(), request.method.size()},
                {local_path.data(), local_path.size()}, {request.body.data(), request.body.size()}};
            AnomalyWebResponseV1 output{sizeof(output), 200,
                ANOMALY_WEB_CONTENT_TYPE_V1_JSON, 0, {buffer.data(), buffer.size()}, 0};
            const auto status = app->callback(app->callback_user, &input, &output);
            if (status.code == ANOMALY_STATUS_V1_OK && output.body.data == buffer.data() &&
                output.body_size <= buffer.size() && output.http_status >= 200 && output.http_status <= 599) {
                reply.status = output.http_status;
                reply.content_type = output.content_type == ANOMALY_WEB_CONTENT_TYPE_V1_TEXT
                    ? "text/plain; charset=utf-8" : "application/json; charset=utf-8";
                reply.body.assign(reinterpret_cast<const char*>(buffer.data()), output.body_size);
            }
        } catch (...) {
            reply = {500, "application/json; charset=utf-8", "{\"error\":\"callback_failed\"}"};
        }
        {
            std::scoped_lock lock(app->mutex);
            --app->callbacks;
            app->callback_thread = {};
            app->drained.notify_all();
        }
        {
            std::scoped_lock lock(pending->mutex);
            pending->reply = std::move(reply);
            pending->done = true;
            pending->ready.notify_all();
        }
    }

    Reply Handle(const Request& request) {
        const auto host = request.headers.find("host");
        if (host == request.headers.end() || host->second != Authority()) return {403, {}, {}};
        const auto origin = request.headers.find("origin");
        if (origin != request.headers.end() && origin->second != "http://" + Authority()) return {403, {}, {}};
        if (request.method != "GET" && request.method != "POST") return {405, {}, {}};
        std::shared_ptr<Application> app;
        {
            std::scoped_lock lock(mutex);
            for (const auto& [id, value] : applications) {
                if (request.path.starts_with(value->route)) { app = value; break; }
            }
        }
        if (!app) return {404, {}, {}};
        std::string path = request.path.substr(app->route.size() - 1);
        if (path.starts_with("/api/")) {
            const auto token = request.headers.find("x-anomaly-token");
            if (token == request.headers.end() || token->second != app->session_token) return {403, {}, {}};
            auto pending = std::make_shared<PendingReply>();
            if (!submit(app->scope->Owner(), app->handle.generation,
                    [app, request, path, pending] { Call(app, request, path, pending); })) return {};
            std::unique_lock lock(pending->mutex);
            const auto deadline = std::chrono::steady_clock::now() + kRequestTimeout;
            while (!pending->done && running.load() && std::chrono::steady_clock::now() < deadline) {
                pending->ready.wait_for(lock, std::chrono::milliseconds(100));
            }
            if (!pending->done) {
                pending->cancelled = true;
                return {504, "application/json; charset=utf-8", "{\"error\":\"request_timeout\"}"};
            }
            return std::move(pending->reply);
        }
        if (request.method != "GET") return {405, {}, {}};
        if (path == "/") path = "/index.html";
        std::scoped_lock lock(app->mutex);
        if (!app->active || !app->assets_ready) return {};
        const auto asset = app->assets.find(path);
        return asset == app->assets.end() ? Reply{404, {}, {}} : asset->second;
    }

    void Run(std::stop_token stop) noexcept {
        while (!stop.stop_requested()) {
            const SOCKET socket = listener.load();
            if (socket == INVALID_SOCKET) break;
            fd_set readers;
            FD_ZERO(&readers);
            FD_SET(socket, &readers);
            timeval wait{0, 100000};
            if (select(0, &readers, nullptr, nullptr, &wait) <= 0) continue;
            const SOCKET client = accept(socket, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            connection.store(client);
            // Stop can exchange connection before accept publishes it.
            if (stop.stop_requested() || !running.load()) {
                if (connection.exchange(INVALID_SOCKET) == client) CloseSocket(client);
                break;
            }
            const DWORD timeout = 2000;
            static_cast<void>(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)));
            static_cast<void>(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)));
            try {
                const auto request = ReadRequest(client);
                SendReply(client, request ? Handle(*request) : Reply{400, {}, {}});
            } catch (...) {
                try { SendReply(client, {500, {}, {}}); } catch (...) {}
            }
            if (connection.exchange(INVALID_SOCKET) == client) CloseSocket(client);
        }
    }
};

WebServer::WebServer(SubmitWorker submit)
    : impl_(std::make_shared<Impl>(std::move(submit))),
      provider_{sizeof(WebProviderV1), 1, this} {}
WebServer::~WebServer() { Stop(); }

std::uint32_t WebServer::LastError() const noexcept { return impl_->start_error.load(); }
const WebProviderV1* WebServer::Provider() const noexcept { return &provider_; }

bool WebServer::Start() noexcept {
    try {
        if (impl_->running.load()) return true;
        impl_->start_error.store(ERROR_SUCCESS);
        WSADATA data{};
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup != 0) { impl_->start_error.store(static_cast<std::uint32_t>(startup)); return false; }
        impl_->winsock_started = true;
        const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) {
            impl_->start_error.store(static_cast<std::uint32_t>(WSAGetLastError())); Stop(); return false;
        }
        impl_->listener.store(listener);
        const BOOL exclusive = TRUE;
        if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
            impl_->start_error.store(static_cast<std::uint32_t>(WSAGetLastError())); Stop(); return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener, SOMAXCONN) != 0) {
            impl_->start_error.store(static_cast<std::uint32_t>(WSAGetLastError())); Stop(); return false;
        }
        int size = sizeof(address);
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            impl_->start_error.store(static_cast<std::uint32_t>(WSAGetLastError())); Stop(); return false;
        }
        impl_->port = ntohs(address.sin_port);
        impl_->running.store(true);
        impl_->network = std::jthread([impl = impl_](std::stop_token stop) { impl->Run(stop); });
        return true;
    } catch (const std::system_error& error) {
        impl_->start_error.store(static_cast<std::uint32_t>(error.code().value())); Stop(); return false;
    } catch (...) { impl_->start_error.store(ERROR_GEN_FAILURE); Stop(); return false; }
}

void WebServer::Stop() noexcept {
    if (!impl_) return;
    impl_->running.store(false);
    impl_->network.request_stop();
    CloseSocket(impl_->listener.exchange(INVALID_SOCKET));
    CloseSocket(impl_->connection.exchange(INVALID_SOCKET));
    if (impl_->network.joinable()) impl_->network.join();
    std::vector<std::shared_ptr<Impl::Application>> apps;
    {
        std::scoped_lock lock(impl_->mutex);
        for (const auto& [id, app] : impl_->applications) apps.push_back(app);
    }
    for (const auto& app : apps) {
        impl_->Remove(app);
        static_cast<void>(app->scope->Release(app->token));
    }
    if (impl_->winsock_started) { WSACleanup(); impl_->winsock_started = false; }
}

AnomalyStatusV1 WebServer::Register(std::shared_ptr<PluginScope> scope,
    const std::filesystem::path& package, std::string_view assets,
    AnomalyWebCallbackV1 callback, void* callback_user,
    AnomalyGenerationHandleV1* application, std::shared_ptr<void> callback_lifetime) noexcept {
    if (!application || !scope || !callback || !SafeRelative(assets)) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *application = {};
    if (!impl_->running.load()) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    std::shared_ptr<Impl::Application> app;
    try {
        app = std::make_shared<Impl::Application>();
        app->scope = std::move(scope);
        app->callback = callback;
        app->callback_user = callback_user;
        app->callback_lifetime = std::move(callback_lifetime);
        app->route = "/plugins/" + app->scope->Owner() + '/';
        app->session_token = SessionToken();
        if (app->session_token.empty()) return Status(ANOMALY_STATUS_V1_FAILED);
        {
            std::scoped_lock lock(impl_->mutex);
            for (const auto& [id, previous] : impl_->applications) {
                if (previous->scope->Owner() == app->scope->Owner()) return Status(ANOMALY_STATUS_V1_CONFLICT);
            }
            app->handle = {impl_->next_id++, app->scope->Generation()};
            impl_->applications.emplace(app->handle.id, app);
        }
        const std::weak_ptr<Impl> weak = impl_;
        app->token = app->scope->Register(PluginResourceKind::Web, "web", [weak, app] {
            if (const auto impl = weak.lock()) impl->Remove(app);
        });
        if (app->token == 0) { impl_->Remove(app); return Status(ANOMALY_STATUS_V1_UNAVAILABLE); }
        if (!impl_->submit(app->scope->Owner(), app->handle.generation,
                [app, package, path = std::string(assets)] { Impl::LoadAssets(app, package, path); })) {
            static_cast<void>(app->scope->Release(app->token));
            return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
        }
        *application = app->handle;
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) {
        if (app) {
            if (app->token != 0) static_cast<void>(app->scope->Release(app->token));
            else impl_->Remove(app);
        }
        return Status(ANOMALY_STATUS_V1_FAILED);
    }
}

AnomalyStatusV1 WebServer::Unregister(const std::shared_ptr<PluginScope>& scope,
    AnomalyGenerationHandleV1 application) noexcept {
    try {
        const auto app = impl_->Find(scope, application);
        if (!app) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        {
            std::scoped_lock lock(app->mutex);
            if (app->callbacks != 0 && app->callback_thread == std::this_thread::get_id()) return Status(ANOMALY_STATUS_V1_CONFLICT);
        }
        static_cast<void>(scope->Release(app->token));
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) { return Status(ANOMALY_STATUS_V1_FAILED); }
}

AnomalyStatusV1 WebServer::Url(const std::shared_ptr<PluginScope>& scope,
    AnomalyGenerationHandleV1 application, char* destination, std::size_t* inout_size) noexcept {
    if (!inout_size) return Status(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    if (!impl_->running.load()) return Status(ANOMALY_STATUS_V1_UNAVAILABLE);
    try {
        const auto app = impl_->Find(scope, application);
        if (!app) return Status(ANOMALY_STATUS_V1_NOT_FOUND);
        const auto url = "http://" + impl_->Authority() + app->route + "#token=" + app->session_token;
        const auto required = url.size() + 1;
        const auto capacity = *inout_size;
        *inout_size = required;
        if (!destination) return Status(ANOMALY_STATUS_V1_OK);
        if (capacity < required) return Status(ANOMALY_STATUS_V1_BUFFER_TOO_SMALL);
        std::memcpy(destination, url.c_str(), required);
        return Status(ANOMALY_STATUS_V1_OK);
    } catch (...) { return Status(ANOMALY_STATUS_V1_FAILED); }
}

}  // namespace anomaly
