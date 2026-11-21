#pragma once
#include "native.hpp"
#include "icon_pixels.hpp"
#include <anomaly/sdk/services/ui_resources.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <chrono>
#include <span>
#include <atomic>

namespace shop {
class CardControls {
    std::array<Handle,6> handles{};
    float row_height{};
public:
    const AnomalyTextureServiceV1* service{};
    void Release() {
        if (service) for (auto& handle : handles) if (handle.id) { service->release(service->user,handle); handle = {}; }
        row_height = 0;
    }
    void Prepare(float height) {
        if (!service || height == row_height) return;
        Release(); row_height = height;
        for (size_t i = 0; i < handles.size(); ++i) {
            const auto pixels = i == 5 ? std::array<uint8_t,32 * 32 * 4>{} : i == 2 ? CloseIconPixels() : i >= 3 ? StarIconPixels(i == 4) : CheckboxIconPixels(i == 1);
            const auto bitmap = CenterControlIcon(pixels,height);
            if (bitmap.pixels.empty()) continue;
            AnomalyTextureRequestV1 request{sizeof(request)};
            request.format = ANOMALY_TEXTURE_FORMAT_V1_RGBA8; request.width = 32; request.height = bitmap.height;
            request.encoded_bytes = {bitmap.pixels.data(),bitmap.pixels.size()};
            service->request(service->user,&request,&handles[i]);
        }
    }
    bool DrawSized(size_t kind,float width,float height) const {
        if (!service || kind >= handles.size() || !handles[kind].id) return false;
        AnomalyTextureStateV1 state{sizeof(state)};
        return Succeeded(service->state(service->user,handles[kind],&state)) && (state.flags & ANOMALY_TEXTURE_STATE_V1_READY) &&
            Succeeded(service->draw(service->user,handles[kind],width,height,0xffffffff));
    }
    bool Draw(size_t kind) const { return DrawSized(kind,20,row_height); }
    bool Space(float height) const { return height > 0 && DrawSized(5,1,height); }
};
inline size_t PngLength(std::span<const uint8_t> bytes) {
    constexpr uint8_t signature[]{137,80,78,71,13,10,26,10};
    if (bytes.size() < 20 || !std::equal(std::begin(signature),std::end(signature),bytes.begin())) return 0;
    for (size_t offset = 8; offset <= bytes.size() - 12;) {
        const auto length = (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
            (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
        if (length > bytes.size() - offset - 12) return 0;
        const auto end = offset + 12 + length;
        if (std::memcmp(bytes.data() + offset + 4,"IEND",4) == 0) return length == 0 ? end : 0;
        offset = end;
    }
    return 0;
}
class IconCache {
    struct Entry {
        Name package{}, asset{};
        std::filesystem::path file;
        Handle texture{};
        uint64_t started{}, used{}, last_read{};
        bool queued{}, exported{}, failed{};
    };
    std::mutex mutex;
    std::map<std::string,Entry> entries;
    std::filesystem::path root;
    uint64_t next_tick{}, sequence{};
    uint64_t next_file_poll{};
    std::atomic<bool> worker_pending{};
    bool available{true};
    static uint64_t Clock() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static uint64_t Hash(std::string_view path) {
        uint64_t result = 14695981039346656037ull;
        for (unsigned char c : path) result = (result ^ c) * 1099511628211ull;
        return result;
    }
public:
    const AnomalyTextureServiceV1* service{};
    const AnomalySchedulerServiceV1* scheduler{};
    void Initialize() {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH,temporary)) { available = false; return; }
        root = std::filesystem::path(temporary) / L"AnomalyRemoteShopIcons" / std::to_wstring(GetCurrentProcessId());
        std::error_code error; std::filesystem::create_directories(root,error);
        if (error) available = false;
    }
    void ReadPending() {
        std::string key; Entry job;
        {
            std::scoped_lock lock(mutex);
            if (!available) return;
            for (const auto& [path,entry] : entries) if (entry.exported && !entry.failed && !entry.texture.id &&
                (key.empty() || entry.last_read < job.last_read)) {
                key = path; job = entry;
            }
            if (!key.empty()) entries.at(key).last_read = Clock();
        }
        if (key.empty()) return;
        std::vector<uint8_t> pixels;
        std::ifstream file(job.file,std::ios::binary | std::ios::ate);
        const auto length = file ? std::streamoff(file.tellg()) : 0;
        if (length > 20 && length < 8 * 1024 * 1024) {
            std::vector<uint8_t> bytes(static_cast<size_t>(length)); file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),length);
            if (const auto png_length = file ? PngLength(bytes) : 0) {
                bytes.resize(png_length); pixels = NormalizeIcon(bytes);
            }
        }
        Handle texture{};
        if (!pixels.empty()) {
            AnomalyTextureRequestV1 request{sizeof(request)}; request.encoded_bytes = {pixels.data(),pixels.size()};
            request.format = ANOMALY_TEXTURE_FORMAT_V1_RGBA8; request.width = request.height = 128;
            service->request(service->user,&request,&texture);
        }
        std::scoped_lock lock(mutex);
        const auto found = entries.find(key);
        if (!available || found == entries.end()) {
            if (texture.id) service->release(service->user,texture);
            return;
        }
        found->second.texture = texture;
        if (!texture.id && Clock() - job.started > 20000) found->second.failed = true;
    }
    void SchedulePending() {
        if (!service || !scheduler || worker_pending.load()) return;
        {
            std::scoped_lock lock(mutex);
            if (!available || Clock() < next_file_poll || !std::ranges::any_of(entries,[](const auto& row) {
                const auto& entry = row.second; return entry.exported && !entry.failed && !entry.texture.id;
            })) return;
            next_file_poll = Clock() + 120;
        }
        if (worker_pending.exchange(true)) return;
        Handle task{};
        const auto status = scheduler->schedule(scheduler->user,0,[](void* raw,Handle) noexcept {
            auto& cache = *static_cast<IconCache*>(raw);
            try { cache.ReadPending(); } catch (...) {}
            cache.worker_pending = false;
        },this,&task);
        if (!Succeeded(status)) worker_pending = false;
    }
    void Request(const IconSource& item) {
        if (!service || item.icon_path.empty()) return;
        std::scoped_lock lock(mutex);
        if (!available || root.empty()) return;
        if (!entries.contains(item.icon_path) && entries.size() >= 128) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if ((it->second.texture.id || it->second.failed || (it->second.queued && Clock() - it->second.started > 2000)) &&
                    (oldest == entries.end() || it->second.used < oldest->second.used)) oldest = it;
            if (oldest == entries.end()) return;
            if (oldest->second.texture.id) service->release(service->user,oldest->second.texture);
            entries.erase(oldest);
        }
        auto [entry,fresh] = entries.try_emplace(item.icon_path);
        if (fresh) {
            entry->second.package = item.icon_package; entry->second.asset = item.icon_asset;
            entry->second.file = root / (std::to_wstring(Hash(item.icon_path + "#alpha-v1")) + L".png");
        }
        entry->second.used = ++sequence;
    }
    void Request(const Goods& item) {
        Request(IconSource{item.icon_package,item.icon_asset,item.icon_path});
    }
    void Tick(Engine& engine) {
        const auto now = Clock();
        if (now < next_tick) return;
        next_tick = now + 120;
        std::string key;
        Entry job;
        {
            std::scoped_lock lock(mutex);
            if (!available) return;
            for (auto& [path,entry] : entries) if (!entry.queued && !entry.failed) {
                entry.queued = true; entry.started = now; key = path; job = entry; break;
            }
        }
        if (key.empty()) return;
        const auto texture = engine.LoadAsset(job.package,job.asset);
        bool ok = engine.IsA(texture,"Texture2D");
        uintptr_t target{};
        const auto renderer = engine.Find("/Script/Engine.Default__KismetRenderingLibrary");
        if (ok) {
            // Cooked icons are GPU-compressed. Draw into an RGBA target before PNG export.
            std::string error;
            struct Create { uintptr_t world{}; int32_t width{128},height{128}; uint8_t format{2},pad[3]{}; float clear[4]{}; uint8_t mip{},uav{},pad2[2]{}; uintptr_t result{}; } create;
            static_assert(sizeof(Create) == 48);
            create.world = engine.PlayerState(error);
            ok = create.world && engine.Invoke(renderer,engine.Find("/Script/Engine.KismetRenderingLibrary.CreateRenderTarget2D"),&create,sizeof(create)) && engine.IsA(create.result,"TextureRenderTarget2D");
            target = create.result;
            if (ok) {
                struct Begin { uintptr_t world{},target{},canvas{}; double size[2]{}; uint64_t context{}; } begin;
                static_assert(sizeof(Begin) == 48);
                begin.world = create.world; begin.target = target;
                ok = engine.Invoke(renderer,engine.Find("/Script/Engine.KismetRenderingLibrary.BeginDrawCanvasToRenderTarget"),&begin,sizeof(begin)) && engine.IsA(begin.canvas,"Canvas");
                if (ok) {
                    struct Draw { uintptr_t texture{}; double position[2]{},size[2]{128,128},uv[2]{},uv_size[2]{1,1}; float tint[4]{1,1,1,1}; uint8_t blend{1},padding[3]{}; float rotation{}; double pivot[2]{}; } draw;
                    static_assert(sizeof(Draw) == 112);
                    draw.texture = texture;
                    const auto draw_function = engine.Find("/Script/Engine.Canvas.K2_DrawTexture");
                    ok = engine.Invoke(engine.ObjectHandle(begin.canvas),draw_function,&draw,sizeof(draw));
                }
                if (begin.context) {
                    struct End { uintptr_t world{}; uint64_t context{}; } end{create.world,begin.context};
                    const auto ended = engine.Invoke(renderer,engine.Find("/Script/Engine.KismetRenderingLibrary.EndDrawCanvasToRenderTarget"),&end,sizeof(end));
                    ok = ok && ended;
                }
            }
        }
        if (ok) {
            struct ExportParams { uintptr_t world{},texture{}; Array directory{},filename{}; } output;
            static_assert(sizeof(output) == 48);
            auto filename = job.file.filename().wstring(), directory = job.file.parent_path().wstring();
            std::string error; output.world = engine.PlayerState(error); output.texture = target;
            output.directory = {reinterpret_cast<uintptr_t>(directory.data()),int32_t(directory.size() + 1),int32_t(directory.size() + 1)};
            output.filename = {reinterpret_cast<uintptr_t>(filename.data()),int32_t(filename.size() + 1),int32_t(filename.size() + 1)};
            ok = engine.Invoke(renderer,engine.Find("/Script/Engine.KismetRenderingLibrary.ExportRenderTarget"),&output,sizeof(output));
        }
        if (target) engine.Invoke(renderer,engine.Find("/Script/Engine.KismetRenderingLibrary.ReleaseRenderTarget2D"),&target,sizeof(target));
        if (engine.core->log) engine.core->log(engine.core->user,ANOMALY_CORE_LOG_LEVEL_V1_INFO,
            anomaly::sdk::StringView("RemoteShop icon export " + key + (ok ? " queued" : " failed")));
        { std::scoped_lock lock(mutex); if (entries.contains(key)) { entries.at(key).failed = !ok; entries.at(key).exported = ok; } }
    }
    bool Draw(const IconSource& item, float size) {
        Request(item);
        std::scoped_lock lock(mutex);
        const auto found = entries.find(item.icon_path);
        if (found == entries.end() || found->second.failed || !found->second.queued) return false;
        auto& entry = found->second;
        if (!entry.texture.id) return false;
        AnomalyTextureStateV1 state{sizeof(state)};
        return Succeeded(service->state(service->user,entry.texture,&state)) && (state.flags & ANOMALY_TEXTURE_STATE_V1_READY) &&
            Succeeded(service->draw(service->user,entry.texture,size,size,0xffffffff));
    }
    bool Draw(const Goods& item, float size) {
        return Draw(IconSource{item.icon_package,item.icon_asset,item.icon_path},size);
    }
    void Release() {
        std::scoped_lock lock(mutex);
        available = false;
        for (const auto& [key,entry] : entries) if (entry.texture.id) service->release(service->user,entry.texture);
        entries.clear();
    }
};
}
