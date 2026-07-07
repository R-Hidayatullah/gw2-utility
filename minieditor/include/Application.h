#pragma once
#include "HexViewer.h"
#include "TreeView.h"
#include "JsonEditor.h"
#include "TemplateManager.h"
#include "BinaryParser.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

struct GLFWwindow;

// Owns all panel objects, the current file buffer, and the current parse
// result. Wires drag-and-drop, the docking layout, and panel <-> panel
// communication (tree selection -> hex highlight, editor Run -> parser).
class Application {
public:
    bool init(GLFWwindow* window);
    void drawUI();
    void onFileDropped(const std::string& path);

private:
    HexViewer m_hexViewer;
    TreeView m_treeView;
    JsonEditor m_jsonEditor;
    TemplateManager m_templateManager;
    BinaryParser m_binaryParser;

    std::vector<uint8_t> m_fileData;
    std::string m_currentFilePath;
    char m_pathBuffer[512] = "";

    ParsedNodePtr m_root;
    size_t m_selOffset = 0;
    size_t m_selSize = 0;
    std::string m_statusMessage;

    // --- GW2 packfile panel state ---
    struct Gw2ChunkRow {
        std::string fourcc; int version; size_t offset; size_t size;
        std::string type;                      // currently chosen struct typeKey ("" = none)
        std::vector<std::string> tabLabels;    // one per available strucTab: "0x141EE4268 [MODL,ROOT]"
        std::vector<std::string> tabTypes;     // resolved typeKey for THIS version per strucTab ("" if absent)
        int tabChoice = 0;                     // index into tabLabels/tabTypes
    };
    nlohmann::json m_gw2Registry;
    bool m_gw2Loaded = false;
    bool m_pfDetected = false;
    std::string m_pfContainer;
    int m_pfVersion = 0;
    int m_pfPtrBits = 0;
    size_t m_pfHeaderSize = 12;
    std::string m_gw2Status;
    std::vector<Gw2ChunkRow> m_pfChunks;
    std::vector<std::string> m_ftKeys;   // combo items; [0] = "(auto: <container>)"
    int m_ftSelected = 0;
    int m_chunkSelected = -1;
    std::string m_schemaPreview;

    bool loadGw2Registry();
    void detectPf();
    std::string resolveChunkType(const std::string& container, const std::string& fourcc, int version);
    void rebuildChunkTabs(const std::string& container);
    void updateSchemaPreview();
    void drawGw2Panel();

    void openFile(const std::string& path);
    void runTemplate(const nlohmann::json& templateJson);
    void drawDockspace();
    void drawMenuBar();
};
