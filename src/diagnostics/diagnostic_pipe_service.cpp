#include "anomaly/diagnostic_pipe_service.hpp"

#include "analyzer.hpp"

#include <AccCtrl.h>
#include <Aclapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace anomaly {
namespace {

constexpr DWORD kInputBufferSize = 64U * 1024U;
constexpr DWORD kOutputBufferSize = 1024U * 1024U;
constexpr std::size_t kMaximumRequestSize = 64U * 1024U;
constexpr std::size_t kMaximumRequestIdSize = 256U;
constexpr char kDiagnosticProtocol[] = "anomaly.diagnostics";
constexpr std::uint64_t kDiagnosticProtocolVersion = 1U;

using Json = nlohmann::json;

class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}

    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) Reset(std::exchange(other.handle_, nullptr));
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE handle = nullptr) noexcept {
        if (*this) CloseHandle(handle_);
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

class PipeSecurity final {
public:
    PipeSecurity() = default;

    ~PipeSecurity() {
        if (acl_ != nullptr) LocalFree(acl_);
    }

    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;

    [[nodiscard]] DWORD Prepare() {
        UniqueHandle token;
        HANDLE raw_token{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token) == FALSE) {
            return GetLastError();
        }
        token.Reset(raw_token);

        DWORD required{};
        if (GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &required) != FALSE) {
            return ERROR_INVALID_DATA;
        }
        const DWORD size_error = GetLastError();
        if (size_error != ERROR_INSUFFICIENT_BUFFER || required == 0) return size_error;

        const std::size_t words =
            (static_cast<std::size_t>(required) + sizeof(std::uintptr_t) - 1U) /
            sizeof(std::uintptr_t);
        token_user_.resize(words);
        if (GetTokenInformation(
                token.Get(), TokenUser, token_user_.data(), required, &required) == FALSE) {
            return GetLastError();
        }

        PSID logon_sid{};
        DWORD logon_required{};
        if (GetTokenInformation(
                token.Get(), TokenLogonSid, nullptr, 0, &logon_required) == FALSE &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
            logon_required >= sizeof(TOKEN_GROUPS)) {
            const std::size_t logon_words =
                (static_cast<std::size_t>(logon_required) + sizeof(std::uintptr_t) - 1U) /
                sizeof(std::uintptr_t);
            token_logon_sid_.resize(logon_words);
            if (GetTokenInformation(
                    token.Get(), TokenLogonSid, token_logon_sid_.data(),
                    logon_required, &logon_required) != FALSE) {
                const auto* groups =
                    reinterpret_cast<const TOKEN_GROUPS*>(token_logon_sid_.data());
                if (groups->GroupCount != 0 && IsValidSid(groups->Groups[0].Sid) != FALSE) {
                    logon_sid = groups->Groups[0].Sid;
                }
            }
        }

        const auto* user = reinterpret_cast<const TOKEN_USER*>(token_user_.data());
        std::array<EXPLICIT_ACCESSW, 2> access{};
        const auto initialize_access = [](EXPLICIT_ACCESSW& entry, PSID sid, TRUSTEE_TYPE type) {
            entry.grfAccessPermissions = GENERIC_ALL;
            entry.grfAccessMode = SET_ACCESS;
            entry.grfInheritance = NO_INHERITANCE;
            entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
            entry.Trustee.TrusteeType = type;
            entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
        };
        initialize_access(access[0], user->User.Sid, TRUSTEE_IS_USER);
        ULONG access_count = 1;
        if (logon_sid != nullptr) {
            initialize_access(access[1], logon_sid, TRUSTEE_IS_GROUP);
            access_count = 2;
        }

        const DWORD acl_error = SetEntriesInAclW(access_count, access.data(), nullptr, &acl_);
        if (acl_error != ERROR_SUCCESS) return acl_error;
        if (InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) == FALSE) {
            return GetLastError();
        }
        if (SetSecurityDescriptorDacl(&descriptor_, TRUE, acl_, FALSE) == FALSE) {
            return GetLastError();
        }

        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
        return ERROR_SUCCESS;
    }

    [[nodiscard]] SECURITY_ATTRIBUTES* Attributes() noexcept { return &attributes_; }

