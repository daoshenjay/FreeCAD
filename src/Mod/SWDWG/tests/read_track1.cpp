//! Print the exact TRACK1 shape bytes from ltypeshp.shx, with parse hints.
#include <cstdio>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>

int main()
{
    const char* path = "C:/Program Files/Autodesk/AutoCAD 2016/UserDataCache/Support/ltypeshp.shx";
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", path); return 1; }
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    std::printf("file: %zu bytes\n\n", d.size());

    // SHX header layout
    if (d.size() < 30) return 1;
    const unsigned first = d[24] | (d[25] << 8);
    const unsigned last  = d[26] | (d[27] << 8);
    const unsigned count = d[28] | (d[29] << 8);
    std::printf("first shape = %u, last = %u, count = %u\n", first, last, count);

    struct Loc { unsigned num; unsigned len; std::size_t off; };
    std::vector<Loc> locs;
    std::size_t p = 30;
    std::size_t data_off = 30 + 4 * count;
    for (unsigned i = 0; i < count && p + 4 <= d.size(); ++i) {
        Loc L;
        L.num = d[p] | (d[p+1] << 8);
        L.len = d[p+2] | (d[p+3] << 8);
        L.off = data_off;
        locs.push_back(L);
        p += 4;
    }
    for (const Loc& L : locs) {
        std::printf("shape %3u length %3u offset 0x%zx\n", L.num, L.len, L.off);
    }
    // Find TRACK1 = 130
    for (const Loc& L : locs) {
        if (L.num != 130) continue;
        std::printf("\nTRACK1 raw bytes (0x%zx..0x%zx, %u bytes):\n",
                    L.off, L.off + L.len - 1, L.len);
        for (std::size_t i = L.off; i < L.off + L.len && i < d.size(); ++i) {
            std::printf("  [0x%02zx] 0x%02X", i, d[i]);
            if (d[i] >= 0x20 && d[i] < 0x7F) {
                std::printf("  '%c'", d[i]);
            }
            std::printf("\n");
        }
    }
    return 0;
}
