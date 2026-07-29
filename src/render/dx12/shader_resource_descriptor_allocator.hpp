#pragma once

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace ue5mem::embedded {

// The ImGui DX12 backend consumes the first allocation for its font atlas.
// Keeping every UI SRV in this one shader-visible heap lets future host-owned
// textures use their GPU descriptor handles as ImTextureID values.
inline constexpr UINT kEmbeddedShaderResourceDescriptorCapacity = 128;

class ShaderResourceDescriptorAllocator final {
public:
    [[nodiscard]] bool Configure(
        D3D12_CPU_DESCRIPTOR_HANDLE cpu_base,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu_base,
        UINT increment,
        UINT capacity) noexcept {
        if (cpu_base.ptr == 0 || gpu_base.ptr == 0 || increment == 0 || capacity == 0 ||
            capacity > kEmbeddedShaderResourceDescriptorCapacity) {
            return false;
        }

        Reset();
        cpu_base_ = cpu_base;
        gpu_base_ = gpu_base;
        increment_ = increment;
        capacity_ = capacity;
        free_count_ = capacity;
        for (UINT slot = 0; slot < capacity; ++slot) {
            // Allocate slot zero first so the existing ImGui font atlas keeps
            // its historical descriptor position.
            free_slots_[slot] = capacity - slot - 1;
        }
        return true;
    }

    void Reset() noexcept {
        cpu_base_ = {};
        gpu_base_ = {};
        increment_ = 0;
        capacity_ = 0;
        free_count_ = 0;
        allocated_.fill(false);
        reserved_.fill(false);
    }

    [[nodiscard]] bool Allocate(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpu) noexcept {
        if (cpu == nullptr || gpu == nullptr || free_count_ == 0) return false;
        const UINT slot = free_slots_[free_count_ - 1];
        D3D12_CPU_DESCRIPTOR_HANDLE resolved_cpu{};
        D3D12_GPU_DESCRIPTOR_HANDLE resolved_gpu{};
        if (!HandlesFor(slot, &resolved_cpu, &resolved_gpu)) return false;
        --free_count_;
        allocated_[slot] = true;
        *cpu = resolved_cpu;
        *gpu = resolved_gpu;
        return true;
    }

    // ImGui keeps its font descriptor handles across device-object rebuilds,
    // even though it invokes the supplied free callback between rebuilds.
    // Reserve that descriptor until the whole heap is reset so another UI
    // texture cannot be assigned the same slot.
    [[nodiscard]] bool AllocateReserved(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpu) noexcept {
        if (cpu == nullptr || gpu == nullptr) return false;
        for (UINT slot = 0; slot < capacity_; ++slot) {
            if (!reserved_[slot]) continue;
            if (!allocated_[slot]) {
                reserved_[slot] = false;
                continue;
            }
            return HandlesFor(slot, cpu, gpu);
        }
        if (!Allocate(cpu, gpu)) return false;
        const auto slot = SlotFor(*cpu, *gpu);
        if (!slot) return false;
        reserved_[*slot] = true;
        return true;
    }

    [[nodiscard]] bool Free(
        D3D12_CPU_DESCRIPTOR_HANDLE cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu) noexcept {
        const auto slot = SlotFor(cpu, gpu);
        if (!slot || !allocated_[*slot] || free_count_ >= capacity_) return false;
        if (reserved_[*slot]) return true;
        allocated_[*slot] = false;
        free_slots_[free_count_++] = *slot;
        return true;
    }

    // ImGui releases the atlas descriptor through its callback during an
    // atlas rebuild, but the device backend retains the handle until DX12
    // shutdown. Release every protected slot at that shutdown boundary.
    void ReleaseReserved() noexcept {
        for (UINT slot = 0; slot < capacity_; ++slot) {
            if (!reserved_[slot]) continue;
            reserved_[slot] = false;
            if (allocated_[slot] && free_count_ < capacity_) {
                allocated_[slot] = false;
                free_slots_[free_count_++] = slot;
            }
        }
    }

    [[nodiscard]] bool Owns(
        D3D12_CPU_DESCRIPTOR_HANDLE cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu) const noexcept {
        return SlotFor(cpu, gpu).has_value();
    }

    [[nodiscard]] bool IsAllocated(
        D3D12_CPU_DESCRIPTOR_HANDLE cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu) const noexcept {
        const auto slot = SlotFor(cpu, gpu);
        return slot && allocated_[*slot];
    }

    [[nodiscard]] UINT Capacity() const noexcept { return capacity_; }
    [[nodiscard]] UINT Available() const noexcept { return free_count_; }

private:
    [[nodiscard]] bool HandlesFor(
        UINT slot,
        D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpu) const noexcept {
        if (slot >= capacity_ || increment_ == 0 || cpu == nullptr || gpu == nullptr) return false;
        const std::uint64_t offset = static_cast<std::uint64_t>(slot) * increment_;
        if (offset > (std::numeric_limits<SIZE_T>::max)() - cpu_base_.ptr ||
            offset > (std::numeric_limits<UINT64>::max)() - gpu_base_.ptr) {
            return false;
        }
        cpu->ptr = cpu_base_.ptr + static_cast<SIZE_T>(offset);
        gpu->ptr = gpu_base_.ptr + offset;
        return true;
    }

    [[nodiscard]] std::optional<UINT> SlotFor(
        D3D12_CPU_DESCRIPTOR_HANDLE cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu) const noexcept {
        if (capacity_ == 0 || increment_ == 0 || cpu.ptr < cpu_base_.ptr ||
            gpu.ptr < gpu_base_.ptr) {
            return std::nullopt;
        }
        const SIZE_T cpu_offset = cpu.ptr - cpu_base_.ptr;
        const UINT64 gpu_offset = gpu.ptr - gpu_base_.ptr;
        if ((cpu_offset % increment_) != 0 || (gpu_offset % increment_) != 0 ||
            static_cast<std::uint64_t>(cpu_offset / increment_) != gpu_offset / increment_) {
            return std::nullopt;
        }
        const std::uint64_t slot = gpu_offset / increment_;
        if (slot >= capacity_) return std::nullopt;
        return static_cast<UINT>(slot);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu_base_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_base_{};
    UINT increment_{};
    UINT capacity_{};
    UINT free_count_{};
    std::array<UINT, kEmbeddedShaderResourceDescriptorCapacity> free_slots_{};
    std::array<bool, kEmbeddedShaderResourceDescriptorCapacity> allocated_{};
    std::array<bool, kEmbeddedShaderResourceDescriptorCapacity> reserved_{};
};

}  // namespace ue5mem::embedded
