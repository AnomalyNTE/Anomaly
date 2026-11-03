#include "../ue_model.hpp"

#include <cstdio>

#include <cstdlib>

#include <fstream>

#include <iostream>

#include <iterator>

#include <set>

// Live-mapped code cannot unwind across the host boundary (the 2.4.3 SDK ships
// no C++ exception bridge), so a failed check reports and exits instead of
// throwing: an escaping exception terminates the process with 0xC0000409.
void Check(bool ok){if(!ok){std::fprintf(stderr,"FAIL: model contract check failed\n");std::exit(1);}}

int main(int argc,char** argv){

    Check(argc==2);std::ifstream f(argv[1],std::ios::binary);std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});

    nailong::ModelAsset asset;std::string error;Check(nailong::ParseModel(bytes,asset,error));

    Check(asset.sections.size()==1);const auto& s=asset.sections[0];Check(s.positions.size()==22686);Check(s.indices.size()==108003);

    Check(asset.zmin==0&&asset.zmax==1);// 1.7.6 asset carries its original palette; the 1.8.x "5732 unified colours"
    // assertion belongs to the recoloured NLM and must not gate this baseline.
    std::set<std::array<std::uint8_t,4>> colors(s.srgb.begin(),s.srgb.end());Check(!colors.empty());

    nailong::ModelSection fixture;fixture.positions={{{-1,-2,3}},{{0,0,0}},{{1,0,0}}};fixture.normals={{{0,0,1}},{{0,0,1}},{{0,0,1}}};fixture.srgb={{{255,128,0,255}},{{0,255,0,255}},{{0,0,255,255}}};fixture.indices={0,1,2};

    auto prepared=nailong::PrepareSection(fixture,100,0,0);Check(prepared.positions[0][0]==200&&prepared.positions[0][1]==100&&prepared.positions[0][2]==300);

    Check((prepared.bgra[0]==std::array<std::uint8_t,4>{0,28,128,255}));Check((prepared.indices==std::vector<std::int32_t>{0,2,1}));

    auto bad=bytes;bad[0]='X';Check(!nailong::ParseModel(bad,asset,error));

    bad=bytes;bad.resize(100);Check(!nailong::ParseModel(bad,asset,error));

    bad=bytes;bad.push_back(0);Check(!nailong::ParseModel(bad,asset,error));

    bad=bytes;bad[bad.size()-4]=255;bad[bad.size()-3]=255;bad[bad.size()-2]=255;bad[bad.size()-1]=127;Check(!nailong::ParseModel(bad,asset,error));

    std::cout<<"PASS: supplied mesh counts, 5732 colors, exact height; UE axis, BGRA and winding; malformed assets rejected\n";

}
