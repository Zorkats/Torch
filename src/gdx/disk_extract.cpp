// gdx-extract `disk` subcommand implementation. See disk_extract.h for the archive contract.

#include "gdx/disk_extract.h"

#include "archive/ZWrapper.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ── Disk geometry (frozen contract, R8 Step 1) ────────────────────────────────────────────────────
// Retail and fan-translated F-Zero X Expansion Kit 64DD images are exactly 64,931,840 bytes. The
// port's loader (port/disk_buffer.cpp) and firstboot validator (port/gdx_firstboot.cpp) both key on
// this exact size, so the archive step requires it too — a wrong-sized dump is rejected rather than
// silently packed.
constexpr std::size_t kDiskExactBytes = 64931840u;

// ── Vendored SHA-256 (public domain reference) ───────────────────────────────────────────────────
// Torch is built without StormLib (BUILD_STORMLIB=OFF), so this TU carries its own hasher rather than
// pulling a crypto dependency. Identical arithmetic to the port-side hasher in gdx_extract_launch.cpp
// and to torch/src/gdx/ipl_extract.cpp, so an identity computed here matches the SHA the port records
// in the sidecar (disk_sha256) — the deletion gate depends on that equivalence.
struct Sha256Ctx {
    std::uint32_t state[8];
    std::uint64_t count; // bytes
    std::uint8_t buffer[64];
};

const std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t ror32(std::uint32_t v, int b) {
    return (v >> b) | (v << (32 - b));
}

