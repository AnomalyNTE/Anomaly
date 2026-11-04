#include "anomaly/symbol_resolver.hpp"
#include "anomaly/sdk/profile.hpp"
#include "anomaly/signature_updater.hpp"
#include "pattern.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Image final : public anomaly::SymbolMemory {
public:
    std::vector<std::uint8_t> bytes;
    std::optional<ue5mem::ModuleInfo> FindModule(std::wstring_view) const override {
        ue5mem::ModuleInfo result{};
        result.base = 0x10000;
        result.size = bytes.size();
        return result;
    }
    std::vector<ue5mem::SectionInfo> Sections(const ue5mem::ModuleInfo&) const override { return {}; }
    std::vector<std::uintptr_t> Scan(const ue5mem::ModuleInfo&, std::string_view,
                                    std::string_view pattern, std::size_t limit) const override {
        std::vector<std::uintptr_t> result;
        for (const auto offset : ue5mem::Pattern::Parse(pattern).FindAll(bytes, limit))
            result.push_back(0x10000 + offset);
        return result;
    }
    bool Read(std::uintptr_t address, void* out, std::size_t size) const override {
        if (address < 0x10000 || address - 0x10000 > bytes.size() ||
            size > bytes.size() - (address - 0x10000)) return false;
        std::memcpy(out, bytes.data() + address - 0x10000, size);
        return true;
    }
    std::optional<anomaly::SymbolMemoryRegion> Query(std::uintptr_t) const override { return {}; }
};

anomaly::SignatureScanContext Context(Image& image) {
    anomaly::SignatureScanContext context;
    context.module={L"fixture.exe",L"",0x10000,image.bytes.size()};
    context.section={".text",0x10000,image.bytes.size(),IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ};
    context.read=[&image](auto address,void* out,auto size){ return image.Read(address,out,size); };
    context.scan=[&image](auto pattern,auto limit){ return image.Scan({},".text",pattern,limit); };
    return context;
}

void CheckCompactRecovery() {
    Image image;
    image.bytes.resize(256,0xCC);
    // Independent x64 function: long register-save prologue followed by a leaf
    // body. A future body edit must not invalidate a unique unchanged prologue.
    const std::vector<std::uint8_t> body{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,
        0x48,0x89,0x74,0x24,0x18,0x48,0x89,0x7C,0x24,0x20,
        0x41,0x56,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF2,0x48,0x8B,0xF9,
        0xB8,0x11,0,0,0,0xC3};
    std::copy(body.begin(),body.end(),image.bytes.begin());
    auto context=Context(image);
    std::size_t scans{};
    const auto scan = context.scan;
    context.scan = [&](auto pattern, auto limit) {
        ++scans;
        return scan(pattern, limit);
    };
    anomaly::SignatureUpdater updater({});
    const std::string declaration="48 89 5C 24 08 48 89 6C 24 10";
    Require(updater.Learn(context,"compact",declaration,0x10000,{},true).empty(),"short declaration may learn a unique longer window");
    Require(scans == 0, "uniquely matched declaration should not rescan its stronger evidence");
    const auto initial_scans = scans;
    Require(updater.Learn(context,"compact",declaration,0x10000,{},true).empty() && scans == initial_scans,
        "unchanged cached evidence should not rescan the section");
    image.bytes[33]=0x22;
    Require(updater.Recover(context,"compact",declaration).instruction==0x10000,"unrelated body edit must preserve learned prefix");
    std::copy_n(image.bytes.begin(),body.size(),image.bytes.begin()+128);
    Require(updater.Recover(context,"compact",declaration).instruction==0,"duplicated compact prefix must remain ambiguous");
    image.bytes[33]=0x11;
    Require(updater.Recover(context,"compact",declaration).instruction==0x10000,"full learned evidence must separate a newly duplicated prefix");
    std::stop_source cancelled; cancelled.request_stop();
    Require(updater.Recover(context,"compact",declaration,cancelled.get_token()).instruction==0,"cancelled recovery must publish nothing");
    image.bytes.assign(256,0xCC);
    std::copy_n(body.begin(),32,image.bytes.begin());
    const std::uint8_t member[]{0x8B,0x81,0x20,0x01,0,0,0xC3};
    std::copy(std::begin(member),std::end(member),image.bytes.begin()+32);
    const std::string with_member="48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8B F2 48 8B F9 8B 81 20 01 00 00 C3";
    Require(updater.Learn(context,"member-preserved",with_member,0x10000).empty(),"declared member function must learn");
    image.bytes[34]=0x38;
    Require(updater.Recover(context,"member-preserved",with_member).instruction==0,"compaction must not omit a declared member displacement");
    image.bytes.assign(256,0xCC);
    std::copy_n(std::begin(member),6,image.bytes.begin());
    std::copy(body.begin(),body.end(),image.bytes.begin()+6);
    const std::string wildcard="8B 81 ?? ?? ?? ??";
    Require(updater.Learn(context,"declared-wildcard",wildcard,0x10000).empty(),"wildcard declaration must learn surrounding evidence");
    image.bytes[2]=0x38;
    Require(updater.Recover(context,"declared-wildcard",wildcard).instruction==0x10000,"learning must preserve declared operand wildcards");
    image.bytes[1]=0x82;
    Require(updater.Recover(context,"declared-wildcard",wildcard).instruction==0,"operand wildcards must not weaken the instruction identity");
}

