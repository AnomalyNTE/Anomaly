#include "anomaly/core_api.h"

#include <Windows.h>

namespace {

thread_local volatile LONG g_static_tls_value{0x24681357};

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI AnomalyStart(
    const AnomalyStartInfo*) {
    return g_static_tls_value == 0x24681357 ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

BOOL WINAPI DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