private:
    std::vector<std::uintptr_t> token_user_;
    std::vector<std::uintptr_t> token_logon_sid_;
    ACL* acl_{};
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

enum class IoStatus {
    Completed,
    Stopped,
    Failed,
};

struct IoResult final {
    IoStatus status{IoStatus::Failed};
    DWORD transferred{};
    DWORD error{ERROR_GEN_FAILURE};
};

class PipeIo final {
public:
    PipeIo(HANDLE pipe, HANDLE stop_event, std::stop_token stop_token) noexcept
        : pipe_(pipe),
          stop_event_(stop_event),
          stop_token_(stop_token),
          operation_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          event_error_(operation_event_ ? ERROR_SUCCESS : GetLastError()) {}

    [[nodiscard]] DWORD InitializationError() const noexcept { return event_error_; }

    [[nodiscard]] IoResult Connect() const noexcept {
        if (Stopping()) return Stopped();
        if (!operation_event_) return Failed(event_error_);

        OVERLAPPED operation{};
        operation.hEvent = operation_event_.Get();
        ResetEvent(operation_event_.Get());
        if (ConnectNamedPipe(pipe_, &operation) != FALSE) return Completed(0);

        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) return Completed(0);
        if (error == ERROR_OPERATION_ABORTED && Stopping()) return Stopped();
        if (error != ERROR_IO_PENDING) return Failed(error);
        return Await(operation);
    }

    [[nodiscard]] IoResult Read(void* buffer, DWORD size) const noexcept {
        return Transfer(
            [buffer, size](HANDLE pipe, DWORD* bytes, OVERLAPPED* operation) {
                return ReadFile(pipe, buffer, size, bytes, operation);
            });
    }

    [[nodiscard]] IoResult Write(const void* buffer, DWORD size) const noexcept {
        return Transfer(
            [buffer, size](HANDLE pipe, DWORD* bytes, OVERLAPPED* operation) {
                return WriteFile(pipe, buffer, size, bytes, operation);
            });
    }

    [[nodiscard]] IoResult Flush() const noexcept {
        if (Stopping()) return Stopped();
        if (FlushFileBuffers(pipe_) != FALSE) return Completed(0);
        const DWORD error = GetLastError();
        return error == ERROR_OPERATION_ABORTED && Stopping()
            ? Stopped()
            : Failed(error);
    }

private:
    template <typename StartOperation>
    [[nodiscard]] IoResult Transfer(StartOperation&& start) const noexcept {
        if (Stopping()) return Stopped();
        if (!operation_event_) return Failed(event_error_);

        OVERLAPPED operation{};
        operation.hEvent = operation_event_.Get();
        ResetEvent(operation_event_.Get());
        DWORD transferred{};
        if (start(pipe_, &transferred, &operation) != FALSE) {
            return Completed(transferred);
        }

        const DWORD error = GetLastError();
        if (error == ERROR_OPERATION_ABORTED && Stopping()) return Stopped();
        if (error != ERROR_IO_PENDING) return Failed(error);
        return Await(operation);
    }

    [[nodiscard]] IoResult Await(OVERLAPPED& operation) const noexcept {
        const HANDLE handles[] = {operation_event_.Get(), stop_event_};
        for (;;) {
            if (Stopping()) {
                CancelAndDrain(operation);
                return Stopped();
            }

            const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                DWORD transferred{};
                if (GetOverlappedResult(pipe_, &operation, &transferred, FALSE) != FALSE) {
                    return Completed(transferred);
                }
                const DWORD error = GetLastError();
                return error == ERROR_OPERATION_ABORTED && Stopping()
                    ? Stopped()
                    : Failed(error);
            }
            if (wait == WAIT_OBJECT_0 + 1U) {
                CancelAndDrain(operation);
                return Stopped();
            }
            if (wait == WAIT_FAILED) {
                const DWORD error = GetLastError();
                CancelAndDrain(operation);
                return Failed(error);
            }
            CancelAndDrain(operation);
            return Failed(ERROR_GEN_FAILURE);
        }
    }

    void CancelAndDrain(OVERLAPPED& operation) const noexcept {
        static_cast<void>(CancelIoEx(pipe_, &operation));
        DWORD ignored{};
        static_cast<void>(GetOverlappedResult(pipe_, &operation, &ignored, TRUE));
    }

    [[nodiscard]] bool Stopping() const noexcept {
        return stop_token_.stop_requested() ||
            WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    }

    [[nodiscard]] static IoResult Completed(DWORD transferred) noexcept {
        return {IoStatus::Completed, transferred, ERROR_SUCCESS};
    }

    [[nodiscard]] static IoResult Stopped() noexcept {
        return {IoStatus::Stopped, 0, ERROR_SUCCESS};
    }

    [[nodiscard]] static IoResult Failed(DWORD error) noexcept {
        return {IoStatus::Failed, 0, error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error};
    }

    HANDLE pipe_{};
    HANDLE stop_event_{};
    std::stop_token stop_token_;
    UniqueHandle operation_event_;
    DWORD event_error_{};
};

