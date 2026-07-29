#pragma once

#include <Windows.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace anomaly {

template <typename T>
class ThreadLocalObject final {
public:
    ThreadLocalObject() : ThreadLocalObject(T{}) {}

    explicit ThreadLocalObject(T initial)
        : slot_(FlsAlloc(&Destroy)), initial_(std::move(initial)) {
        if (slot_ == FLS_OUT_OF_INDEXES) throw std::bad_alloc();
    }

    ~ThreadLocalObject() {
        if (slot_ == FLS_OUT_OF_INDEXES) return;
        if (void* current = FlsGetValue(slot_); current != nullptr) {
            static_cast<void>(FlsSetValue(slot_, nullptr));
            Destroy(current);
        }
        static_cast<void>(FlsFree(slot_));
    }

    ThreadLocalObject(const ThreadLocalObject&) = delete;
    ThreadLocalObject& operator=(const ThreadLocalObject&) = delete;

    [[nodiscard]] T& Get() {
        if (void* current = FlsGetValue(slot_); current != nullptr) {
            return *static_cast<T*>(current);
        }
        auto value = std::make_unique<T>(initial_);
        if (FlsSetValue(slot_, value.get()) == FALSE) throw std::bad_alloc();
        return *value.release();
    }

private:
    static void NTAPI Destroy(void* value) noexcept {
        delete static_cast<T*>(value);
    }

    DWORD slot_{FLS_OUT_OF_INDEXES};
    T initial_;
};

template <typename T>
class ThreadLocalScalar final {
    static_assert(
        (std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>) &&
        sizeof(T) <= sizeof(void*));

public:
    ThreadLocalScalar() : slot_(FlsAlloc(nullptr)) {
        if (slot_ == FLS_OUT_OF_INDEXES) throw std::bad_alloc();
    }

    ~ThreadLocalScalar() {
        if (slot_ != FLS_OUT_OF_INDEXES) static_cast<void>(FlsFree(slot_));
    }

    ThreadLocalScalar(const ThreadLocalScalar&) = delete;
    ThreadLocalScalar& operator=(const ThreadLocalScalar&) = delete;

    [[nodiscard]] T Get() const noexcept {
        const auto value = reinterpret_cast<std::uintptr_t>(FlsGetValue(slot_));
        if constexpr (std::is_pointer_v<T>) {
            return reinterpret_cast<T>(value);
        } else {
            return static_cast<T>(value);
        }
    }

    void Set(T value) noexcept {
        std::uintptr_t encoded{};
        if constexpr (std::is_pointer_v<T>) {
            encoded = reinterpret_cast<std::uintptr_t>(value);
        } else {
            encoded = static_cast<std::uintptr_t>(value);
        }
        if (FlsSetValue(slot_, reinterpret_cast<void*>(encoded)) == FALSE) {
            std::terminate();
        }
    }

private:
    DWORD slot_{FLS_OUT_OF_INDEXES};
};

}  // namespace anomaly
