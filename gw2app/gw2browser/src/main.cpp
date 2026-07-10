#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <commctrl.h>
#include <commdlg.h>
#include <cwchar>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <windowsx.h>

#include "d3d_renderer.h"
#include "entry_extractor.h"
#include "gw2dat.h"
#include "hexview.h"
#include "info_panel.h"
#include "mft_listview.h"
#include "model_renderer.h"
#include "splitter.h"
#include "struct_template.h"

namespace {

constexpr wchar_t kMainClassName[] = L"Gw2BrowserMain";
constexpr wchar_t kPreviewClassName[] = L"Gw2PreviewSurface";
constexpr wchar_t kModelClassName[] = L"Gw2ModelSurface";

constexpr int kSplitterThickness = 5;
constexpr int kSearchBarHeight = 64;
constexpr int kMinPaneSize = 80;
constexpr int kTabHeight = 26;
constexpr int kToolbarHeight = 28;
constexpr int kStatusBarHeight = 26;

constexpr UINT_PTR ID_FILE_OPEN = 1001;
constexpr UINT_PTR ID_FILE_EXPORT_COMPRESSED = 1002;
constexpr UINT_PTR ID_FILE_EXPORT_DECOMPRESSED = 1003;
constexpr UINT_PTR ID_FILE_EXIT = 1004;
constexpr UINT_PTR ID_FILE_LOAD_TEMPLATE = 1005;
constexpr int ID_LISTVIEW = 2001;
constexpr int ID_HEX_BEFORE = 2002;
constexpr int ID_HEX_AFTER = 2003;
constexpr int ID_INFO_PANEL = 2004;
constexpr int ID_TAB = 2005;
constexpr int ID_SPLIT_LIST_MIDDLE = 2010;
constexpr int ID_SPLIT_MIDDLE_INFO = 2011;
constexpr int ID_SEARCH_EDIT = 2020;
constexpr int ID_SEARCH_FILEID_CHECK = 2021;
constexpr UINT_PTR ID_SEARCH_BUTTON = 2022;
constexpr UINT_PTR ID_CLEAR_BUTTON = 2023;
constexpr UINT_PTR ID_ZOOM_IN = 2030;
constexpr UINT_PTR ID_ZOOM_OUT = 2031;
constexpr UINT_PTR ID_ROTATE = 2032;
constexpr UINT_PTR ID_FIT = 2033;
constexpr UINT_PTR ID_MODE_FULL = 2034;
constexpr UINT_PTR ID_MODE_PLAIN = 2035;
constexpr UINT_PTR ID_MODE_WIRE = 2036;
constexpr UINT_PTR ID_MODEL_RESET = 2037;
constexpr UINT_PTR ID_MODE_SKEL = 2038;
constexpr UINT_PTR ID_MODE_SHADER = 2039;
constexpr UINT_PTR ID_ANIM_COMBO = 2050;
constexpr UINT_PTR ID_ANIM_PLAY = 2051;
constexpr UINT_PTR ID_LAYER_PROP = 2052;
constexpr UINT_PTR ID_LAYER_ZONE = 2053;
constexpr UINT_PTR ID_LAYER_COLL = 2054;
constexpr UINT_PTR ID_TEX_FULLRES = 2055;
constexpr UINT_PTR TIMER_ANIM = 1;
constexpr int ID_STATUS_LABEL = 2040;
constexpr int ID_PROGRESS = 2041;

constexpr UINT WM_APP_EXTRACT_DONE = WM_APP + 1;

enum class MiddleTab { Compressed = 0, Decompressed = 1, Preview = 2 };

// Result of a background extract_entry() call, handed back to the UI thread
// via PostMessageW (WPARAM = generation, LPARAM = heap pointer to this,
// ownership transferred to whoever handles WM_APP_EXTRACT_DONE).
struct ExtractResult {
    uint64_t generation = 0;
    uint32_t mft_index = 0;
    bool success = false;
    std::string error_message;
    ExtractedEntry entry;
};

HINSTANCE g_hinstance = nullptr;
HMENU g_file_menu = nullptr;

struct AppState {
    Gw2Dat data_gw2;
    ExtractedEntry current_entry;
    uint32_t current_mft_index = 0;
    bool dat_loaded = false;
    bool has_loaded_entry = false; // current_entry reflects a *completed* extraction, safe to export

    // Bumped on every new selection (and on opening a new archive); a
    // background result is only applied if its snapshot still matches this
    // when it comes back -- anything older is silently discarded, which is
    // what makes "select something else before the old one finishes" work.
    uint64_t request_generation = 0;

    HWND hwnd_main = nullptr;
    HWND hwnd_status_label = nullptr;
    HWND hwnd_progress = nullptr;
    HWND hwnd_search_edit = nullptr;
    HWND hwnd_search_fileid_check = nullptr;
    HWND hwnd_search_button = nullptr;
    HWND hwnd_clear_button = nullptr;
    HWND hwnd_list = nullptr;
    HWND hwnd_tab = nullptr;
    HWND hwnd_preview = nullptr;      // image D3D surface (gw2gfx)
    HWND hwnd_model = nullptr;        // model D3D surface (gw2m3d)
    HWND hwnd_text_preview = nullptr; // text / strs read-only edit
    HWND hwnd_info = nullptr;
    HWND hwnd_hex_before = nullptr;
    HWND hwnd_hex_after = nullptr;
    HWND hwnd_split_list_middle = nullptr;
    HWND hwnd_split_middle_info = nullptr;
    HWND hwnd_zoom_in = nullptr;
    HWND hwnd_zoom_out = nullptr;
    HWND hwnd_rotate = nullptr;
    HWND hwnd_fit = nullptr;
    HWND hwnd_mode_full = nullptr;
    HWND hwnd_mode_plain = nullptr;
    HWND hwnd_mode_wire = nullptr;
    HWND hwnd_mode_shader = nullptr;
    HWND hwnd_model_reset = nullptr;
    HWND hwnd_skel_toggle = nullptr;
    HWND hwnd_anim_combo = nullptr;
    HWND hwnd_anim_play = nullptr;
    HWND hwnd_tex_fullres = nullptr;
    HWND hwnd_layer_prop = nullptr;
    HWND hwnd_layer_zone = nullptr;
    HWND hwnd_layer_coll = nullptr;
    bool map_zone_loaded = false; // whether the zone layer has been lazily loaded

    // Layout ratios (0..1) of available space; scale sanely on window resize.
    double list_width_ratio = 0.22;
    double info_width_ratio = 0.18;

    // Preview zoom/pan/rotation (mirrors gw2gfx's internal state so drag math
    // has something to read back without adding renderer getters).
    bool preview_dragging = false;
    POINT preview_drag_last{};
    float preview_zoom = 1.0f;
    float preview_pan_x = 0.0f;
    float preview_pan_y = 0.0f;
    int preview_rotation_quarters = 0;

