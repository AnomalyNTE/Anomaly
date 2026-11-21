#include "native.hpp"
#include "browser.hpp"
#include "preferences.hpp"
#include "icons.hpp"
#include "cards.hpp"
#include "batch.hpp"
#include <anomaly/sdk/services/ui_resources.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace shop {
constexpr auto Id = "b1ank.remote-shop";
constexpr auto Version = "0.6.21";
constexpr auto Title = "Remote Shop";
using anomaly::sdk::Host;
using anomaly::sdk::Ok;

uint64_t Now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::string Number(int64_t value) {
    auto text = std::to_string(value);
    for (size_t index = text.size(); index > 3; index -= 3) text.insert(index - 3, ",");
    return text;
}
enum class Page { Shops, Search, Favorites, Basket };

struct Command {
    bool refresh{};
    std::string shop, goods;
    uint32_t quantity{};
    Handle player{};
    std::vector<BatchLine> batch;
    bool cancel_batch{};
};
struct View {
    std::shared_ptr<const std::vector<Goods>> goods = std::make_shared<const std::vector<Goods>>();
    std::shared_ptr<const std::map<GoodsKey,ProductKey>> product_keys = std::make_shared<const std::map<GoodsKey,ProductKey>>();
    std::string message;
    bool busy{}, ready{};
    Handle player{};
    bool batch_active{};
    std::set<GoodsKey> sold_out;
    std::vector<std::pair<GoodsKey,uint32_t>> confirmed_basket;
    IconSource shop_fallback_icon;
};
struct Context {
    Engine engine;
    CatalogReader reader{engine};
    const AnomalyWindowServiceV1* windows{};
    const AnomalyUe5AhudServiceV1* hud{};
    Handle window{}, tick_subscription{}, event_subscription{};
    CardControls controls;
    std::atomic<bool> stopping{};
    std::mutex mutex;
    View view;
    std::optional<Command> command;
    // These fields belong to serialized Game-thread callbacks.
    Purchase purchase;
    Batch batch;
    Handle state{};
    uintptr_t state_address{}, buy_function{}, receipt_function{};
    bool scanning{}, dispatching{};
    uint64_t next_poll{};
    std::string submitted_label;
    // Render-thread editor state.
    std::array<char, 128> filter{};
    std::string selected_shop, selected_goods;
    uint32_t quantity{1};
    FavoriteStore favorites;
    Page page{Page::Search};
    std::optional<Page> synced_tab;
    std::optional<std::pair<std::string,bool>> synced_branch_tab;
    bool show_branches{};
    std::array<char,128> merchant_filter{};
    std::string merchant, open_shop;
    BrowserIndex browser;
    std::shared_ptr<const std::vector<Goods>> indexed_goods;
    uint64_t catalog_revision{};
    uint64_t rendered_catalog_revision{};
    bool catalog_initialized{}, scroll_reset_frame{};
    IconCache icons;
    const AnomalyInputServiceV1* input{};
    bool selection_mouse_down{};
    std::map<GoodsKey,uint32_t> basket;
    std::map<ProductKey,GoodsKey> sources;
    std::vector<GoodsKey> restored_sources;
    std::optional<BrowserSettings> last_browser;
    std::optional<ProductKey> source_picker;
    uintptr_t stock_function{},stock_reply{};
    uint64_t stock_request_at{},stock_reply_at{},stock_poll_at{};
    std::map<std::string,int64_t> bought;
    bool stock_dirty{true};

    void Message(std::string text, bool busy = false) {
        std::scoped_lock lock(mutex);
        view.message = std::move(text);
        view.busy = busy;
    }
    void Log(std::string_view message) const {
        if (engine.core && engine.core->log) engine.core->log(engine.core->user, ANOMALY_CORE_LOG_LEVEL_V1_INFO, StringView(message));
    }
};

bool BindFunctions(Context& ctx, uintptr_t state) {
    ctx.stock_function = ctx.engine.Function(state,"ServerQueryShopGoodsLimitBuyedInfo",0,0,{},0x00200040);
    ctx.stock_reply = ctx.engine.Function(state,"ClientNortifyShopGoodsLimitBuyed",1,16,{{"InGoodsInfo",0,16}},0x01000040);
    ctx.buy_function = ctx.engine.Function(state, "ServerShopBuyGoods", 5, sizeof(BuyParameters), {
        {"InShopName", 0, 8}, {"InGoodID", 8, 8}, {"InAmount", 16, 8}, {"bShowPopUpWindow", 24, 1},
        {"InCityShopRPCParameter", 28, 20}}, 0x00200040);
    ctx.receipt_function = ctx.engine.Function(state, "ClientShopBuyGoodsSuccess", 4, ReceiptParameterSize, {
        {"InShopName", 0, 8}, {"InGoodID", 8, 8}, {"InAmount", 16, 8}, {"IsCityShop", 24, 1}}, 0x01000040);
    return ctx.buy_function && ctx.receipt_function;
}

void Scan(Context& ctx) {
    ctx.scanning = ctx.reader.Begin();
    if (!ctx.scanning) {
        ctx.Log("RemoteShop catalog initialization failed");
        ctx.Message(ctx.engine.localizer.Text("status.catalog_unavailable","Shop data is unavailable. Enter the game world and refresh."));
        return;
    }
    ctx.Log("RemoteShop item labels indexed=" + std::to_string(ctx.reader.item_names.size()));
    ctx.Message(ctx.engine.localizer.Text("status.loading","Reading shops and products."), true);
}

bool Buy(Context& ctx, const Command& command) {
    if (!Same(ctx.state, command.player) || !ctx.reader.ready || ctx.purchase.Waiting()) {
        ctx.Message(ctx.engine.localizer.Text("status.player_changed","Player state changed. Refresh and select the product again."));
        return false;
    }
    const auto found = std::ranges::find_if(ctx.reader.goods, [&](const Goods& item) {
        return item.shop_id == command.shop && item.id == command.goods;
    });
    if (found == ctx.reader.goods.end() || !command.quantity) {
        ctx.Message(ctx.engine.localizer.Text("status.invalid_order","Invalid product or quantity."));
        return false;
    }
    const auto& item = *found;
    if (command.quantity < item.minimum || (item.selector_max > 0 && command.quantity > item.selector_max) ||
        (item.maximum > 0 && command.quantity > item.maximum) || (item.stock && command.quantity > *item.stock) || !TotalPrice(item.price, command.quantity)) {
        ctx.Message(ctx.engine.localizer.Text("status.invalid_quantity","Quantity or total price is outside the supported range."));
        return false;
    }
    BuyParameters params{};
    params.shop = item.shop;
    params.goods = item.key;
    params.quantity = command.quantity;
    ctx.purchase.Begin(params, ctx.state.id, ctx.state.generation, Now());
    ctx.submitted_label = item.shop_label + " / " + item.label;
    ctx.Message(ctx.engine.localizer.Format("status.submitted","Submitted {0} x {1}; waiting for the purchase receipt.",std::array<std::string_view,2>{ctx.submitted_label,std::to_string(command.quantity)}), true);
    ctx.Log("Submit ServerShopBuyGoods shop=" + item.shop_id + " goods=" + item.id + " quantity=" + std::to_string(command.quantity));
    ctx.dispatching = true;
    const bool invoked = ctx.engine.InvokePurchase(ctx.state_address, ctx.buy_function, params);
    ctx.dispatching = false;
    if (!invoked && ctx.purchase.Waiting()) {
        ctx.purchase.outcome = Outcome::DispatchFailed;
        ctx.Message(ctx.engine.localizer.Text("status.invoke_failed","Purchase call failed. Check inventory and balance before retrying."));
    }
    return invoked;
}

void PublishCatalog(Context& ctx) {
    auto goods = std::make_shared<std::vector<Goods>>();
    auto product_keys = std::make_shared<std::map<GoodsKey,ProductKey>>();
    std::set<ProductKey> products;
    std::set<GoodsKey> sold;
    for (auto& item : ctx.reader.goods) {
        product_keys->emplace(GoodsKey{item.shop_id,item.id},item.Product());
        const auto found = ctx.bought.find(item.id);
        item.stock = RemainingStock(item.limit,found == ctx.bought.end() ? std::optional<int64_t>{} : found->second,item.variable_limit);
        if (item.stock && *item.stock == 0) sold.emplace(item.shop_id,item.id);
        else { goods->push_back(item); products.insert(item.Product()); }
    }
    ctx.Log("RemoteShop stock filter visible=" + std::to_string(goods->size()) + " products=" + std::to_string(products.size()) + " soldOut=" + std::to_string(sold.size()));
    std::scoped_lock lock(ctx.mutex);
    ctx.view.goods = std::move(goods); ctx.view.sold_out = std::move(sold);
    ctx.view.product_keys = std::move(product_keys);
    ctx.view.shop_fallback_icon = ctx.reader.shop_fallback_icon;
}

