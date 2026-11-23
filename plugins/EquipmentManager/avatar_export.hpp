#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace equipment_manager::avatar_export {

// Fixed reflected call ABI, not UObject layouts. Reader supplies controlled
// function/CDO handles, Profile entry validation and Game-only ProcessEvent.
struct Vector2 { double x{}, y{}; };
struct Color { float r{}, g{}, b{}, a{}; };
struct StringReference {
    const wchar_t* data{};
    std::int32_t count{}, capacity{};
};
struct DrawContext { std::uintptr_t target{}; };
struct CreateParameters {
    std::uintptr_t world{};
    std::int32_t width{}, height{};
    std::uint8_t format{};
    std::array<std::byte, 3> format_padding{};
    Color clear{};
    std::uint8_t generate_mips{}, support_uavs{};
    std::array<std::byte, 2> result_padding{};
    std::uintptr_t result{};
};
struct BeginParameters {
    std::uintptr_t world{}, target{}, canvas{};
    Vector2 size;
    DrawContext context;
};
struct DrawParameters {
    std::uintptr_t texture{};
    Vector2 position, size, uv_position, uv_size{1., 1.};
    Color color{1.f, 1.f, 1.f, 1.f};
    std::uint8_t blend{};
    std::array<std::byte, 3> blend_padding{};
    float rotation{};
    Vector2 pivot;
};
struct EndParameters { std::uintptr_t world{}; DrawContext context; };
struct ReleaseParameters { std::uintptr_t target{}; };

struct alignas(16) WriteOptions {
    std::uint8_t format{};
    std::array<std::byte, 3> format_padding{};
    // An unbound FScriptDelegate, not a native function pointer. Its packed
    // reflected position is intentionally represented by bytes, not alignas(8).
    std::array<std::byte, 16> completion{};
    std::int32_t compression_quality{100};
    std::uint8_t overwrite{1}, asynchronous{1};
    // Unreflected native TFunction state stays empty. The validated native
    // implementation tests its null invoker before touching capture storage.
    std::array<std::byte, 54> empty_native_state{};
};
struct alignas(16) ExportParameters {
    std::uintptr_t texture{};
    StringReference filename;
    std::array<std::byte, 8> options_padding{};
    WriteOptions options;
};
struct SoftObjectReference { std::array<std::byte, 40> bytes{}; };
struct ScriptDelegate { std::uint64_t weak_object{}, function_name{}; };
struct LatentInfo {
    std::int32_t linkage{-1}, uuid{};
    std::uint64_t execution_function{};
    std::uintptr_t callback_target{};
};
struct LoadParameters {
    std::uintptr_t world{};
    SoftObjectReference asset;
    ScriptDelegate loaded;
    LatentInfo latent;
};

static_assert(sizeof(std::uintptr_t) == 8 && sizeof(wchar_t) == 2);
static_assert(sizeof(Vector2) == 16 && sizeof(Color) == 16 && sizeof(StringReference) == 16);
static_assert(sizeof(CreateParameters) == 48 && offsetof(CreateParameters, clear) == 20 &&
    offsetof(CreateParameters, result) == 40);
static_assert(sizeof(BeginParameters) == 48 && offsetof(BeginParameters, context) == 40);
static_assert(sizeof(DrawParameters) == 112 && offsetof(DrawParameters, color) == 72 &&
    offsetof(DrawParameters, blend) == 88 && offsetof(DrawParameters, rotation) == 92 &&
    offsetof(DrawParameters, pivot) == 96);
static_assert(sizeof(EndParameters) == 16 && sizeof(ReleaseParameters) == 8);
static_assert(sizeof(WriteOptions) == 80 && alignof(WriteOptions) == 16 &&
    offsetof(WriteOptions, completion) == 4 && offsetof(WriteOptions, compression_quality) == 20 &&
    offsetof(WriteOptions, overwrite) == 24 && offsetof(WriteOptions, asynchronous) == 25);
static_assert(sizeof(ExportParameters) == 112 && offsetof(ExportParameters, filename) == 8 &&
    offsetof(ExportParameters, options) == 32);
