// Runtime mesh contract: NLM4 v3, exported from the supplied 我是奶龙.blend.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>
namespace nailong {
struct ModelSection {
    std::vector<std::array<double,3>> positions, normals;
    std::vector<std::array<std::uint8_t,4>> srgb;
    std::vector<std::int32_t> indices;
    std::vector<std::array<std::uint16_t,4>> bone_indices;
    std::vector<std::array<float,4>> weights;
};
struct ModelAsset { std::vector<ModelSection> sections; double zmin=0, zmax=1; };
inline bool ParseModel(std::span<const std::uint8_t> bytes, ModelAsset& out, std::string& error) {
    auto fail=[&](const char* s){error=s; return false;};
    if(bytes.size()<40 || std::memcmp(bytes.data(),"NLM4",4)) return fail("NLM4 header missing");
    auto read=[&]<typename T>(std::size_t p) { T x{}; std::memcpy(&x,bytes.data()+p,sizeof(T)); return x; };
    if(read.template operator()<std::uint32_t>(4)!=3) return fail("unsupported NLM4 version");
    const auto ns=read.template operator()<std::uint32_t>(12);
    if(!ns || ns>8) return fail("section count invalid");
    ModelAsset parsed; parsed.zmin=read.template operator()<double>(24); parsed.zmax=read.template operator()<double>(32);
    if(!std::isfinite(parsed.zmin)||!std::isfinite(parsed.zmax)||parsed.zmax<=parsed.zmin) return fail("model height invalid");
    std::size_t off=40;
    for(std::uint32_t s=0;s<ns;++s) {
        if(off>bytes.size() || bytes.size()-off<16) return fail("truncated section header");
        const auto nv=read.template operator()<std::uint32_t>(off), nt=read.template operator()<std::uint32_t>(off+4); off+=16;
        const std::uint64_t need=std::uint64_t(nv)*76+std::uint64_t(nt)*12;
        if(!nv||!nt||nv>1000000||nt>2000000||need>bytes.size()-off) return fail("section payload invalid");
        ModelSection section; section.positions.resize(nv); section.normals.resize(nv); section.srgb.resize(nv); section.indices.resize(std::size_t(nt)*3);
        std::memcpy(section.positions.data(),bytes.data()+off,std::size_t(nv)*24);
        std::memcpy(section.normals.data(),bytes.data()+off+std::size_t(nv)*24,std::size_t(nv)*24);
        std::memcpy(section.srgb.data(),bytes.data()+off+std::size_t(nv)*48,std::size_t(nv)*4);
        std::memcpy(section.indices.data(),bytes.data()+off+std::size_t(nv)*76,std::size_t(nt)*12);
        section.bone_indices.resize(nv);section.weights.resize(nv);
        std::memcpy(section.bone_indices.data(),bytes.data()+off+std::size_t(nv)*52,std::size_t(nv)*8);
        std::memcpy(section.weights.data(),bytes.data()+off+std::size_t(nv)*60,std::size_t(nv)*16);
        for(std::size_t v=0;v<nv;++v) for(int a=0;a<3;++a)
            if(!std::isfinite(section.positions[v][a])||!std::isfinite(section.normals[v][a])) return fail("nonfinite vertex");
        for(auto i:section.indices) if(i<0||std::uint32_t(i)>=nv) return fail("index out of range");
        off+=std::size_t(need); parsed.sections.push_back(std::move(section));
    }
    if(off!=bytes.size()) return fail("unexpected model trailing bytes");
    out=std::move(parsed); error.clear(); return true;
}
struct EngineSection {
    std::vector<std::array<double,3>> positions, normals;
    std::vector<std::array<double,2>> uv;
    std::vector<std::array<std::uint8_t,4>> bgra;
    std::vector<std::array<double,4>> tangents;
    std::vector<std::int32_t> indices;
};
inline std::uint8_t LinearByte(std::uint8_t srgb) {
    const double c=double(srgb)/255;
    // Darken by 50% to compensate for VertexColorMaterial's lack of lighting
    return std::uint8_t(std::lround(255*0.5*(c<=0.04045?c/12.92:std::pow((c+0.055)/1.055,2.4))));
}
inline EngineSection PrepareSection(const ModelSection& src, double scale, double zmin, double yawDegrees) {
    EngineSection out; const auto n=src.positions.size(); out.positions.resize(n); out.normals.resize(n); out.uv.resize(n); out.bgra.resize(n); out.tangents.resize(n); out.indices=src.indices;
    const double a=yawDegrees*3.14159265358979323846/180, c=std::cos(a), s=std::sin(a);
    // Blender (-Y front, Z up) -> UE (+X front, Z up). Reflection changes winding.
    for(std::size_t i=0;i<n;++i) {
        const auto p=src.positions[i], q=src.normals[i];
        out.positions[i]={(-p[1]*c+p[0]*s)*scale,(-p[1]*s-p[0]*c)*scale,(p[2]-zmin)*scale};
        out.normals[i]={-q[1]*c+q[0]*s,-q[1]*s-q[0]*c,q[2]};
        out.tangents[i]={1.0,0.0,0.0,0.0};
        out.bgra[i]={LinearByte(src.srgb[i][2]),LinearByte(src.srgb[i][1]),LinearByte(src.srgb[i][0]),src.srgb[i][3]};
    }
    for(std::size_t i=0;i<out.indices.size();i+=3) std::swap(out.indices[i+1],out.indices[i+2]);
    return out;
}
} // namespace nailong
