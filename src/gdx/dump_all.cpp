// gdx-extract `dump` subcommand implementation. See dump_all.h for the contract and dump_common.h
// for the shared internals. Wave 1 classes live in dump_textures.cpp / dump_extra.cpp.

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

// zip_file.hpp is a header-only amalgamation: lines 977-4945 are the miniz C implementation,
// guarded by MINIZ_HEADER_FILE_ONLY. src/archive/ZWrapper.cpp is the one translation unit that
// compiles that implementation; defining the macro here takes the declarations only, so the two
// TUs do not each emit mz_* definitions (GNU ld: "multiple definition of
// `mz_zip_extract_archive_file_to_heap'"). MSVC tolerated the duplicates, so this only ever
// surfaced on the Linux build.
#define MINIZ_HEADER_FILE_ONLY
#include <miniz/zip_file.hpp>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

extern "C" {
#include "libmio0/mio0.h"
// n64graphics stb_image_write (compiled in lib/n64graphics/n64graphics.c, C linkage).
int stbi_write_png(char const* filename, int w, int h, int comp, const void* data,
                   int stride_in_bytes);
}

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace gdxdump {

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// Archive (miniz_cpp read wrapper)
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
Archive::Archive(const std::string& path) {
    if (!fileExists(path)) {
        return;
    }
    try {
        mZip = new miniz_cpp::zip_file(path);
        mNames = mZip->namelist();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  dump: failed to open archive %s (%s)\n", path.c_str(), e.what());
        delete mZip;
        mZip = nullptr;
    }
}

Archive::~Archive() { delete mZip; }

bool Archive::has(const std::string& key) const {
    return std::find(mNames.begin(), mNames.end(), key) != mNames.end();
}

std::vector<std::string> Archive::names() const { return mNames; }

std::optional<std::vector<std::uint8_t>> Archive::readRaw(const std::string& key) const {
    if (mZip == nullptr || !has(key)) {
        return std::nullopt;
    }
    std::string s = mZip->read(key);
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

std::optional<std::vector<std::uint8_t>> Archive::readStripped(const std::string& key,
                                                              std::size_t strip) const {
    auto raw = readRaw(key);
    if (!raw || raw->size() < strip) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(raw->begin() + static_cast<std::ptrdiff_t>(strip), raw->end());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// MIO0
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
std::vector<std::uint8_t> mio0Decompress(const std::uint8_t* data, std::size_t len) {
    if (len < MIO0_HEADER_LENGTH || std::memcmp(data, "MIO0", 4) != 0) {
        return {};
    }
    mio0_header_t head;
    if (!mio0_decode_header(data, &head)) {
        return {};
    }
    std::vector<std::uint8_t> out(head.dest_size);
    unsigned int end = 0;
    int n = mio0_decode(data, out.data(), &end);
    if (n < 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// N64 texel decoders (mirror gen_dump_all.py DECODERS exactly)
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
namespace {
inline int scale_5_8(int v) { return (v * 0xFF) / 0x1F; }
inline int scale_4_8(int v) { return v * 0x11; }
inline int scale_3_8(int v) { return v * 0x24; }

void decodeCiPixel(std::uint8_t* out, int o, int col16) {
    int a = col16 & 1;
    out[o] = static_cast<std::uint8_t>(scale_5_8(col16 >> 11));
    out[o + 1] = static_cast<std::uint8_t>(scale_5_8((col16 >> 6) & 0x1F));
    out[o + 2] = static_cast<std::uint8_t>(scale_5_8((col16 >> 1) & 0x1F));
    out[o + 3] = a ? 255 : 0;
}
} // namespace

int bitsPerTexel(const std::string& fmt) {
    if (fmt == "RGBA16" || fmt == "IA16" || fmt == "TLUT") return 16;
    if (fmt == "RGBA32") return 32;
    if (fmt == "IA8" || fmt == "I8" || fmt == "CI8") return 8;
    if (fmt == "IA4" || fmt == "I4" || fmt == "CI4") return 4;
    return 0;
}

std::size_t texelBytes(const std::string& fmt, std::size_t count) {
    int bpt = bitsPerTexel(fmt);
    if (bpt == 0) return 0;
    return (count * static_cast<std::size_t>(bpt) + 7) / 8;
}

bool isDecodableFormat(const std::string& fmt) {
    return fmt == "RGBA16" || fmt == "RGBA32" || fmt == "IA16" || fmt == "IA8" || fmt == "IA4" ||
           fmt == "I8" || fmt == "I4" || fmt == "CI4" || fmt == "CI8";
}

std::vector<std::uint8_t> decodeTexel(const std::string& fmt, const std::uint8_t* p,
                                      std::size_t plen, int w, int h, const std::uint8_t* pal,
                                      std::size_t palLen) {
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (texelBytes(fmt, n) > plen) {
        return {};
    }
    std::vector<std::uint8_t> out(n * 4, 0);
    std::uint8_t* o = out.data();
    if (fmt == "RGBA16") {
        for (std::size_t i = 0; i < n; ++i) {
            int c = (p[2 * i] << 8) | p[2 * i + 1];
            int a = c & 1;
            std::size_t k = 4 * i;
            o[k] = static_cast<std::uint8_t>(scale_5_8(c >> 11));
            o[k + 1] = static_cast<std::uint8_t>(scale_5_8((c >> 6) & 0x1F));
            o[k + 2] = static_cast<std::uint8_t>(scale_5_8((c >> 1) & 0x1F));
            o[k + 3] = a ? 255 : 0;
        }
    } else if (fmt == "RGBA32") {
        std::memcpy(o, p, n * 4);
    } else if (fmt == "IA16") {
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t intensity = p[2 * i];
            std::uint8_t alpha = p[2 * i + 1];
            std::size_t k = 4 * i;
            o[k] = o[k + 1] = o[k + 2] = intensity;
            o[k + 3] = alpha;
        }
    } else if (fmt == "IA8") {
        for (std::size_t i = 0; i < n; ++i) {
            int byte = p[i];
            int intensity = scale_4_8(byte >> 4);
            int alpha = scale_4_8(byte & 0xF);
            std::size_t k = 4 * i;
            o[k] = o[k + 1] = o[k + 2] = static_cast<std::uint8_t>(intensity);
            o[k + 3] = static_cast<std::uint8_t>(alpha);
        }
    } else if (fmt == "IA4") {
        for (std::size_t i = 0; i < n; ++i) {
            int byte = p[i / 2];
            int part = (byte >> (4 - (i % 2) * 4)) & 0xF;
            int intensity = scale_3_8(part >> 1);
            int alpha = part & 1;
            std::size_t k = 4 * i;
            o[k] = o[k + 1] = o[k + 2] = static_cast<std::uint8_t>(intensity);
            o[k + 3] = alpha ? 255 : 0;
        }
    } else if (fmt == "I8") {
        for (std::size_t i = 0; i < n; ++i) {
            std::uint8_t v = p[i];
            std::size_t k = 4 * i;
            o[k] = o[k + 1] = o[k + 2] = o[k + 3] = v;
        }
    } else if (fmt == "I4") {
        for (std::size_t i = 0; i < n; ++i) {
            int byte = p[i / 2];
            int part = (byte >> (4 - (i % 2) * 4)) & 0xF;
            std::uint8_t v = static_cast<std::uint8_t>(scale_4_8(part));
            std::size_t k = 4 * i;
            o[k] = o[k + 1] = o[k + 2] = o[k + 3] = v;
        }
    } else if (fmt == "CI4") {
        if (pal == nullptr) return {};
        for (std::size_t i = 0; i < n; ++i) {
            int byte = p[i / 2];
            int idx = (byte >> (4 - (i % 2) * 4)) & 0xF;
            if (static_cast<std::size_t>(idx * 2 + 1) >= palLen) return {};
            int col16 = (pal[idx * 2] << 8) | pal[idx * 2 + 1];
            decodeCiPixel(o, static_cast<int>(4 * i), col16);
        }
    } else if (fmt == "CI8") {
        if (pal == nullptr) return {};
        for (std::size_t i = 0; i < n; ++i) {
            int idx = p[i];
            if (static_cast<std::size_t>(idx * 2 + 1) >= palLen) return {};
            int col16 = (pal[idx * 2] << 8) | pal[idx * 2 + 1];
            decodeCiPixel(o, static_cast<int>(4 * i), col16);
        }
    } else {
        return {};
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// PNG
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
bool writePng(const std::string& path, int w, int h, const std::uint8_t* rgba) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// Filesystem helpers
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}
bool dirExists(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}
void makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}
std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return (fs::path(a) / b).string();
}
bool safeOutputComponent(const std::string& component) {
    if (component.empty()) return false;
    // Absolute / drive-rooted: a leading path separator, or a drive-colon anywhere (C:, and NTFS ADS
    // "name:stream" both use ':'). Windows treats either as an escape from the output directory.
    if (component[0] == '/' || component[0] == '\\') return false;
    if (component.find(':') != std::string::npos) return false;
    // Parent-directory escape: any ".." token. A plain substring test is deliberately conservative --
    // legitimate archive keys/symbols never contain "..".
    if (component.find("..") != std::string::npos) return false;
    return true;
}
std::vector<std::string> listYaml(const std::string& dir, bool recursive) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!dirExists(dir)) return out;
    if (recursive) {
        for (auto& e : fs::recursive_directory_iterator(dir, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".yaml") out.push_back(e.path().string());
        }
    } else {
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".yaml") out.push_back(e.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}
std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
bool writeFileBytes(const std::string& path, const std::uint8_t* data, std::size_t len) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    return out.good();
}
bool writeFileText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}
void writeManifest(const std::string& path, const std::string& header,
                   const std::vector<std::vector<std::string>>& rows) {
    std::string s = "# " + header + "\n";
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i) s += "\t";
            s += row[i];
        }
        s += "\n";
    }
    writeFileText(path, s);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// yaml helpers
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
long long parseIntStr(const std::string& s) {
    if (s.empty()) return 0;
    return std::strtoll(s.c_str(), nullptr, 0);
}

