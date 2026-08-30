// `textures` dump class — cart + 64DD EK. Mirrors gen_dump_all.py walk_textures / decode_texture /
// walk_ek_textures / decode_ek_texture / decode_ek_swatch.

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gdxdump {
namespace {

struct TexItem {
    std::string key;      // "<stem>/<sym>" (cart) or "ek/<sym>" (EK)
    std::string sym;
    std::string fmt;
    int width = 0;
    int height = 0;
    std::optional<std::string> paletteKey;
    bool hasRomBase = false;
    std::uint64_t romBase = 0;
    std::uint64_t offset = 0;
    int compressed = 0;
};

std::string nodeStr(const YAML::Node& n) { return n ? n.as<std::string>() : std::string(); }

// ── cart recipe walk (mirrors walk_textures) ─────────────────────────────────────────────────────
std::vector<TexItem> walkCartTextures(const std::string& yamlDir) {
    std::vector<TexItem> items;
    for (const std::string& path : listYaml(yamlDir, false)) {
        std::string fname = fs::path(path).filename().string();
        if (isBlobRecipeFilename(fname)) continue;
        bool isCommon = (fname == "common_assets_compressed.yaml");
        std::string stem = fs::path(path).stem().string();
        YAML::Node data;
        try {
            data = YAML::LoadFile(path);
        } catch (...) {
            continue;
        }
        bool hasSeg = false;
        int segId = 0;
        std::uint64_t romBase = 0;
        int compressed = 0;
        if (data[":config"]) {
            YAML::Node cfg = data[":config"];
            if (cfg["segments"] && cfg["segments"].IsSequence() && cfg["segments"].size() > 0) {
                YAML::Node s0 = cfg["segments"][0];
                if (s0.IsSequence() && s0.size() >= 2) {
                    hasSeg = true;
                    segId = static_cast<int>(parseIntStr(s0[0].as<std::string>()));
                    romBase = static_cast<std::uint64_t>(parseIntStr(s0[1].as<std::string>()));
                }
            }
            if (cfg["compression"] && cfg["compression"]["offset"]) compressed = 1;
        }
        // TLUT address map for this yaml.
        std::map<std::uint64_t, std::string> tlutByAddr;
        for (const auto& kv : data) {
            std::string key = kv.first.as<std::string>();
            if (!key.empty() && key[0] == ':') continue;
            YAML::Node val = kv.second;
            if (!val.IsMap()) continue;
            if (val["format"] && val["format"].as<std::string>() == "TLUT" && val["offset"]) {
                std::uint64_t off = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
                std::uint64_t full = hasSeg ? ((static_cast<std::uint64_t>(segId) << 24) | off) : off;
                tlutByAddr[full] = val["symbol"] ? val["symbol"].as<std::string>() : key;
            }
        }
        for (const auto& kv : data) {
            std::string key = kv.first.as<std::string>();
            if (!key.empty() && key[0] == ':') continue;
            YAML::Node val = kv.second;
            if (!val.IsMap()) continue;
            std::string type = nodeStr(val["type"]);
            if (type != "TEXTURE" && type != "COMPRESSED_TEXTURE") continue;
            std::string fmt = nodeStr(val["format"]);
            if (!isDecodableFormat(fmt)) continue;
            if (!val["offset"]) continue;
            if (!isCommon && !hasSeg) continue;
            std::string sym = val["symbol"] ? val["symbol"].as<std::string>() : key;
            TexItem it;
            it.key = stem + "/" + sym;
            it.sym = sym;
            it.fmt = fmt;
            it.width = static_cast<int>(parseIntStr(val["width"].as<std::string>()));
            it.height = static_cast<int>(parseIntStr(val["height"].as<std::string>()));
            it.hasRomBase = hasSeg;
            it.romBase = romBase;
            it.offset = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
            it.compressed = compressed;
            if (fmt == "CI4" || fmt == "CI8") {
                if (!val["tlut"]) continue;
                std::uint64_t tlut = static_cast<std::uint64_t>(parseIntStr(val["tlut"].as<std::string>()));
                auto pit = tlutByAddr.find(tlut);
                if (pit == tlutByAddr.end()) pit = tlutByAddr.find(tlut & 0x00FFFFFF);
                if (pit == tlutByAddr.end()) continue;
                it.paletteKey = stem + "/" + pit->second;
            }
            items.push_back(std::move(it));
        }
    }
    return items;
}

// Decode a cart TexItem: archive payload (strip 0x50), else archive-first ROM-slice fallback.
std::vector<std::uint8_t> decodeCart(const TexItem& it, Context& ctx) {
    std::optional<std::vector<std::uint8_t>> payload = ctx.cart->readStripped(it.key, kTexPayloadOffset);
    std::size_t need = texelBytes(it.fmt, static_cast<std::size_t>(it.width) * it.height);
    if (!payload) {
        // Fallback only for uncompressed segment entries (mirrors the oracle's rom_slice, which needs
        // a raw ROM). The cart archive is normally complete, so this rarely triggers.
        if (it.compressed == 0 && it.hasRomBase && ctx.rom->hasRom()) {
            auto slice = ctx.rom->read(it.romBase + it.offset, need);
            if (slice) payload = std::move(*slice);
        }
        if (!payload) return {};
    }
    if (payload->size() < need) return {};
    const std::uint8_t* pal = nullptr;
    std::vector<std::uint8_t> palBuf;
    if (it.paletteKey) {
        auto p = ctx.cart->readStripped(*it.paletteKey, kTexPayloadOffset);
        if (!p) return {};
        palBuf = std::move(*p);
        pal = palBuf.data();
    }
    return decodeTexel(it.fmt, payload->data(), payload->size(), it.width, it.height, pal, palBuf.size());
}

// ── EK manifest parse (mirrors parse_ek_manifest) ────────────────────────────────────────────────
struct EkRow {
    std::string sym;
    std::uint64_t off;
    std::uint64_t len;
    std::string typ;
    std::string fmt;
    int w = -1, h = -1;         // -1 == absent ('-')
    std::optional<std::uint64_t> tlut;
};

std::vector<EkRow> parseEkManifest(const std::string& path) {
    std::vector<EkRow> rows;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::istringstream ls(line);
        std::vector<std::string> parts;
        std::string tok;
        while (ls >> tok) parts.push_back(tok);
        if (parts.size() != 8) continue;
        if (parts[3] != "TEXTURE" && parts[3] != "COMPRESSED_TEXTURE") continue;
        EkRow r;
        r.sym = parts[0];
        r.off = static_cast<std::uint64_t>(std::strtoull(parts[1].c_str(), nullptr, 16));
        r.len = static_cast<std::uint64_t>(std::strtoull(parts[2].c_str(), nullptr, 10));
        r.typ = parts[3];
        r.fmt = parts[4];
        r.w = (parts[5] == "-") ? -1 : static_cast<int>(std::strtol(parts[5].c_str(), nullptr, 10));
        r.h = (parts[6] == "-") ? -1 : static_cast<int>(std::strtol(parts[6].c_str(), nullptr, 10));
        if (parts[7] != "-") r.tlut = static_cast<std::uint64_t>(std::strtoull(parts[7].c_str(), nullptr, 16));
        rows.push_back(std::move(r));
    }
    return rows;
}

// Baked EK tlut map (Task C): load the build-generated ek_tlut_map.json, a {full_addr(dec str):
// symbol} object emitted by tools/gen_dump_tables_data.py from the SAME EK recipe tree the oracle's
// ek_recipe_index() walks — so this yields the identical full_addr -> palette-symbol map WITHOUT the
// EK yaml source present. Returns empty on any problem (caller falls back / skips EK).
std::map<std::uint64_t, std::string> loadEkTlutMap(const std::string& path) {
    std::map<std::uint64_t, std::string> tlutByAddr;
    if (path.empty() || !fileExists(path)) return tlutByAddr;
    YAML::Node data;
    try {
        data = YAML::LoadFile(path);
    } catch (...) {
        return tlutByAddr;
    }
    if (!data.IsMap()) return tlutByAddr;
    for (const auto& kv : data) {
        std::uint64_t addr = static_cast<std::uint64_t>(
            std::strtoull(kv.first.as<std::string>().c_str(), nullptr, 10));
        tlutByAddr[addr] = kv.second.as<std::string>();
    }
    return tlutByAddr;
}

// EK recipe tlut map (mirrors ek_recipe_index) — full_addr -> palette symbol.
std::map<std::uint64_t, std::string> ekRecipeTlut(const std::string& ekYamlDir) {
    std::map<std::uint64_t, std::string> tlutByAddr;
    for (const std::string& path : listYaml(ekYamlDir, true)) {
        YAML::Node data;
        try {
            data = YAML::LoadFile(path);
        } catch (...) {
            continue;
        }
        bool hasSeg = false;
        int segId = 0;
        if (data[":config"] && data[":config"]["segments"] && data[":config"]["segments"].IsSequence() &&
            data[":config"]["segments"].size() > 0) {
            YAML::Node s0 = data[":config"]["segments"][0];
            if (s0.IsSequence() && s0.size() >= 2) {
                hasSeg = true;
                segId = static_cast<int>(parseIntStr(s0[0].as<std::string>()));
            }
        }
        for (const auto& kv : data) {
            std::string key = kv.first.as<std::string>();
            if (!key.empty() && key[0] == ':') continue;
            YAML::Node val = kv.second;
            if (!val.IsMap()) continue;
            std::string type = nodeStr(val["type"]);
            if (type != "TEXTURE" && type != "COMPRESSED_TEXTURE") continue;
            std::string fmt = nodeStr(val["format"]);
            if (fmt.empty() || !val["offset"]) continue;
            if (fmt == "TLUT") {
                std::uint64_t off = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
                std::uint64_t full = hasSeg ? ((static_cast<std::uint64_t>(segId) << 24) | off) : off;
                tlutByAddr[full] = val["symbol"] ? val["symbol"].as<std::string>() : key;
            }
        }
    }
    return tlutByAddr;
}

// Atlas band geometry for buffers the game loads as one tall strip and samples per-tile
// (devdocs/POST_1_0_SCOPING.md "Per-tile atlas overrides"). key -> (tile_w, tile_h); tiles stack
// vertically, so band i starts at byte offset i * texelBytes(fmt, tile_w * tile_h) — the offset the
// runtime's containing-range lookup keys on. Mirrors ATLAS_TILE_GEOMETRY in tools/gen_dump_all.py.
const std::map<std::string, std::pair<int, int>> kAtlasTileGeometry = {
    {"machine_custom_gfx/aTimerSymbolsTex", {8, 16}},
    {"machine_custom_gfx/aSpeedDigitsTex", {12, 16}},
};

// Emit the scheme-2 "atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H>" rows for an atlas strip: the
// whole-atlas key (o0 at full W×H — the band geometry whole-atlas LoadTextureBlock loads produce)
// plus one sliced PNG + row per band. rgba is the decoded whole-atlas buffer, or empty on the
// skipped-existing path (band PNGs were written by the run that wrote the base; rows are still
// re-emitted because writeManifest regenerates the manifest from scratch each run).
void emitAtlasBands(const TexItem& it, const std::string& outDir, const std::vector<std::uint8_t>& rgba,
                    std::vector<std::vector<std::string>>& manifest, int& atlasDumped) {
    auto git = kAtlasTileGeometry.find(it.key);
    if (git == kAtlasTileGeometry.end()) return;
    const int tileW = git->second.first;
    const int tileH = git->second.second;
    if (it.width != tileW || tileH <= 0 || it.height % tileH != 0) {
        std::fprintf(stderr, "  warn: %s: atlas geometry %dx%d tiles do not tile %dx%d - no band rows\n",
                     it.key.c_str(), tileW, tileH, it.width, it.height);
        return;
    }
    const std::string base = "atlas/" + it.key;
    const std::size_t bandBytes = texelBytes(it.fmt, static_cast<std::size_t>(tileW) * tileH);
    const std::size_t rowStride = static_cast<std::size_t>(it.width) * 4;
    const std::string pal = it.paletteKey.value_or("-");
    struct Band {
        std::size_t off; // byte offset within the atlas buffer (the runtime lookup key)
        int w, h, row0;  // band dims + first source row in the decoded strip
    };
    // The whole-atlas key comes first, then one band per tile.
    std::vector<Band> bands;
    bands.push_back({0, it.width, it.height, 0});
    const int nTiles = it.height / tileH;
    for (int i = 0; i < nTiles; ++i) {
        bands.push_back({static_cast<std::size_t>(i) * bandBytes, tileW, tileH, i * tileH});
    }
    for (const Band& b : bands) {
        char suffix[96];
        std::snprintf(suffix, sizeof(suffix), "/o%zu/%s/%dx%d", b.off, it.fmt.c_str(), b.w, b.h);
        const std::string key = base + suffix;
        manifest.push_back({key, std::to_string(b.w), std::to_string(b.h), it.fmt, pal});
        if (rgba.empty()) continue;
        const std::string png = joinPath(outDir, key + ".png");
        if (fileExists(png)) continue;
        std::vector<std::uint8_t> band(static_cast<std::size_t>(b.w) * b.h * 4);
        for (int y = 0; y < b.h; ++y) {
            std::memcpy(band.data() + static_cast<std::size_t>(y) * b.w * 4,
                        rgba.data() + static_cast<std::size_t>(b.row0 + y) * rowStride,
                        static_cast<std::size_t>(b.w) * 4);
        }
        makeDirs(fs::path(png).parent_path().string());
        if (writePng(png, b.w, b.h, band.data())) ++atlasDumped;
    }
}

} // namespace

