#pragma once
#include <anomaly/sdk/cpp.hpp>
#include <anomaly/sdk/services/platform.h>
#include <anomaly/sdk/services/core.h>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <atomic>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "product.hpp"
#include <anomaly/sdk/localization.hpp>

namespace shop {
struct FavoriteShop { std::string name; };
struct FavoriteGoods {
    std::string shop_name, name;
    uint32_t quantity{1};
    std::optional<ProductKey> product;
};
using GoodsKey = std::pair<std::string, std::string>;
struct BrowserSettings {
    int page{1};
    uint32_t quantity{1};
    std::string filter,merchant_filter,merchant,shop,selected_shop,selected_goods;
    std::vector<GoodsKey> sources;
    bool operator==(const BrowserSettings&) const = default;
};
struct Favorites {
    std::map<std::string, FavoriteShop> shops;
    std::map<GoodsKey, FavoriteGoods> goods;
    std::optional<BrowserSettings> browser;
    bool Empty() const { return shops.empty() && goods.empty(); }
};

class FavoriteStore {
    static constexpr auto Id = "favorites";
    static constexpr uint32_t Version = 1;
    static constexpr size_t MaxBytes = 2 * 1024 * 1024;
    static constexpr size_t MaxEntries = 4096;
    static AnomalyByteSpanV1 Bytes(std::string_view value) {
        return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
    }
    static bool ValidId(const std::string& id) {
        return !id.empty() && id.size() <= 256 && id.find('\0') == std::string::npos;
    }
    struct SaveJob { Favorites value; std::string json; uint64_t revision{}; bool quiet{}; };
    struct SaveResult { uint64_t revision{}; bool success{}; Favorites durable; bool quiet{}; };
    std::mutex queue_mutex,io_mutex;
    std::optional<SaveJob> pending;
    std::optional<SaveResult> completed;
    Favorites durable;
    uint64_t revision{};
    std::atomic<bool> task_scheduled{};
    static void ANOMALY_CALL SaveTask(void* raw,AnomalyGenerationHandleV1) noexcept {
        auto& store = *static_cast<FavoriteStore*>(raw);
        try { store.CommitPending(); }
        catch (...) {
            if (store.core && store.core->log) store.core->log(store.core->user,ANOMALY_CORE_LOG_LEVEL_V1_ERROR,anomaly::sdk::StringView("RemoteShop favorites worker failed"));
        }
        store.task_scheduled = false;
    }
public:
    const anomaly::sdk::Localizer* localizer{};
    std::string Text(std::string_view key,std::string_view fallback) const {
        return localizer ? localizer->Text(key,fallback) : std::string(fallback);
    }
    const AnomalyConfigServiceV1* config{};
    const AnomalyCoreServiceV1* core{};
    const AnomalySchedulerServiceV1* scheduler{};
    Favorites value;
    bool writable{};
    std::string message;
    static constexpr std::string_view Schema = R"({"type":"object","additionalProperties":false,"required":["shops","goods"],"properties":{
      "browser":{"type":"object","additionalProperties":false,"required":["filter","merchantFilter","merchant","shop","selectedShop","selectedGoods","page","quantity","sources"],"properties":{"filter":{"type":"string","maxLength":127},"merchantFilter":{"type":"string","maxLength":127},"merchant":{"type":"string","maxLength":2048},"shop":{"type":"string","maxLength":256},"selectedShop":{"type":"string","maxLength":256},"selectedGoods":{"type":"string","maxLength":256},"page":{"type":"integer","minimum":0,"maximum":3},"quantity":{"type":"integer","minimum":0,"maximum":4294967295},"sources":{"type":"array","maxItems":4096,"items":{"type":"object","additionalProperties":false,"required":["shop","id"],"properties":{"shop":{"type":"string","minLength":1,"maxLength":256},"id":{"type":"string","minLength":1,"maxLength":256}}}}}},
      "shops":{"type":"array","maxItems":4096,"items":{"type":"object","additionalProperties":false,"required":["id","name"],"properties":{"id":{"type":"string","minLength":1,"maxLength":256},"name":{"type":"string","maxLength":2048}}}},
      "goods":{"type":"array","maxItems":4096,"items":{"type":"object","additionalProperties":false,"required":["shop","id","shopName","name","quantity"],"properties":{"shop":{"type":"string","minLength":1,"maxLength":256},"id":{"type":"string","minLength":1,"maxLength":256},"shopName":{"type":"string","maxLength":2048},"name":{"type":"string","maxLength":2048},"quantity":{"type":"integer","minimum":1,"maximum":4294967295},"product":{"type":"object","additionalProperties":false,"required":["item","display","currency","price"],"properties":{"item":{"type":"string","minLength":1,"maxLength":1024},"display":{"type":"string","maxLength":256},"currency":{"type":"string","maxLength":256},"price":{"type":"integer"}}}}}}
    }})";

    bool Load() {
        writable = false;
        value = {}; durable = {};
        AnomalyGenerationHandleV1 schema{};
        if (!config || !anomaly::sdk::Succeeded(config->register_schema(config->user, anomaly::sdk::StringView(Id),
                Version, Bytes(Schema), &schema))) {
            message = Text("favorite.unavailable","Favorites service is unavailable."); return false;
        }
        size_t size{};
        uint32_t version{};
        auto status = config->read(config->user, anomaly::sdk::StringView(Id), &version, {nullptr, 0}, &size);
        if (status.code == ANOMALY_STATUS_V1_NOT_FOUND) {
            writable = true; message.clear(); return true;
        }
        if ((status.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL && !anomaly::sdk::Succeeded(status)) ||
            !size || size > MaxBytes || version != Version) {
            message = Text("favorite.read_failed","Could not read favorites. The original file was kept."); return false;
        }
        std::vector<uint8_t> buffer(size);
        status = config->read(config->user, anomaly::sdk::StringView(Id), &version, {buffer.data(), buffer.size()}, &size);
        if (!anomaly::sdk::Succeeded(status) || size > buffer.size() || version != Version) {
            message = Text("favorite.read_failed","Could not read favorites. The original file was kept."); return false;
        }
        try {
            const auto json = nlohmann::json::parse(buffer.begin(), buffer.begin() + size);
            const auto& shops = json.at("shops"); const auto& goods = json.at("goods");
            if (!shops.is_array() || !goods.is_array() || shops.size() > MaxEntries || goods.size() > MaxEntries)
                throw std::runtime_error("invalid favorite lists");
            Favorites loaded;
            if (json.contains("browser")) {
                const auto& row = json.at("browser");
                BrowserSettings browser;
                browser.page = row.at("page").get<int>(); browser.quantity = row.at("quantity").get<uint32_t>();
                browser.filter = row.at("filter").get<std::string>(); browser.merchant_filter = row.at("merchantFilter").get<std::string>();
                browser.merchant = row.at("merchant").get<std::string>(); browser.shop = row.at("shop").get<std::string>();
                browser.selected_shop = row.at("selectedShop").get<std::string>(); browser.selected_goods = row.at("selectedGoods").get<std::string>();
                for (const auto& source : row.at("sources")) browser.sources.emplace_back(source.at("shop").get<std::string>(),source.at("id").get<std::string>());
                if (browser.page < 0 || browser.page > 3 || browser.filter.size() >= 128 || browser.merchant_filter.size() >= 128 ||
                    browser.merchant.size() > 2048 || browser.shop.size() > 256 || browser.selected_shop.size() > 256 || browser.selected_goods.size() > 256 ||
                    browser.sources.size() > MaxEntries) throw std::runtime_error("invalid browser preferences");
                for (const auto& source : browser.sources) if (!ValidId(source.first) || !ValidId(source.second)) throw std::runtime_error("invalid preferred source");
                loaded.browser = std::move(browser);
            }
            for (const auto& row : shops) {
                const auto id = row.at("id").get<std::string>();
                const auto name = row.at("name").get<std::string>();
                if (!ValidId(id) || name.size() > 2048 || !loaded.shops.emplace(id, FavoriteShop{name}).second)
                    throw std::runtime_error("invalid favorite shop");
            }
            for (const auto& row : goods) {
                const auto shop = row.at("shop").get<std::string>(); const auto id = row.at("id").get<std::string>();
                const auto name = row.at("name").get<std::string>(); const auto shop_name = row.at("shopName").get<std::string>();
                const auto& quantity = row.at("quantity");
                FavoriteGoods saved{shop_name,name,0};
                if (row.contains("product")) {
                    const auto& product = row.at("product");
                    const auto& price = product.at("price");
                    if (!price.is_number_integer() || (price.is_number_unsigned() && price.get<uint64_t>() > INT64_MAX))
                        throw std::runtime_error("invalid favorite price");
                    saved.product = ProductKey{product.at("item").get<std::string>(),product.at("display").get<std::string>(),
                        product.at("currency").get<std::string>(),price.get<int64_t>()};
                    if (saved.product->item.empty() || saved.product->item.size() > 1024 || saved.product->display.size() > 256 ||
                        saved.product->currency.size() > 256)
                        throw std::runtime_error("invalid favorite product");
                }
                if (!ValidId(shop) || !ValidId(id) || name.size() > 2048 || shop_name.size() > 2048 ||
                    !quantity.is_number_unsigned() || quantity.get<uint64_t>() == 0 || quantity.get<uint64_t>() > UINT32_MAX ||
                    loaded.goods.contains({shop,id}))
                    throw std::runtime_error("invalid favorite goods");
                saved.quantity = quantity.get<uint32_t>();
                loaded.goods.emplace(GoodsKey{shop,id},std::move(saved));
            }
            value = std::move(loaded); durable = value; writable = true; message.clear(); return true;
        } catch (const std::exception&) {
            message = Text("favorite.corrupt","Favorites are invalid. The original file was kept."); return false;
        }
    }
    bool Save(Favorites next,bool quiet = false) {
        if (!writable) { message = Text("favorite.readonly","Favorites could not be loaded. Saving is disabled; the original file was kept."); return false; }
        if (next.shops.size() > MaxEntries || next.goods.size() > MaxEntries) {
            message = Text("favorite.limit","Favorites limit reached."); return false;
        }
        auto shops = nlohmann::json::array(), goods = nlohmann::json::array();
        for (const auto& [id, row] : next.shops) shops.push_back({{"id",id},{"name",row.name}});
        for (const auto& [key, row] : next.goods) {
            nlohmann::json entry{{"shop",key.first},{"id",key.second},{"shopName",row.shop_name},{"name",row.name},{"quantity",row.quantity}};
            if (row.product) entry["product"] = {{"item",row.product->item},{"display",row.product->display},
                {"currency",row.product->currency},{"price",row.product->price}};
            goods.push_back(std::move(entry));
        }
        nlohmann::json document{{"shops",std::move(shops)},{"goods",std::move(goods)}};
        if (next.browser) {
            const auto& browser = *next.browser;
            auto sources = nlohmann::json::array();
            for (const auto& source : browser.sources) sources.push_back({{"shop",source.first},{"id",source.second}});
            document["browser"] = {{"page",browser.page},{"quantity",browser.quantity},{"filter",browser.filter},
                {"merchantFilter",browser.merchant_filter},{"merchant",browser.merchant},{"shop",browser.shop},
                {"selectedShop",browser.selected_shop},{"selectedGoods",browser.selected_goods},{"sources",std::move(sources)}};
        }
        const auto json = document.dump();
        if (json.size() > MaxBytes) { message = Text("favorite.too_large","Favorites are too large to save."); return false; }
        {
            std::scoped_lock lock(queue_mutex);
            quiet = quiet && (!pending || pending->quiet);
            pending = SaveJob{next,json,++revision,quiet};
        }
        value = std::move(next); if (!quiet) message = Text("favorite.saving","Saving favorites."); return true;
    }
    // Game/update may enqueue work, but configuration I/O belongs to the Worker task.
    void SchedulePending() {
        { std::scoped_lock lock(queue_mutex); if (!pending) return; }
        if (task_scheduled.exchange(true)) return;
        AnomalyGenerationHandleV1 task{};
        if (scheduler && anomaly::sdk::Succeeded(scheduler->schedule(scheduler->user,0,SaveTask,this,&task))) return;
        std::scoped_lock io_lock(io_mutex);
        std::scoped_lock lock(queue_mutex);
        if (pending) { completed = SaveResult{pending->revision,false,durable}; pending.reset(); }
        task_scheduled = false;
    }
    void CommitPending() {
        std::scoped_lock io_lock(io_mutex);
        std::optional<SaveJob> job;
        { std::scoped_lock lock(queue_mutex); job = std::move(pending); pending.reset(); }
        if (!job) return;
        const auto status = config->write_atomic(config->user,anomaly::sdk::StringView(Id),Version,Bytes(job->json));
        const bool success = anomaly::sdk::Succeeded(status);
        if (!success) {
            if (core && core->log) {
                const auto detail = status.message.data ? std::string(status.message.data,(std::min)(status.message.size,size_t(2048))) : std::string{};
                const auto text = "RemoteShop favorites write failed code=" + std::to_string(status.code) + " detail=" + detail;
                core->log(core->user,ANOMALY_CORE_LOG_LEVEL_V1_ERROR,anomaly::sdk::StringView(text));
            }
        } else {
            durable = std::move(job->value);
            if (core && core->log) core->log(core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,anomaly::sdk::StringView("RemoteShop favorites saved revision=" + std::to_string(job->revision)));
        }
        std::scoped_lock lock(queue_mutex);
        completed = SaveResult{job->revision,success,success ? Favorites{} : durable,job->quiet};
    }
    // Render callback only: apply acknowledgments without touching the filesystem.
    void PollPending() {
        std::optional<SaveResult> result;
        { std::scoped_lock lock(queue_mutex); result = std::move(completed); completed.reset(); }
        if (!result || result->revision != revision) return;
        if (result->success) { if (!result->quiet) message = Text("favorite.saved_restart","Favorites saved and will restore on restart."); }
        else { value = std::move(result->durable); message = Text("favorite.save_failed","Could not save favorites. Restored the last saved version; please retry."); }
    }
    bool ToggleShop(const std::string& id, const std::string& name) {
        auto next = value;
        if (!next.shops.erase(id)) next.shops.emplace(id, FavoriteShop{name});
        return Save(std::move(next));
    }
    bool ToggleGoods(const std::string& shop, const std::string& id, const std::string& shop_name,
                     const std::string& name, uint32_t quantity) {
        auto next = value;
        const GoodsKey key{shop, id};
        if (!next.goods.erase(key)) next.goods.emplace(key, FavoriteGoods{shop_name,name,quantity ? quantity : 1});
        return Save(std::move(next));
    }
    bool SetQuantity(const GoodsKey& key, uint32_t quantity) {
        if (!quantity || !value.goods.contains(key)) return false;
        auto next = value; next.goods.at(key).quantity = quantity;
        return Save(std::move(next));
    }
    bool RemoveGoods(const GoodsKey& key) {
        if (!value.goods.contains(key)) return true;
        auto next = value; next.goods.erase(key);
        return Save(std::move(next));
    }
};
} // namespace shop
