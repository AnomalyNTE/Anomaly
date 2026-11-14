#include "world_reload.hpp"
#include "visual_reload.hpp"
#include <anomaly/sdk/localization.hpp>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace anomaly::sdk;
using Reload = mod_manager::WorldReload;
using Visual = mod_manager::VisualReload;

struct Row {
    std::string key, name;
    std::vector<std::string> groups;  // the categories the mod replaces, one section per set of them
    std::string skins;                // the variants inside those categories, shown on the mod's row
    bool enabled{}, disabled{}, complete{};
    bool operator==(const Row&) const = default;
};
struct RowCollection { std::vector<Row> values; bool failed{}; };
struct AssetCollection { std::set<std::string> values; bool failed{}; };
enum class Kind { Toggle, ReloadWorld };
struct Action { Kind kind; std::string key; bool enabled{}; };
struct Prepared { bool reload_world{}; std::vector<std::string> packages; };

struct Context {
    Localizer localizer;
    const AnomalyHostApiV1* api{};
    const AnomalyModsServiceV1* mods{};
    const AnomalySchedulerServiceV1* scheduler{};
    const AnomalyWindowServiceV1* windows{};
    AnomalyGenerationHandleV1 task{}, window{};
    std::mutex mutex;
    bool stopping{true}, busy{}, compatibility{}, profile_valid{}, services_ready{}, visual_ready{}, catalog_seen{};
    const AnomalyNteSessionServiceV1* session{};
    std::set<std::string> pending_visual;
    std::map<std::string, std::set<std::string>> visual_index;
    std::vector<Row> rows;
    std::optional<Action> action;
    std::optional<Prepared> prepared;
    std::string status;
    // The Chinese names of the categories, keyed by the container identifier they classify. The
    // table is read once at load, so the panel never touches the file system while drawing.
    std::map<std::string, std::string, std::less<>> category_names;
    // The search box. Only the draw callback reads and writes it, so it needs no lock.
    std::string filter;
    mod_manager::NativeAccess native;
    Reload reload;
    Visual visual;
} g;

std::string Copy(AnomalyStringViewV1 value) { return value.data ? std::string(value.data, value.size) : std::string{}; }
std::string ErrorText(AnomalyStatusV1 value) { return value.message.size ? Copy(value.message) : std::to_string(value.code); }
// The ABI carries the classification as `group/skin` tokens separated by ", ". The panel names the
// group in the section header and the variants on the mod's own row, so two outfits of one character
// stay one section instead of becoming a section of their own. Content directories are the only
// tokens that start with '/', which is what tells a group apart from its skin without guessing.
std::pair<std::vector<std::string>, std::string> SplitGroups(const std::string_view text) {
    std::vector<std::string> groups, skins;
    std::size_t at{};
    while (at < text.size()) {
        auto end = text.find(',', at);
        if (end == std::string_view::npos) end = text.size();
        auto token = text.substr(at, end - at);
        at = end + 1;
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (token.empty()) continue;
        auto group = token;
        std::string_view skin;
        if (token.front() != '/') {
            if (const auto slash = token.find('/'); slash != std::string_view::npos) {
                group = token.substr(0, slash);
                skin = token.substr(slash + 1);
            }
        }
        const std::string value(group);
        if (std::find(groups.begin(), groups.end(), value) == groups.end()) groups.push_back(value);
        if (!skin.empty() && std::find(skins.begin(), skins.end(), skin) == skins.end())
            skins.emplace_back(skin);
    }
    std::string variants;
    for (const auto& skin : skins) {
        if (!variants.empty()) variants += " · ";
        variants += skin;
    }
    return {std::move(groups), std::move(variants)};
}
void LocalizedStatus(std::string_view key, std::string_view fallback, std::string_view detail = {}) {
    const std::array<std::string_view, 1> args{detail};
    auto text = detail.empty() ? g.localizer.Text(key, fallback) : g.localizer.Format(key, fallback, args);
    std::scoped_lock lock(g.mutex); g.status = std::move(text);
}
int ANOMALY_CALL Collect(void* user, const AnomalyModEntryV1* entry) noexcept {
    auto& result = *static_cast<RowCollection*>(user);
    try {
        if (!entry || entry->struct_size < offsetof(AnomalyModEntryV1, resource_groups)) {
            result.failed = true; return 0;
        }
        const auto groups = Copy(entry->struct_size >= offsetof(AnomalyModEntryV1, resource_groups) + sizeof(entry->resource_groups)
            ? entry->resource_groups : entry->vehicles);
        const auto classification = SplitGroups(groups);
        result.values.push_back({Copy(entry->key), Copy(entry->name),
            classification.first, classification.second,
            entry->enabled != 0, entry->disabled_files != 0, entry->index_complete != 0});
        return 1;
    } catch (...) { result.failed = true; return 0; }
}
int ANOMALY_CALL CollectAsset(void* user, AnomalyStringViewV1 path) noexcept {
    auto& result = *static_cast<AssetCollection*>(user);
    try { result.values.insert(Copy(path)); return 1; }
    catch (...) { result.failed = true; return 0; }
}

