#include "splitter.h"

#include <windowsx.h>

namespace gw2ui {

namespace {

constexpr wchar_t kClassName[] = L"Gw2Splitter";

struct State {
    SplitOrientation orientation = SplitOrientation::Vertical;
    SplitterDragCallback on_drag;
    bool dragging = false;
    POINT last_screen_pos{};
};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* state = new State();
        state->orientation = static_cast<SplitOrientation>(reinterpret_cast<intptr_t>(create->lpCreateParams));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (msg == WM_NCDESTROY) {
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, state->orientation == SplitOrientation::Vertical ? IDC_SIZEWE : IDC_SIZENS));
        return TRUE;
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        state->dragging = true;
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(hwnd, &pt);
        state->last_screen_pos = pt;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (state->dragging) {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(hwnd, &pt);
            int delta = (state->orientation == SplitOrientation::Vertical) ? (pt.x - state->last_screen_pos.x)
                                                                            : (pt.y - state->last_screen_pos.y);
            state->last_screen_pos = pt;
            if (delta != 0 && state->on_drag) {
                state->on_drag(delta);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (state->dragging) {
            state->dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_BTNSHADOW + 1));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void register_splitter_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNSHADOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND create_splitter(HWND parent, HINSTANCE instance, int control_id, SplitOrientation orientation) {
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance,
                            reinterpret_cast<LPVOID>(static_cast<intptr_t>(orientation)));
}

void set_drag_callback(HWND splitter, SplitterDragCallback callback) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(splitter, GWLP_USERDATA));
    if (state != nullptr) {
        state->on_drag = std::move(callback);
    }
}

} // namespace gw2ui
