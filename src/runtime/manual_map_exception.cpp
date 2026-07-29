#include <Windows.h>

#include <intrin.h>

#include <iterator>

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" __declspec(dllexport) __declspec(noreturn) void WINAPI
AnomalyManualMapCxxThrow(void* exception_object, const void* throw_info) {
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