[[nodiscard]] bool IsClientDisconnectError(DWORD error) noexcept {
    return error == ERROR_NO_DATA || error == ERROR_BROKEN_PIPE ||
        error == ERROR_PIPE_NOT_CONNECTED;
}

[[nodiscard]] DWORD CurrentExceptionError() noexcept {
    try {
        throw;
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (const std::system_error& error) {
        const auto value = error.code().value();
        return value > 0 ? static_cast<DWORD>(value) : ERROR_GEN_FAILURE;
    } catch (...) {
        return ERROR_UNHANDLED_EXCEPTION;
    }
}

[[nodiscard]] bool IsJsonEnvelopeRequest(std::string_view request) noexcept {
    for (const unsigned char character : request) {
        switch (character) {
            case ' ':
            case '\t':
            case '\r':
                continue;
            default:
                return character == '{';
        }
    }
    return false;
}

[[nodiscard]] Json ResponseEnvelope(const Json& request_id) {
    return Json{{"protocol", kDiagnosticProtocol},
                {"version", kDiagnosticProtocolVersion},
                {"type", "response"},
                {"id", request_id}};
}

[[nodiscard]] std::string ProtocolError(
    const Json& request_id,
    std::string_view code,
    std::string_view message) {
    Json response = ResponseEnvelope(request_id);
    response["ok"] = false;
    response["error"] = {{"code", code}, {"message", message}};
    return response.dump();
}

[[nodiscard]] bool IsRequestId(const Json& value) noexcept {
    return value.is_string() &&
        value.get_ref<const std::string&>().size() <= kMaximumRequestIdSize;
}

[[nodiscard]] std::string ExecuteJsonEnvelope(
    const ue5mem::Analyzer& analyzer,
    std::string_view request_text) {
    const Json request = Json::parse(request_text, nullptr, false);
    if (request.is_discarded()) {
        return ProtocolError(nullptr, "malformed_json", "request is not valid JSON");
    }
    if (!request.is_object()) {
        return ProtocolError(nullptr, "invalid_request", "request must be a JSON object");
    }

    const auto request_id = request.find("id");
    if (request_id == request.end() || !IsRequestId(*request_id)) {
        return ProtocolError(
            nullptr,
            "invalid_request",
            "request id must be a string no longer than 256 bytes");
    }

    const auto protocol = request.find("protocol");
    if (protocol == request.end() || !protocol->is_string()) {
        return ProtocolError(*request_id, "invalid_request", "request protocol must be a string");
    }
    if (protocol->get_ref<const std::string&>() != kDiagnosticProtocol) {
        return ProtocolError(*request_id, "unsupported_protocol", "request protocol is unsupported");
    }

    const auto version = request.find("version");
    if (version == request.end() ||
        (!version->is_number_integer() && !version->is_number_unsigned())) {
        return ProtocolError(*request_id, "invalid_request", "request version must be an integer");
    }
    const bool supported_version = version->is_number_unsigned()
        ? version->get<std::uint64_t>() == kDiagnosticProtocolVersion
        : version->get<std::int64_t>() ==
            static_cast<std::int64_t>(kDiagnosticProtocolVersion);
    if (!supported_version) {
        return ProtocolError(*request_id, "unsupported_version", "request version is unsupported");
    }

    const auto type = request.find("type");
    if (type != request.end() &&
        (!type->is_string() || type->get_ref<const std::string&>() != "request")) {
        return ProtocolError(
            *request_id, "unsupported_message_type", "request type must be request");
    }

    const auto command = request.find("command");
    if (command == request.end() || !command->is_string() ||
        command->get_ref<const std::string&>().empty()) {
        return ProtocolError(*request_id, "invalid_request", "request command must be a non-empty string");
    }

    const Json analyzer_response = Json::parse(
        analyzer.Execute(command->get_ref<const std::string&>()), nullptr, false);
    if (analyzer_response.is_discarded() || !analyzer_response.is_object()) {
        return ProtocolError(
            *request_id,
            "analyzer_response_invalid",
            "diagnostic command returned an invalid response");
    }
    const auto result_ok = analyzer_response.find("ok");
    if (result_ok == analyzer_response.end() || !result_ok->is_boolean()) {
        return ProtocolError(
            *request_id,
            "analyzer_response_invalid",
            "diagnostic command response does not contain an ok flag");
    }

    Json response = ResponseEnvelope(*request_id);
    response["ok"] = result_ok->get<bool>();
    if (*result_ok) {
        Json result = analyzer_response;
        result.erase("ok");
        response["result"] = std::move(result);
        return response.dump();
    }

    std::string message = "diagnostic command failed";
    const auto error = analyzer_response.find("error");
    if (error != analyzer_response.end() && error->is_string()) {
        message = error->get_ref<const std::string&>();
    }
    response["error"] = {{"code", "command_failed"}, {"message", message}};
    return response.dump();
}

}  // namespace

