#include "../App/DWGShapeFile.h"
#include <cmath>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    using namespace SWDWG;
    const char* shx = "C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/"
                      "Support/ltypeshp.shx";
    ShapeFile sf = parseShapeFile(shx);
    const int target = (argc > 1) ? std::atoi(argv[1]) : 134;
    auto it = sf.find(target);
    if (it == sf.end()) {
        std::printf("shape %d not found\n", target);
        return 1;
    }
    std::printf("shape %d: %zu segs, raw:", target, it->second.segs.size());
    for (std::size_t i = 0; i < it->second.codes.size(); ++i) {
        std::printf(" %02X", it->second.codes[i]);
    }
    std::printf("\n");
    if (it->second.segs.empty()) return 0;
    double yMin = 1e300, xMin = 1e300, xMax = -1e300;
    for (const auto& s : it->second.segs) {
        yMin = std::min(yMin, std::min(s.y0, s.y1));
        xMin = std::min(xMin, std::min(s.x0, s.x1));
        xMax = std::max(xMax, std::max(s.x0, s.x1));
    }
    const auto& f = it->second.segs.front();
    const auto& b = it->second.segs.back();
    std::printf("  first=(%.3f,%.3f)  last=(%.3f,%.3f)  y_min=%.3f\n",
                f.x0, f.y0, b.x1, b.y1, yMin);
    std::printf("  will draw base? first.y0==y_min? %s   last.y1==y_min? %s\n",
                std::fabs(f.y0 - yMin) < 1e-6 ? "yes" : "no",
                std::fabs(b.y1 - yMin) < 1e-6 ? "yes" : "no");
    return 0;
}