// The panel groups a mod by the containers it replaces, so a section header is the identifier the
// classification matched on: `Player_051`, `Vehicle_037`, a content directory. Those identifiers are
// the classification key and never change; the names the game itself gives those actors are data, and
// the package carries the Chinese ones in `assets/mod-groups.json`. The table declares the locale it
// is written for and is read only while the UI is that locale, so an English UI keeps the generated
// "Character 051" / "Vehicle 037" spelling instead of a Chinese name.
std::wstring PackagePath(const wchar_t* relative_path) {
    std::array<wchar_t, 4096> buffer{};
    HMODULE module{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PackagePath), &module) == FALSE ||
        module == nullptr)
        return {};
    const DWORD size = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    auto path = std::filesystem::path(std::wstring(buffer.data(), size)).parent_path() / relative_path;
    // The host may load an extended-length (\\?\) path, which does not normalize '/'.
    path.make_preferred();
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path.wstring() : std::wstring{};
}

// The locale the host resolved for this process; empty when the service cannot report it, which
// leaves the identifiers unnamed rather than guessing a language.
std::string ActiveLocale(const AnomalyLocalizationServiceV1* localization) {
    if (localization == nullptr || localization->locale == nullptr) return {};
    std::array<char, 32> buffer{};
    std::size_t size = buffer.size();
    if (localization->locale(localization->user, buffer.data(), &size).code != ANOMALY_STATUS_V1_OK ||
        size == 0 || size > buffer.size())
        return {};
    return std::string(buffer.data(), size - 1U);
}

void LoadCategoryNames(const std::string_view locale) {
    if (locale.empty()) return;
    const auto path = PackagePath(L"assets/mod-groups.json");
    if (path.empty()) return;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return;
    const auto document = nlohmann::json::parse(stream, nullptr, false);
    if (document.is_discarded() || !document.is_object()) return;
    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_number_integer() || version->get<int>() != 1) return;
    const auto named = document.find("locale");
    if (named == document.end() || !named->is_string() || named->get_ref<const std::string&>() != locale)
        return;
    const auto table = document.find("groups");
    if (table == document.end() || !table->is_object()) return;
    for (const auto& [key, value] : table->items()) {
        if (key.empty() || !value.is_string()) continue;
        auto name = value.get<std::string>();
        if (name.empty()) continue;
        g.category_names.insert_or_assign(key, std::move(name));
    }
}