static_assert(sizeof(LatentInfo) == 24 && offsetof(LatentInfo, callback_target) == 16);
static_assert(sizeof(LoadParameters) == 88 && offsetof(LoadParameters, asset) == 8 &&
    offsetof(LoadParameters, loaded) == 48 && offsetof(LoadParameters, latent) == 64);

struct ParameterContract {
    const char* name;
    const char* kind;
    std::size_t offset, size;
    const char* value_type{};
};

inline constexpr ParameterContract kCreate[] = {
    {"WorldContextObject", "ObjectProperty", offsetof(CreateParameters, world), 8},
    {"Width", "IntProperty", offsetof(CreateParameters, width), 4},
    {"Height", "IntProperty", offsetof(CreateParameters, height), 4},
    {"Format", "ByteProperty", offsetof(CreateParameters, format), 1},
    {"ClearColor", "StructProperty", offsetof(CreateParameters, clear), 16, "LinearColor"},
    {"bAutoGenerateMipMaps", "BoolProperty", offsetof(CreateParameters, generate_mips), 1},
    {"bSupportUAVs", "BoolProperty", offsetof(CreateParameters, support_uavs), 1},
    {"ReturnValue", "ObjectProperty", offsetof(CreateParameters, result), 8}};
inline constexpr ParameterContract kBegin[] = {
    {"WorldContextObject", "ObjectProperty", offsetof(BeginParameters, world), 8},
    {"TextureRenderTarget", "ObjectProperty", offsetof(BeginParameters, target), 8},
    {"Canvas", "ObjectProperty", offsetof(BeginParameters, canvas), 8},
    {"Size", "StructProperty", offsetof(BeginParameters, size), 16, "Vector2D"},
    {"Context", "StructProperty", offsetof(BeginParameters, context), 8, "DrawToRenderTargetContext"}};
inline constexpr ParameterContract kDraw[] = {
    {"RenderTexture", "ObjectProperty", offsetof(DrawParameters, texture), 8},
    {"ScreenPosition", "StructProperty", offsetof(DrawParameters, position), 16, "Vector2D"},
    {"ScreenSize", "StructProperty", offsetof(DrawParameters, size), 16, "Vector2D"},
    {"CoordinatePosition", "StructProperty", offsetof(DrawParameters, uv_position), 16, "Vector2D"},
    {"CoordinateSize", "StructProperty", offsetof(DrawParameters, uv_size), 16, "Vector2D"},
    {"RenderColor", "StructProperty", offsetof(DrawParameters, color), 16, "LinearColor"},
    {"BlendMode", "ByteProperty", offsetof(DrawParameters, blend), 1},
    {"Rotation", "FloatProperty", offsetof(DrawParameters, rotation), 4},
    {"PivotPoint", "StructProperty", offsetof(DrawParameters, pivot), 16, "Vector2D"}};
inline constexpr ParameterContract kEnd[] = {
    {"WorldContextObject", "ObjectProperty", offsetof(EndParameters, world), 8},
    {"Context", "StructProperty", offsetof(EndParameters, context), 8, "DrawToRenderTargetContext"}};
inline constexpr ParameterContract kExport[] = {
    {"Texture", "ObjectProperty", offsetof(ExportParameters, texture), 8},
    {"Filename", "StrProperty", offsetof(ExportParameters, filename), 16},
    {"Options", "StructProperty", offsetof(ExportParameters, options), 80, "ImageWriteOptions"}};
inline constexpr ParameterContract kRelease[] = {
    {"TextureRenderTarget", "ObjectProperty", offsetof(ReleaseParameters, target), 8}};
inline constexpr ParameterContract kLoad[] = {
    {"WorldContextObject", "ObjectProperty", offsetof(LoadParameters, world), 8},
    {"Asset", "SoftObjectProperty", offsetof(LoadParameters, asset), 40},
    {"OnLoaded", "DelegateProperty", offsetof(LoadParameters, loaded), 16},
    {"LatentInfo", "StructProperty", offsetof(LoadParameters, latent), 24, "LatentActionInfo"}};

template<class Reader, std::size_t N>
void Describe(Reader& reader, std::uintptr_t function, const ParameterContract (&fields)[N]) {
    for (const auto& field : fields)
        reader.Parameter(function, field.name, field.kind, field.offset, field.size,
            field.value_type ? field.value_type : "");
}

