#pragma once
#include "engine.hpp"
#include <anomaly/sdk/cpp.hpp>

namespace creative_mode {
// A catalog contains registry metadata, never live UObject addresses. Cache I/O,
// native registry enumeration, translation and sorting belong to scoped Worker.
class AssetCatalog {
public:
    enum class Phase { Cache, Prepare, Query, Ready, Failed };
    struct Snapshot {
        Phase phase{Phase::Cache};
        std::shared_ptr<const std::vector<Asset>> assets = std::make_shared<const std::vector<Asset>>();
        std::string error;
        bool cached{};
    };
    void Initialize(const AnomalyHostApiV1* host,const CatalogNames& names,const anomaly::sdk::Localizer& l10n) {
        const anomaly::sdk::Host api(host); names_ = &names; l10n_ = &l10n;
        scheduler_ = api.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
        storage_ = api.Query<AnomalyStorageServiceV1>(ANOMALY_STORAGE_SERVICE_V1_ID).get();
        build_ = api.Query<AnomalyUe5BuildServiceV1>(ANOMALY_UE5_BUILD_SERVICE_V1_ID).get();
        if (!scheduler_ || !scheduler_->schedule || !storage_ || !storage_->read || !storage_->write_atomic || !build_ || !build_->build_id) {
            Fail(l10n.Text("error.catalog_services","The asset catalog services are unavailable")); return;
        }
        Schedule();
    }
    std::shared_ptr<const Snapshot> State() const { return state_.load(); }
    void Refresh() {
        auto next = std::make_shared<Snapshot>(*State()); next->phase = Phase::Prepare; next->error.clear(); next->cached = false; state_.store(next);
    }
    void Start(Engine& engine,RegistryBinding binding) {
        engine_ = &engine; binding_ = binding;
        auto next = std::make_shared<Snapshot>(*State()); next->phase = Phase::Query; state_.store(next); Schedule();
    }
    void Prepare(Engine& engine) {
        try { Start(engine,engine.PrepareRegistry()); }
        catch (const std::exception& error) { Fail(error.what()); }
    }
    void Stop() { stop_ = true; }
private:
    void Schedule() {
        Handle task{};
        if (scheduler_->schedule(scheduler_->user,0,Run,this,&task).code != 0)
            Fail(l10n_->Text("error.catalog_worker","The asset catalog background task could not start"));
    }
    void Fail(std::string message) {
        auto next = std::make_shared<Snapshot>(*State()); next->phase = Phase::Failed; next->error = std::move(message); state_.store(next);
    }
    void Publish(std::vector<Asset> assets,bool cached) {
        for (auto& asset : assets) {
            asset.name = names_->Name(asset.path); asset.search = SearchText(asset.name.label+" "+asset.path);
        }
        std::sort(assets.begin(),assets.end(),[](const auto& a,const auto& b) { return a.path < b.path; });
        assets.erase(std::unique(assets.begin(),assets.end(),[](const auto& a,const auto& b) { return a.path == b.path; }),assets.end());
        auto next = std::make_shared<Snapshot>(); next->phase = Phase::Ready; next->cached = cached;
        next->assets = std::make_shared<const std::vector<Asset>>(std::move(assets)); state_.store(next);
    }
    nlohmann::json Read(std::string_view file) const {
        std::size_t size{}; const auto result = storage_->read(storage_->user,anomaly::sdk::StringView(file),{},&size);
        if ((result.code != 0 && result.code != ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) || !size || size > 4U*1024*1024) return {};
        std::vector<std::uint8_t> bytes(size);
        if (storage_->read(storage_->user,anomaly::sdk::StringView(file),{bytes.data(),bytes.size()},&size).code != 0 || size > bytes.size()) return {};
        return nlohmann::json::parse(bytes.begin(),bytes.begin()+size,nullptr,false);
    }
    bool Write(std::string_view file,const nlohmann::json& document) const {
        const auto bytes = document.dump();
        return storage_->write_atomic(storage_->user,anomaly::sdk::StringView(file),
            {reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()}).code == 0;
    }
    static std::string Part(std::size_t index) { return "asset-catalog-v1-part-"+std::to_string(index)+".json"; }
    bool LoadCache() {
        // Host storage admits at most 4 MiB per entry. Each shard contains at
        // most 4096 bounded paths; the small index is committed last.
        try {
            const auto index = Read("asset-catalog-v1-index.json");
            if (!index.is_object() || index.value("schemaVersion",0) != 1 || index.value("build",std::string{}) != build_id_) return false;
            const auto count = index.at("count").get<std::size_t>(), parts = index.at("parts").get<std::size_t>();
            if (!count || count > 2097152 || parts != (count+4095)/4096 || parts > 512) return false;
            std::vector<Asset> assets; assets.reserve(count);
            for (std::size_t part = 0; part < parts; ++part) {
                const auto items = Read(Part(part)); if (!items.is_array() || items.size() > 4096) return false;
                for (const auto& item : items) {
                    if (!item.is_array() || item.size() != 2 || !item[0].is_string() || !item[1].is_boolean()) return false;
                    auto path = item[0].get<std::string>(); if (!AssetPath(path)) return false;
                    assets.push_back({std::move(path),item[1].get<bool>(),{}});
                }
            }
            if (assets.size() != count) return false;
            Publish(std::move(assets),true); return true;
        } catch (const nlohmann::json::exception&) { return false; }
    }
    void Work() {
        if (State()->phase == Phase::Cache) {
            std::array<char,512> text{}; std::size_t length = text.size();
            if (build_->build_id(build_->user,text.data(),&length).code == 0 && length > 1 && length <= text.size()) build_id_.assign(text.data(),length-1);
            // The active Host deliberately publishes no PE fingerprint. The
            // executable/archive metadata remains a Worker-only cache key.
            else build_id_ = NativeContentVersion();
            if (build_id_.empty()) throw std::runtime_error(l10n_->Text("error.catalog_version","The game version for the asset cache is unavailable"));
            if (LoadCache()) return;
            Refresh(); return;
        }
        auto assets = engine_->RegistryAssets(binding_,stop_);
        if (stop_.load()) return;
        const auto count = assets.size(), parts = (count+4095)/4096; bool saved = true;
        for (std::size_t part = 0; part < parts && saved; ++part) {
            auto entries = nlohmann::json::array();
            for (std::size_t i = part*4096; i < std::min(count,(part+1)*4096); ++i) entries.push_back({assets[i].path,assets[i].skeletal});
            saved = Write(Part(part),entries);
        }
        if (saved) saved = Write("asset-catalog-v1-index.json",{{"schemaVersion",1},{"build",build_id_},{"parts",parts},{"count",count}});
        Publish(std::move(assets),false);
        if (!saved) {
            auto next = std::make_shared<Snapshot>(*State());
            next->error = l10n_->Text("error.catalog_save","The asset catalog is ready, but its cache could not be saved"); state_.store(next);
        }
    }
    static void ANOMALY_CALL Run(void* raw,Handle) noexcept {
        auto& self = *static_cast<AssetCatalog*>(raw);
        try { self.Work(); }
        catch (const std::exception& error) { try { self.Fail(error.what()); } catch (...) {} }
        catch (...) { try { self.Fail(self.l10n_->Text("error.catalog_api","The asset registry is unavailable")); } catch (...) {} }
    }
    const AnomalySchedulerServiceV1* scheduler_{};
    const AnomalyStorageServiceV1* storage_{};
    const AnomalyUe5BuildServiceV1* build_{};
    const CatalogNames* names_{};
    const anomaly::sdk::Localizer* l10n_{};
    Engine* engine_{};
    RegistryBinding binding_;
    std::string build_id_;
    std::atomic<bool> stop_{};
    std::atomic<std::shared_ptr<const Snapshot>> state_{std::make_shared<const Snapshot>()};
};
}