void ANOMALY_CALL Worker(void*, AnomalyGenerationHandleV1) noexcept {
    try {
        g.visual.ReportStall();
        std::optional<Action> action;
        bool services_ready{}, binding_pending{}, busy{}, visual_ready{}, compatibility{};
        std::vector<Row> previous;
        {
            std::scoped_lock lock(g.mutex);
            if (g.stopping) return;
            action = std::exchange(g.action, {});
            services_ready = g.services_ready; binding_pending = g.session != nullptr;
            busy = g.busy; visual_ready = g.visual_ready; compatibility = g.compatibility;
            previous = g.rows;
        }
        if (g.native.ReflectionAvailable() && !services_ready && !binding_pending) {
            const Host host(g.api);
            const auto session = host.Query<AnomalyNteSessionServiceV1>(ANOMALY_NTE_SESSION_SERVICE_V1_ID).get();
            if (session && g.native.BindServices(g.api)) {
                std::scoped_lock lock(g.mutex);
                g.session = session;
            }
        }

        bool ok = true, synchronized = false;
        if (action && action->kind == Kind::Toggle) {
            const auto saved = compatibility ? AnomalyStatusV1{ANOMALY_STATUS_V1_CONFLICT}
                : g.mods->set_enabled(g.mods->user, StringView(action->key), action->enabled);
            ok = saved.code == 0;
            if (ok) {
                const auto mounted = g.mods->synchronize(g.mods->user);
                synchronized = mounted.code == 0; ok = synchronized;
                if (ok) LocalizedStatus("status.saved", "Applied");
                else LocalizedStatus("status.mount_pending", "Container switch failed: {0}", ErrorText(mounted));
            } else LocalizedStatus("status.save_failed", "Save failed: {0}", ErrorText(saved));
        }

        RowCollection collection;
        auto listing = g.mods->enumerate(g.mods->user, Collect, &collection);
        if (collection.failed && !listing.code) listing = {ANOMALY_STATUS_V1_FAILED};
        const bool listed = listing.code == 0;
        auto rows = std::move(collection.values);
        if (!listed) { rows.clear(); LocalizedStatus("status.catalog_failed", "Read failed: {0}", ErrorText(listing)); }
        // One section per set of categories, so the order is the categories, then the variants, then
        // the mod's own name: every row of a section stays together as the rows are drawn.
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return std::tie(a.groups, a.skins, a.name) < std::tie(b.groups, b.skins, b.name);
        });

        if (listed && !compatibility) {
            bool index_failed{};
            for (const auto& row : rows) {
                const auto old = std::find_if(previous.begin(), previous.end(), [&](const Row& value) { return value.key == row.key; });
                const bool changed = g.catalog_seen && (old == previous.end() ? row.enabled : old->enabled != row.enabled);
                if (!changed && g.visual_index.contains(row.key)) continue;
                if (changed) if (const auto cached = g.visual_index.find(row.key); cached != g.visual_index.end())
                    g.pending_visual.insert(cached->second.begin(), cached->second.end());
                AssetCollection assets;
                if (!row.complete || g.mods->assets(g.mods->user, StringView(row.key), CollectAsset, &assets).code || assets.failed) {
                    if (changed) index_failed = true;
                    continue;
                }
                if (changed) g.pending_visual.insert(assets.values.begin(), assets.values.end());
                g.visual_index[row.key] = std::move(assets.values);
            }
            if (g.catalog_seen) for (const auto& old : previous)
                if (std::none_of(rows.begin(), rows.end(), [&](const Row& row) { return row.key == old.key; })) {
                    if (old.enabled) if (const auto cached = g.visual_index.find(old.key); cached != g.visual_index.end())
                        g.pending_visual.insert(cached->second.begin(), cached->second.end());
                    g.visual_index.erase(old.key);
                }
            g.catalog_seen = true;
            if (index_failed) {
                g.pending_visual.clear();
                LocalizedStatus("status.visual_unavailable", "Local refresh unavailable; reload world");
            }
        }
        if (!ok) g.pending_visual.clear();

        std::optional<Prepared> prepared;
        const bool reload = action && action->kind == Kind::ReloadWorld;
        if (reload) {
            if (!compatibility && services_ready) {
                const auto mounted = g.mods->synchronize(g.mods->user);
                if (!mounted.code) {
                    prepared = Prepared{true, {}};
                    g.pending_visual.clear();
                } else LocalizedStatus("status.mount_pending", "Container switch failed: {0}", ErrorText(mounted));
            } else LocalizedStatus("status.reload_unavailable", "World reload unavailable");
        }

        bool auto_visual = !reload && !g.pending_visual.empty() && ok && (!busy || (action && action->kind == Kind::Toggle));
        if (auto_visual && !action) {
            std::scoped_lock lock(g.mutex);
            if (g.busy) auto_visual = false; else g.busy = true;
        }
        if (auto_visual) {
            const auto mounted = synchronized ? Ok() : g.mods->synchronize(g.mods->user);
            if (!mounted.code && visual_ready) {
                Prepared value;
                value.packages.assign(g.pending_visual.begin(), g.pending_visual.end());
                prepared = std::move(value);
            } else if (mounted.code) LocalizedStatus("status.mount_pending", "Container switch failed: {0}", ErrorText(mounted));
            else LocalizedStatus("status.visual_unavailable", "Local refresh unavailable; reload world");
            g.pending_visual.clear();
        }
        {
            std::scoped_lock lock(g.mutex);
            if (g.stopping) return;
            if (listed && g.rows != rows) g.rows.swap(rows);
            if (prepared) { g.prepared = std::move(prepared); g.busy = true; }
            else if (action || auto_visual) g.busy = false;
            if (g.scheduler->schedule(g.scheduler->user, 1000, Worker, nullptr, &g.task).code)
                g.status = g.localizer.Text("status.worker_failed", "Refresh stopped; reload plugin.");
        }
    } catch (...) {
        std::scoped_lock lock(g.mutex); g.busy = false;
        g.status = g.localizer.Text("status.worker_failed", "Refresh stopped; reload plugin.");
    }
}
void Request(Action action) {
    std::scoped_lock lock(g.mutex);
    if (g.stopping || g.busy || g.compatibility) return;
    g.action = std::move(action); g.busy = true;
    g.status = g.localizer.Text("status.queued", "Queued");
}
void ReportReload(Reload::Result result) {
    switch (result) {
    case Reload::Result::Waiting: LocalizedStatus("status.reload_wait", "Reloading world..."); break;
    case Reload::Result::Reloaded: LocalizedStatus("status.reloaded", "World reloaded"); break;
    case Reload::Result::DifferentWorld: LocalizedStatus("status.reload_changed", "Entered another scene; reload not confirmed"); break;
    case Reload::Result::Timeout: LocalizedStatus("status.reload_timeout", "World reload not confirmed"); break;
    case Reload::Result::Retained: LocalizedStatus("status.reload_retained", "World reloaded; old game data is still resident"); break;
    default: LocalizedStatus("status.reload_unavailable", "World reload unavailable"); break;
    }
}
void ANOMALY_CALL Update(void*, double) noexcept {
    try {
        std::optional<Prepared> prepared;
        {
            std::unique_lock lock(g.mutex, std::try_to_lock);
            if (!lock || g.stopping) return;
            if (g.session) {
                g.reload.Bind(&g.native, g.session);
                g.visual_ready = g.visual.Bind(&g.native);
                g.session = nullptr; g.services_ready = true;
            }
            if (!g.reload.Busy()) prepared = std::exchange(g.prepared, {});
        }
        const auto now = Reload::Clock::now();
        if (prepared && !prepared->reload_world) {
            const auto result = g.visual.Run(prepared->packages);
            switch (result) {
            case Visual::Result::Updated: LocalizedStatus("status.visual_updated", "Appearance refreshed"); break;
            case Visual::Result::Partial: LocalizedStatus("status.visual_partial", "Appearance refreshed; world reload needed for: {0}", g.visual.Detail()); break;
            case Visual::Result::NotResident: LocalizedStatus("status.saved", "Applied"); break;
            case Visual::Result::Unsupported: LocalizedStatus("status.visual_unsupported", "Use world reload for: {0}", g.visual.Detail()); break;
            case Visual::Result::Unavailable: LocalizedStatus("status.visual_unavailable", "Local refresh unavailable; reload world"); break;
            default: LocalizedStatus("status.visual_failed", "Local refresh failed: {0}", g.visual.Detail()); break;
            }
            std::scoped_lock lock(g.mutex); g.visual_ready = g.visual.Available(); g.busy = g.prepared.has_value() || g.action.has_value();
            return;
        }
        if ((prepared && prepared->reload_world) || g.reload.Busy()) {
            const auto before = g.reload.State();
            const auto result = prepared ? g.reload.Begin(now) : g.reload.Poll(now);
            if (prepared || before != result) ReportReload(result);
            if (!g.reload.Busy()) { std::scoped_lock lock(g.mutex); g.busy = g.prepared.has_value() || g.action.has_value(); }
        }
    } catch (...) {
        g.reload.Cancel(); LocalizedStatus("status.unavailable", "Game service unavailable.");
        std::scoped_lock lock(g.mutex); g.busy = false;
    }
}
// The digits an identifier of the given family ends in: the number the game's own tables use for the
// actor ("051" in "Player_051"). Empty when the token belongs to another family or carries no number,
// which keeps a content directory and a folder a mod author chose in their own spelling.
std::string_view CategoryNumber(const std::string_view token, const std::string_view prefix) {
    if (!token.starts_with(prefix)) return {};
    const auto number = token.substr(prefix.size());
    if (number.empty() || number.find_first_not_of("0123456789") != std::string_view::npos) return {};
    return number;
}