bool isBlobRecipeFilename(const std::string& fname) {
    return fname == "segment_blob.yaml" || fname == "audio_blob.yaml";
}

// Reads a :config: segments: block: sets segId/romBase, returns true if present.
static bool readSegments(const YAML::Node& data, int& segId, std::uint64_t& romBase) {
    if (!data[":config"]) return false;
    YAML::Node cfg = data[":config"];
    if (!cfg["segments"] || !cfg["segments"].IsSequence() || cfg["segments"].size() == 0)
        return false;
    YAML::Node s0 = cfg["segments"][0];
    if (!s0.IsSequence() || s0.size() < 2) return false;
    segId = static_cast<int>(parseIntStr(s0[0].as<std::string>()));
    romBase = static_cast<std::uint64_t>(parseIntStr(s0[1].as<std::string>()));
    return true;
}

std::vector<SegmentItem> walkSegmentItems(const std::string& cartYamlDir,
                                          const std::string& wantedType) {
    std::vector<SegmentItem> out;
    for (const std::string& path : listYaml(cartYamlDir, false)) {
        std::string fname = fs::path(path).filename().string();
        if (isBlobRecipeFilename(fname)) continue;
        YAML::Node data;
        try {
            data = YAML::LoadFile(path);
        } catch (...) {
            continue;
        }
        int segId = 0;
        std::uint64_t romBase = 0;
        if (!readSegments(data, segId, romBase)) continue;
        std::string stem = fs::path(path).stem().string();
        for (const auto& kv : data) {
            std::string key = kv.first.as<std::string>();
            if (!key.empty() && key[0] == ':') continue;
            YAML::Node val = kv.second;
            if (!val.IsMap()) continue;
            if (!val["type"] || val["type"].as<std::string>() != wantedType) continue;
            if (!val["offset"]) continue;
            SegmentItem it;
            it.yamlStem = stem;
            it.sym = val["symbol"] ? val["symbol"].as<std::string>() : key;
            it.offset = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
            it.count = val["count"] ? parseIntStr(val["count"].as<std::string>()) : -1;
            it.segmentId = segId;
            it.romBase = romBase;
            out.push_back(std::move(it));
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// RomByteProvider
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
RomByteProvider::RomByteProvider(const std::string& cartYamlDir, const Archive& archive,
                                 std::vector<std::uint8_t> rom)
    : mRom(std::move(rom)) {
    // Build the containment map from segment_blob.yaml (single source of truth: the same yaml the
    // generator consumes to build sSegmentBlobMap). Each entry: [offset, offset+size) -> archive key
    // segment_blob/<name>, whose payload (at 0x44) is the verbatim ROM slice.
    std::string sbPath = joinPath(cartYamlDir, "segment_blob.yaml");
    YAML::Node data;
    try {
        data = YAML::LoadFile(sbPath);
    } catch (...) {
        return;
    }
    for (const auto& kv : data) {
        std::string name = kv.first.as<std::string>();
        if (!name.empty() && name[0] == ':') continue;
        YAML::Node val = kv.second;
        if (!val.IsMap() || !val["type"] || val["type"].as<std::string>() != "BLOB") continue;
        if (!val["offset"] || !val["size"]) continue;
        std::uint64_t off = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
        std::uint64_t size = static_cast<std::uint64_t>(parseIntStr(val["size"].as<std::string>()));
        auto payload = archive.readStripped("segment_blob/" + name, kBlobPayloadOffset);
        if (!payload) continue; // not carried by this archive; ROM fallback still applies
        // The blob payload length is authoritative for what is actually servable.
        std::uint64_t len = std::min<std::uint64_t>(size, payload->size());
        mBlobs.push_back(Blob{off, off + len, std::move(*payload)});
    }
    std::sort(mBlobs.begin(), mBlobs.end(),
              [](const Blob& a, const Blob& b) { return a.start < b.start; });
}

std::optional<std::uint8_t> RomByteProvider::byteAt(std::uint64_t addr, bool* viaRom) const {
    // Binary search for a blob containing addr.
    auto it = std::upper_bound(mBlobs.begin(), mBlobs.end(), addr,
                               [](std::uint64_t a, const Blob& b) { return a < b.start; });
    if (it != mBlobs.begin()) {
        --it;
        if (addr >= it->start && addr < it->end) {
            return it->data[static_cast<std::size_t>(addr - it->start)];
        }
    }
    if (!mRom.empty() && addr < mRom.size()) {
        if (viaRom) *viaRom = true;
        return mRom[static_cast<std::size_t>(addr)];
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> RomByteProvider::read(std::uint64_t addr, std::size_t n,
                                                               bool* viaRom) const {
    std::vector<std::uint8_t> out;
    out.reserve(n);
    bool rom = false;
    for (std::size_t i = 0; i < n; ++i) {
        auto b = byteAt(addr + i, &rom);
        if (!b) return std::nullopt;
        out.push_back(*b);
    }
    if (viaRom) *viaRom = rom;
    return out;
}

std::vector<std::uint8_t> RomByteProvider::readUpTo(std::uint64_t addr, std::size_t n,
                                                    bool* viaRom) const {
    std::vector<std::uint8_t> out;
    out.reserve(n);
    bool rom = false;
    for (std::size_t i = 0; i < n; ++i) {
        auto b = byteAt(addr + i, &rom);
        if (!b) break;
        out.push_back(*b);
    }
    if (viaRom) *viaRom = rom;
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// Path auto-discovery + orchestration
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
namespace {

std::string exeDir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(std::string(buf, n)).parent_path().string();
    }
#elif defined(__linux__)
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path().string();
#endif
    return "";
}

// First existing candidate path, or "".
std::string firstExisting(const std::vector<std::string>& cands, bool wantDir) {
    for (const auto& c : cands) {
        if (wantDir ? dirExists(c) : fileExists(c)) return c;
    }
    return "";
}

// Discover a file by basename next to the exe, then in the CWD.
std::string discoverFile(const std::string& explicitPath, const std::string& basename) {
    if (!explicitPath.empty()) return explicitPath;
    std::vector<std::string> cands;
    std::string ed = exeDir();
    if (!ed.empty()) cands.push_back(joinPath(ed, basename));
    cands.push_back(joinPath(fs::current_path().string(), basename));
    return firstExisting(cands, false);
}

std::string discoverDir(const std::string& explicitPath, const std::string& basename) {
    if (!explicitPath.empty()) return explicitPath;
    std::vector<std::string> cands;
    std::string ed = exeDir();
    if (!ed.empty()) cands.push_back(joinPath(ed, basename));
    cands.push_back(joinPath(fs::current_path().string(), basename));
    return firstExisting(cands, true);
}

// The registry: name -> runner. Ordered so --list-classes prints alphabetically.
const std::vector<std::pair<std::string, std::function<ClassResult(Context&)>>>& registry() {
    static const std::vector<std::pair<std::string, std::function<ClassResult(Context&)>>> r = {
        {"arrays", runArrays},         {"audio", runAudio},           {"coursedata", runCourseData},
        {"dlists", runDlists},         {"fonts", runFonts},           {"ghosts", runGhosts},
        {"midi", runMidi},             {"models", runModels},         {"tables", runTables},
        {"textures", runTextures},     {"vertexdata", runVertexData},
    };
    return r;
}

} // namespace

} // namespace gdxdump

int GdxDumpListClasses() {
    for (const auto& e : gdxdump::registry()) {
        std::printf("%s\n", e.first.c_str());
    }
    return 0;
}

int GdxRunDumpAll(const DumpOptions& opts) {
    using namespace gdxdump;

    if (opts.listClasses) {
        return GdxDumpListClasses();
    }
    if (opts.dumpDir.empty()) {
        std::fprintf(stderr, "dump: --dump-dir/-d is required\n");
        return 2;
    }

    // Validate requested classes up front.
    std::map<std::string, std::function<ClassResult(Context&)>> reg;
    for (const auto& e : registry()) reg[e.first] = e.second;
    for (const auto& c : opts.classes) {
        if (reg.find(c) == reg.end()) {
            std::fprintf(stderr, "dump: unknown class '%s'\n", c.c_str());
            return 2;
        }
    }
    if (opts.classes.empty()) {
        std::fprintf(stderr, "dump: no classes requested (use --classes a,b,...)\n");
        return 2;
    }

    // Resolve source paths (explicit flag, then next-to-exe, then CWD). Auto-discovery tries the
    // deployed cart archive name (fzerox.o2r) first, then the dev-tree default (generic.o2r). There is
    // deliberately NO other fallback name: an unrecognized archive must fail fast rather than be picked
    // up by accident.
    std::string archivePath = discoverFile(opts.archivePath, "fzerox.o2r");
    if (archivePath.empty()) archivePath = discoverFile(opts.archivePath, "generic.o2r");
    if (archivePath.empty()) {
        std::fprintf(stderr,
                     "dump: no cart archive found (looked for 'fzerox.o2r' then 'generic.o2r' next to "
                     "the executable and in the working directory). Pass --archive <path>.\n");
        return 2;
    }
    std::string diskPath = discoverFile(opts.diskArchivePath, "fzerox-disk.o2r");
    std::string iplPath = discoverFile(opts.iplArchivePath, "n64ddipl.o2r");
    std::string recipesDir = discoverDir(opts.recipesDir, "decomp-recipes");

    // The cart yaml dir lives under the recipes dir as assets/yaml/us/rev0.
    std::string cartYamlDir;
    if (!recipesDir.empty()) cartYamlDir = joinPath(recipesDir, "assets/yaml/us/rev0");

    // EK manifest: explicit flag, else <recipes>/ek_slice_manifest.txt.
    std::string manifestPath = opts.manifestPath;
    if (manifestPath.empty() && !recipesDir.empty()) {
        std::string p = joinPath(recipesDir, "ek_slice_manifest.txt");
        if (fileExists(p)) manifestPath = p;
    }
    // Tables data (generated at build time into recipes): <recipes>/audio_tables_data.txt.
    std::string tablesDataPath;
    if (!recipesDir.empty()) {
        std::string p = joinPath(recipesDir, "audio_tables_data.json");
        if (fileExists(p)) tablesDataPath = p;
    }
    // EK recipe yaml tree (CI8->TLUT palette resolution).
    std::string ekYamlDir = opts.ekYamlDir;
    if (ekYamlDir.empty() && !recipesDir.empty()) {
        std::string p = joinPath(recipesDir, "ek-yaml");
        if (dirExists(p)) ekYamlDir = p;
    }
    // Baked EK TLUT-resolution map (Task C): lets the textures class resolve EK CI4/CI8 palettes with
    // no --ek-yaml-dir. Generated into recipes at build by tools/gen_dump_tables_data.py.
    std::string ekTlutMapPath;
    if (!recipesDir.empty()) {
        std::string p = joinPath(recipesDir, "ek_tlut_map.json");
        if (fileExists(p)) ekTlutMapPath = p;
    }

    if (cartYamlDir.empty() || !dirExists(cartYamlDir)) {
        std::fprintf(stderr,
                     "dump: could not locate the cart recipe tree (assets/yaml/us/rev0). Pass "
                     "--recipes <decomp-recipes dir>.\n");
        return 2;
    }

    Archive cart(archivePath);
    if (!cart.ok()) {
        std::fprintf(stderr, "dump: could not open cart archive '%s'. Pass --archive.\n",
                     archivePath.empty() ? "(none found)" : archivePath.c_str());
        return 2;
    }
    Archive disk(diskPath); // optional; classes handle absence
    Archive ipl(iplPath);   // optional

    // Build the archive-first ROM byte provider (segment_blob containment + optional raw ROM).
    std::vector<std::uint8_t> romBytes;
    if (!opts.romPath.empty() && fileExists(opts.romPath)) {
        std::string s = readFile(opts.romPath);
        romBytes.assign(s.begin(), s.end());
    }
    RomByteProvider romProvider(cartYamlDir, cart, std::move(romBytes));

    Context ctx;
    ctx.opts = &opts;
    ctx.cartYamlDir = cartYamlDir;
    ctx.ekYamlDir = ekYamlDir;
    ctx.manifestPath = manifestPath;
    ctx.tablesDataPath = tablesDataPath;
    ctx.ekTlutMapPath = ekTlutMapPath;
    ctx.cart = &cart;
    ctx.disk = disk.ok() ? &disk : nullptr;
    ctx.ipl = ipl.ok() ? &ipl : nullptr;
    ctx.rom = &romProvider;
    ctx.dumpDir = opts.dumpDir;

    makeDirs(opts.dumpDir);
    std::printf("dump: archive %s | recipes %s | dump %s\n", archivePath.c_str(),
                recipesDir.empty() ? "(none)" : recipesDir.c_str(), opts.dumpDir.c_str());

    int rc = 0;
    for (const auto& name : opts.classes) {
        std::printf("[%s]\n", name.c_str());
        ClassResult res;
        try {
            res = reg[name](ctx);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "  %s: FATAL %s\n", name.c_str(), e.what());
            rc = 1;
            continue;
        }
        if (res.hardError) {
            std::fprintf(stderr, "  %s: %s\n", name.c_str(), res.errorMsg.c_str());
            rc = 1;
        } else if (res.failed > 0) {
            std::fprintf(stderr, "  %s: %d item(s) failed\n", name.c_str(), res.failed);
            rc = 1;
        }
    }
    std::fflush(stdout);
    return rc;
}
