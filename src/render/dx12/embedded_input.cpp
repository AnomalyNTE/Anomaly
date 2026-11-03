#include "embedded_host_internal.hpp"
#include "embedded_input_policy.hpp"

#include "anomaly/host_ui_service.hpp"

#include <imgui.h>
#include <backends/imgui_impl_win32.h>

#include <imm.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>

#if defined(_MSC_VER)
#pragma comment(lib, "imm32.lib")
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

namespace ue5mem::embedded {
namespace {

bool IsMouseMessage(UINT message) noexcept {
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
}

bool IsKeyboardMessage(UINT message) noexcept {
    switch (message) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        return true;
    default:
        return false;
    }
}

bool IsKeyboardReleaseMessage(UINT message) noexcept {
    return message == WM_KEYUP || message == WM_SYSKEYUP;
}

bool IsTextMessage(UINT message) noexcept {
    switch (message) {
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
    case WM_UNICHAR:
        return true;
    default:
        return false;
    }
}

bool IsClipboardMessage(UINT message) noexcept {
    switch (message) {
    case WM_PASTE:
    case WM_COPY:
    case WM_CUT:
    case WM_CLEAR:
        return true;
    default:
        return false;
    }
}

bool IsFocusMessage(UINT message) noexcept {
    return message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_INPUTLANGCHANGE;
}

bool IsFocusLossMessage(UINT message, WPARAM wparam) noexcept {
    return message == WM_KILLFOCUS || message == WM_DESTROY ||
        message == WM_NCDESTROY ||
        (message == WM_ACTIVATEAPP && wparam == FALSE);
}

void ClearInputMailbox(
    EmbeddedInputMailbox& mailbox, anomaly::InputResetReason reason) noexcept {
    mailbox.frame = {};
    mailbox.last_published_mouse_x = 0.0F;
    mailbox.last_published_mouse_y = 0.0F;
    mailbox.pending_wheel_delta = 0;
    mailbox.mouse_position_published = false;
    mailbox.reset_pending = true;
    mailbox.reset_reason = reason;
}

void UpdateModifiers(anomaly::InputFrameState& frame) noexcept {
    const bool shift = frame.keys[VK_SHIFT] || frame.keys[VK_LSHIFT] ||
        frame.keys[VK_RSHIFT];
    const bool control = frame.keys[VK_CONTROL] || frame.keys[VK_LCONTROL] ||
        frame.keys[VK_RCONTROL];
    const bool alt = frame.keys[VK_MENU] || frame.keys[VK_LMENU] ||
        frame.keys[VK_RMENU];
    const bool super = frame.keys[VK_LWIN] || frame.keys[VK_RWIN];
    frame.modifiers =
        (shift ? anomaly::ToMask(anomaly::InputModifier::Shift) : 0U) |
        (control ? anomaly::ToMask(anomaly::InputModifier::Control) : 0U) |
        (alt ? anomaly::ToMask(anomaly::InputModifier::Alt) : 0U) |
        (super ? anomaly::ToMask(anomaly::InputModifier::Super) : 0U);
}

UINT NormalizeModifierVirtualKey(UINT virtual_key, LPARAM lparam) noexcept {
    switch (virtual_key) {
    case VK_SHIFT: {
        const UINT scan_code = static_cast<UINT>((lparam >> 16U) & 0xffU);
        const UINT translated = MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX);
        return translated == VK_LSHIFT || translated == VK_RSHIFT
            ? translated
            : virtual_key;
    }
    case VK_CONTROL:
        return (lparam & 0x01000000L) != 0 ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return (lparam & 0x01000000L) != 0 ? VK_RMENU : VK_LMENU;
    default:
        return virtual_key;
    }
}