// The name a section header reads. The identifier stays the classification key; a name only renames
// what the header shows, and one the table does not name still reads as a category instead of a bare
// id. Only the zh-CN table ships, so the Chinese names appear while the UI is Chinese; the English
// catalogs carry the generated spelling and an unmapped id falls back to it in either language.
std::string CategoryLabel(const std::string_view token) {
    if (token.empty()) return g.localizer.Text("group.unknown", "Unclassified resources");
    if (const auto named = g.category_names.find(token); named != g.category_names.end())
        return named->second;
    if (const auto number = CategoryNumber(token, "Player_"); !number.empty()) {
        const std::array<std::string_view, 1> arguments{number};
        return g.localizer.Format("category.player", "Character {0}", arguments);
    }
    if (const auto number = CategoryNumber(token, "Vehicle_"); !number.empty()) {
        const std::array<std::string_view, 1> arguments{number};
        return g.localizer.Format("category.vehicle", "Vehicle {0}", arguments);
    }
    if (const auto number = CategoryNumber(token, "Pendant_"); !number.empty()) {
        const std::array<std::string_view, 1> arguments{number};
        return g.localizer.Format("category.pendant", "Pendant {0}", arguments);
    }
    return std::string(token);
}

std::vector<std::string> CategoryLabels(const Row& row) {
    // A mod whose containers name no category at all is the one case with no token to name.
    if (row.groups.empty()) return {CategoryLabel({})};
    std::vector<std::string> labels;
    labels.reserve(row.groups.size());
    for (const auto& token : row.groups) labels.push_back(CategoryLabel(token));
    return labels;
}