class DiagnosticPipeService::Impl final {
public:
    explicit Impl(PipeServiceOptions options) : options_(std::move(options)) {}

    ~Impl() { Stop(); }

    [[nodiscard]] DWORD Prepare() noexcept {
        std::scoped_lock lock(mutex_);
        if (state_ == State::Prepared || state_ == State::Running ||
            state_ == State::Stopping) {
            return ERROR_ALREADY_INITIALIZED;
        }
        if (state_ == State::Stopped) return ERROR_OPERATION_ABORTED;
        if (!options_.analyzer || options_.pipe_name.empty() ||
            options_.pipe_name.find(L'\0') != std::wstring::npos) {
            return ERROR_INVALID_PARAMETER;
        }

        try {
            auto security = std::make_unique<PipeSecurity>();
            const DWORD security_error = security->Prepare();
            if (security_error != ERROR_SUCCESS) return security_error;

            UniqueHandle stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!stop_event) return GetLastError();

            DWORD listener_error{};
            UniqueHandle listener = CreateListener(*security, listener_error);
            if (!listener) return listener_error;

            security_ = std::move(security);
            stop_event_ = std::move(stop_event);
            prepared_listener_ = std::move(listener);
            state_ = State::Prepared;
            return ERROR_SUCCESS;
        } catch (...) {
            return CurrentExceptionError();
        }
    }

    [[nodiscard]] DWORD Run(std::stop_token stop_token) noexcept {
        UniqueHandle listener;
        {
            std::scoped_lock lock(mutex_);
            if (state_ == State::Fresh) return ERROR_INVALID_STATE;
            if (state_ == State::Running || state_ == State::Stopping) return ERROR_BUSY;
            if (state_ == State::Stopped) return ERROR_SUCCESS;

            HANDLE raw_run_thread{};
            if (DuplicateHandle(
                    GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &raw_run_thread, THREAD_TERMINATE, FALSE, 0) != FALSE) {
                run_thread_.Reset(raw_run_thread);
            }
            listener = std::move(prepared_listener_);
            state_ = State::Running;
            active_listener_ = listener.Get();
        }

        std::stop_callback stop_callback(stop_token, [this] { Stop(); });
        DWORD result{};
        try {
            result = RunLoop(std::move(listener), stop_token);
        } catch (...) {
            result = CurrentExceptionError();
        }

        {
            std::scoped_lock lock(mutex_);
            active_listener_ = nullptr;
            run_thread_.Reset();
            state_ = State::Stopped;
        }
        return stop_requested_.load(std::memory_order_acquire) ? ERROR_SUCCESS : result;
    }

    void Stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        UniqueHandle listener;
        std::scoped_lock lock(mutex_);
        if (stop_event_) static_cast<void>(SetEvent(stop_event_.Get()));

        switch (state_) {
            case State::Fresh:
                state_ = State::Stopped;
                break;
            case State::Prepared:
                listener = std::move(prepared_listener_);
                state_ = State::Stopped;
                break;
            case State::Running:
                state_ = State::Stopping;
                [[fallthrough]];
            case State::Stopping:
                if (active_listener_ != nullptr) {
                    static_cast<void>(CancelIoEx(active_listener_, nullptr));
                    static_cast<void>(DisconnectNamedPipe(active_listener_));
                }
                break;
            case State::Stopped:
                break;
        }
        if (run_thread_) {
            static_cast<void>(CancelSynchronousIo(run_thread_.Get()));
        }
    }

