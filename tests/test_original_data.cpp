#include "assets/original_data.h"
#include "game/puzzle_data.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
void require(bool value) { if (!value) throw std::runtime_error("Original-data test failed"); }
template<class F> void rejects(F f) {
    bool failed = false;
    try { f(); } catch (const std::runtime_error &) { failed = true; }
    require(failed);
}

void synthetic() {
    auto &source = fgp::OriginalData::instance();
    const auto values = fgp::originalArray<std::uint32_t, 3>(0x401000U);
    rejects([&] { (void)values[0]; });
    std::vector<std::uint8_t> bytes(1024);
    const auto word = [&](std::size_t at, std::uint16_t n) { bytes[at]=n&255; bytes[at+1]=n>>8; };
    const auto dword = [&](std::size_t at, std::uint32_t n) { fgp::writeLe32(bytes,at,n); };
    word(0,0x5a4d); dword(60,128); dword(128,0x4550); word(134,1);
    word(148,224); word(152,0x10b); dword(180,0x400000);
    dword(384,1024); dword(388,0x1000); dword(392,512); dword(396,512);
    dword(512,17); dword(516,29); dword(520,43);
    bytes[528]='a'; bytes[529]='b';
    source.install(bytes);
    require(values[0]==17 && values[2]==43 && source.string(0x401010) == "ab");
    rejects([&] { (void)source.bytes(0x400fff,1); });
    rejects([&] { (void)source.bytes(0x4011ff,2); });
    rejects([&] { (void)source.bytes(0x401200,1); }); // virtual section tail has no data
    rejects([&] { (void)source.string(0x401010,2); });
    auto bad=bytes; bad[0]=0;
    rejects([&] { source.install(bad); });
    require(values[0]==17); // failed installation leaves the last source intact
    dword(512,71); source.install(bytes); require(values[0]==71);
    bad=bytes; fgp::writeLe32(bad,396,1000);
    rejects([&] { source.install(bad); });
    std::cout << "Original-data bounds, deferred reads and cache replacement passed\n";
}

void probe(const char *path) {
    fgp::OriginalData::instance().install(fgp::readBinaryFile(path));
    const auto &p = fgp::puzzleOriginalData();
    std::cout << std::setprecision(17) << "{\"eye\":[";
    for (std::size_t i=0;i<3;++i) std::cout << (i?",":"") << p.eye[i];
    std::cout << "],\"cells\":[";
    for (std::size_t i=0;i<72;++i) {
        if(i) std::cout << ',';
        std::cout << '[' << p.anchors[i].x << ',' << p.anchors[i].y << ',' << p.seeds[i].x << ',' << p.seeds[i].y
          << ',' << p.path[i][0] << ',' << p.path[i][1] << ',' << p.centres[i][0] << ',' << p.centres[i][1] << ',' << p.centres[i][2] << ']';
    }
    std::cout << "],\"meshes\":[";
    for(std::size_t i=0;i<7;++i) {
        if(i)std::cout << ',';
        std::cout << "{\"vertices\":[";
        bool first=true;
        for(auto v:p.vertices[i]) { if(!first)std::cout<<',';first=false;std::cout<<'['<<v.x<<','<<v.y<<','<<v.z<<']'; }
        std::cout << "],\"faces\":["; first=true;
        for(auto f:p.faces[i]) { if(!first)std::cout<<',';first=false;std::cout<<'['<<f.a<<','<<f.b<<','<<f.c<<']'; }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
}
int main(int argc,char **argv) {
    try { if(argc==2)probe(argv[1]);else synthetic(); }
    catch(const std::exception &error) { std::cerr << error.what() << '\n';return EXIT_FAILURE; }
}