    // Model orbit-drag state (mirrors the image drag state above).
    bool model_dragging = false;
    POINT model_drag_last{};
    gw2m3d::RenderMode model_mode = gw2m3d::RenderMode::Full;
    bool show_skeleton = false; // bind-pose skeleton overlay toggle (persists across models)
};

AppState* g_app = nullptr;

// Windows' narrow file APIs (and MinGW's std::ifstream, which has no wide
// overload) expect the current ANSI code page, not UTF-8 -- convert here
// once rather than truncating the wide path byte-by-byte.
std::string to_ansi(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    int needed = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string narrow(static_cast<size_t>(needed) - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, narrow.data(), needed, nullptr, nullptr);
    return narrow;
}

void layout_children(int client_w, int client_h) {
    if (g_app == nullptr) {
        return;
    }

    // Reserve the bottom status strip (loading label + progress bar) out of
    // the height every other pane gets to use.
    const int usable_h = std::max(0, client_h - kStatusBarHeight);

    int list_w = std::clamp(static_cast<int>(client_w * g_app->list_width_ratio), kMinPaneSize,
                             std::max(kMinPaneSize, client_w - 2 * kMinPaneSize - 2 * kSplitterThickness));
    int info_w = std::clamp(static_cast<int>(client_w * g_app->info_width_ratio), kMinPaneSize,
                             std::max(kMinPaneSize, client_w - list_w - kMinPaneSize - 2 * kSplitterThickness));
    int middle_w = std::max(kMinPaneSize, client_w - list_w - info_w - 2 * kSplitterThickness);

    // Left column: search bar + list.
    MoveWindow(g_app->hwnd_search_edit, 6, 6, std::max(0, list_w - 12), 22, TRUE);
    MoveWindow(g_app->hwnd_search_fileid_check, 6, 32, std::max(0, list_w - 12 - 130), 22, TRUE);
    MoveWindow(g_app->hwnd_search_button, std::max(6, list_w - 122), 32, 55, 22, TRUE);
    MoveWindow(g_app->hwnd_clear_button, std::max(6, list_w - 63), 32, 55, 22, TRUE);
    MoveWindow(g_app->hwnd_list, 0, kSearchBarHeight, list_w, std::max(0, usable_h - kSearchBarHeight), TRUE);

    int x = list_w;
    MoveWindow(g_app->hwnd_split_list_middle, x, 0, kSplitterThickness, usable_h, TRUE);
    x += kSplitterThickness;

    const int middle_x = x;
    MoveWindow(g_app->hwnd_tab, middle_x, 0, middle_w, kTabHeight, TRUE);

    const int content_y = kTabHeight;
    const int content_h = std::max(0, usable_h - content_y);
    int sel = TabCtrl_GetCurSel(g_app->hwnd_tab);
    MiddleTab tab = (sel == 0) ? MiddleTab::Compressed : (sel == 1) ? MiddleTab::Decompressed : MiddleTab::Preview;

    ShowWindow(g_app->hwnd_hex_before, tab == MiddleTab::Compressed ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_hex_after, tab == MiddleTab::Decompressed ? SW_SHOW : SW_HIDE);

    // The single Preview tab hosts one of three surfaces depending on the
    // selected entry's detected type: image (gw2gfx), model (gw2m3d), or a
    // read-only text control (text/strs, and the fallback for anything else).
    bool show_preview = tab == MiddleTab::Preview;
    PreviewKind pk = g_app->has_loaded_entry ? g_app->current_entry.kind : PreviewKind::None;
    bool show_image = show_preview && pk == PreviewKind::Image;
    bool show_map = show_preview && pk == PreviewKind::Map && g_app->current_entry.map != nullptr &&
                    !g_app->current_entry.map->instances.empty();
    bool show_model = show_preview && ((pk == PreviewKind::Model && g_app->current_entry.model != nullptr) || show_map);
    bool show_text = show_preview && !show_image && !show_model;

    ShowWindow(g_app->hwnd_preview, show_image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_model, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_text_preview, show_text ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_zoom_in, show_image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_zoom_out, show_image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_rotate, show_image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_fit, show_image ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_mode_full, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_mode_plain, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_mode_wire, show_model ? SW_SHOW : SW_HIDE);
    // "Shader" (real DXBC) mode applies to single models only, not the map scene.
    ShowWindow(g_app->hwnd_mode_shader, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_model_reset, show_model ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_skel_toggle, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    bool show_anim = show_model && !show_map && gw2m3d::has_skeleton();
    ShowWindow(g_app->hwnd_anim_combo, show_anim ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_anim_play, show_anim ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_tex_fullres, (show_model && !show_map) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_layer_prop, show_map ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_layer_zone, show_map ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app->hwnd_layer_coll, show_map ? SW_SHOW : SW_HIDE);

    if (tab == MiddleTab::Compressed) {
        MoveWindow(g_app->hwnd_hex_before, middle_x, content_y, middle_w, content_h, TRUE);
    } else if (tab == MiddleTab::Decompressed) {
        MoveWindow(g_app->hwnd_hex_after, middle_x, content_y, middle_w, content_h, TRUE);
    } else {
        constexpr int kButtonW = 70;
        constexpr int kButtonH = 22;
        constexpr int kGap = 6;
        auto place_button = [&](HWND h, int slot) {
            MoveWindow(h, middle_x + kGap + (kButtonW + kGap) * slot, content_y + 3, kButtonW, kButtonH, TRUE);
        };
        int surface_y = content_y + kToolbarHeight;
        int surface_h = std::max(0, content_h - kToolbarHeight);
        if (show_image) {
            place_button(g_app->hwnd_zoom_in, 0);
            place_button(g_app->hwnd_zoom_out, 1);
            place_button(g_app->hwnd_rotate, 2);
            place_button(g_app->hwnd_fit, 3);
            MoveWindow(g_app->hwnd_preview, middle_x, surface_y, middle_w, surface_h, TRUE);
            gw2gfx::on_resize(middle_w, surface_h);
            // A DXGI_SWAP_EFFECT_DISCARD swapchain otherwise keeps showing its
            // last-Presented frame stretched into the new size until the next
            // real Present() -- force one immediately so resizes never look stale.
            gw2gfx::render();
        } else if (show_model) {
            place_button(g_app->hwnd_mode_full, 0);
            place_button(g_app->hwnd_mode_plain, 1);
            place_button(g_app->hwnd_mode_wire, 2);
            if (show_map) {
                place_button(g_app->hwnd_model_reset, 3);
                // Map layer toggles occupy slots 4-6.
                place_button(g_app->hwnd_layer_prop, 4);
                place_button(g_app->hwnd_layer_zone, 5);
                place_button(g_app->hwnd_layer_coll, 6);
            } else {
                // Single model: add the "Shader" (real DXBC) mode, then Reset,
                // Skeleton, and the animation controls.
                place_button(g_app->hwnd_mode_shader, 3);
                place_button(g_app->hwnd_model_reset, 4);
                place_button(g_app->hwnd_skel_toggle, 5);
                int anim_x = middle_x + kGap + (kButtonW + kGap) * 6;
                MoveWindow(g_app->hwnd_anim_combo, anim_x, content_y + 3, 150, 200, TRUE);
                MoveWindow(g_app->hwnd_anim_play, anim_x + 156, content_y + 3, 56, kButtonH, TRUE);
                MoveWindow(g_app->hwnd_tex_fullres, anim_x + 156 + 56 + kGap, content_y + 3, 90, kButtonH, TRUE);
            }
            MoveWindow(g_app->hwnd_model, middle_x, surface_y, middle_w, surface_h, TRUE);
            gw2m3d::on_resize(middle_w, surface_h);
            gw2m3d::render();
        } else {
            // text / strs / unrecognized / model-without-template: full-height
            // text control, no toolbar row.
            MoveWindow(g_app->hwnd_text_preview, middle_x, content_y, middle_w, content_h, TRUE);
        }
    }

    x = middle_x + middle_w;
    MoveWindow(g_app->hwnd_split_middle_info, x, 0, kSplitterThickness, usable_h, TRUE);
    x += kSplitterThickness;
    MoveWindow(g_app->hwnd_info, x, 0, std::max(0, client_w - x), usable_h, TRUE);

    // Bottom status strip: label on the left, progress bar on the right.
    const int status_y = usable_h;
    const int progress_w = 200;
    const int inner_h = kStatusBarHeight - 6;
    MoveWindow(g_app->hwnd_progress, std::max(0, client_w - progress_w - 8), status_y + 3, progress_w, inner_h, TRUE);
    MoveWindow(g_app->hwnd_status_label, 8, status_y + 3, std::max(0, client_w - progress_w - 24), inner_h, TRUE);
}

void relayout() {
    if (g_app == nullptr) {
        return;
    }
    RECT rc;
    GetClientRect(g_app->hwnd_main, &rc);
    layout_children(rc.right - rc.left, rc.bottom - rc.top);
}

void set_export_enabled(bool enabled) {
    if (g_file_menu == nullptr) {
        return;
    }
    UINT flags = enabled ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_COMPRESSED, flags);
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_DECOMPRESSED, flags);
}

