#pragma once

#include "anomaly/sdk/cpp.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace character_studio {
using Address = std::uintptr_t;
using Handle = AnomalyGenerationHandleV1;
using Json = nlohmann::json;

inline bool Same(Handle a, Handle b) {
    return a.id == b.id && a.generation == b.generation;
}
struct Name {
    std::uint32_t index{}, number{};
    bool operator==(const Name&) const = default;
};
inline std::uint64_t Key(Name n) {
    return (std::uint64_t(n.number) << 32) | n.index;
}
struct Vector {
    double x{}, y{}, z{};
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};
struct Transform {
    double rotation[4]{0, 0, 0, 1};
    Vector translation{};
    double padding{};
    Vector scale{1, 1, 1};
    double padding2{};
};
static_assert(sizeof(Name) == 8 && sizeof(Vector) == 24 && sizeof(Transform) == 96);

// Public SDK services implement this boundary. No production UE type crosses it.
// In particular Invoke is Game-domain only; Read has no write counterpart.
struct Process {
    virtual ~Process() = default;
    virtual bool Read(Address address, void* data, std::size_t size) = 0;
    virtual std::string ResolveName(std::uint32_t index) = 0;
    virtual std::string ResolveText(Address address) = 0;
    virtual Handle Find(std::string_view path) = 0;
    virtual std::uint64_t Generation() = 0;
    virtual bool Invoke(Address object, Address function, void* data, std::size_t size) = 0;
};

struct Property {
    Address field{}, structure{};
    std::int32_t offset{}, size{}, dimension{};
    std::string type;
};

