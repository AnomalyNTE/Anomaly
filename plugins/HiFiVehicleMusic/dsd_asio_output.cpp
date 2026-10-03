#include "plugins/HiFiVehicleMusic/dsd_asio_output.hpp"

#include <Windows.h>

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#include "plugins/HiFiVehicleMusic/dsd_reader.hpp"

#include <algorithm>
#include <atomic>

// Steinberg host helper (asiodrivers.cpp): loads a driver into the global
// slot that the ASIO* functions use.
bool loadAsioDriver(char* name);
extern AsioDrivers* asioDrivers;

namespace hifi_vehicle_music {
namespace {

// The ASIO callbacks carry no user pointer; one output exists at a time.
std::atomic<DsdAsioOutput*> g_active{};

// Idle DSD pattern (01101001), MSB first: zero average, no click.
constexpr std::uint8_t kDsdSilence = 0x69;

std::string AsioErrorText(const char* step, const ASIOError result) {
    return std::string(step) + " (" + std::to_string(result) + ")";
}

} // namespace

DsdAsioOutput::~DsdAsioOutput() { Close(); }

bool DsdAsioOutput::Open(const std::string& driver, const unsigned channels, const unsigned rate,
    const Fill fill, void* const user, std::string& error) {
    Close();
    std::string name = driver;
    if (!loadAsioDriver(name.data())) {
        error = "cannot load ASIO driver " + driver;
        return false;
    }
    loaded_ = true;
    ASIODriverInfo info{};
    info.asioVersion = 2;
    info.sysRef = GetForegroundWindow();
    if (const ASIOError result = ASIOInit(&info); result != ASE_OK) {
        error = AsioErrorText("ASIOInit", result);
        Close();
        return false;
    }

    // Switching to DSD must happen before the buffers are created. The spec
    // asks for ASE_SUCCESS; some drivers answer ASE_OK instead.
    // Some drivers only switch after being asked kAsioCanDoIoFormat first.
    ASIOIoFormat format{};
    format.FormatType = kASIODSDFormat;
    const ASIOError can_do = ASIOFuture(kAsioCanDoIoFormat, &format);
    format.FormatType = kASIODSDFormat;
    if (const ASIOError result = ASIOFuture(kAsioSetIoFormat, &format);
        result != ASE_SUCCESS && result != ASE_OK) {
        error = "native ASIO DSD refused (CanDoIoFormat " + std::to_string(can_do) + ", SetIoFormat " +
            std::to_string(result) + ")";
        Close();
        return false;
    }
    dsd_mode_ = true;
    if (ASIOCanSampleRate(rate) != ASE_OK) {
        error = driver + " does not accept DSD rate " + std::to_string(rate);
        Close();
        return false;
    }
    if (const ASIOError result = ASIOSetSampleRate(rate); result != ASE_OK) {
        error = AsioErrorText("ASIOSetSampleRate", result);
        Close();
        return false;
    }

    long inputs{};
    long outputs{};
    if (ASIOGetChannels(&inputs, &outputs) != ASE_OK || outputs < static_cast<long>(channels)) {
        error = driver + " has fewer than " + std::to_string(channels) + " outputs";
        Close();
        return false;
    }
    ASIOChannelInfo channel{};
    channel.channel = 0;
    channel.isInput = ASIOFalse;
    if (ASIOGetChannelInfo(&channel) != ASE_OK ||
        (channel.type != ASIOSTDSDInt8LSB1 && channel.type != ASIOSTDSDInt8MSB1)) {
        error = driver + " reports no 1-bit DSD sample type (" + std::to_string(channel.type) + ")";
        Close();
        return false;
    }
    lsb_first_ = channel.type == ASIOSTDSDInt8LSB1;

    long min_size{};
    long max_size{};
    long preferred{};
    long granularity{};
    if (ASIOGetBufferSize(&min_size, &max_size, &preferred, &granularity) != ASE_OK || preferred < 8) {
        error = "ASIOGetBufferSize failed";
        Close();
        return false;
    }
    // Buffer sizes count DSD samples; the 1-bit types pack 8 per byte.
    bytes_ = static_cast<std::size_t>(preferred) / 8;
    channels_ = channels;
    rate_ = rate;
    fill_ = fill;
    user_ = user;
    scratch_.assign(bytes_ * channels_, kDsdSilence);

    std::vector<ASIOBufferInfo> buffers(channels_);
    for (unsigned c = 0; c < channels_; ++c) {
        buffers[c].isInput = ASIOFalse;
        buffers[c].channelNum = static_cast<long>(c);
    }
    static ASIOCallbacks callbacks{&DsdAsioOutput::BufferSwitch, &DsdAsioOutput::SampleRateChanged,
        &DsdAsioOutput::Message, &DsdAsioOutput::BufferSwitchTimeInfo};
    g_active.store(this, std::memory_order_release);
    if (const ASIOError result = ASIOCreateBuffers(buffers.data(), static_cast<long>(channels_), preferred,
            &callbacks);
        result != ASE_OK) {
        error = AsioErrorText("ASIOCreateBuffers", result);
        Close();
        return false;
    }
    buffers_ = true;
    const std::uint8_t idle = lsb_first_ ? ReverseBits(kDsdSilence) : kDsdSilence;
    for (int half = 0; half < 2; ++half) {
        halves_[half].resize(channels_);
        for (unsigned c = 0; c < channels_; ++c) {
            halves_[half][c] = buffers[c].buffers[half];
            std::fill_n(static_cast<std::uint8_t*>(halves_[half][c]), bytes_, idle);
        }
    }
    if (const ASIOError result = ASIOStart(); result != ASE_OK) {
        error = AsioErrorText("ASIOStart", result);
        Close();
        return false;
    }
    open_ = true;
    return true;
}

void DsdAsioOutput::Close() noexcept {
    if (open_) ASIOStop();
    open_ = false;
    if (buffers_) ASIODisposeBuffers();
    buffers_ = false;
    g_active.store(nullptr, std::memory_order_release);
    if (dsd_mode_) {
        // Leave the driver in PCM mode for RtAudio and other hosts.
        ASIOIoFormat format{};
        format.FormatType = kASIOPCMFormat;
        ASIOFuture(kAsioSetIoFormat, &format);
    }
    dsd_mode_ = false;
    // ASIOExit releases the driver loadAsioDriver loaded, except after a
    // failed ASIOInit, which already dropped its pointer to it.
    if (loaded_) {
        ASIOExit();
        if (asioDrivers != nullptr) asioDrivers->removeCurrentDriver();
    }
    loaded_ = false;
    channels_ = rate_ = 0;
    halves_[0].clear();
    halves_[1].clear();
}

void DsdAsioOutput::Render(const long index) noexcept {
    if (index < 0 || index > 1 || halves_[index].size() != channels_) return;
    fill_(user_, scratch_.data(), bytes_);
    for (unsigned c = 0; c < channels_; ++c) {
        auto* const out = static_cast<std::uint8_t*>(halves_[index][c]);
        for (std::size_t i = 0; i < bytes_; ++i) {
            const std::uint8_t value = scratch_[i * channels_ + c];
            out[i] = lsb_first_ ? ReverseBits(value) : value;
        }
    }
    ASIOOutputReady();
}

void DsdAsioOutput::BufferSwitch(const long index, long) {
    if (DsdAsioOutput* const self = g_active.load(std::memory_order_acquire)) self->Render(index);
}

void DsdAsioOutput::SampleRateChanged(double) {}

long DsdAsioOutput::Message(const long selector, const long value, void*, double*) {
    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioResyncRequest ||
                value == kAsioLatenciesChanged
            ? 1L : 0L;
    case kAsioEngineVersion:
        return 2L;
    case kAsioResetRequest:
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        return 1L;
    default:
        return 0L;
    }
}

::ASIOTime* DsdAsioOutput::BufferSwitchTimeInfo(::ASIOTime* time, const long index, const long direct_process) {
    BufferSwitch(index, direct_process);
    return time;
}

} // namespace hifi_vehicle_music
