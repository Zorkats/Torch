// gdx-extract `ipl` subcommand implementation. See ipl_extract.h for the archive contract (C-R3.1).

#include "gdx/ipl_extract.h"

#include "archive/ZWrapper.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// ── IPL geometry (frozen contract C-R3.1) ────────────────────────────────────────────────────────
// The port reads only the drive-ROM font glyph block. DDROM_FONT_START (0xA0000) is where the block
// begins; the max computed font reach is 0x117E80, so a whole-block blob to 0x140000 is the safe
// shape (guards at every consumer tolerate over-inclusion). The archived slice is therefore exactly
// [0xA0000, 0x140000) = 0xA0000 bytes, copied by the port to that same offset inside a 0x140000
// buffer whose low region is zero-filled.
constexpr std::size_t kFontStart = 0xA0000u;
constexpr std::size_t kFontEnd = 0x140000u;
constexpr std::size_t kFontBytes = kFontEnd - kFontStart; // 0xA0000

// ── Vendored SHA-256 (public domain reference) ───────────────────────────────────────────────────
// Torch is built without StormLib (BUILD_STORMLIB=OFF), so this TU carries its own hasher rather than
// pulling a crypto dependency. Identical arithmetic to the port-side hasher in gdx_extract_launch.cpp,
// so an identity computed here matches the one the launcher records in the sidecar.
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

// Byte-order-normalize an IPL dump to native big-endian, in place. Identical detection/normalization
// to the port's gdx_ddipl_load (disk_buffer.cpp): dumps circulate as z64 (BE, first byte 0x80), v64
// (16-bit-swapped, second byte 0x80), or n64 (32-bit-LE, fourth byte 0x80). Returns the format code
// stored in the identity entry: 0=z64/native, 1=v64, 2=n64, 3=unrecognized (left as-is).
std::uint8_t normalizeToBigEndian(std::vector<std::uint8_t>& buf) {
    const std::size_t sz = buf.size();
    if (sz < 4) {
        return 3;
    }
    if (buf[0] == 0x80) {
        return 0; // native big-endian
    }
    if (buf[1] == 0x80) {
        for (std::size_t i = 0; i + 1 < sz; i += 2) { // v64: swap 16-bit pairs
            std::uint8_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t;
        }
        return 1;
    }
    if (buf[3] == 0x80) {
        for (std::size_t i = 0; i + 3 < sz; i += 4) { // n64: reverse 32-bit words
            std::uint8_t t0 = buf[i], t1 = buf[i + 1];
            buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2];
            buf[i + 2] = t1; buf[i + 3] = t0;
        }
        return 2;
    }
    std::fprintf(stderr,
                 "gdx-extract ipl: WARNING unrecognized byte order (first bytes %02X %02X %02X %02X); "
                 "using as-is\n",
                 buf[0], buf[1], buf[2], buf[3]);
    return 3;
}

} // namespace

int GdxRunIplExtract(const std::string& iplPath, const std::string& destDir) {
    // Read the whole IPL dump.
    std::ifstream in(iplPath, std::ios::binary);
    if (!in.is_open()) {
        std::fprintf(stderr, "gdx-extract ipl: ERROR cannot open %s\n", iplPath.c_str());
        return 2;
    }
    std::vector<std::uint8_t> ipl((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (ipl.size() < kFontEnd) {
        std::fprintf(stderr,
                     "gdx-extract ipl: ERROR %s is %zu bytes; need at least 0x%zX for the font block\n",
                     iplPath.c_str(), ipl.size(), kFontEnd);
        return 2;
    }

    // Normalize to native big-endian, then compute the identity over the FULL normalized image so the
    // identity is byte-order-independent (a z64/v64/n64 dump of the same ROM yields the same hash).
    const std::uint8_t fmt = normalizeToBigEndian(ipl);

    Sha256Ctx ctx;
    sha256Init(ctx);
    sha256Update(ctx, ipl.data(), ipl.size());
    std::uint8_t digest[32];
    sha256Final(ctx, digest);
    const std::string shaHex = toHex(digest, 32);

    // Slice the font block [0xA0000, 0x140000).
    std::vector<char> fontBlock(reinterpret_cast<const char*>(ipl.data()) + kFontStart,
                                reinterpret_cast<const char*>(ipl.data()) + kFontEnd);

    // Identity entry: [u8 fmt][32-byte SHA-256].
    std::vector<char> identity;
    identity.reserve(1 + 32);
    identity.push_back(static_cast<char>(fmt));
    for (int i = 0; i < 32; ++i) {
        identity.push_back(static_cast<char>(digest[i]));
    }

    // Write the two entries in sorted key order ("ipl/font_block" < "ipl/identity"). Deterministic
    // output relies on stable insertion order plus the build-level MINIZ_NO_TIME + pinned deflate
    // level; adding in sorted order keeps the archive byte-identical across runs.
    const std::string outPath = destDir.empty() ? std::string("n64ddipl.o2r")
                                                 : (destDir + "/n64ddipl.o2r");
    ZWrapper zip(outPath);
    zip.CreateArchive();
    zip.AddFile("ipl/font_block", fontBlock);
    zip.AddFile("ipl/identity", identity);
    zip.Close();

    // Machine-parseable stdout for the runtime launcher's sidecar (gdx_extract_launch.cpp). Keep the
    // token wording stable — scanStdoutLine keys on "ipl identity sha256".
    std::printf("gdx-extract ipl: ipl identity sha256 %s (fmt %u)\n", shaHex.c_str(),
                static_cast<unsigned>(fmt));
    std::printf("gdx-extract ipl: wrote %s (font_block %zu bytes, identity %zu bytes)\n",
                outPath.c_str(), fontBlock.size(), identity.size());
    std::fflush(stdout);
    return 0;
}