void PollStock(Context& ctx,uint64_t now) {
    if (ctx.stock_function && (ctx.stock_dirty || now >= ctx.stock_request_at + 30000)) {
        ctx.stock_dirty = false; ctx.stock_request_at = now;
        ctx.engine.InvokeActor(ctx.state_address,ctx.stock_function,nullptr);
    }
    if (!ctx.reader.ready || now < ctx.stock_poll_at) return;
    ctx.stock_poll_at = now + 1000;
    std::map<std::string,int64_t> bought;
    if (ctx.stock_reply_at && now - ctx.stock_reply_at < 45000) {
        const auto component = ctx.engine.Child(ctx.state_address,"ShopComponent");
        uintptr_t type{}; Array rows{};
        if (!ctx.engine.IsA(component,"HTShopComponent") || !ctx.engine.Read(component + ctx.engine.layout.objectClass,type)) return;
        const auto offset = ctx.engine.Field(type,"LimitGoodsBuyedInfo",16);
        if (offset < 0 || !ctx.engine.Read(component + offset,rows) || rows.count < 0 || rows.count > 16384 || rows.capacity < rows.count) return;
        struct NetGoods { Name id; int64_t amount{}; uint8_t rest[16]{}; };
        static_assert(sizeof(NetGoods) == 32);
        std::vector<NetGoods> entries(rows.count);
        if (rows.count && !ctx.engine.ReadBytes(rows.data,entries.data(),entries.size() * sizeof(NetGoods))) return;
        for (const auto& row : entries) if (row.amount >= 0) bought[ctx.engine.NameText(row.id)] = row.amount;
    }
    if (bought != ctx.bought) {
        ctx.bought = std::move(bought); PublishCatalog(ctx);
        ctx.Log("RemoteShop stock snapshot entries=" + std::to_string(ctx.bought.size()));
    }
}

void AdvanceBatch(Context& ctx) {
    const bool was_active = ctx.batch.active;
    const auto completed = ctx.batch.completed;
    ctx.batch.Observe(ctx.purchase.outcome);
    if (ctx.batch.completed > completed) {
        const auto& line = ctx.batch.lines[completed];
        std::scoped_lock lock(ctx.mutex); ctx.view.confirmed_basket.push_back({{line.shop,line.goods},line.quantity});
    }
    if (was_active && !ctx.batch.active) {
        const bool complete = ctx.batch.completed == ctx.batch.lines.size();
        ctx.Message(complete ? ctx.engine.localizer.Format("status.batch_complete","Batch complete: {0} / {1} products confirmed.",std::array<std::string_view,2>{std::to_string(ctx.batch.completed),std::to_string(ctx.batch.lines.size())}) : ctx.engine.localizer.Format("status.batch_stopped","Batch stopped: {0} / {1} products confirmed. Check the pending order and balance before continuing.",std::array<std::string_view,2>{std::to_string(ctx.batch.completed),std::to_string(ctx.batch.lines.size())}));
    }
    if (ctx.batch.Ready()) {
        const auto& next = ctx.batch.lines[ctx.batch.completed];
        if (Buy(ctx,{false,next.shop,next.goods,next.quantity,ctx.state})) ctx.batch.inflight = true;
        else ctx.batch.active = false;
    }
    { std::scoped_lock lock(ctx.mutex); ctx.view.batch_active = ctx.batch.active; if(ctx.batch.active) ctx.view.busy = true; }
}

void ANOMALY_CALL OnEvent(void* raw, uintptr_t object, uintptr_t function, void* parameters) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    if (ctx.stopping || !parameters || object != ctx.state_address) return;
    try {
        if (function == ctx.stock_reply) { ctx.stock_reply_at = Now(); ctx.stock_poll_at = Now() + 50; ctx.Log("RemoteShop stock reply received"); return; }
        if (function == ctx.buy_function) {
            BuyParameters other{};
            std::memcpy(&other, parameters, sizeof(other));
            ctx.Log("Observed ServerShopBuyGoods origin=" + std::string(ctx.dispatching ? "plugin" : "game") +
                " shop=" + ctx.engine.NameText(other.shop) + " goods=" + ctx.engine.NameText(other.goods) +
                " quantity=" + std::to_string(other.quantity) + " popup=" + std::to_string(other.popup) +
                " city=" + std::to_string(other.city.city) + " brand=" + ctx.engine.NameText(other.city.brand) +
                " level=" + std::to_string(other.city.level) + " likeability=" + std::to_string(other.city.likeability));
            if (ctx.purchase.Waiting() && !ctx.dispatching) ctx.purchase.ObserveOtherOrder(other);
        }
        if (function != ctx.receipt_function) return;
        BuyReceipt receipt{};
        std::memcpy(&receipt, parameters, ReceiptParameterSize);
        ctx.Log("Observed ClientShopBuyGoodsSuccess shop=" + ctx.engine.NameText(receipt.shop) +
            " goods=" + ctx.engine.NameText(receipt.goods) + " quantity=" + std::to_string(receipt.quantity) +
            " city=" + std::to_string(receipt.city));
        if (!ctx.purchase.Waiting()) return;
        const auto handle = ctx.engine.ObjectHandle(object);
        if (!ctx.purchase.Accept(receipt, handle.id, handle.generation)) return;
        ctx.stock_dirty = true;
        ctx.Log("ClientShopBuyGoodsSuccess quantity=" + std::to_string(receipt.quantity));
        if (ctx.purchase.outcome == Outcome::Confirmed) {
            ctx.Message(ctx.engine.localizer.Format("status.confirmed","Server confirmed {0} x {1}.",std::array<std::string_view,2>{ctx.submitted_label,std::to_string(receipt.quantity)}),ctx.batch.active);
        } else if (ctx.purchase.outcome == Outcome::Partial) {
            ctx.Message(ctx.engine.localizer.Format("status.partial","Server confirmed quantity {0}; check the actual inventory increase.",std::array<std::string_view,1>{std::to_string(receipt.quantity)}));
        } else {
            ctx.Message(ctx.engine.localizer.Text("status.ambiguous_receipt","A receipt matched the product but not the order or quantity. Check inventory and balance."));
        }
    } catch (...) {
        ctx.purchase.outcome = Outcome::Unconfirmed;
        ctx.Message(ctx.engine.localizer.Text("status.receipt_failed","Could not process the receipt. Check inventory and balance."));
    }
}

