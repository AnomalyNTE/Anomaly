#include "awakening_data.hpp"
#include <iostream>
using namespace awakening_data;
int main() {
    int failed{};
    const auto check = [&](bool ok, const char* label) { if (!ok) { ++failed; std::cerr << label << '\n'; } };
    const std::array<Name,6> names{{{11,0},{22,0},{33,0},{44,0},{55,0},{66,0}}};
    Effects original{};
    original[0] = {1,{7,8,9},names[5]};
    original[1] = {1,{1,2,3},names[1]};
    const auto unlocked = Unlock(original,names);
    check(unlocked.has_value(),"partially awakened character accepted");
    if (!unlocked) return 1;
    check((*unlocked)[0] == original[0] && (*unlocked)[1] == original[1],"existing reordered selections and padding preserved");
    for (const auto n : names) {
        int count{}; for (const auto& e : *unlocked) if (e.unlocked && e.name == n) ++count;
        check(count == 1,"all six upgrades present once");
    }
    check(Restore(*unlocked,*unlocked,original) == original,"full restoration matches original bytes");
    auto authoritative = *unlocked;
    authoritative[2] = {1,{0,0,0},{99,0}};
    auto expected = original; expected[2] = authoritative[2];
    check(Restore(authoritative,*unlocked,original) == expected,"server-updated node is preserved on restore");
    auto bad = original; bad[1].name = bad[0].name;
    check(!Unlock(bad,names),"duplicate selected effects rejected");
    bad = original; bad[1].name = {999,0};
    check(!Unlock(bad,names),"unknown selected effect rejected");
    auto ambiguous = names; ambiguous[5] = names[0];
    check(!Unlock(original,ambiguous),"invalid effect catalog rejected");
    std::cout << "Awakening selection and restoration: " << (failed ? "FAILED" : "passed") << '\n';
    return failed ? 1 : 0;
}
