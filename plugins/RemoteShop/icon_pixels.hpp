#pragma once
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>
#include <array>

namespace shop {
inline double LineDistance(double x,double y,double ax,double ay,double bx,double by) {
    const auto dx = bx - ax,dy = by - ay;
    const auto t = (std::clamp)(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy),0.0,1.0);
    return std::hypot(x - ax - t * dx,y - ay - t * dy);
}
inline std::array<uint8_t,32 * 32 * 4> CheckboxIconPixels(bool checked) {
    std::array<uint8_t,32 * 32 * 4> pixels{};
    for (size_t y = 0; y < 32; ++y) for (size_t x = 0; x < 32; ++x) {
        const auto qx = std::abs(x + .5 - 16) - 9.5,qy = std::abs(y + .5 - 16) - 9.5;
        const auto distance = std::hypot((std::max)(qx,0.0),(std::max)(qy,0.0)) + (std::min)((std::max)(qx,qy),0.0) - 4;
        const auto outer = (std::clamp)(.5 - distance,0.0,1.0);
        const auto inner = (std::clamp)(.5 - distance - 1.7,0.0,1.0);
        const auto tick = checked ? (std::clamp)(1.65 - (std::min)(LineDistance(x + .5,y + .5,8,16,14,22),LineDistance(x + .5,y + .5,14,22,24,10)),0.0,1.0) : 0;
        const auto offset = (y * 32 + x) * 4;
        const std::array<double,3> color = checked ? std::array<double,3>{77,151,221} : std::array<double,3>{145,171,194};
        for (size_t c = 0; c < 3; ++c) pixels[offset + c] = uint8_t(std::lround(color[c] * (1 - tick) + 250 * tick));
        pixels[offset + 3] = uint8_t(std::lround((checked ? outer : outer - inner) * 255));
    }
    return pixels;
}
inline std::array<uint8_t,32 * 32 * 4> CloseIconPixels() {
    std::array<uint8_t,32 * 32 * 4> pixels{};
    const auto distance = [](double x,double y,double ax,double ay,double bx,double by) {
        const auto dx = bx - ax,dy = by - ay;
        const auto t = (std::clamp)(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy),0.0,1.0);
        return std::hypot(x - ax - t * dx,y - ay - t * dy);
    };
    for (size_t y = 0; y < 32; ++y) for (size_t x = 0; x < 32; ++x) {
        const auto d = (std::min)(distance(x + .5,y + .5,7,7,25,25),distance(x + .5,y + .5,7,25,25,7));
        const auto offset = (y * 32 + x) * 4;
        pixels[offset] = 140; pixels[offset + 1] = 163; pixels[offset + 2] = 186;
        pixels[offset + 3] = uint8_t(std::lround((std::clamp)(1.25 - d,0.0,1.0) * 255));
    }
    return pixels;
}
inline std::array<uint8_t,32 * 32 * 4> StarIconPixels(bool filled) {
    std::array<std::array<double,2>,10> vertices{};
    constexpr double pi = 3.14159265358979323846;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const double angle = -pi / 2 + i * pi / 5,radius = i % 2 ? 6 : 13;
        vertices[i] = {16 + std::cos(angle) * radius,16 + std::sin(angle) * radius};
    }
    std::array<uint8_t,32 * 32 * 4> pixels{};
    for (size_t y = 0; y < 32; ++y) for (size_t x = 0; x < 32; ++x) {
        const double px = x + .5,py = y + .5;
        double distance = 100; bool inside{};
        for (size_t i = 0,j = vertices.size() - 1; i < vertices.size(); j = i++) {
            const auto& a = vertices[i]; const auto& b = vertices[j];
            distance = (std::min)(distance,LineDistance(px,py,a[0],a[1],b[0],b[1]));
            if ((a[1] > py) != (b[1] > py) && px < (b[0] - a[0]) * (py - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
        }
        const auto d = inside ? -distance : distance;
        const auto alpha = filled ? (std::clamp)(.5 - d,0.0,1.0) : (std::clamp)(1.3 - distance,0.0,1.0);
        const auto offset = (y * 32 + x) * 4;
        pixels[offset] = 255; pixels[offset + 1] = 202; pixels[offset + 2] = 77;
        pixels[offset + 3] = uint8_t(std::lround(alpha * (filled ? 255 : 210)));
    }
    return pixels;
}
struct ControlBitmap { uint32_t height{}; std::vector<uint8_t> pixels; };
inline ControlBitmap CenterControlIcon(std::span<const uint8_t> source,float row_height) {
    if (source.size() != 32 * 32 * 4 || !std::isfinite(row_height) || row_height < 20 || row_height > 256) return {};
    ControlBitmap result;
    result.height = static_cast<uint32_t>(std::ceil(row_height * 32 / 20));
    result.pixels.resize(size_t(32) * result.height * 4);
    const auto top = (result.height - 32) / 2;
    std::copy(source.begin(),source.end(),result.pixels.begin() + size_t(top) * 32 * 4);
    return result;
}
struct IconBounds {
    uint32_t x{},y{},width{},height{};
};
inline IconBounds VisibleIconBounds(std::span<const uint8_t> rgba,uint32_t width,uint32_t height) {
    if (!width || !height || width > 1024 || height > 1024 || rgba.size() != size_t(width) * height * 4) return {};
    uint32_t left = width,top = height,right{},bottom{};
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        // Ignore nearly invisible antialiasing/glow at the canvas edge.
        if (rgba[(size_t(y) * width + x) * 4 + 3] < 8) continue;
        left = (std::min)(left,x); top = (std::min)(top,y);
        right = (std::max)(right,x); bottom = (std::max)(bottom,y);
    }
    return left < width ? IconBounds{left,top,right - left + 1,bottom - top + 1} : IconBounds{};
}
inline IconBounds FitIcon(IconBounds source) {
    if (!source.width || !source.height) return {};
    // Match the visible size of the game's shopping-cart icon in a 56px slot.
    constexpr uint32_t canvas = 128,content = 92;
    const double scale = double(content) / (std::max)(source.width,source.height);
    const auto width = (std::clamp)(uint32_t(std::lround(source.width * scale)),1u,content);
    const auto height = (std::clamp)(uint32_t(std::lround(source.height * scale)),1u,content);
    return {(canvas - width) / 2,(canvas - height) / 2,width,height};
}
inline std::vector<uint8_t> NormalizeIcon(std::span<const uint8_t> png) {
    if (png.empty() || png.size() > 8 * 1024 * 1024) return {};
    struct ComScope {
        HRESULT status = CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        ~ComScope() { if (SUCCEEDED(status)) CoUninitialize(); }
    } com;
    if (FAILED(com.status) && com.status != RPC_E_CHANGED_MODE) return {};
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> rgba;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),static_cast<DWORD>(png.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)) ||
        FAILED(decoder->GetFrame(0,&frame))) return {};
    UINT width{},height{};
    if (FAILED(frame->GetSize(&width,&height)) || !width || !height || width > 1024 || height > 1024 ||
        FAILED(factory->CreateFormatConverter(&rgba)) ||
        FAILED(rgba->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))) return {};
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    if (FAILED(rgba->CopyPixels(nullptr,width * 4,static_cast<UINT>(pixels.size()),pixels.data()))) return {};
    const auto bounds = VisibleIconBounds(pixels,width,height), fit = FitIcon(bounds);
    if (!fit.width) return {};
    WICRect rectangle{int(bounds.x),int(bounds.y),int(bounds.width),int(bounds.height)};
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(rgba.Get(),&rectangle)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(clipper.Get(),fit.width,fit.height,WICBitmapInterpolationModeHighQualityCubic))) return {};
    std::vector<uint8_t> output(128 * 128 * 4);
    const auto offset = (size_t(fit.y) * 128 + fit.x) * 4;
    if (FAILED(scaler->CopyPixels(nullptr,128 * 4,static_cast<UINT>(output.size() - offset),output.data() + offset))) return {};
    return output;
}
} // namespace shop
