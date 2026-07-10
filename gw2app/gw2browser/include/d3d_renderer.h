#ifndef GW2_D3D_RENDERER_H
#define GW2_D3D_RENDERER_H

#include <cstdint>
#include <windows.h>

namespace gw2gfx {

// Creates the D3D11 device/swapchain bound to this child window's client
// area. Returns false if D3D11 initialization failed.
bool initialize(HWND target_window);

void shutdown();

// Call from the host window's WM_SIZE (or equivalent) handler.
void on_resize(int width, int height);

// Uploads a single-mip BCn (BC1-BC7) compressed texture; it will be drawn
// letterboxed (aspect-ratio preserved) on the next render(). width/height are
// the texture's true dimensions (need not be a multiple of 4 -- D3D11 pads
// the block grid internally for the base mip level); sys_mem_pitch is the
// block-row byte pitch, e.g. from gw2dds::DdsInfo::sys_mem_pitch.
void set_texture(uint32_t dxgi_format, uint32_t width, uint32_t height, uint32_t sys_mem_pitch, const uint8_t* data);

// Drops the current texture; render() will just clear to the background color.
void clear_texture();

// Adjusts the sampled UV rectangle: uv' = 0.5 + (uv - 0.5) / zoom + pan.
// zoom > 1 magnifies; pan is in UV units (same units regardless of zoom), so
// panning feels consistent to the mouse whether zoomed in or out.
void set_view(float zoom, float pan_x, float pan_y);

// Back to zoom=1, pan=(0,0) -- i.e. today's default letterboxed fit. Leaves
// rotation untouched (rotation is an independent, explicit user choice).
void reset_view();

// Rotates the displayed image by 90 degrees * quarter_turns (mod 4,
// clockwise). The letterbox aspect ratio is recomputed for 90/270 so a
// non-square image still fits its pane correctly when rotated on its side.
void set_rotation(int quarter_turns);

void render();

} // namespace gw2gfx

#endif // GW2_D3D_RENDERER_H
