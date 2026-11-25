#pragma once
#include <anomaly/sdk/localization.hpp>
#include <algorithm>
#include <cctype>
#include <vector>

namespace creative_mode {
enum class AssetCategory : std::uint32_t { All, Shapes, Furniture, Roads, Vehicles, Buildings, Nature, Props };
struct AssetName { std::string label; AssetCategory category{AssetCategory::Props}; };
class CatalogNames {
public:
    explicit CatalogNames(const anomaly::sdk::Localizer& localizer) : localizer_(localizer) {
        using C = AssetCategory;
        terms_.push_back({"cube",localizer.Text("primitive.cube","Cube"),C::Shapes});
        terms_.push_back({"sphere",localizer.Text("primitive.sphere","Sphere"),C::Shapes});
        terms_.push_back({"cylinder",localizer.Text("primitive.cylinder","Cylinder"),C::Shapes});
        terms_.push_back({"cone",localizer.Text("primitive.cone","Cone"),C::Shapes});
        terms_.push_back({"plane",localizer.Text("asset.plane","Plane"),C::Shapes});
        terms_.push_back({"chair",localizer.Text("asset.chair","Chair"),C::Furniture});
        terms_.push_back({"sofa",localizer.Text("asset.sofa","Sofa"),C::Furniture});
        terms_.push_back({"couch",localizer.Text("asset.sofa","Sofa"),C::Furniture});
        terms_.push_back({"table",localizer.Text("asset.table","Table"),C::Furniture});
        terms_.push_back({"desk",localizer.Text("asset.desk","Desk"),C::Furniture});
        terms_.push_back({"bench",localizer.Text("asset.bench","Bench"),C::Furniture});
        terms_.push_back({"bed",localizer.Text("asset.bed","Bed"),C::Furniture});
        terms_.push_back({"cabinet",localizer.Text("asset.cabinet","Cabinet"),C::Furniture});
        terms_.push_back({"shelf",localizer.Text("asset.shelf","Shelf"),C::Furniture});
        terms_.push_back({"shelves",localizer.Text("asset.shelf","Shelf"),C::Furniture});
        terms_.push_back({"bookshelf",localizer.Text("asset.bookshelf","Bookshelf"),C::Furniture});
        terms_.push_back({"stool",localizer.Text("asset.stool","Stool"),C::Furniture});
        terms_.push_back({"lamp",localizer.Text("asset.lamp","Lamp"),C::Furniture});
        terms_.push_back({"light",localizer.Text("asset.light","Light"),C::Props});
        terms_.push_back({"curtain",localizer.Text("asset.curtain","Curtain"),C::Furniture});
        terms_.push_back({"carpet",localizer.Text("asset.carpet","Carpet"),C::Furniture});
        terms_.push_back({"pillow",localizer.Text("asset.pillow","Pillow"),C::Furniture});
        terms_.push_back({"television",localizer.Text("asset.tv","Television"),C::Furniture});
        terms_.push_back({"tv",localizer.Text("asset.tv","Television"),C::Furniture});
        terms_.push_back({"furniture",localizer.Text("asset.furniture","Furniture"),C::Furniture});
        terms_.push_back({"road",localizer.Text("asset.road","Road"),C::Roads});
        terms_.push_back({"street",localizer.Text("asset.street","Street"),C::Roads});
        terms_.push_back({"highway",localizer.Text("asset.highway","Highway"),C::Roads});
        terms_.push_back({"bridge",localizer.Text("asset.bridge","Bridge"),C::Roads});
        terms_.push_back({"pier",localizer.Text("asset.pier","Bridge pier"),C::Roads});
        terms_.push_back({"bollard",localizer.Text("asset.bollard","Bollard"),C::Roads});
        terms_.push_back({"hydrant",localizer.Text("asset.hydrant","Fire hydrant"),C::Props});
        terms_.push_back({"billboard",localizer.Text("asset.billboard","Billboard"),C::Props});
        terms_.push_back({"ramp",localizer.Text("asset.ramp","Ramp"),C::Roads});
        terms_.push_back({"sidewalk",localizer.Text("asset.sidewalk","Sidewalk"),C::Roads});
        terms_.push_back({"pavement",localizer.Text("asset.sidewalk","Sidewalk"),C::Roads});
        terms_.push_back({"rail",localizer.Text("asset.rail","Rail"),C::Roads});
        terms_.push_back({"barrier",localizer.Text("asset.barrier","Barrier"),C::Roads});
        terms_.push_back({"guardrail",localizer.Text("asset.guardrail","Guardrail"),C::Roads});
        terms_.push_back({"traffic",localizer.Text("asset.traffic","Traffic"),C::Roads});
        terms_.push_back({"sign",localizer.Text("asset.sign","Sign"),C::Props});
        terms_.push_back({"car",localizer.Text("asset.car","Car"),C::Vehicles});
        terms_.push_back({"vehicle",localizer.Text("asset.vehicle","Vehicle"),C::Vehicles});
        terms_.push_back({"bus",localizer.Text("asset.bus","Bus"),C::Vehicles});
        terms_.push_back({"truck",localizer.Text("asset.truck","Truck"),C::Vehicles});
        terms_.push_back({"taxi",localizer.Text("asset.taxi","Taxi"),C::Vehicles});
        terms_.push_back({"bike",localizer.Text("asset.bike","Bicycle"),C::Vehicles});
        terms_.push_back({"bicycle",localizer.Text("asset.bike","Bicycle"),C::Vehicles});
        terms_.push_back({"motorcycle",localizer.Text("asset.motorcycle","Motorcycle"),C::Vehicles});
        terms_.push_back({"scooter",localizer.Text("asset.scooter","Scooter"),C::Vehicles});
        terms_.push_back({"boat",localizer.Text("asset.boat","Boat"),C::Vehicles});
        terms_.push_back({"wheel",localizer.Text("asset.wheel","Wheel"),C::Vehicles});
        terms_.push_back({"tire",localizer.Text("asset.tire","Tire"),C::Vehicles});
        terms_.push_back({"building",localizer.Text("asset.building","Building"),C::Buildings});
        terms_.push_back({"house",localizer.Text("asset.house","House"),C::Buildings});
        terms_.push_back({"wall",localizer.Text("asset.wall","Wall"),C::Buildings});
        terms_.push_back({"floor",localizer.Text("asset.floor","Floor"),C::Buildings});
        terms_.push_back({"roof",localizer.Text("asset.roof","Roof"),C::Buildings});
        terms_.push_back({"door",localizer.Text("asset.door","Door"),C::Buildings});
        terms_.push_back({"window",localizer.Text("asset.window","Window"),C::Buildings});
        terms_.push_back({"stairs",localizer.Text("asset.stairs","Stairs"),C::Buildings});
        terms_.push_back({"stair",localizer.Text("asset.stairs","Stairs"),C::Buildings});
        terms_.push_back({"step",localizer.Text("asset.step","Step"),C::Buildings});
        terms_.push_back({"fence",localizer.Text("asset.fence","Fence"),C::Buildings});
        terms_.push_back({"pillar",localizer.Text("asset.pillar","Pillar"),C::Buildings});
        terms_.push_back({"column",localizer.Text("asset.pillar","Pillar"),C::Buildings});
        terms_.push_back({"platform",localizer.Text("asset.platform","Platform"),C::Buildings});
        terms_.push_back({"brick",localizer.Text("asset.brick","Brick"),C::Buildings});
        terms_.push_back({"block",localizer.Text("asset.block","Block"),C::Buildings});
        terms_.push_back({"tree",localizer.Text("asset.tree","Tree"),C::Nature});
        terms_.push_back({"bush",localizer.Text("asset.bush","Bush"),C::Nature});
        terms_.push_back({"grass",localizer.Text("asset.grass","Grass"),C::Nature});
        terms_.push_back({"flower",localizer.Text("asset.flower","Flower"),C::Nature});
        terms_.push_back({"plant",localizer.Text("asset.plant","Plant"),C::Nature});
        terms_.push_back({"rock",localizer.Text("asset.rock","Rock"),C::Nature});
        terms_.push_back({"stone",localizer.Text("asset.stone","Stone"),C::Nature});
        terms_.push_back({"foliage",localizer.Text("asset.foliage","Foliage"),C::Nature});
        terms_.push_back({"box",localizer.Text("asset.box","Box"),C::Props});
        terms_.push_back({"crate",localizer.Text("asset.crate","Crate"),C::Props});
        terms_.push_back({"barrel",localizer.Text("asset.barrel","Barrel"),C::Props});
        terms_.push_back({"bin",localizer.Text("asset.bin","Bin"),C::Props});
        terms_.push_back({"trash",localizer.Text("asset.trash","Trash"),C::Props});
        terms_.push_back({"container",localizer.Text("asset.container","Container"),C::Props});
        terms_.push_back({"pipe",localizer.Text("asset.pipe","Pipe"),C::Props});
        terms_.push_back({"screen",localizer.Text("asset.screen","Screen"),C::Props});
        terms_.push_back({"aircon",localizer.Text("asset.aircon","Air conditioner"),C::Props});
        terms_.push_back({"pot",localizer.Text("asset.pot","Pot"),C::Props});
        terms_.push_back({"bottle",localizer.Text("asset.bottle","Bottle"),C::Props});
        terms_.push_back({"book",localizer.Text("asset.book","Book"),C::Props});
        terms_.push_back({"vase",localizer.Text("asset.vase","Vase"),C::Props});
        terms_.push_back({"small",localizer.Text("asset.small","Small"),C::All});
        terms_.push_back({"large",localizer.Text("asset.large","Large"),C::All});
        terms_.push_back({"big",localizer.Text("asset.large","Large"),C::All});
        terms_.push_back({"wood",localizer.Text("asset.wood","Wood"),C::All});
        terms_.push_back({"wooden",localizer.Text("asset.wood","Wood"),C::All});
        terms_.push_back({"metal",localizer.Text("asset.metal","Metal"),C::All});
        terms_.push_back({"glass",localizer.Text("asset.glass","Glass"),C::All});
        terms_.push_back({"white",localizer.Text("asset.white","White"),C::All});
        terms_.push_back({"black",localizer.Text("asset.black","Black"),C::All});
        terms_.push_back({"red",localizer.Text("asset.red","Red"),C::All});
        terms_.push_back({"blue",localizer.Text("asset.blue","Blue"),C::All});
        terms_.push_back({"green",localizer.Text("asset.green","Green"),C::All});
        terms_.push_back({"yellow",localizer.Text("asset.yellow","Yellow"),C::All});
        generic_ = localizer.Text("asset.generic", "Scene prop");
    }
    AssetName Name(std::string_view path) const {
        const auto dot = path.rfind('.');
        const auto raw = path.substr(dot == std::string_view::npos ? path.rfind('/')+1 : dot+1);
        const auto words = Words(raw);
        AssetName result;
        std::vector<std::string> names;
        bool classified{};
        for (const auto& word : words) for (const auto& term : terms_) {
            if (word != term.token && !(word.size() > 2 && word.back() == 's' && word.substr(0,word.size()-1) == term.token)) continue;
            if (std::find(names.begin(),names.end(),term.label) == names.end()) names.push_back(term.label);
            if (!classified && term.category != AssetCategory::All) { result.category = term.category; classified = true; }
            break;
        }
        if (!classified) for (const auto& word : Words(path.substr(0,path.rfind('/'))))
            for (const auto& term : terms_) if (word == term.token && term.category != AssetCategory::All) {
                result.category = term.category; classified = true;
                if (names.empty()) names.push_back(term.label);
            }
        if (names.empty()) names.push_back(generic_);
        if (std::any_of(raw.begin(),raw.end(),[](unsigned char c) { return c >= 128; })) {
            result.label = std::string(raw); return result;
        }
        for (std::size_t i = 0; i < names.size() && i < 4; ++i) { if (!result.label.empty()) result.label += " · "; result.label += names[i]; }
        std::string variant;
        for (const auto& word : words) if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0]))) {
            if (!variant.empty()) variant += '-'; variant += word;
        }
        if (variant.empty() && names.front() == generic_) {
            std::uint32_t hash = 2166136261u;
            for (const auto c : path) hash = (hash ^ static_cast<unsigned char>(c))*16777619u;
            variant = std::to_string(hash % 100000);
        }
        if (!variant.empty()) {
            const std::array args{std::string_view(result.label),std::string_view(variant)};
            result.label = localizer_.Format("asset.variant", "{0} {1}", args);
        }
        return result;
    }
private:
    static std::vector<std::string> Words(std::string_view text) {
        std::vector<std::string> words; std::string word;
        const auto finish = [&] { if (!word.empty()) { words.push_back(std::move(word)); word.clear(); } };
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if (!std::isalnum(c)) { finish(); continue; }
            if (!word.empty() && ((std::isupper(c) && std::islower(static_cast<unsigned char>(text[i-1]))) ||
                (std::isdigit(c) != std::isdigit(static_cast<unsigned char>(text[i-1]))))) finish();
            word.push_back(static_cast<char>(std::tolower(c)));
        }
        finish(); return words;
    }
    struct Term { std::string token,label; AssetCategory category; };
    const anomaly::sdk::Localizer& localizer_;
    std::vector<Term> terms_;
    std::string generic_;
};
}
