#pragma once

#include <Windows.h>

#include <vector>

namespace anomaly::launcher {

// Both Win32 and native procedure lookup can return process-local AppCompat
// wrappers. Read the PE exports instead, retaining forwarded modules until the
// caller has translated the resulting module/RVA into the destination process.
class ImportProcedureResolver final {
public:
    ImportProcedureResolver() = default;
    ~ImportProcedureResolver();
    ImportProcedureResolver(const ImportProcedureResolver&) = delete;
    ImportProcedureResolver& operator=(const ImportProcedureResolver&) = delete;

    [[nodiscard]] FARPROC Resolve(HMODULE module, const char* name);

private:
    std::vector<HMODULE> forwarded_modules_;
};

}  // namespace anomaly::launcher
