#pragma once

#include <Windows.h>
#include <imm.h>
#include <msctf.h>
#include <wrl/client.h>

#include <mutex>
#include <utility>

namespace ue5mem::embedded {

// ImGui runs on Render; IMM context association belongs to the HWND thread.
// Borrow its native IMM/TSF text context while an overlay field has focus, then
// restore the game's associations (which may deliberately disable text input).
class EmbeddedImeContext final {
public:
    [[nodiscard]] bool Initialize(HWND window) noexcept {
        window_ = window;
        message_ = RegisterWindowMessageW(L"Anomaly.EmbeddedIme.Update.v1");
        return message_ != 0;
    }

    [[nodiscard]] UINT Message() const noexcept { return message_; }
    // Called only on the window thread, including reentrant IMM messages.
    [[nodiscard]] bool HandleMessage(
        UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) const noexcept {
        if (!active_) return false;
        switch (message) {
        case WM_IME_SETCONTEXT:
        case WM_IME_NOTIFY:
        case WM_IME_CONTROL:
        case WM_IME_COMPOSITIONFULL:
        case WM_IME_SELECT:
        case WM_IME_CHAR:
        case WM_IME_REQUEST:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_INPUTLANGCHANGEREQUEST:
        case WM_INPUTLANGCHANGE:
            // Windows owns the composition/candidate UI and translates a commit
            // into WM_CHAR once; the game's procedure must not disable this IME.
            result = DefWindowProcW(window_, message, wparam, lparam);
            return true;
        default:
            return false;
        }
    }

    void Request(bool visible, POINT position) noexcept {
        try {
            {
                std::scoped_lock lock(mutex_);
                requested_visible_ = visible;
                position_ = position;
            }
            RequestApply();
        } catch (...) {
        }
    }

    // Let the host finish WM_SETFOCUS before borrowing its text document. It can
    // select a disabled TSF document from its own focus handler.
    void RequestApply() const noexcept { PostMessageW(window_, message_, 0, 0); }

    // Called by our registered window message, on the HWND thread.
    [[nodiscard]] bool Apply(bool text_input_allowed) noexcept {
        bool visible{};
        POINT position{};
        try {
            std::scoped_lock lock(mutex_);
            visible = requested_visible_;
            position = position_;
        } catch (...) {
            return false;
        }
        if (!visible || !text_input_allowed || GetFocus() != window_) {
            Reset();
            return true;
        }
        if (!active_) {
            previous_ = ImmGetContext(window_);
            if (previous_ != nullptr) ImmReleaseContext(window_, previous_);
            // Association can synchronously send WM_IME_SETCONTEXT. Route it to
            // Windows from the start, rather than back through the game's IME.
            active_ = true;
            if (!DetachTextServices() ||
                !ImmAssociateContextEx(window_, nullptr, IACE_DEFAULT) ||
                !FocusNativeTextServices()) {
                Reset();
                return false;
            }
        }
        if (HIMC context = ImmGetContext(window_)) {
            COMPOSITIONFORM composition{};
            composition.dwStyle = CFS_FORCE_POSITION;
            composition.ptCurrentPos = position;
            ImmSetCompositionWindow(context, &composition);
            CANDIDATEFORM candidate{};
            candidate.dwStyle = CFS_CANDIDATEPOS;
            candidate.ptCurrentPos = position;
            ImmSetCandidateWindow(context, &candidate);
            ImmReleaseContext(window_, context);
        }
        return true;
    }

    // HWND thread only. Cancel unfinished composition before returning input.
    void Reset(bool restore_focus = true) noexcept {
        if (!active_) return;
        // Cancellation/association can reenter WndProc. Relinquish ownership
        // before either call so a collapsed-menu reset cannot recurse.
        const HIMC previous = previous_;
        previous_ = nullptr;
        active_ = false;
        auto manager = std::move(thread_manager_);
        auto association = std::move(previous_document_);
        auto focus = std::move(previous_focus_);
        // AssociateFocus does not retain the supplied document. Keep it alive
        // through restoration, and release every TSF object on the HWND thread.
        auto input_document = std::move(input_document_);
        if (HIMC context = ImmGetContext(window_)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(window_, context);
        }
        ImmAssociateContext(window_, previous);
        if (manager != nullptr) {
            Microsoft::WRL::ComPtr<ITfDocumentMgr> replaced;
            manager->AssociateFocus(window_, association.Get(), &replaced);
            // Losing HWND focus must not move TSF away from the new window.
            if (restore_focus && GetFocus() == window_ && focus != nullptr)
                manager->SetFocus(focus.Get());
        }
    }

    // Complete restoration before the subclass and its renderer can be removed.
    // A hung window leaves the existing renderer generation retained by its owner.
    [[nodiscard]] bool Release() noexcept {
        if (!IsWindow(window_)) return true;
        try {
            std::scoped_lock lock(mutex_);
            requested_visible_ = false;
        } catch (...) {
            return false;
        }
        DWORD_PTR result{};
        return SendMessageTimeoutW(window_, message_, 0, 0,
                   SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result) != 0 && result == TRUE;
    }

private:
    // A TSF-aware game can focus an empty document even when IMM is enabled.
    // Removing its HWND binding lets Windows provide the native IMM document;
    // restoring HIMC alone leaves TSF keyboard input disabled in that case.
    [[nodiscard]] bool DetachTextServices() noexcept {
        const HMODULE module = GetModuleHandleW(L"msctf.dll");
        if (module == nullptr) return true;
        using GetThreadManager = HRESULT(WINAPI*)(ITfThreadMgr**);
        const auto get_manager = reinterpret_cast<GetThreadManager>(
            GetProcAddress(module, "TF_GetThreadMgr"));
        if (get_manager == nullptr) return false;
        Microsoft::WRL::ComPtr<ITfThreadMgr> manager;
        if (FAILED(get_manager(&manager))) return false;
        if (manager == nullptr) return true;
        Microsoft::WRL::ComPtr<ITfDocumentMgr> focus;
        if (FAILED(manager->GetFocus(&focus))) return false;
        if (FAILED(manager->AssociateFocus(window_, nullptr, &previous_document_)))
            return false;
        previous_focus_ = std::move(focus);
        thread_manager_ = std::move(manager);
        return true;
    }

    [[nodiscard]] bool FocusNativeTextServices() noexcept {
        if (thread_manager_ == nullptr) return true;
        // There is no GetAssociatedDocument API. Exchange and restore the HWND
        // binding to obtain the document established by Windows after detaching
        // the game's binding. The old TSF focus is independent of that binding.
        if (FAILED(thread_manager_->AssociateFocus(window_, nullptr, &input_document_)) ||
            input_document_ == nullptr) return false;
        Microsoft::WRL::ComPtr<ITfDocumentMgr> replaced;
        if (FAILED(thread_manager_->AssociateFocus(window_, input_document_.Get(), &replaced)))
            return false;
        return SUCCEEDED(thread_manager_->SetFocus(input_document_.Get()));
    }

    HWND window_{};
    UINT message_{};
    std::mutex mutex_;
    bool requested_visible_{};
    POINT position_{};
    HIMC previous_{};
    bool active_{};
    Microsoft::WRL::ComPtr<ITfThreadMgr> thread_manager_;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> previous_document_;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> previous_focus_;
    Microsoft::WRL::ComPtr<ITfDocumentMgr> input_document_;
};

}  // namespace ue5mem::embedded
