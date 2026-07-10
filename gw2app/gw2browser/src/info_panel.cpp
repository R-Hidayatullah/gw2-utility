#include "info_panel.h"

#include <cwchar>
#include <string>

#include "gw2model.hpp" // gw2model::describeFvf

namespace gw2info {

namespace {

struct State {
    std::wstring dat_info;
};

State* get_state(HWND panel) { return reinterpret_cast<State*>(GetWindowLongPtrW(panel, GWLP_USERDATA)); }

void set_panel_text(HWND panel, const std::wstring& entry_info) {
    State* state = get_state(panel);
    if (state == nullptr) {
        return;
    }
    std::wstring combined = state->dat_info;
    if (!entry_info.empty()) {
        combined += L"\r\n";
        combined += entry_info;
    }
    SetWindowTextW(panel, combined.c_str());
}

const wchar_t* dxgi_format_name(uint32_t format) {
    switch (format) {
    case 71: return L"BC1_UNORM (DXT1)";
    case 74: return L"BC2_UNORM (DXT2/3)";
    case 77: return L"BC3_UNORM (DXT4/5)";
    case 80: return L"BC4_UNORM (ATI1)";
    case 83: return L"BC5_UNORM (ATI2/3Dc)";
    case 95: return L"BC6H_UF16 (HDR)";
    case 98: return L"BC7_UNORM";
    case 28: return L"R8G8B8A8_UNORM (decoded RGBA)";
    default: return L"Unknown";
    }
}

// Channel layout (RGB / RGBA / RG / R / ...) inferred from the DXGI format.
const wchar_t* channel_layout(uint32_t format) {
    switch (format) {
    case 71: return L"RGB (BC1, 4 bpp, optional 1-bit alpha)";
    case 74: return L"RGBA (BC2, explicit alpha)";
    case 77: return L"RGBA (BC3, interpolated alpha)";
    case 80: return L"R (BC4, single channel)";
    case 83: return L"RG (BC5, two channel -- normal map)";
    case 95: return L"RGB (BC6H, HDR float)";
    case 98: return L"RGBA (BC7)";
    case 28: return L"RGBA (8-bit per channel)";
    default: return L"Unknown";
    }
}

// Whether the preview pixels are native block-compressed (GPU-decoded) or a
// fully decoded RGBA8888 surface.
bool is_block_compressed(uint32_t format) { return format != 28; }

void append_image_info(std::wstring& text, const ExtractedEntry& entry) {
    std::wstring format_label;
    if (!entry.preview_format_label.empty()) {
        format_label.assign(entry.preview_format_label.begin(), entry.preview_format_label.end());
    } else {
        format_label = dxgi_format_name(entry.preview_dxgi_format);
    }

    uint32_t bytes = static_cast<uint32_t>(entry.preview_pixels.size());
    wchar_t buf[512];
    swprintf(buf, 512,
             L"\r\n--- Image ---\r\n"
             L"Image type: 2D texture\r\n"
             L"Dimensions: %u x %u\r\n"
             L"Source format: %ls\r\n"
             L"DXGI format: %ls\r\n"
             L"Pixel layout: %ls\r\n"
             L"Data type: %ls\r\n"
             L"Preview payload: %u bytes\r\n",
             entry.preview_width, entry.preview_height, format_label.c_str(),
             dxgi_format_name(entry.preview_dxgi_format), channel_layout(entry.preview_dxgi_format),
             is_block_compressed(entry.preview_dxgi_format) ? L"block-compressed (GPU-decoded)"
                                                            : L"uncompressed RGBA8888",
             bytes);
    text += buf;
}

void append_model_info(std::wstring& text, const ExtractedEntry& entry) {
    if (!entry.model) {
        text += L"\r\n--- Model ---\r\n"
                L"Detected a .modl packfile, but no struct template is loaded.\r\n"
                L"Use File -> Load Struct JSON... (gw2_packfile.json) to preview it.\r\n";
        return;
    }
    const ModelPreview& m = *entry.model;

    wchar_t buf[512];
    swprintf(buf, 512,
             L"\r\n--- Model ---\r\n"
             L"Meshes/submeshes: %zu\r\n"
             L"Materials: %zu\r\n"
             L"Textures decoded: %zu\r\n"
             L"Total vertices: %u\r\n"
             L"Total triangles: %u\r\n"
             L"Bounding radius: %.2f\r\n",
             m.meshes.size(), m.materials.size(), m.textures.size(), m.totalVerts, m.totalTris, m.radius);
    text += buf;

    // Skeleton (bind pose) + embedded-animation summary.
    text += L"\r\n--- Skeleton / Animation ---\r\n";
    if (!m.joints.empty()) {
        std::wstring stypew(m.skeletonType.begin(), m.skeletonType.end());
        swprintf(buf, 512, L"Skeleton: %zu bones  (SKEL v%d, %ls)\r\n", m.joints.size(), m.skeletonVersion,
                 stypew.c_str());
        text += buf;
        // A few root/leading bone names for orientation.
        int shown = 0;
        for (const ModelJoint& j : m.joints) {
            if (j.name.empty()) continue;
            std::wstring nw(j.name.begin(), j.name.end());
            swprintf(buf, 512, L"    [%d] parent=%d  %ls\r\n", shown, j.parent, nw.c_str());
            text += buf;
            if (++shown >= 8) break;
        }
        if (m.joints.size() > 8) text += L"    ...\r\n";
    } else if (m.externalSkeletonRef != 0) {
        swprintf(buf, 512, L"Skeleton: external rig (fileId %u) -- not embedded in this .modl\r\n",
                 m.externalSkeletonRef);
        text += buf;
    } else {
        text += L"Skeleton: none (static model)\r\n";
    }
    if (m.hasAnimation) {
        std::wstring atypew(m.animationType.begin(), m.animationType.end());
        swprintf(buf, 512, L"Animation: %zu clip(s) decoded  (ANIM v%d, %ls)\r\n", m.animClips.size(),
                 m.animationVersion, atypew.c_str());
        text += buf;
        int shown = 0;
        for (const granny::Anim& c : m.animClips) {
            std::wstring nw(c.name.begin(), c.name.end());
            swprintf(buf, 512, L"    \"%ls\"  %.3fs, %zu tracks\r\n", nw.c_str(), c.duration, c.tracks.size());
            text += buf;
            if (++shown >= 8) break;
        }
        if (m.animClips.size() > 8) text += L"    ...\r\n";
        text += L"    Use the clip dropdown + Play to pose the skeleton.\r\n"
                L"    (Embedded clips are usually the static \"zeropose\"; real\r\n"
                L"     locomotion lives in separate external anim files.)\r\n";
    } else {
        text += L"Animation: none embedded\r\n";
    }

    // Per-mesh: FVF breakdown with per-component sizes + stride.
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const ModelMeshCPU& mesh = m.meshes[i];
        int stride = 0;
        std::vector<std::string> comps = gw2model::describeFvf(mesh.fvf, &stride);
        swprintf(buf, 512,
                 L"\r\n  Mesh #%zu  material=%u  verts=%u  tris=%zu\r\n"
                 L"    FVF 0x%08X  stride=%dB  (%zu components):\r\n",
                 i, mesh.materialIndex, mesh.vertexCount, mesh.indices.size() / 3, mesh.fvf, stride, comps.size());
        text += buf;
        for (const std::string& c : comps) {
            text += L"      - ";
            text.append(c.begin(), c.end());
            text += L"\r\n";
        }
    }

