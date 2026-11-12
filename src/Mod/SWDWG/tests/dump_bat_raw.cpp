//! Print the raw BAT shape definition bytes from ltypeshp.shx, together with
//! the interpreted segments, for several candidate files.
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>

static void dumpFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("== %s  (cannot open)\n", path.c_str());
        return;
    }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    std::printf("== %s  (%zu bytes)\n", path.c_str(), d.size());
    if (d.size() < 30) {
        return;
    }
    // Skip header, dump from offset 0x20 to end.
    std::printf("  bytes 0x20..0x7F:\n");
    for (std::size_t i = 0x20; i < d.size() && i < 0x80; ++i) {
        std::printf("    [0x%02zx] 0x%02X\n", i, d[i]);
    }
    // Locate "BAT" marker and print the 8 bytes following it.
    for (std::size_t i = 0; i + 8 < d.size(); ++i) {
        if (d[i] == 'B' && d[i + 1] == 'A' && d[i + 2] == 'T') {
            std::printf("  'BAT' at 0x%02zx, definition bytes:",
                        i + 3);
            for (std::size_t j = i + 3; j < i + 12 && j < d.size(); ++j) {
                std::printf(" %02X", d[j]);
            }
            std::printf("\n");
            break;
        }
    }
}

int main()
{
    dumpFile("C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/Support/ltypeshp.shx");
    dumpFile("C:/Program Files/Autodesk/AutoCAD 2016/Fonts/ltypeshp.shx");
    dumpFile("C:/Users/angan/AppData/Roaming/Autodesk/AutoCAD 2016/R20.1/chs/Support/ltypeshp.shx");
    return 0;
}
