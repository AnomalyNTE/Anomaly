#pragma once

#include <cstdint>

#include <Windows.h>

namespace anomaly::launcher {

// Contract between the launcher process and the relay image (Anomaly.Relay.dll).
//
// The relay image is manually mapped by the launcher into an innocent host process
// (see ManualMapRelayOptions). It receives this structure through the remote bootstrap:
// the bootstrap calls the image's exported AnomalyStart with a pointer to the marshaled
// block, and the relay reinterprets that pointer as RelayStartInfo*.
//
// All strings are inline and NUL-terminated so the block is a single self-contained
// marshaling unit: the launcher writes it with one remote allocation.
constexpr std::uint32_t ANOMALY_RELAY_START_INFO_VERSION = 1u;
constexpr std::uint32_t ANOMALY_RELAY_RESULT_VERSION = 1u;

struct RelayStartInfo final {
    std::uint32_t struct_size;
    std::uint32_t version;
    // The relay waits for this manual-reset event (signalled by the launcher hook)
    // before it starts polling for the game process. Empty name: poll immediately.
    wchar_t armed_event_name[96];
    // How long the relay waits for the armed event before polling anyway.
    std::uint32_t armed_timeout_ms;
    // Budget for discovering the game process and for the game loader to initialize.
    std::uint32_t capture_timeout_ms;
    std::uint32_t loader_timeout_ms;
    // Budget for mapping and bootstrapping the core inside the game process.
    std::uint32_t attach_timeout_ms;
    // Game processes that already existed when the launcher started the official
    // launcher. The relay treats any process outside this set as the new session's
    // game. The launcher snapshots this set before the official launcher exists, which
    // is the only point that precedes the hook signalling the armed event and starting
    // the game -- a snapshot taken later, inside the relay, could miss a game the hook
    // already spawned while the relay host was still starting up. Excess pids beyond the
    // array are ignored.
    std::uint32_t excluded_pid_count;
    DWORD excluded_pids[16];
    wchar_t game_executable_name[64];
    wchar_t core_path[512];
    wchar_t runtime_root[512];
    wchar_t log_directory[512];
    // Named shared state the relay reports through: a file mapping holding RelayResult
    // and a manual-reset event the relay sets once the result is visible.
    wchar_t result_mapping_name[96];
    wchar_t result_event_name[96];
};

constexpr std::uint32_t ANOMALY_RELAY_START_INFO_V1_SIZE = sizeof(RelayStartInfo);

// Written by the relay into the named result mapping after the attach attempt settles,
// regardless of outcome. The launcher reads it after the result event is signalled.
struct RelayResult final {
    std::uint32_t struct_size;
    std::uint32_t version;
    // ManualMapError value of the attach attempt.
    std::uint32_t error;
    std::uint32_t win32_error;
    std::uint32_t runtime_start_error;
    std::uint32_t game_process_id;
    std::uint32_t reserved;
    std::uint64_t remote_image;
    wchar_t message[256];
};

constexpr std::uint32_t ANOMALY_RELAY_RESULT_V1_SIZE = sizeof(RelayResult);

}  // namespace anomaly::launcher