void CheckScalarMembers() {
    Image image;
    image.bytes.resize(512,0xCC);
    // MOV RBX,RCX; MOVSS XMM1,[RBX+0x120]; MOVSS [RBX+0x120],XMM1.
    // The XMM1 write must not destroy the RCX object alias. Both directions
    // independently name the same 4-byte field.
    const std::vector<std::uint8_t> body{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xD9,
        0xF3,0x0F,0x10,0x8B,0x20,0x01,0,0,0xF3,0x0F,0x11,0x8B,0x20,0x01,0,0,
        0x48,0x8B,0xCB,0x48,0x83,0xC4,0x20,0x5F,0xC3};
    std::copy(body.begin(),body.end(),image.bytes.begin());
    auto context=Context(image);
    anomaly::SignatureLayoutDefinition definition{
        {{"speed",0x120,4}},
        {{"scalar","48 89 5C 24 08 57 48 83 EC 20 48 8B D9 F3 0F 10 8B 20 01 00 00 F3 0F 11 8B 20 01 00 00 48 8B CB 48 83 C4 20 5F C3",{{0,0,4,3}}}}};
    anomaly::SignatureUpdater updater({});
    const auto seed=updater.ResolveLayout(context,definition);
    Require(seed.Ok() && seed.offsets[0]==0x120,"scalar reads/writes must supply field evidence");
    image.bytes[17]=image.bytes[25]=0x38;
    const auto moved=updater.ResolveLayout(context,definition);
    Require(moved.Ok() && moved.offsets[0]==0x138,"scalar displacement drift must recover the actual field");
    image.bytes[25]=0x40;
    Require(!updater.ResolveLayout(context,definition).Ok(),"disagreeing reads/writes cannot publish a field");
    image.bytes[25]=0x38; image.bytes[13]=0xF2;
    Require(!updater.ResolveLayout(context,definition).Ok(),"float-to-double width change must be rejected");
    image.bytes[13]=0xF3;
    std::copy_n(image.bytes.begin(),body.size(),image.bytes.begin()+256);
    Require(!updater.ResolveLayout(context,definition).Ok(),"ambiguous member function must fail closed");
}