template<class Reader, std::size_t N>
std::uintptr_t Function(Reader& reader, std::string_view path, std::string_view symbol,
                        std::size_t size, const ParameterContract (&fields)[N]) {
    return reader.AvatarFunction(path, symbol, size, [&](std::uintptr_t function) { Describe(reader, function, fields); });
}

template<class Reader>
std::uintptr_t ExportFunction(Reader& reader) {
    return reader.AvatarFunction("/Script/ImageWriteQueue.ImageWriteBlueprintLibrary:ExportToDisk", "avatar-export-png",
        sizeof(ExportParameters), [&](std::uintptr_t function) {
            Describe(reader, function, kExport);
            const auto options = reader.Exact("/Script/ImageWriteQueue.ImageWriteOptions").second;
            if (reader.template Field<std::int32_t>(options, "structSize") != static_cast<std::int32_t>(sizeof(WriteOptions)))
                throw std::runtime_error("The reflected image write options ABI is unsupported");
            reader.Property(options, "Format", "EnumProperty", offsetof(WriteOptions, format), 1);
            reader.Property(options, "OnComplete", "DelegateProperty", offsetof(WriteOptions, completion), 16);
            reader.Property(options, "CompressionQuality", "IntProperty", offsetof(WriteOptions, compression_quality), 4);
            reader.Property(options, "bOverwriteFile", "BoolProperty", offsetof(WriteOptions, overwrite), 1);
            reader.Property(options, "bAsync", "BoolProperty", offsetof(WriteOptions, asynchronous), 1);
        });
}

// All Reader methods below run on Game. AvatarFunction validates/caches only
// successful bindings and runs its descriptor once; AvatarInvoke reacquires
// receiver/function handles. It never retains a caller-owned frame or string.
// Creation queues resource initialization. Its return is a weak UE identity,
// not a GC pin or proof that TextureRHI is ready for ExportToDisk's precheck.
template<class Reader>
std::uintptr_t CreateTarget(Reader& reader, std::uintptr_t world, std::int32_t edge = 256) {
    if (edge <= 0) throw std::runtime_error("A positive avatar image size is required");
    const auto create = Function(reader, "/Script/Engine.KismetRenderingLibrary:CreateRenderTarget2D", "avatar-create-target", sizeof(CreateParameters), kCreate);
    const auto library = reader.AvatarDefaultObject("/Script/Engine.KismetRenderingLibrary");
    // UI textures are sampled into linear RGB. Encode the render target as
    // sRGB before PNG readback; alpha stays linear. A linear UNORM target
    // would be displayed too dark by the browser's ordinary PNG pipeline.
    const auto rgba = reader.AvatarEnumValue("/Script/Engine.ETextureRenderTargetFormat", "RTF_RGBA8_SRGB");
    CreateParameters target{};
    target.world = world; target.width = edge; target.height = edge; target.format = static_cast<std::uint8_t>(rgba);
    reader.AvatarInvoke(library, create, &target, sizeof(target));
    if (!target.result) throw std::runtime_error("The game did not create the avatar render target");
    return target.result;
}

