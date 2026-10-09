// Compiled into every C++ plugin package by anomaly_add_plugin.
//
// The runtime loads plugin images with an in-process mapper instead of the
// Windows loader, so the stock vcruntime _CxxThrowException cannot derive the
// throwing module's base: it asks the loader (RtlPcToFileHeader on its own
// return address, resolved through the loaded-module list) and a mapped image
// is not on that list. The mapper therefore rebinds each plugin's
// _CxxThrowException import to this bridge, which states the image base
// explicitly through the exception parameters.
//
// This file is self-contained on purpose: plugin packages must not include host
// internal headers, and the SDK distribution ships it as source.
#include <Windows.h>

#include <iterator>

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" __declspec(dllexport) __declspec(noreturn) void WINAPI
AnomalyPluginCxxThrowV1(void* exception_object, const void* throw_info) {
    constexpr DWORD kMsvcCxxException = 0xE06D7363UL;
    constexpr ULONG_PTR kMsvcEhMagicNumber1 = 0x19930520UL;
    const ULONG_PTR parameters[]{
        kMsvcEhMagicNumber1,
        reinterpret_cast<ULONG_PTR>(exception_object),
        reinterpret_cast<ULONG_PTR>(throw_info),
        reinterpret_cast<ULONG_PTR>(&__ImageBase),
    };
    RaiseException(
        kMsvcCxxException, EXCEPTION_NONCONTINUABLE,
        static_cast<DWORD>(std::size(parameters)), parameters);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}