void Tick(Context& ctx) {
    const auto now = Now();
    if (ctx.purchase.Expire(now)) ctx.Message(ctx.engine.localizer.Text("status.timeout","No success receipt within 20 seconds. Check inventory, balance and game messages before retrying."));
    bool has_command{};
    {
        std::scoped_lock lock(ctx.mutex);
        has_command = ctx.command.has_value();
    }
    if (!ctx.scanning && !has_command && now < ctx.next_poll) return;
    ctx.next_poll = now + 500;

    std::string player_error;
    const auto address = ctx.engine.PlayerState(player_error);
    const auto state = ctx.engine.ObjectHandle(address);
    if (!state.id) {
        std::scoped_lock lock(ctx.mutex);
        ctx.command.reset();
        ctx.view.ready = false;
        ctx.view.busy = false;
        ctx.view.message = std::move(player_error);
        ctx.state = {};
        ctx.batch.active = false; ctx.view.batch_active = false; ctx.stock_reply_at = 0; ctx.bought.clear();
        ctx.scanning = false;
        if (ctx.purchase.Waiting()) ctx.purchase.outcome = Outcome::SceneChanged;
        return;
    }
    if (!Same(state, ctx.state)) {
        const bool interrupted = ctx.purchase.Waiting();
        ctx.state = state;
        ctx.state_address = address;
        ctx.batch.active = false; ctx.bought.clear(); ctx.stock_reply_at = 0; ctx.stock_dirty = true;
        ctx.purchase.outcome = interrupted ? Outcome::SceneChanged : Outcome::Idle;
        {
            std::scoped_lock lock(ctx.mutex);
            ctx.command.reset();
            ctx.view = {};
            ctx.view.player = state;
        }
        if (!BindFunctions(ctx, address)) {
            ctx.Message(ctx.engine.localizer.Text("status.incompatible","Purchase interface is incompatible. Update the plugin before retrying."));
            return;
        }
        Scan(ctx);
        if (interrupted) ctx.Message(ctx.engine.localizer.Text("status.scene_changed","Player or scene changed. Check the previous purchase in your inventory."), ctx.scanning);
    }
    if (!ctx.buy_function || !ctx.receipt_function) {
        if (!has_command) return;
        if (!BindFunctions(ctx, address)) {
            { std::scoped_lock lock(ctx.mutex); ctx.command.reset(); }
            ctx.Message(ctx.engine.localizer.Text("status.incompatible","Purchase interface is incompatible. Update the plugin before retrying."));
            return;
        }
    }

    std::optional<Command> command;
    {
        std::scoped_lock lock(ctx.mutex);
        command = std::move(ctx.command);
        ctx.command.reset();
    }
    if (command) {
        if (command->cancel_batch) ctx.batch.cancel = true;
        else if (!command->batch.empty() && Same(command->player,ctx.state) && !ctx.batch.active && !ctx.purchase.Waiting()) {
            const bool valid = ctx.reader.ready && std::ranges::all_of(command->batch,[&](const BatchLine& line) {
                const auto item = std::ranges::find_if(ctx.reader.goods,[&](const Goods& item){return item.shop_id == line.shop && item.id == line.goods;});
                return item != ctx.reader.goods.end() && line.quantity >= item->minimum && line.quantity &&
                    (item->selector_max <= 0 || line.quantity <= item->selector_max) && (item->maximum <= 0 || line.quantity <= item->maximum) &&
                    (!item->stock || line.quantity <= *item->stock) && TotalPrice(item->price,line.quantity).has_value();
            });
            if (valid) ctx.batch.Start(std::move(command->batch));
            else ctx.Message(ctx.engine.localizer.Text("status.invalid_batch","A batch product or quantity is invalid. Refresh and check the list; no order was submitted."));
        }
        else if (command->refresh && !ctx.purchase.Waiting()) { ctx.stock_dirty = true; Scan(ctx); }
        else if (!command->refresh) Buy(ctx, *command);
    }
    PollStock(ctx,now);
    AdvanceBatch(ctx);
    if (!ctx.scanning) return;
    if (!ctx.reader.Step()) {
        ctx.scanning = false;
        ctx.Message(ctx.engine.localizer.Text("status.read_failed","Could not read products. Refresh and retry."));
        return;
    }
    if (ctx.reader.ready) {
        ctx.scanning = false;
        ctx.Log("RemoteShop catalog ready goods=" + std::to_string(ctx.reader.goods.size()) + " shops=" +
            std::to_string(ctx.reader.shops.size()));
        for (const auto& [key,shop] : ctx.reader.shops) if (DisplayText(shop.label) != shop.label) {
            ctx.Log("RemoteShop font display " + shop.label + " -> " + DisplayText(shop.label));
            break;
        }
        const auto unresolved = std::ranges::count_if(ctx.reader.goods, [](const Goods& item) { return item.label == item.id; });
        ctx.Log("RemoteShop product names resolved=" + std::to_string(ctx.reader.goods.size() - unresolved) +
            " fallback=" + std::to_string(unresolved));
        for (size_t i = 0; i < (std::min)(size_t(6), ctx.reader.goods.size()); ++i) {
            const auto& item = ctx.reader.goods[i];
            ctx.Log("RemoteShop product name " + item.id + " = " + item.label);
        }
        PublishCatalog(ctx);
        if (!ctx.reader.goods.empty()) ctx.icons.Request(ctx.reader.goods.front());
        ctx.icons.Request(ctx.reader.shop_fallback_icon);
        std::scoped_lock lock(ctx.mutex);
        ctx.view.ready = true;
        ctx.view.busy = false;
        ctx.view.message.clear();
    } else {
        ctx.Message(ctx.engine.localizer.Format("status.reading","Reading products {0} / {1}",std::array<std::string_view,2>{std::to_string(ctx.reader.cursor),std::to_string(ctx.reader.rows.size())}), true);
    }
}

void ANOMALY_CALL OnGame(void* raw, const AnomalyUe5AhudFrameV1*) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    if (ctx.stopping || !ctx.engine.framework->is_game_thread(ctx.engine.framework->user)) return;
    try { Tick(ctx); ctx.icons.Tick(ctx.engine); }
    catch (...) {
        ctx.scanning = false;
        if (ctx.purchase.Waiting()) ctx.purchase.outcome = Outcome::Unconfirmed;
        ctx.Message(ctx.engine.localizer.Text("status.processing_failed","Shop processing failed. Check purchases before refreshing."));
    }
}

template<class T> const T* Query(const Host& host, const char* id) {
    const auto* service = host.Query<T>(id).get();
    return service && service->struct_size >= sizeof(T) ? service : nullptr;
}

AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* api, void** out) noexcept {
    if (!out) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
    *out = nullptr;
    try {
        auto ctx = std::make_unique<Context>();
        const Host host(api);
        ctx->windows = Query<AnomalyWindowServiceV1>(host, ANOMALY_WINDOW_SERVICE_V1_ID);
        ctx->hud = Query<AnomalyUe5AhudServiceV1>(host, ANOMALY_UE5_AHUD_SERVICE_V1_ID);
        auto& engine = ctx->engine;
        engine.localizer = anomaly::sdk::Localizer(api);
        const auto loaded = anomaly::sdk::LoadProfile(host,"compatibility.profile.json",{"nte","HTGame.exe",".text","remote-shop-v1"});
        if (!loaded || !engine.layout.Load(loaded.value()))
            return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView("RemoteShop compatibility profile is unavailable or invalid")};
        engine.profile = loaded.value();
        ctx->view.message = engine.localizer.Text("status.connecting","Connecting to the game.");
        ctx->favorites.localizer = &engine.localizer;
        engine.core = Query<AnomalyCoreServiceV1>(host, ANOMALY_CORE_SERVICE_V1_ID);
        // find_utf8 is optional; the map icon lookup checks its appended service size.
        engine.names = host.Query<AnomalyUe5NamesServiceV1>(ANOMALY_UE5_NAMES_SERVICE_V1_ID).get();
        if (engine.names && engine.names->struct_size < offsetof(AnomalyUe5NamesServiceV1, find_utf8)) engine.names = nullptr;
        engine.objects = Query<AnomalyUe5ObjectsServiceV1>(host, ANOMALY_UE5_OBJECTS_SERVICE_V1_ID);
        engine.events = Query<AnomalyUe5ProcessEventServiceV1>(host, ANOMALY_UE5_PROCESS_EVENT_SERVICE_V1_ID);
        engine.players = Query<AnomalyNtePlayerServiceV1>(host, ANOMALY_NTE_PLAYER_SERVICE_V1_ID);
        engine.framework = Query<AnomalyUe5FrameworkServiceV1>(host, ANOMALY_UE5_FRAMEWORK_SERVICE_V1_ID);
        engine.build = Query<AnomalyUe5BuildServiceV1>(host, ANOMALY_UE5_BUILD_SERVICE_V1_ID);
        ctx->favorites.config = Query<AnomalyConfigServiceV1>(host, ANOMALY_CONFIG_SERVICE_V1_ID);
        ctx->favorites.core = engine.core;
        ctx->favorites.scheduler = Query<AnomalySchedulerServiceV1>(host,ANOMALY_SCHEDULER_SERVICE_V1_ID);
        ctx->icons.service = Query<AnomalyTextureServiceV1>(host, ANOMALY_TEXTURE_SERVICE_V1_ID);
        ctx->input = Query<AnomalyInputServiceV1>(host,ANOMALY_INPUT_SERVICE_V1_ID);
        if (ctx->icons.service && (!ctx->icons.service->request || !ctx->icons.service->state || !ctx->icons.service->draw || !ctx->icons.service->release)) ctx->icons.service = nullptr;
        ctx->controls.service = ctx->icons.service;
        ctx->icons.scheduler = ctx->favorites.scheduler;
        ctx->icons.Initialize();
        if (!ctx->windows || !ctx->hud || !engine.core || !engine.names || !engine.objects || !engine.events ||
            !engine.players || !engine.framework || !engine.build || !engine.build->feature_state || !engine.events->invoke || !engine.events->subscribe ||
            !engine.events->unsubscribe || !engine.players->get_object_handles || !engine.core->read_memory ||
            !engine.names->resolve_utf8 || !engine.objects->snapshot_at || !engine.objects->find_exact ||
            !engine.framework->is_game_thread || !ctx->hud->subscribe || !ctx->hud->unsubscribe ||
            !ctx->windows->register_window || !ctx->windows->release_window || !ctx->windows->begin || !ctx->windows->end) {
            return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, StringView("RemoteShop requires the current Anomaly ProcessEvent invoke and player object handle extensions")};
        }
        if (!ctx->favorites.config || !ctx->favorites.config->register_schema || !ctx->favorites.config->read ||
            !ctx->favorites.config->write_atomic) return {ANOMALY_STATUS_V1_UNAVAILABLE, 0, StringView("RemoteShop requires anomaly.config for persistent favorites")};
        if (!ctx->favorites.scheduler || !ctx->favorites.scheduler->schedule) return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView("RemoteShop requires anomaly.scheduler for persistent favorites")};
        if (!ctx->input || !ctx->input->snapshot) return {ANOMALY_STATUS_V1_UNAVAILABLE,0,StringView("RemoteShop requires the input snapshot service")};
        if (ctx->favorites.Load()) ctx->Log("RemoteShop favorites restored shops=" + std::to_string(ctx->favorites.value.shops.size()) +
            " goods=" + std::to_string(ctx->favorites.value.goods.size()));
        if (ctx->favorites.value.browser) {
            const auto& browser = *ctx->favorites.value.browser;
            ctx->page = static_cast<Page>(browser.page); ctx->quantity = browser.quantity;
            std::copy(browser.filter.begin(),browser.filter.end(),ctx->filter.begin());
            std::copy(browser.merchant_filter.begin(),browser.merchant_filter.end(),ctx->merchant_filter.begin());
            ctx->merchant = browser.merchant; ctx->open_shop = browser.shop;
            ctx->selected_shop = browser.selected_shop; ctx->selected_goods = browser.selected_goods;
            ctx->restored_sources = browser.sources; ctx->last_browser = browser;
        } else if (!ctx->favorites.value.Empty()) ctx->page = Page::Favorites;
        *out = ctx.release();
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

