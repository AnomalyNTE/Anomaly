#include "account_identity.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void Check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void AccountUidIsCanonicalAndPathSafe() {
    const auto known = nte::account::FromUidText("000216065736008");
    Check(known.has_value() && known->uid == "216065736008" &&
            known->source == nte::account::Source::UiRoleId,
        "numeric UI text should become a canonical known account");
    Check(!nte::account::FromUidText("0") &&
            !nte::account::FromUidText("123456789012345678901") &&
            !nte::account::FromUidText("12/34") &&
            !nte::account::FromUidText(""),
        "zero, overlong, non-numeric, and empty UIDs should be rejected");
    Check(nte::account::DirectoryName(*known) == "216065736008" &&
            nte::account::DirectoryName(nte::account::Unknown()) == "_unknown",
        "account directory names should be deterministic and path safe");
    Check(nte::account::FromDirectoryName("216065736008") == known &&
            nte::account::FromDirectoryName("_unknown") ==
                std::optional{nte::account::Unknown()} &&
            !nte::account::FromDirectoryName("../outside"),
        "only known UID and unknown sentinel directories should parse");
    Check(nte::account::ShouldTransition(nte::account::Unknown(), known) &&
            !nte::account::ShouldTransition(*known, known) &&
            !nte::account::ShouldTransition(*known, std::nullopt),
        "only a newly detected different UID should create an account boundary");
}

}  // namespace

int main() {
    AccountUidIsCanonicalAndPathSafe();
    std::cout << "All account identity tests passed\n";
}