    // Textures used, with size + role.
    if (!m.textures.empty()) {
        text += L"\r\n  Textures:\r\n";
        for (const ModelTextureCPU& t : m.textures) {
            std::wstring fmtw(t.fmt.begin(), t.fmt.end());
            swprintf(buf, 512, L"    fileId %u: %d x %d  %ls  [%ls]\r\n", t.fileId, t.width, t.height, fmtw.c_str(),
                     t.isNormal ? L"normal" : L"diffuse");
            text += buf;
        }
    }
}

} // namespace

HWND create(HWND parent, HINSTANCE instance, int control_id) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0,
                                 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance,
                                 nullptr);
    if (edit != nullptr) {
        static HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                         FIXED_PITCH | FF_MODERN, L"Consolas");
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(edit, EM_SETLIMITTEXT, 0, 0); // lift the default edit-control text cap
        SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new State()));
    }
    return edit;
}

void show_dat_info(HWND panel, const Gw2Dat& data_gw2) {
    State* state = get_state(panel);
    if (state == nullptr) {
        return;
    }

    wchar_t buf[2048];
    swprintf(buf, 2048,
             L"=== Archive ===\r\n"
             L"File: %hs\r\n"
             L"Size: %llu bytes\r\n"
             L"Version: %u\r\n"
             L"Header size: %u bytes\r\n"
             L"Chunk size: %u bytes\r\n"
             L"MFT offset: 0x%llX\r\n"
             L"MFT size: %u bytes\r\n"
             L"MFT entries: %zu\r\n"
             L"Assets (base ids): %zu\r\n"
             L"File ids: %zu\r\n",
             data_gw2.file_info.file_path.c_str(), static_cast<unsigned long long>(data_gw2.file_info.file_size),
             static_cast<unsigned>(data_gw2.dat_header.version), data_gw2.dat_header.header_size,
             data_gw2.dat_header.chunk_size, static_cast<unsigned long long>(data_gw2.dat_header.mft_offset),
             data_gw2.dat_header.mft_size, data_gw2.mft_data_list.size(), data_gw2.mft_base_id_data_list.size(),
             data_gw2.mft_file_id_data_list.size());

    state->dat_info = buf;
    set_panel_text(panel, L"");
}