AnomalyStatusV1 ANOMALY_CALL Start(void* raw) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        ctx.stopping = false;
        AnomalyWindowSpecV1 window{};
        window.struct_size = sizeof(window);
        window.id = StringView("remote-shop-main");
        const auto title = ctx.engine.localizer.Text("window.title","Remote Shop");
        window.title = StringView(title);
        window.initial_width = 860; window.initial_height = 800;
        window.minimum_width = 560; window.minimum_height = 520;
        window.default_open = 1;
        auto status = ctx.windows->register_window(ctx.windows->user, &window, &ctx.window);
        if (!Succeeded(status)) return status;
        status = ctx.engine.events->subscribe(ctx.engine.events->user, OnEvent, &ctx, &ctx.event_subscription);
        if (!Succeeded(status)) return status;
        return ctx.hud->subscribe(ctx.hud->user, OnGame, &ctx, &ctx.tick_subscription);
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}

AnomalyStatusV1 ANOMALY_CALL Stop(void* raw, uint32_t) noexcept {
    try {
        auto& ctx = *static_cast<Context*>(raw);
        ctx.stopping = true;
        if (ctx.tick_subscription.id) {
            const auto status = ctx.hud->unsubscribe(ctx.hud->user, ctx.tick_subscription);
            if (!Succeeded(status) && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
            ctx.tick_subscription = {};
        }
        if (ctx.event_subscription.id) {
            const auto status = ctx.engine.events->unsubscribe(ctx.engine.events->user, ctx.event_subscription);
            if (!Succeeded(status) && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
            ctx.event_subscription = {};
        }
        if (ctx.window.id) {
            const auto status = ctx.windows->release_window(ctx.windows->user, ctx.window);
            if (!Succeeded(status) && status.code != ANOMALY_STATUS_V1_NOT_FOUND) return status;
            ctx.window = {};
        }
        ctx.favorites.CommitPending();
        ctx.controls.Release();
        ctx.icons.Release();
        return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}
void ANOMALY_CALL Unload(void* raw) noexcept { delete static_cast<Context*>(raw); }
void ANOMALY_CALL Update(void* raw, double) noexcept {
    auto& ctx = *static_cast<Context*>(raw);
    if (ctx.stopping) return;
    try { ctx.favorites.SchedulePending(); ctx.icons.SchedulePending(); }
    catch (...) { ctx.Log("RemoteShop favorites worker failed"); }
}

void Queue(Context& ctx, Command command) {
    std::scoped_lock lock(ctx.mutex);
    if (ctx.command || ((ctx.view.busy || ctx.view.batch_active) && !command.cancel_batch)) return;
    ctx.command = std::move(command);
    ctx.view.busy = true;
    ctx.view.message = ctx.engine.localizer.Text("status.processing","Processing.");
}

std::vector<GoodsKey> SavedProduct(const Context& ctx,const ProductKey& product) {
    std::vector<GoodsKey> result;
    for (const auto& [key,saved] : ctx.favorites.value.goods) {
        const auto offer = ctx.browser.offer_products.find(key);
        if ((saved.product && *saved.product == product) ||
            (!saved.product && offer != ctx.browser.offer_products.end() && offer->second == product)) result.push_back(key);
    }
    return result;
}
void RemoveProduct(Context& ctx,const ProductKey& product) {
    const auto keys = SavedProduct(ctx,product);
    if (keys.empty()) return;
    auto next = ctx.favorites.value;
    for (const auto& key : keys) next.goods.erase(key);
    ctx.favorites.Save(std::move(next));
}
void ToggleProduct(Context& ctx,const Goods& item,uint32_t quantity) {
    const auto product = item.Product();
    if (!SavedProduct(ctx,product).empty()) { RemoveProduct(ctx,product); return; }
    auto next = ctx.favorites.value;
    next.goods[{item.shop_id,item.id}] = {item.shop_label,item.label,quantity ? quantity : 1,product};
    ctx.favorites.Save(std::move(next));
}
size_t CurrentOffer(const Context& ctx,const std::vector<Goods>& goods,const ProductGroup& group) {
    if (const auto source = ctx.sources.find(group.key); source != ctx.sources.end()) {
        for (auto i : group.offers) if (GoodsKey{goods[i].shop_id,goods[i].id} == source->second) return i;
    }
    // Keep an already selected basket/favorite source when it is still available.
    for (auto i : group.offers) if (ctx.basket.contains({goods[i].shop_id,goods[i].id})) return i;
    for (const auto& key : SavedProduct(ctx,group.key))
        for (auto i : group.offers) if (GoodsKey{goods[i].shop_id,goods[i].id} == key) return i;
    return DefaultOffer(goods,group);
}

void PersistBrowser(Context& ctx) {
    BrowserSettings browser;
    browser.page = static_cast<int>(ctx.page); browser.quantity = ctx.quantity;
    browser.filter = ctx.filter.data(); browser.merchant_filter = ctx.merchant_filter.data();
    browser.merchant = ctx.merchant; browser.shop = ctx.open_shop;
    browser.selected_shop = ctx.selected_shop; browser.selected_goods = ctx.selected_goods;
    std::set<GoodsKey> sources(ctx.restored_sources.begin(),ctx.restored_sources.end());
    for (const auto& [product,key] : ctx.sources) sources.insert(key);
    browser.sources.assign(sources.begin(),sources.end());
    if (ctx.last_browser && *ctx.last_browser == browser) return;
    ctx.last_browser = browser;
    if (!ctx.favorites.writable) return;
    auto next = ctx.favorites.value; next.browser = std::move(browser);
    ctx.favorites.Save(std::move(next),true);
}

void DrawContents(Context& ctx, const AnomalyUiServiceV1& ui) {
    ctx.favorites.PollPending();
    bool selection_pressed{};
    AnomalyInputSnapshotV1 input{sizeof(input)};
    if (ctx.input && Succeeded(ctx.input->snapshot(ctx.input->user,&input))) {
        const bool down = (input.mouse_buttons & 1) != 0;
        selection_pressed = down && !ctx.selection_mouse_down;
        ctx.selection_mouse_down = down;
    }
    View view;
    { std::scoped_lock lock(ctx.mutex); view = ctx.view; ctx.view.confirmed_basket.clear(); }
    for (const auto& [key,quantity] : view.confirmed_basket) {
        const auto row = ctx.basket.find(key);
        if (row != ctx.basket.end() && row->second == quantity) ctx.basket.erase(row);
    }
    std::erase_if(ctx.basket,[&](const auto& row){return view.sold_out.contains(row.first);});
    if (ctx.indexed_goods != view.goods) {
        ctx.browser.localizer = &ctx.engine.localizer;
        ctx.browser.Build(*view.goods); ctx.indexed_goods = view.goods; ++ctx.catalog_revision;
        // Keep identity for unavailable sources so older favorites can resolve to a stocked branch.
        ctx.browser.offer_products.insert(view.product_keys->begin(),view.product_keys->end());
        for (auto it = ctx.restored_sources.begin(); it != ctx.restored_sources.end();) {
            const auto product = ctx.browser.offer_products.find(*it);
            if (product == ctx.browser.offer_products.end()) { ++it; continue; }
            ctx.sources.try_emplace(product->second,*it); it = ctx.restored_sources.erase(it);
        }
    }
    const auto text = [&](std::string_view value) { ui.text(ui.user, StringView(DisplayText(std::string(value)))); };
    const auto button = [&](std::string_view label, bool enabled = true) {
        return ui.button_enabled(ui.user, StringView(DisplayText(std::string(label))), 0, 0, enabled ? 1 : 0) != 0;
    };
    const auto line = [&] { ui.same_line(ui.user, 0, -1); };
    const auto navigate = [&](Page page) {
        ctx.page = page; ctx.merchant.clear(); ctx.open_shop.clear(); ctx.filter.fill(0);
        ctx.merchant_filter.fill(0); ctx.show_branches = false;
        ctx.source_picker.reset();
        ctx.selected_goods.clear(); ++ctx.catalog_revision;
    };
    const auto select = [&](const Goods& item) {
        ctx.selected_shop = item.shop_id; ctx.selected_goods = item.id;
        ctx.sources[item.Product()] = {item.shop_id,item.id};
        const auto favorites = SavedProduct(ctx,item.Product());
        const auto exact = ctx.favorites.value.goods.find({item.shop_id,item.id});
        auto quantity = exact != ctx.favorites.value.goods.end() ? exact->second.quantity :
            favorites.empty() ? uint32_t(1) : ctx.favorites.value.goods.at(favorites.front()).quantity;
        if (const auto picked = ctx.basket.find({item.shop_id,item.id}); picked != ctx.basket.end()) quantity = picked->second;
        quantity = (std::max)(quantity, static_cast<uint32_t>((std::min)(int64_t(UINT32_MAX),item.minimum)));
        for (const auto upper : {item.selector_max,item.maximum})
            if (upper > 0) quantity = (std::min)(quantity, static_cast<uint32_t>((std::min)(int64_t(UINT32_MAX),upper)));
        ctx.quantity = quantity;
        if (item.stock && *item.stock > 0) ctx.quantity = (std::min)(ctx.quantity,static_cast<uint32_t>((std::min)(int64_t(UINT32_MAX),*item.stock)));
    };
    const auto favorite_shop = [&](const Branch& shop) {
        const auto saved = ctx.favorites.value.shops.contains(shop.id);
        if (button(std::string(saved ? ctx.engine.localizer.Text("favorite.saved","Saved") : ctx.engine.localizer.Text("favorite.save","Favorite")) + "##shop-favorite-" + shop.id, ctx.favorites.writable))
            ctx.favorites.ToggleShop(shop.id, shop.name);
    };
    float window_width = 720, window_height{};
    if (ui.get_window_size) ui.get_window_size(ui.user,&window_width,&window_height);
    const auto grid = CardGrid::ForWidth(window_width);
    const auto merchant_grid = CardGrid::ForWidth(window_width,200,6);
    const auto shop_icon = [&](const IconSource& icon,float size) {
        if (!icon.icon_path.empty() && ctx.icons.Draw(icon,size)) return true;
        return ctx.icons.Draw(view.shop_fallback_icon,size);
    };
    const auto shop_card = [&](const std::string& id,const std::string& title,const std::string& subtitle,const IconSource& icon) {
        bool pressed{};
        const bool visible = ui.begin_child(ui.user,StringView("shop-card-" + id),merchant_grid.width,80,129) != 0;
        if (visible) {
            const bool icon_visible = ui.begin_child(ui.user,StringView("shop-icon-" + id),56,56,0) != 0;
            if (icon_visible) shop_icon(icon,56);
            ui.end_child(ui.user);
            line();
            const float width = merchant_grid.width - 82;
            const auto label = FitText(ui,title,width - 16) + "\n" + FitText(ui,subtitle,width - 16) + "##" + id;
            pressed = ui.button_enabled(ui.user,StringView(label),width,56,1) != 0;
        }
        ui.end_child(ui.user);
        return pressed;
    };
    float line_height = 13, measured_width{};
    if (ui.struct_size >= offsetof(AnomalyUiServiceV1,overlay_text) && ui.overlay_measure_text)
        ui.overlay_measure_text(ui.user,StringView("Ag"),0,&measured_width,&line_height);
    // Anomaly's scaled 13px font has 6px frame padding above and below.
    const auto metrics = CardMetrics::ForLineHeight(line_height);
    const float header_height = metrics.header,row_spacing = metrics.spacing;
    const float row_box_height = metrics.row,card_height = metrics.height;
    ctx.controls.Prepare(header_height);
    size_t card_index{};
    std::optional<ProductKey> remove_favorite;
    const auto goods_row = [&](const ProductGroup& group) {
        const auto& item = (*view.goods)[CurrentOffer(ctx,*view.goods,group)];
        const auto saved = !SavedProduct(ctx,group.key).empty();
        const bool favorite_card = ctx.page == Page::Favorites && ctx.open_shop.empty() && saved;
        const auto& anchor = (*view.goods)[group.offers.front()];
        const auto tag = anchor.shop_id + "/" + anchor.id;
        if (card_index++ % grid.columns) ui.same_line(ui.user,0,CardGrid::Gap);
        // Native ImGuiChildFlags_Borders | FrameStyle gives each compact offer a visible frame background.
        const bool visible = ui.begin_child(ui.user,StringView("card-" + tag),grid.width,card_height,129) != 0;
        if (visible) {
        const auto currency = item.currency_label.empty() ? item.currency_id : item.currency_label;
        const bool icon_area = ui.begin_child(ui.user,StringView("card-icon-" + tag),56,-2,0) != 0;
        if (icon_area) {
            float icon_width = 56,icon_height = card_height - 16;
            if (ui.get_window_size) ui.get_window_size(ui.user,&icon_width,&icon_height);
            const float top = (icon_height - 56) / 2 - row_spacing;
            ctx.controls.Space(top);
            if (!ctx.icons.Draw(item,56)) ui.button_enabled(ui.user,StringView("…##icon-" + tag),56,56,0);
        }
        ui.end_child(ui.user);
        ui.same_line(ui.user,0,6);
        float body_width = grid.width - 80,body_height = card_height - 16;
        const bool body = ui.begin_child(ui.user,StringView("card-info-" + tag),-2,-2,0) != 0;
        if (body) {
        if (ui.get_window_size) ui.get_window_size(ui.user,&body_width,&body_height);
        const bool header = ui.begin_child(ui.user,StringView("card-header-" + tag),-2,row_box_height,0) != 0;
        if (header) {
        const bool title = ui.begin_child(ui.user,StringView("card-header-main-" + tag),favorite_card ? -28.0f : -2.0f,-2,0) != 0;
        if (title) {
        float title_width = body_width - (favorite_card ? 32.0f : 2.0f),title_height{};
        if (ui.get_window_size) ui.get_window_size(ui.user,&title_width,&title_height);
        const auto amount = Number(item.price);
        const auto price = FitText(ui,currency,(std::max)(0.0f,title_width - TextWidth(ui,amount) - 50)) + "  " + amount;
        const float name_width = (std::max)(24.0f,title_width - TextWidth(ui,price) - 20);
        const auto name = FitText(ui,item.label,name_width - 8);
        // Let the host include its actual font ascent and frame padding in button height.
        if (ui.button_enabled(ui.user,StringView(name + "##goods-" + tag),name_width,0,1)) select(item);
        line(); RightText(ui,price,title_width);
        }
        ui.end_child(ui.user);
        if (favorite_card) {
            ui.same_line(ui.user,0,4);
            const bool close = ui.begin_child(ui.user,StringView("card-close-" + tag),20,-2,0) != 0;
            if (close) {
            if (!ctx.controls.Draw(2)) text("×");
            if (ctx.favorites.writable && selection_pressed && (ui.frame_state(ui.user) & ANOMALY_UI_FRAME_V1_ITEM_HOVERED))
                remove_favorite = group.key;
            }
            ui.end_child(ui.user);
        }
        }
        ui.end_child(ui.user);
        // Empty child windows inherit WindowRounding's minimum height. A transparent
        // image is a true-sized layout item and can occupy a 4px gap without growing.
        ctx.controls.Space(metrics.FooterGap(body_height));
        const bool footer = ui.begin_child(ui.user,StringView("card-footer-" + tag),-2,row_box_height,0) != 0;
        if (footer) {
        const bool star = ui.begin_child(ui.user,StringView("card-star-" + tag),20,-2,0) != 0;
        if (star) {
        if (!ctx.controls.Draw(saved ? 4 : 3)) text(saved ? "★" : "☆");
        if (ctx.favorites.writable && selection_pressed && (ui.frame_state(ui.user) & ANOMALY_UI_FRAME_V1_ITEM_HOVERED)) {
            const auto quantity = ctx.selected_shop == item.shop_id && ctx.selected_goods == item.id ? ctx.quantity :
                static_cast<uint32_t>((std::min)(int64_t(UINT32_MAX),item.minimum));
            ToggleProduct(ctx,item,quantity);
        }
        }
        ui.end_child(ui.user);
        ui.same_line(ui.user,0,4);
        const bool source_area = ui.begin_child(ui.user,StringView("card-source-" + tag),-28,-2,0) != 0;
        if (source_area) {
            float source_width = body_width - 56,source_height{};
            if (ui.get_window_size) ui.get_window_size(ui.user,&source_width,&source_height);
            std::set<std::string> shops,brands;
            for (auto i : group.offers) { shops.insert((*view.goods)[i].shop_id); brands.insert((*view.goods)[i].shop_label); }
            const auto source = shops.size() <= 1 ? item.shop_label + " · " + BranchLabel(item.shop_id,ctx.engine.localizer) :
                brands.size() == 1 ? ctx.engine.localizer.Format("source.branches","{0} · {1} branches",std::array<std::string_view,2>{item.shop_label,std::to_string(shops.size())}) :
                ctx.engine.localizer.Format("source.shops","{0} shops · choose source",std::array<std::string_view,1>{std::to_string(shops.size())});
            RightText(ui,source,source_width);
            if (group.offers.size() > 1 && selection_pressed && (ui.frame_state(ui.user) & ANOMALY_UI_FRAME_V1_ITEM_HOVERED)) {
                select(item); ctx.source_picker = group.key;
            }
        }
        ui.end_child(ui.user);
        ui.same_line(ui.user,0,4);
        const bool check = ui.begin_child(ui.user,StringView("card-check-" + tag),20,-2,0) != 0;
        if (check) {
        int picked = std::ranges::any_of(group.offers,[&](auto i){const auto& offer = (*view.goods)[i];return ctx.basket.contains({offer.shop_id,offer.id});}) ? 1 : 0;
        if (!ctx.controls.Draw(picked ? 1 : 0)) {
            text(picked ? "[x]" : "[ ]");
        }
        if (!view.busy && !view.batch_active && selection_pressed && (ui.frame_state(ui.user) & ANOMALY_UI_FRAME_V1_ITEM_HOVERED)) {
            if (!picked) { select(item); ctx.basket[{item.shop_id,item.id}] = ctx.quantity; }
            else for (auto i : group.offers) { const auto& offer = (*view.goods)[i]; ctx.basket.erase({offer.shop_id,offer.id}); }
        }
        }
        ui.end_child(ui.user);
        }
        ui.end_child(ui.user);
        }
        ui.end_child(ui.user);
        }
        ui.end_child(ui.user);
    };
    if (!view.message.empty()) text(view.message);
    if (button(ctx.engine.localizer.Text("action.refresh","Refresh shops and products"), !view.busy)) Queue(ctx, Command{true});
    const bool sync_tab = !ctx.synced_tab || *ctx.synced_tab != ctx.page;
    const auto requested_page = ctx.page;
    std::optional<Page> active_tab;
    if (ui.begin_tab_bar(ui.user,StringView("remote-shop-pages"),ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN)) {
        const std::array<std::pair<Page,std::string>,4> tabs{{
            {Page::Shops,ctx.engine.localizer.Label("tab.merchants","Merchants","remote-shops")},
            {Page::Search,ctx.engine.localizer.Label("tab.products","All products","remote-goods")},
            {Page::Favorites,ctx.engine.localizer.Label("tab.favorites","Favorites","remote-favorites")},
            {Page::Basket,ctx.engine.localizer.Label("tab.basket","Batch purchases","remote-basket")}
        }};
        for (const auto& [page,label] : tabs) {
            const uint32_t flags = sync_tab && page == requested_page ? ANOMALY_UI_TAB_ITEM_V1_SET_SELECTED : ANOMALY_UI_TAB_ITEM_V1_NONE;
            if (ui.begin_tab_item(ui.user,StringView(label),nullptr,flags,1)) {
                active_tab = page;
                ui.end_tab_item(ui.user);
            }
        }
        ui.end_tab_bar(ui.user);
    }
    if (!sync_tab && active_tab && *active_tab != ctx.page) navigate(*active_tab);
    ctx.synced_tab = ctx.page;
    const bool merchant_list = ctx.page == Page::Shops && ctx.merchant.empty() && ctx.open_shop.empty();
    if (merchant_list) {
        if (ui.input_text(ui.user,StringView(ctx.engine.localizer.Text("search.merchants","Search merchants")),ctx.merchant_filter.data(),ctx.merchant_filter.size(),0)) ++ctx.catalog_revision;
    } else if (ctx.page != Page::Basket && ui.input_text(ui.user, StringView(ctx.page == Page::Shops && !ctx.merchant.empty() ? ctx.engine.localizer.Text("search.merchant_goods","Search this merchant") : ctx.engine.localizer.Text("search.global","Search products / shops")), ctx.filter.data(), ctx.filter.size(), 0)) {
        if (ctx.filter[0] && !(ctx.page == Page::Shops && !ctx.merchant.empty())) { ctx.page = Page::Search; ctx.open_shop.clear(); ctx.merchant.clear(); }
        ++ctx.catalog_revision;
    }
    if (!ctx.favorites.message.empty()) text(ctx.favorites.message);
    text(ctx.page == Page::Basket ? ctx.engine.localizer.Format("basket.count","{0} products selected",std::array<std::string_view,1>{std::to_string(ctx.basket.size())}) : ctx.engine.localizer.Format("basket.hint","Select cards to add products · {0} selected",std::array<std::string_view,1>{std::to_string(ctx.basket.size())}));
    if (view.batch_active && button(ctx.engine.localizer.Text("batch.stop","Stop remaining purchases"))) { Command stop; stop.cancel_batch = true; Queue(ctx,std::move(stop)); }
    if (ctx.page == Page::Basket) {
        std::vector<BatchLine> batch_lines;
        std::map<std::string,int64_t> totals;
        bool basket_valid = !ctx.basket.empty();
        for (const auto& [key,quantity] : ctx.basket) {
            const auto item = std::ranges::find_if(*view.goods,[&](const Goods& item){return item.shop_id == key.first && item.id == key.second;});
            if (item == view.goods->end() || !quantity) { basket_valid = false; continue; }
            const auto price = TotalPrice(item->price,quantity);
            const auto currency = item->currency_label.empty() ? item->currency_id : item->currency_label;
            if (!price || totals[currency] > INT64_MAX - *price || quantity < item->minimum ||
                (item->selector_max > 0 && quantity > item->selector_max) || (item->maximum > 0 && quantity > item->maximum) ||
                (item->stock && quantity > *item->stock)) basket_valid = false;
            else totals[currency] += *price;
            batch_lines.push_back({key.first,key.second,quantity});
        }
        if (button(ctx.engine.localizer.Text("batch.purchase","Buy selected products"),view.ready && !view.busy && !view.batch_active && basket_valid)) {
            Command order; order.player = view.player; order.batch = std::move(batch_lines); Queue(ctx,std::move(order));
        }
        line(); if (button(ctx.engine.localizer.Text("batch.clear","Clear selection"),!view.busy && !view.batch_active)) ctx.basket.clear();
        if (!ctx.basket.empty()) for (const auto& [currency,total] : totals) text(ctx.engine.localizer.Format("basket.total","Estimated batch total · {0} {1}",std::array<std::string_view,2>{currency,Number(total)}));
        if (!basket_valid && !ctx.basket.empty()) text(ctx.engine.localizer.Text("batch.invalid","Some products or quantities are invalid. Check the list before purchasing."));
        const bool open = ui.begin_child(ui.user,StringView("batch-basket"),0,0,1) != 0;
        std::optional<GoodsKey> remove;
        if (open && ctx.basket.empty()) text(ctx.engine.localizer.Text("basket.empty","The list is empty. Select product cards to add them."));
        if (open) for (auto& [key,quantity] : ctx.basket) {
            const auto item = std::ranges::find_if(*view.goods,[&](const Goods& item){return item.shop_id == key.first && item.id == key.second;});
            const auto label = item == view.goods->end() ? ctx.engine.localizer.Format("goods.unavailable","{0} (unavailable)",std::array<std::string_view,1>{key.second}) :
                item->label + " · " + item->shop_label + " · " + BranchLabel(item->shop_id,ctx.engine.localizer);
            const float quantity_x = (std::max)(180.0f,window_width - 270);
            if (ui.set_cursor_pos_x) ui.set_cursor_pos_x(ui.user,quantity_x);
            if (!view.busy && !view.batch_active) {
                if (ui.struct_size >= offsetof(AnomalyUiServiceV1,overlay_viewport_size) && ui.set_next_item_width)
                    ui.set_next_item_width(ui.user,140);
                if (ui.input_uint32(ui.user,StringView("##basket-" + key.first + "/" + key.second),&quantity,1,10) &&
                    ctx.selected_shop == key.first && ctx.selected_goods == key.second) ctx.quantity = quantity;
            } else text(ctx.engine.localizer.Format("goods.quantity_value","Quantity {0}",std::array<std::string_view,1>{std::to_string(quantity)}));
            line();
            if (ui.set_cursor_pos_x) ui.set_cursor_pos_x(ui.user,8);
            text(FitText(ui,label,quantity_x - 24));
            line();
            if (ui.set_cursor_pos_x) ui.set_cursor_pos_x(ui.user,quantity_x + 152);
            if (button(ctx.engine.localizer.Text("basket.remove","Remove") + "##basket-remove-" + key.first + "/" + key.second,!view.busy && !view.batch_active)) remove = key;
        }
        if (remove) ctx.basket.erase(*remove);
        ui.end_child(ui.user);
        return;
    }

    if (ctx.source_picker) {
        const auto group = ctx.browser.products.find(*ctx.source_picker);
        ui.separator(ui.user);
        if (button(ctx.engine.localizer.Text("source.close","Hide sources"))) ctx.source_picker.reset();
        if (group != ctx.browser.products.end() && ctx.source_picker) {
            const auto& product = group->second;
            text(ctx.engine.localizer.Format("source.title","Choose purchase source · {0}",std::array<std::string_view,1>{(*view.goods)[product.offers.front()].label}));
            const bool open = ui.begin_child(ui.user,StringView("product-sources"),0,160,1) != 0;
            if (open) for (auto i : product.offers) {
                const auto& item = (*view.goods)[i];
                const bool current = ctx.selected_shop == item.shop_id && ctx.selected_goods == item.id;
                const auto stock = item.stock ? ctx.engine.localizer.Format("goods.stock_value","Stock {0}",std::array<std::string_view,1>{Number(*item.stock)}) : ctx.engine.localizer.Text("source.stock_unknown","Stock unconfirmed");
                const auto label = std::string(current ? ctx.engine.localizer.Text("source.current","Current · ") : "") + item.shop_label + " · " + BranchLabel(item.shop_id,ctx.engine.localizer) + " · " + stock;
                if (button(label + "##source-" + item.shop_id + "/" + item.id,!view.busy && !view.batch_active)) select(item);
            }
            ui.end_child(ui.user);
            if (std::ranges::any_of(product.offers,[&](auto i){const auto& item = (*view.goods)[i];return ctx.basket.contains({item.shop_id,item.id});}))
                text(ctx.engine.localizer.Text("source.basket_retained","Products already in the batch list use the sources shown there."));
        } else if (ctx.source_picker) text(ctx.engine.localizer.Text("source.empty","No shops are currently available."));
    }

    const auto selected = std::ranges::find_if(*view.goods, [&](const Goods& item) {
        return item.shop_id == ctx.selected_shop && item.id == ctx.selected_goods;
    });
    if (view.ready && selected != view.goods->end()) {
        ui.separator(ui.user);
        if (ctx.icons.Draw(*selected,56)) line();
        text(selected->label + " · " + selected->shop_label + " · " + BranchLabel(selected->shop_id,ctx.engine.localizer));
        if (const auto group = ctx.browser.products.find(selected->Product()); group != ctx.browser.products.end() && group->second.offers.size() > 1) {
            if (button(ctx.engine.localizer.Text("source.choose","Choose purchase branch"))) ctx.source_picker = group->first;
        }
        text(ctx.engine.localizer.Format("goods.unit_price","Configured unit price {0} · {1}",std::array<std::string_view,2>{Number(selected->price),selected->currency_label}));
        if (ui.struct_size >= offsetof(AnomalyUiServiceV1,set_next_item_width) + sizeof(ui.set_next_item_width) && ui.set_next_item_width)
            ui.set_next_item_width(ui.user,180);
        if (ui.input_uint32(ui.user, StringView(ctx.engine.localizer.Text("goods.quantity","Purchase quantity")), &ctx.quantity, 1, 10) && !view.batch_active && ctx.basket.contains({selected->shop_id,selected->id}))
            ctx.basket[{selected->shop_id,selected->id}] = ctx.quantity;
        if (const auto total = TotalPrice(selected->price, ctx.quantity)) text(ctx.engine.localizer.Format("goods.total","Estimated total {0}",std::array<std::string_view,1>{Number(*total)}));
        if (selected->selector_max > 0) text(ctx.engine.localizer.Format("goods.selector_limit","Per-order selection limit {0}",std::array<std::string_view,1>{Number(selected->selector_max)}));
        if (selected->limit > 0) text(ctx.engine.localizer.Format("goods.purchase_limit","Configured purchase limit {0}",std::array<std::string_view,1>{Number(selected->limit)}));
        if (selected->stock) text(ctx.engine.localizer.Format("goods.stock","Stock remaining {0}",std::array<std::string_view,1>{std::to_string(*selected->stock)}));
        if (button(ctx.engine.localizer.Text("goods.purchase","Buy quantity"), !view.busy && !view.batch_active && ctx.quantity > 0)) {
            Queue(ctx, {false, selected->shop_id, selected->id, ctx.quantity, view.player});
        }
        const auto favorites = SavedProduct(ctx,selected->Product());
        line();
        if (favorites.empty()) {
            if (button(ctx.engine.localizer.Text("favorite.goods","Favorite this product"), ctx.favorites.writable && ctx.quantity > 0))
                ToggleProduct(ctx,*selected,ctx.quantity);
        } else if (ctx.favorites.value.goods.at(favorites.front()).quantity != ctx.quantity) {
            if (button(ctx.engine.localizer.Text("favorite.save_quantity","Save favorite quantity"), ctx.favorites.writable && ctx.quantity > 0)) {
                auto next = ctx.favorites.value;
                for (const auto& key : favorites) next.goods.erase(key);
                next.goods[{selected->shop_id,selected->id}] = {selected->shop_label,selected->label,ctx.quantity,selected->Product()};
                ctx.favorites.Save(std::move(next));
            }
        } else {
            text(ctx.engine.localizer.Format("favorite.quantity","Saved · quantity {0}",std::array<std::string_view,1>{std::to_string(ctx.favorites.value.goods.at(favorites.front()).quantity)}));
        }
        ui.separator(ui.user);
    }

    if (ctx.page == Page::Shops && ctx.open_shop.empty() && ctx.merchant.empty()) {
        text(ctx.engine.localizer.Format("merchant.count","Browse merchants · {0} brands",std::array<std::string_view,1>{std::to_string(ctx.browser.groups.size())}));
    }
    if (ctx.page == Page::Shops && !ctx.merchant.empty()) {
        if (button(ctx.engine.localizer.Text("merchant.back","Back to merchants"))) {
            ctx.merchant.clear(); ctx.open_shop.clear(); ctx.filter.fill(0); ctx.selected_goods.clear(); ctx.show_branches = false; ++ctx.catalog_revision;
        }
        if (!ctx.merchant.empty()) {
            text(ctx.merchant + (ctx.open_shop.empty() ? " · " + ctx.engine.localizer.Text("merchant.all","All branches") : " · " + BranchLabel(ctx.open_shop,ctx.engine.localizer)));
            const bool branch_mode = ctx.show_branches || !ctx.open_shop.empty();
            const auto branch_state = std::pair{ctx.merchant,branch_mode};
            const bool sync_branch = !ctx.synced_branch_tab || *ctx.synced_branch_tab != branch_state;
            std::optional<bool> active_branch;
            if (ui.begin_tab_bar(ui.user,StringView("merchant-branch-tabs"),ANOMALY_UI_TAB_BAR_V1_FITTING_POLICY_RESIZE_DOWN)) {
                const std::array<std::pair<bool,std::string>,2> tabs{{
                    {false,ctx.engine.localizer.Text("merchant.all","All branches") + "###merchant-all-branches"},
                    {true,(ctx.open_shop.empty() ? std::string(ctx.engine.localizer.Text("merchant.choose","Choose branch")) : BranchLabel(ctx.open_shop,ctx.engine.localizer)) + "###merchant-one-branch"}
                }};
                for (const auto& [mode,label] : tabs) {
                    const uint32_t flags = sync_branch && mode == branch_mode ? ANOMALY_UI_TAB_ITEM_V1_SET_SELECTED : ANOMALY_UI_TAB_ITEM_V1_NONE;
                    if (ui.begin_tab_item(ui.user,StringView(label),nullptr,flags,1)) {
                        active_branch = mode; ui.end_tab_item(ui.user);
                    }
                }
                ui.end_tab_bar(ui.user);
            }
            if (!sync_branch && active_branch && *active_branch != branch_mode) {
                if (*active_branch) ctx.show_branches = true;
                else {
                    ctx.open_shop.clear(); ctx.show_branches = false; ctx.selected_goods.clear(); ++ctx.catalog_revision;
                }
            }
            ctx.synced_branch_tab = std::pair{ctx.merchant,ctx.show_branches || !ctx.open_shop.empty()};
            if (ctx.show_branches) {
                if (button(ctx.engine.localizer.Text("merchant.hide_branches","Hide branches"))) ctx.show_branches = false;
            } else if (!ctx.open_shop.empty() && button(ctx.engine.localizer.Text("merchant.change_branch","Change branch"))) ctx.show_branches = true;
            const auto group = ctx.browser.groups.find(ctx.merchant);
            if (group != ctx.browser.groups.end()) {
                if (ctx.show_branches) {
                    const bool visible = ui.begin_child(ui.user,StringView("merchant-branches"),0,176,1) != 0;
                    size_t branch_index{};
                    if (visible) for (const auto& branch : group->second.branches) {
                        if (branch_index++ % merchant_grid.columns) ui.same_line(ui.user,0,CardGrid::Gap);
                        const auto title = std::string(branch.kind == 2 ? ctx.engine.localizer.Text("merchant.machine","Vending machine · ") : "") + BranchLabel(branch.id,ctx.engine.localizer);
                        const auto subtitle = ctx.engine.localizer.Format("goods.count","{0} products",std::array<std::string_view,1>{std::to_string(branch.count)}) +
                            (ctx.favorites.value.shops.contains(branch.id) ? " · " + ctx.engine.localizer.Text("favorite.saved","Saved") : "");
                        if (shop_card("branch-" + branch.id,title,subtitle,branch.icon)) {
                            ctx.open_shop = branch.id; ctx.selected_goods.clear(); ctx.show_branches = false; ++ctx.catalog_revision;
                        }
                    }
                    ui.end_child(ui.user);
                }
                if (!ctx.open_shop.empty()) {
                    const auto branch = ctx.browser.branches.find(ctx.open_shop);
                    if (branch != ctx.browser.branches.end()) favorite_shop(branch->second);
                } else if (group->second.branches.size() == 1) favorite_shop(group->second.branches.front());
            }
        }
    } else if (!ctx.open_shop.empty()) {
        if (button(ctx.page == Page::Favorites ? ctx.engine.localizer.Text("favorite.back","Back to favorites") : ctx.engine.localizer.Text("merchant.back_categories","Back to shops"))) {
            ctx.open_shop.clear(); ctx.selected_goods.clear(); ++ctx.catalog_revision;
        }
        if (const auto found = ctx.browser.branches.find(ctx.open_shop); found != ctx.browser.branches.end()) {
            text(found->second.name + " · " + BranchLabel(found->second.id,ctx.engine.localizer));
            favorite_shop(found->second);
        }
    }

    std::vector<size_t> results;
    std::vector<ProductGroup> product_results;
    const bool list_goods = ctx.page == Page::Search || !ctx.open_shop.empty() || (ctx.page == Page::Shops && !ctx.merchant.empty());
    if (list_goods) {
        if (ctx.page == Page::Shops && !ctx.merchant.empty()) results = ctx.browser.FindMerchant(*view.goods,ctx.merchant,ctx.filter.data(),ctx.open_shop);
        else if (ctx.page == Page::Search) results = ctx.browser.Find(*view.goods, ctx.filter.data());
        else for (size_t i = 0; i < view.goods->size(); ++i) if ((*view.goods)[i].shop_id == ctx.open_shop) results.push_back(i);
        product_results = MergeProducts(*view.goods,results);
        text(ctx.engine.localizer.Format("goods.scroll_count","{0} products · scroll to browse",std::array<std::string_view,1>{std::to_string(product_results.size())}));
    }

    // Reuse the same native child and card identities across searches. One empty
    // layout frame lets native scrolling clamp to the top without retaining a
    // separate set of thousands of child windows for every search string.
    ctx.scroll_reset_frame = ctx.catalog_initialized && ctx.rendered_catalog_revision != ctx.catalog_revision;
    ctx.rendered_catalog_revision = ctx.catalog_revision;
    ctx.catalog_initialized = true;
    const bool child_visible = ui.begin_child(ui.user, StringView("remote-shop-catalog"), 0, 0, 0) != 0;
    if (child_visible) {
        if (ctx.scroll_reset_frame) {
            text(ctx.engine.localizer.Text("status.updating_list","Updating list."));
        } else if (list_goods) {
            if (product_results.empty()) text(view.ready ? ctx.engine.localizer.Text("search.empty","No matching products. Try another keyword.") : ctx.engine.localizer.Text("status.waiting_goods","Waiting for products."));
            // Clip whole rows before visiting their cards. The fixed row extent
            // preserves the complete scroll range without laying out thousands
            // of individual child windows on every frame.
            for (size_t first = 0; first < product_results.size(); first += grid.columns) {
                const auto row_id = "catalog-row-" + std::to_string(first / grid.columns);
                const bool row_visible = ui.begin_child(ui.user,StringView(row_id),0,card_height + 8,0) != 0;
                if (row_visible) {
                    card_index = 0;
                    const auto end = (std::min)(product_results.size(),first + grid.columns);
                    for (auto i = first; i < end; ++i) goods_row(product_results[i]);
                }
                ui.end_child(ui.user);
            }
        } else if (ctx.page == Page::Favorites) {
            const auto favorites = ctx.favorites.value;
            if (favorites.Empty()) text(ctx.engine.localizer.Text("favorite.empty","No favorites yet. Favorite a product or shop to reopen it next time."));
            if (!favorites.goods.empty()) { ui.separator(ui.user); text(ctx.engine.localizer.Text("favorite.products","Favorite products")); }
            auto favorite_indices = ctx.browser.Find(*view.goods,"");
            std::erase_if(favorite_indices,[&](auto i){return SavedProduct(ctx,(*view.goods)[i].Product()).empty();});
            std::set<GoodsKey> shown_favorites;
            for (const auto& group : MergeProducts(*view.goods,favorite_indices)) {
                for (const auto& key : SavedProduct(ctx,group.key)) shown_favorites.insert(key);
                goods_row(group);
            }
            for (const auto& [key, saved] : favorites.goods) {
                if (shown_favorites.contains(key) || view.sold_out.contains(key)) continue;
                card_index = 0;
                if (button(ctx.engine.localizer.Text("favorite.remove","Remove favorite") + "##missing-" + key.first + "/" + key.second, ctx.favorites.writable))
                    ctx.favorites.ToggleGoods(key.first,key.second,saved.shop_name,saved.name,saved.quantity);
                line(); text(saved.name + " · " + saved.shop_name + ctx.engine.localizer.Text("favorite.unloaded"," (not loaded)"));
            }
            if (!favorites.shops.empty()) { ui.separator(ui.user); text(ctx.engine.localizer.Text("favorite.shops","Favorite shops")); }
            for (const auto& [id, saved] : favorites.shops) {
                const auto branch = ctx.browser.branches.find(id);
                if (button(ctx.engine.localizer.Text("favorite.remove","Remove favorite") + "##saved-shop-" + id, ctx.favorites.writable)) ctx.favorites.ToggleShop(id,saved.name);
                line();
                if (shop_icon(branch == ctx.browser.branches.end() ? IconSource{} : branch->second.icon,24)) line();
                if (button(saved.name + " · " + BranchLabel(id,ctx.engine.localizer) + (branch == ctx.browser.branches.end() ? ctx.engine.localizer.Text("favorite.unloaded"," (not loaded)") : "") +
                        "##open-saved-shop-" + id, branch != ctx.browser.branches.end())) {
                    ctx.open_shop = id; ctx.selected_goods.clear(); ++ctx.catalog_revision;
                }
            }
        } else {
            bool found{};
            size_t merchant_index{};
            for (const auto& key : ctx.browser.merchant_order) {
                const auto& group = ctx.browser.groups.at(key);
                if (Fold(group.name).find(Fold(ctx.merchant_filter.data())) == std::string::npos) continue;
                found = true;
                if (merchant_index++ % merchant_grid.columns) ui.same_line(ui.user,0,CardGrid::Gap);
                const auto subtitle = ctx.engine.localizer.Format("merchant.summary","{0} shops · {1} offers",std::array<std::string_view,2>{std::to_string(group.branches.size()),std::to_string(group.count)});
                if (shop_card("merchant-" + group.name,group.name,subtitle,group.icon)) {
                    ctx.selected_goods.clear(); ctx.merchant = group.name; ctx.open_shop.clear(); ctx.filter.fill(0); ctx.show_branches = false; ++ctx.catalog_revision;
                }
            }
            if (view.ready && !found) text(ctx.engine.localizer.Text("search.merchants_empty","No matching merchants. Try another keyword."));
            if (!view.ready) text(ctx.engine.localizer.Text("status.waiting_shops","Waiting for shops and products. Saved favorites remain available."));
        }
    }
    ui.end_child(ui.user);
    if (remove_favorite) RemoveProduct(ctx,*remove_favorite);
}

void ANOMALY_CALL Draw(void* raw, const AnomalyUiServiceV1* ui) noexcept {
    constexpr size_t required_size = offsetof(AnomalyUiServiceV1, end_tab_bar) + sizeof(AnomalyUiServiceV1::end_tab_bar);
    if (!ui || ui->struct_size < required_size || !ui->button_enabled || !ui->text || !ui->input_text ||
        !ui->input_uint32 || !ui->separator || !ui->begin_child || !ui->end_child || !ui->same_line || !ui->frame_state ||
        !ui->begin_tab_bar || !ui->begin_tab_item || !ui->end_tab_item || !ui->end_tab_bar) return;
    auto& ctx = *static_cast<Context*>(raw);
    int32_t visible{};
    if (ctx.stopping || !ctx.window.id || !Succeeded(ctx.windows->begin(ctx.windows->user, ctx.window, 0, &visible))) return;
    if (visible) {
        try { DrawContents(ctx, *ui); PersistBrowser(ctx); }
        catch (...) { ctx.Message(ctx.engine.localizer.Text("status.draw_failed","Could not draw the shop. Reload the plugin.")); }
    }
    ctx.windows->end(ctx.windows->user, ctx.window);
}
} // namespace shop

ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    using namespace shop;
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView(Id), StringView(Title), StringView("b1ank"), StringView(Version), Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
