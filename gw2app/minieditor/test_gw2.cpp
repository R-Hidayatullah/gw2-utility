// Console test harness for the GW2 packfile parsing path (no GUI).
// Builds a synthetic PF buffer + inline registry, verifies pointer following,
// then smoke-loads the real 2 MB gw2_packfile.json.
#include "BinaryParser.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <functional>

static void put16(std::vector<uint8_t>& b, size_t o, uint16_t v){ b[o]=v&0xFF; b[o+1]=v>>8; }
static void put32(std::vector<uint8_t>& b, size_t o, uint32_t v){ for(int i=0;i<4;i++) b[o+i]=(v>>(8*i))&0xFF; }
static void put64(std::vector<uint8_t>& b, size_t o, uint64_t v){ for(int i=0;i<8;i++) b[o+i]=(v>>(8*i))&0xFF; }
static void putf (std::vector<uint8_t>& b, size_t o, float f){ uint32_t v; std::memcpy(&v,&f,4); put32(b,o,v); }

static void dump(const ParsedNodePtr& n, int d){
    for(int i=0;i<d;i++) printf("  ");
    printf("%s : %s @%zu", n->name.c_str(), n->typeName.c_str(), n->offset);
    if(!n->valueString.empty()) printf(" = %s", n->valueString.c_str());
    printf("\n");
    for(auto& c : n->children) dump(c, d+1);
}

int main(int argc, char** argv){
    // --- synthetic PF buffer ---
    std::vector<uint8_t> buf(72, 0);
    buf[0]='P'; buf[1]='F';
    put16(buf,2,0); put16(buf,4,0); put16(buf,6,12);       // version,zero,headerSize=12
    buf[8]='T';buf[9]='E';buf[10]='S';buf[11]='T';          // container type
    // chunk @12
    buf[12]='T';buf[13]='E';buf[14]='S';buf[15]='T';        // fourcc
    put32(buf,16,52);                                       // chunkSize (bytes after this field -> next=12+8+52=72)
    put16(buf,20,0);                                        // version 0
    put16(buf,22,16);                                       // chunkHdrSize
    put32(buf,24,44);                                       // offsetToOffsetTable (info only)
    // data @28
    put32(buf,28,0x11223344);                              // a dword
    putf (buf,32,1.5f);                                     // b float
    put32(buf,36,3);                                        // arr count
    put64(buf,40,24);                                       // arr ptr: 40+24=64
    put64(buf,48,20);                                       // sub ptr: 48+20=68
    buf[64]=10; buf[65]=20; buf[66]=30;                     // arr bytes @64
    put16(buf,68,0xAAAA); put16(buf,70,0xBBBB);            // Sub{x,y} @68

    nlohmann::json tpl = {
        {"format","gw2packfile"},{"pointerSize",64},
        {"chunks",{{"TEST",{{"0","Root"}}}}},
        {"types",{
            {"Root",{{"fields",{
                {{"name","a"},{"kind","dword"}},
                {{"name","b"},{"kind","float"}},
                {{"name","arr"},{"kind","array_ptr"},{"element","byte"}},
                {{"name","sub"},{"kind","ptr"},{"target",{{"struct","Sub"}}}}
            }}}},
            {"Sub",{{"fields",{
                {{"name","x"},{"kind","word"}},
                {{"name","y"},{"kind","word"}}
            }}}}
        }}
    };

    BinaryParser p; ParsedNodePtr root; std::string err;
    printf("=== SYNTHETIC TEST ===\n");
    if(!p.parse(buf, tpl, root, err)){ printf("FAIL: %s\n", err.c_str()); return 1; }
    dump(root, 0);

    // --- smoke test: load the real registry ---
    if(argc>1){
        printf("\n=== REAL REGISTRY SMOKE (%s) ===\n", argv[1]);
        std::ifstream in(argv[1], std::ios::binary);
        if(!in){ printf("cannot open\n"); return 0; }
        nlohmann::json big;
        try { in >> big; } catch(std::exception&e){ printf("JSON PARSE FAIL: %s\n", e.what()); return 1; }
        printf("loaded ok: format=%s ptr=%d chunks=%zu types=%zu\n",
            big.value("format","?").c_str(), big.value("pointerSize",0),
            big["chunks"].size(), big["types"].size());
        printf("ASND v2 root = %s\n", big["chunks"]["ASND"]["2"].get<std::string>().c_str());

        // --- parse a real packfile if given ---
        if(argc>2){
            printf("\n=== REAL PACKFILE (%s) ===\n", argv[2]);
            std::ifstream bin(argv[2], std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
            printf("%zu bytes\n", data.size());
            BinaryParser p2; ParsedNodePtr r2; std::string e2;
            if(!p2.parse(data, big, r2, e2)){ printf("PARSE FAIL: %s\n", e2.c_str()); return 1; }
            // print root + first 2 levels, capped
            int printed=0;
            std::function<void(const ParsedNodePtr&,int,int)> pr=[&](const ParsedNodePtr& n,int d,int maxd){
                if(printed>60) return;
                for(int i=0;i<d;i++) printf("  ");
                printf("%s : %s @%zu", n->name.c_str(), n->typeName.c_str(), n->offset);
                if(!n->valueString.empty()) printf(" = %s", n->valueString.c_str());
                printf("\n"); printed++;
                if(d<maxd) for(auto&c:n->children) pr(c,d+1,maxd);
            };
            pr(r2,0,2);

            // --- per-chunk strucTab override demo: force SKEL -> PHYS strucTab ---
            size_t skelOff=0;
            for (auto& c : r2->children) if (c->name.rfind("SKEL",0)==0) skelOff=c->offset;
            if (skelOff){
                // find PHYS SKEL v1 typeKey from strucTabs
                std::string physKey;
                for (auto& e : big["strucTabs"]["SKEL"])
                    for (auto& u : e["usedBy"]) if (u=="PHYS" && e["versions"].contains("1")) physKey=e["versions"]["1"];
                printf("\n=== OVERRIDE demo: SKEL@%zu forced to PHYS strucTab (%s) ===\n", skelOff, physKey.c_str());
                nlohmann::json big2 = big;
                big2["chunkOverrides"] = { { std::to_string(skelOff), physKey } };
                BinaryParser p3; ParsedNodePtr r3; std::string e3;
                p3.parse(data, big2, r3, e3);
                for (auto& c : r3->children) if (c->name.rfind("SKEL",0)==0){ printed=0; pr(c,0,2); }
            }
        }
    }
    printf("\nALL OK\n");
    return 0;
}