ClassResult runTextures(Context& ctx) {
    ClassResult res;
    res.name = "textures";

    std::string outDir = ctx.dumpDir; // textures land at the dump root
    makeDirs(outDir);

    // ── cart ──
    std::vector<TexItem> items = walkCartTextures(ctx.cartYamlDir);
    std::sort(items.begin(), items.end(), [](const TexItem& a, const TexItem& b) { return a.key < b.key; });
    // Every palette a cart CI texture references (deduped): dumped below as Nx1 TLUT swatches,
    // mirroring the EK palette dump, so the packer can quantize CI replacements offline.
    std::set<std::string> cartPal;
    for (const TexItem& it : items) {
        if (it.paletteKey) cartPal.insert(*it.paletteKey);
    }

    std::vector<std::vector<std::string>> manifest; // (key, w, h, fmt, palette_key) in dump order
    int dumped = 0, skipped = 0, failed = 0, atlasDumped = 0;
    for (const TexItem& it : items) {
        if (!safeOutputComponent(it.key)) {
            std::fprintf(stderr, "  warn: textures: unsafe output name '%s'; skipping\n", it.key.c_str());
            ++failed;
            continue;
        }
        std::string png = joinPath(outDir, it.key + ".png");
        if (fileExists(png)) {
            ++skipped;
            manifest.push_back({it.key, std::to_string(it.width), std::to_string(it.height), it.fmt,
                                it.paletteKey.value_or("-")});
            emitAtlasBands(it, outDir, {}, manifest, atlasDumped);
            continue;
        }
        std::vector<std::uint8_t> rgba = decodeCart(it, ctx);
        if (rgba.empty()) {
            std::fprintf(stderr, "  warn: textures %s: decode failed\n", it.key.c_str());
            ++failed;
            continue;
        }
        makeDirs(fs::path(png).parent_path().string());
        if (!writePng(png, it.width, it.height, rgba.data())) {
            ++failed;
            continue;
        }
        ++dumped;
        manifest.push_back({it.key, std::to_string(it.width), std::to_string(it.height), it.fmt,
                            it.paletteKey.value_or("-")});
        emitAtlasBands(it, outDir, rgba, manifest, atlasDumped);
    }
    // ── cart TLUT swatches (mirror of the EK palette loop below) ──
    for (const std::string& key : cartPal) {
        if (!safeOutputComponent(key)) {
            std::fprintf(stderr, "  warn: cart palette: unsafe output name '%s'; skipping\n", key.c_str());
            ++failed;
            continue;
        }
        std::string png = joinPath(outDir, key + ".png");
        auto raw = ctx.cart->readStripped(key, kTexPayloadOffset);
        int n = raw ? static_cast<int>(raw->size() / 2) : 0;
        if (fileExists(png)) {
            ++skipped;
            manifest.push_back({key, std::to_string(n), "1", "TLUT", "-"});
            continue;
        }
        if (!raw || n == 0) {
            ++failed;
            continue;
        }
        std::vector<std::uint8_t> rgba =
            decodeTexel("RGBA16", raw->data(), raw->size(), n, 1, nullptr, 0);
        if (rgba.empty()) {
            ++failed;
            continue;
        }
        makeDirs(fs::path(png).parent_path().string());
        if (!writePng(png, n, 1, rgba.data())) {
            ++failed;
            continue;
        }
        ++dumped;
        manifest.push_back({key, std::to_string(n), "1", "TLUT", "-"});
    }
    std::printf("  textures: %d dumped, %d skipped, %d failed (of %zu)\n", dumped, skipped, failed,
                items.size() + cartPal.size());
    if (atlasDumped > 0) {
        std::printf("  atlas bands: %d per-tile slice(s) written (key scheme 2)\n", atlasDumped);
    }
    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(items.size());

    // ── EK (optional; graceful skip when build artifacts absent) ──
    // TLUT resolution prefers --ek-yaml-dir (override); otherwise the baked ek_tlut_map.json (Task C)
    // lets EK CI4/CI8 palettes resolve with no EK yaml source present.
    bool haveEkYaml = !ctx.ekYamlDir.empty() && dirExists(ctx.ekYamlDir);
    bool haveEkMap = !ctx.ekTlutMapPath.empty() && fileExists(ctx.ekTlutMapPath);
    bool ekReady = ctx.disk != nullptr && !ctx.manifestPath.empty() && fileExists(ctx.manifestPath) &&
                   (haveEkYaml || haveEkMap);
    if (!ekReady) {
        std::printf("  ek textures: EK artifacts missing (disk archive / manifest / tlut map or recipe "
                    "tree) -- skipping EK texture dump\n");
        writeManifest(joinPath(outDir, "manifest.tsv"),
                      "key\tnative_w\tnative_h\tn64_fmt\tpalette_key   (one row per dumped texture; "
                      "palette_key = TLUT swatch key for CI4/CI8, - otherwise)", manifest);
        return res;
    }

    std::map<std::uint64_t, std::string> tlutByAddr =
        haveEkYaml ? ekRecipeTlut(ctx.ekYamlDir) : loadEkTlutMap(ctx.ekTlutMapPath);
    std::vector<EkRow> rows = parseEkManifest(ctx.manifestPath);

    std::vector<TexItem> ekTex;
    std::vector<std::pair<std::string, std::string>> ekPal; // (key, sym)
    for (const EkRow& r : rows) {
        std::string key = "ek/" + r.sym;
        if (r.fmt == "TLUT") {
            ekPal.emplace_back(key, r.sym);
            continue;
        }
        if (!isDecodableFormat(r.fmt)) continue;
        // Renderable rows need real dimensions. A non-positive w/h (corrupt/placeholder manifest row)
        // would feed a bogus texelBytes(w*h) below -- e.g. -1*-1 wraps to a giant size_t. Reject the row.
        if (r.w <= 0 || r.h <= 0) {
            std::fprintf(stderr, "  warn: EK texture %s: non-positive dimensions %dx%d; skipping\n",
                         r.sym.c_str(), r.w, r.h);
            continue;
        }
        TexItem it;
        it.key = key;
        it.sym = r.sym;
        it.fmt = r.fmt;
        it.width = r.w;
        it.height = r.h;
        it.offset = r.off;
        it.compressed = (r.typ == "COMPRESSED_TEXTURE") ? 1 : 0;
        if (r.fmt == "CI4" || r.fmt == "CI8") {
            if (!r.tlut) continue;
            auto pit = tlutByAddr.find(*r.tlut);
            if (pit == tlutByAddr.end()) pit = tlutByAddr.find(*r.tlut & 0x00FFFFFF);
            if (pit == tlutByAddr.end()) continue;
            it.paletteKey = "ek/" + pit->second;
        }
        ekTex.push_back(std::move(it));
    }
    std::sort(ekTex.begin(), ekTex.end(), [](const TexItem& a, const TexItem& b) { return a.key < b.key; });
    std::sort(ekPal.begin(), ekPal.end());

    int ekDumped = 0, ekSkipped = 0, ekFailed = 0;
    for (const TexItem& it : ekTex) {
        if (!safeOutputComponent(it.key)) {
            std::fprintf(stderr, "  warn: EK textures: unsafe output name '%s'; skipping\n", it.key.c_str());
            ++ekFailed;
            continue;
        }
        std::string png = joinPath(outDir, it.key + ".png");
        if (fileExists(png)) {
            ++ekSkipped;
            manifest.push_back({it.key, std::to_string(it.width), std::to_string(it.height), it.fmt,
                                it.paletteKey.value_or("-")});
            continue;
        }
        auto raw = ctx.disk->readRaw(it.key);
        if (!raw) {
            std::fprintf(stderr, "  warn: no EK archive entry for %s\n", it.key.c_str());
            ++ekFailed;
            continue;
        }
        std::vector<std::uint8_t> data;
        if (it.compressed) {
            if (raw->size() < 4 || std::memcmp(raw->data(), "MIO0", 4) != 0) {
                std::fprintf(stderr, "  warn: %s: not a MIO0 blob\n", it.key.c_str());
                ++ekFailed;
                continue;
            }
            data = mio0Decompress(raw->data(), raw->size());
        } else {
            data = std::move(*raw);
        }
        std::size_t need = texelBytes(it.fmt, static_cast<std::size_t>(it.width) * it.height);
        if (data.size() < need) {
            std::fprintf(stderr, "  warn: %s: payload short\n", it.key.c_str());
            ++ekFailed;
            continue;
        }
        const std::uint8_t* pal = nullptr;
        std::vector<std::uint8_t> palBuf;
        if (it.paletteKey) {
            auto p = ctx.disk->readRaw(*it.paletteKey);
            if (!p) {
                ++ekFailed;
                continue;
            }
            palBuf = std::move(*p);
            pal = palBuf.data();
        }
        std::vector<std::uint8_t> rgba =
            decodeTexel(it.fmt, data.data(), data.size(), it.width, it.height, pal, palBuf.size());
        if (rgba.empty()) {
            ++ekFailed;
            continue;
        }
        makeDirs(fs::path(png).parent_path().string());
        if (!writePng(png, it.width, it.height, rgba.data())) {
            ++ekFailed;
            continue;
        }
        ++ekDumped;
        manifest.push_back({it.key, std::to_string(it.width), std::to_string(it.height), it.fmt,
                            it.paletteKey.value_or("-")});
    }
    for (const auto& kp : ekPal) {
        const std::string& key = kp.first;
        if (!safeOutputComponent(key)) {
            std::fprintf(stderr, "  warn: EK palette: unsafe output name '%s'; skipping\n", key.c_str());
            ++ekFailed;
            continue;
        }
        std::string png = joinPath(outDir, key + ".png");
        auto raw = ctx.disk->readRaw(key);
        int n = raw ? static_cast<int>(raw->size() / 2) : 0;
        if (fileExists(png)) {
            ++ekSkipped;
            manifest.push_back({key, std::to_string(n), "1", "TLUT", "-"});
            continue;
        }
        if (!raw || n == 0) {
            ++ekFailed;
            continue;
        }
        std::vector<std::uint8_t> rgba =
            decodeTexel("RGBA16", raw->data(), raw->size(), n, 1, nullptr, 0);
        if (rgba.empty()) {
            ++ekFailed;
            continue;
        }
        makeDirs(fs::path(png).parent_path().string());
        if (!writePng(png, n, 1, rgba.data())) {
            ++ekFailed;
            continue;
        }
        ++ekDumped;
        manifest.push_back({key, std::to_string(n), "1", "TLUT", "-"});
    }
    std::printf("  ek textures: %d dumped, %d skipped, %d failed (of %zu)\n", ekDumped, ekSkipped,
                ekFailed, ekTex.size() + ekPal.size());

    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "key\tnative_w\tnative_h\tn64_fmt\tpalette_key   (one row per dumped texture; "
                  "palette_key = TLUT swatch key for CI4/CI8, - otherwise)", manifest);

    res.dumped += ekDumped;
    res.skipped += ekSkipped;
    res.failed += ekFailed;
    res.total += static_cast<int>(ekTex.size() + ekPal.size());
    return res;
}

} // namespace gdxdump
