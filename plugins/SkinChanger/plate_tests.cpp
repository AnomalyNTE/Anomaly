#include "plate_animation.hpp"
#include <iostream>
int main() {
    using namespace plate_animation;
    int failed{};
    const auto check = [&](bool ok,const char* name) { if (!ok) { ++failed; std::cerr << name << '\n'; } };
    check(Normalize(L"b1ank",7) == std::optional<std::wstring>{L"B1ANK"},"lowercase uses available uppercase glyphs");
    check(Normalize(L"A-\u2665",7).has_value(),"known game glyphs accepted");
    check(!Normalize(L"12345678",7) && !Normalize(L"\u4e2d",7) && !Normalize(L"",7),"unsupported input rejected");
    check(Frame(L"ABC",7,0,.2,false) == L"ABC    ","static plate clears unused positions");
    check(Frame(L"ABC",7,0,.2,true) == L"ABC    ","animation starts with the full plate");
    check(Frame(L"ABC",7,.21,.2,true) == L"BC    A","leftmost glyph wraps to the right");
    check(Frame(L"ABC",7,.61,.2,true) == L"    ABC","short text rotates with its padding");
    check(Frame(L"ABC",7,1.41,.2,true) == L"ABC    ","cycle returns to its starting position");
    check(Frame(L"ABCDEFG",7,.21,.2,true) == L"BCDEFGA","full-width plate scrolls without disappearing");
    check(Frame(L"ABCDEFG",7,.41,.2,true) == L"CDEFGAB","continuous wrap preserves glyph order");
    check(Frame(L"ABC",7,.61,.1,true) == L" ABC   ","adjusting interval changes scroll speed");
    check(Frame(L"ABC",7,2.01,.2,false) == L"ABC    ","disabling animation shows the full plate");
    std::cout << "Plate glyph validation and scrolling loop: " << (failed ? "FAILED" : "passed") << '\n';
    return failed ? 1 : 0;
}
