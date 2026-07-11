#include "mft_listview.h"

#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <string>
#include <unordered_map>

namespace gw2mft {

namespace {

constexpr int kColumnCount = 9;

struct MftListState {
    Gw2Dat* data_gw2 = nullptr;
    std::unordered_map<uint32_t, size_t> base_id_to_index;
    std::vector<size_t> display_order; // indices into mft_base_id_data_list, in the order shown
    int sort_column = -1;              // -1 == natural (archive) order
    bool sort_ascending = true;
    SelectionCallback on_select;
    MetadataProvider meta_provider;    // Type/Container columns (from an index DB)
};

MftListState* get_state(HWND listview) {
    return reinterpret_cast<MftListState*>(GetWindowLongPtrW(listview, GWLP_USERDATA));
}

size_t row_count(const MftListState& state) { return state.display_order.size(); }

size_t row_to_base_index(const MftListState& state, int row) {
    return state.display_order[static_cast<size_t>(row)];
}

// Numeric value backing column `column`'s sort order, for base_index into mft_base_id_data_list.
uint64_t sort_key(const MftListState& state, size_t base_index, int column) {
    const MftBaseIdData& base_entry = state.data_gw2->mft_base_id_data_list[base_index];
    const MftData* mft = nullptr;
    if (base_entry.base_id >= 1 && (base_entry.base_id - 1) < state.data_gw2->mft_data_list.size()) {
        mft = &state.data_gw2->mft_data_list[base_entry.base_id - 1];
    }

    switch (column) {
    case 0: return static_cast<uint64_t>(base_index);
    case 1: return base_entry.base_id;
    case 2: return base_entry.file_id.empty() ? 0 : base_entry.file_id[0];
    case 3: return mft ? mft->offset : 0;
    case 4: return mft ? mft->size : 0;
    case 5: return mft ? mft->uncompressed_size : 0;
    case 6: return mft ? mft->compression_flag : 0;
    default: return 0;
    }
}

void apply_sort(MftListState& state) {
    if (state.sort_column < 0 || state.data_gw2 == nullptr) {
        return;
    }
    int column = state.sort_column;
    bool ascending = state.sort_ascending;
    std::sort(state.display_order.begin(), state.display_order.end(), [&](size_t a, size_t b) {
        uint64_t ka = sort_key(state, a, column);
        uint64_t kb = sort_key(state, b, column);
        return ascending ? (ka < kb) : (ka > kb);
    });
}

void update_sort_arrow(HWND listview, const MftListState& state) {
    HWND header = ListView_GetHeader(listview);
    if (header == nullptr) {
        return;
    }
    for (int i = 0; i < kColumnCount; i++) {
        HDITEMW hdi{};
        hdi.mask = HDI_FORMAT;
        if (!Header_GetItem(header, i, &hdi)) {
            continue;
        }
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == state.sort_column) {
            hdi.fmt |= state.sort_ascending ? HDF_SORTUP : HDF_SORTDOWN;
        }
        Header_SetItem(header, i, &hdi);
    }
}

void add_column(HWND listview, int index, const wchar_t* text, int width) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<wchar_t*>(text);
    col.cx = width;
    col.iSubItem = index;
    ListView_InsertColumn(listview, index, &col);
}

void fill_disp_info(MftListState& state, NMLVDISPINFOW& info) {
    if (!(info.item.mask & LVIF_TEXT) || state.data_gw2 == nullptr) {
        return;
    }

    size_t base_index = row_to_base_index(state, info.item.iItem);
    if (base_index >= state.data_gw2->mft_base_id_data_list.size()) {
        return;
    }

    const MftBaseIdData& base_entry = state.data_gw2->mft_base_id_data_list[base_index];
    const MftData* mft = nullptr;
    if (base_entry.base_id >= 1 && (base_entry.base_id - 1) < state.data_gw2->mft_data_list.size()) {
        mft = &state.data_gw2->mft_data_list[base_entry.base_id - 1];
    }

    switch (info.item.iSubItem) {
    case 0:
        swprintf(info.item.pszText, info.item.cchTextMax, L"%d", info.item.iItem);
        break;
    case 1:
        swprintf(info.item.pszText, info.item.cchTextMax, L"%u", base_entry.base_id);
        break;
    case 2: {
        std::wstring joined;
        const size_t shown = std::min<size_t>(base_entry.file_id.size(), 8);
        for (size_t i = 0; i < shown; i++) {
            if (i != 0) joined += L",";
            joined += std::to_wstring(base_entry.file_id[i]);
        }
        if (base_entry.file_id.size() > shown) joined += L",...";
        wcsncpy(info.item.pszText, joined.c_str(), static_cast<size_t>(info.item.cchTextMax) - 1);
        info.item.pszText[info.item.cchTextMax - 1] = L'\0';
        break;
    }
    case 3:
        swprintf(info.item.pszText, info.item.cchTextMax, L"0x%llX", mft ? static_cast<unsigned long long>(mft->offset) : 0ULL);
        break;
    case 4:
        swprintf(info.item.pszText, info.item.cchTextMax, L"%u", mft ? mft->size : 0);
        break;
    case 5:
        swprintf(info.item.pszText, info.item.cchTextMax, L"%u", mft ? mft->uncompressed_size : 0);
        break;
    case 6:
        swprintf(info.item.pszText, info.item.cchTextMax, L"%u", mft ? mft->compression_flag : 0);
        break;
    case 7:
    case 8: {
        if (!state.meta_provider) break;
        std::wstring type, container;
        if (state.meta_provider(base_entry.base_id, type, container)) {
            const std::wstring& v = (info.item.iSubItem == 7) ? type : container;
            wcsncpy(info.item.pszText, v.c_str(), static_cast<size_t>(info.item.cchTextMax) - 1);
            info.item.pszText[info.item.cchTextMax - 1] = L'\0';
        }
        break;
    }
    default:
        break;
    }
}

} // namespace

