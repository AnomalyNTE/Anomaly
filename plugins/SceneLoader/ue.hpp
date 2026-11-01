#pragma once
uintptr_t Find(const Reader& r,const std::string& path) {
    auto& h=functions[path];const auto old=Resolve(r,objects,h);if(old) return old;
    if(Clock::now()<missing_functions[path]) return 0;
    if(objects->find_exact(objects->user,StringView(path),&h).code!=0) {h={};missing_functions[path]=Clock::now()+std::chrono::seconds(5);return 0;}
    const auto found=Resolve(r,objects,h);
    if(!found) missing_functions[path]=Clock::now()+std::chrono::seconds(5);
    return found;
}
uintptr_t Fn(const Reader& r,const char* package,const char* owner,const char* name,int count,int size) {
    const auto f=Find(r,std::string("/Script/")+package+"."+owner+"."+name);
    return f && r.Function(f,name,static_cast<uint8_t>(count),static_cast<uint16_t>(size))?f:0;
}
int Field(const Reader& r,uintptr_t object,const char* name,int size) {
    uintptr_t cls{};if(!r.Read(object+16,cls)) return -1;
    const auto key=std::to_string(cls)+":"+name+":"+std::to_string(size);
    const auto cached=offsets.find(key);if(cached!=offsets.end()) return cached->second;
    int found=-1;
    for(int depth=0;cls && depth<32;++depth) {
        uintptr_t p{};if(!r.Read(cls+112,p)) break;
        for(int i=0;p && i<2048;++i) {
            if(r.NameAt(p+32)==name) {
                int bytes{},offset{};
                if(r.Read(p+52,bytes) && bytes==size && r.Read(p+68,offset) && offset>=0 && offset<0x20000) found=offset;
                break;
            }
            if(!r.Read(p+72,p)) break;
        }
        if(found>=0 || !r.Read(cls+64,cls)) break;
    }
    offsets[key]=found;return found;
}
uintptr_t Child(const Reader& r,uintptr_t parent,const char* field) {
    const auto offset=Field(r,parent,field,8);uintptr_t value{};
    if(offset>=0) r.Read(parent+offset,value);return value;
}
AnomalyGenerationHandleV1 Handle(const Reader& r,uintptr_t address) {
    int index{};AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if(!r.Read(address+12,index) || index<0 || objects->snapshot_at(objects->user,index,&snapshot).code!=0 ||
       Resolve(r,objects,snapshot.handle)!=address) return {};
    return snapshot.handle;
}
uintptr_t World(const Reader& r) {
    if(!world_slot) {
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        uintptr_t instruction{};int32_t displacement{};
        if(!sig || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
           StringView("48 8B 1D ?? ?? ?? ?? 48 85 DB 74 ?? 41 B0 01"),&instruction).code!=0 || !r.Read(instruction+3,displacement)) return 0;
        world_slot=static_cast<uintptr_t>(static_cast<intptr_t>(instruction)+7+displacement);
    }
    uintptr_t value{};return r.Read(world_slot,value) && r.IsA(value,"World")?value:0;
}