void RecordKeyboardMessage(
    EmbeddedInputMailbox& mailbox, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!IsKeyboardMessage(message) || wparam == 0 ||
        wparam >= anomaly::kInputKeyCount) {
        return;
    }
    const bool down = !IsKeyboardReleaseMessage(message);
    const UINT virtual_key = static_cast<UINT>(wparam);
    mailbox.frame.keys[virtual_key] = down;
    const UINT normalized = NormalizeModifierVirtualKey(virtual_key, lparam);
    if (normalized < anomaly::kInputKeyCount) mailbox.frame.keys[normalized] = down;
    UpdateModifiers(mailbox.frame);
}

int MouseButtonForMessage(UINT message, WPARAM wparam) noexcept {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
    case WM_RBUTTONUP:
        return 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    case WM_MBUTTONUP:
        return 2;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
    case WM_XBUTTONUP:
        return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4;
    default:
        return -1;
    }
}

bool IsMouseButtonRelease(UINT message) noexcept {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}

void RecordMouseMessage(
    EmbeddedInputMailbox& mailbox, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    const int button = MouseButtonForMessage(message, wparam);
    if (message == WM_MOUSEMOVE || button >= 0) {
        mailbox.frame.mouse_x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
        mailbox.frame.mouse_y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
    }
    if (button >= 0 && static_cast<std::size_t>(button) < mailbox.frame.mouse_buttons.size()) {
        mailbox.frame.mouse_buttons[static_cast<std::size_t>(button)] =
            !IsMouseButtonRelease(message);
    }
    if (message == WM_MOUSEWHEEL) {
        const std::int64_t delta = static_cast<std::int64_t>(GET_WHEEL_DELTA_WPARAM(wparam));
        const std::int64_t lower = (std::numeric_limits<std::int32_t>::min)();
        const std::int64_t upper = (std::numeric_limits<std::int32_t>::max)();
        mailbox.pending_wheel_delta = (std::clamp)(
            mailbox.pending_wheel_delta + delta, lower, upper);
    }
}

