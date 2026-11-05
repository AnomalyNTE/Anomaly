#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace claw {
struct Name { std::uint32_t id{}, number{}; friend bool operator==(Name, Name) = default; };
inline std::uint64_t Pack(Name n) { return std::uint64_t{n.id} | (std::uint64_t{n.number} << 32); }
struct Array { std::uintptr_t data{}; std::int32_t count{}, capacity{}; };
struct Wrapper {
    std::uint8_t type{}, padding[3]{};
    Name custom_type{};
    std::uint32_t align{};
    Array bytes{};
};
struct WrapperArray { Array items{}; std::int32_t cursor{}, padding{}; };
struct Rpc { std::int32_t message{}, padding{}; WrapperArray parameters{}; };
static_assert(sizeof(Name)==8 && sizeof(Array)==16 && sizeof(Wrapper)==32 && sizeof(WrapperArray)==24 && sizeof(Rpc)==32);
static_assert(offsetof(Wrapper,custom_type)==4 && offsetof(Wrapper,align)==12 && offsetof(Wrapper,bytes)==16 &&
    offsetof(WrapperArray,cursor)==16 && offsetof(Rpc,parameters)==8);

// The game's Name wrapper serializes FName::ToString with FArchive's FString
// format, not a process-local name-pool index. Row identifiers are ASCII.
inline std::vector<std::uint8_t> EncodeName(std::string_view name) {
    if (name.empty() || name.size()>255) return {};
    for (const auto c : name) if (static_cast<unsigned char>(c)<32 || static_cast<unsigned char>(c)>126) return {};
    std::vector<std::uint8_t> data(5+name.size());
    const auto count=static_cast<std::int32_t>(name.size()+1);
    std::memcpy(data.data(), &count, 4);
    std::memcpy(data.data()+4, name.data(), name.size());
    return data;
}
inline std::optional<std::string> DecodeName(std::span<const std::uint8_t> bytes) {
    if (bytes.size()<5 || bytes.size()>516) return {};
    std::int32_t count{}; std::memcpy(&count, bytes.data(), 4);
    std::string result;
    if (count>1 && count<=256 && bytes.size()==4+static_cast<std::size_t>(count) && bytes.back()==0) {
        for (std::int32_t i=0;i<count-1;++i) {
            const auto c=bytes[4+i]; if (c<32 || c>126) return {}; result.push_back(static_cast<char>(c));
        }
    } else if (count<-1 && count>=-256 && bytes.size()==4+2*static_cast<std::size_t>(-count) &&
               bytes[bytes.size()-1]==0 && bytes[bytes.size()-2]==0) {
        for (std::int32_t i=0;i<-count-1;++i) {
            const auto c=bytes[4+2*i]; if (c<32 || c>126 || bytes[5+2*i]) return {}; result.push_back(static_cast<char>(c));
        }
    } else return {};
    return result;
}
struct Request {
    std::array<std::vector<std::uint8_t>,2> storage;
    std::array<Wrapper,2> wrappers{};
    Rpc rpc{1};
    Request(std::string_view machine, std::string_view doll) : storage{EncodeName(machine),EncodeName(doll)} {
        for (std::size_t i=0;i<2;++i) {
            wrappers[i].type=15;
            wrappers[i].bytes={reinterpret_cast<std::uintptr_t>(storage[i].data()),static_cast<std::int32_t>(storage[i].size()),static_cast<std::int32_t>(storage[i].size())};
        }
        rpc.parameters.items={reinterpret_cast<std::uintptr_t>(wrappers.data()),2,2};
    }
    Request(const Request&)=delete;
    Request& operator=(const Request&)=delete;
    bool Valid() const { return !storage[0].empty() && !storage[1].empty(); }
};
struct Identity {
    // UObject identities plus the independent world-service epoch. Handles
    // from anomaly.ue5.world are opaque and cannot be resolved as UObjects.
    std::array<std::uint64_t,8> handles{};
    friend bool operator==(const Identity&,const Identity&)=default;
    bool Valid() const { for(const auto v:handles) if(!v)return false; return true; }
};
enum class State { NeedCoin, WaitingBegin, Ready, Used, Pending, Success, Rejected, Unconfirmed };
struct Round {
    Identity identity{};
    State state{State::NeedCoin};
    std::uint64_t sequence{}, deadline{};
    std::string target, item;
    void Reset() { *this={}; }
    void BeginRequest(const Identity& id,std::uint64_t now) {
        // A second click while the first paid request is still pending cannot
        // create a second local round or make an already-spent round reusable.
        if(!id.Valid() || state==State::WaitingBegin || state==State::Pending) return;
        identity=id; state=State::WaitingBegin; deadline=now+15000; target.clear(); item.clear();
    }
    void BeginResponse(const Identity& id,std::uint64_t now) {
        if(state==State::WaitingBegin && id==identity && now<deadline) { state=State::Ready; ++sequence; }
    }
    bool CanSubmit(const Identity& id,std::uint64_t round) const { return state==State::Ready && id==identity && round==sequence; }
    bool Submit(const Identity& id,std::uint64_t round,std::string selected,std::uint64_t now) {
        if(!CanSubmit(id,round) || selected.empty())return false;
        target=std::move(selected); state=State::Pending; deadline=now+15000; return true;
    }
    void ManualCapture() { if(state==State::Ready) state=State::Used; }
    void Result(const Identity& id,bool rejected,std::string_view doll,std::string_view reward) {
        if(state!=State::Pending || id!=identity)return;
        if(rejected) {state=State::Rejected;return;}
        if(doll==target && !reward.empty() && reward!="None") { item=reward; state=State::Success; }
        else state=State::Unconfirmed;
    }
    void Tick(std::uint64_t now) {
        if((state==State::WaitingBegin || state==State::Pending) && now>=deadline)state=State::Unconfirmed;
    }
};
}