void show_loading(bool loading, uint32_t mft_index) {
    if (g_app == nullptr) {
        return;
    }
    if (loading) {
        wchar_t buf[128];
        swprintf(buf, 128, L"Loading entry #%u...", mft_index);
        SetWindowTextW(g_app->hwnd_status_label, buf);
        ShowWindow(g_app->hwnd_progress, SW_SHOW);
        SendMessageW(g_app->hwnd_progress, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(g_app->hwnd_progress, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(g_app->hwnd_progress, SW_HIDE);
        SetWindowTextW(g_app->hwnd_status_label, L"Ready");
    }
}

void zoom_by(float factor) {
    if (g_app == nullptr) {
        return;
    }
    g_app->preview_zoom = std::clamp(g_app->preview_zoom * factor, 0.05f, 40.0f);
    gw2gfx::set_view(g_app->preview_zoom, g_app->preview_pan_x, g_app->preview_pan_y);
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
}

void rotate_90() {
    if (g_app == nullptr) {
        return;
    }
    g_app->preview_rotation_quarters = (g_app->preview_rotation_quarters + 1) % 4;
    g_app->preview_pan_x = 0.0f;
    g_app->preview_pan_y = 0.0f;
    gw2gfx::set_rotation(g_app->preview_rotation_quarters);
    gw2gfx::set_view(g_app->preview_zoom, g_app->preview_pan_x, g_app->preview_pan_y);
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
}

void fit_view() {
    if (g_app == nullptr) {
        return;
    }
    g_app->preview_zoom = 1.0f;
    g_app->preview_pan_x = 0.0f;
    g_app->preview_pan_y = 0.0f;
    gw2gfx::reset_view();
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
}

void set_model_mode(gw2m3d::RenderMode mode) {
    if (g_app == nullptr) {
        return;
    }
    g_app->model_mode = mode;
    gw2m3d::set_mode(mode);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

void reset_model_view() {
    gw2m3d::reset_view();
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

void update_preview_texture() {
    if (!g_app->current_entry.is_image || g_app->current_entry.preview_pixels.empty()) {
        gw2gfx::clear_texture();
        return;
    }

    gw2gfx::set_texture(g_app->current_entry.preview_dxgi_format, g_app->current_entry.preview_width,
                         g_app->current_entry.preview_height, g_app->current_entry.preview_pitch,
                         g_app->current_entry.preview_pixels.data());
    g_app->preview_zoom = 1.0f;
    g_app->preview_pan_x = 0.0f;
    g_app->preview_pan_y = 0.0f;
    g_app->preview_rotation_quarters = 0;
}

// Applies a (freshly extracted or failed) entry to the UI. Called only from
// the main thread: either synchronously right after a same-thread extract
// (there isn't one anymore, but kept generic) or from the WM_APP_EXTRACT_DONE
// handler once a background result's generation is confirmed current.
void apply_extracted_entry(uint32_t mft_index, ExtractedEntry&& entry) {
    if (g_app == nullptr) {
        return;
    }

    g_app->current_entry = std::move(entry);
    g_app->has_loaded_entry = true;

    gw2hex::set_data(g_app->hwnd_hex_before, g_app->current_entry.compressed.data(),
                      g_app->current_entry.compressed.size());
    gw2hex::set_data(g_app->hwnd_hex_after, g_app->current_entry.decompressed.data(),
                      g_app->current_entry.decompressed.size());

    // Route the decoded payload to whichever preview surface fits its type.
    switch (g_app->current_entry.kind) {
    case PreviewKind::Image:
        gw2m3d::clear_model();
        update_preview_texture();
        break;
    case PreviewKind::Model:
        gw2gfx::clear_texture();
        if (g_app->current_entry.model) {
            gw2m3d::set_model(*g_app->current_entry.model);
            gw2m3d::set_mode(g_app->model_mode);
            // Restore the skeleton toggle; disable + uncheck it for rig-less models.
            bool has_skel = gw2m3d::has_skeleton();
            EnableWindow(g_app->hwnd_skel_toggle, has_skel);
            bool on = has_skel && g_app->show_skeleton;
            SendMessageW(g_app->hwnd_skel_toggle, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            gw2m3d::set_show_skeleton(on);

            // Populate the animation clip selector (item 0 = bind pose).
            KillTimer(g_app->hwnd_main, TIMER_ANIM);
            SendMessageW(g_app->hwnd_anim_play, BM_SETCHECK, BST_UNCHECKED, 0);
            gw2m3d::set_playing(false);
            SendMessageW(g_app->hwnd_anim_combo, CB_RESETCONTENT, 0, 0);
            int nclips = gw2m3d::animation_count();
            if (gw2m3d::has_skeleton()) {
                // Item 0 = bind pose, 1..nclips = embedded clips.
                SendMessageW(g_app->hwnd_anim_combo, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(L"Bind pose (rest)"));
                for (int i = 0; i < nclips; ++i) {
                    wchar_t item[160];
                    const char* nm = gw2m3d::animation_name(i);
                    wchar_t wnm[96];
                    MultiByteToWideChar(CP_UTF8, 0, (nm && *nm) ? nm : "clip", -1, wnm, 96);
                    swprintf(item, 160, L"%ls  (%.2fs)", wnm, gw2m3d::animation_duration(i));
                    SendMessageW(g_app->hwnd_anim_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
                }
                SendMessageW(g_app->hwnd_anim_combo, CB_SETCURSEL, 0, 0);
                gw2m3d::set_animation(-1);
            }
        } else {
            gw2m3d::clear_model();
            SetWindowTextW(g_app->hwnd_text_preview,
                           L"This is a .modl model, but no struct template is loaded.\r\n"
                           L"Use File -> Load Struct JSON... (gw2_packfile.json) to enable model preview.");
        }
        break;
    case PreviewKind::Map:
        gw2gfx::clear_texture();
        if (g_app->current_entry.map && !g_app->current_entry.map->instances.empty()) {
            const MapScene& ms = *g_app->current_entry.map;
            std::vector<gw2m3d::SceneInstance> insts;
            insts.reserve(ms.instances.size());
            for (const MapInstance& mi : ms.instances) {
                gw2m3d::SceneInstance si;
                si.model = mi.model;
                for (int k = 0; k < 3; ++k) { si.pos[k] = mi.pos[k]; si.rot[k] = mi.rot[k]; }
                si.scale = mi.scale;
                si.layer = mi.layer;
                insts.push_back(si);
            }
            gw2m3d::set_scene(ms.models, insts);
            gw2m3d::set_mode(g_app->model_mode);
            // Default layers: props + terrain on, zones + collision off.
            g_app->map_zone_loaded = false;
            gw2m3d::set_layer_visible(gw2m3d::LAYER_PROP, true);
            gw2m3d::set_layer_visible(gw2m3d::LAYER_TERRAIN, true);
            gw2m3d::set_layer_visible(gw2m3d::LAYER_ZONE, false);
            gw2m3d::set_layer_visible(gw2m3d::LAYER_COLLISION, false);
            SendMessageW(g_app->hwnd_layer_prop, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(g_app->hwnd_layer_zone, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(g_app->hwnd_layer_coll, BM_SETCHECK, BST_UNCHECKED, 0);
        } else {
            gw2m3d::clear_model();
            SetWindowTextW(g_app->hwnd_text_preview,
                           L"This is a map (mapc/area), but its prop scene could not be built.\r\n"
                           L"Load the struct template (gw2_packfile.json) to enable map preview.");
        }
        break;
    case PreviewKind::Text:
    case PreviewKind::Strs:
        gw2gfx::clear_texture();
        gw2m3d::clear_model();
        SetWindowTextW(g_app->hwnd_text_preview, g_app->current_entry.text_preview.c_str());
        break;
    default:
        gw2gfx::clear_texture();
        gw2m3d::clear_model();
        SetWindowTextW(g_app->hwnd_text_preview, L"(no preview available for this entry)");
        break;
    }

    gw2info::show_entry_info(g_app->hwnd_info, g_app->data_gw2, mft_index, g_app->current_entry);
    set_export_enabled(true);
    relayout(); // swap in the surface that matches this entry's type
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
}

void on_entry_selected(uint32_t mft_index) {
    if (g_app == nullptr || mft_index >= g_app->data_gw2.mft_data_list.size()) {
        return;
    }

    // A fresh mouse click on a new row emits both LVN_ITEMCHANGED and NM_CLICK
    // for it; skip the duplicate while that exact row's extraction is still in
    // flight (has_loaded_entry flips true only once it completes). Re-clicking a
    // row that has already finished loading still reloads it, which is the point
    // of also listening to NM_CLICK/NM_DBLCLK.
    if (g_app->request_generation != 0 && mft_index == g_app->current_mft_index && !g_app->has_loaded_entry) {
        return;
    }

    uint64_t generation = ++g_app->request_generation;
    g_app->current_mft_index = mft_index;

    // Immediately wipe the previous entry's data from all three content panels
    // so nothing stale lingers while the new entry decodes in the background;
    // the WM_APP_EXTRACT_DONE handler refills them once it's ready.
    g_app->current_entry = ExtractedEntry{};
    g_app->has_loaded_entry = false;
    set_export_enabled(false);
    gw2hex::set_data(g_app->hwnd_hex_before, nullptr, 0);
    gw2hex::set_data(g_app->hwnd_hex_after, nullptr, 0);
    gw2gfx::clear_texture();
    gw2m3d::clear_model();
    SetWindowTextW(g_app->hwnd_text_preview, L"");
    InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);
    InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
    show_loading(true, mft_index);

    std::string file_path = g_app->data_gw2.file_info.file_path;
    MftData entry_copy = g_app->data_gw2.mft_data_list[mft_index];
    HWND hwnd_main = g_app->hwnd_main;

    // Fully self-contained: touches no shared state (own file handle inside
    // extract_entry, copies of everything else), so it's safe to just detach
    // and let stale results be discarded by generation number when they
    // arrive -- no thread ever needs to be waited on or interrupted.
    std::thread([hwnd_main, generation, mft_index, file_path = std::move(file_path), entry_copy]() {
        auto result = std::make_unique<ExtractResult>();
        result->generation = generation;
        result->mft_index = mft_index;
        try {
            result->entry = extract_entry(file_path, entry_copy);
            result->success = true;
        } catch (const std::exception& e) {
            result->success = false;
            result->error_message = e.what();
        }
        PostMessageW(hwnd_main, WM_APP_EXTRACT_DONE, static_cast<WPARAM>(generation),
                     reinterpret_cast<LPARAM>(result.release()));
    }).detach();
}

void do_open_file(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Guild Wars 2 Archive (*.dat)\0*.dat\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    HCURSOR old_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    try {
        Gw2Dat fresh;
        load_dat_file(fresh, to_ansi(path));

        // Invalidate any extraction still in flight from the previous
        // archive -- read_entry_bytes() captured its own path/copy of the
        // MftData it needs, so it'll finish safely regardless, but its
        // result must never be applied against the newly loaded archive.
        ++g_app->request_generation;
        show_loading(false, 0);

        g_app->data_gw2 = std::move(fresh);
        g_app->current_entry = ExtractedEntry{};
        g_app->has_loaded_entry = false;
        g_app->dat_loaded = true;

        gw2mft::set_source(g_app->hwnd_list, g_app->data_gw2);
        gw2hex::set_data(g_app->hwnd_hex_before, nullptr, 0);
        gw2hex::set_data(g_app->hwnd_hex_after, nullptr, 0);
        gw2gfx::clear_texture();
        gw2m3d::clear_model();
        SetWindowTextW(g_app->hwnd_text_preview, L"");
        set_export_enabled(false);
        gw2info::show_dat_info(g_app->hwnd_info, g_app->data_gw2);
        SetWindowTextW(g_app->hwnd_search_edit, L"");
        InvalidateRect(g_app->hwnd_preview, nullptr, FALSE);

        wchar_t title[512];
        swprintf(title, 512, L"gw2browser - %ls (%zu assets)", path, g_app->data_gw2.mft_base_id_data_list.size());
        SetWindowTextW(hwnd, title);
    } catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "Failed to load .dat", MB_ICONERROR);
    }

    SetCursor(old_cursor);
}

void do_load_template(HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Struct template (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    std::string error;
    if (!gw2tpl::load_from_file(to_ansi(path), error)) {
        MessageBoxA(hwnd, error.c_str(), "Failed to load struct JSON", MB_ICONERROR);
        return;
    }
    SetWindowTextW(g_app->hwnd_status_label, L"Struct template loaded.");

    // If a .modl entry is currently selected but wasn't parsed (no template was
    // loaded when it was extracted), re-extract it now that we have the template.
    if (g_app->dat_loaded && g_app->has_loaded_entry && g_app->current_entry.kind == PreviewKind::Model &&
        !g_app->current_entry.model) {
        on_entry_selected(g_app->current_mft_index);
    }
}

void do_export(HWND hwnd, bool export_compressed) {
    const std::vector<uint8_t>& data =
        export_compressed ? g_app->current_entry.compressed : g_app->current_entry.decompressed;

    if (!g_app->has_loaded_entry || data.empty()) {
        MessageBoxW(hwnd, L"Select an entry first.", L"gw2browser", MB_ICONINFORMATION);
        return;
    }

    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Binary file\0*.bin\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bin";
    ofn.Flags = OFN_OVERWRITEPROMPT;

    if (!GetSaveFileNameW(&ofn)) {
        return;
    }

    std::ofstream out(to_ansi(path), std::ios::binary);
    if (!out) {
        MessageBoxW(hwnd, L"Failed to open the file for writing.", L"gw2browser", MB_ICONERROR);
        return;
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void do_search() {
    if (!g_app->dat_loaded) {
        return;
    }

    wchar_t buf[32] = L"";
    GetWindowTextW(g_app->hwnd_search_edit, buf, 32);
    if (buf[0] == L'\0') {
        gw2mft::set_filter(g_app->hwnd_list, {});
        return;
    }

    uint32_t value = static_cast<uint32_t>(wcstoul(buf, nullptr, 10));
    bool by_file_id = SendMessageW(g_app->hwnd_search_fileid_check, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::vector<uint32_t> base_ids;
    if (by_file_id) {
        for (uint32_t file_id : search_by_file_id(g_app->data_gw2, value)) {
            uint32_t base_id = get_by_base_id(g_app->data_gw2, file_id);
            if (base_id != 0) {
                base_ids.push_back(base_id);
            }
        }
    } else {
        base_ids = search_by_base_id(g_app->data_gw2, value);
    }

    gw2mft::set_filter(g_app->hwnd_list, base_ids);
}

void do_clear_search() {
    SetWindowTextW(g_app->hwnd_search_edit, L"");
    gw2mft::set_filter(g_app->hwnd_list, {});
}

HMENU build_menu() {
    g_file_menu = CreatePopupMenu();
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_OPEN, L"&Open .dat...");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_LOAD_TEMPLATE, L"Load Struct &JSON... (gw2_packfile.json)");
    AppendMenuW(g_file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_EXPORT_COMPRESSED, L"Export &Compressed (Before)...");
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_EXPORT_DECOMPRESSED, L"Export &Decompressed (After)...");
    AppendMenuW(g_file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(g_file_menu, MF_STRING, ID_FILE_EXIT, L"E&xit");
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_COMPRESSED, MF_GRAYED | MF_DISABLED);
    EnableMenuItem(g_file_menu, ID_FILE_EXPORT_DECOMPRESSED, MF_GRAYED | MF_DISABLED);

    HMENU menu_bar = CreateMenu();
    AppendMenuW(menu_bar, MF_POPUP, reinterpret_cast<UINT_PTR>(g_file_menu), L"&File");
    return menu_bar;
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        gw2gfx::render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL: {
        if (g_app == nullptr) {
            break;
        }
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        zoom_by(std::pow(1.1f, static_cast<float>(notches)));
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (g_app != nullptr) {
            g_app->preview_dragging = true;
            g_app->preview_drag_last = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_app != nullptr && g_app->preview_dragging) {
            POINT cur{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT rc;
            GetClientRect(hwnd, &rc);
            float w = static_cast<float>(std::max<LONG>(1, rc.right - rc.left));
            float h = static_cast<float>(std::max<LONG>(1, rc.bottom - rc.top));
            g_app->preview_pan_x +=
                static_cast<float>(g_app->preview_drag_last.x - cur.x) / w / g_app->preview_zoom;
            g_app->preview_pan_y +=
                static_cast<float>(g_app->preview_drag_last.y - cur.y) / h / g_app->preview_zoom;
            g_app->preview_drag_last = cur;
            gw2gfx::set_view(g_app->preview_zoom, g_app->preview_pan_x, g_app->preview_pan_y);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app != nullptr && g_app->preview_dragging) {
            g_app->preview_dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        fit_view();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK ModelWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        gw2m3d::render();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL: {
        if (g_app == nullptr) {
            break;
        }
        int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        gw2m3d::zoom(std::pow(0.9f, static_cast<float>(notches))); // wheel up -> closer
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        if (g_app != nullptr) {
            g_app->model_dragging = true;
            g_app->model_drag_last = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_app != nullptr && g_app->model_dragging) {
            POINT cur{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            // Negate both deltas so the model follows the cursor (drag right ->
            // spins right, drag down -> tilts down) instead of the inverted default.
            gw2m3d::orbit(static_cast<float>(g_app->model_drag_last.x - cur.x) * 0.01f,
                          static_cast<float>(g_app->model_drag_last.y - cur.y) * 0.01f);
            g_app->model_drag_last = cur;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_app != nullptr && g_app->model_dragging) {
            g_app->model_dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        reset_model_view();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        g_app = new AppState();
        g_app->hwnd_main = hwnd;

        g_app->hwnd_search_edit =
            CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_EDIT)), g_hinstance, nullptr);
        g_app->hwnd_search_fileid_check = CreateWindowExW(
            0, L"BUTTON", L"By File ID", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_FILEID_CHECK)), g_hinstance, nullptr);
        g_app->hwnd_search_button =
            CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_SEARCH_BUTTON), g_hinstance, nullptr);
        g_app->hwnd_clear_button =
            CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_CLEAR_BUTTON), g_hinstance, nullptr);

        g_app->hwnd_list = gw2mft::create(hwnd, g_hinstance, ID_LISTVIEW);
        gw2mft::set_selection_callback(g_app->hwnd_list, on_entry_selected);

        g_app->hwnd_split_list_middle =
            gw2ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_LIST_MIDDLE, gw2ui::SplitOrientation::Vertical);
        gw2ui::set_drag_callback(g_app->hwnd_split_list_middle, [](int delta) {
            RECT rc;
            GetClientRect(g_app->hwnd_main, &rc);
            int client_w = rc.right - rc.left;
            if (client_w <= 0) {
                return;
            }
            g_app->list_width_ratio =
                std::clamp(g_app->list_width_ratio + static_cast<double>(delta) / client_w, 0.08, 0.6);
            relayout();
        });

        g_app->hwnd_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TAB)), g_hinstance, nullptr);
        {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(L"Compressed");
            TabCtrl_InsertItem(g_app->hwnd_tab, 0, &item);
            item.pszText = const_cast<wchar_t*>(L"Decompressed");
            TabCtrl_InsertItem(g_app->hwnd_tab, 1, &item);
            item.pszText = const_cast<wchar_t*>(L"Preview");
            TabCtrl_InsertItem(g_app->hwnd_tab, 2, &item);
            TabCtrl_SetCurSel(g_app->hwnd_tab, static_cast<int>(MiddleTab::Preview));
        }

        g_app->hwnd_preview = CreateWindowExW(WS_EX_CLIENTEDGE, kPreviewClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                               0, hwnd, nullptr, g_hinstance, nullptr);
        g_app->hwnd_zoom_in =
            CreateWindowExW(0, L"BUTTON", L"Zoom In", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ZOOM_IN), g_hinstance, nullptr);
        g_app->hwnd_zoom_out =
            CreateWindowExW(0, L"BUTTON", L"Zoom Out", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ZOOM_OUT), g_hinstance, nullptr);
        g_app->hwnd_rotate =
            CreateWindowExW(0, L"BUTTON", L"Rotate", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ROTATE), g_hinstance, nullptr);
        g_app->hwnd_fit = CreateWindowExW(0, L"BUTTON", L"Fit", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(ID_FIT), g_hinstance, nullptr);

        // Model preview surface (its own D3D device/swapchain) + mode buttons.
        g_app->hwnd_model = CreateWindowExW(WS_EX_CLIENTEDGE, kModelClassName, L"", WS_CHILD, 0, 0, 0, 0, hwnd,
                                             nullptr, g_hinstance, nullptr);
        g_app->hwnd_mode_full =
            CreateWindowExW(0, L"BUTTON", L"Full", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_FULL), g_hinstance, nullptr);
        g_app->hwnd_mode_plain =
            CreateWindowExW(0, L"BUTTON", L"Plain", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_PLAIN), g_hinstance, nullptr);
        g_app->hwnd_mode_wire =
            CreateWindowExW(0, L"BUTTON", L"Wireframe", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_WIRE), g_hinstance, nullptr);
        // Real game shaders (bgfx DXBC from each material's AMAT package).
        g_app->hwnd_mode_shader =
            CreateWindowExW(0, L"BUTTON", L"Shader", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_SHADER), g_hinstance, nullptr);
        g_app->hwnd_model_reset =
            CreateWindowExW(0, L"BUTTON", L"Reset", WS_CHILD, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODEL_RESET), g_hinstance, nullptr);
        // Skeleton on/off: a push-like checkbox so the button stays lit while on.
        g_app->hwnd_skel_toggle =
            CreateWindowExW(0, L"BUTTON", L"Skeleton", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_MODE_SKEL), g_hinstance, nullptr);
        // Animation clip selector + play/pause (populated per model).
        g_app->hwnd_anim_combo =
            CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ANIM_COMBO), g_hinstance, nullptr);
        g_app->hwnd_anim_play =
            CreateWindowExW(0, L"BUTTON", L"Play", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_ANIM_PLAY), g_hinstance, nullptr);
        // Texture resolution: full-res (default, checked) vs the reduced half-size
        // sibling. Toggling reloads the current model with the chosen resolution.
        g_app->hwnd_tex_fullres =
            CreateWindowExW(0, L"BUTTON", L"Full-res tex", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_TEX_FULLRES), g_hinstance, nullptr);
        SendMessageW(g_app->hwnd_tex_fullres, BM_SETCHECK, BST_CHECKED, 0);
        // Map layer toggles (shown only for map/mapc entries).
        g_app->hwnd_layer_prop =
            CreateWindowExW(0, L"BUTTON", L"Props", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LAYER_PROP), g_hinstance, nullptr);
        g_app->hwnd_layer_zone =
            CreateWindowExW(0, L"BUTTON", L"Zones", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LAYER_ZONE), g_hinstance, nullptr);
        g_app->hwnd_layer_coll =
            CreateWindowExW(0, L"BUTTON", L"Collision", WS_CHILD | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(ID_LAYER_COLL), g_hinstance, nullptr);

        // Text / strs preview (read-only, both scrollbars for code/markup).
        g_app->hwnd_text_preview = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL, 0, 0,
            0, 0, hwnd, nullptr, g_hinstance, nullptr);
        {
            static HFONT text_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                                  FIXED_PITCH | FF_MODERN, L"Consolas");
            SendMessageW(g_app->hwnd_text_preview, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
            SendMessageW(g_app->hwnd_text_preview, EM_SETLIMITTEXT, 0, 0);
        }

        g_app->hwnd_hex_before = gw2hex::create(hwnd, g_hinstance, ID_HEX_BEFORE, 0, 0, 0, 0);
        g_app->hwnd_hex_after = gw2hex::create(hwnd, g_hinstance, ID_HEX_AFTER, 0, 0, 0, 0);

        g_app->hwnd_split_middle_info =
            gw2ui::create_splitter(hwnd, g_hinstance, ID_SPLIT_MIDDLE_INFO, gw2ui::SplitOrientation::Vertical);
        gw2ui::set_drag_callback(g_app->hwnd_split_middle_info, [](int delta) {
            RECT rc;
            GetClientRect(g_app->hwnd_main, &rc);
            int client_w = rc.right - rc.left;
            if (client_w <= 0) {
                return;
            }
            g_app->info_width_ratio =
                std::clamp(g_app->info_width_ratio - static_cast<double>(delta) / client_w, 0.08, 0.6);
            relayout();
        });

        g_app->hwnd_info = gw2info::create(hwnd, g_hinstance, ID_INFO_PANEL);

        g_app->hwnd_status_label =
            CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS_LABEL)), g_hinstance, nullptr);
        g_app->hwnd_progress =
            CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_MARQUEE, 0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROGRESS)), g_hinstance, nullptr);

        if (!gw2gfx::initialize(g_app->hwnd_preview)) {
            MessageBoxW(hwnd, L"Failed to initialize Direct3D 11. Image preview will be unavailable.",
                        L"gw2browser", MB_ICONWARNING);
        }
        if (!gw2m3d::initialize(g_app->hwnd_model)) {
            MessageBoxW(hwnd, L"Failed to initialize Direct3D 11 for the model viewer. 3D preview will be unavailable.",
                        L"gw2browser", MB_ICONWARNING);
        }
        // Best-effort: find gw2_packfile.json next to the exe / templates dir so
        // model preview works out of the box (File -> Load Struct JSON overrides).
        gw2tpl::auto_load();
        return 0;
    }
    case WM_SIZE:
        if (g_app != nullptr) {
            layout_children(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lparam);
        if (g_app == nullptr) {
            return 0;
        }
        if (header->hwndFrom == g_app->hwnd_list) {
            return gw2mft::handle_notify(g_app->hwnd_list, header);
        }
        if (header->hwndFrom == g_app->hwnd_tab && header->code == TCN_SELCHANGE) {
            relayout();
        }
        return 0;
    }
    case WM_APP_EXTRACT_DONE: {
        std::unique_ptr<ExtractResult> result(reinterpret_cast<ExtractResult*>(lparam));
        if (g_app == nullptr || result->generation != g_app->request_generation) {
            return 0; // superseded by a newer selection (or a newly opened archive) -- discard
        }
        show_loading(false, 0);
        if (result->success) {
            apply_extracted_entry(result->mft_index, std::move(result->entry));
        } else {
            g_app->current_entry = ExtractedEntry{};
            g_app->has_loaded_entry = false;
            set_export_enabled(false);
            MessageBoxA(hwnd, result->error_message.c_str(), "Extraction failed", MB_ICONWARNING);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_FILE_OPEN:
            do_open_file(hwnd);
            return 0;
        case ID_FILE_LOAD_TEMPLATE:
            do_load_template(hwnd);
            return 0;
        case ID_FILE_EXPORT_COMPRESSED:
            do_export(hwnd, true);
            return 0;
        case ID_FILE_EXPORT_DECOMPRESSED:
            do_export(hwnd, false);
            return 0;
        case ID_FILE_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case ID_SEARCH_BUTTON:
            do_search();
            return 0;
        case ID_CLEAR_BUTTON:
            do_clear_search();
            return 0;
        case ID_ZOOM_IN:
            zoom_by(1.25f);
            return 0;
        case ID_ZOOM_OUT:
            zoom_by(1.0f / 1.25f);
            return 0;
        case ID_ROTATE:
            rotate_90();
            return 0;
        case ID_FIT:
            fit_view();
            return 0;
        case ID_MODE_FULL:
            set_model_mode(gw2m3d::RenderMode::Full);
            return 0;
        case ID_MODE_PLAIN:
            set_model_mode(gw2m3d::RenderMode::Plain);
            return 0;
        case ID_MODE_WIRE:
            set_model_mode(gw2m3d::RenderMode::Wireframe);
            break;
        case ID_MODE_SHADER:
            set_model_mode(gw2m3d::RenderMode::GameShader);
            return 0;
        case ID_MODEL_RESET:
            reset_model_view();
            return 0;
        case ID_LAYER_PROP:
            gw2m3d::set_layer_visible(gw2m3d::LAYER_PROP,
                                      SendMessageW(g_app->hwnd_layer_prop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_LAYER_COLL:
            gw2m3d::set_layer_visible(gw2m3d::LAYER_COLLISION,
                                      SendMessageW(g_app->hwnd_layer_coll, BM_GETCHECK, 0, 0) == BST_CHECKED);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_LAYER_ZONE: {
            bool on = SendMessageW(g_app->hwnd_layer_zone, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // Lazily load the zone models the first time zones are enabled (keeps
            // the initial map load prop-only and fast).
            if (on && !g_app->map_zone_loaded && g_app->current_entry.map) {
                HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                auto zl = build_map_zone_layer(g_app->current_entry.decompressed,
                                               g_app->data_gw2.file_info.file_path);
                if (zl && !zl->instances.empty()) {
                    std::vector<gw2m3d::SceneInstance> insts;
                    for (const MapInstance& mi : zl->instances) {
                        gw2m3d::SceneInstance si;
                        si.model = mi.model;
                        for (int k = 0; k < 3; ++k) { si.pos[k] = mi.pos[k]; si.rot[k] = mi.rot[k]; }
                        si.scale = mi.scale; si.layer = mi.layer;
                        insts.push_back(si);
                    }
                    gw2m3d::add_scene_models(zl->models, insts);
                }
                g_app->map_zone_loaded = true;
                SetCursor(old);
            }
            gw2m3d::set_layer_visible(gw2m3d::LAYER_ZONE, on);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_MODE_SKEL:
            g_app->show_skeleton =
                SendMessageW(g_app->hwnd_skel_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            gw2m3d::set_show_skeleton(g_app->show_skeleton);
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        case ID_ANIM_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(g_app->hwnd_anim_combo, CB_GETCURSEL, 0, 0));
                gw2m3d::set_animation(sel - 1); // item 0 = bind pose (-1), 1..nclips = clips
                // Selecting the rig implies the user wants to see it.
                if (gw2m3d::has_skeleton() && !g_app->show_skeleton) {
                    g_app->show_skeleton = true;
                    SendMessageW(g_app->hwnd_skel_toggle, BM_SETCHECK, BST_CHECKED, 0);
                    gw2m3d::set_show_skeleton(true);
                }
                InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            }
            return 0;
        case ID_ANIM_PLAY: {
            bool playing = SendMessageW(g_app->hwnd_anim_play, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // Play from the bind pose auto-selects the first embedded clip with real
            // keyframed motion (character/boss/weapon models); mounts/props embed only a
            // static zeropose, so there's nothing to auto-play there.
            if (playing && gw2m3d::current_animation() == -1) {
                int motion = gw2m3d::first_motion_animation();
                if (motion >= 0) {
                    SendMessageW(g_app->hwnd_anim_combo, CB_SETCURSEL, motion + 1, 0); // item0 = bind
                    gw2m3d::set_animation(motion);
                }
            }
            gw2m3d::set_playing(playing);
            if (playing) {
                SetTimer(g_app->hwnd_main, TIMER_ANIM, 16, nullptr); // ~60 fps
            } else {
                KillTimer(g_app->hwnd_main, TIMER_ANIM);
            }
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
            return 0;
        }
        case ID_TEX_FULLRES: {
            bool full = SendMessageW(g_app->hwnd_tex_fullres, BM_GETCHECK, 0, 0) == BST_CHECKED;
            set_texture_full_res(full);
            // Reload the current model so materials re-decode at the chosen resolution.
            if (g_app->dat_loaded && g_app->has_loaded_entry &&
                g_app->current_entry.kind == PreviewKind::Model) {
                on_entry_selected(g_app->current_mft_index);
            }
            return 0;
        }
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (wparam == TIMER_ANIM && gw2m3d::is_playing()) {
            InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ANIM);
        gw2gfx::shutdown();
        gw2m3d::shutdown();
        delete g_app;
        g_app = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int cmd_show) {
    g_hinstance = hInstance;

    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    gw2hex::register_class(hInstance);
    gw2ui::register_splitter_class(hInstance);

    WNDCLASSEXW preview_class{};
    preview_class.cbSize = sizeof(preview_class);
    preview_class.style = CS_DBLCLKS;
    preview_class.lpfnWndProc = PreviewWndProc;
    preview_class.hInstance = hInstance;
    preview_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    preview_class.hbrBackground = nullptr;
    preview_class.lpszClassName = kPreviewClassName;
    RegisterClassExW(&preview_class);

    WNDCLASSEXW model_class{};
    model_class.cbSize = sizeof(model_class);
    model_class.style = CS_DBLCLKS;
    model_class.lpfnWndProc = ModelWndProc;
    model_class.hInstance = hInstance;
    model_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    model_class.hbrBackground = nullptr;
    model_class.lpszClassName = kModelClassName;
    RegisterClassExW(&model_class);

    WNDCLASSEXW main_class{};
    main_class.cbSize = sizeof(main_class);
    main_class.style = CS_HREDRAW | CS_VREDRAW;
    main_class.lpfnWndProc = MainWndProc;
    main_class.hInstance = hInstance;
    main_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    main_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    main_class.lpszClassName = kMainClassName;
    RegisterClassExW(&main_class);

    HWND hwnd = CreateWindowExW(0, kMainClassName, L"gw2browser", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1500, 950, nullptr, build_menu(), hInstance, nullptr);
    if (hwnd == nullptr) {
        return 0;
    }

    ACCEL accel_entries[] = {{FVIRTKEY, VK_RETURN, static_cast<WORD>(ID_SEARCH_BUTTON)}};
    HACCEL accel_table = CreateAcceleratorTable(accel_entries, 1);

    ShowWindow(hwnd, cmd_show);
    UpdateWindow(hwnd);

    // Debug: GW2_SCENE=<mftIndex> builds a synthetic coordinated scene (a grid of
    // that model at varied positions/rotations) to verify the map scene renderer.
    if (const char* sc = std::getenv("GW2_SCENE")) {
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            g_app->dat_loaded = true;
            uint32_t idx = static_cast<uint32_t>(atoi(sc));
            ExtractedEntry e = extract_entry(g_app->data_gw2, idx);
            if (e.model) {
                std::vector<ModelPreview> models{*e.model};
                std::vector<gw2m3d::SceneInstance> insts;
                float step = e.model->radius * 2.6f;
                for (int gz = 0; gz < 3; ++gz)
                    for (int gx = 0; gx < 3; ++gx) {
                        gw2m3d::SceneInstance in;
                        in.model = 0;
                        in.pos[0] = (gx - 1) * step; in.pos[1] = 0; in.pos[2] = (gz - 1) * step;
                        in.rot[1] = (gx + gz) * 0.6f; // vary yaw per cell
                        in.scale = 1.0f;
                        insts.push_back(in);
                    }
                g_app->current_entry = std::move(e);
                gw2m3d::on_resize(1000, 800); // size the offscreen swapchain for the headless shot
                gw2m3d::set_scene(models, insts);
                gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_scene.bmp");
            }
        } catch (const std::exception& ex) { MessageBoxA(hwnd, ex.what(), "scene failed", MB_OK); }
    }

    // Debug: GW2_AUTOLOAD=<mftIndex> loads the Steam dat and applies that entry
    // synchronously (drives the real model pipeline for headless skinning checks).
    if (const char* al = std::getenv("GW2_AUTOLOAD")) {
        try {
            load_dat_file(g_app->data_gw2,
                          "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Guild Wars 2\\Gw2.dat");
            g_app->dat_loaded = true;
            uint32_t idx = static_cast<uint32_t>(atoi(al));
            ExtractedEntry e = extract_entry(g_app->data_gw2, idx);
            bool isMap = (e.kind == PreviewKind::Map);
            apply_extracted_entry(idx, std::move(e));
            gw2m3d::on_resize(1000, 800);
            if (isMap) {
                gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_map.bmp");
                gw2m3d::set_layer_visible(gw2m3d::LAYER_COLLISION, true);
                gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_map_coll.bmp");
                gw2m3d::set_layer_visible(gw2m3d::LAYER_COLLISION, false);
                auto zl = build_map_zone_layer(g_app->current_entry.decompressed, g_app->data_gw2.file_info.file_path);
                if (zl && !zl->instances.empty()) {
                    std::vector<gw2m3d::SceneInstance> zi;
                    for (const MapInstance& mi : zl->instances) {
                        gw2m3d::SceneInstance si; si.model = mi.model;
                        for (int k = 0; k < 3; ++k) { si.pos[k] = mi.pos[k]; si.rot[k] = mi.rot[k]; }
                        si.scale = mi.scale; si.layer = mi.layer; zi.push_back(si);
                    }
                    gw2m3d::add_scene_models(zl->models, zi);
                }
                gw2m3d::set_layer_visible(gw2m3d::LAYER_ZONE, true);
                gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_map_zone.bmp");
            } else {
                gw2m3d::set_show_skeleton(false); // isolate the MESH
                gw2m3d::set_animation(-1);
                gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_bind.bmp");
                // Real game (bgfx DXBC) shaders: bind pose first.
                gw2m3d::set_mode(gw2m3d::RenderMode::GameShader);
                gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_shader.bmp");
                // Accurate Granny animation + original DXBC shaders together: play the
                // first real motion clip and capture two distinct frames in Shader mode.
                {
                    int motion = gw2m3d::first_motion_animation();
                    if (motion >= 0) {
                        float du = gw2m3d::animation_duration(motion);
                        gw2m3d::set_animation(motion);
                        gw2m3d::set_anim_time(du * 0.30f);
                        gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_shader_anim1.bmp");
                        gw2m3d::set_anim_time(du * 0.70f);
                        gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_shader_anim2.bmp");
                    }
                }
                gw2m3d::set_animation(-1);
                gw2m3d::set_mode(gw2m3d::RenderMode::Full);
                // Pose the longest real embedded clip (to check whether the sword
                // assembles into the hand vs the separated zeropose).
                {
                    int best = -1; float bestDur = 0.05f;
                    for (int ci = 0; ci < gw2m3d::animation_count(); ++ci) {
                        float du = gw2m3d::animation_duration(ci);
                        if (du > bestDur) { bestDur = du; best = ci; }
                    }
                    if (best >= 0) {
                        gw2m3d::set_animation(best);
                        gw2m3d::set_anim_time(bestDur * 0.5f);
                        gw2m3d::save_screenshot("C:/Users/RIDWAN~1/AppData/Local/Temp/claude/shot_clip.bmp");
                    }
                    gw2m3d::set_animation(-1);
                }
            }
        } catch (const std::exception& ex) {
            MessageBoxA(hwnd, ex.what(), "autoload failed", MB_OK);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (accel_table == nullptr || !TranslateAcceleratorW(hwnd, accel_table, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
