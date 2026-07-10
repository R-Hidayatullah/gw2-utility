#ifndef GW2_INFO_PANEL_H
#define GW2_INFO_PANEL_H

#include <cstdint>
#include <windows.h>

#include "entry_extractor.h"
#include "gw2dat.h"

namespace gw2info {

// Creates a read-only, scrollable details panel (a plain multi-line Edit
// control -- gives free scrolling/selection/copy with no custom drawing).
HWND create(HWND parent, HINSTANCE instance, int control_id);

// Shows the archive-level header info. Call once right after a successful load_dat_file().
void show_dat_info(HWND panel, const Gw2Dat& data_gw2);

// Appends the selected MFT entry's fields (and, if it's an image, its decoded
// dimensions/format) below the archive info set by show_dat_info().
void show_entry_info(HWND panel, const Gw2Dat& data_gw2, uint32_t mft_index, const ExtractedEntry& entry);

} // namespace gw2info

#endif // GW2_INFO_PANEL_H
