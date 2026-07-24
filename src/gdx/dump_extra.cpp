// Tier-1/Tier-2 dump classes: coursedata, dlists, vertexdata, tables, ghosts, fonts.
// Mirrors gen_dump_all_extra.py. JSON output is compared by parsed structure (see
// tools/verify_native_dump.py), so exact formatting is not load-bearing; values are.

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace gdxdump {
namespace {

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// small helpers: big-endian reads, hex formatting, JSON building
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::int32_t beS32(const std::uint8_t* p) { return static_cast<std::int32_t>(be32(p)); }
std::int16_t beS16(const std::uint8_t* p) {
    return static_cast<std::int16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}
std::uint16_t beU16(const std::uint8_t* p) { return static_cast<std::uint16_t>((std::uint16_t(p[0]) << 8) | p[1]); }
float beF32(const std::uint8_t* p) {
    std::uint32_t u = be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
std::uint16_t leU16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8)); }
std::uint32_t leU32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::int32_t leS32(const std::uint8_t* p) { return static_cast<std::int32_t>(leU32(p)); }

std::string hexU(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}
std::string hex08(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}
std::string hex04(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%04X", v);
    return b;
}
std::string hexBytesLower(const std::uint8_t* p, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0xF]);
    }
    return s;
}

// JSON: build compact JSON; harness compares parsed structure.
std::string jStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    o += "\"";
    return o;
}
std::string jFloat(float f) {
    // %.17g guarantees the double (the float32 value widened) round-trips exactly, so the harness's
    // json.load yields the identical double the Python oracle's struct('>f')/repr produced.
    char b[64];
    std::snprintf(b, sizeof(b), "%.17g", static_cast<double>(f));
    std::string s = b;
    if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
    return s;
}
// Decode bytes as ASCII, invalid (>=0x80) -> U+FFFD, stopping at NUL if `nulTerminated`.
std::string asciiReplace(const std::uint8_t* p, std::size_t n, bool nulTerminated) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (nulTerminated && p[i] == 0) break;
        if (p[i] < 0x80) {
            s.push_back(static_cast<char>(p[i]));
        } else {
            s += "\xEF\xBF\xBD"; // U+FFFD replacement char (UTF-8)
        }
    }
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// 1) coursedata
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
const char* kVenueNames[] = {"VENUE_MUTE_CITY", "VENUE_PORT_TOWN", "VENUE_BIG_BLUE", "VENUE_SAND_OCEAN",
                             "VENUE_DEVILS_FOREST", "VENUE_WHITE_LAND", "VENUE_SECTOR", "VENUE_RED_CANYON",
                             "VENUE_FIRE_FIELD", "VENUE_SILENCE", "VENUE_ENDING"};
const char* kSkyboxNames[] = {"SKYBOX_PURPLE", "SKYBOX_TURQUOISE", "SKYBOX_DESERT", "SKYBOX_BLUE",
                              "SKYBOX_NIGHT", "SKYBOX_ORANGE", "SKYBOX_SUNSET", "SKYBOX_SKY_BLUE"};
const char* kTrackShapeNames[] = {"ROAD", "WALLED_ROAD", "PIPE", "CYLINDER", "HALF_PIPE", "TUNNEL", "AIR",
                                  "BORDERLESS_ROAD"};

std::string nameOrUnk(const char* const* names, int count, int idx) {
    if (idx >= 0 && idx < count) return names[idx];
    return "unk_" + std::to_string(idx);
}

std::string decodeTrackSegmentInfo(std::int32_t v) {
    std::uint32_t uv = static_cast<std::uint32_t>(v);
    int shapeIdx = (uv & 0x1C0) >> 6;
    std::vector<std::string> flags;
    struct { std::uint32_t bit; const char* name; } fl[] = {
        {0x8000000u, "FLAG_8000000"}, {0x10000000u, "JOINABLE"}, {0x20000000u, "INSIDE"},
        {0x40000000u, "CONTINUOUS"}, {0x80000000u, "FLAG_80000000"}};
    for (auto& f : fl)
        if (uv & f.bit) flags.push_back(f.name);
    std::string o = "{";
    o += "\"raw\":" + jStr(hex08(uv));
    o += ",\"type\":";
    if ((uv & 0x3F) != 0x3F)
        o += std::to_string(uv & 0x3F);
    else
        o += jStr("NONE");
    o += ",\"shapeIndex\":" + std::to_string(shapeIdx);
    o += ",\"shapeName\":" + jStr(nameOrUnk(kTrackShapeNames, 8, shapeIdx));
    o += ",\"join\":" + std::to_string((uv & 0x600) >> 9);
    o += ",\"chunkJoinEnd\":" + std::to_string((uv & 0x1800) >> 11);
    o += ",\"chunkJoinStart\":" + std::to_string((uv & 0x6000) >> 13);
    o += ",\"form\":" + std::to_string((uv & 0x38000) >> 15);
    o += ",\"flags\":[";
    for (std::size_t i = 0; i < flags.size(); ++i) {
        if (i) o += ",";
        o += jStr(flags[i]);
    }
    o += "]}";
    return o;
}

std::string s8ArrayJson(const std::uint8_t* raw, std::size_t base) {
    std::string o = "[";
    for (int i = 0; i < 64; ++i) {
        if (i) o += ",";
        o += std::to_string(static_cast<int>(static_cast<std::int8_t>(raw[base + i])));
    }
    o += "]";
    return o;
}

