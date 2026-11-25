#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>

namespace ue5mem::embedded {
inline D3D12_SHADER_RESOURCE_VIEW_DESC SharedTextureView(DXGI_FORMAT format,bool opaque) noexcept {
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Shader4ComponentMapping = opaque ?
        D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0,1,2,D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1) :
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    return view;
}
// The caller has already duplicated the borrowed NT handle into a scope-owned
// descriptor. Opening it creates an owning COM reference to the shared heap;
// no producer pointer, CPU pixel buffer, command flush, or fence wait is used.
inline HRESULT OpenSharedTexture(
    ID3D12Device* device, void* shared_handle, std::uint32_t width, std::uint32_t height,
    Microsoft::WRL::ComPtr<ID3D12Resource>& texture, DXGI_FORMAT& view_format) noexcept {
    if (!device || !shared_handle || !width || !height) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Resource> opened;
    const auto result = device->OpenSharedHandle(shared_handle,IID_PPV_ARGS(&opened));
    if (FAILED(result)) return result;
    const auto description = opened->GetDesc();
    if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        description.Width != width || description.Height != height ||
        description.DepthOrArraySize != 1 || description.MipLevels != 1 ||
        description.SampleDesc.Count != 1 ||
        (description.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) return E_INVALIDARG;
    switch (description.Format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: view_format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: view_format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default: return E_INVALIDARG;
    }
    texture = std::move(opened);
    return S_OK;
}
}
