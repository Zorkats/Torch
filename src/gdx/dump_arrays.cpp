// gdx-extract `dump` class: ARRAY recipe entries (Torch::ResourceType::GenericArray, 'GARR').
//
// Every other dump class walks recipes filtered to a type it understands -- GFX, VTX, TEXTURE,
// BLOB.  ARRAY was never one of them, so the 62 ARRAY entries in the US cart recipes had no dump
// path at all and silently never appeared under dump/.  38 of those are the ending-cutscene
// character firework masks; the rest are s16 lookup tables.  Their bytes were in the archive the
// whole time, which is why the gap read as "assets missing" rather than "class missing".
//
// Entry layout, matching ArrayBinaryExporter::Export (torch/src/factories/GenericArrayFactory.cpp)
// and the port's ResourceFactoryBinaryGenericArrayV0 (port/resource/ResourceFactories.cpp):
//
//     [0x00] 64-byte libultraship ResourceInitData header (type 'GARR' at +4, version at +8)
//     [0x40] u32 arrayType   -- index into the ArrayType enum, NOT a size
//     [0x44] u32 count       -- element count
//     [0x48] payload         -- count * elementSize bytes
//
// Both the Torch writer and the LUS reader leave BinaryWriter/BinaryReader at Endianness::Native,
// so on an x86 host every field above (and every multi-byte payload element) is little-endian.
// The dumped .bin is therefore host-native element order, not raw big-endian ROM order -- fine for
// inspection and for feeding back through the same reader, wrong if diffed against a raw ROM slice.

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gdxdump {
namespace {

// Mirrors Torch's `enum class ArrayType` (GenericArrayFactory.h) -- the wire value is the enum
// index, so this table's ORDER is the contract, not just its contents. The port's factory switches
// over the same indices.
struct ArrayKind {
    const char* name;
    std::uint32_t code;
    std::size_t elemSize;
};
constexpr ArrayKind kArrayKinds[] = {
    { "u8", 0, 1 },      { "s8", 1, 1 },     { "u16", 2, 2 },    { "s16", 3, 2 },
    { "u32", 4, 4 },     { "s32", 5, 4 },    { "u64", 6, 8 },    { "f32", 7, 4 },
    { "f64", 8, 8 },     { "Vec2f", 9, 8 },  { "Vec3f", 10, 12 }, { "Vec3s", 11, 6 },
    { "Vec3i", 12, 12 }, { "Vec3iu", 13, 12 }, { "Vec4f", 14, 16 }, { "Vec4s", 15, 8 },
};

const ArrayKind* kindByName(const std::string& name) {
    for (const ArrayKind& k : kArrayKinds) {
        if (name == k.name) return &k;
    }
    return nullptr;
}

std::uint32_t leU32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string hexU(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string jStr(const std::string& s) { return "\"" + s + "\""; }

struct ArrayItem {
    std::string yamlStem;
    std::string sym;
    const ArrayKind* kind = nullptr;
    long long count = 0;
    std::uint64_t offset = 0;   // segment-relative, for the manifest only
};

// ARRAY entries live in ordinary recipe yamls alongside every other type, so this walks the same
// tree walkSegmentItems() does -- but it deliberately does NOT require a `:config: segments:`
// block, because an ARRAY is addressed by archive key, never by segment+offset.
std::vector<ArrayItem> walkArrayItems(const std::string& yamlDir, bool recursive) {
    std::vector<ArrayItem> out;
    for (const std::string& path : listYaml(yamlDir, recursive)) {
        if (isBlobRecipeFilename(fs::path(path).filename().string())) continue;
        YAML::Node data;
        try {
            data = YAML::LoadFile(path);
        } catch (...) {
            continue;
        }
        std::string stem = fs::path(path).stem().string();
        for (const auto& kv : data) {
            std::string key = kv.first.as<std::string>();
            if (!key.empty() && key[0] == ':') continue;
            YAML::Node val = kv.second;
            if (!val.IsMap()) continue;
            if (!val["type"] || val["type"].as<std::string>() != "ARRAY") continue;
            ArrayItem it;
            it.yamlStem = stem;
            it.sym = val["symbol"] ? val["symbol"].as<std::string>() : key;
            it.kind = val["array_type"] ? kindByName(val["array_type"].as<std::string>()) : nullptr;
            it.count = val["count"] ? parseIntStr(val["count"].as<std::string>()) : -1;
            it.offset = val["offset"]
                            ? static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()))
                            : 0;
            out.push_back(std::move(it));
        }
    }
    std::sort(out.begin(), out.end(), [](const ArrayItem& a, const ArrayItem& b) {
        if (a.yamlStem != b.yamlStem) return a.yamlStem < b.yamlStem;
        return a.sym < b.sym;
    });
    return out;
}

// The ending cutscene's character firework masks are the only u8[512] arrays in the recipes, and
// they are 1-bit 64x64 bitmaps: ending_effects.c derives the launch vector from
// `var_s1 & 0x3F` (x) and `var_s1 >> 6` (y) while scanning bit `var_s1` of the buffer, and
// sFireworksBitMask is { 1<<7 ... 1<<0 }, i.e. MSB-first within each byte. 64*64 bits = 512 bytes.
// A silhouette PNG is the whole point of dumping these -- a .bin of a bitmask tells you nothing.
constexpr std::size_t kFireworkMaskBytes = 512;
constexpr int kFireworkMaskDim = 64;

bool writeMaskPng(const std::string& path, const std::uint8_t* bits) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kFireworkMaskDim) * kFireworkMaskDim * 4, 0);
    for (int i = 0; i < kFireworkMaskDim * kFireworkMaskDim; ++i) {
        const bool set = (bits[i >> 3] & (0x80u >> (i & 7))) != 0;
        const std::size_t k = static_cast<std::size_t>(i) * 4;
        rgba[k] = rgba[k + 1] = rgba[k + 2] = set ? 0xFF : 0x00;
        rgba[k + 3] = 0xFF;
    }
    return writePng(path, kFireworkMaskDim, kFireworkMaskDim, rgba.data());
}

} // namespace