std::string decodeCourseJson(const std::uint8_t* raw, int slot, const std::string& symbol,
                             std::uint64_t romOffset) {
    int creatorId = raw[0];
    int cpCount = static_cast<std::int8_t>(raw[1]);
    int venue = static_cast<std::int8_t>(raw[2]);
    int skybox = static_cast<std::int8_t>(raw[3]);
    std::uint32_t checksum = be32(raw + 4);
    int flag = raw[8];
    std::string fileName = asciiReplace(raw + 9, 22, true);
    int bgm = static_cast<std::int8_t>(raw[0x1F]);

    std::string o = "{";
    o += "\"symbol\":" + jStr(symbol);
    o += ",\"slot\":" + std::to_string(slot);
    o += ",\"romOffset\":" + jStr(hexU(romOffset));
    o += ",\"creatorId\":" + std::to_string(creatorId);
    o += ",\"creatorIsNintendo\":" + std::string(creatorId == 4 ? "true" : "false");
    o += ",\"controlPointCount\":" + std::to_string(cpCount);
    o += ",\"venue\":" + std::to_string(venue);
    o += ",\"venueName\":" + jStr(nameOrUnk(kVenueNames, 11, venue));
    o += ",\"skybox\":" + std::to_string(skybox);
    o += ",\"skyboxName\":" + jStr(nameOrUnk(kSkyboxNames, 8, skybox));
    o += ",\"checksum\":" + jStr(hex08(checksum));
    o += ",\"flag\":" + std::to_string(flag);
    o += ",\"flag_unk_meaning\":true";
    o += ",\"fileName\":" + jStr(fileName);
    o += ",\"bgm\":" + std::to_string(bgm);
    // controlPoints truncated to clamp(cpCount, 0, 64). The record has a FIXED 64-slot control-point
    // region (0x20..0x520); the oracle always decodes those 64 slots and truncates the OUTPUT list to
    // control_points[:max(cp_count,0)] (a Python slice, so a cpCount > 64 yields all 64). cpCount is an
    // untrusted on-ROM byte (int8, so up to 127 here) -- clamping to [0,64] both mirrors the oracle
    // byte-for-byte AND keeps the decode loop inside the fixed region (no OOB past 0x520).
    int keep = cpCount;
    if (keep < 0) keep = 0;
    if (keep > 64) keep = 64;
    o += ",\"controlPoints\":[";
    for (int i = 0; i < keep; ++i) {
        std::size_t base = 0x20 + static_cast<std::size_t>(i) * 0x14;
        float x = beF32(raw + base), y = beF32(raw + base + 4), z = beF32(raw + base + 8);
        int radL = beS16(raw + base + 0x0C);
        int radR = beS16(raw + base + 0x0E);
        std::int32_t tsi = beS32(raw + base + 0x10);
        if (i) o += ",";
        o += "{\"pos\":[" + jFloat(x) + "," + jFloat(y) + "," + jFloat(z) + "]";
        o += ",\"radiusLeft\":" + std::to_string(radL);
        o += ",\"radiusRight\":" + std::to_string(radR);
        o += ",\"trackSegmentInfo\":" + decodeTrackSegmentInfo(tsi);
        o += "}";
    }
    o += "]";
    o += ",\"controlPointsRawCount\":64";
    o += ",\"note\":" +
         jStr("controlPoints[] truncated to controlPointCount; bankAngle/pit/dash/dirt/ice/jump/landmine/"
              "gate/building/sign are the full 64-slot raw ROM arrays -- only indices < "
              "controlPointCount are meaningful, the rest is stale/padding ROM data.");
    // bankAngle: 64 s16 at 0x520
    o += ",\"bankAngle\":[";
    for (int i = 0; i < 64; ++i) {
        if (i) o += ",";
        o += std::to_string(static_cast<int>(beS16(raw + 0x520 + i * 2)));
    }
    o += "]";
    o += ",\"pit\":" + s8ArrayJson(raw, 0x5A0);
    o += ",\"dash\":" + s8ArrayJson(raw, 0x5E0);
    o += ",\"dirt\":" + s8ArrayJson(raw, 0x620);
    o += ",\"ice\":" + s8ArrayJson(raw, 0x660);
    o += ",\"jump\":" + s8ArrayJson(raw, 0x6A0);
    o += ",\"landmine\":" + s8ArrayJson(raw, 0x6E0);
    o += ",\"gate\":" + s8ArrayJson(raw, 0x720);
    o += ",\"building\":" + s8ArrayJson(raw, 0x760);
    o += ",\"sign\":" + s8ArrayJson(raw, 0x7A0);
    o += "}";
    return o;
}

} // namespace

