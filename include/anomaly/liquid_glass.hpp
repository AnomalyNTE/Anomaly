#pragma once

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <span>

#include <dxgiformat.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct D3D12_CPU_DESCRIPTOR_HANDLE;
struct ID3D12DescriptorHeap;

// Liquid glass for the ImGui hosts.
//
// The material is a real backdrop effect, not a translucent fill: a draw-list
// callback copies whatever the back buffer holds at that point of the frame and
// blurs a reduced copy of it, and every glass surface drawn after that point is
// shaded by a pixel shader that samples the copy through a rounded-rectangle
// lens -- refraction and a faint chromatic split along the rim, a specular edge
// lit from the upper left, and a soft drop shadow.
//
// Only hosts that own their D3D11 device can install the renderer. Without it,
// or with Reduce Transparency on, every surface falls back to a solid enough
// fill with the same rim, so no layout depends on the effect being available.
namespace anomaly::glass {

enum class Variant : std::uint8_t {
    // Functional layer: sidebars, toolbars, segmented controls, toasts, menus
    // and floating windows. Blurred, lightly tinted, refracting rim.
    Regular,
    // Highly translucent and sharp. Used for a control while it is being
    // manipulated, such as the knob of a switch that is held down.
    Clear,
    // Content layer: a thicker frosted material for cards and lists. No lens,
    // so stacked content never reads as glass on glass.
    Material,
    // The sliding selection of a sidebar, tab bar or segmented control: an
    // unblurred, magnifying droplet that refracts the label under it.
    Thumb,
};

struct SurfaceOptions final {
    Variant variant{Variant::Regular};
    float rounding{16.0f};
    // Multiplies every pixel metric of the effect (edge, refraction, shadow).
    float scale{1.0f};
    bool shadow{true};
    // Clip the surface and its shadow to the draw list's current clip
    // rectangle. Floating surfaces turn this off so their shadow can spill.
    bool clip{true};
    // Overrides the variant's tint strength when >= 0.
    float tint_strength{-1.0f};
    // Overrides the variant's face blur when >= 0: 0 sharp, 1 light, 2 heavy.
    float face_blur{-1.0f};
    // Overrides the sharpest blur the rim may sample when >= 0, in the same
    // units as face_blur. A deep face needs a soft rim, or the bevel reads as a
    // hard ring inside the pane.
    float rim_blur{-1.0f};
    // Ask the capture behind this surface for the deep backdrop: two extra blur
    // runs over the heavy level, for a pane that sits on a picture rather than on
    // a themed background. Only the frame that requests it pays for the passes.
    bool deep_backdrop{false};
    // Overrides the variant's tint colour when its alpha is > 0; the alpha is
    // how far toward the colour the tint moves.
    ImVec4 tint{0.0f, 0.0f, 0.0f, 0.0f};
};

// palette_tint selects the tinted material the shells ship with; a host that
// should stay neutral (the launcher) turns it off and frosts without a colour.
[[nodiscard]] bool InitializeRenderer(ID3D11Device* device, ID3D11DeviceContext* context,
    bool palette_tint = true) noexcept;
// A host whose back buffer is a D3D12 resource: the material records into the
// command list the host is already drawing the UI with.
[[nodiscard]] bool InitializeRendererD3D12(ID3D12Device* device, DXGI_FORMAT format,
    bool palette_tint = true) noexcept;
// Called once per frame, next to the back buffer that frame will present, before
// the UI is recorded. It resets the per-frame state the surfaces draw from.
// host_heap is the shader-visible heap the host binds before it draws the UI.
// The backend has to restore it, because ImGui's D3D12 backend only rebinds its
// root table per command and would otherwise sample the material's textures.
void BeginD3D12Frame(ID3D12GraphicsCommandList* list, ID3D12Resource* back_buffer,
    D3D12_CPU_DESCRIPTOR_HANDLE frame_rtv, ID3D12DescriptorHeap* host_heap) noexcept;
void ShutdownRenderer() noexcept;
[[nodiscard]] bool RendererAvailable() noexcept;
// The renderer is installed and Reduce Transparency is off.
[[nodiscard]] bool Active() noexcept;

void SetReduceTransparency(bool enabled) noexcept;
[[nodiscard]] bool ReduceTransparency() noexcept;
void SetReduceMotion(bool enabled) noexcept;
[[nodiscard]] bool ReduceMotion() noexcept;

// True when the surface colour of the active palette is dark.
[[nodiscard]] bool DarkAppearance() noexcept;

// Snapshots everything rendered so far in the frame. Glass drawn afterwards
// refracts that snapshot. `blur` also refreshes the blurred copy; a thumb only
// needs the sharp one. `deep` adds the wider runs a pane over a live picture
// asks for, and is ignored without `blur`.
void CaptureBackdrop(ImDrawList* list, bool blur = true, bool deep = false) noexcept;
// Draws the sliding selection droplet over what is already drawn under it.
// Draw it before the labels it carries so they stay crisp on top.
void DrawThumb(ImDrawList* list, ImVec2 min, ImVec2 max, float rounding, float scale,
    bool accent = true) noexcept;
// Eases a rectangle toward `target`. The motion is tracked relative to
// `origin` (the control's own position), so moving or dragging the window
// carries the selection with it instead of making it chase the new place.
[[nodiscard]] ImRect AnimateThumb(ImGuiID id, const ImRect& target, ImVec2 origin) noexcept;
void DrawSurface(ImDrawList* list, ImVec2 min, ImVec2 max, const SurfaceOptions& options) noexcept;
// Gives the current top-level window a glass body. The surface is inserted at
// the very start of the window's draw list when the frame is rendered, so it
// sits under the window's own content, resize grips included. Popups, menus
// and tooltips receive one automatically while glass is active.
void RegisterWindowSurface(const SurfaceOptions& options) noexcept;
// Call between ImGui::Render() and the renderer backend: inserts the window
// surfaces registered this frame under their windows.
void PrepareDrawData(ImDrawData* data) noexcept;

// Eases a per-item value toward `target`; instant under Reduce Motion.
[[nodiscard]] float Animate(ImGuiID id, float target, float speed = 16.0f) noexcept;

// Switch with the semantics of ImGui::Checkbox: a label after "##" is hidden,
// a visible label is drawn to the right, and the return value reports a
// toggle.
bool Switch(const char* label, bool* value) noexcept;
// Draws only the switch visual. `on` is the eased on-amount and `press` the
// eased pressed-amount, both 0..1.
void DrawSwitch(ImDrawList* list, ImVec2 min, ImVec2 max, float on, float press,
    bool hovered, float scale) noexcept;
// Size of a switch at the current frame height.
[[nodiscard]] ImVec2 SwitchSize() noexcept;

struct SegmentedOptions final {
    float height{30.0f};
    float padding_x{14.0f};
    float scale{1.0f};
    bool glass_track{true};
    std::uint32_t disabled_mask{};
};

// A capsule segmented control. Returns true when the selection changed.
bool SegmentedControl(const char* id, std::span<const char* const> labels, int& selected,
    const SegmentedOptions& options) noexcept;

// A slider with the semantics of ImGui::SliderScalar: a thin capsule track
// filled with the accent up to a round knob, which turns into a clear glass
// lens while it is dragged. The value is printed at the right end of the
// frame instead of under the knob, so the knob never covers a digit.
bool SliderScalar(const char* label, ImGuiDataType data_type, void* value, const void* minimum,
    const void* maximum, const char* format = nullptr, ImGuiSliderFlags flags = 0) noexcept;
bool SliderFloat(const char* label, float* value, float minimum, float maximum,
    const char* format = "%.3f", ImGuiSliderFlags flags = 0) noexcept;
bool SliderInt(const char* label, int* value, int minimum, int maximum,
    const char* format = "%d", ImGuiSliderFlags flags = 0) noexcept;

// Where the visible ink of a single glyph is centred, relative to the text
// position AddText would be given. Icon fonts carry uneven side bearings, so
// centring by advance width leaves icons visibly off centre.
[[nodiscard]] ImVec2 GlyphInkCenter(ImFont* font, float size, const char* glyph) noexcept;

// The accent as a control fill: light accents are deepened until a white
// knob or label stays legible on them.
[[nodiscard]] ImVec4 ControlAccent() noexcept;

// ImGui::BeginCombo with a thin chevron in place of the solid arrow button.
// Call ImGui::EndCombo when it returns true.
bool BeginCombo(const char* label, const char* preview, ImGuiComboFlags flags = 0) noexcept;
bool Combo(const char* label, int* current, const char* const items[], int count) noexcept;

// ImGui tab bars with a glass track and a glass thumb on the selected tab.
// Call EndTabBar only when BeginTabBar returned true.
bool BeginTabBar(const char* id, ImGuiTabBarFlags flags) noexcept;
void EndTabBar() noexcept;
[[nodiscard]] float SegmentedControlWidth(
    std::span<const char* const> labels, const SegmentedOptions& options) noexcept;

}  // namespace anomaly::glass
