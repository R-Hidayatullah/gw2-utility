#ifndef GW2_MFT_LISTVIEW_H
#define GW2_MFT_LISTVIEW_H

#include <cstdint>
#include <functional>
#include <vector>
#include <windows.h>

#include "gw2dat.h"

namespace gw2mft {

using SelectionCallback = std::function<void(uint32_t mft_index)>;

// Creates an LVS_OWNERDATA report-view ListView. Because the control never
// owns per-row data itself (only ever asked for the ~40 rows currently on
// screen via LVN_GETDISPINFO), this scales to archives with hundreds of
// thousands of MFT entries with no extra memory or up-front formatting cost.
HWND create(HWND parent, HINSTANCE instance, int control_id);

// Binds the list to every asset (one row per distinct base_id) in data_gw2.
// Must be called after the .dat file has finished loading.
void set_source(HWND listview, Gw2Dat& data_gw2);

// Restricts the displayed rows to this set of base_ids (e.g. search results).
// Pass an empty vector to go back to showing every asset.
void set_filter(HWND listview, std::vector<uint32_t> base_ids);

void set_selection_callback(HWND listview, SelectionCallback callback);

// Optional provider for the Type/Container columns (filled from a loaded index
// DB). Return false to leave the cells blank. Pass an empty function to clear.
using MetadataProvider = std::function<bool(uint32_t base_id, std::wstring& type, std::wstring& container)>;
void set_metadata_provider(HWND listview, MetadataProvider provider);

// Forward WM_NOTIFY messages here from the parent window when
// notify->hwndFrom is this listview's HWND.
LRESULT handle_notify(HWND listview, NMHDR* notify);

} // namespace gw2mft

#endif // GW2_MFT_LISTVIEW_H
