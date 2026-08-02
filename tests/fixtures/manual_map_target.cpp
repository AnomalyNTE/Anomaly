#include <Windows.h>

#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    if (IsDebuggerPresent() != FALSE) return 5;
    const HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[1]);
    const HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    if (ready == nullptr || stop == nullptr) {
        if (ready != nullptr) CloseHandle(ready);
        if (stop != nullptr) CloseHandle(stop);
        return 3;
    }
    SetEvent(ready);
    const DWORD wait = WaitForSingleObject(stop, 30000);
    CloseHandle(stop);
    CloseHandle(ready);
    return wait == WAIT_OBJECT_0 ? 0 : 4;
}
