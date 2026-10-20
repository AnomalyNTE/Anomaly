#pragma once
struct BaitOption {
    FName shop{},goods{},item{},currency{};
    std::string key,label,currency_label;
    int64_t price{},minimum{1},maximum{99};
};
struct BaitSettings {bool buy{};uint32_t quantity{20};std::string goods;};
std::atomic<std::shared_ptr<const BaitSettings>> bait_settings;
std::atomic<std::shared_ptr<const std::vector<BaitOption>>> bait_catalog;
std::atomic_int64_t bait_stock{-1};
std::shared_ptr<const BaitSettings> last_bait_settings,saved_bait_settings;
fishing::Restock restock;
BaitOption pending_bait;
bool equip_pending{};
uint64_t equip_sent{};
uintptr_t text_to_string{};
Clock::time_point next_catalog{},next_stock{};
std::filesystem::path bait_file;
struct TableEntry {FName key;uintptr_t row;uint32_t hash,next;};
static_assert(sizeof(TableEntry)==24);
struct CatalogBuild {
    AnomalyGenerationHandleV1 table{};
    std::map<uint64_t,uintptr_t> baits;
    std::vector<TableEntry> goods;
    std::vector<BaitOption> found;
    size_t cursor{};
} catalog_build;
uint64_t NameKey(FName n) {return static_cast<uint64_t>(n.number)<<32 | n.id;}
bool SameName(FName a,FName b) {return a.id==b.id && a.number==b.number;}
bool SparseElements(const Reader& r,uintptr_t address,size_t stride,int limit,std::vector<uint8_t>& raw,std::vector<uint32_t>& flags) {
    HudMapHeader h{};
    if(!r.Read(address,h) || h.count<0 || h.count>limit || h.capacity<h.count || h.capacity>65536 ||
       h.bit_count<h.count || h.bit_count>65536 || h.bit_capacity<h.bit_count ||
       h.free_count<0 || h.free_count>h.count || (!h.extra_bits && h.count>128) || (h.count && !h.data)) return false;
    raw.resize(static_cast<size_t>(h.count)*stride);flags.resize((h.count+31)/32);
    if(h.count && (core->read_memory(core->user,h.data,{raw.data(),raw.size()}).code!=0 ||
       core->read_memory(core->user,h.extra_bits?h.extra_bits:address+16,
           {reinterpret_cast<uint8_t*>(flags.data()),flags.size()*4}).code!=0)) return false;
    return true;
}
bool TableRows(const Reader& r,uintptr_t table,const char* type,std::vector<TableEntry>& rows) {
    uintptr_t row_type{};
    if(!r.IsA(table,"DataTable") || !r.Read(table+0x28,row_type) || r.NameAt(row_type+24)!=type) return false;
    std::vector<uint8_t> raw;std::vector<uint32_t> flags;
    if(!SparseElements(r,table+0x30,24,8192,raw,flags)) return false;
    rows.clear();
    for(size_t i=0;i<raw.size()/24;++i) if(flags[i/32]&(1u<<(i%32))) {
        TableEntry row{};std::memcpy(&row,raw.data()+i*24,24);
        if(!row.key.id || !row.row) return false;rows.push_back(row);
    }
    return true;
}
uintptr_t InvokeTextString(uintptr_t function,uintptr_t text) {
    __try {return reinterpret_cast<uintptr_t(__fastcall*)(const void*)>(function)(reinterpret_cast<const void*>(text));}
    __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
std::string BaitText(const Reader& r,uintptr_t address) {
    char cached[512]{};size_t length=sizeof(cached);
    if(names->resolve_ftext_utf8 && names->resolve_ftext_utf8(names->user,address,cached,&length).code==0 && cached[0]) return cached;
    if(!text_to_string) {
        const auto sig=Host(api).Query<AnomalySignatureServiceV1>(ANOMALY_SIGNATURE_SERVICE_V1_ID).get();
        if(!sig || !sig->resolve || sig->resolve(sig->user,StringView("HTGame.exe"),StringView(".text"),
            StringView(fishing::profile::TextToString),&text_to_string).code!=0) return {};
    }
    // FText::ToString returns a borrowed FString reference; no returned allocation is owned here.
    const auto string=InvokeTextString(text_to_string,address);RowArray value{};
    if(!string || !r.Read(string,value) || !value.data || value.count<1 || value.count>512 || value.capacity<value.count) return {};
    std::vector<wchar_t> wide(value.count);
    if(core->read_memory(core->user,value.data,{reinterpret_cast<uint8_t*>(wide.data()),wide.size()*2}).code!=0 || wide.back()!=0) return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,wide.data(),value.count-1,nullptr,0,nullptr,nullptr);
    if(size<=0 || size>2048) return {};
    std::string text(size,'\0');WideCharToMultiByte(CP_UTF8,0,wide.data(),value.count-1,text.data(),size,nullptr,nullptr);return text;
}
void UpdateBaitCatalog(const Reader& r) {
    if(bait_catalog.load() || Clock::now()<next_catalog) return;
    const auto now=Clock::now();next_catalog=now+std::chrono::milliseconds(100);
    if(!catalog_build.table.id) {
        next_catalog=now+std::chrono::seconds(5);
        const auto cdo=Find(r,"/Script/HTGame.Default__HTGameData"),get=Fn(r,"HTGame","HTGameData","GetGameData",1,8);
        uintptr_t gd{};if(!cdo || !get || !Invoke(r,cdo,get,&gd) || !r.IsA(gd,"HTGameData")) return;
        const auto fish=Child(r,gd,"FishingDataAsset"),bait=Child(r,fish,"StaticFishBaitDataTable"),goods=Child(r,gd,"GoodsDataTable");
        uintptr_t row_type{};
        if(!goods || !r.Read(goods+0x28,row_type) || !r.Property(row_type,"OwnerShopID",0x28,8) ||
           !r.Property(row_type,"ItemID",0x30,8) || !r.Property(row_type,"ItemName",0x38,16) ||
           !r.Property(row_type,"CurrencyID",0x8C,8) || !r.Property(row_type,"Price",0x98,8)) return;
        // This asset stores bait rows as generic StaticItemData, with bait-specific
        // details in their extension payload. The property name is not its row type.
        uintptr_t bait_type{};
        if(!bait || !r.Read(bait+0x28,bait_type) || !r.Property(bait_type,"ItemName",0xA0,16) ||
           !r.Property(bait_type,"ItemType",8,1)) return;
        std::vector<TableEntry> baits,all;
        if(!TableRows(r,bait,"StaticItemData",baits) || !TableRows(r,goods,"GoodsData",all)) return;
        catalog_build={};catalog_build.table=Handle(r,goods);if(!catalog_build.table.id) return;
        catalog_build.goods=std::move(all);
        for(auto entry:baits) {uint8_t type{};if(r.Read(entry.row+8,type) && type==42) catalog_build.baits[NameKey(entry.key)]=entry.row;}
        next_catalog=now;
    }
    if(!Resolve(r,objects,catalog_build.table)) {catalog_build={};next_catalog=now+std::chrono::seconds(5);return;}
    // Build in small batches; never scan the global object or name pools every frame.
    int found_this_tick=0;
    for(size_t end=std::min(catalog_build.cursor+128,catalog_build.goods.size());catalog_build.cursor<end;) {
        const auto entry=catalog_build.goods[catalog_build.cursor++];FName item{};
        if(!r.Read(entry.row+0x30,item) || !catalog_build.baits.contains(NameKey(item))) continue;
        BaitOption option{};option.item=item;
        if(!r.Read(entry.row+0x28,option.shop) || !r.Read(entry.row+0x8C,option.currency) ||
           !r.Read(entry.row+0x98,option.price) || !r.Read(entry.row+0x58,option.minimum) ||
           !r.Read(entry.row+0x60,option.maximum) || !option.shop.id || !option.currency.id || option.price<=0) continue;
        int64_t selector{};r.Read(entry.row+0x68,selector);
        option.maximum=selector>0?selector:999;option.minimum=std::max<int64_t>(1,option.minimum);
        option.maximum=std::clamp<int64_t>(option.maximum,option.minimum,9999);
        option.goods=entry.key;option.key=r.Name(entry.key);option.label=BaitText(r,catalog_build.baits.at(NameKey(item))+0xA0);
        if(option.label.empty()) option.label=BaitText(r,entry.row+0x38);
        if(option.label.empty()) option.label=r.Name(item);
        const auto currency=r.Name(option.currency);
        // Only normal fishing currencies are offered, not charge or unrelated shop goods.
        if(currency!="Fons" && currency!="FishingCoin") continue;
        option.currency_label=currency=="Fons"?"方斯":"钓鱼币";
        option.label+="（"+std::to_string(option.price)+" "+option.currency_label+"/个）";
        catalog_build.found.push_back(std::move(option));if(++found_this_tick>=4) break;
    }
    if(catalog_build.cursor>=catalog_build.goods.size()) {
        if(catalog_build.found.empty()) {catalog_build={};next_catalog=now+std::chrono::seconds(5);return;}
        std::sort(catalog_build.found.begin(),catalog_build.found.end(),[](const auto& a,const auto& b) {return a.key<b.key;});
        Log("autofish bait catalog="+std::to_string(catalog_build.found.size()));
        bait_catalog.store(std::make_shared<const std::vector<BaitOption>>(std::move(catalog_build.found)));catalog_build={};
    }
}
bool ItemAmount(const Reader& r,uintptr_t inventory,FName item,int64_t& amount) {
    const auto get=Fn(r,"HTGame","HTInventoryComponent","GetItemTotalAmount",3,24);
    struct Query {FName item;uint8_t bind{1};uint8_t padding[7]{};int64_t amount{-1};} query{item};
    if(!get || !r.IsA(inventory,"HTInventoryComponent") || !Invoke(r,inventory,get,&query) || query.amount<0) return false;
    amount=query.amount;return true;
}
uintptr_t InventoryBait(const Reader& r,uintptr_t inventory,uintptr_t util,FName item) {
    const auto get=Fn(r,"HTGame","HTUtil","GetInventoryContainerTypeByItemID",2,9);
    struct Query {FName item;uint8_t type{255};} query{item};
    const auto offset=Field(r,inventory,"InventoryContainerMap",80);
    const auto structure=Find(r,"/Script/HTGame.InventoryData");
    if(!get || !structure || !r.Property(structure,"InventoryItemsMap",0,80) ||
       !Invoke(r,util,get,&query) || query.type>=28 || offset<0) return 0;
    std::vector<uint8_t> raw;std::vector<uint32_t> flags;
    if(!SparseElements(r,inventory+offset,176,64,raw,flags)) return 0;
    for(size_t i=0;i<raw.size()/176;++i) if((flags[i/32]&(1u<<(i%32))) && raw[i*176]==query.type) {
        // The container owns its map; use its current address, not a copied header's bitmap.
        HudMapHeader outer{};if(!r.Read(inventory+offset,outer)) return 0;
        std::vector<uint8_t> items;std::vector<uint32_t> item_flags;
        if(!SparseElements(r,outer.data+i*176+8,24,8192,items,item_flags)) return 0;
        for(size_t j=0;j<items.size()/24;++j) if(item_flags[j/32]&(1u<<(j%32))) {
            uintptr_t object{};std::memcpy(&object,items.data()+j*24+8,8);
            if(!object) continue;
            const auto id=Field(r,object,"ItemID",8),count=Field(r,object,"Amount",8);FName candidate{};int64_t stock{};
            if(id>=0 && count>=0 && r.Read(object+id,candidate) && SameName(item,candidate) &&
               r.Read(object+count,stock) && stock>0 && r.IsA(object,"HTItem")) return object;
        }
    }
    return 0;
}
struct BaitSettingsFile {uint32_t magic{0x41464253},version{1},quantity{20},buy{};char goods[128]{};};
void LoadBaitSettings() {
    auto config=std::make_shared<BaitSettings>();bait_file=point_file.empty()?std::filesystem::path{}:point_file.parent_path()/"bait-settings-v1.bin";
    if(!bait_file.empty()) {
        BaitSettingsFile saved{};std::ifstream in(bait_file,std::ios::binary);
        if(in.read(reinterpret_cast<char*>(&saved),sizeof(saved)) && saved.magic==0x41464253 && saved.version==1 &&
           saved.quantity>=1 && saved.quantity<=9999 && saved.buy<=1 && std::memchr(saved.goods,0,sizeof(saved.goods))) {
            config->buy=saved.buy!=0;config->quantity=saved.quantity;config->goods=saved.goods;
        }
    }
    bait_settings.store(config);last_bait_settings=saved_bait_settings=config;
}
void SyncBaitSettings() {
    const auto config=bait_settings.load();if(!config) return;
    if(config!=last_bait_settings) {
        if(!restock.pending) equip_pending=false;
        restock.Retry();if(restock.failed==false) {paused=false;flow.Reset();}
        last_bait_settings=config;next_stock={};
    }
    if(config==saved_bait_settings || bait_file.empty()) return;
    BaitSettingsFile saved{};saved.quantity=config->quantity;saved.buy=config->buy;
    if(config->goods.size()>=sizeof(saved.goods)) return;
    std::memcpy(saved.goods,config->goods.data(),config->goods.size());
    if(persisted_files.Store(bait_file,&saved,sizeof(saved))) saved_bait_settings=config;
}
const BaitOption* SelectedBait(const BaitSettings& config,const std::vector<BaitOption>& list) {
    for(const auto& option:list) if(option.key==config.goods) return &option;
    return nullptr;
}
void RefreshBaitStock(const Reader& r,uintptr_t ps) {
    if(Clock::now()<next_stock) return;next_stock=Clock::now()+std::chrono::seconds(1);
    const auto config=bait_settings.load();
    const auto list=bait_catalog.load();const auto option=config && list?SelectedBait(*config,*list):nullptr;
    int64_t count{};
    if(option && ItemAmount(r,Child(r,ps,"InventoryComponent"),option->item,count)) bait_stock=count;
}
bool EnsureBait(const Reader& r,uintptr_t ps,uintptr_t util,uint64_t now) {
    const auto config=bait_settings.load();if(!config || !config->buy) return true;
    const auto get=Fn(r,"HTGame","HTPlayerState","GetEquipedFishBait",1,8);uintptr_t equipped{};
    if(!get || !Invoke(r,ps,get,&equipped)) {Publish("正在读取已装备鱼饵。");return false;}
    int64_t available{};
    const auto amount_offset=equipped?Field(r,equipped,"Amount",8):-1;
    if(amount_offset>=0 && r.Read(equipped+amount_offset,available) && available>0) {
        equip_pending=false;restock.Retry();return true;
    }
    const auto list=bait_catalog.load();const auto option=list?SelectedBait(*config,*list):nullptr;
    if(!option) {Publish(list?"请先选择补购鱼饵。":"正在读取鱼饵商店。");return false;}
    const auto inventory=Child(r,ps,"InventoryComponent");
    const auto& current=restock.pending?pending_bait:*option;
    int64_t stock{};if(!ItemAmount(r,inventory,current.item,stock)) {Publish("正在读取鱼饵库存。");return false;}
    bait_stock=stock;
    if(restock.pending) {
        if(!restock.Received(stock)) {
            if(restock.Expired(now)) {paused=true;Publish("鱼饵补购未到账，已暂停。请检查余额或购买限制，再重新开启。");}
            else Publish("正在补购鱼饵，等待到账。");
            return false;
        }
        Log("autofish bait received stock="+std::to_string(stock));next_stock={};
        if(!SameName(current.item,option->item)) return false;
    }
    if(stock==0) {
        if(restock.failed) {paused=true;Publish("鱼饵补购已暂停，请关闭再开启自动钓鱼重试。");return false;}
        if(config->quantity<option->minimum || config->quantity>option->maximum) {paused=true;Publish("购买数量超出该鱼饵可选范围。");return false;}
        int64_t cost{},balance{};
        if(!fishing::PurchaseCost(option->price,config->quantity,cost) || !ItemAmount(r,inventory,option->currency,balance)) {Publish("正在读取购买价格和余额。");return false;}
        if(balance<cost) {paused=true;restock.failed=true;Publish(option->currency_label+"不足，已暂停自动钓鱼。");return false;}
        const auto buy=Fn(r,"HTGame","HTPlayerState","ServerShopBuyGoods",5,48);
        if(!buy) {paused=true;Publish("鱼饵购买接口暂不可用。");return false;}
        struct Purchase {FName shop,goods;int64_t amount;uint8_t popup{};uint8_t padding[3]{};uint8_t city[20]{};};
        static_assert(sizeof(Purchase)==48);
        Purchase request{option->shop,option->goods,config->quantity};pending_bait=*option;
        if(!restock.Begin(now,stock) || !Invoke(r,ps,buy,&request)) {restock.pending=false;restock.failed=true;paused=true;Publish("鱼饵补购失败，已暂停。");return false;}
        Log("autofish bait buy goods="+option->key+" quantity="+std::to_string(config->quantity));
        Publish("正在补购鱼饵，等待到账。");return false;
    }
    const auto id=equipped?Field(r,equipped,"ItemID",8):-1;FName equipped_id{};
    if(id>=0 && r.Read(equipped+id,equipped_id) && SameName(equipped_id,option->item)) {equip_pending=false;restock.Retry();return true;}
    if(equip_pending) {
        if(now-equip_sent>=10000) {paused=true;Publish("鱼饵未能装备，已暂停。请检查所选鱼饵是否适用。");}
        else Publish("正在装备所选鱼饵。");
        return false;
    }
    const auto item=InventoryBait(r,inventory,util,option->item),equip=Fn(r,"HTGame","HTPlayerState","ServerEquipFishBait",1,8);
    uint64_t unique{};const auto unique_offset=item?Field(r,item,"UniqueID",8):-1;
    if(!item || unique_offset<0 || !r.Read(item+unique_offset,unique) || !unique || !equip) {Publish("正在等待鱼饵库存更新。");return false;}
    equip_pending=true;equip_sent=now;
    if(!Invoke(r,ps,equip,&unique)) {equip_pending=false;paused=true;Publish("鱼饵装备失败，已暂停。");return false;}
    Log("autofish bait equip item="+r.Name(option->item));Publish("正在装备所选鱼饵。");return false;
}
void BaitContent(const AnomalyUiServiceV1* ui) {
    auto config=bait_settings.load();if(!config) return;
    auto changed=std::make_shared<BaitSettings>(*config);bool dirty=false;
    int buy=config->buy;
    if(ui->checkbox(ui->user,StringView("鱼饵用完自动购买"),&buy)) {changed->buy=buy!=0;dirty=true;}
    const auto list=bait_catalog.load();const auto option=list?SelectedBait(*config,*list):nullptr;
    ui->text(ui->user,StringView(option?"所选："+option->label:"请选择补购鱼饵。"));
    if(ui->begin_menu && ui->end_menu && ui->button && ui->begin_menu(ui->user,StringView("选择鱼饵"),list && !list->empty())) {
        for(const auto& entry:*list) if(ui->button(ui->user,StringView(entry.label+"##"+entry.key),0,0)) {
            changed->goods=entry.key;changed->quantity=static_cast<uint32_t>(std::clamp<int64_t>(changed->quantity,entry.minimum,entry.maximum));dirty=true;
        }
        ui->end_menu(ui->user);
    }
    uint32_t quantity=config->quantity;
    if(ui->input_uint32 && ui->input_uint32(ui->user,StringView("每次购买数量"),&quantity,1,10)) {
        changed->quantity=std::clamp<uint32_t>(quantity,option?static_cast<uint32_t>(option->minimum):1,option?static_cast<uint32_t>(option->maximum):9999);dirty=true;
    }
    int64_t total{};
    if(option && fishing::PurchaseCost(option->price,changed->quantity,total)) ui->text(ui->user,StringView("每次花费："+std::to_string(total)+" "+option->currency_label));
    if(bait_stock.load()>=0 && option) ui->text(ui->user,StringView("所选鱼饵库存："+std::to_string(bait_stock.load())));
    if(buy) ui->text(ui->user,StringView("鱼饵用完后补购所选鱼饵，并自动装备。"));
    if(dirty) {bait_stock=-1;bait_settings.store(changed);}
}
