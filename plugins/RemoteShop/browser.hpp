#pragma once
#include "native.hpp"
#include "text.hpp"
#include <set>

namespace shop {
inline std::string Fold(std::string value) {
    value = DisplayText(std::move(value));
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
inline std::string Category(int kind,const anomaly::sdk::Localizer* localizer) {
    switch (kind) {
        case 0: return localizer ? localizer->Text("category.daily","General store") : "General store";
        case 2: return localizer ? localizer->Text("category.vending","Vending machine") : "Vending machine";
        case 3: return localizer ? localizer->Text("category.fons","Fons shop") : "Fons shop";
        case 4: return localizer ? localizer->Text("category.gift","Gift shop") : "Gift shop";
        case 5: return localizer ? localizer->Text("category.event","Event shop") : "Event shop";
        default: return localizer ? localizer->Text("category.all","All types") : "All types";
    }
}
inline std::string BranchLabel(const std::string& id,const anomaly::sdk::Localizer& localizer) {
    if (!id.starts_with("Shop_")) return id;
    const auto start = id.find('_', 5);
    if (start == std::string::npos || start + 1 == id.size()) return id;
    auto tail = id.substr(start + 1);
    std::ranges::replace(tail, '_', '-');
    return localizer.Format("merchant.branch","Branch {0}",std::array<std::string_view,1>{tail});
}
struct Branch {
    std::string id, name;
    uint8_t kind{};
    size_t count{};
    IconSource icon;
};
struct MerchantGroup {
    std::string name;
    std::vector<Branch> branches;
    size_t count{};
    IconSource icon;
};
struct ProductGroup {
    ProductKey key;
    std::vector<size_t> offers;
};
inline std::vector<ProductGroup> MergeProducts(const std::vector<Goods>& goods,const std::vector<size_t>& indices) {
    std::map<ProductKey,size_t> positions;
    std::vector<ProductGroup> result;
    for (const auto index : indices) {
        const auto& item = goods[index];
        if (item.stock && *item.stock <= 0) continue;
        auto key = item.Product();
        auto [entry,fresh] = positions.try_emplace(key,result.size());
        if (fresh) result.push_back({std::move(key),{}});
        result[entry->second].offers.push_back(index);
    }
    return result;
}
inline size_t DefaultOffer(const std::vector<Goods>& goods,const ProductGroup& group,
        const std::pair<std::string,std::string>& preferred = {}) {
    for (auto index : group.offers) if (goods[index].shop_id == preferred.first && goods[index].id == preferred.second) return index;
    for (auto index : group.offers) if (goods[index].stock && *goods[index].stock > 0) return index;
    return group.offers.front();
}
class BrowserIndex {
public:
    const anomaly::sdk::Localizer* localizer{};
    std::map<std::string, MerchantGroup> groups;
    std::map<std::string, Branch> branches;
    std::vector<std::string> merchant_order;
    std::vector<std::string> search;
    std::map<ProductKey,ProductGroup> products;
    std::map<std::pair<std::string,std::string>,ProductKey> offer_products;
    void Build(const std::vector<Goods>& goods) {
        groups.clear(); branches.clear(); merchant_order.clear(); search.clear(); search.reserve(goods.size());
        products.clear(); offer_products.clear();
        for (const auto& item : goods) {
            const auto key = item.Product();
            auto [product,created] = products.try_emplace(key,ProductGroup{key});
            if (!item.stock || *item.stock > 0) product->second.offers.push_back(search.size());
            offer_products.emplace(std::pair{item.shop_id,item.id},key);
            auto [branch, fresh] = branches.try_emplace(item.shop_id, Branch{item.shop_id,item.shop_label,item.shop_kind});
            ++branch->second.count;
            if (branch->second.icon.icon_path.empty()) branch->second.icon = item.shop_icon;
            search.push_back(Fold(item.label + " " + item.id + " " + item.shop_label + " " + item.shop_id + " " +
                std::string(Category(item.shop_kind,localizer))));
        }
        std::erase_if(products,[](const auto& product){return product.second.offers.empty();});
        for (const auto& [id, branch] : branches) {
            auto [group, fresh] = groups.try_emplace(branch.name,MerchantGroup{branch.name});
            group->second.branches.push_back(branch); group->second.count += branch.count;
            if (group->second.icon.icon_path.empty()) group->second.icon = branch.icon;
        }
        for (const auto& [name,group] : groups) merchant_order.push_back(name);
        std::stable_partition(merchant_order.begin(),merchant_order.end(),[&](const auto& name) {
            return !groups.at(name).icon.icon_path.empty();
        });
    }
    std::vector<size_t> FindMerchant(const std::vector<Goods>& goods,const std::string& merchant,std::string query,const std::string& branch = {}) const {
        auto result = Find(goods,std::move(query));
        std::erase_if(result,[&](size_t i){return goods[i].shop_label != merchant || (!branch.empty() && goods[i].shop_id != branch);});
        return result;
    }
    std::vector<size_t> Find(const std::vector<Goods>& goods, std::string query, int kind = -1) const {
        query = Fold(std::move(query));
        std::vector<std::string_view> words;
        for (size_t begin = 0, end; (begin = query.find_first_not_of(" \t\r\n", begin)) != std::string::npos; begin = end) {
            end = query.find_first_of(" \t\r\n", begin);
            words.push_back(std::string_view(query).substr(begin, end == std::string::npos ? end : end - begin));
        }
        std::vector<size_t> result;
        for (size_t i = 0; i < goods.size() && i < search.size(); ++i) {
            if (kind >= 0 && goods[i].shop_kind != kind) continue;
            if (std::ranges::all_of(words, [&](auto word) { return search[i].find(word) != std::string::npos; })) result.push_back(i);
        }
        std::ranges::stable_sort(result,[&](size_t a,size_t b) {
            return std::tie(goods[a].label,goods[a].price,goods[a].shop_label,goods[a].shop_id,goods[a].id) <
                std::tie(goods[b].label,goods[b].price,goods[b].shop_label,goods[b].shop_id,goods[b].id);
        });
        return result;
    }
};
} // namespace shop