void show_entry_info(HWND panel, const Gw2Dat& data_gw2, uint32_t mft_index, const ExtractedEntry& entry) {
    if (mft_index >= data_gw2.mft_data_list.size()) {
        return;
    }
    const MftData& mft = data_gw2.mft_data_list[mft_index];

    const wchar_t* kind_name = L"Binary";
    switch (entry.kind) {
    case PreviewKind::Image: kind_name = L"Image"; break;
    case PreviewKind::Model: kind_name = L"Model"; break;
    case PreviewKind::Map: kind_name = L"Map (mapc/area)"; break;
    case PreviewKind::Text: kind_name = L"Text"; break;
    case PreviewKind::Strs: kind_name = L"String table (strs)"; break;
    default: break;
    }

    wchar_t buf[1024];
    int written = swprintf(buf, 1024,
                            L"\r\n=== Selected Entry ===\r\n"
                            L"MFT index: %u\r\n"
                            L"Detected type: %ls\r\n"
                            L"Offset: 0x%llX\r\n"
                            L"Stored size: %u bytes\r\n"
                            L"Compression flag: %u\r\n"
                            L"Entry flag: %u\r\n"
                            L"Counter: %u\r\n"
                            L"CRC32C: 0x%08X\r\n"
                            L"Compressed (before) bytes: %zu\r\n"
                            L"Decompressed (after) bytes: %zu\r\n",
                            mft_index, kind_name, static_cast<unsigned long long>(mft.offset), mft.size,
                            mft.compression_flag, mft.entry_flag, mft.counter, mft.crc, entry.compressed.size(),
                            entry.decompressed.size());

    std::wstring text(buf, static_cast<size_t>(written > 0 ? written : 0));

    if (entry.kind == PreviewKind::Image) {
        append_image_info(text, entry);
    } else if (entry.kind == PreviewKind::Model) {
        append_model_info(text, entry);
    } else if (entry.kind == PreviewKind::Map) {
        text += L"\r\n--- Map scene ---\r\n";
        if (entry.map) {
            wchar_t mb[256];
            swprintf(mb, 256, L"Placed props: %u\r\nUnique models loaded: %u\r\nInstances rendered: %zu\r\n",
                     entry.map->totalProps, entry.map->loadedModels, entry.map->instances.size());
            text += mb;
            text += L"Drag to orbit, wheel to zoom. (Props only; terrain/water not rendered.)\r\n";
        } else {
            text += L"Map detected, but the scene could not be built (load the struct template).\r\n";
        }
    }

    set_panel_text(panel, text);
}

} // namespace gw2info
