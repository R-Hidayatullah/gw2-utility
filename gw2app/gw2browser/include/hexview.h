#ifndef GW2_HEXVIEW_H
#define GW2_HEXVIEW_H

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace gw2hex {

// Registers the "Gw2HexView" window class. Call once before creating any
// instances (e.g. from WinMain).
void register_class(HINSTANCE instance);

// Creates a hex-view child control. Behaves like any other Win32 child: move
// it around with MoveWindow/SetWindowPos as the parent resizes.
HWND create(HWND parent, HINSTANCE instance, int control_id, int x, int y, int width, int height);

// Points the control at `data`/`size` (not copied -- caller must keep the
// buffer alive until the next SetData call or window destruction) and resets
// the scroll position. Only the rows currently visible on screen are ever
// formatted/drawn, so this stays fast regardless of `size`.
void set_data(HWND hwnd, const uint8_t* data, size_t size);

} // namespace gw2hex

#endif // GW2_HEXVIEW_H