std::string JoinNames(const std::vector<std::string>& values) {
    std::string text;
    for (const auto& value : values) {
        if (!text.empty()) text += " · ";
        text += value;
    }
    return text;
}

// Everything a row can be found by: the mod's own name and key, the variants it carries, and every
// category it replaces under both the identifier and the name it is shown as. Searching both is what
// makes a Chinese input and an English one find the same row.
std::string SearchText(const Row& row, const std::vector<std::string>& labels) {
    std::string text = row.name + " " + row.key + " " + row.skins;
    for (std::size_t index = 0; index < row.groups.size(); ++index)
        text += " " + row.groups[index] + " " + labels[index];
    return text;
}

// A Core older than the host filter leaves the search working rather than empty: the same
// case-insensitive byte search the host performs, which matches a UTF-8 Chinese input verbatim.
bool ContainsFolded(const std::string_view text, const std::string_view filter) {
    const auto folded = [](const char left, const char right) {
        return std::tolower(static_cast<unsigned char>(left)) ==
            std::tolower(static_cast<unsigned char>(right));
    };
    return std::search(text.begin(), text.end(), filter.begin(), filter.end(), folded) != text.end();
}

bool UiFieldAvailable(const AnomalyUiServiceV1* ui, const std::size_t offset, const std::size_t size) {
    return ui != nullptr && ui->struct_size >= offset + size;
}

