#pragma once

#include "miniaudio.h"

namespace hifi_vehicle_music {

// Monkey's Audio (.ape) as a miniaudio custom decoding backend: decoders
// created with it in ma_decoder_config::ppCustomBackendVTables read APE files
// through the official SDK and convert like any built-in format.
[[nodiscard]] ma_decoding_backend_vtable* ApeBackend() noexcept;

} // namespace hifi_vehicle_music
