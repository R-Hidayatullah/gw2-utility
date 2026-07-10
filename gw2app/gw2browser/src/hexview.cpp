#include "hexview.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace gw2hex {

namespace {

constexpr wchar_t kClassName[] = L"Gw2HexView";
constexpr int kBytesPerRow = 16;

struct State {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int scroll_pos = 0; // in rows
    HFONT font = nullptr;
    int line_height = 16;
    int char_width = 8;
};

size_t row_count(const State& state) {
    return state.size == 0 ? 0 : (state.size + kBytesPerRow - 1) / kBytesPerRow;
}

int visible_rows(HWND hwnd, const State& state) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    return std::max(1, static_cast<int>(rc.bottom / std::max(1, state.line_height)));
}

void update_scrollbar(HWND hwnd, State& state) {
    int total = static_cast<int>(row_count(state));
    int page = visible_rows(hwnd, state);
    state.scroll_pos = std::clamp(state.scroll_pos, 0, std::max(0, total - 1));

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, total - 1);
    si.nPage = static_cast<UINT>(page);
    si.nPos = state.scroll_pos;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void format_row(const State& state, size_t row_index, wchar_t* out, size_t out_capacity) {
    size_t start = row_index * kBytesPerRow;
    wchar_t hex[kBytesPerRow * 3 + 1] = {};
    wchar_t ascii[kBytesPerRow + 1] = {};

    for (int i = 0; i < kBytesPerRow; i++) {
        size_t idx = start + static_cast<size_t>(i);
        if (idx < state.size) {
            uint8_t byte = state.data[idx];
            swprintf(hex + i * 3, 4, L"%02X ", byte);
            ascii[i] = (byte >= 32 && byte <= 126) ? static_cast<wchar_t>(byte) : L'.';
        } else {
            hex[i * 3] = L' ';
            hex[i * 3 + 1] = L' ';
            hex[i * 3 + 2] = L' ';
            ascii[i] = L' ';
        }
    }

    // NB: swprintf's "%s" means a *narrow* char* per the ISO C rules (only
    // "%ls"/"%S" means wide) -- using "%s" here silently truncated every
    // field to one character, since the null high byte of the next wchar_t
    // looked like a narrow string terminator.
    swprintf(out, out_capacity, L"%08zX  %ls %ls", start, hex, ascii);
}

void paint(HWND hwnd, State& state) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));

    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, state.font));
    SetBkMode(hdc, TRANSPARENT);

    size_t total_rows = row_count(state);
    int rows_on_screen = visible_rows(hwnd, state) + 1;
    wchar_t line[128];

    for (int i = 0; i < rows_on_screen; i++) {
        size_t row = static_cast<size_t>(state.scroll_pos) + static_cast<size_t>(i);
        if (row >= total_rows) {
            break;
        }
        format_row(state, row, line, 128);
        TextOutW(hdc, 4, i * state.line_height, line, static_cast<int>(wcslen(line)));
    }

    SelectObject(hdc, old_font);
    EndPaint(hwnd, &ps);
}

void scroll_by(HWND hwnd, State& state, int delta_rows) {
    int total = static_cast<int>(row_count(state));
    int new_pos = std::clamp(state.scroll_pos + delta_rows, 0, std::max(0, total - 1));
    if (new_pos == state.scroll_pos) {
        return;
    }
    state.scroll_pos = new_pos;
    update_scrollbar(hwnd, state);
    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_NCCREATE: {
        auto* state = new State();
        state->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        HDC hdc = GetDC(hwnd);
        HFONT old_font = static_cast<HFONT>(SelectObject(hdc, state->font));
        TEXTMETRICW tm;
        GetTextMetricsW(hdc, &tm);
        state->line_height = tm.tmHeight + tm.tmExternalLeading;
        state->char_width = tm.tmAveCharWidth;
        SelectObject(hdc, old_font);
        ReleaseDC(hwnd, hdc);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_NCDESTROY: {
        auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (state) {
            DeleteObject(state->font);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    default:
        break;
    }

    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state == nullptr) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_SIZE:
        update_scrollbar(hwnd, *state);
        return 0;
    case WM_PAINT:
        paint(hwnd, *state);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_VSCROLL: {
        int page = visible_rows(hwnd, *state);
        switch (LOWORD(wparam)) {
        case SB_LINEUP: scroll_by(hwnd, *state, -1); break;
        case SB_LINEDOWN: scroll_by(hwnd, *state, 1); break;
        case SB_PAGEUP: scroll_by(hwnd, *state, -page); break;
        case SB_PAGEDOWN: scroll_by(hwnd, *state, page); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            state->scroll_pos = si.nTrackPos;
            update_scrollbar(hwnd, *state);
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }
        default:
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        scroll_by(hwnd, *state, -notches * 3);
        return 0;
    }
    case WM_SETFOCUS:
        SetFocus(hwnd);
        return 0;
    case WM_KEYDOWN: {
        int page = visible_rows(hwnd, *state);
        switch (wparam) {
        case VK_UP: scroll_by(hwnd, *state, -1); break;
        case VK_DOWN: scroll_by(hwnd, *state, 1); break;
        case VK_PRIOR: scroll_by(hwnd, *state, -page); break;
        case VK_NEXT: scroll_by(hwnd, *state, page); break;
        case VK_HOME: scroll_by(hwnd, *state, -static_cast<int>(row_count(*state))); break;
        case VK_END: scroll_by(hwnd, *state, static_cast<int>(row_count(*state))); break;
        default: break;
        }
        return 0;
    }
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void register_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
}

HWND create(HWND parent, HINSTANCE instance, int control_id, int x, int y, int width, int height) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, x, y,
                            width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance,
                            nullptr);
}

void set_data(HWND hwnd, const uint8_t* data, size_t size) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (state == nullptr) {
        return;
    }
    state->data = data;
    state->size = size;
    state->scroll_pos = 0;
    update_scrollbar(hwnd, *state);
    InvalidateRect(hwnd, nullptr, TRUE);
}

} // namespace gw2hex
