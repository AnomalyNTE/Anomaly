#include <Windows.h>

extern "C" __declspec(dllexport) HRESULT WINAPI DwmIsCompositionEnabled(BOOL* enabled) {
    if (enabled != nullptr) *enabled = FALSE;
    return static_cast<HRESULT>(0x81234567L);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
