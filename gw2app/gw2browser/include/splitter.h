#ifndef GW2_SPLITTER_H
#define GW2_SPLITTER_H

#include <functional>
#include <windows.h>

namespace gw2ui {

enum class SplitOrientation {
    Vertical,  // a vertical divider bar the user drags left/right
    Horizontal // a horizontal divider bar the user drags up/down
};

using SplitterDragCallback = std::function<void(int delta_pixels)>;

// Registers the "Gw2Splitter" window class. Call once before creating any.
void register_splitter_class(HINSTANCE instance);

// Creates a thin draggable divider bar (~5px). It draws itself and sets the
// resize cursor on hover; it does not resize anything on its own -- wire
// set_drag_callback() to adjust your own layout and re-run it.
HWND create_splitter(HWND parent, HINSTANCE instance, int control_id, SplitOrientation orientation);

// Called with the pixel delta (positive = right/down) while the user drags.
void set_drag_callback(HWND splitter, SplitterDragCallback callback);

} // namespace gw2ui

#endif // GW2_SPLITTER_H
