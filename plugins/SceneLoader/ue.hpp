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
    uintptr_t cls{};if(!r.Read(object+scene_profile::native_layout.ObjectClass,cls)) return -1;
    const auto key=std::to_string(cls)+":"+name+":"+std::to_string(size);
    const auto cached=offsets.find(key);if(cached!=offsets.end()) return cached->second;
    int found=-1;
    for(int depth=0;cls && depth<32;++depth) {
        uintptr_t p{};if(!r.Read(cls+scene_profile::native_layout.StructProperties,p)) break;
        for(int i=0;p && i<2048;++i) {
            if(r.NameAt(p+scene_profile::native_layout.PropertyName)==name) {
                int bytes{},offset{};
                if(r.Read(p+scene_profile::native_layout.PropertySize,bytes) && bytes==size && r.Read(p+scene_profile::native_layout.PropertyOffset,offset) && offset>=0 && offset<0x20000) found=offset;
                break;
            }
            if(!r.Read(p+scene_profile::native_layout.PropertyNext,p)) break;
        }
        if(found>=0 || !r.Read(cls+scene_profile::native_layout.StructSuper,cls)) break;
    }
    offsets[key]=found;return found;
}
uintptr_t Child(const Reader& r,uintptr_t parent,const char* field) {
    const auto offset=Field(r,parent,field,8);uintptr_t value{};
    if(offset>=0) r.Read(parent+offset,value);return value;
}
AnomalyGenerationHandleV1 Handle(const Reader& r,uintptr_t address) {
    int index{};AnomalyUe5ObjectSnapshotV1 snapshot{sizeof(snapshot)};
    if(!r.Read(address+scene_profile::native_layout.ObjectIndex,index) || index<0 || objects->snapshot_at(objects->user,index,&snapshot).code!=0 ||
       Resolve(r,objects,snapshot.handle)!=address) return {};
    return snapshot.handle;
}
uintptr_t World(const Reader& r) {
    if(!world_slot) {
        const auto instruction=scene_profile::compatibility.Address("World");int32_t displacement{};
        if(!instruction || !r.Read(instruction+3,displacement)) return 0;
        world_slot=static_cast<uintptr_t>(static_cast<intptr_t>(instruction)+7+displacement);
    }
    uintptr_t value{};return r.Read(world_slot,value) && r.IsA(value,"World")?value:0;
}
