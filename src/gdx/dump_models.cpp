// Wave-3 `models` DumpClass: an F3DEX2 -> Wavefront OBJ/MTL exporter. Native, byte-for-byte port of
// tools/gen_dump_all_models.py (the proven oracle). See that file's module docstring for the full
// format derivation (all from decomp/include/PR/gbi.h). This TU mirrors the oracle exactly:
//   - an F3DEX2 interpreter (G_VTX 64-slot cache, G_TRI1/TRI2/QUAD, G_DL push/branch recursion,
//     G_SETTIMG material binding, G_TEXTURE scale, G_GEOMETRYMODE lighting bit) that accumulates a Mesh
//   - cart roots from the segment-configured cart yamls (7 uncompressed + 2 MIO0 segments) read through
//     the archive-first RomByteProvider (blob payload IS the compressed segment; ROM only as fallback)
//   - Expansion-Kit roots + FZX:LIMB skeleton from fzerox-disk.o2r ek/ entries + the slice manifest
//     (pointers are fully-resolved D_<hex> addresses)
//
// The harness (tools/verify_native_dump.py) compares .obj/.mtl as EXACT BYTES, skeleton.json
// structurally, and manifest.tsv data-rows exactly -- so every emitted glyph of text here is
// load-bearing and reproduces the oracle's own writers (write_obj_mtl / _fmt / the note strings /
// _write_manifest) character-for-character.

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace gdxdump {
namespace {

// ── F3DEX2 opcodes (decomp/include/PR/gbi.h:90-121) ─────────────────────────────────────────────────
constexpr int G_VTX = 0x01;
constexpr int G_TRI1 = 0x05;
constexpr int G_TRI2 = 0x06;
constexpr int G_QUAD = 0x07;
constexpr int G_TEXTURE = 0xD7;
constexpr int G_GEOMETRYMODE = 0xD9;
constexpr int G_MTX = 0xDA;
constexpr int G_DL = 0xDE;
constexpr int G_ENDDL = 0xDF;
constexpr int G_SETTIMG = 0xFD;
constexpr std::uint32_t G_DL_NOPUSH = 1;            // gbi.h:1039 (bit16 of w0)
constexpr std::uint32_t G_LIGHTING = 0x00020000;    // gbi.h:369
constexpr std::size_t VTX_SIZE = 16;                // sizeof(Vtx)
constexpr int VTX_CACHE = 64;                       // F-Zero X microcode cache depth
constexpr int MAX_CMDS = 20000;                     // per-DL safety cap
constexpr int MAX_DL_DEPTH = 128;                   // native G_DL push-recursion cap (real content
                                                    // nests only a handful deep; guards the C++ stack
                                                    // against a corrupt/cyclic push chain that slips
                                                    // past the mVisited address-revisit set)

// ── big-endian reads ────────────────────────────────────────────────────────────────────────────────
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::int16_t beS16(const std::uint8_t* p) {
    return static_cast<std::int16_t>((std::uint16_t(p[0]) << 8) | p[1]);
}

// ── deterministic compact float (mirrors oracle _fmt: "%.6f" then strip trailing 0s and '.') ─────────
std::string fmt(double x) {
    double v = x + 0.0; // normalise -0.0 -> 0.0 exactly as Python's (x + 0.0)
    char b[64];
    std::snprintf(b, sizeof(b), "%.6f", v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        std::size_t e = s.size();
        while (e > 0 && s[e - 1] == '0') --e; // rstrip("0")
        while (e > 0 && s[e - 1] == '.') --e; // rstrip(".")
        s.resize(e);
    }
    if (s.empty()) s = "0";
    return s;
}

// ── Python-repr of a double (shortest round-tripping decimal; used only in the limb note tuple) ──────
std::string pyFloatRepr(double d) {
    if (d == 0.0) return std::signbit(d) ? "-0.0" : "0.0";
    for (int p = 1; p <= 17; ++p) {
        char b[64];
        std::snprintf(b, sizeof(b), "%.*g", p, d);
        if (std::strtod(b, nullptr) == d) {
            std::string s = b;
            if (s.find_first_of(".eE") == std::string::npos) s += ".0";
            return s;
        }
    }
    char b[64];
    std::snprintf(b, sizeof(b), "%.17g", d);
    return b;
}
// Python round(x, 2): correctly-rounded 2-decimal value re-parsed to the nearest double.
double pyRound2(double x) {
    char b[64];
    std::snprintf(b, sizeof(b), "%.2f", x);
    return std::strtod(b, nullptr);
}

// ── address space: resolve a (segmented or fully-qualified) pointer to raw bytes ─────────────────────
struct Space {
    virtual ~Space() = default;
    // Returns [ptr, ptr+n) or nullopt if not fully resolvable.
    virtual std::optional<std::vector<std::uint8_t>> read(std::uint64_t ptr, std::size_t n) = 0;
};

// Cart: one segment, backed either by the archive-first RomByteProvider (uncompressed -> ROM offset
// rom_base + (ptr - base_addr)) or by a decompressed MIO0 buffer (index ptr - base_addr).
struct CartSpace : Space {
    const RomByteProvider* rom = nullptr; // uncompressed path
    std::uint64_t baseAddr = 0;
    std::uint64_t romBase = 0;
    std::vector<std::uint8_t> comp;       // decompressed segment (compressed path)
    bool compressed = false;
    bool compViaRom = false;              // whether the compressed blob was ROM-served
    long archiveReads = 0, romReads = 0;

    std::optional<std::vector<std::uint8_t>> read(std::uint64_t ptr, std::size_t n) override {
        if (ptr < baseAddr) return std::nullopt;
        if (compressed) {
            std::size_t idx = static_cast<std::size_t>(ptr - baseAddr);
            if (idx + n > comp.size()) return std::nullopt;
            (compViaRom ? romReads : archiveReads)++;
            return std::vector<std::uint8_t>(comp.begin() + static_cast<std::ptrdiff_t>(idx),
                                             comp.begin() + static_cast<std::ptrdiff_t>(idx + n));
        }
        std::uint64_t abs = romBase + (ptr - baseAddr);
        bool viaRom = false;
        auto r = rom->read(abs, n, &viaRom);
        if (!r) return std::nullopt;
        (viaRom ? romReads : archiveReads)++;
        return r;
    }
};

// EK: a set of in-memory ranges (each ek/<D_hex> slice placed at the address its name encodes).
struct EkSpace : Space {
    struct R { std::uint64_t start, end; std::vector<std::uint8_t> data; };
    std::vector<R> ranges;
    void add(std::uint64_t start, std::vector<std::uint8_t> data) {
        std::uint64_t end = start + data.size();
        ranges.push_back(R{start, end, std::move(data)});
    }
    void finalize() {
        std::sort(ranges.begin(), ranges.end(), [](const R& a, const R& b) { return a.start < b.start; });
    }
    std::optional<std::vector<std::uint8_t>> read(std::uint64_t ptr, std::size_t n) override {
        auto it = std::upper_bound(ranges.begin(), ranges.end(), ptr,
                                   [](std::uint64_t a, const R& b) { return a < b.start; });
        if (it == ranges.begin()) return std::nullopt;
        --it;
        if (ptr < it->start || ptr + n > it->end) return std::nullopt;
        std::size_t idx = static_cast<std::size_t>(ptr - it->start);
        return std::vector<std::uint8_t>(it->data.begin() + static_cast<std::ptrdiff_t>(idx),
                                         it->data.begin() + static_cast<std::ptrdiff_t>(idx + n));
    }
};

// ── texture binding info: material key, relative PNG path, on-disk existence, texel dims ─────────────
struct TexInfo {
    std::string key;
    std::string relPng;
    bool exists = false;
    int w = 32, h = 32;
};

// ── mesh accumulator (mirrors oracle Mesh) ──────────────────────────────────────────────────────────
struct Face {
    std::size_t i0, i1, i2;
    std::optional<std::string> mat;
    std::string group;
};
struct Stats {
    long vtxLoads = 0, tris = 0, dlCalls = 0, mtx = 0;
    long unresolvedVtx = 0, unresolvedDl = 0, unresolvedTimg = 0, droppedTris = 0;
};
struct Mesh {
    std::vector<std::array<double, 3>> pos;
    std::vector<std::array<double, 2>> uv;
    std::vector<std::optional<std::array<double, 3>>> color;
    std::vector<std::optional<std::array<double, 3>>> normal;
    std::vector<Face> faces;
    std::map<std::string, std::pair<std::string, bool>> materials; // key -> (rel_png, exists)
    Stats stats;
};

// ── F3DEX2 -> mesh interpreter (mirrors oracle Interpreter) ──────────────────────────────────────────
class Interpreter {
  public:
    Interpreter(Space& space, const std::map<std::uint64_t, TexInfo>& texMap,
                const std::map<std::uint64_t, std::string>& labelFor)
        : mSpace(space), mTexMap(texMap), mLabelFor(labelFor) {
        mCache.assign(VTX_CACHE, kNoSlot);
    }
    Mesh& mesh() { return mMesh; }

    void run(std::uint64_t rootPtr, const std::string& groupName, int depth = 0) {
        // Depth cap: a G_DL push recurses natively (see the G_DL case below). mVisited stops address
        // REVISITS, but a long non-cyclic push chain (or corrupt pointers) could still overflow the C++
        // stack. On exceed, record the same partial/unresolved outcome an unreadable DL pointer uses and
        // unwind cleanly -- never crash. The cap is far above real content's nesting (a handful deep).
        if (depth >= MAX_DL_DEPTH) { mMesh.stats.unresolvedDl++; return; }
        if (mVisited.count(rootPtr)) return;
        mVisited.insert(rootPtr);
        std::uint64_t a = rootPtr;
        int guard = 0;
        while (guard < MAX_CMDS) {
            auto word = mSpace.read(a, 8);
            if (!word) { mMesh.stats.unresolvedDl++; return; }
            std::uint32_t w0 = be32(word->data());
            std::uint32_t w1 = be32(word->data() + 4);
            int op = (w0 >> 24) & 0xFF;
            a += 8;
            guard++;
            if (op == G_VTX) {
                loadVtx(w0, w1);
            } else if (op == G_TRI1) {
                emitTri(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1, (w0 & 0xFF) >> 1, groupName);
            } else if (op == G_TRI2 || op == G_QUAD) {
                emitTri(((w0 >> 16) & 0xFF) >> 1, ((w0 >> 8) & 0xFF) >> 1, (w0 & 0xFF) >> 1, groupName);
                emitTri(((w1 >> 16) & 0xFF) >> 1, ((w1 >> 8) & 0xFF) >> 1, (w1 & 0xFF) >> 1, groupName);
            } else if (op == G_SETTIMG) {
                setTimg(w1);
            } else if (op == G_TEXTURE) {
                double ss = ((w1 >> 16) & 0xFFFF) / 65536.0;
                double st = (w1 & 0xFFFF) / 65536.0;
                mScaleS = (ss != 0.0) ? ss : 1.0;
                mScaleT = (st != 0.0) ? st : 1.0;
            } else if (op == G_GEOMETRYMODE) {
                std::uint32_t clear = (~w0) & 0x00FFFFFF;
                mGeoMode = (mGeoMode & ~clear) | (w1 & 0x00FFFFFF);
            } else if (op == G_MTX) {
                mMesh.stats.mtx++;
            } else if (op == G_DL) {
                mMesh.stats.dlCalls++;
                std::string sub;
                auto lit = mLabelFor.find(w1);
                if (lit != mLabelFor.end()) sub = lit->second;
                else { char b[24]; std::snprintf(b, sizeof(b), "dl_%08X", w1); sub = b; }
                if (!mSpace.read(w1, 8)) {
                    mMesh.stats.unresolvedDl++;
                } else if (((w0 >> 16) & G_DL_NOPUSH) == G_DL_NOPUSH) {
                    a = w1; // branch / tail-jump
                } else {
                    run(w1, sub, depth + 1); // push / call
                }
            } else if (op == G_ENDDL) {
                return;
            }
            // all other RDP/state ops don't affect geometry
        }
        mMesh.stats.unresolvedDl++; // fell off the safety cap
    }

  private:
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    void loadVtx(std::uint32_t w0, std::uint32_t w1) {
        int n = (w0 >> 12) & 0xFF;
        int end = (w0 >> 1) & 0x7F;
        int v0 = end - n;
        if (n <= 0 || v0 < 0 || v0 + n > VTX_CACHE) { mMesh.stats.unresolvedVtx++; return; }
        auto raw = mSpace.read(w1, static_cast<std::size_t>(n) * VTX_SIZE);
        if (!raw) { mMesh.stats.unresolvedVtx++; return; }
        mMesh.stats.vtxLoads++;
        bool lighting = (mGeoMode & G_LIGHTING) != 0;
        int w = mCurTex ? mCurTex->w : 32;
        int h = mCurTex ? mCurTex->h : 32;
        const std::uint8_t* p = raw->data();
        for (int i = 0; i < n; ++i) {
            const std::uint8_t* v = p + static_cast<std::size_t>(i) * VTX_SIZE;
            double x = beS16(v), y = beS16(v + 2), z = beS16(v + 4);
            int tu = beS16(v + 8), tv = beS16(v + 10);
            double u = (tu / 32.0) * mScaleS / (w ? w : 1);
            double vv = (tv / 32.0) * mScaleT / (h ? h : 1);
            std::size_t gidx = mMesh.pos.size();
            mMesh.pos.push_back({x, y, z});
            mMesh.uv.push_back({u, 1.0 - vv});
            if (lighting) {
                int nx = static_cast<std::int8_t>(v[12]);
                int ny = static_cast<std::int8_t>(v[13]);
                int nz = static_cast<std::int8_t>(v[14]);
                mMesh.normal.push_back(std::array<double, 3>{double(nx), double(ny), double(nz)});
                mMesh.color.push_back(std::nullopt);
            } else {
                int r = v[12], g = v[13], b = v[14];
                mMesh.color.push_back(std::array<double, 3>{r / 255.0, g / 255.0, b / 255.0});
                mMesh.normal.push_back(std::nullopt);
            }
            mCache[static_cast<std::size_t>(v0 + i)] = gidx;
        }
    }

    void emitTri(int s0, int s1, int s2, const std::string& group) {
        if (s0 >= VTX_CACHE || s1 >= VTX_CACHE || s2 >= VTX_CACHE) { mMesh.stats.droppedTris++; return; }
        std::size_t i0 = mCache[s0], i1 = mCache[s1], i2 = mCache[s2];
        if (i0 == kNoSlot || i1 == kNoSlot || i2 == kNoSlot) { mMesh.stats.droppedTris++; return; }
        std::optional<std::string> mat;
        if (mCurTex) mat = mCurTex->key;
        mMesh.faces.push_back(Face{i0, i1, i2, mat, group});
        mMesh.stats.tris++;
    }

    void setTimg(std::uint32_t w1) {
        auto it = mTexMap.find(w1);
        if (it == mTexMap.end()) { mCurTex = nullptr; mMesh.stats.unresolvedTimg++; return; }
        mCurTex = &it->second;
        mMesh.materials[it->second.key] = {it->second.relPng, it->second.exists};
    }

    Space& mSpace;
    const std::map<std::uint64_t, TexInfo>& mTexMap;
    const std::map<std::uint64_t, std::string>& mLabelFor;
    Mesh mMesh;
    std::vector<std::size_t> mCache;
    const TexInfo* mCurTex = nullptr;
    double mScaleS = 1.0, mScaleT = 1.0;
    std::uint32_t mGeoMode = 0;
    std::set<std::uint64_t> mVisited;
};

// ── DL-call scanner: reachable DL targets (push AND branch), for root subtraction ────────────────────
std::set<std::uint64_t> scanDlCalls(Space& space, std::uint64_t rootPtr) {
    std::set<std::uint64_t> calls;
    std::vector<std::uint64_t> stack{rootPtr};
    std::set<std::uint64_t> seen;
    while (!stack.empty()) {
        std::uint64_t a = stack.back();
        stack.pop_back();
        if (seen.count(a)) continue;
        seen.insert(a);
        int guard = 0;
        while (guard < MAX_CMDS) {
            auto word = space.read(a, 8);
            if (!word) break;
            std::uint32_t w0 = be32(word->data());
            std::uint32_t w1 = be32(word->data() + 4);
            int op = (w0 >> 24) & 0xFF;
            a += 8;
            guard++;
            if (op == G_DL) {
                calls.insert(w1);
                if (((w0 >> 16) & G_DL_NOPUSH) == G_DL_NOPUSH) {
                    if (space.read(w1, 8) && !seen.count(w1)) a = w1;
                    else break;
                } else {
                    stack.push_back(w1);
                }
            } else if (op == G_ENDDL) {
                break;
            }
        }
    }
    return calls;
}

// ── OBJ / MTL writers (byte-for-byte port of oracle write_obj_mtl) ──────────────────────────────────
void writeObjMtl(const Mesh& mesh, const std::string& objPath, const std::string& mtlPath,
                 const std::string& objName, const std::string& note) {
    bool hasNormals = false;
    for (const auto& nn : mesh.normal)
        if (nn.has_value()) { hasNormals = true; break; }

    std::string mtlBase = fs::path(mtlPath).filename().string();
    std::string out;
    out += "# " + objName + "\n";
    out += "# " + note + "\n";
    out += "# vertices=" + std::to_string(mesh.pos.size()) + " faces=" + std::to_string(mesh.faces.size()) +
           " materials=" + std::to_string(mesh.materials.size()) + "\n";
    out += "mtllib " + mtlBase + "\n";
    out += "o " + objName + "\n";
    for (std::size_t i = 0; i < mesh.pos.size(); ++i) {
        const auto& v = mesh.pos[i];
        if (mesh.color[i].has_value()) {
            const auto& c = *mesh.color[i];
            out += "v " + fmt(v[0]) + " " + fmt(v[1]) + " " + fmt(v[2]) + " " + fmt(c[0]) + " " +
                   fmt(c[1]) + " " + fmt(c[2]) + "\n";
        } else {
            out += "v " + fmt(v[0]) + " " + fmt(v[1]) + " " + fmt(v[2]) + "\n";
        }
    }
    for (const auto& t : mesh.uv) out += "vt " + fmt(t[0]) + " " + fmt(t[1]) + "\n";
    if (hasNormals) {
        for (const auto& nn : mesh.normal) {
            if (!nn.has_value()) out += "vn 0 0 1\n";
            else out += "vn " + fmt((*nn)[0]) + " " + fmt((*nn)[1]) + " " + fmt((*nn)[2]) + "\n";
        }
    }
    bool groupInit = true, matInit = true;
    std::string curGroup;
    std::optional<std::string> curMat;
    for (const auto& f : mesh.faces) {
        if (groupInit || f.group != curGroup) {
            out += "g " + (f.group.empty() ? std::string("default") : f.group) + "\n";
            curGroup = f.group;
            groupInit = false;
            matInit = true;
        }
        if (matInit || f.mat != curMat) {
            out += "usemtl " + (f.mat ? *f.mat : std::string("none")) + "\n";
            curMat = f.mat;
            matInit = false;
        }
        std::size_t a = f.i0 + 1, b = f.i1 + 1, c = f.i2 + 1;
        char line[128];
        if (hasNormals)
            std::snprintf(line, sizeof(line), "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b,
                          c, c, c);
        else
            std::snprintf(line, sizeof(line), "f %zu/%zu %zu/%zu %zu/%zu\n", a, a, b, b, c, c);
        out += line;
    }
    writeFileText(objPath, out);

    std::string mtl;
    mtl += "# materials for " + objName + "\n";
    mtl += "# map_Kd paths are relative to this .mtl; textures dumped by the `textures` class\n";
    mtl += "newmtl none\n";
    mtl += "Kd 0.8 0.8 0.8\n";
    for (const auto& kv : mesh.materials) { // std::map iterates sorted by key
        const std::string& key = kv.first;
        const std::string& relPng = kv.second.first;
        bool exists = kv.second.second;
        mtl += "newmtl " + key + "\n";
        mtl += "Kd 1 1 1\n";
        if (!exists) mtl += "# NOTE: texture PNG not present on disk (run --classes textures)\n";
        std::string rel = relPng;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        mtl += "map_Kd " + rel + "\n";
    }
    writeFileText(mtlPath, mtl);
}

// Relative PNG path (from dump/models/) to dump/<key>.png, forward-slashed, + on-disk existence.
std::pair<std::string, bool> relPng(const std::string& dumpDir, const std::string& key) {
    // Guard the existence probe the same way every write site guards its output path (dump_all.cpp's
    // safeOutputComponent): an unsafe key (path traversal / absolute / drive-rooted) must never be
    // joined onto dumpDir, even for a read-only fileExists probe. Treat it as simply not-present --
    // the caller (the .mtl writer) already prints a "texture PNG not present" note for that case.
    if (!safeOutputComponent(key)) {
        return {"../" + key + ".png", false};
    }
    std::string png = joinPath(dumpDir, key + ".png");
    // models_dir = dumpDir/models ; png = dumpDir/<key>.png -> "../" + key + ".png"
    return {"../" + key + ".png", fileExists(png)};
}

// Cheap (v, f) count for an existing OBJ (idempotent-skip manifest completeness).
std::pair<long, long> objCounts(const std::string& path) {
    long v = 0, f = 0;
    std::string s = readFile(path);
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t nl = s.find('\n', i);
        std::size_t end = (nl == std::string::npos) ? s.size() : nl;
        if (end - i >= 2) {
            if (s[i] == 'v' && s[i + 1] == ' ') v++;
            else if (s[i] == 'f' && s[i + 1] == ' ') f++;
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    return {v, f};
}

std::string hexUpper(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// ── cart model description (one segment-configured cart yaml) ────────────────────────────────────────
struct CartModel {
    std::string yamlStem;
    int segmentId = 0;
    std::uint64_t romBase = 0;
    std::vector<std::pair<std::uint64_t, std::string>> gfx; // (offset, symbol) sorted
    std::map<std::uint64_t, std::tuple<std::string, std::string, int, int>> tex; // full -> (ystem,sym,w,h)
    bool compressed = false;
    std::uint64_t compOffset = 0;
};

// Walk every segment-configured cart yaml carrying GFX entries (mirrors oracle _model_yamls, minus the
// AddressSpace build which we defer to CartSpace so the byte source is the archive-first provider).
std::vector<CartModel> modelYamls(const std::string& yamlDir) {
    std::vector<CartModel> models;
    for (const std::string& path : listYaml(yamlDir, false)) {
        std::string fname = fs::path(path).filename().string();
        if (isBlobRecipeFilename(fname)) continue;
        YAML::Node data;
        try { data = YAML::LoadFile(path); } catch (...) { continue; }
        if (!data[":config"]) continue;
        YAML::Node cfg = data[":config"];
        if (!cfg["segments"] || !cfg["segments"].IsSequence() || cfg["segments"].size() == 0) continue;
        YAML::Node s0 = cfg["segments"][0];
        if (!s0.IsSequence() || s0.size() < 2) continue;
        CartModel m;
        m.segmentId = static_cast<int>(parseIntStr(s0[0].as<std::string>()));
        m.romBase = static_cast<std::uint64_t>(parseIntStr(s0[1].as<std::string>()));
        m.yamlStem = fs::path(path).stem().string();
        if (cfg["compression"] && cfg["compression"]["offset"]) {
            m.compressed = true;
            m.compOffset = static_cast<std::uint64_t>(parseIntStr(cfg["compression"]["offset"].as<std::string>()));
        }
        std::uint64_t baseAddr = static_cast<std::uint64_t>(m.segmentId) << 24;
        for (const auto& kv : data) {
            std::string key = kv.first.as<std::string>();
            if (!key.empty() && key[0] == ':') continue;
            YAML::Node val = kv.second;
            if (!val.IsMap() || !val["type"]) continue;
            std::string t = val["type"].as<std::string>();
            if (t == "GFX" && val["offset"]) {
                std::uint64_t off = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
                std::string sym = val["symbol"] ? val["symbol"].as<std::string>() : key;
                m.gfx.emplace_back(off, sym);
            } else if ((t == "TEXTURE" || t == "COMPRESSED_TEXTURE") && val["offset"] && val["width"] &&
                       val["height"]) {
                std::uint64_t off = static_cast<std::uint64_t>(parseIntStr(val["offset"].as<std::string>()));
                std::uint64_t full = baseAddr | off;
                std::string sym = val["symbol"] ? val["symbol"].as<std::string>() : key;
                m.tex[full] = {m.yamlStem, sym, static_cast<int>(parseIntStr(val["width"].as<std::string>())),
                               static_cast<int>(parseIntStr(val["height"].as<std::string>()))};
            }
        }
        if (m.gfx.empty()) continue;
        std::sort(m.gfx.begin(), m.gfx.end()); // sorted by (offset, symbol)
        models.push_back(std::move(m));
    }
    return models;
}

// EK slice manifest row.
struct EkRow { std::string sym; std::uint64_t offset; long len; std::string type; };

// sym_addr: ^D_(?:[A-Za-z0-9]+_)?([0-9A-Fa-f]{6,8})$  -> trailing hex, or nullopt.
std::optional<std::uint64_t> symAddr(const std::string& name) {
    static const std::regex re("^D_(?:[A-Za-z0-9]+_)?([0-9A-Fa-f]{6,8})$");
    std::smatch mm;
    if (!std::regex_match(name, mm, re)) return std::nullopt;
    return static_cast<std::uint64_t>(std::strtoull(mm[1].str().c_str(), nullptr, 16));
}

bool isDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// models class
// ═══════════════════════════════════════════════════════════════════════════════════════════════════
ClassResult runModels(Context& ctx) {
    ClassResult res;
    res.name = "models";
    std::string outDir = joinPath(ctx.dumpDir, "models");
    makeDirs(outDir);

    // manifest rows: (key, source, yaml, rootAddr, vertices, faces, status)
    std::vector<std::array<std::string, 7>> manifestRows;
    int cartD = 0, cartS = 0, cartF = 0, ekD = 0, ekS = 0, ekF = 0;

    // ── cart ─────────────────────────────────────────────────────────────────────────────────────
    struct Census { std::string yaml; int seg; bool compressed; long archiveReads; long romReads; bool compViaRom; };
    std::vector<Census> census;
    {
        std::vector<CartModel> models = modelYamls(ctx.cartYamlDir);
        for (CartModel& model : models) {
            std::uint64_t baseAddr = static_cast<std::uint64_t>(model.segmentId) << 24;
            CartSpace space;
            space.baseAddr = baseAddr;
            space.romBase = model.romBase;
            if (model.compressed) {
                bool viaRom = false;
                std::vector<std::uint8_t> blob = ctx.rom->readUpTo(model.compOffset, 0x400000, &viaRom);
                std::vector<std::uint8_t> dec = mio0Decompress(blob.data(), blob.size());
                if (dec.empty()) {
                    std::fprintf(stderr, "  warn: models %s: MIO0 decompress failed -- skipping yaml\n",
                                 model.yamlStem.c_str());
                    continue;
                }
                space.compressed = true;
                space.comp = std::move(dec);
                space.compViaRom = viaRom;
            } else {
                space.rom = ctx.rom;
            }

            // texture-address -> material info
            std::map<std::uint64_t, TexInfo> texMap;
            for (const auto& kv : model.tex) {
                const auto& tup = kv.second;
                TexInfo ti;
                ti.key = std::get<0>(tup) + "/" + std::get<1>(tup);
                auto rp = relPng(ctx.dumpDir, ti.key);
                ti.relPng = rp.first;
                ti.exists = rp.second;
                ti.w = std::get<2>(tup);
                ti.h = std::get<3>(tup);
                texMap[kv.first] = ti;
            }
            std::map<std::uint64_t, std::string> labelFor;
            for (const auto& g : model.gfx) labelFor[baseAddr | g.first] = g.second;

            // root detection: a GFX entry no other GFX entry push/branch-calls
            std::set<std::uint64_t> called;
            for (const auto& g : model.gfx) {
                auto c = scanDlCalls(space, baseAddr | g.first);
                called.insert(c.begin(), c.end());
            }
            for (const auto& g : model.gfx) {
                std::uint64_t rootPtr = baseAddr | g.first;
                if (called.count(rootPtr)) continue;
                const std::string& symbol = g.second;
                std::string key = model.yamlStem + "_" + symbol;
                if (!safeOutputComponent(key)) {
                    std::fprintf(stderr, "  warn: models: unsafe output name '%s'; skipping\n", key.c_str());
                    continue;
                }
                std::string objPath = joinPath(outDir, key + ".obj");
                std::string mtlPath = joinPath(outDir, key + ".mtl");
                if (fileExists(objPath)) {
                    cartS++;
                    auto vf = objCounts(objPath);
                    manifestRows.push_back({key, "cart", model.yamlStem, hexUpper(rootPtr),
                                            std::to_string(vf.first), std::to_string(vf.second),
                                            "skip-existing"});
                    continue;
                }
                Interpreter interp(space, texMap, labelFor);
                interp.run(rootPtr, symbol);
                Mesh& mesh = interp.mesh();
                if (mesh.faces.empty()) {
                    manifestRows.push_back({key, "cart", model.yamlStem, hexUpper(rootPtr),
                                            std::to_string(mesh.pos.size()), "0", "no-geometry"});
                    continue;
                }
                const Stats& s = mesh.stats;
                char note[512];
                std::snprintf(note, sizeof(note),
                              "cart yaml=%s segment=%d rootAddr=0x%llX | vtxLoads=%ld dlCalls=%ld mtx=%ld "
                              "unresolved(vtx=%ld dl=%ld timg=%ld) droppedTris=%ld",
                              model.yamlStem.c_str(), model.segmentId,
                              static_cast<unsigned long long>(rootPtr), s.vtxLoads, s.dlCalls, s.mtx,
                              s.unresolvedVtx, s.unresolvedDl, s.unresolvedTimg, s.droppedTris);
                writeObjMtl(mesh, objPath, mtlPath, key, note);
                cartD++;
                std::string status = "ok";
                if (s.unresolvedVtx || s.unresolvedDl || s.unresolvedTimg) {
                    char st[96];
                    std::snprintf(st, sizeof(st), "ok-partial(uv=%ld ud=%ld ut=%ld)", s.unresolvedVtx,
                                  s.unresolvedDl, s.unresolvedTimg);
                    status = st;
                }
                manifestRows.push_back({key, "cart", model.yamlStem, hexUpper(rootPtr),
                                        std::to_string(mesh.pos.size()), std::to_string(mesh.faces.size()),
                                        status});
            }
            census.push_back(Census{model.yamlStem, model.segmentId, model.compressed, space.archiveReads,
                                     space.romReads, model.compressed ? space.compViaRom : false});
        }
    }

    // ── ek ───────────────────────────────────────────────────────────────────────────────────────
    if (ctx.disk == nullptr || ctx.manifestPath.empty() || !fileExists(ctx.manifestPath)) {
        std::printf("  models: no EK archive/manifest found -- skipping EK meshes\n");
    } else {
        std::string manifestText = readFile(ctx.manifestPath);
        std::vector<std::string> lines;
        {
            std::size_t i = 0;
            while (i < manifestText.size()) {
                std::size_t nl = manifestText.find('\n', i);
                std::size_t end = (nl == std::string::npos) ? manifestText.size() : nl;
                lines.push_back(manifestText.substr(i, end - i));
                if (nl == std::string::npos) break;
                i = nl + 1;
            }
        }
        auto splitWs = [](const std::string& s) {
            std::vector<std::string> p;
            std::size_t i = 0;
            while (i < s.size()) {
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                std::size_t st = i;
                while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                if (i > st) p.push_back(s.substr(st, i - st));
            }
            return p;
        };

        std::vector<EkRow> rows;
        for (const std::string& ln : lines) {
            auto p = splitWs(ln);
            if (p.size() >= 4 && p[1].rfind("0x", 0) == 0)
                rows.push_back(EkRow{p[0], static_cast<std::uint64_t>(std::strtoull(p[1].c_str(), nullptr, 16)),
                                     std::atol(p[2].c_str()), p[3]});
        }

        EkSpace space;
        std::map<std::uint64_t, std::string> labelFor;
        std::map<std::uint64_t, std::string> texNames; // addr -> symbol (dims filled below)
        std::vector<std::pair<std::uint64_t, std::string>> gfxSyms;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> limbSyms;
        for (const EkRow& r : rows) {
            std::string entry = "ek/" + r.sym;
            auto dataOpt = ctx.disk->readRaw(entry);
            if (!dataOpt) continue;
            std::vector<std::uint8_t> data = std::move(*dataOpt);
            auto a = symAddr(r.sym);
            if (a) {
                space.add(*a, data);
                labelFor[*a] = r.sym;
                if (r.type == "TEXTURE") texNames[*a] = r.sym;
            }
            if (r.type == "GFX" && a) gfxSyms.emplace_back(*a, r.sym);
            else if (r.type == "FZX:LIMB") limbSyms.emplace_back(r.sym, std::move(data));
        }
        space.finalize();

        // EK texture dims from the manifest (width,height columns)
        std::map<std::uint64_t, std::pair<int, int>> ekTexDims;
        for (const std::string& ln : lines) {
            auto p = splitWs(ln);
            if (p.size() >= 8 && p[3] == "TEXTURE") {
                auto a = symAddr(p[0]);
                if (a && isDigits(p[5]) && isDigits(p[6]))
                    ekTexDims[*a] = {std::atoi(p[5].c_str()), std::atoi(p[6].c_str())};
            }
        }
        std::map<std::uint64_t, TexInfo> ekTexInfo;
        for (const auto& kv : texNames) {
            TexInfo ti;
            int w = 32, h = 32;
            auto dit = ekTexDims.find(kv.first);
            if (dit != ekTexDims.end()) { w = dit->second.first; h = dit->second.second; }
            ti.key = "ek/" + kv.second;
            auto rp = relPng(ctx.dumpDir, ti.key);
            ti.relPng = rp.first;
            ti.exists = rp.second;
            ti.w = w;
            ti.h = h;
            ekTexInfo[kv.first] = ti;
        }

        // root detection over EK GFX
        std::set<std::uint64_t> called;
        for (const auto& g : gfxSyms) {
            auto c = scanDlCalls(space, g.first);
            called.insert(c.begin(), c.end());
        }
        std::vector<std::pair<std::uint64_t, std::string>> roots;
        for (const auto& g : gfxSyms)
            if (!called.count(g.first)) roots.push_back(g);
        std::sort(roots.begin(), roots.end());

        for (const auto& rt : roots) {
            std::uint64_t a = rt.first;
            const std::string& name = rt.second;
            std::string key = "ek_" + name;
            if (!safeOutputComponent(key)) {
                std::fprintf(stderr, "  warn: models(ek): unsafe output name '%s'; skipping\n", key.c_str());
                continue;
            }
            std::string objPath = joinPath(outDir, key + ".obj");
            std::string mtlPath = joinPath(outDir, key + ".mtl");
            if (fileExists(objPath)) {
                ekS++;
                auto vf = objCounts(objPath);
                manifestRows.push_back({key, "ek", "-", hexUpper(a), std::to_string(vf.first),
                                        std::to_string(vf.second), "skip-existing"});
                continue;
            }
            Interpreter interp(space, ekTexInfo, labelFor);
            interp.run(a, name);
            Mesh& mesh = interp.mesh();
            if (mesh.faces.empty()) {
                manifestRows.push_back({key, "ek", "-", hexUpper(a), std::to_string(mesh.pos.size()), "0",
                                        "no-geometry"});
                continue;
            }
            const Stats& s = mesh.stats;
            char note[512];
            std::snprintf(note, sizeof(note),
                          "expansion-kit rootAddr=0x%llX | vtxLoads=%ld dlCalls=%ld unresolved(vtx=%ld "
                          "dl=%ld timg=%ld) droppedTris=%ld",
                          static_cast<unsigned long long>(a), s.vtxLoads, s.dlCalls, s.unresolvedVtx,
                          s.unresolvedDl, s.unresolvedTimg, s.droppedTris);
            writeObjMtl(mesh, objPath, mtlPath, key, note);
            ekD++;
            std::string status = "ok";
            if (s.unresolvedVtx || s.unresolvedDl || s.unresolvedTimg) {
                char st[96];
                std::snprintf(st, sizeof(st), "ok-partial(uv=%ld ud=%ld ut=%ld)", s.unresolvedVtx,
                              s.unresolvedDl, s.unresolvedTimg);
                status = st;
            }
            manifestRows.push_back({key, "ek", "-", hexUpper(a), std::to_string(mesh.pos.size()),
                                    std::to_string(mesh.faces.size()), status});
        }

        // FZX:LIMB -> per-limb OBJ + skeleton.json hierarchy
        std::sort(limbSyms.begin(), limbSyms.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });
        std::string skelJson; // built incrementally
        std::vector<std::string> limbEntries;
        for (const auto& ls : limbSyms) {
            const std::string& name = ls.first;
            const std::vector<std::uint8_t>& data = ls.second;
            if (data.size() < 0x38) continue;
            const std::uint8_t* d = data.data();
            std::uint32_t dl = be32(d + 0x00);
            auto beF = [](const std::uint8_t* p) { std::uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return static_cast<double>(f); };
            double sx = beF(d + 0x04), sy = beF(d + 0x08), sz = beF(d + 0x0C);
            double px = beF(d + 0x10), py = beF(d + 0x14), pz = beF(d + 0x18);
            int r0 = beS16(d + 0x1C), r1 = beS16(d + 0x1E), r2 = beS16(d + 0x20);
            std::uint32_t nextLimb = be32(d + 0x24);
            std::uint32_t childLimb = be32(d + 0x28);
            std::uint32_t assocLimb = be32(d + 0x2C);
            std::uint32_t assocDl = be32(d + 0x30);
            int limbId = beS16(d + 0x34);

            // JSON limb entry (structural compare -- pyFloatRepr for float lists to be safe)
            char hx[16];
            std::string e = "{";
            e += "\"symbol\": \"" + name + "\", \"limbId\": " + std::to_string(limbId) + ", ";
            std::snprintf(hx, sizeof(hx), "0x%08X", dl); e += std::string("\"dl\": \"") + hx + "\", ";
            std::snprintf(hx, sizeof(hx), "0x%08X", assocDl); e += std::string("\"associatedLimbDL\": \"") + hx + "\", ";
            e += "\"scale\": [" + pyFloatRepr(sx) + ", " + pyFloatRepr(sy) + ", " + pyFloatRepr(sz) + "], ";
            e += "\"pos\": [" + pyFloatRepr(px) + ", " + pyFloatRepr(py) + ", " + pyFloatRepr(pz) + "], ";
            e += "\"rot\": [" + std::to_string(r0) + ", " + std::to_string(r1) + ", " + std::to_string(r2) + "], ";
            std::snprintf(hx, sizeof(hx), "0x%08X", nextLimb); e += std::string("\"nextLimb\": \"") + hx + "\", ";
            std::snprintf(hx, sizeof(hx), "0x%08X", childLimb); e += std::string("\"childLimb\": \"") + hx + "\", ";
            std::snprintf(hx, sizeof(hx), "0x%08X", assocLimb); e += std::string("\"associatedLimb\": \"") + hx + "\"}";
            limbEntries.push_back(e);

            if (dl == 0) continue;
            std::string key = "limb_" + name;
            if (!safeOutputComponent(key)) {
                std::fprintf(stderr, "  warn: models(limb): unsafe output name '%s'; skipping\n", key.c_str());
                continue;
            }
            std::string objPath = joinPath(outDir, key + ".obj");
            std::string mtlPath = joinPath(outDir, key + ".mtl");
            if (fileExists(objPath)) {
                ekS++;
                auto vf = objCounts(objPath);
                manifestRows.push_back({key, "ek-limb", "-", hexUpper(dl), std::to_string(vf.first),
                                        std::to_string(vf.second), "skip-existing"});
                continue;
            }
            Interpreter interp(space, ekTexInfo, labelFor);
            interp.run(dl, name);
            Mesh& mesh = interp.mesh();
            if (mesh.faces.empty()) {
                manifestRows.push_back({key, "ek-limb", "-", hexUpper(dl), std::to_string(mesh.pos.size()),
                                        "0", "no-geometry"});
                continue;
            }
            // note: pos rounded to 2dp, formatted as a Python tuple repr
            std::string posTuple = "(" + pyFloatRepr(pyRound2(px)) + ", " + pyFloatRepr(pyRound2(py)) +
                                   ", " + pyFloatRepr(pyRound2(pz)) + ")";
            char note[512];
            std::snprintf(note, sizeof(note),
                          "EAD skeleton limb (limb-local space; pos/scale/rot in skeleton.json, NOT "
                          "composed) dl=0x%llX pos=%s | vtxLoads=%ld droppedTris=%ld",
                          static_cast<unsigned long long>(dl), posTuple.c_str(), mesh.stats.vtxLoads,
                          mesh.stats.droppedTris);
            writeObjMtl(mesh, objPath, mtlPath, key, note);
            ekD++;
            manifestRows.push_back({key, "ek-limb", "-", hexUpper(dl), std::to_string(mesh.pos.size()),
                                    std::to_string(mesh.faces.size()), "ok"});
        }

        if (!limbEntries.empty()) {
            std::string skelPath = joinPath(outDir, "skeleton.json");
            if (!fileExists(skelPath)) {
                std::string j = "{\n";
                j += "  \"source\": \"fzerox-disk.o2r ek/aEADDemoSkeletonLimb* (FZX:LIMB, torch "
                     "EADLimbFactory::parse layout)\",\n";
                j += "  \"note\": \"Per-limb OBJs are in limb-local space. Full skeleton assembly "
                     "(composing pos/scale + binary-angle Vec3s rot down the child/next hierarchy) is "
                     "documented as future work; the transforms are provided here for anyone who wants "
                     "to assemble the pose.\",\n";
                j += "  \"limbCount\": " + std::to_string(limbEntries.size()) + ",\n";
                j += "  \"limbs\": [\n";
                for (std::size_t i = 0; i < limbEntries.size(); ++i) {
                    j += "    " + limbEntries[i];
                    if (i + 1 < limbEntries.size()) j += ",";
                    j += "\n";
                }
                j += "  ]\n}";
                writeFileText(skelPath, j);
            }
        }
    }

    // ── manifest + summary ─────────────────────────────────────────────────────────────────────────
    std::sort(manifestRows.begin(), manifestRows.end(),
              [](const std::array<std::string, 7>& a, const std::array<std::string, 7>& b) {
                  if (a[1] != b[1]) return a[1] < b[1]; // source
                  return a[0] < b[0];                   // key
              });
    std::vector<std::vector<std::string>> rows;
    for (const auto& r : manifestRows) rows.push_back({r.begin(), r.end()});
    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "key\tsource\tyaml\trootAddr\tvertices\tfaces\tstatus   (one OBJ+MTL per root display "
                  "list; F3DEX2 -> Wavefront OBJ)",
                  rows);

    int dumped = cartD + ekD, skipped = cartS + ekS, failed = cartF + ekF;
    std::printf("  models: %d dumped, %d skipped, %d failed  (cart %d/%d, ek %d/%d) -> models\n", dumped,
                skipped, failed, cartD, cartS, ekD, ekS);
    // per-segment archive-vs-rom census (proves archive-only coverage of the model segments)
    for (const Census& c : census) {
        std::printf("  models census: yaml=%s seg=%d %s archiveReads=%ld romReads=%ld%s\n",
                    c.yaml.c_str(), c.seg, c.compressed ? "MIO0" : "raw", c.archiveReads, c.romReads,
                    c.compressed ? (c.compViaRom ? " (comp-blob via ROM)" : " (comp-blob via archive)") : "");
    }

    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(manifestRows.size());
    return res;
}

} // namespace gdxdump
