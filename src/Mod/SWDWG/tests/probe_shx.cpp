// Dump a .shx shape file: per-shape outline stats, and the raw byte codes +
// every segment of one chosen shape.  Used to diagnose "glyph draws as a
// single line / not visible" issues in the SHX viewer.
//
// Build & run (see _b_probe_shx.bat):
//   cl /nologo /EHsc /std:c++17 /O2 /MD /I "..\App" probe_shx.cpp
//       ..\App\DWGShapeFile.cpp /Fe:probe_shx.exe
//   probe_shx.exe <path.shx> [shapeNumber]
#include "../App/DWGShapeFile.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace SWDWG;

int main(int argc, char** argv)
{
    std::string path =
        "C:/Program Files/Autodesk/AutoCAD 2016/Fonts/FSDB_2.SHX";
    int want = -1;
    if (argc > 1) {
        path = argv[1];
    }
    if (argc > 2) {
        want = std::atoi(argv[2]);
    }

    const ShapeFile sf = parseShapeFile(path);
    std::printf("file: %s\nshapes: %zu\n", path.c_str(), sf.size());

    for (const auto& kv : sf) {
        const ShapeDef& sd = kv.second;
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        bool have = false;
        for (const ShapeSeg& s : sd.segs) {
            if (!have) {
                x0 = std::min(s.x0, s.x1);
                x1 = std::max(s.x0, s.x1);
                y0 = std::min(s.y0, s.y1);
                y1 = std::max(s.y0, s.y1);
                have = true;
            }
            else {
                x0 = std::min(x0, std::min(s.x0, s.x1));
                x1 = std::max(x1, std::max(s.x0, s.x1));
                y0 = std::min(y0, std::min(s.y0, s.y1));
                y1 = std::max(y1, std::max(s.y0, s.y1));
            }
        }
        std::printf("code=%d name='%s' segs=%zu codes=%zu", sd.number,
                    sd.name.c_str(), sd.segs.size(), sd.codes.size());
        if (have) {
            std::printf(" bbox=(%.4g,%.4g)-(%.4g,%.4g)", x0, y0, x1, y1);
        }
        std::printf("\n");

        if (want == sd.number) {
            std::printf("  raw codes:");
            for (unsigned char c : sd.codes) {
                std::printf(" %02X", c);
            }
            std::printf("\n");
            for (const ShapeSeg& s : sd.segs) {
                std::printf("  seg (%.6g,%.6g)->(%.6g,%.6g)\n", s.x0, s.y0,
                            s.x1, s.y1);
            }
        }
    }
    return 0;
}