void CheckCalleeRecovery(bool conditional = false) {
    Image image;
    image.bytes.resize(2048, 0xCC);
    // Two wrapper bodies have identical opcodes but call different functions.
    std::vector<std::uint8_t> wrapper{
        0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x20,
        0x48,0x8B,0xD9,0x48,0x8B,0xFA,0xE8,0,0,0,0,
        0x48,0x8B,0xCB,0x48,0x8B,0xD7,0x48,0x83,0xC4,0x20,0x5F,0xC3};
    if (conditional) { wrapper.insert(wrapper.begin()+16,0x0F); wrapper[17]=0x84; }
    const auto install = [&](std::size_t at, std::size_t target) {
        std::copy(wrapper.begin(), wrapper.end(), image.bytes.begin()+at);
        const auto relative=static_cast<std::int32_t>(target-at-21-static_cast<int>(conditional));
        std::memcpy(image.bytes.data()+at+17+static_cast<int>(conditional),&relative,sizeof(relative));
    };
    install(0,512); install(256,768);
    const std::vector<std::uint8_t> callee{0x48,0x89,0x5C,0x24,0x08,0x48,0x83,0xEC,0x20,0x48,0x8B,0xC1,0xC3};
    std::copy(callee.begin(),callee.end(),image.bytes.begin()+512);
    std::copy(callee.begin(),callee.end(),image.bytes.begin()+768);
    image.bytes[768+11]=0xC2;
    anomaly::SignatureScanContext context;
    context.module={L"fixture.exe",L"",0x10000,image.bytes.size()};
    context.section={".text",0x10000,image.bytes.size(),IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ};
    context.read=[&](auto address,void* out,auto size){ return image.Read(address,out,size); };
    context.scan=[&](auto pattern,auto limit){ return image.Scan(context.module,".text",pattern,limit); };
    const auto directory=std::filesystem::temp_directory_path()/
        ("anomaly-callee-fixture-"+std::to_string(GetCurrentProcessId()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path,error); } } cleanup{directory};
    const std::string pattern=std::string("48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA ")+
        (conditional ? "0F 84" : "E8")+" ?? ?? ?? ?? 48 8B CB 48 8B D7 48 83 C4 20 5F C3";
    {
        anomaly::SignatureUpdater updater(directory);
        Require(updater.Learn(context,"fixture",pattern,0x10000).empty(),"callee evidence should distinguish wrappers");
    }
    anomaly::SignatureUpdater updater(directory);
    Require(updater.Recover(context,"fixture",pattern).instruction==0x10000,"persisted callee evidence must select original wrapper");
    // A sole remaining wrapper with the wrong callee must still be rejected.
    image.bytes[0]=0xCC;
    Require(updater.Recover(context,"fixture",pattern).instruction==0,"one wrong callee cannot bypass learned evidence");
    install(0,512);
    image.bytes[512+11]=0xC2;
    Require(updater.Recover(context,"fixture",pattern).instruction==0,"changed callees must invalidate the recipe");
    image.bytes[512+11]=0xC1;
    const auto real_scan=context.scan;
    context.scan=[&](auto pat,auto limit) {
        auto matches=real_scan(pat,limit);
        if (limit>=256) matches.resize(limit,0x10100);
        return matches;
    };
    Require(updater.Recover(context,"fixture",pattern).instruction==0,"truncated candidate enumeration cannot prove uniqueness");
    Require(!updater.Learn(context,"limit",pattern,0x10000).empty(),"learning must also reject truncated enumeration");
    context.scan=real_scan;
    // Persisted anchor offsets are untrusted input: overflow and an interior
    // instruction byte cannot be used as a new call-site boundary.
    nlohmann::json cache;
    { std::ifstream input(directory/"signatures.json"); input>>cache; }
    for (auto bad : {std::uint64_t{UINT64_MAX-3},std::uint64_t{17}}) {
        auto invalid=cache;
        invalid["records"][0]["anchors"][0]["instruction"]=bad;
        { std::ofstream output(directory/"signatures.json"); output<<invalid; }
        anomaly::SignatureUpdater reloaded(directory);
        Require(reloaded.Recover(context,"fixture",pattern).instruction==0,"invalid persisted anchor must not recover an address");
    }
}
}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "expected repository profile path");
        auto loaded = anomaly::LoadBuildProfile(argv[1]);
        if (!loaded.Ok()) {
            for (const auto& diagnostic : loaded.diagnostics) std::cerr << diagnostic.path << ": " << diagnostic.message << '\n';
        }
        Require(loaded.Ok(), "bundled profile must pass the real host schema");
        Require(loaded.profile->layout_candidates.size() == 2, "expected both supported layouts");
        Require(!loaded.profile->layout.contains("playerCharacter.currentDisplayCharacterId"),
                "unselected layouts must not publish fabricated zero offsets");
        CheckCalleeRecovery();
        CheckCalleeRecovery(true);
        CheckCompactRecovery();
        CheckScalarMembers();

        // Native byte fixtures, independently observed in 1.4 PID 44236 and the
        // 1.5 IDA image. The appearance flag displacement selects the corresponding character layout.
        Image image;
        image.bytes = {0x80,0xB9,0x50,0x2A,0,0,0};
        anomaly::BuildProfile recipe;
        recipe.layout["unchanged"] = 16;
        anomaly::ProfileLayoutCandidate first;
        first.module = L"HTGame.exe"; first.section = ".text";
        first.pattern = "80 B9 50 2A 00 00 00";
        first.entry = {0x80,0xB9};
        first.values["display"] = 10724;
        auto second = first;
        second.pattern = "80 B9 48 2B 00 00 00";
        second.values["display"] = 10956;
        recipe.layout_candidates = {first, second};
        std::string error;
        auto selected = recipe;
        Require(anomaly::SelectProfileLayout(selected, image, error), "first candidate should resolve");
        Require(selected.layout.at("display") == 10724 && selected.layout.at("unchanged") == 16,
                "selected candidate must merge only its own fields");
        image.bytes[2] = 0x48; image.bytes[3] = 0x2B;
        selected = recipe;
        Require(anomaly::SelectProfileLayout(selected, image, error) && selected.layout.at("display") == 10956,
                "second native displacement must select second layout");
        image.bytes.insert(image.bytes.end(), {0x80,0xB9,0x50,0x2A,0,0,0});
        selected = recipe;
        Require(!anomaly::SelectProfileLayout(selected, image, error) && selected.layout == recipe.layout,
                "mixed candidate image must fail without publishing partial values");
        image.bytes = {0x90,0xC3};
        Require(!anomaly::SelectProfileLayout(selected, image, error), "unknown image must not get fallback layout");
        image.bytes = {0x80,0xB9,0x50,0x2A,0,0,0};
        selected = recipe; selected.layout_candidates[0].entry = {0xCC};
        Require(!anomaly::SelectProfileLayout(selected, image, error), "entry mismatch must reject signature match");
        image.bytes.insert(image.bytes.end(), {0x80,0xB9,0x50,0x2A,0,0,0});
        selected = recipe;
        Require(!anomaly::SelectProfileLayout(selected, image, error), "duplicate signature must be rejected");
        std::cout << "PASS: profile schema, both layouts, missing/ambiguous/entry-mismatch rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
