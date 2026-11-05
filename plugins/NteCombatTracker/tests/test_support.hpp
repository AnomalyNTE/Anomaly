#pragma once

// Shared scaffolding for the plugin's offline fixtures.
//
// Each test source exposes one or more named runners instead of its own main(),
// and a per-group entry point calls them in turn. That keeps one executable per
// link dependency group rather than one per source file, without dropping any
// assertion: a failure is counted and reported, so a single run shows every
// broken case.

#include <cstdio>

namespace nte::test {

inline int& FailureCount() {
    static int failures{};
    return failures;
}

using Runner = void (*)();

[[nodiscard]] inline int RunGroup(const char* group, const Runner* runners,
                                  const std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        runners[index]();
    }
    const int failures = FailureCount();
    if (failures != 0) {
        std::fprintf(stderr, "%s: %d check(s) failed\n", group, failures);
        return 1;
    }
    std::printf("%s: all tests passed\n", group);
    return 0;
}

}  // namespace nte::test

#define NTE_CHECK(condition, message)                                     \
    do {                                                                  \
        if (!(condition)) {                                               \
            std::fprintf(stderr, "FAIL: %s\n", message);                  \
            ++::nte::test::FailureCount();                                \
        }                                                                 \
    } while (false)

// Defines the entry point for one dependency group.
#define NTE_TEST_GROUP(group, ...)                                        \
    int main() {                                                          \
        const ::nte::test::Runner runners[]{__VA_ARGS__};                 \
        return ::nte::test::RunGroup(group, runners, sizeof(runners) / sizeof(runners[0])); \
    }