ClassResult runCourseData(Context& ctx) {
    ClassResult res;
    res.name = "coursedata";
    const std::string key = "segment_blob/course_data";
    auto payloadOpt = ctx.cart->readStripped(key, kBlobPayloadOffset);
    if (!payloadOpt) {
        res.hardError = true;
        res.errorMsg = key + " not found in archive -- skipping";
        return res;
    }
    std::vector<std::uint8_t>& payload = *payloadOpt;
    const std::size_t stride = 0x7E0;

    // Read course_data.yaml FZX:COURSE entries, sorted by (offset, key).
    std::string cdPath = joinPath(ctx.cartYamlDir, "course_data.yaml");
    YAML::Node cd;
    try {
        cd = YAML::LoadFile(cdPath);
    } catch (...) {
        res.hardError = true;
        res.errorMsg = "cannot load course_data.yaml";
        return res;
    }
    std::vector<std::tuple<std::uint64_t, std::string, std::string>> entries; // (offset, key, symbol)
    for (const auto& kv : cd) {
        std::string k = kv.first.as<std::string>();
        if (!k.empty() && k[0] == ':') continue;
        YAML::Node v = kv.second;
        if (!v.IsMap() || !v["type"] || v["type"].as<std::string>() != "FZX:COURSE") continue;
        std::uint64_t off = static_cast<std::uint64_t>(parseIntStr(v["offset"].as<std::string>()));
        std::string sym = v["symbol"] ? v["symbol"].as<std::string>() : k;
        entries.emplace_back(off, k, sym);
    }
    std::sort(entries.begin(), entries.end());

    std::string outDir = joinPath(ctx.dumpDir, "coursedata");
    makeDirs(outDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0;
    for (std::size_t slot = 0; slot < entries.size(); ++slot) {
        std::uint64_t romOffset = std::get<0>(entries[slot]);
        const std::string& symbol = std::get<2>(entries[slot]);
        if (!safeOutputComponent(symbol)) {
            std::fprintf(stderr, "  warn: coursedata: unsafe output name '%s'; skipping\n", symbol.c_str());
            ++failed;
            continue;
        }
        std::string binPath = joinPath(outDir, symbol + ".bin");
        std::string jsonPath = joinPath(outDir, symbol + ".json");
        if (fileExists(binPath) && fileExists(jsonPath)) {
            ++skipped;
            manifest.push_back({symbol, std::to_string(slot), hexU(romOffset), "skip-existing"});
            continue;
        }
        if ((slot + 1) * stride > payload.size()) {
            ++failed;
            continue;
        }
        const std::uint8_t* raw = payload.data() + slot * stride;
        writeFileBytes(binPath, raw, stride);
        writeFileText(jsonPath, decodeCourseJson(raw, static_cast<int>(slot), symbol, romOffset));
        ++dumped;
        manifest.push_back({symbol, std::to_string(slot), hexU(romOffset), "ok"});
    }
    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "symbol\tslot\tromOffset\tstatus   (26 CourseData records, fzx_course.h)", manifest);
    std::printf("  coursedata: %d dumped, %d skipped, %d failed (of %zu)\n", dumped, skipped, failed,
                entries.size());
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(entries.size());
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// 2) dlists + 3) vertexdata (archive-first segment_blob resolution)
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
namespace {
const std::map<int, std::string>& f3dex2Mnemonics() {
    static const std::map<int, std::string> m = {
        {0x00, "G_NOOP"}, {0x01, "G_VTX"}, {0x02, "G_MODIFYVTX"}, {0x03, "G_CULLDL"},
        {0x04, "G_BRANCH_Z"}, {0x05, "G_TRI1"}, {0x06, "G_TRI2"}, {0x07, "G_QUAD"}, {0x08, "G_LINE3D"},
        {0xD6, "G_DMA_IO"}, {0xD7, "G_TEXTURE"}, {0xD8, "G_POPMTX"}, {0xD9, "G_GEOMETRYMODE"},
        {0xDA, "G_MTX"}, {0xDB, "G_MOVEWORD"}, {0xDC, "G_MOVEMEM"}, {0xDD, "G_LOAD_UCODE"},
        {0xDE, "G_DL"}, {0xDF, "G_ENDDL"}, {0xE0, "G_SPNOOP"}, {0xE1, "G_RDPHALF_1"},
        {0xE2, "G_SETOTHERMODE_L"}, {0xE3, "G_SETOTHERMODE_H"}, {0xE4, "G_TEXRECT"},
        {0xE5, "G_TEXRECTFLIP"}, {0xE6, "G_RDPLOADSYNC"}, {0xE7, "G_RDPPIPESYNC"},
        {0xE8, "G_RDPTILESYNC"}, {0xE9, "G_RDPFULLSYNC"}, {0xEA, "G_SETKEYGB"}, {0xEB, "G_SETKEYR"},
        {0xEC, "G_SETCONVERT"}, {0xED, "G_SETSCISSOR"}, {0xEE, "G_SETPRIMDEPTH"},
        {0xEF, "G_RDPSETOTHERMODE"}, {0xF0, "G_LOADTLUT"}, {0xF1, "G_RDPHALF_2"},
        {0xF2, "G_SETTILESIZE"}, {0xF3, "G_LOADBLOCK"}, {0xF4, "G_LOADTILE"}, {0xF5, "G_SETTILE"},
        {0xF6, "G_FILLRECT"}, {0xF7, "G_SETFILLCOLOR"}, {0xF8, "G_SETFOGCOLOR"},
        {0xF9, "G_SETBLENDCOLOR"}, {0xFA, "G_SETPRIMCOLOR"}, {0xFB, "G_SETENVCOLOR"},
        {0xFC, "G_SETCOMBINE"}, {0xFD, "G_SETTIMG"}, {0xFE, "G_SETZIMG"}, {0xFF, "G_SETCIMG"}};
    return m;
}
constexpr int kGEnddl = 0xDF;
constexpr int kGDl = 0xDE;
constexpr int kMaxInstr = 8192;
} // namespace

ClassResult runDlists(Context& ctx) {
    ClassResult res;
    res.name = "dlists";
    std::vector<SegmentItem> items = walkSegmentItems(ctx.cartYamlDir, "GFX");
    std::sort(items.begin(), items.end(), [](const SegmentItem& a, const SegmentItem& b) {
        if (a.yamlStem != b.yamlStem) return a.yamlStem < b.yamlStem;
        return a.sym < b.sym;
    });
    std::string outDir = joinPath(ctx.dumpDir, "dlists");
    makeDirs(outDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0, archiveCovered = 0, romOnly = 0;
    std::vector<std::string> romRequired;
    for (const SegmentItem& it : items) {
        std::uint64_t addr = it.romBase + it.offset;
        std::string k = it.yamlStem + "_" + it.sym;
        if (!safeOutputComponent(k)) {
            std::fprintf(stderr, "  warn: dlists: unsafe output name '%s'; skipping\n", k.c_str());
            ++failed;
            continue;
        }
        std::string binPath = joinPath(outDir, k + ".bin");
        std::string txtPath = joinPath(outDir, k + ".txt");
        if (fileExists(binPath)) {
            ++skipped;
            std::size_t nbytes = static_cast<std::size_t>(fs::file_size(binPath));
            manifest.push_back({k, it.yamlStem, it.sym, hexU(addr), std::to_string(nbytes / 8),
                                std::to_string(nbytes), "skip-existing"});
            continue;
        }
        bool viaRom = false;
        std::vector<std::uint8_t> buf =
            ctx.rom->readUpTo(addr, static_cast<std::size_t>(kMaxInstr) * 8, &viaRom);
        if (buf.size() < 8) {
            std::fprintf(stderr,
                         "  warn: dlists %s: rom addr %s not resolvable from archive; original ROM "
                         "required for this entry\n",
                         k.c_str(), hexU(addr).c_str());
            romRequired.push_back(k);
            ++failed;
            continue;
        }
        // Scan.
        std::vector<std::tuple<std::size_t, std::uint32_t, std::uint32_t>> words; // (rel, w0, w1)
        std::size_t a = 0;
        int i = 0;
        while (i < kMaxInstr && a + 8 <= buf.size()) {
            std::uint32_t w0 = be32(&buf[a]);
            std::uint32_t w1 = be32(&buf[a + 4]);
            words.emplace_back(a, w0, w1);
            int op = (w0 >> 24) & 0xFF;
            a += 8;
            ++i;
            if (op == kGEnddl) break;
            if (op == kGDl && ((w0 >> 16) & 1) == 1) break;
        }
        std::vector<std::uint8_t> raw(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(a));
        writeFileBytes(binPath, raw.data(), raw.size());
        // txt
        std::string txt;
        {
            char h[256];
            std::snprintf(h, sizeof(h),
                          "# %s (yaml=%s symbol=%s) romAddr=%s segment=%d words=%zu bytes=%zu\n",
                          k.c_str(), it.yamlStem.c_str(), it.sym.c_str(), hexU(addr).c_str(),
                          it.segmentId, words.size(), raw.size());
            txt += h;
        }
        txt += "# offset(rel)  w0        w1        mnemonic\n";
        for (auto& w : words) {
            int op = (std::get<1>(w) >> 24) & 0xFF;
            auto mit = f3dex2Mnemonics().find(op);
            std::string mnem;
            if (mit != f3dex2Mnemonics().end())
                mnem = mit->second;
            else {
                char b[16];
                std::snprintf(b, sizeof(b), "op_%02X", op);
                mnem = b;
            }
            char line[128];
            std::snprintf(line, sizeof(line), "0x%06zX  %08X  %08X  %s\n", std::get<0>(w),
                          std::get<1>(w), std::get<2>(w), mnem.c_str());
            txt += line;
        }
        writeFileText(txtPath, txt);
        ++dumped;
        if (viaRom)
            ++romOnly;
        else
            ++archiveCovered;
        manifest.push_back({k, it.yamlStem, it.sym, hexU(addr), std::to_string(words.size()),
                            std::to_string(raw.size()), "ok"});
    }
    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "key\tyaml\tsymbol\tromAddr\twords\tbytes\tstatus   (raw F3DEX2 DLs, archive-first)",
                  manifest);
    std::printf("  dlists: %d dumped, %d skipped, %d failed (of %zu GFX entries)\n", dumped, skipped,
                failed, items.size());
    std::printf("  dlists coverage: %d archive-covered, %d rom-only, %d unresolved\n", archiveCovered,
                romOnly, failed);
    if (!romRequired.empty()) {
        std::fprintf(stderr, "  dlists: %zu entr(y/ies) require the original ROM (not archive-covered)\n",
                     romRequired.size());
    }
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(items.size());
    return res;
}

ClassResult runVertexData(Context& ctx) {
    ClassResult res;
    res.name = "vertexdata";
    std::vector<SegmentItem> items = walkSegmentItems(ctx.cartYamlDir, "VTX");
    std::sort(items.begin(), items.end(), [](const SegmentItem& a, const SegmentItem& b) {
        if (a.yamlStem != b.yamlStem) return a.yamlStem < b.yamlStem;
        return a.sym < b.sym;
    });
    std::string outDir = joinPath(ctx.dumpDir, "vertexdata");
    makeDirs(outDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0, archiveCovered = 0, romOnly = 0;
    std::vector<std::string> romRequired;
    // Sane upper bound on the per-entry vertex count. `it.count` is an untrusted yaml value parsed via
    // strtoll (see walkSegmentItems); without this a huge count wraps `count*16` (size_t) while the
    // decode loop still indexes with the un-wrapped count -> OOB read. 1<<24 (16M) vertices = 256 MiB,
    // far beyond any real F3D segment and comfortably below the size_t*16 wrap point.
    static constexpr long long kMaxVertexCount = 1LL << 24;
    for (const SegmentItem& it : items) {
        if (it.count <= 0 || it.count > kMaxVertexCount) {
            ++failed;
            continue;
        }
        std::uint64_t addr = it.romBase + it.offset;
        std::string k = it.yamlStem + "_" + it.sym;
        if (!safeOutputComponent(k)) {
            std::fprintf(stderr, "  warn: vertexdata: unsafe output name '%s'; skipping\n", k.c_str());
            ++failed;
            continue;
        }
        std::string binPath = joinPath(outDir, k + ".bin");
        std::string jsonPath = joinPath(outDir, k + ".json");
        if (fileExists(binPath) && fileExists(jsonPath)) {
            ++skipped;
            manifest.push_back({k, it.yamlStem, it.sym, hexU(addr), std::to_string(it.count),
                                std::to_string(static_cast<std::size_t>(fs::file_size(binPath))),
                                "skip-existing"});
            continue;
        }
        std::size_t nbytes = static_cast<std::size_t>(it.count) * 16;
        bool viaRom = false;
        auto sliceOpt = ctx.rom->read(addr, nbytes, &viaRom);
        if (!sliceOpt) {
            std::fprintf(stderr,
                         "  warn: vertexdata %s: rom range %s not resolvable from archive; original "
                         "ROM required for this entry\n",
                         k.c_str(), hexU(addr).c_str());
            romRequired.push_back(k);
            ++failed;
            continue;
        }
        std::vector<std::uint8_t>& raw = *sliceOpt;
        // Belt-and-braces: the reader should return exactly nbytes, but never index past what it gave us.
        if (raw.size() < nbytes) {
            std::fprintf(stderr, "  warn: vertexdata %s: short read (%zu < %zu); skipping\n", k.c_str(),
                         raw.size(), nbytes);
            ++failed;
            continue;
        }
        writeFileBytes(binPath, raw.data(), raw.size());
        int minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
        for (long long v = 0; v < it.count; ++v) {
            int x = beS16(&raw[v * 16]);
            int y = beS16(&raw[v * 16 + 2]);
            int z = beS16(&raw[v * 16 + 4]);
            if (v == 0) {
                minX = maxX = x;
                minY = maxY = y;
                minZ = maxZ = z;
            } else {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
                minZ = std::min(minZ, z); maxZ = std::max(maxZ, z);
            }
        }
        std::string j = "{";
        j += "\"symbol\":" + jStr(it.sym);
        j += ",\"yaml\":" + jStr(it.yamlStem);
        j += ",\"romAddr\":" + jStr(hexU(addr));
        j += ",\"segment\":" + std::to_string(it.segmentId);
        j += ",\"count\":" + std::to_string(it.count);
        j += ",\"bytes\":" + std::to_string(nbytes);
        j += ",\"bbox\":{\"x\":[" + std::to_string(minX) + "," + std::to_string(maxX) + "]";
        j += ",\"y\":[" + std::to_string(minY) + "," + std::to_string(maxY) + "]";
        j += ",\"z\":[" + std::to_string(minZ) + "," + std::to_string(maxZ) + "]}}";
        writeFileText(jsonPath, j);
        ++dumped;
        if (viaRom)
            ++romOnly;
        else
            ++archiveCovered;
        manifest.push_back({k, it.yamlStem, it.sym, hexU(addr), std::to_string(it.count),
                            std::to_string(nbytes), "ok"});
    }
    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "key\tyaml\tsymbol\tromAddr\tcount\tbytes\tstatus   (raw N64Vtx_t[], archive-first)",
                  manifest);
    std::printf("  vertexdata: %d dumped, %d skipped, %d failed (of %zu VTX entries)\n", dumped, skipped,
                failed, items.size());
    std::printf("  vertexdata coverage: %d archive-covered, %d rom-only, %d unresolved\n", archiveCovered,
                romOnly, failed);
    if (!romRequired.empty()) {
        std::fprintf(stderr,
                     "  vertexdata: %zu entr(y/ies) require the original ROM (not archive-covered)\n",
                     romRequired.size());
    }
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(items.size());
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// 4) tables
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
ClassResult runTables(Context& ctx) {
    ClassResult res;
    res.name = "tables";
    std::string outDir = joinPath(ctx.dumpDir, "tables");
    makeDirs(outDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0;

    // (a) raw regions from the cart archive.
    struct RawReg { const char* key; const char* fname; };
    RawReg regs[] = {{"audio_blob/audio_bank", "audio_bank.bin"}, {"audio_blob/audio_seq", "audio_seq.bin"}};
    for (const RawReg& r : regs) {
        auto payload = ctx.cart->readStripped(r.key, kBlobPayloadOffset);
        std::string outPath = joinPath(outDir, r.fname);
        if (!payload) {
            ++failed;
            continue;
        }
        if (fileExists(outPath)) {
            ++skipped;
            manifest.push_back({r.fname, r.key, std::to_string(static_cast<std::size_t>(fs::file_size(outPath))),
                                "raw-region-skip"});
            continue;
        }
        writeFileBytes(outPath, payload->data(), payload->size());
        ++dumped;
        manifest.push_back({r.fname, r.key, std::to_string(payload->size()), "raw-region"});
    }

    // (b) parsed soundfont/sequence tables from the build-generated data file (which the build step
    // produces FROM decomp/src/audio/disk/audio_tables.c using the oracle's own parser, so they can
    // never diverge).
    if (ctx.tablesDataPath.empty() || !fileExists(ctx.tablesDataPath)) {
        std::fprintf(stderr,
                     "  tables: generated table data not found (audio_tables_data.json). The build "
                     "step (tools/gen_dump_tables_data.py) must emit it into decomp-recipes.\n");
        ++failed; // sound font
        ++failed; // sequence
    } else {
        YAML::Node data;
        bool ok = true;
        try {
            data = YAML::LoadFile(ctx.tablesDataPath);
        } catch (...) {
            ok = false;
        }
        struct TblOut { const char* var; const char* out; };
        TblOut outs[] = {{"gSoundFontTable", "soundfont_table.json"},
                         {"gSequenceTable", "sequence_table.json"}};
        for (const TblOut& t : outs) {
            std::string outPath = joinPath(outDir, t.out);
            if (fileExists(outPath)) {
                ++skipped;
                // numEntries from existing file's data node for the manifest.
                int ne = 0;
                if (ok && data[t.var] && data[t.var]["entries"]) ne = static_cast<int>(data[t.var]["entries"].size());
                manifest.push_back({t.out, t.var, std::to_string(ne), "parsed-source-skip"});
                continue;
            }
            if (!ok || !data[t.var]) {
                ++failed;
                continue;
            }
            YAML::Node tn = data[t.var];
            std::string headerRaw = tn["headerRaw"] ? tn["headerRaw"].as<std::string>() : "";
            YAML::Node entries = tn["entries"];
            int numEntries = entries ? static_cast<int>(entries.size()) : 0;
            std::string j = "{";
            j += "\"headerRaw\":" + jStr(headerRaw);
            j += ",\"numEntries\":" + std::to_string(numEntries);
            j += ",\"entries\":[";
            for (std::size_t i = 0; i < (entries ? entries.size() : 0); ++i) {
                YAML::Node e = entries[i];
                if (i) j += ",";
                auto field = [&](const char* k) -> std::string {
                    if (!e[k] || e[k].IsNull()) return "null";
                    return jStr(e[k].as<std::string>());
                };
                j += "{\"name\":" + field("name");
                j += ",\"offset\":" + field("offset");
                j += ",\"size\":" + field("size");
                j += ",\"medium\":" + field("medium");
                j += ",\"cachePolicy\":" + field("cachePolicy");
                j += ",\"col5_raw\":" + field("col5_raw");
                j += ",\"col6_raw\":" + field("col6_raw");
                j += ",\"col7_raw\":" + field("col7_raw");
                j += "}";
            }
            j += "]";
            j += ",\"source\":" + jStr(std::string("decomp/src/audio/disk/audio_tables.c (") + t.var + ")");
            j += ",\"note\":" +
                 jStr("Compiled-in AudioTable initializer, transcribed from decomp source -- not present "
                      "as raw bytes in generic.o2r (audio_bank/audio_seq are placeholder-sized DATA "
                      "regions, not this header/entries struct; see class docstring).");
            j += "}";
            writeFileText(outPath, j);
            ++dumped;
            manifest.push_back({t.out, t.var, std::to_string(numEntries), "parsed-source"});
        }
    }

    writeManifest(joinPath(outDir, "manifest.tsv"), "file\tsource\tsize_or_entries\tkind", manifest);
    std::printf("  tables: %d dumped, %d skipped, %d failed\n", dumped, skipped, failed);
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = dumped + skipped + failed;
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// 5) ghosts
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
namespace {
std::uint32_t crc32(const std::uint8_t* data, std::size_t len) {
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
std::uint16_t checksum16(const std::uint8_t* data, std::size_t len) {
    std::uint32_t s = 0;
    for (std::size_t i = 0; i < len; ++i) s += data[i];
    return static_cast<std::uint16_t>(s & 0xFFFF);
}
std::uint32_t replayFingerprint(const std::uint8_t* replay, std::size_t len) {
    std::uint64_t checksum = 0;
    std::uint32_t total = 0;
    int i = 0;
    for (std::size_t n = 0; n < len; ++n) {
        total += static_cast<std::uint32_t>(replay[n]) << ((3 - i) * 8);
        i = (i + 1) % 4;
        if (i == 0) {
            checksum += total;
            total = 0;
        }
    }
    return static_cast<std::uint32_t>(checksum & 0xFFFFFFFFu);
}

struct GhostParsed {
    std::uint16_t ghostType;
    std::int32_t courseEncoding;
    std::int32_t raceTime;
    std::uint16_t unk10;
    std::string trackName;
    std::uint8_t machineInfo[20];
    std::int32_t lapTimes[3];
    std::int32_t replayEnd;
    std::uint32_t replaySize;
    std::vector<std::uint8_t> replayData;
    std::size_t consumedBytes;
    std::size_t totalBytes;
    bool valid = false;
};

GhostParsed parseGhost(const std::vector<std::uint8_t>& p) {
    GhostParsed g;
    std::size_t off = 0;
    auto need = [&](std::size_t n) { return off + n <= p.size(); };
    if (!need(2)) return g;
    g.ghostType = leU16(&p[off]); off += 2;
    if (!need(4)) return g;
    g.courseEncoding = leS32(&p[off]); off += 4;
    if (!need(4)) return g;
    g.raceTime = leS32(&p[off]); off += 4;
    if (!need(2)) return g;
    g.unk10 = leU16(&p[off]); off += 2;
    if (!need(4)) return g;
    std::uint32_t trackNameLen = leU32(&p[off]); off += 4;
    if (!need(trackNameLen)) return g;
    g.trackName = asciiReplace(&p[off], trackNameLen, false); off += trackNameLen;
    if (!need(20)) return g;
    std::memcpy(g.machineInfo, &p[off], 20); off += 20;
    if (!need(12)) return g;
    for (int i = 0; i < 3; ++i) { g.lapTimes[i] = leS32(&p[off]); off += 4; }
    if (!need(4)) return g;
    g.replayEnd = leS32(&p[off]); off += 4;
    if (!need(4)) return g;
    g.replaySize = leU32(&p[off]); off += 4;
    if (!need(4)) return g;
    std::uint32_t replayDataLen = leU32(&p[off]); off += 4;
    if (!need(replayDataLen)) return g;
    g.replayData.assign(p.begin() + static_cast<std::ptrdiff_t>(off),
                        p.begin() + static_cast<std::ptrdiff_t>(off + replayDataLen));
    off += replayDataLen;
    g.consumedBytes = off;
    g.totalBytes = p.size();
    g.valid = true;
    return g;
}

std::string ghostJson(const GhostParsed& g, const std::string& symbol) {
    const std::uint8_t* mi = g.machineInfo;
    std::string j = "{";
    j += "\"ghostType\":" + std::to_string(g.ghostType);
    j += ",\"courseEncoding\":" + std::to_string(g.courseEncoding);
    j += ",\"raceTime\":" + std::to_string(g.raceTime);
    j += ",\"unk10\":" + std::to_string(g.unk10);
    j += ",\"trackName\":" + jStr(g.trackName);
    j += ",\"machineInfo\":{";
    const char* names[20] = {"character", "customType", "frontType", "rearType", "wingType", "logo",
                             "number", "decal", "bodyR", "bodyG", "bodyB", "numberR", "numberG",
                             "numberB", "decalR", "decalG", "decalB", "cockpitR", "cockpitG", "cockpitB"};
    for (int i = 0; i < 20; ++i) {
        if (i) j += ",";
        j += jStr(names[i]) + ":" + std::to_string(mi[i]);
    }
    j += "}";
    j += ",\"machineInfoRaw\":" + jStr(hexBytesLower(mi, 20));
    j += ",\"lapTimes\":[" + std::to_string(g.lapTimes[0]) + "," + std::to_string(g.lapTimes[1]) + "," +
         std::to_string(g.lapTimes[2]) + "]";
    j += ",\"replayEnd\":" + std::to_string(g.replayEnd);
    j += ",\"replaySize\":" + std::to_string(g.replaySize);
    j += ",\"replayData\":" + jStr(hexBytesLower(g.replayData.data(), g.replayData.size()));
    j += ",\"consumedBytes\":" + std::to_string(g.consumedBytes);
    j += ",\"totalBytes\":" + std::to_string(g.totalBytes);
    j += ",\"derivedCourseId\":" + std::to_string(g.courseEncoding & 0x1F);
    j += ",\"symbol\":" + jStr(symbol);
    j += "}";
    return j;
}

// Builds the GDG1 container. Returns empty on error (replay too large).
std::vector<std::uint8_t> buildGdg(const GhostParsed& g) {
    if (g.replayData.size() > 16200) return {};
    std::vector<std::uint8_t> record(0x40, 0);
    auto putU16 = [](std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) {
        b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF;
    };
    auto putS32 = [](std::vector<std::uint8_t>& b, std::size_t o, std::int32_t v) {
        std::uint32_t u = static_cast<std::uint32_t>(v);
        b[o] = u & 0xFF; b[o + 1] = (u >> 8) & 0xFF; b[o + 2] = (u >> 16) & 0xFF; b[o + 3] = (u >> 24) & 0xFF;
    };
    putU16(record, 2, g.ghostType);
    std::uint32_t fp = replayFingerprint(g.replayData.data(), g.replayData.size());
    putS32(record, 4, static_cast<std::int32_t>(fp));
    putS32(record, 8, g.courseEncoding);
    putS32(record, 0xC, g.raceTime);
    putU16(record, 0x10, g.unk10);
    std::string name = g.trackName.substr(0, 9); // ascii-ish; already decoded
    for (std::size_t i = 0; i < name.size() && i < 9; ++i) record[0x17 + i] = static_cast<std::uint8_t>(name[i]);
    std::memcpy(&record[0x20], g.machineInfo, 20);
    putU16(record, 0, checksum16(&record[2], 0x40 - 2));

    std::vector<std::uint8_t> data(0x3F80, 0);
    for (int i = 0; i < 3; ++i) putS32(data, 4 + i * 4, g.lapTimes[i]);
    putS32(data, 0x10, g.replayEnd);
    // replaySize as u32 LE
    {
        std::uint32_t u = g.replaySize;
        data[0x14] = u & 0xFF; data[0x15] = (u >> 8) & 0xFF; data[0x16] = (u >> 16) & 0xFF; data[0x17] = (u >> 24) & 0xFF;
    }
    std::memcpy(&data[0x20], g.replayData.data(), g.replayData.size());
    putU16(data, 0, checksum16(&data[2], 0x3F80 - 2));

    std::vector<std::uint8_t> payload;
    payload.insert(payload.end(), record.begin(), record.end());
    payload.insert(payload.end(), data.begin(), data.end());
    std::uint32_t courseId = static_cast<std::uint32_t>(g.courseEncoding & 0x1F);
    std::uint32_t crc = crc32(payload.data(), payload.size());
    std::uint32_t payloadSize = 0x3FC0;
    std::vector<std::uint8_t> out;
    const char* magic = "GDG1";
    out.insert(out.end(), magic, magic + 4);
    auto push32 = [&](std::uint32_t v) {
        out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF); out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };
    push32(1);           // version
    push32(courseId);    // i32 course_id
    push32(payloadSize); // I payload size
    push32(crc);         // I crc
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
} // namespace

ClassResult runGhosts(Context& ctx) {
    ClassResult res;
    res.name = "ghosts";
    std::vector<std::string> names;
    for (const std::string& n : ctx.cart->names())
        if (n.rfind("staff_ghost_records/", 0) == 0) names.push_back(n);
    std::sort(names.begin(), names.end());

    std::string outDir = joinPath(ctx.dumpDir, "ghosts");
    makeDirs(outDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0;
    for (const std::string& key : names) {
        std::string symbol = key.substr(std::string("staff_ghost_records/").size());
        if (!safeOutputComponent(symbol)) {
            std::fprintf(stderr, "  warn: ghosts: unsafe output name '%s'; skipping\n", symbol.c_str());
            ++failed;
            continue;
        }
        std::string binPath = joinPath(outDir, symbol + ".bin");
        std::string jsonPath = joinPath(outDir, symbol + ".json");
        std::string gdgPath = joinPath(outDir, symbol + ".gdg");
        bool alreadyDone = fileExists(binPath) && fileExists(jsonPath);
        GhostParsed g;
        if (alreadyDone) {
            // Re-parse from the archive (simpler than re-reading JSON; deterministic identical result).
            auto raw = ctx.cart->readStripped(key, kOtrHeaderSize);
            if (raw) g = parseGhost(*raw);
            ++skipped;
        } else {
            auto raw = ctx.cart->readStripped(key, kOtrHeaderSize);
            if (!raw) {
                ++failed;
                continue;
            }
            writeFileBytes(binPath, raw->data(), raw->size());
            g = parseGhost(*raw);
            if (!g.valid) {
                std::fprintf(stderr, "  warn: ghosts %s: parse failed\n", symbol.c_str());
                ++failed;
                continue;
            }
            writeFileText(jsonPath, ghostJson(g, symbol));
            ++dumped;
        }
        if (!g.valid) continue;
        std::uint32_t courseId = static_cast<std::uint32_t>(g.courseEncoding & 0x1F);
        std::string gdgStatus;
        if (fileExists(gdgPath)) {
            gdgStatus = alreadyDone ? "ok" : "ok-skip";
        } else {
            std::vector<std::uint8_t> gdg = buildGdg(g);
            if (gdg.empty()) {
                gdgStatus = "skip: replay too large";
            } else {
                writeFileBytes(gdgPath, gdg.data(), gdg.size());
                gdgStatus = alreadyDone ? "ok-repaired" : "ok";
            }
        }
        manifest.push_back({symbol, std::to_string(g.ghostType),
                            hexU(static_cast<std::uint32_t>(g.courseEncoding) & 0xFFFFFFFFu),
                            std::to_string(courseId), std::to_string(g.replaySize), gdgStatus});
    }
    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "symbol\tghostType\tcourseEncoding\tderivedCourseId\treplaySize\tgdg_status", manifest);
    std::printf("  ghosts: %d dumped, %d skipped, %d failed (of %zu)\n", dumped, skipped, failed,
                names.size());
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(names.size());
    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// 6) fonts
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
namespace {
const int kPalette[16] = {0, 16, 32, 48, 64, 80, 96, 112, 136, 152, 168, 184, 200, 216, 232, 255};
constexpr std::size_t kGlyphBytes = 0x80;

// Python-slice semantics for block[start:start+len] with negative index handling; returns the bytes.
std::vector<std::uint8_t> pySlice(const std::vector<std::uint8_t>& block, long long start, long long len) {
    long long n = static_cast<long long>(block.size());
    long long stop = start + len;
    if (start < 0) { start += n; if (start < 0) start = 0; }
    if (stop < 0) { stop += n; if (stop < 0) stop = 0; }
    if (start > n) start = n;
    if (stop > n) stop = n;
    std::vector<std::uint8_t> out;
    if (start < stop) out.assign(block.begin() + start, block.begin() + stop);
    return out;
}

// Decode a 16x16 glyph at slot -> RGBA (or empty if the cell is short).
std::vector<std::uint8_t> decodeGlyph(const std::vector<std::uint8_t>& block, long long slot) {
    std::vector<std::uint8_t> cell = pySlice(block, slot * static_cast<long long>(kGlyphBytes), kGlyphBytes);
    if (cell.size() < kGlyphBytes) return {};
    std::vector<std::uint8_t> out(16 * 16 * 4, 0);
    for (int byteIdx = 0; byteIdx < 64; ++byteIdx) {
        int b = cell[byteIdx];
        int hi = (b >> 4) & 0xF;
        int lo = b & 0xF;
        int px0 = byteIdx * 2;
        int vals[2] = {kPalette[hi], kPalette[lo]};
        int pxs[2] = {px0, px0 + 1};
        for (int t = 0; t < 2; ++t) {
            int o = pxs[t] * 4;
            out[o] = out[o + 1] = out[o + 2] = static_cast<std::uint8_t>(vals[t]);
            out[o + 3] = 255;
        }
    }
    return out;
}

bool saveGlyph(const std::vector<std::uint8_t>& block, long long slot, const std::string& path) {
    std::vector<std::uint8_t> rgba = decodeGlyph(block, slot);
    if (rgba.empty()) return false;
    return writePng(path, 16, 16, rgba.data());
}

void saveSheet(const std::vector<std::uint8_t>& block, const std::vector<long long>& slots, int cols,
               const std::string& path) {
    int rows = (static_cast<int>(slots.size()) + cols - 1) / cols;
    if (rows == 0) return;
    int W = cols * 16, H = rows * 16;
    std::vector<std::uint8_t> sheet(static_cast<std::size_t>(W) * H * 4, 0);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        std::vector<std::uint8_t> g = decodeGlyph(block, slots[i]);
        if (g.empty()) continue;
        int gx = (static_cast<int>(i) % cols) * 16;
        int gy = (static_cast<int>(i) / cols) * 16;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                std::size_t src = (static_cast<std::size_t>(y) * 16 + x) * 4;
                std::size_t dst = (static_cast<std::size_t>(gy + y) * W + (gx + x)) * 4;
                sheet[dst] = g[src]; sheet[dst + 1] = g[src + 1];
                sheet[dst + 2] = g[src + 2]; sheet[dst + 3] = g[src + 3];
            }
        }
    }
    writePng(path, W, H, sheet.data());
}

// Port of LeoGetKAdr (port/n64_leo.c). Returns byte offset into the font block, or INT64_MIN if
// unresolvable (mirrors the None sentinel).
constexpr long long kNoAdr = (-1LL << 62);
long long getKAdr(int sjis, const std::vector<std::uint8_t>& kanji) {
    if (sjis < 0x8140 || sjis >= 0x9873) return kNoAdr;
    int cell = (sjis & 0xFF) - 0x40;
    if (cell >= 0x40) cell -= 1;
    if (sjis >= 0x8800) {
        int row = (sjis >> 8) - 0x88;
        return (static_cast<long long>(cell) + 0x30A + static_cast<long long>(row) * 0xBC) << 7;
    }
    int row = (sjis >> 8) - 0x81;
    long long tblRel = (static_cast<long long>(cell) + static_cast<long long>(row) * 0xBC) * 2;
    if (tblRel < 0 || tblRel + 2 > static_cast<long long>(kanji.size())) return kNoAdr;
    std::int16_t raw16 = beS16(&kanji[static_cast<std::size_t>(tblRel)]);
    return static_cast<long long>(raw16) << 7;
}

// Fault font code list, decoded from leo_fault_dd.c's octal-escaped C string literal.
const char* kFaultFontOctal =
    "\\243\\260\\243\\261\\243\\262\\243\\263\\243\\264\\243\\265\\243\\266\\243\\267\\243\\270\\243\\271"
    "\\245\\250\\245\\351\\241\\274\\310\\326\\271\\346\\274\\350\\260\\267\\300\\342\\314\\300\\275\\361"
    "\\244\\362\\244\\252\\306\\311\\244\\337\\244\\257\\244\\300\\244\\265\\244\\244\\241\\243\\241\\332"
    "\\303\\355\\260\\325\\241\\333\\245\\242\\245\\257\\245\\273\\245\\271\\245\\363\\245\\327\\305\\300"
    "\\314\\307\\303\\346\\244\\313\\245\\307\\245\\243\\310\\264\\244\\253\\244\\312\\244\\307\\276\\334"
    "\\244\\267\\244\\317\\241\\242\\272\\271\\271\\376\\244\\363\\244\\306\\264\\326\\260\\343\\244\\303"
    "\\244\\277\\244\\254\\244\\336\\244\\354\\244\\353\\262\\304\\307\\275\\134\\300\\255\\244\\242\\244"
    "\\352\\244\\271\\300\\265\\270\\362\\264\\271\\265\\257\\306\\260\\273\\376\\244\\316\\275\\320\\301"
    "\\260\\262\\363\\245\\277\\272\\307\\270\\345\\244\\255\\244\\301\\244\\310\\245\\326\\244\\273\\301"
    "\\264\\276\\303\\243\\301\\245\\334\\262\\241\\245\\262\\245\\340\\245\\263\\244\\341\\245\\352\\245"
    "\\303\\245\\310\\244\\320\\244\\351\\302\\324\\245\\342\\245\\311\\245\\354\\262\\350\\314\\314\\314\\341";

std::vector<std::uint8_t> decodeOctalEscapes(const std::string& s) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit((unsigned char)s[i + 1]) &&
            std::isdigit((unsigned char)s[i + 2]) && std::isdigit((unsigned char)s[i + 3])) {
            int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out.push_back(static_cast<std::uint8_t>(v));
            i += 4;
        } else {
            out.push_back(static_cast<std::uint8_t>(s[i]));
            i += 1;
        }
    }
    return out;
}

std::vector<int> faultFontCodes() {
    std::vector<std::uint8_t> raw = decodeOctalEscapes(kFaultFontOctal);
    std::vector<int> codes;
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) codes.push_back((raw[i] << 8) | raw[i + 1]);
    if (codes.size() > 110) codes.resize(110);
    return codes;
}
} // namespace

