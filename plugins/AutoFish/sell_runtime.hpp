#pragma once
struct SellSettings {bool sell{};uint32_t threshold{100};};
std::atomic<std::shared_ptr<const SellSettings>> sell_settings;
std::shared_ptr<const SellSettings> last_sell_settings,saved_sell_settings;
std::atomic_int64_t fish_total{-1},fish_locked{-1},sold_total{};
std::atomic<std::shared_ptr<const std::string>> sell_status;
Clock::time_point next_fish_count{};
fishing::SellPolicy sell_policy;
std::filesystem::path sell_file;
bool AutoSellEnabled() {const auto config=sell_settings.load();return config && config->sell;}
void PublishSell(std::string text) {sell_status.store(std::make_shared<const std::string>(std::move(text)));}
struct SellSettingsFile {uint32_t magic{0x41465353},version{1},threshold{100},sell{};};
void LoadSellSettings() {
    auto config=std::make_shared<SellSettings>();
    sell_file=point_file.empty()?std::filesystem::path{}:point_file.parent_path()/"sell-settings-v1.bin";
    if(!sell_file.empty()) {
        SellSettingsFile saved{};std::ifstream in(sell_file,std::ios::binary);
        if(in.read(reinterpret_cast<char*>(&saved),sizeof(saved)) && saved.magic==0x41465353 && saved.version==1 &&
           saved.threshold>=1 && saved.threshold<=100000 && saved.sell<=1) {
            config->sell=saved.sell!=0;config->threshold=saved.threshold;
        }
    }
    sell_settings.store(config);last_sell_settings=saved_sell_settings=config;
    sell_policy={};next_fish_count={};fish_total=fish_locked=-1;sold_total=0;
    PublishSell("达到阈值时一键卖鱼，锁定的鱼保留。");
}
void SyncSellSettings() {
    const auto config=sell_settings.load();if(!config) return;
    if(config!=last_sell_settings) {
        sell_policy.Retry();last_sell_settings=config;next_fish_count={};
        if(!sell_policy.pending) PublishSell("达到阈值时一键卖鱼，锁定的鱼保留。");
    }
    if(config==saved_sell_settings || sell_file.empty()) return;
    SellSettingsFile saved{};saved.threshold=config->threshold;saved.sell=config->sell;
    if(persisted_files.Store(sell_file,&saved,sizeof(saved))) saved_sell_settings=config;
}
bool FishCount(const Reader& r,uintptr_t ps,int64_t& total,int64_t& locked) {
    const auto inventory=Child(r,ps,"InventoryComponent");
    const auto count=Fn(r,"HTGame","HTInventoryComponent","GetCurTypeItemTotalAmount",2,16);
    const auto lock_count=Fn(r,"HTGame","HTInventoryComponent","GetLockedFishCount",1,8);
    struct Query {uint8_t type{41};uint8_t padding[7]{};int64_t count{-1};} query{};
    int64_t reserved{-1};
    if(!inventory || !r.IsA(inventory,"HTInventoryComponent") || !count || !lock_count ||
       !Invoke(r,inventory,count,&query) || !Invoke(r,inventory,lock_count,&reserved) ||
       query.count<0 || reserved<0 || reserved>query.count) return false;
    total=query.count;locked=reserved;return true;
}
// Return true only while an actual sale is being reconciled. A failure stops this
// feature's retries without stopping normal fishing or automatic bait restocking.
bool ServiceAutoSell(const Reader& r,uintptr_t ps,bool idle,uint64_t now) {
    const auto config=sell_settings.load();if(!config) return false;
    if(Clock::now()<next_fish_count) return sell_policy.pending;
    next_fish_count=Clock::now()+std::chrono::seconds(1);
    int64_t total{},locked{};
    if(!FishCount(r,ps,total,locked)) {
        if(sell_policy.pending && now-sell_policy.sent>=10000) {
            sell_policy.pending=false;sell_policy.failed=true;PublishSell("卖鱼库存暂不可读，自动卖鱼已暂停；关闭再开启可重试。");
        }
        return sell_policy.pending;
    }
    fish_total=total;fish_locked=locked;
    const auto action=sell_policy.Next(config->sell,config->threshold,total,locked,idle,now);
    if(action==fishing::SellAction::Received) {
        const auto removed=sell_policy.baseline-total;sold_total.fetch_add(removed);
        PublishSell("本轮已卖出 "+std::to_string(removed)+" 条鱼。");
        Log("autofish fish sale received before="+std::to_string(sell_policy.baseline)+" after="+std::to_string(total));
        return false;
    }
    if(action==fishing::SellAction::Timeout) {
        PublishSell("卖鱼未收到响应，关闭再开启自动卖鱼可重试。");return false;
    }
    if(action==fishing::SellAction::Wait) return true;
    if(action!=fishing::SellAction::Send) return false;
    const auto sell=Fn(r,"HTGame","HTPlayerState","ServerOneKeySellFish",0,0);
    if(!sell) {sell_policy.failed=true;PublishSell("当前卖鱼接口暂不可用。");return false;}
    sell_policy.Begin(total,now);
    if(!Invoke(r,ps,sell,nullptr)) {sell_policy.pending=false;sell_policy.failed=true;PublishSell("卖鱼请求失败，关闭再开启自动卖鱼可重试。");return false;}
    PublishSell("仓库达到阈值，正在卖鱼。");
    Log("autofish fish sale requested total="+std::to_string(total)+" locked="+std::to_string(locked)+" threshold="+std::to_string(config->threshold));
    return true;
}
void SellContent(const AnomalyUiServiceV1* ui) {
    const auto config=sell_settings.load();if(!config) return;
    auto changed=std::make_shared<SellSettings>(*config);bool dirty=false;int sell=config->sell;
    if(ui->checkbox(ui->user,StringView("自动卖鱼"),&sell)) {changed->sell=sell!=0;dirty=true;}
    uint32_t threshold=config->threshold;
    if(ui->input_uint32 && ui->input_uint32(ui->user,StringView("卖出阈值（条）"),&threshold,1,10)) {
        changed->threshold=std::clamp<uint32_t>(threshold,1,100000);dirty=true;
    }
    if(fish_total.load()>=0) ui->text(ui->user,StringView("仓库鱼数量："+std::to_string(fish_total.load())+"（锁定 "+std::to_string(fish_locked.load())+"）"));
    if(const auto text=sell_status.load()) ui->text(ui->user,StringView(*text));
    if(sold_total.load()>0) ui->text(ui->user,StringView("本次已卖出："+std::to_string(sold_total.load())+" 条"));
    if(dirty) sell_settings.store(changed);
}