void ANOMALY_CALL Draw(void*, const AnomalyUiServiceV1* ui) noexcept {
    if (!ui || !g.windows || !g.window.id) return;
    try {
        AnomalyWindowStateV1 state{sizeof(state)};
        if (g.windows->state(g.windows->user, g.window, &state).code || !state.open) return;
        int visible{}; if (g.windows->begin(g.windows->user, g.window, 0, &visible).code) return;
        struct End { ~End() { g.windows->end(g.windows->user, g.window); } } end;
        if (!visible) return;
        std::vector<Row> rows;
        std::string status;
        bool busy{}, services_ready{};
        {
            std::unique_lock lock(g.mutex, std::try_to_lock);
            if (!lock) return;
            rows = g.rows; status = g.status; busy = g.busy; services_ready = g.services_ready;
        }
        const auto TextLocalized = [&](std::string_view key, std::string_view fallback) {
            ui->text(ui->user, StringView(g.localizer.Text(key, fallback)));
        };
        if (g.compatibility) TextLocalized("mode.compatibility", "Compatibility mode");
        else TextLocalized("mode.hot", "Hot mode");
        if (!g.compatibility && !g.profile_valid)
            TextLocalized("status.profile_unavailable", "Profile mismatch; refresh unavailable");
        else if (!g.compatibility && !services_ready)
            TextLocalized("status.services_pending", "Waiting for game services.");
        if (!status.empty()) ui->text(ui->user, StringView(status));
        if (ui->button_enabled(ui->user, StringView(g.localizer.Text("action.reload_world", "Reload world")), 0, 0, !busy && !g.compatibility && services_ready))
            Request({Kind::ReloadWorld, {}});

        // The search box filters which rows the page lists; the whole set is drawn otherwise, so a
        // list of a couple of dozen mods needs no pager.
        if (UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, input_text),
                sizeof(decltype(AnomalyUiServiceV1::input_text))) && ui->input_text) {
            std::array<char, 128> buffer{};
            const auto length = (std::min)(g.filter.size(), buffer.size() - 1U);
            std::copy_n(g.filter.data(), length, buffer.data());
            const auto hint = g.localizer.Text("search.hint", "Search character, vehicle or mod");
            if (ui->input_text(ui->user, StringView(hint), buffer.data(), buffer.size(), 0U))
                g.filter.assign(buffer.data());
        }
        const bool host_filter = UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, filter_match),
            sizeof(decltype(AnomalyUiServiceV1::filter_match))) && ui->filter_match;
        // Only the list scrolls. The mode, the status, the reload button and the search box are drawn
        // above it and keep their place; the rows take the rest of the window inside their own scroll
        // region, which a width and height of zero fill. A window too short for the header leaves
        // nothing to fill, and the body scrolls the page as the only way to reach the controls.
        const bool has_list = UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, begin_child),
                                  sizeof(decltype(AnomalyUiServiceV1::begin_child))) &&
            ui->begin_child && UiFieldAvailable(ui, offsetof(AnomalyUiServiceV1, end_child),
                                  sizeof(decltype(AnomalyUiServiceV1::end_child))) &&
            ui->end_child;
        struct ListScope {
            const AnomalyUiServiceV1* ui{};
            bool visible{true};
            ~ListScope() { if (ui) ui->end_child(ui->user); }
        } list;
        if (has_list) {
            list.ui = ui;
            list.visible = ui->begin_child(ui->user, StringView("##mod-list"), 0.0f, 0.0f,
                               ANOMALY_UI_CHILD_V1_NONE) != 0;
        }
        std::optional<std::vector<std::string>> group;
        std::size_t shown{};
        if (list.visible) {
            for (const auto& row : rows) {
                const bool filtered = !g.filter.empty();
                std::vector<std::string> labels;
                if (filtered) {
                    labels = CategoryLabels(row);
                    const auto search = SearchText(row, labels);
                    const bool matched = host_filter
                        ? ui->filter_match(ui->user, StringView(g.filter), StringView(search)) != 0
                        : ContainsFolded(search, g.filter);
                    if (!matched) continue;
                }
                ++shown;
                if (!group || row.groups != *group) {
                    ui->separator(ui->user); group = row.groups;
                    if (labels.empty()) labels = CategoryLabels(row);
                    ui->text(ui->user, StringView(JoinNames(labels)));
                }
                int enabled = row.enabled;
                if (ui->checkbox(ui->user, StringView("##enabled-" + row.key), &enabled) && !busy && !g.compatibility && (row.enabled || !row.disabled))
                    Request({Kind::Toggle, row.key, enabled != 0});
                ui->same_line(ui->user, 0, -1); ui->text(ui->user, StringView(row.name));
                // The variants this mod replaces, on its own row: the section is the character.
                if (!row.skins.empty()) { ui->same_line(ui->user, 0, -1); ui->text(ui->user, StringView(row.skins)); }
                if (!row.complete) TextLocalized("row.incomplete", "Index unavailable.");
                if (row.disabled) TextLocalized("row.disabled_files", "Files disabled; restore in Launcher.");
            }
            if (shown == 0 && !rows.empty()) TextLocalized("search.empty", "No matching mods.");
        }
    } catch (...) {}
}
AnomalyStatusV1 ANOMALY_CALL Load(const AnomalyHostApiV1* api, void** context) noexcept {
    try {
        const Host host(api);
        g.api = api; g.localizer = Localizer(api);
        g.mods = host.Query<AnomalyModsServiceV1>(ANOMALY_MODS_SERVICE_V1_ID).get();
        g.scheduler = host.Query<AnomalySchedulerServiceV1>(ANOMALY_SCHEDULER_SERVICE_V1_ID).get();
        g.windows = host.Query<AnomalyWindowServiceV1>(ANOMALY_WINDOW_SERVICE_V1_ID).get();
        if (!context || !g.mods || g.mods->struct_size < sizeof(*g.mods) || !g.mods->synchronize ||
            !g.mods->assets || !g.mods->enumerate || !g.mods->set_enabled || !g.mods->compatibility_mode ||
            !g.scheduler || g.scheduler->struct_size < sizeof(*g.scheduler) ||
            !g.scheduler->schedule || !g.scheduler->cancel || !g.windows ||
            g.windows->struct_size < sizeof(*g.windows) || !g.windows->register_window ||
            !g.windows->release_window || !g.windows->state || !g.windows->begin || !g.windows->end)
            return {ANOMALY_STATUS_V1_UNAVAILABLE};
        g.compatibility = g.mods->compatibility_mode(g.mods->user) != 0;
        LoadCategoryNames(ActiveLocale(
            host.Query<AnomalyLocalizationServiceV1>(ANOMALY_LOCALIZATION_SERVICE_V1_ID).get()));
        try { g.profile_valid = !g.compatibility && g.native.Load(api); } catch (...) { g.profile_valid = false; }
        AnomalyWindowSpecV1 spec{sizeof(spec)};
        const auto title = g.localizer.Text("window.title", "Mod Manager");
        spec.id = StringView("mod-manager"); spec.title = StringView(title);
        spec.initial_width = 900; spec.initial_height = 650;
        spec.minimum_width = 650; spec.minimum_height = 400;
        const auto status = g.windows->register_window(g.windows->user, &spec, &g.window);
        if (status.code) return status;
        *context = &g; return Ok();
    } catch (...) { return {ANOMALY_STATUS_V1_FAILED}; }
}
AnomalyStatusV1 ANOMALY_CALL Start(void*) noexcept {
    try { if (!g.compatibility && g.native.ReflectionAvailable()) g.reload.Install(g.api, &g.native); } catch (...) {}
    std::scoped_lock lock(g.mutex);
    g.stopping = false;
    return g.scheduler->schedule(g.scheduler->user, 0, Worker, nullptr, &g.task);
}
AnomalyStatusV1 ANOMALY_CALL Stop(void*, uint32_t) noexcept {
    AnomalyGenerationHandleV1 task{};
    { std::scoped_lock lock(g.mutex); g.stopping = true; task = g.task; }
    if (task.id) g.scheduler->cancel(g.scheduler->user, task);
    return g.reload.Stop();
}
void ANOMALY_CALL Unload(void*) noexcept {
    if (g.window.id) g.windows->release_window(g.windows->user, g.window);
    g.window = {}; g.reload.Cancel();
}
}
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* descriptor) {
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor)) return {ANOMALY_STATUS_V1_INVALID_ARGUMENT};
    *descriptor = {sizeof(*descriptor), ANOMALY_PLUGIN_API_V1_MAJOR, ANOMALY_PLUGIN_API_V1_MINOR,
        StringView("anomaly.mod-manager"), StringView("Mod Manager"), StringView("Anomaly"), StringView("0.1.0"),
        Load, Start, Stop, Unload, Update, Draw};
    return Ok();
}