class Engine {
public:
    Engine(Process& process, Json resource, Address registry)
        : process_(process), resource_(std::move(resource)), registry_(registry) {
        if (resource_.at("schema") != "anomaly.plugin.character-studio.layout" ||
            resource_.at("version") != 1 || resource_.at("game") != "nte")
            throw std::runtime_error("不支持的模型布局资料");
    }
    const Json& resource() const { return resource_; }
    std::size_t Offset(std::string_view key) const {
        return resource_.at("layout").at(std::string(key)).get<std::size_t>();
    }
    template<class T> T Read(Address address) {
        T value{};
        if (!address || !process_.Read(address, &value, sizeof(value)))
            throw std::runtime_error("对象内存不可读");
        return value;
    }
    void Bytes(Address address, void* data, std::size_t size) {
        if (size && (!address || !process_.Read(address, data, size)))
            throw std::runtime_error("资源数据不可读");
    }
    template<class T> T Layout(Address object, std::string_view field) {
        return Read<T>(object + Offset(field));
    }
    std::string Text(Address address) { return process_.ResolveText(address); }
    std::string Spelling(Name name) {
        auto value = process_.ResolveName(name.index);
        if (name.number) value += "_" + std::to_string(name.number - 1);
        return value;
    }
    std::string ObjectName(Address object) {
        return Spelling(Layout<Name>(object, "object.nameOffset"));
    }
    Address Class(Address object) { return Layout<Address>(object, "object.class"); }
    bool Derived(Address type, std::string_view expected) {
        for (unsigned depth = 0; type && depth < 64; ++depth) {
            if (ObjectName(type) == expected) return true;
            type = Layout<Address>(type, "ustruct.superStruct");
        }
        return false;
    }
    bool IsA(Address object, std::string_view type) {
        return object && Derived(Class(object), type);
    }
    Address Resolve(Handle handle) {
        if (!handle.id || !static_cast<std::uint32_t>(handle.id) ||
            !handle.generation || handle.generation != process_.Generation())
            return 0;
        const auto index = ANOMALY_UE5_OBJECT_HANDLE_INDEX(handle);
        std::uint32_t serial{};
        const auto object = ObjectAt(index, serial);
        if (!object || serial != ANOMALY_UE5_OBJECT_HANDLE_SERIAL(handle) ||
            Layout<std::int32_t>(object, "object.internalIndex") !=
                static_cast<std::int32_t>(index)) return 0;
        return object;
    }
    Handle Pin(Address object) {
        if (!object) return {};
        const auto generation = process_.Generation();
        if (!generation) return {};
        const auto index = Layout<std::int32_t>(object, "object.internalIndex");
        if (index < 0) return {};
        std::uint32_t serial{};
        if (ObjectAt(static_cast<std::uint32_t>(index), serial) != object) return {};
        return {(std::uint64_t(serial) << 32) | (std::uint64_t(index) + 1), generation};
    }
    Address Find(std::string_view path) {
        auto object = Resolve(process_.Find(path));
        // Host exact paths use ':' between an owner and its function in some versions.
        if (!object) {
            std::string alternate(path);
            if (const auto at = alternate.rfind('.'); at != std::string::npos) {
                alternate[at] = ':';
                object = Resolve(process_.Find(alternate));
            }
        }
        if (!object) throw std::runtime_error("未找到反射对象：" + std::string(path));
        return object;
    }
    Property PropertyAt(Address type, std::string_view name) {
        auto field = Layout<Address>(type, "ustruct.propertyLink");
        for (unsigned seen = 0; field && seen < 4096; ++seen) {
            if (Spelling(Layout<Name>(field, "ffield.name")) == name) {
                Property result;
                result.field = field;
                const auto field_class = Layout<Address>(field, "ffield.class");
                result.type = Spelling(Layout<Name>(field_class, "ffieldClass.name"));
                result.offset = Layout<std::int32_t>(field, "fproperty.offsetInternal");
                result.size = Layout<std::int32_t>(field, "fproperty.elementSize");
                result.dimension = Layout<std::int32_t>(field, "fproperty.arrayDim");
                if (result.offset < 0 || result.offset > 1024 * 1024 ||
                    result.size <= 0 || result.size > 1024 * 1024 || result.dimension != 1)
                    throw std::runtime_error("属性布局不合法：" + std::string(name));
                if (result.type == "StructProperty")
                    result.structure = Layout<Address>(field, "fstructProperty.struct");
                return result;
            }
            field = Layout<Address>(field, "fproperty.propertyLinkNext");
        }
        throw std::runtime_error("未找到属性：" + std::string(name));
    }
    Property Expect(Address type, std::string_view name, std::string_view kind,
                    std::size_t size, std::string_view structure = {}) {
        auto field = PropertyAt(type, name);
        const auto kinds = "|" + std::string(kind) + "|";
        if (kinds.find("|" + field.type + "|") == std::string::npos ||
            static_cast<std::size_t>(field.size) != size ||
            (!structure.empty() && (!field.structure ||
                 ObjectName(field.structure) != structure)))
            throw std::runtime_error("属性合同不匹配：" + std::string(name));
        return field;
    }
    template<class T> T Value(Address object, Address type, std::string_view name,
                              std::string_view kind) {
        return Read<T>(object + Expect(type, name, kind, sizeof(T)).offset);
    }
    Address ObjectField(Address object, std::string_view field) {
        return Value<Address>(object, Class(object), field, "ObjectProperty");
    }
    struct Instance { Address type{}, data{}; };
    Instance Instanced(Address object, Address type, std::string_view name,
                       std::string_view expected) {
        const auto field = Expect(type, name, "StructProperty", 16, "InstancedStruct");
        const auto address = object + field.offset;
        Instance value{Layout<Address>(address, "instancedStruct.scriptStruct"),
                       Layout<Address>(address, "instancedStruct.memory")};
        if (!value.type || !value.data || !Derived(value.type, expected))
            throw std::runtime_error("实例化结构不匹配：" + std::string(expected));
        return value;
    }
    // TMap is a sparse array, NOT a packed list. Respect holes and allocation flags.
    std::vector<Address> Sparse(Address map, std::size_t stride) {
        const auto data = Layout<Address>(map, "dataTable.rowMapData");
        const auto count = Layout<std::int32_t>(map, "dataTable.rowMapNum");
        const auto capacity = Layout<std::int32_t>(map, "dataTable.rowMapMax");
        const auto free = Layout<std::int32_t>(map, "dataTable.rowMapNumFree");
        const auto bits = Layout<std::int32_t>(map, "dataTable.rowMapFlagsNum");
        const auto maximum = Layout<std::int32_t>(map, "dataTable.rowMapFlagsMax");
        auto flags = Layout<Address>(map, "dataTable.rowMapFlagsData");
        if (count < 0 || count > 65536 || capacity < count || capacity > 65536 ||
            free < 0 || free > count || bits < count || bits > 65536 || maximum < bits ||
            (!flags && bits > 128) || (count && !data))
            throw std::runtime_error("稀疏资源表布局不合法");
        if (!flags) flags = map + Offset("dataTable.rowMapInlineFlags");
        std::vector<std::uint32_t> allocation((count + 31) / 32);
        Bytes(flags, allocation.data(), allocation.size() * sizeof(std::uint32_t));
        std::vector<Address> result;
        result.reserve(count - free);
        for (std::int32_t i = 0; i < count; ++i)
            if (allocation[i / 32] & (1u << (i % 32))) result.push_back(data + i * stride);
        if (result.size() != static_cast<std::size_t>(count - free))
            throw std::runtime_error("资源表分配计数不一致");
        return result;
    }
    struct Table {
        Address type{};
        std::map<std::uint64_t, Address> rows;
    };
    Table Rows(Address table, std::string_view row_type) {
        if (!IsA(table, "DataTable")) throw std::runtime_error("资源不是 DataTable");
        Table result;
        result.type = Layout<Address>(table, "dataTable.rowStruct");
        if (!result.type || ObjectName(result.type) != row_type)
            throw std::runtime_error("数据表行类型不匹配：" + std::string(row_type));
        for (auto pair : Sparse(table + Offset("dataTable.rowMap"),
                                Offset("dataTable.rowMapElementStride"))) {
            const auto key = Read<Name>(pair);
            const auto row = Read<Address>(pair + Offset("dataTable.rowMapRowOffset"));
            if (!key.index || !row || !result.rows.emplace(Key(key), row).second)
                throw std::runtime_error("角色表包含无效或重复行键");
        }
        return result;
    }
    struct Function {
        Handle handle{};
        std::size_t size{};
        std::map<std::string, Property, std::less<>> fields;
    };
    Function& Bind(const std::string& path) {
        if (auto found = functions_.find(path); found != functions_.end() &&
            Resolve(found->second.handle)) return found->second;
        const auto& spec = resource_.at("functions").at(path);
        const auto function = Find(path);
        if (!IsA(function, "Function")) throw std::runtime_error("目标不是 UFunction");
        Function result;
        result.handle = Pin(function);
        result.size = spec.at("size").get<std::size_t>();
        const auto count = Layout<std::uint8_t>(function, "ufunction.numParms");
        const auto size = Layout<std::uint16_t>(function, "ufunction.parmsSize");
        const auto ret = Layout<std::uint16_t>(function, "ufunction.returnValueOffset");
        const auto flags = Layout<std::uint32_t>(function, "ufunction.flags");
        // Dumper C++ parameter structs include trailing alignment padding.
        // UFunction::ParmsSize may stop at the last reflected parameter instead.
        std::size_t parameter_end{};
        for (const auto& param : spec.at("fields"))
            parameter_end = std::max(parameter_end,
                param.at("offset").get<std::size_t>() + param.at("size").get<std::size_t>());
        if (!result.handle.id || result.size > 1024 || parameter_end > result.size ||
            (size != parameter_end && size != result.size) ||
            count != spec.at("fields").size() || (flags & 0x40u))
            throw std::runtime_error("函数 ABI 或本地调用合同不匹配：" + path +
                "（参数数=" + std::to_string(count) + "/" +
                std::to_string(spec.at("fields").size()) + "，ParmsSize=" +
                std::to_string(size) + "，参数末端=" + std::to_string(parameter_end) +
                "，存储=" + std::to_string(result.size) + "，Flags=" +
                std::to_string(flags) + "）");
        std::uint16_t expected_return = 0xffff;
        for (const auto& param : spec.at("fields")) {
            const auto name = param.at("name").get<std::string>();
            auto field = Expect(function, name, param.at("type").get<std::string>(),
                                param.at("size").get<std::size_t>());
            if (field.offset != param.at("offset").get<std::int32_t>() ||
                field.offset + field.size > size)
                throw std::runtime_error("函数参数偏移不匹配：" + path + "." + name);
            if (name == "ReturnValue") expected_return = static_cast<std::uint16_t>(field.offset);
            result.fields.emplace(name, std::move(field));
        }
        if (ret != expected_return) throw std::runtime_error("返回参数偏移不匹配：" + path);
        result.size = size;
        return functions_.insert_or_assign(path, std::move(result)).first->second;
    }
    class Parameters {
    public:
        explicit Parameters(Function& function) : function_(function) {}
        template<class T> void Put(std::string_view name, const T& value) {
            const auto& f = function_.fields.at(std::string(name));
            if (sizeof(T) != static_cast<std::size_t>(f.size))
                throw std::runtime_error("参数类型大小不匹配");
            std::memcpy(data.data() + f.offset, &value, sizeof(T));
        }
        template<class T> T Get(std::string_view name) const {
            const auto& f = function_.fields.at(std::string(name));
            if (sizeof(T) != static_cast<std::size_t>(f.size))
                throw std::runtime_error("返回参数类型大小不匹配");
            T value{};
            std::memcpy(&value, data.data() + f.offset, sizeof(T));
            return value;
        }
        alignas(16) std::array<std::uint8_t, 1024> data{};
    private:
        Function& function_;
    };
    template<class Fill> Parameters Call(const std::string& path, Address object, Fill fill) {
        auto& function = Bind(path);
        Parameters params(function);
        fill(params);
        const auto receiver = Pin(object);
        const auto target = Resolve(function.handle);
        if (!receiver.id || !Resolve(receiver) || !target ||
            !process_.Invoke(object, target, params.data.data(), function.size))
            throw std::runtime_error("本地反射调用失败：" + path);
        return params;
    }
    Parameters Call(const std::string& path, Address object) {
        return Call(path, object, [](Parameters&) {});
    }
    template<class T> void Set(Address object, const char* function, const char* param,
                               const T& value) {
        Call(std::string("/Script/Engine.") + function, object,
             [&](Parameters& p) { p.Put(param, value); });
    }
    void Reset() { functions_.clear(); }
private:
    Address ObjectAt(std::uint32_t index, std::uint32_t& serial) {
        // Spawning/loading can grow GUObjectArray within this Game callback.
        // Host snapshots still use the count sampled at the start of the tick.
        const auto count = Layout<std::int32_t>(registry_, "objects.countOffset");
        const auto chunks_count = Layout<std::int32_t>(registry_, "objects.numChunksOffset");
        const auto chunk_size = Offset("objects.chunkSize");
        if (count <= 0 || index >= static_cast<std::uint32_t>(count) || !chunk_size ||
            chunks_count <= 0 || index / chunk_size >= static_cast<std::size_t>(chunks_count))
            return 0;
        const auto chunks = Layout<Address>(registry_, "objects.itemsOffset");
        const auto chunk = Read<Address>(chunks + (index / chunk_size) * sizeof(Address));
        const auto slot = chunk + (index % chunk_size) * Offset("objects.itemStride");
        serial = Layout<std::uint32_t>(slot, "objects.serialOffset");
        return Layout<Address>(slot, "objects.objectOffset");
    }
    Process& process_;
    Json resource_;
    Address registry_{};
    std::map<std::string, Function, std::less<>> functions_;
};
} // namespace character_studio