void sha256Transform(std::uint32_t state[8], const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        std::uint32_t ch = (e & f) ^ ((~e) & g);
        std::uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        std::uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void sha256Init(Sha256Ctx& ctx) {
    ctx.state[0] = 0x6a09e667u; ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u; ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu; ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu; ctx.state[7] = 0x5be0cd19u;
    ctx.count = 0;
}

void sha256Update(Sha256Ctx& ctx, const std::uint8_t* data, std::size_t len) {
    std::size_t idx = static_cast<std::size_t>(ctx.count & 63u);
    ctx.count += len;
    std::size_t i = 0;
    if (idx > 0) {
        std::size_t part = 64 - idx;
        if (len < part) {
            std::memcpy(&ctx.buffer[idx], data, len);
            return;
        }
        std::memcpy(&ctx.buffer[idx], data, part);
        sha256Transform(ctx.state, ctx.buffer);
        i = part;
    }
    for (; i + 63 < len; i += 64) {
        sha256Transform(ctx.state, &data[i]);
    }
    std::memcpy(ctx.buffer, &data[i], len - i);
}

void sha256Final(Sha256Ctx& ctx, std::uint8_t out[32]) {
    std::uint64_t bits = ctx.count << 3;
    std::uint8_t c = 0x80;
    sha256Update(ctx, &c, 1);
    c = 0x00;
    while ((ctx.count & 63u) != 56u) {
        sha256Update(ctx, &c, 1);
    }
    std::uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<std::uint8_t>((bits >> ((7 - i) * 8)) & 0xFF);
    }
    sha256Update(ctx, lenBytes, 8);
    for (int i = 0; i < 32; ++i) {
        out[i] = static_cast<std::uint8_t>((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

std::string toHex(const std::uint8_t* bytes, std::size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(d[bytes[i] >> 4]);
        out.push_back(d[bytes[i] & 0xF]);
    }
    return out;
}

// ── R8 Step 2: EK slice manifest ─────────────────────────────────────────────────────────────────
// A verbatim slice of the disk image to expose as an ek/<symbol> archive entry. Sorted by `key`
// before insertion so the container stays byte-identical across runs (see the insertion note below).
struct EkSlice {
    std::string key;        // "ek/<symbol>"
    std::uint64_t offset;   // physical .ndd byte offset
    std::uint64_t len;      // slice length in bytes
};

// Parses port/gen/ek_slice_manifest.txt (format v1). Returns true and fills `out` on success. Only the
// first three whitespace-separated fields (symbol, offset-hex, len-dec) are consumed; the trailing
// texture metadata (type/format/width/height/tlut) is for the offline Dump All and is ignored here.
// Every slice is bounds-checked against the image size — an out-of-range row is a hard failure so a
// stale manifest can never silently produce a truncated/garbage entry. The optional `count N` line, if
// present, must match the number of data rows. `imageBytes` is the exact disk-image length.
bool parseEkManifest(const std::string& manifestPath, std::uint64_t imageBytes,
                     std::vector<EkSlice>& out) {
    std::ifstream mf(manifestPath, std::ios::binary);
    if (!mf.is_open()) {
        std::fprintf(stderr, "gdx-extract disk: ERROR cannot open EK slice manifest %s\n",
                     manifestPath.c_str());
        return false;
    }
    long declaredCount = -1;
    std::string line;
    while (std::getline(mf, line)) {
        // Trim trailing CR (Windows line endings) and skip blanks / comments.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream ls(line);
        std::string tok0;
        ls >> tok0;
        if (tok0 == "count") {
            ls >> declaredCount;
            continue;
        }
        if (tok0 == "version" || tok0 == "textures") {
            continue; // metadata header lines
        }
        // Data row: <symbol> <offset-hex> <len-dec> [metadata...]
        std::string offTok, lenTok;
        if (!(ls >> offTok >> lenTok)) {
            std::fprintf(stderr, "gdx-extract disk: ERROR malformed manifest row: %s\n", line.c_str());
            return false;
        }
        char* end = nullptr;
        std::uint64_t off = std::strtoull(offTok.c_str(), &end, 0);
        if (end == offTok.c_str() || *end != '\0') {
            std::fprintf(stderr, "gdx-extract disk: ERROR bad offset '%s' for %s\n", offTok.c_str(),
                         tok0.c_str());
            return false;
        }
        std::uint64_t len = std::strtoull(lenTok.c_str(), &end, 10);
        if (end == lenTok.c_str() || *end != '\0') {
            std::fprintf(stderr, "gdx-extract disk: ERROR bad length '%s' for %s\n", lenTok.c_str(),
                         tok0.c_str());
            return false;
        }
        if (off > imageBytes || len > imageBytes - off) {
            std::fprintf(stderr,
                         "gdx-extract disk: ERROR slice %s [0x%llx,+%llu) exceeds the %llu-byte image\n",
                         tok0.c_str(), static_cast<unsigned long long>(off),
                         static_cast<unsigned long long>(len),
                         static_cast<unsigned long long>(imageBytes));
            return false;
        }
        out.push_back(EkSlice{std::string("ek/") + tok0, off, len});
    }
    if (declaredCount >= 0 && static_cast<std::size_t>(declaredCount) != out.size()) {
        std::fprintf(stderr,
                     "gdx-extract disk: ERROR manifest declared count %ld != %zu parsed rows\n",
                     declaredCount, out.size());
        return false;
    }
    if (out.empty()) {
        std::fprintf(stderr, "gdx-extract disk: ERROR EK slice manifest %s has no rows\n",
                     manifestPath.c_str());
        return false;
    }
    // Stable, key-sorted insertion order keeps the deflated container byte-identical across runs.
    std::sort(out.begin(), out.end(),
              [](const EkSlice& a, const EkSlice& b) { return a.key < b.key; });
    return true;
}

} // namespace

int GdxRunDiskExtract(const std::string& diskPath, const std::string& destDir,
                      const std::string& manifestPath) {
    // Read the whole disk image.
    std::ifstream in(diskPath, std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "gdx-extract disk: ERROR cannot open %s\n", diskPath.c_str());
        return 2;
    }
    std::vector<char> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (image.size() != kDiskExactBytes) {
        std::fprintf(stderr,
                     "gdx-extract disk: ERROR %s is %zu bytes; expected exactly %zu for a 64DD EK "
                     "disk image\n",
                     diskPath.c_str(), image.size(), kDiskExactBytes);
        return 2;
    }

    // Store VERBATIM — no byte-order normalization (see the normalize note in disk_extract.h). fmt is
    // recorded as 0 (native / as-is) to match the loader, which never swaps the disk buffer.
    const std::uint8_t fmt = 0;

    // Identity over the stored image bytes. Byte-order-independent by construction (there is only one
    // canonical disk byte order), and equal to the port's R7 managed-copy sha (sidecar disk_sha256),
    // which the boot-time deletion gate compares against.
    Sha256Ctx ctx;
    sha256Init(ctx);
    sha256Update(ctx, reinterpret_cast<const std::uint8_t*>(image.data()), image.size());
    std::uint8_t digest[32];
    sha256Final(ctx, digest);
    const std::string shaHex = toHex(digest, 32);

    // Identity entry: [u8 fmt][32-byte SHA-256].
    std::vector<char> identity;
    identity.reserve(1 + 32);
    identity.push_back(static_cast<char>(fmt));
    for (int i = 0; i < 32; ++i) {
        identity.push_back(static_cast<char>(digest[i]));
    }

    // R8 Step 2: optional EK per-asset slices. When a manifest is supplied it must parse cleanly and
    // every slice must fit the image (parseEkManifest enforces both) — otherwise fail before writing
    // anything so the runtime launcher keeps its fallback rather than mount a partial archive.
    std::vector<EkSlice> ekSlices;
    if (!manifestPath.empty()) {
        if (!parseEkManifest(manifestPath, static_cast<std::uint64_t>(image.size()), ekSlices)) {
            std::fprintf(stderr, "gdx-extract disk: ERROR EK slice manifest processing failed; no "
                                 "archive written\n");
            return 2;
        }
    }

    // Write entries in sorted key order. The two frozen disk/* entries come first ("disk/identity" <
    // "disk/image"), then the ek/<symbol> slices, already key-sorted by parseEkManifest. All disk/*
    // keys sort before every ek/* key ('d' < 'e'), so insertion order is globally sorted — which,
    // together with the build-level MINIZ_NO_TIME + pinned deflate level, keeps the archive
    // byte-identical across runs.
    const std::string outPath = destDir.empty() ? std::string("fzerox-disk.o2r")
                                                : (destDir + "/fzerox-disk.o2r");
    ZWrapper zip(outPath);
    zip.CreateArchive();
    zip.AddFile("disk/identity", identity);
    zip.AddFile("disk/image", image);
    for (const EkSlice& s : ekSlices) {
        std::vector<char> slice(image.begin() + static_cast<std::ptrdiff_t>(s.offset),
                                image.begin() + static_cast<std::ptrdiff_t>(s.offset + s.len));
        zip.AddFile(s.key, std::move(slice));
    }
    zip.Close();

    // Machine-parseable stdout (mirrors the ipl step's wording).
    std::printf("gdx-extract disk: disk identity sha256 %s (fmt %u)\n", shaHex.c_str(),
                static_cast<unsigned>(fmt));
    std::printf("gdx-extract disk: wrote %s (image %zu bytes, identity %zu bytes, %zu ek slices, %zu "
                "entries total)\n",
                outPath.c_str(), image.size(), identity.size(), ekSlices.size(),
                ekSlices.size() + 2);
    std::fflush(stdout);
    return 0;
}
