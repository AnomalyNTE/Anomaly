#pragma once

#include "miniaudio.h"

namespace hifi_vehicle_music {

// DSF/DFF converted to PCM as a miniaudio custom decoding backend: dsd2pcm
// low-pass filters and decimates 8:1 (DSD64 -> 352.8 kHz float), miniaudio
// resamples further when the device needs it. Only used when neither native
// DSD nor DoP reaches the DAC, or when the user chose PCM.
[[nodiscard]] ma_decoding_backend_vtable* DsdPcmBackend() noexcept;

} // namespace hifi_vehicle_music