HWND create(HWND parent, HINSTANCE instance, int control_id) {
    HWND listview =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL, 0, 0, 0, 0,
                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance, nullptr);
    if (listview == nullptr) {
        return nullptr;
    }

    ListView_SetExtendedListViewStyle(listview, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    add_column(listview, 0, L"#", 50);
    add_column(listview, 1, L"Base ID", 80);
    add_column(listview, 2, L"File ID(s)", 170);
    add_column(listview, 3, L"Offset", 110);
    add_column(listview, 4, L"Size", 90);
    add_column(listview, 5, L"Uncompressed", 100);
    add_column(listview, 6, L"Comp", 50);
    add_column(listview, 7, L"Type", 70);
    add_column(listview, 8, L"Container", 80);

    SetWindowLongPtrW(listview, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new MftListState()));
    return listview;
}

void set_source(HWND listview, Gw2Dat& data_gw2) {
    MftListState* state = get_state(listview);
    if (state == nullptr) {
        return;
    }

    state->data_gw2 = &data_gw2;
    state->sort_column = -1;
    state->sort_ascending = true;

    state->base_id_to_index.clear();
    state->base_id_to_index.reserve(data_gw2.mft_base_id_data_list.size());
    state->display_order.resize(data_gw2.mft_base_id_data_list.size());
    for (size_t i = 0; i < data_gw2.mft_base_id_data_list.size(); i++) {
        state->base_id_to_index[data_gw2.mft_base_id_data_list[i].base_id] = i;
        state->display_order[i] = i;
    }

    update_sort_arrow(listview, *state);
    ListView_SetItemCountEx(listview, static_cast<int>(data_gw2.mft_base_id_data_list.size()), LVSICF_NOSCROLL);
}

void set_filter(HWND listview, std::vector<uint32_t> base_ids) {
    MftListState* state = get_state(listview);
    if (state == nullptr || state->data_gw2 == nullptr) {
        return;
    }

    if (base_ids.empty()) {
        state->display_order.resize(state->data_gw2->mft_base_id_data_list.size());
        for (size_t i = 0; i < state->display_order.size(); i++) {
            state->display_order[i] = i;
        }
    } else {
        state->display_order.clear();
        state->display_order.reserve(base_ids.size());
        for (uint32_t id : base_ids) {
            auto it = state->base_id_to_index.find(id);
            if (it != state->base_id_to_index.end()) {
                state->display_order.push_back(it->second);
            }
        }
    }

    apply_sort(*state);
    ListView_SetItemCountEx(listview, static_cast<int>(row_count(*state)), LVSICF_NOSCROLL);
    InvalidateRect(listview, nullptr, TRUE);
}

void set_selection_callback(HWND listview, SelectionCallback callback) {
    MftListState* state = get_state(listview);
    if (state != nullptr) {
        state->on_select = std::move(callback);
    }
}

void set_metadata_provider(HWND listview, MetadataProvider provider) {
    MftListState* state = get_state(listview);
    if (state != nullptr) {
        state->meta_provider = std::move(provider);
        InvalidateRect(listview, nullptr, TRUE);
    }
}

// Resolves a display row to its base_id and invokes the selection callback.
// on_entry_selected() dedupes an in-flight request for the same row, so calling
// this from both LVN_ITEMCHANGED and NM_CLICK/NM_DBLCLK is safe.
void fire_select_for_row(MftListState& state, int row) {
    if (row < 0 || static_cast<size_t>(row) >= row_count(state)) {
        return;
    }
    size_t base_index = row_to_base_index(state, row);
    if (base_index >= state.data_gw2->mft_base_id_data_list.size()) {
        return;
    }
    uint32_t base_id = state.data_gw2->mft_base_id_data_list[base_index].base_id;
    if (base_id >= 1 && state.on_select) {
        state.on_select(base_id - 1);
    }
}

LRESULT handle_notify(HWND listview, NMHDR* notify) {
    MftListState* state = get_state(listview);
    if (state == nullptr || state->data_gw2 == nullptr) {
        return 0;
    }

    if (notify->code == LVN_GETDISPINFOW) {
        fill_disp_info(*state, *reinterpret_cast<NMLVDISPINFOW*>(notify));
        return 0;
    }

    if (notify->code == LVN_ITEMCHANGED) {
        auto* change = reinterpret_cast<NMLISTVIEW*>(notify);
        bool became_selected = (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED);
        if (became_selected) {
            fire_select_for_row(*state, change->iItem);
        }
        return 0;
    }

    // A click (or double-click) on an *already selected* row produces no
    // LVN_ITEMCHANGED, so the view would otherwise never refresh to it. Handle
    // the mouse notifications too; on_entry_selected() drops the duplicate when
    // the same row's extraction is still in flight.
    if (notify->code == NM_CLICK || notify->code == NM_DBLCLK) {
        auto* activate = reinterpret_cast<NMITEMACTIVATE*>(notify);
        fire_select_for_row(*state, activate->iItem);
        return 0;
    }

    if (notify->code == LVN_COLUMNCLICK) {
        auto* click = reinterpret_cast<NMLISTVIEW*>(notify);
        if (click->iSubItem == state->sort_column) {
            state->sort_ascending = !state->sort_ascending;
        } else {
            state->sort_column = click->iSubItem;
            state->sort_ascending = true;
        }

        HCURSOR old_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        apply_sort(*state);
        SetCursor(old_cursor);

        update_sort_arrow(listview, *state);
        InvalidateRect(listview, nullptr, TRUE);
        return 0;
    }

    return 0;
}

} // namespace gw2mft
