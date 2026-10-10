#include "synthetic_ue.hpp"

#include <cstdio>
#include <Windows.h>

namespace jelly_tests {
int g_failures = 0;
}  // namespace jelly_tests

int main() {
    // A faulting fixture has to fail fast: without this the process would wait behind a
    // crash dialog that nobody is there to answer, and an automated run would hang.
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    std::printf("jelly offline fixture\n");
    jelly_tests::RunLogicTests();
    jelly_tests::RunModelTests();
    if (jelly_tests::g_failures == 0) {
        std::printf("all checks passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", jelly_tests::g_failures);
    return 1;
}