// Caller resolves the retained weak identity and checks the Profile-bound
// Game resource/TextureRHI fields on a later update before this single export.
// EndCanvas submits drawing before readback on the same render queue. Later
// UE GC releases resources through that queue; encoding owns copied pixels.
template<class Reader>
std::uintptr_t Submit(Reader& reader, std::uintptr_t source_texture, std::uintptr_t world,
                      std::wstring_view filename, std::uintptr_t existing_target, std::int32_t edge = 256) {
    if (filename.empty() || filename.size() >= static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) || edge <= 0)
        throw std::runtime_error("A bounded avatar output path and positive image size are required");
    const auto begin = Function(reader, "/Script/Engine.KismetRenderingLibrary:BeginDrawCanvasToRenderTarget", "avatar-begin-canvas", sizeof(BeginParameters), kBegin);
    const auto draw = Function(reader, "/Script/Engine.Canvas:K2_DrawTexture", "avatar-draw-texture", sizeof(DrawParameters), kDraw);
    const auto end = Function(reader, "/Script/Engine.KismetRenderingLibrary:EndDrawCanvasToRenderTarget", "avatar-end-canvas", sizeof(EndParameters), kEnd);
    const auto export_png = ExportFunction(reader);
    const auto library = reader.AvatarDefaultObject("/Script/Engine.KismetRenderingLibrary");
    const auto writer = reader.AvatarDefaultObject("/Script/ImageWriteQueue.ImageWriteBlueprintLibrary");
    const auto opaque = reader.AvatarEnumValue("/Script/Engine.EBlendMode", "BLEND_Opaque");
    const auto png = reader.AvatarEnumValue("/Script/ImageWriteQueue.EDesiredImageFormat", "PNG");
    std::wstring output(filename);
    BeginParameters canvas{}; canvas.world = world; canvas.target = existing_target;
    reader.AvatarInvoke(library, begin, &canvas, sizeof(canvas));
    if (!canvas.context.target) throw std::runtime_error("The game did not begin the avatar canvas");
    EndParameters finish{}; finish.world = world; finish.context = canvas.context;
    try {
        if (!canvas.canvas) throw std::runtime_error("The game did not return an avatar canvas");
        DrawParameters pixels{};
        pixels.texture = source_texture; pixels.size = {static_cast<double>(edge), static_cast<double>(edge)}; pixels.blend = static_cast<std::uint8_t>(opaque);
        reader.AvatarInvoke(canvas.canvas, draw, &pixels, sizeof(pixels));
    } catch (...) {
        reader.AvatarInvoke(library, end, &finish, sizeof(finish));
        throw;
    }
    reader.AvatarInvoke(library, end, &finish, sizeof(finish));
    ExportParameters image{};
    image.texture = existing_target;
    image.filename = {output.c_str(), static_cast<std::int32_t>(output.size() + 1), static_cast<std::int32_t>(output.size() + 1)};
    image.options.format = static_cast<std::uint8_t>(png);
    reader.AvatarInvoke(writer, export_png, &image, sizeof(image));
    return existing_target;
}

// Called only after completion/failure and only while the caller's weak UE
// identity still resolves. Never release the target before Submit's readback.
template<class Reader>
void Release(Reader& reader, std::uintptr_t target) {
    const auto release = Function(reader, "/Script/Engine.KismetRenderingLibrary:ReleaseRenderTarget2D",
        "avatar-release-target", sizeof(ReleaseParameters), kRelease);
    const auto library = reader.AvatarDefaultObject("/Script/Engine.KismetRenderingLibrary");
    ReleaseParameters parameters{target};
    reader.AvatarInvoke(library, release, &parameters, sizeof(parameters));
}

// Asset is copied from a verified native soft reference for this Game callback;
// it is not reconstructed from invented FName IDs. The engine copies the path.
template<class Reader>
void RequestLoad(Reader& reader, std::uintptr_t world, const SoftObjectReference& asset,
                 std::uintptr_t controller, std::int32_t uuid) {
    const auto load = reader.AvatarFunction("/Script/Engine.KismetSystemLibrary:LoadAsset", "avatar-load-asset",
        sizeof(LoadParameters), [&](std::uintptr_t function) {
            Describe(reader, function, kLoad);
            const auto info = reader.Exact("/Script/Engine.LatentActionInfo").second;
            if (reader.template Field<std::int32_t>(info, "structSize") != static_cast<std::int32_t>(sizeof(LatentInfo)))
                throw std::runtime_error("The reflected latent action ABI is unsupported");
            reader.Property(info, "Linkage", "IntProperty", offsetof(LatentInfo, linkage), 4);
            reader.Property(info, "UUID", "IntProperty", offsetof(LatentInfo, uuid), 4);
            reader.Property(info, "ExecutionFunction", "NameProperty", offsetof(LatentInfo, execution_function), 8);
            reader.Property(info, "CallbackTarget", "ObjectProperty", offsetof(LatentInfo, callback_target), 8);
        });
    const auto library = reader.AvatarDefaultObject("/Script/Engine.KismetSystemLibrary");
    LoadParameters parameters{}; parameters.world = world; parameters.asset = asset;
    parameters.latent.uuid = uuid; parameters.latent.callback_target = controller;
    reader.AvatarInvoke(library, load, &parameters, sizeof(parameters));
}

} // namespace equipment_manager::avatar_export