void ReconcileAsyncInput(EmbeddedState& state, EmbeddedInputMailbox& mailbox) noexcept {
    // GetAsyncKeyState reads the desktop-wide keyboard state, which would leak
    // keystrokes typed into other applications into game-local input (and
    // therefore into plugin hotkeys). Only reconcile physical keys while the
    // game window owns the foreground.
    const bool foreground =
        state.window != nullptr && GetForegroundWindow() == state.window;
    if (!foreground) {
        mailbox.frame.keys.fill(false);
        mailbox.frame.mouse_buttons.fill(false);
        UpdateModifiers(mailbox.frame);
        return;
    }
    for (std::size_t key = 1; key < mailbox.frame.keys.size(); ++key) {
        mailbox.frame.keys[key] =
            (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
    constexpr UINT mouse_virtual_keys[]{
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (std::size_t index{}; index < std::size(mouse_virtual_keys); ++index) {
        mailbox.frame.mouse_buttons[index] =
            (GetAsyncKeyState(static_cast<int>(mouse_virtual_keys[index])) & 0x8000) != 0;
    }
    POINT cursor{};
    if (state.window != nullptr && GetCursorPos(&cursor) &&
        ScreenToClient(state.window, &cursor)) {
        mailbox.frame.mouse_x = static_cast<float>(cursor.x);
        mailbox.frame.mouse_y = static_cast<float>(cursor.y);
    }
    UpdateModifiers(mailbox.frame);
}

struct DrainedInputFrame final {
    anomaly::InputFrameState frame;
    anomaly::InputResetReason reset_reason{anomaly::InputResetReason::None};
};

DrainedInputFrame DrainInputFrame(EmbeddedState& state) noexcept {
    EmbeddedInputMailbox& mailbox = state.input_mailbox;
    std::scoped_lock lock(mailbox.mutex);
    if (mailbox.reset_pending) {
        const anomaly::InputResetReason reason = mailbox.reset_reason;
        mailbox.reset_pending = false;
        mailbox.reset_reason = anomaly::InputResetReason::None;
        return {{}, reason};
    }

    ReconcileAsyncInput(state, mailbox);
    anomaly::InputFrameState frame = mailbox.frame;
    frame.timestamp_milliseconds = static_cast<std::uint64_t>(GetTickCount64());
    if (mailbox.mouse_position_published) {
        frame.mouse_delta_x = frame.mouse_x - mailbox.last_published_mouse_x;
        frame.mouse_delta_y = frame.mouse_y - mailbox.last_published_mouse_y;
    } else {
        frame.mouse_delta_x = 0.0F;
        frame.mouse_delta_y = 0.0F;
        mailbox.mouse_position_published = true;
    }
    mailbox.last_published_mouse_x = frame.mouse_x;
    mailbox.last_published_mouse_y = frame.mouse_y;
    frame.mouse_wheel_delta = static_cast<std::int32_t>(mailbox.pending_wheel_delta);
    mailbox.pending_wheel_delta = 0;
    frame.keys[0] = false;
    return {frame, anomaly::InputResetReason::None};
}

anomaly::InputUiCaptureState CurrentUiCapture() noexcept {
    anomaly::InputUiCaptureState capture;
    if (ImGui::GetCurrentContext() == nullptr || !anomaly::HostUiMenusCaptureMouse()) {
        return capture;
    }
    const ImGuiIO& io = ImGui::GetIO();
    capture.flags = anomaly::ToFlags(anomaly::InputCaptureFlag::Mouse);
    if (io.WantCaptureKeyboard) {
        capture.flags |= anomaly::ToFlags(anomaly::InputCaptureFlag::Keyboard);
    }
    if (io.WantTextInput || io.WantCaptureKeyboard) {
        capture.flags |= anomaly::ToFlags(anomaly::InputCaptureFlag::Text);
    }
    // This runs both immediately after NewFrame and after every plugin has
    // closed its windows. Use global queries so capture is valid without a
    // current ImGui window on either boundary.
    capture.item_hovered = ImGui::IsAnyItemHovered();
    capture.window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow);
    capture.item_active = ImGui::IsAnyItemActive();
    return capture;
}

void SetEmbeddedImeData(
    ImGuiContext*, ImGuiViewport* viewport, ImGuiPlatformImeData* data) {
    auto* state = g_state.load(std::memory_order_acquire);
    if (state == nullptr || viewport->PlatformHandleRaw != state->window) return;
    const POINT position{
        static_cast<LONG>(data->InputPos.x - viewport->Pos.x),
        static_cast<LONG>(data->InputPos.y - viewport->Pos.y)};
    state->ime.Request(data->WantVisible, position);
}

void QueueClipboardText(ImGuiIO& io) noexcept {
    try {
        const char* text = ImGui::GetClipboardText();
        if (text != nullptr && *text != '\0') io.AddInputCharactersUTF8(text);
    } catch (...) {
        // Clipboard providers are outside the renderer's failure boundary.
    }
}

void QueueUnicodeCharacter(WPARAM wparam, ImGuiIO& io) noexcept {
    if (wparam == UNICODE_NOCHAR || wparam == 0 || wparam > 0x10ffff) return;
    try {
        io.AddInputCharacter(static_cast<unsigned int>(wparam));
    } catch (...) {
        // A rejected character is preferable to unwinding through WndProc.
    }
}

bool IsRawMouseInput(LPARAM lparam) noexcept {
    RAWINPUTHEADER header{};
    UINT size = sizeof(header);
    return GetRawInputData(
               reinterpret_cast<HRAWINPUT>(lparam), RID_HEADER, &header, &size,
               sizeof(RAWINPUTHEADER)) == sizeof(header) &&
        header.dwType == RIM_TYPEMOUSE;
}

LRESULT ForwardToGameWindowProc(
    EmbeddedState* state, HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    return state != nullptr && state->original_window_proc != nullptr
        ? CallWindowProcW(state->original_window_proc, window, message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK EmbeddedWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = g_state.load(std::memory_order_acquire);
    if (state != nullptr && window == state->window) {
        // Restoration also runs while Render is stopping, before this subclass
        // and the ImGui context are released.
        if (state->ime.Message() != 0 && message == state->ime.Message())
            return state->ime.Apply(!anomaly::HostUiMenusCollapsed()) ? TRUE : FALSE;
        if (IsFocusLossMessage(message, wparam)) state->ime.Reset(false);
        else if (message == WM_SETFOCUS) state->ime.RequestApply();
    }
    if (state != nullptr && state->renderer.load() == RendererLifecycle::Ready) {
        RecordEmbeddedInputMessage(*state, message, wparam, lparam);
    }
    if (state != nullptr && state->renderer.load() == RendererLifecycle::Ready &&
        ImGui::GetCurrentContext() != nullptr) {
        // A locked window remains visible while menus are collapsed, but it
        // never owns input. The menu gate therefore precedes all ImGui flags.
        const bool menus_expanded = !anomaly::HostUiMenusCollapsed();
        if (!menus_expanded) state->ime.Reset();
        const ImGuiIO& io = ImGui::GetIO();
        const bool capture_mouse = anomaly::HostUiMenusCaptureMouse();
        const bool capture_keyboard = menus_expanded && io.WantCaptureKeyboard;
        // WantTextInput is the explicit desktop/mobile text-input signal. On
        // desktop ImGui versions it is commonly false for InputText, while
        // WantCaptureKeyboard is true, so accept either signal for text.
        const bool capture_text = menus_expanded &&
            (io.WantTextInput || io.WantCaptureKeyboard);
        const bool mouse_message = IsMouseMessage(message);
        const bool raw_mouse = message == WM_INPUT && IsRawMouseInput(lparam);
        const bool keyboard_message = IsKeyboardMessage(message);
        const bool text_message = IsTextMessage(message);
        const bool clipboard_message = IsClipboardMessage(message);
        const bool focus_message = IsFocusMessage(message);

        if (ShouldForwardCollapsedClientCursorToGame(
                capture_mouse, message,
                static_cast<std::uint16_t>(LOWORD(lparam)))) {
            return ForwardToGameWindowProc(state, window, message, wparam, lparam);
        }

        if (capture_mouse && (mouse_message || raw_mouse)) {
            static_cast<void>(ImGui_ImplWin32_WndProcHandler(
                window, message, wparam, lparam));
        }
        if (capture_mouse && mouse_message) {
            return TRUE;
        }
        if (capture_mouse && raw_mouse) {
            // DefWindowProc performs the required WM_INPUT cleanup after the
            // backend has observed the packet. Do not leave raw input queued.
            return DefWindowProcW(window, message, wparam, lparam);
        }

        // Keep ImGui's key state coherent across a menu transition. Release
        // messages are forwarded without ever being consumed by the game.
        const bool forward_keyboard = keyboard_message &&
            ((menus_expanded && capture_keyboard) || IsKeyboardReleaseMessage(message));
        const bool forward_text = text_message && capture_text;
        const bool forward_clipboard = clipboard_message && capture_text;
        if (forward_keyboard || forward_text || forward_clipboard ||
            focus_message) {
            static_cast<void>(ImGui_ImplWin32_WndProcHandler(
                window, message, wparam, lparam));
            if (forward_text && message == WM_UNICHAR) {
                QueueUnicodeCharacter(wparam, ImGui::GetIO());
            }
            if (forward_clipboard && message == WM_PASTE) QueueClipboardText(ImGui::GetIO());
        }

        LRESULT ime_result{};
        if (state->ime.HandleMessage(message, wparam, lparam, ime_result)) return ime_result;
        if (capture_keyboard && keyboard_message) return TRUE;
        if (capture_text && (text_message || clipboard_message)) {
            return TRUE;
        }
    }
    return ForwardToGameWindowProc(state, window, message, wparam, lparam);
}

}  // namespace

void RecordEmbeddedInputMessage(
    EmbeddedState& state, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try {
        EmbeddedInputMailbox& mailbox = state.input_mailbox;
        std::scoped_lock lock(mailbox.mutex);
        if (IsFocusLossMessage(message, wparam)) {
            ClearInputMailbox(mailbox, anomaly::InputResetReason::FocusLost);
            return;
        }
        RecordKeyboardMessage(mailbox, message, wparam, lparam);
        RecordMouseMessage(mailbox, message, wparam, lparam);
    } catch (...) {
        // WndProc must not allow synchronization failures to unwind into the
        // game. A subsequent Render frame will reconcile physical input.
    }
}

bool PublishEmbeddedInputFrame(EmbeddedState& state) noexcept {
    if (state.plugins == nullptr) return false;
    try {
        const DrainedInputFrame drained = DrainInputFrame(state);
        if (drained.reset_reason != anomaly::InputResetReason::None) {
            state.plugins->ResetInput(drained.reset_reason);
            return false;
        }
        state.plugins->PublishInputFrame(drained.frame, CurrentUiCapture());
        return true;
    } catch (...) {
        try {
            std::scoped_lock lock(state.input_mailbox.mutex);
            ClearInputMailbox(state.input_mailbox, anomaly::InputResetReason::Explicit);
        } catch (...) {
        }
        return false;
    }
}

void PublishEmbeddedUiCapture(EmbeddedState& state) noexcept {
    if (state.plugins == nullptr) return;
    try {
        state.plugins->PublishUiCapture(CurrentUiCapture());
    } catch (...) {
        // Capture is advisory frame-local state. The next input ingress will
        // replace it, so renderer progress is more important than retrying.
    }
}

bool InstallEmbeddedInput(EmbeddedState& state) noexcept {
    state.input_installed = false;
    state.input_install_error = ERROR_SUCCESS;
    if (state.window == nullptr || !IsWindow(state.window)) {
        state.input_install_error = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }
    if (!state.ime.Initialize(state.window)) {
        state.input_install_error = GetLastError();
        return false;
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrW(
        state.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EmbeddedWindowProc));
    if (previous == 0) {
        const DWORD error = GetLastError();
        state.input_install_error = error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error;
        if (error != ERROR_SUCCESS) {
            // The exchange was refused, so the game still owns its procedure and
            // there is nothing to undo.
            return false;
        }
        // The exchange went through but produced no forwarding target. Leaving
        // this procedure installed would route every game message to
        // DefWindowProc and the window would stop responding, so put the
        // window back the only way still available and report the failure.
        if (GetWindowLongPtrW(state.window, GWLP_WNDPROC) ==
            reinterpret_cast<LONG_PTR>(EmbeddedWindowProc)) {
            SetLastError(ERROR_SUCCESS);
            static_cast<void>(SetWindowLongPtrW(state.window, GWLP_WNDPROC, previous));
        }
        return false;
    }
    state.original_window_proc = reinterpret_cast<WNDPROC>(previous);
    state.input_installed = true;
    ImGui::GetPlatformIO().Platform_SetImeDataFn = SetEmbeddedImeData;
    return true;
}

bool RestoreEmbeddedInput(EmbeddedState& state) noexcept {
    try {
        std::scoped_lock lock(state.input_mailbox.mutex);
        ClearInputMailbox(state.input_mailbox, anomaly::InputResetReason::FocusLost);
    } catch (...) {
    }
    if (state.original_window_proc == nullptr || !IsWindow(state.window)) {
        state.original_window_proc = nullptr;
        state.input_installed = false;
        return true;
    }
    if (!state.ime.Release()) return false;
    state.input_installed = false;
    const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(state.window, GWLP_WNDPROC));
    if (current == EmbeddedWindowProc) {
        SetWindowLongPtrW(
            state.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state.original_window_proc));
    }
    state.original_window_proc = nullptr;
    return true;
}

}  // namespace ue5mem::embedded