ClassResult runFonts(Context& ctx) {
    ClassResult res;
    res.name = "fonts";

    // Font block: prefer n64ddipl.o2r ipl/font_block, else the raw IPL ROM (candidate discovery).
    std::vector<std::uint8_t> block;
    if (ctx.ipl != nullptr) {
        auto fb = ctx.ipl->readRaw("ipl/font_block");
        if (fb) block = std::move(*fb);
    }
    if (block.empty()) {
        // Raw IPL fallback: read N64DDIPLROM.n64 and slice [0xA0000, 0xA0000+655360).
        std::vector<std::string> cands = {joinPath(fs::current_path().string(), "N64DDIPLROM.n64")};
        for (const std::string& c : cands) {
            if (!fileExists(c)) continue;
            std::string s = readFile(c);
            if (s.size() >= 0xA0000 + 655360) {
                block.assign(s.begin() + 0xA0000, s.begin() + 0xA0000 + 655360);
                break;
            }
        }
    }
    if (block.empty()) {
        res.hardError = true;
        res.errorMsg = "no IPL font block (n64ddipl.o2r ipl/font_block or N64DDIPLROM.n64) found -- skipping";
        return res;
    }
    std::size_t numSlots = block.size() / kGlyphBytes;

    // kanji index table (segment_blob/kanji_tables).
    std::vector<std::uint8_t> kanji;
    {
        auto kb = ctx.cart->readStripped("segment_blob/kanji_tables", kBlobPayloadOffset);
        if (kb) kanji = std::move(*kb);
    }

    std::string outDir = joinPath(ctx.dumpDir, "fonts");
    std::string faultDir = joinPath(outDir, "fault");
    std::string kanjiDir = joinPath(outDir, "kanji");
    makeDirs(faultDir);
    makeDirs(kanjiDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0;

    // ── fault font (110 fixed glyphs) ──
    std::vector<int> faultCodes = faultFontCodes();
    std::vector<long long> faultSlots;
    for (std::size_t idx = 0; idx < faultCodes.size(); ++idx) {
        int code = faultCodes[idx];
        long long slotByte = kanji.empty() ? kNoAdr : getKAdr(code, kanji);
        long long slotNum = (slotByte != kNoAdr) ? (slotByte / static_cast<long long>(kGlyphBytes))
                                                  : static_cast<long long>(idx);
        faultSlots.push_back(slotNum);
        char fn[32];
        std::snprintf(fn, sizeof(fn), "%03zu_%04X.png", idx, code);
        std::string pngPath = joinPath(faultDir, fn);
        std::string relPath = std::string("fault/") + fn;
        if (fileExists(pngPath)) {
            ++skipped;
            manifest.push_back({relPath, hex04(code), std::to_string(slotNum), "skip-existing"});
            continue;
        }
        if (saveGlyph(block, slotNum, pngPath)) {
            ++dumped;
            manifest.push_back({relPath, hex04(code), std::to_string(slotNum), "ok"});
        } else {
            ++failed;
        }
    }
    std::string faultSheet = joinPath(outDir, "fault_sheet.png");
    if (!fileExists(faultSheet)) saveSheet(block, faultSlots, 11, faultSheet);

    // ── kanji / symbol-kana block enumeration ──
    std::map<int, int> kanjiIndex; // sjis -> slot
    if (!kanji.empty()) {
        for (int row = 0; row < 7; ++row) {
            for (int cell = 0; cell < 188; ++cell) {
                int lowbyte = 0x40 + (cell < 0x3F ? cell : cell + 1);
                int sjis = ((0x81 + row) << 8) | lowbyte;
                long long off = getKAdr(sjis, kanji);
                if (off == kNoAdr || off < 0) continue;
                long long slotNum = off / static_cast<long long>(kGlyphBytes);
                if (slotNum >= 0 && slotNum < static_cast<long long>(numSlots))
                    kanjiIndex[sjis] = static_cast<int>(slotNum);
            }
        }
        int row = 0;
        while (true) {
            long long baseSlot = 0x30A + static_cast<long long>(row) * 0xBC;
            if (baseSlot >= static_cast<long long>(numSlots)) break;
            for (int cell = 0; cell < 188; ++cell) {
                long long slotNum = baseSlot + cell;
                if (slotNum >= static_cast<long long>(numSlots)) continue;
                int lowbyte = 0x40 + (cell < 0x3F ? cell : cell + 1);
                int sjis = ((0x88 + row) << 8) | lowbyte;
                kanjiIndex[sjis] = static_cast<int>(slotNum);
            }
            ++row;
        }
    }
    std::map<int, int> slotToSjis; // first-wins
    for (const auto& kv : kanjiIndex) slotToSjis.emplace(kv.second, kv.first); // map iterates sorted by sjis

    std::vector<std::uint8_t> blankSlot(kGlyphBytes, 0);
    for (std::size_t slot = 0; slot < numSlots; ++slot) {
        std::size_t base = slot * kGlyphBytes;
        if (std::equal(blankSlot.begin(), blankSlot.end(), block.begin() + base)) continue;
        char fn[32];
        std::snprintf(fn, sizeof(fn), "slot_%04zu.png", slot);
        std::string pngPath = joinPath(kanjiDir, fn);
        std::string relPath = std::string("kanji/") + fn;
        std::string sjisLabel;
        auto sit = slotToSjis.find(static_cast<int>(slot));
        sjisLabel = (sit != slotToSjis.end()) ? hex04(sit->second) : "unmapped";
        if (fileExists(pngPath)) {
            ++skipped;
            manifest.push_back({relPath, sjisLabel, std::to_string(slot), "skip-existing"});
            continue;
        }
        if (saveGlyph(block, static_cast<long long>(slot), pngPath)) {
            ++dumped;
            manifest.push_back({relPath, sjisLabel, std::to_string(slot), "ok"});
        } else {
            ++failed;
        }
    }
    std::string kanjiSheet = joinPath(outDir, "kanji_sheet.png");
    if (!fileExists(kanjiSheet)) {
        std::vector<long long> allSlots;
        for (std::size_t s = 0; s < numSlots; ++s) allSlots.push_back(static_cast<long long>(s));
        saveSheet(block, allSlots, 64, kanjiSheet);
    }

    // kanji_index.json
    std::string indexPath = joinPath(outDir, "kanji_index.json");
    if (!fileExists(indexPath)) {
        std::string j = "{";
        j += "\"source\":" + jStr("(ipl font block)");
        j += ",\"note\":" +
             jStr("sjis-code(hex) -> font-block glyph slot index, via ported LeoGetKAdr "
                  "(port/n64_leo.c:347-381); slot N's PNG is kanji/slot_NNNN.png");
        j += ",\"numSlots\":" + std::to_string(numSlots);
        j += ",\"mapping\":{";
        bool first = true;
        for (const auto& kv : kanjiIndex) {
            if (!first) j += ",";
            first = false;
            j += jStr(hex04(kv.first)) + ":" + std::to_string(kv.second);
        }
        j += "}}";
        writeFileText(indexPath, j);
        ++dumped;
    } else {
        ++skipped;
    }

    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "path\tsjisOrSlot\tslot\tstatus   (fault font: 110 fixed; kanji: full font-block "
                  "enumeration, blanks skipped)",
                  manifest);
    std::printf("  fonts: %d dumped, %d skipped, %d failed (fault=%zu codes, kanji=%zu slots, %zu sjis "
                "mapped)\n",
                dumped, skipped, failed, faultCodes.size(), numSlots, kanjiIndex.size());
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(numSlots + faultCodes.size());
    return res;
}

} // namespace gdxdump