private:
    enum class State {
        Fresh,
        Prepared,
        Running,
        Stopping,
        Stopped,
    };

    [[nodiscard]] UniqueHandle CreateListener(
        PipeSecurity& security, DWORD& error) const noexcept {
        HANDLE pipe = CreateNamedPipeW(
            options_.pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            4,
            kOutputBufferSize,
            kInputBufferSize,
            0,
            security.Attributes());
        if (pipe == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            return UniqueHandle{};
        }
        error = ERROR_SUCCESS;
        return UniqueHandle(pipe);
    }

    [[nodiscard]] DWORD RunLoop(
        UniqueHandle listener, std::stop_token stop_token) noexcept {
        for (;;) {
            if (Stopping(stop_token)) {
                ClearActiveListener(listener.Get());
                return ERROR_SUCCESS;
            }

            PipeIo io(listener.Get(), stop_event_.Get(), stop_token);
            if (io.InitializationError() != ERROR_SUCCESS) {
                ClearActiveListener(listener.Get());
                return io.InitializationError();
            }

            const IoResult connection = io.Connect();
            if (connection.status == IoStatus::Stopped) {
                ClearActiveListener(listener.Get());
                return ERROR_SUCCESS;
            }
            if (connection.status == IoStatus::Failed &&
                !IsClientDisconnectError(connection.error)) {
                ClearActiveListener(listener.Get());
                return connection.error;
            }
            if (connection.status == IoStatus::Completed) {
                const IoResult client = ServeClient(io);
                if (client.status == IoStatus::Stopped) {
                    static_cast<void>(DisconnectNamedPipe(listener.Get()));
                    ClearActiveListener(listener.Get());
                    return ERROR_SUCCESS;
                }
                if (client.status == IoStatus::Failed &&
                    !IsClientDisconnectError(client.error)) {
                    static_cast<void>(DisconnectNamedPipe(listener.Get()));
                    ClearActiveListener(listener.Get());
                    return client.error;
                }
            }

            static_cast<void>(DisconnectNamedPipe(listener.Get()));
            ClearActiveListener(listener.Get());
            listener.Reset();
            if (Stopping(stop_token)) return ERROR_SUCCESS;

            DWORD listener_error{};
            listener = CreateListener(*security_, listener_error);
            if (!listener) {
                return Stopping(stop_token) ? ERROR_SUCCESS : listener_error;
            }
            if (!PublishActiveListener(listener.Get())) return ERROR_SUCCESS;
        }
    }

    [[nodiscard]] IoResult ServeClient(const PipeIo& io) noexcept {
        try {
            std::string request;
            std::array<char, 4096> buffer{};
            while (request.size() < kMaximumRequestSize) {
                const std::size_t remaining = kMaximumRequestSize - request.size();
                const DWORD size = static_cast<DWORD>(
                    (std::min)(remaining, buffer.size()));
                const IoResult read = io.Read(buffer.data(), size);
                if (read.status == IoStatus::Stopped) return read;
                if (read.status == IoStatus::Failed) {
                    return IsClientDisconnectError(read.error)
                        ? IoResult{IoStatus::Completed, 0, ERROR_SUCCESS}
                        : read;
                }
                if (read.transferred == 0) break;
                request.append(buffer.data(), read.transferred);
                if (request.find('\n') != std::string::npos) break;
            }

            const auto newline = request.find('\n');
            if (newline != std::string::npos) request.resize(newline);
            const std::string response =
                (IsJsonEnvelopeRequest(request)
                    ? ExecuteJsonEnvelope(*options_.analyzer, request)
                    : options_.analyzer->Execute(request)) +
                "\n";
            std::size_t offset{};
            while (offset < response.size()) {
                const DWORD size = static_cast<DWORD>((std::min<std::size_t>)(
                    response.size() - offset, MAXDWORD));
                const IoResult write = io.Write(response.data() + offset, size);
                if (write.status == IoStatus::Stopped) return write;
                if (write.status == IoStatus::Failed) {
                    return IsClientDisconnectError(write.error)
                        ? IoResult{IoStatus::Completed, 0, ERROR_SUCCESS}
                        : write;
                }
                if (write.transferred == 0) {
                    return {IoStatus::Failed, 0, ERROR_WRITE_FAULT};
                }
                offset += write.transferred;
            }
            const IoResult flush = io.Flush();
            if (flush.status == IoStatus::Stopped) return flush;
            if (flush.status == IoStatus::Failed) {
                return IsClientDisconnectError(flush.error)
                    ? IoResult{IoStatus::Completed, 0, ERROR_SUCCESS}
                    : flush;
            }
        } catch (...) {
            return {IoStatus::Failed, 0, CurrentExceptionError()};
        }
        return {IoStatus::Completed, 0, ERROR_SUCCESS};
    }

    [[nodiscard]] bool Stopping(std::stop_token stop_token) const noexcept {
        return stop_requested_.load(std::memory_order_acquire) ||
            stop_token.stop_requested();
    }

    void ClearActiveListener(HANDLE listener) noexcept {
        std::scoped_lock lock(mutex_);
        if (active_listener_ == listener) active_listener_ = nullptr;
    }

    [[nodiscard]] bool PublishActiveListener(HANDLE listener) noexcept {
        std::scoped_lock lock(mutex_);
        if (state_ != State::Running ||
            stop_requested_.load(std::memory_order_acquire)) {
            return false;
        }
        active_listener_ = listener;
        return true;
    }

    PipeServiceOptions options_;
    std::mutex mutex_;
    State state_{State::Fresh};
    std::atomic_bool stop_requested_{};
    std::unique_ptr<PipeSecurity> security_;
    UniqueHandle stop_event_;
    UniqueHandle prepared_listener_;
    UniqueHandle run_thread_;
    HANDLE active_listener_{};
};

DiagnosticPipeService::DiagnosticPipeService(PipeServiceOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

DiagnosticPipeService::~DiagnosticPipeService() = default;

DWORD DiagnosticPipeService::Prepare() noexcept {
    return impl_->Prepare();
}

DWORD DiagnosticPipeService::Run(std::stop_token stop_token) noexcept {
    return impl_->Run(stop_token);
}

void DiagnosticPipeService::Stop() noexcept {
    impl_->Stop();
}

}  // namespace anomaly
