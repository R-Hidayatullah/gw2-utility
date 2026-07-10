#ifndef GW2_MODEL_RENDERER_H
#define GW2_MODEL_RENDERER_H

#include <windows.h>

#include "entry_extractor.h" // ModelPreview

// Direct3D 11 orbit-camera viewer for GW2 .modl models. Its own device +
// swapchain + depth buffer, bound to a dedicated child window (a sibling of the
// gw2gfx image surface). Shaders are our own runtime-compiled HLSL (ported from
// gw2mcp/viewer/gw2viewer.cpp) -- NOT the game's DXBC blobs.
namespace gw2m3d {

enum class RenderMode {
    Full,      // albedo * lambert + normal maps + effect blending (reconstructed HLSL)
    Plain,     // untextured flat lambert (no textures, white)
    Wireframe, // wireframe, untextured
    GameShader, // the GAME's own bgfx DXBC shaders per material (from the AMAT package)
};

bool initialize(HWND target_window);
void shutdown();
void on_resize(int width, int height);

// Uploads a model (vertex/index buffers + per-material textures) and frames it.
void set_model(const ModelPreview& model);
void clear_model();

// --- map / multi-model scene -----------------------------------------------
// Map content layers (each independently toggleable).
enum SceneLayer { LAYER_PROP = 0, LAYER_TERRAIN = 1, LAYER_COLLISION = 2, LAYER_ZONE = 3, LAYER_COUNT = 4 };

// One placed model instance: which model (index into the models vector) at a
// world position/rotation(Euler radians)/uniform scale, tagged with its layer.
struct SceneInstance {
    int model = 0;
    float pos[3] = {0, 0, 0};
    float rot[3] = {0, 0, 0};
    float scale = 1.0f;
    int layer = LAYER_PROP;
};

// Uploads a set of unique models + a list of instances, and renders them all at
// their world transforms (a coordinated map scene) with the orbit camera. The
// same surface/camera controls as the single-model path apply.
void set_scene(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances);
// Appends more models + instances to the current scene (for lazily-added layers
// like zone models); instance.model is relative to the appended models list.
void add_scene_models(const std::vector<ModelPreview>& models, const std::vector<SceneInstance>& instances);
void clear_scene();
bool scene_active();

// Per-layer render visibility (default: prop + terrain on, collision + zone off).
void set_layer_visible(int layer, bool visible);
bool layer_visible(int layer);

void set_mode(RenderMode mode);

// Toggles the bind-pose skeleton overlay (bones drawn as lines + joint crosses,
// on top of the mesh). No effect on models without an inline rig.
void set_show_skeleton(bool show);
bool has_skeleton();

// --- animation / posing ---------------------------------------------------
// The skeleton overlay can be posed from a decoded Granny clip. Clip index -1 =
// bind pose. Setting a clip or time re-poses the rig (bones with no matching
// animation track keep their bind local transform). Playback advances the clip
// time on each render() when playing (wrapping at the clip duration).
int  animation_count();               // number of decoded clips on the current model
const char* animation_name(int i);    // clip name (e.g. "zeropose"); "" if out of range
float animation_duration(int i);      // clip duration in seconds
bool animation_has_motion(int i);     // true if clip i has real keyframed motion (not a static zeropose)
int  first_motion_animation();        // first clip index with real motion, or -1 if none
void set_animation(int clip_index);   // -1 = bind pose
int  current_animation();             // active clip index (-1 = bind)
void set_anim_time(float seconds);    // scrub to an absolute time
float anim_time();                    // current playback time
void set_playing(bool playing);
bool is_playing();

// Orbit / zoom, in the same feel as the reference viewer.
void orbit(float delta_yaw, float delta_pitch);
void zoom(float factor);
void reset_view();

void render();

// Debug: save the current rendered frame to a 24-bit BMP (for headless checks).
void save_screenshot(const char* path);

} // namespace gw2m3d

#endif // GW2_MODEL_RENDERER_H