ClassResult runArrays(Context& ctx) {
    ClassResult res;
    res.name = "arrays";

    std::vector<ArrayItem> items = walkArrayItems(ctx.cartYamlDir, false);
    if (!ctx.ekYamlDir.empty()) {
        std::vector<ArrayItem> ek = walkArrayItems(ctx.ekYamlDir, true);
        items.insert(items.end(), ek.begin(), ek.end());
    }

    std::string outDir = joinPath(ctx.dumpDir, "arrays");
    makeDirs(outDir);

    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0, previews = 0;

    for (const ArrayItem& it : items) {
        const std::string k = it.yamlStem + "_" + it.sym;
        if (!safeOutputComponent(k)) {
            std::fprintf(stderr, "  warn: arrays: unsafe output name '%s'; skipping\n", k.c_str());
            ++failed;
            continue;
        }
        if (it.kind == nullptr || it.count <= 0) {
            std::fprintf(stderr, "  warn: arrays %s: recipe has no usable array_type/count; skipping\n",
                         k.c_str());
            ++failed;
            continue;
        }

        const std::string binPath = joinPath(outDir, k + ".bin");
        const std::string jsonPath = joinPath(outDir, k + ".json");
        if (fileExists(binPath) && fileExists(jsonPath)) {
            ++skipped;
            manifest.push_back({ k, it.yamlStem, it.sym, it.kind->name, std::to_string(it.count),
                                 hexU(it.offset), "skip-existing" });
            continue;
        }

        // ARRAY entries are looked up by the same "<yaml stem>/<symbol>" key the game uses.
        const std::string archiveKey = it.yamlStem + "/" + it.sym;
        const Archive* src = nullptr;
        if (ctx.cart != nullptr && ctx.cart->has(archiveKey)) {
            src = ctx.cart;
        } else if (ctx.disk != nullptr && ctx.disk->has(archiveKey)) {
            src = ctx.disk;
        }
        if (src == nullptr) {
            std::fprintf(stderr, "  warn: arrays %s: '%s' not present in cart or disk archive\n",
                         k.c_str(), archiveKey.c_str());
            ++failed;
            continue;
        }

        auto rawOpt = src->readRaw(archiveKey);
        if (!rawOpt || rawOpt->size() < kArrayPayloadOffset) {
            std::fprintf(stderr, "  warn: arrays %s: entry shorter than its %zu-byte header\n", k.c_str(),
                         kArrayPayloadOffset);
            ++failed;
            continue;
        }
        const std::vector<std::uint8_t>& raw = *rawOpt;

        const std::uint32_t magic = leU32(raw.data() + 4);
        if (magic != kResourceTypeGenericArray) {
            std::fprintf(stderr, "  warn: arrays %s: resource type %s, expected 'GARR' (%s)\n", k.c_str(),
                         hexU(magic).c_str(), hexU(kResourceTypeGenericArray).c_str());
            ++failed;
            continue;
        }

        // The sub-header is the authority on how to read the payload; the recipe is what the port's
        // generated bindings sized their destination buffer from. A disagreement means the archive and
        // the recipes were built from different trees, which is exactly the kind of drift that shows up
        // in-game as a wrong-looking asset rather than a load failure -- so it fails loudly here.
        const std::uint32_t archType = leU32(raw.data() + kOtrHeaderSize);
        const std::uint32_t archCount = leU32(raw.data() + kOtrHeaderSize + 4);
        if (archType != it.kind->code) {
            std::fprintf(stderr, "  warn: arrays %s: archive array_type %u, recipe says %s (%u)\n",
                         k.c_str(), archType, it.kind->name, it.kind->code);
            ++failed;
            continue;
        }
        if (archCount != static_cast<std::uint32_t>(it.count)) {
            std::fprintf(stderr, "  warn: arrays %s: archive count %u, recipe count %lld\n", k.c_str(),
                         archCount, it.count);
            ++failed;
            continue;
        }

        const std::size_t payloadBytes = static_cast<std::size_t>(archCount) * it.kind->elemSize;
        if (raw.size() < kArrayPayloadOffset + payloadBytes) {
            std::fprintf(stderr, "  warn: arrays %s: short payload (%zu bytes present, %zu needed)\n",
                         k.c_str(), raw.size() - kArrayPayloadOffset, payloadBytes);
            ++failed;
            continue;
        }
        const std::uint8_t* payload = raw.data() + kArrayPayloadOffset;

        if (!writeFileBytes(binPath, payload, payloadBytes)) {
            std::fprintf(stderr, "  warn: arrays %s: could not write %s\n", k.c_str(), binPath.c_str());
            ++failed;
            continue;
        }

        bool wrotePng = false;
        if (it.kind->code == 0 && payloadBytes == kFireworkMaskBytes) {
            const std::string pngPath = joinPath(outDir, k + ".png");
            wrotePng = writeMaskPng(pngPath, payload);
            if (wrotePng) ++previews;
        }

        std::string j = "{";
        j += "\"symbol\":" + jStr(it.sym);
        j += ",\"yaml\":" + jStr(it.yamlStem);
        j += ",\"archiveKey\":" + jStr(archiveKey);
        j += ",\"arrayType\":" + jStr(it.kind->name);
        j += ",\"count\":" + std::to_string(archCount);
        j += ",\"elementBytes\":" + std::to_string(it.kind->elemSize);
        j += ",\"bytes\":" + std::to_string(payloadBytes);
        j += ",\"recipeOffset\":" + jStr(hexU(it.offset));
        j += ",\"byteOrder\":" + jStr("host-native (little-endian on x86)");
        if (wrotePng) {
            j += ",\"preview\":" + jStr(k + ".png");
            j += ",\"previewFormat\":" + jStr("1bpp 64x64, MSB-first (character firework mask)");
        }
        j += "}";
        writeFileText(jsonPath, j);

        ++dumped;
        manifest.push_back({ k, it.yamlStem, it.sym, it.kind->name, std::to_string(archCount),
                             hexU(it.offset), wrotePng ? "ok+png" : "ok" });
    }

    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "key\tyaml\tsymbol\tarrayType\tcount\trecipeOffset\tstatus   (GARR payload at 0x48, "
                  "host-native element order)",
                  manifest);
    std::printf("  arrays: %d dumped, %d skipped, %d failed (of %zu ARRAY recipe entries)\n", dumped,
                skipped, failed, items.size());
    std::printf("  arrays: %d firework-mask PNG preview(s) written\n", previews);

    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(items.size());
    return res;
}

} // namespace gdxdump
