// `audio` dump class — soundfont samples -> WAV (+ per-font instrument JSON + manifest). Mirrors
// tools/gen_dump_all_audio.py SampleDumpClass line-for-line. Also hosts the shared AudioTables loader
// and EK-manifest length map used by both the audio and midi classes.
//
// GROUND TRUTH (same citations as the oracle module docstring): VADPCM decode mirrors
// port/n64_audio_hle.c RunAdpcm (block-convolution path + small-ADPCM deferred-clamping path); the
// soundfont binary parser mirrors decomp/src/audio/disk/lib/load.c gdx_audio_convert_font (disk fonts
// use inst_base 2, cart fonts inst_base 1 per rom/lib/load.c AudioLoad_RelocateFont). The audio tables
// are read from the build-generated audio_tables_data.json "audio" section (already evaluated), so no
// decomp source is needed at runtime.
//
// The .wav files are BYTE-IDENTICAL to Python's stdlib `wave` output (canonical 44-byte RIFF header,
// 16-bit mono PCM). The fonts/*.json are compared by parsed structure (see verify_native_dump.py).

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace gdxdump {
namespace {

// ── SampleMedium / SampleCodec (decomp/include/sfx.h) ────────────────────────────────────────────────
constexpr int MEDIUM_RAM = 0, MEDIUM_LBA = 1, MEDIUM_CART = 2, MEDIUM_DISK_DRIVE = 3;
constexpr int CODEC_ADPCM = 0, CODEC_S8 = 1, CODEC_S16_INMEMORY = 2, CODEC_SMALL_ADPCM = 3,
              CODEC_REVERB = 4, CODEC_S16 = 5, CODEC_UNK6 = 6;
constexpr int SYNTHESIS_RATE_HZ = 32000;

std::string codecName(int c) {
    switch (c) {
        case 0: return "ADPCM";
        case 1: return "S8";
        case 2: return "S16_INMEMORY";
        case 3: return "SMALL_ADPCM";
        case 4: return "REVERB";
        case 5: return "S16";
        case 6: return "UNK6";
        default: return std::to_string(c);
    }
}
const char* mediumName(int m) {
    switch (m) {
        case 0: return "RAM";
        case 1: return "LBA";
        case 2: return "CART";
        case 3: return "DISK_DRIVE";
        default: return "?";
    }
}

// FONT_* enum order (disk config) -> ek_slice_manifest soundfont symbol.
const char* kEkFontSymbols[] = {
    "aAudioSoundFontDDGuitar", "aAudioSoundFontDDSE", "aAudioSoundFontDDMuteCity",
    "aAudioSoundFontDDSilence", "aAudioSoundFontDDSandOcean", "aAudioSoundFontDDPortTown",
    "aAudioSoundFontDDBigBlue", "aAudioSoundFontDDDevilsForest", "aAudioSoundFontDDRedCanyon",
    "aAudioSoundFontDDSector", "aAudioSoundFontDDWhiteLand", "aAudioSoundFontDDRainbowRoad",
    "aAudioSoundFontDDNew03", "aAudioSoundFontDDNew02", "aAudioSoundFontDDNew01",
    "aAudioSoundFontDDNew04", "aAudioSoundFontDDTitle", "aAudioSoundFontDDSelect",
    "aAudioSoundFontDDOption", "aAudioSoundFontDDDeathRace", "aAudioSoundFontDDCourseEditor",
    "aAudioSoundFontDDMachineEditor", "aAudioSoundFontDDEADDemo",
};

// ── big-endian readers (all N64 audio data is big-endian). Callers guard bounds as the oracle does. ──
std::uint32_t bU32(const std::vector<std::uint8_t>& d, std::size_t o) {
    return (std::uint32_t(d[o]) << 24) | (std::uint32_t(d[o + 1]) << 16) | (std::uint32_t(d[o + 2]) << 8) |
           d[o + 3];
}
std::int16_t bS16(const std::vector<std::uint8_t>& d, std::size_t o) {
    return static_cast<std::int16_t>((std::uint16_t(d[o]) << 8) | d[o + 1]);
}
float bF32(const std::vector<std::uint8_t>& d, std::size_t o) {
    std::uint32_t u = bU32(d, o);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Python3 round(): round half to even. tuning is always > 0 here, so x >= 0.
long long pyRound(double x) {
    double f = std::floor(x);
    double diff = x - f;
    if (diff < 0.5) return static_cast<long long>(f);
    if (diff > 0.5) return static_cast<long long>(f) + 1;
    long long fi = static_cast<long long>(f);
    return (fi % 2 == 0) ? fi : fi + 1;
}

// ── JSON helpers (font JSON is compared by parsed structure, not exact bytes) ─────────────────────────
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
    char b[64];
    std::snprintf(b, sizeof(b), "%.17g", static_cast<double>(f));
    std::string s = b;
    if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
    return s;
}
std::string hexU(std::uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// Soundfont binary parser (mirrors parse_font/parse_sample/parse_book/parse_loop/parse_envelope)
// ══════════════════════════════════════════════════════════════════════════════════════════════════
struct BookInfo {
    int order = 0, numPredictors = 0;
    std::vector<int> coefs;
    bool valid = false;
};
struct LoopInfo {
    std::uint32_t start = 0, end = 0, count = 0;
    bool hasState = false;
    bool valid = false;
};
struct Sample {
    int codec = 0;
    int bank_sel = 0;
    std::uint32_t size = 0;
    std::uint32_t sample_addr = 0;
    std::uint32_t hdr_off = 0;
    LoopInfo loop;
    BookInfo book;
    bool valid = false;
};
struct TunedSample {
    Sample sample;
    float tuning = 0.0f;
    bool valid = false;
};
struct Instrument {
    int index = 0, normalRangeLo = 0, normalRangeHi = 0, adsrDecayIndex = 0, envelopePoints = 0;
    TunedSample lowPitch, normalPitch, highPitch;
    bool present = false;
};
struct Drum {
    int index = 0, adsrDecayIndex = 0, pan = 0;
    TunedSample tunedSample;
    bool present = false;
};
struct SoundEffect {
    int index = 0;
    TunedSample tunedSample;
    bool present = false;
};
struct ParsedFont {
    std::vector<Instrument> instruments;
    std::vector<Drum> drums;
    std::vector<SoundEffect> soundEffects;
};

BookInfo parseBook(const std::vector<std::uint8_t>& d, std::uint32_t off) {
    BookInfo b;
    if (off == 0 || static_cast<std::size_t>(off) + 8 > d.size()) return b;
    std::uint32_t order = bU32(d, off);
    std::uint32_t npred = bU32(d, off + 4);
    if (order < 1 || order > 8 || npred < 1 || npred > 8) return b;
    std::size_t n = 8u * order * npred;
    if (static_cast<std::size_t>(off) + 8 + n * 2 > d.size()) return b;
    b.order = static_cast<int>(order);
    b.numPredictors = static_cast<int>(npred);
    b.coefs.resize(n);
    for (std::size_t i = 0; i < n; ++i) b.coefs[i] = bS16(d, off + 8 + i * 2);
    b.valid = true;
    return b;
}

LoopInfo parseLoop(const std::vector<std::uint8_t>& d, std::uint32_t off) {
    LoopInfo l;
    if (off == 0 || static_cast<std::size_t>(off) + 12 > d.size()) return l;
    l.start = bU32(d, off);
    l.end = bU32(d, off + 4);
    l.count = bU32(d, off + 8);
    l.hasState = (l.count != 0 && static_cast<std::size_t>(off) + 0x10 + 32 <= d.size());
    l.valid = true;
    return l;
}

// Envelope point count (delay<=0 terminates; max 64). Only the count is used by the JSON.
int parseEnvelopePoints(const std::vector<std::uint8_t>& d, std::uint32_t off) {
    if (off == 0 || off >= d.size()) return 0;
    int count = 0;
    for (int i = 0; i < 64; ++i) {
        std::size_t p = static_cast<std::size_t>(off) + i * 4;
        if (p + 4 > d.size()) break;
        std::int16_t delay = bS16(d, p);
        ++count;
        if (delay <= 0) break;
    }
    return count;
}

Sample parseSample(const std::vector<std::uint8_t>& d, std::uint32_t hdr_off) {
    Sample s;
    if (hdr_off == 0 || static_cast<std::size_t>(hdr_off) + 0x10 > d.size()) return s;
    std::uint32_t flags = bU32(d, hdr_off);
    s.codec = (flags >> 28) & 7;
    s.bank_sel = (flags >> 26) & 3;
    s.size = flags & 0xFFFFFF;
    s.sample_addr = bU32(d, hdr_off + 4);
    s.loop = parseLoop(d, bU32(d, hdr_off + 8));
    s.book = parseBook(d, bU32(d, hdr_off + 0xC));
    s.hdr_off = hdr_off;
    s.valid = true;
    return s;
}

TunedSample parseTuned(const std::vector<std::uint8_t>& d, std::uint32_t off) {
    TunedSample t;
    if (static_cast<std::size_t>(off) + 8 > d.size()) return t;
    Sample s = parseSample(d, bU32(d, off));
    if (!s.valid) return t;
    t.sample = s;
    t.tuning = bF32(d, off + 4);
    t.valid = true;
    return t;
}

ParsedFont parseFont(const std::vector<std::uint8_t>& d, int num_inst, int num_drums, int num_sfx,
                     int inst_base) {
    ParsedFont pf;
    std::uint32_t drum_arr_off = (d.size() >= 4) ? bU32(d, 0) : 0;
    std::uint32_t sfx_arr_off = (inst_base == 2 && d.size() >= 8) ? bU32(d, 4) : 0;

    if (num_drums > 0 && drum_arr_off != 0) {
        for (int i = 0; i < num_drums; ++i) {
            std::size_t p = static_cast<std::size_t>(drum_arr_off) + i * 4;
            if (p + 4 > d.size()) break;
            std::uint32_t d_off = bU32(d, p);
            Drum dr;
            dr.index = i;
            if (d_off == 0 || static_cast<std::size_t>(d_off) + 0x10 > d.size()) {
                dr.present = false; // None
                pf.drums.push_back(dr);
                continue;
            }
            dr.present = true;
            dr.adsrDecayIndex = d[d_off];
            dr.pan = d[d_off + 1];
            dr.tunedSample = parseTuned(d, d_off + 4);
            pf.drums.push_back(dr);
        }
    }

    if (inst_base == 2 && num_sfx > 0 && sfx_arr_off != 0) {
        for (int i = 0; i < num_sfx; ++i) {
            std::size_t p = static_cast<std::size_t>(sfx_arr_off) + i * 8;
            if (p + 8 > d.size()) break;
            SoundEffect se;
            se.index = i;
            if (bU32(d, p) == 0) {
                se.present = false; // None
                pf.soundEffects.push_back(se);
                continue;
            }
            se.present = true;
            se.tunedSample = parseTuned(d, static_cast<std::uint32_t>(p));
            pf.soundEffects.push_back(se);
        }
    }

    for (int i = 0; i < num_inst; ++i) {
        std::size_t p = static_cast<std::size_t>(inst_base + i) * 4;
        if (p + 4 > d.size()) break;
        std::uint32_t i_off = bU32(d, p);
        Instrument inst;
        inst.index = i;
        if (i_off == 0 || static_cast<std::size_t>(i_off) + 0x20 > d.size()) {
            inst.present = false; // None
            pf.instruments.push_back(inst);
            continue;
        }
        inst.present = true;
        int range_lo = d[i_off + 1];
        int range_hi = d[i_off + 2];
        inst.normalRangeLo = range_lo;
        inst.normalRangeHi = range_hi;
        inst.adsrDecayIndex = d[i_off + 3];
        inst.envelopePoints = parseEnvelopePoints(d, bU32(d, i_off + 4));
        if (range_lo != 0) inst.lowPitch = parseTuned(d, i_off + 8);
        inst.normalPitch = parseTuned(d, i_off + 0x10);
        if (range_hi != 0x7F) inst.highPitch = parseTuned(d, i_off + 0x18);
        pf.instruments.push_back(inst);
    }
    return pf;
}

// Yield (role, sample, tuning) for every distinct-by-header sample the font references. Order:
// instruments (low/normal/high) then drums then sfx, deduped by sample header offset.
struct SampleRef {
    std::string role;
    Sample sample;
    float tuning;
};
std::vector<SampleRef> fontSamples(const ParsedFont& pf) {
    std::vector<SampleRef> out;
    std::vector<std::uint32_t> seen;
    auto emit = [&](const std::string& role, const TunedSample& t) {
        if (!t.valid || !t.sample.valid) return;
        if (std::find(seen.begin(), seen.end(), t.sample.hdr_off) != seen.end()) return;
        seen.push_back(t.sample.hdr_off);
        out.push_back(SampleRef{role, t.sample, t.tuning});
    };
    for (const Instrument& inst : pf.instruments) {
        if (!inst.present) continue;
        emit("inst" + std::to_string(inst.index) + "_low", inst.lowPitch);
        emit("inst" + std::to_string(inst.index) + "_normal", inst.normalPitch);
        emit("inst" + std::to_string(inst.index) + "_high", inst.highPitch);
    }
    for (const Drum& dr : pf.drums)
        if (dr.present) emit("drum" + std::to_string(dr.index), dr.tunedSample);
    for (const SoundEffect& se : pf.soundEffects)
        if (se.present) emit("sfx" + std::to_string(se.index), se.tunedSample);
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// VADPCM decode (mirrors decode_adpcm both paths; int64 accumulators + arithmetic >>11 match Python)
// ══════════════════════════════════════════════════════════════════════════════════════════════════
inline int clampS16(long long v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<int>(v);
}

// Returns decoded s16 PCM. `ok` is set false if there is no book (CODEC_ADPCM/SMALL_ADPCM need one).
std::vector<int> decodeAdpcm(const std::vector<std::uint8_t>& data, const BookInfo& book, bool small) {
    int order = book.order;
    int npred = book.numPredictors;
    const std::vector<int>& coefs = book.coefs;
    int row_stride = 8 * order;
    auto bc = [&](int pred, int tap, int col) -> long long {
        long long idx = static_cast<long long>(pred) * row_stride + tap * 8 + col;
        return (idx >= 0 && idx < static_cast<long long>(coefs.size())) ? coefs[idx] : 0;
    };
    int data_bytes = small ? 4 : 8;
    int frame_bytes = 1 + data_bytes;
    std::vector<int> out;
    long long h1 = 0, h2 = 0; // clamped history: newer(h1), older(h2)
    std::size_t nframes = data.size() / frame_bytes;
    for (std::size_t f = 0; f < nframes; ++f) {
        std::size_t base = f * frame_bytes;
        int header = data[base];
        int shift = (header >> 4) & 0xF;
        int pred = header & 0xF;
        if (pred >= npred) pred = 0;
        if (small) {
            long long raw1 = h1, raw2 = h2;
            for (int s = 0; s < 16; ++s) {
                int byte_val = data[base + 1 + (s / 4)];
                int nib = (byte_val >> ((3 - (s % 4)) * 2)) & 0x3;
                if (nib & 0x2) nib -= 4;
                long long residual = static_cast<long long>(nib) << shift;
                long long predicted = (bc(pred, 0, 0) * raw2 + bc(pred, 1, 0) * raw1) >> 11;
                long long sample_out = predicted + residual; // RAW, unclamped -> feeds next prediction
                int clamped_out = clampS16(sample_out);
                out.push_back(clamped_out);
                raw2 = raw1;
                raw1 = sample_out;      // RAW carry, intra-frame only
                h2 = h1;
                h1 = clamped_out;       // CLAMPED shadow, crosses frames
            }
        } else {
            long long e_h2 = h2, e_h1 = h1;
            for (int sub = 0; sub < 2; ++sub) {
                long long e[8];
                for (int i = 0; i < 8; ++i) {
                    int si = sub * 8 + i;
                    int bv = data[base + 1 + (si / 2)];
                    int nib = (si % 2 == 0) ? ((bv >> 4) & 0xF) : (bv & 0xF);
                    if (nib & 0x8) nib -= 16;
                    e[i] = static_cast<long long>(nib) << shift;
                }
                long long sblk[8];
                for (int i = 0; i < 8; ++i) {
                    long long acc = bc(pred, 0, i) * e_h2 + bc(pred, 1, i) * e_h1;
                    for (int k = 0; k < i; ++k) acc += bc(pred, 1, i - 1 - k) * e[k];
                    acc += e[i] << 11;
                    int v = clampS16(acc >> 11);
                    sblk[i] = v;
                    out.push_back(v);
                }
                e_h2 = sblk[6];
                e_h1 = sblk[7];
            }
            h2 = e_h2;
            h1 = e_h1;
        }
    }
    return out;
}

std::vector<int> decodeS16(const std::vector<std::uint8_t>& data) {
    std::size_t n = data.size() / 2;
    std::vector<int> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = bS16(data, i * 2);
    return out;
}
std::vector<int> decodeS8(const std::vector<std::uint8_t>& data) {
    std::vector<int> out;
    out.reserve(data.size());
    for (std::uint8_t b : data) {
        int v = (b >= 128) ? (b - 256) : b;
        out.push_back(v << 8);
    }
    return out;
}

// Returns decoded PCM; `ok`=false means codec-unsupported (mirrors the oracle returning None).
std::vector<int> decodeSampleBytes(int codec, const std::vector<std::uint8_t>& data,
                                   const BookInfo& book, bool& ok) {
    ok = true;
    if (codec == CODEC_ADPCM) {
        if (!book.valid) { ok = false; return {}; }
        return decodeAdpcm(data, book, false);
    }
    if (codec == CODEC_SMALL_ADPCM) {
        if (!book.valid) { ok = false; return {}; }
        return decodeAdpcm(data, book, true);
    }
    if (codec == CODEC_S16 || codec == CODEC_S16_INMEMORY) return decodeS16(data);
    if (codec == CODEC_S8) return decodeS8(data);
    ok = false; // CODEC_REVERB / CODEC_UNK6
    return {};
}

// ── WAV writer: byte-identical to Python stdlib `wave` (canonical 44-byte RIFF, 16-bit mono PCM) ──────
bool writeWavMono16(const std::string& path, const std::vector<int>& samples, long long rate) {
    long long framerate = rate > 0 ? rate : 1;
    std::uint32_t datalength = static_cast<std::uint32_t>(samples.size() * 2);
    std::vector<std::uint8_t> buf;
    buf.reserve(44 + samples.size() * 2);
    auto put4 = [&](const char* s) { buf.insert(buf.end(), s, s + 4); };
    auto putLE32 = [&](std::uint32_t v) {
        buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF);
        buf.push_back((v >> 16) & 0xFF); buf.push_back((v >> 24) & 0xFF);
    };
    auto putLE16 = [&](std::uint16_t v) { buf.push_back(v & 0xFF); buf.push_back((v >> 8) & 0xFF); };
    put4("RIFF");
    putLE32(36 + datalength);
    put4("WAVE");
    put4("fmt ");
    putLE32(16);
    putLE16(1); // WAVE_FORMAT_PCM
    putLE16(1); // nchannels
    putLE32(static_cast<std::uint32_t>(framerate));
    putLE32(static_cast<std::uint32_t>(framerate * 2)); // byterate = nchannels*framerate*sampwidth
    putLE16(2); // blockalign = nchannels*sampwidth
    putLE16(16); // bits per sample
    put4("data");
    putLE32(datalength);
    for (int s : samples) {
        std::uint16_t u = static_cast<std::uint16_t>(static_cast<std::int16_t>(s));
        putLE16(u);
    }
    return writeFileBytes(path, buf.data(), buf.size());
}

// ── sample-key + per-sample JSON meta ────────────────────────────────────────────────────────────────
std::string sampleKey(const std::string& bankName, std::uint32_t addr, std::uint32_t size) {
    char b[256];
    std::snprintf(b, sizeof(b), "%s__0x%X__%u", bankName.c_str(), addr, size);
    return b;
}

// bank resolution result
struct BankEntry {
    bool valid = false;
    std::string name;
    long long offset = 0, size = 0, medium = 0;
};
BankEntry bankEntry(const AudioTables& t, int bank_id) {
    BankEntry be;
    if (bank_id < 0 || bank_id >= static_cast<int>(t.diskBank.size())) return be;
    const AudioBankEntry& e = t.diskBank[bank_id];
    be.valid = true;
    be.name = e.name;
    be.offset = e.offset;
    be.size = e.size;
    be.medium = e.medium;
    return be;
}

std::string sampleMetaJson(const Sample& s, float tuning, const BankEntry& be) {
    std::string j = "{";
    j += "\"codec\":" + jStr(codecName(s.codec));
    j += ",\"bankSel\":" + std::to_string(s.bank_sel);
    j += ",\"sampleAddr\":" + jStr(hexU(s.sample_addr));
    j += ",\"sizeBytes\":" + std::to_string(s.size);
    j += ",\"tuning\":" + jFloat(tuning);
    long long rate = (tuning > 0) ? pyRound(static_cast<double>(tuning) * SYNTHESIS_RATE_HZ) : 0;
    j += ",\"playbackRateHz\":" + std::to_string(rate);
    j += ",\"bank\":" + std::string(be.valid ? jStr(be.name) : "null");
    j += ",\"bankMedium\":" + jStr(mediumName(be.valid ? static_cast<int>(be.medium) : -1));
    j += ",\"decodable\":" + std::string((be.valid && be.medium == MEDIUM_CART) ? "true" : "false");
    if (s.book.valid) {
        j += ",\"book\":{\"order\":" + std::to_string(s.book.order);
        j += ",\"numPredictors\":" + std::to_string(s.book.numPredictors);
        j += ",\"coefCount\":" + std::to_string(s.book.coefs.size());
        j += ",\"coefCountExpected\":" +
             std::to_string(8 * s.book.order * s.book.numPredictors) + "}";
    } else {
        j += ",\"book\":null";
    }
    if (s.loop.valid) {
        j += ",\"loop\":{\"start\":" + std::to_string(s.loop.start);
        j += ",\"end\":" + std::to_string(s.loop.end);
        j += ",\"count\":" + std::to_string(s.loop.count);
        j += ",\"hasPredictorState\":" + std::string(s.loop.hasState ? "true" : "false") + "}";
    } else {
        j += ",\"loop\":null";
    }
    j += "}";
    return j;
}

// Font descriptor collected from the archives + tables.
struct FontDesc {
    std::string label, origin, ekSymbol;
    int fontIndex = 0;
    std::vector<std::uint8_t> data;
    int num_inst = 0, num_drums = 0, num_sfx = 0;
    int bank_ids[2] = {0, 0};
    int inst_base = 1;
    long long declared_size = 0;
};

std::vector<std::uint8_t> ekPayload(const Archive* disk, const std::string& symbol,
                                    long long expect_len) {
    std::string key = "ek/" + symbol;
    auto raw = disk->readRaw(key);
    if (!raw) return {};
    std::vector<std::uint8_t>& data = *raw;
    for (std::size_t start : {std::size_t(0), kOtrHeaderSize, kOtrHeaderSize + 4}) {
        if (data.size() >= start &&
            static_cast<long long>(data.size() - start) == expect_len) {
            return std::vector<std::uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(start), data.end());
        }
    }
    return data; // last resort: whole entry
}

std::vector<FontDesc> collectFonts(Context& ctx, const AudioTables& t, const Archive* disk,
                                   const std::map<std::string, long long>& ekLens) {
    std::vector<FontDesc> fonts;
    // -- CART fonts (rom gSoundFontTableData, inst_base 1) --
    auto bankBlob = ctx.cart->readStripped("audio_blob/audio_bank", kBlobPayloadOffset);
    if (bankBlob) {
        for (std::size_t idx = 0; idx < t.romFont.size(); ++idx) {
            const AudioFontEntry& e = t.romFont[idx];
            long long off = e.offset, size = e.size;
            FontDesc fd;
            fd.label = "cart_" + e.name;
            fd.origin = "cart";
            fd.fontIndex = static_cast<int>(idx);
            std::size_t start = static_cast<std::size_t>(off);
            std::size_t stop = static_cast<std::size_t>(off + size);
            if (start > bankBlob->size()) start = bankBlob->size();
            if (stop > bankBlob->size()) stop = bankBlob->size();
            if (start < stop) fd.data.assign(bankBlob->begin() + start, bankBlob->begin() + stop);
            fd.num_inst = (e.sd2 >> 8) & 0xFF;
            fd.num_drums = e.sd2 & 0xFF;
            fd.num_sfx = static_cast<int>(e.sfx);
            fd.bank_ids[0] = (e.sd1 >> 8) & 0xFF;
            fd.bank_ids[1] = e.sd1 & 0xFF;
            fd.inst_base = 1;
            fd.declared_size = size;
            fonts.push_back(std::move(fd));
        }
    }
    // -- EK fonts (disk gSoundFontTable, inst_base 2) --
    if (disk != nullptr) {
        int n = static_cast<int>(sizeof(kEkFontSymbols) / sizeof(kEkFontSymbols[0]));
        for (int idx = 0; idx < n; ++idx) {
            if (idx >= static_cast<int>(t.diskFont.size())) break;
            const AudioFontEntry& e = t.diskFont[idx];
            long long declared = e.size;
            std::string symbol = kEkFontSymbols[idx];
            long long expect = declared;
            auto it = ekLens.find(symbol);
            if (it != ekLens.end()) expect = it->second;
            std::vector<std::uint8_t> payload = ekPayload(disk, symbol, expect);
            if (payload.empty() && !disk->has("ek/" + symbol)) continue;
            FontDesc fd;
            fd.label = "ek_" + e.name;
            fd.origin = "ek";
            fd.ekSymbol = symbol;
            fd.fontIndex = idx;
            fd.data = std::move(payload);
            fd.num_inst = (e.sd2 >> 8) & 0xFF;
            fd.num_drums = e.sd2 & 0xFF;
            fd.num_sfx = static_cast<int>(e.sfx);
            fd.bank_ids[0] = (e.sd1 >> 8) & 0xFF;
            fd.bank_ids[1] = e.sd1 & 0xFF;
            fd.inst_base = 2;
            fd.declared_size = declared;
            fonts.push_back(std::move(fd));
        }
    }
    return fonts;
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// Shared loaders (declared in dump_common.h)
// ══════════════════════════════════════════════════════════════════════════════════════════════════
bool loadAudioTables(const std::string& path, AudioTables& out) {
    if (path.empty() || !fileExists(path)) return false;
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (...) {
        return false;
    }
    if (!root["audio"]) return false;
    YAML::Node a = root["audio"];
    auto readFont = [](const YAML::Node& arr, bool withOff) {
        std::vector<AudioFontEntry> v;
        if (!arr || !arr.IsSequence()) return v;
        for (const auto& e : arr) {
            AudioFontEntry f;
            f.name = e["name"] ? e["name"].as<std::string>() : "";
            f.size = e["size"] ? e["size"].as<long long>() : 0;
            f.sd1 = e["sd1"] ? e["sd1"].as<long long>() : 0;
            f.sd2 = e["sd2"] ? e["sd2"].as<long long>() : 0;
            f.sfx = e["sfx"] ? e["sfx"].as<long long>() : 0;
            if (withOff) f.offset = e["offset"] ? e["offset"].as<long long>() : 0;
            v.push_back(std::move(f));
        }
        return v;
    };
    auto readSeq = [](const YAML::Node& arr, bool withOff) {
        std::vector<AudioSeqEntry> v;
        if (!arr || !arr.IsSequence()) return v;
        for (const auto& e : arr) {
            AudioSeqEntry s;
            s.name = e["name"] ? e["name"].as<std::string>() : "";
            s.size = e["size"] ? e["size"].as<long long>() : 0;
            if (withOff) s.offset = e["offset"] ? e["offset"].as<long long>() : 0;
            v.push_back(std::move(s));
        }
        return v;
    };
    out.romFont = readFont(a["romFont"], true);
    out.diskFont = readFont(a["diskFont"], false);
    out.romSeq = readSeq(a["romSeq"], true);
    out.diskSeq = readSeq(a["diskSeq"], false);
    if (a["diskBank"] && a["diskBank"].IsSequence()) {
        for (const auto& e : a["diskBank"]) {
            AudioBankEntry b;
            b.name = e["name"] ? e["name"].as<std::string>() : "";
            b.offset = e["offset"] ? e["offset"].as<long long>() : 0;
            b.size = e["size"] ? e["size"].as<long long>() : 0;
            b.medium = e["medium"] ? e["medium"].as<long long>() : 0;
            out.diskBank.push_back(std::move(b));
        }
    }
    out.ok = !out.diskBank.empty();
    return out.ok;
}

std::map<std::string, long long> loadEkManifestLens(const std::string& manifestPath) {
    std::map<std::string, long long> lens;
    if (manifestPath.empty() || !fileExists(manifestPath)) return lens;
    std::ifstream in(manifestPath, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::istringstream ls(line);
        std::vector<std::string> parts;
        std::string tok;
        while (ls >> tok) parts.push_back(tok);
        if (parts.size() < 4) continue;
        if (parts[3] == "FZX:SEQUENCE" || parts[3] == "FZX:SOUNDFONT") {
            try {
                lens[parts[0]] = std::stoll(parts[2]);
            } catch (...) {
            }
        }
    }
    return lens;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// audio class
// ══════════════════════════════════════════════════════════════════════════════════════════════════
ClassResult runAudio(Context& ctx) {
    ClassResult res;
    res.name = "audio";

    AudioTables tables;
    if (!loadAudioTables(ctx.tablesDataPath, tables)) {
        res.hardError = true;
        res.errorMsg = "audio tables data (audio_tables_data.json 'audio' section) not found -- the "
                       "build step (tools/gen_dump_tables_data.py) must emit it into decomp-recipes";
        return res;
    }
    auto audioTableOpt = ctx.cart->readStripped("audio_blob/audio_table", kBlobPayloadOffset);
    if (!audioTableOpt) {
        std::printf("  audio: generic.o2r audio_blob/audio_table missing -- cannot decode; skipping\n");
        res.hardError = true;
        res.errorMsg = "audio_blob/audio_table missing from cart archive";
        return res;
    }
    std::vector<std::uint8_t>& audioTable = *audioTableOpt;

    std::string outDir = ctx.dumpDir.empty() ? std::string() : joinPath(ctx.dumpDir, "audio");
    std::string samplesDir = joinPath(outDir, "samples");
    std::string fontsDir = joinPath(outDir, "fonts");
    makeDirs(samplesDir);
    makeDirs(fontsDir);

    const Archive* disk = ctx.disk;
    std::map<std::string, long long> ekLens = loadEkManifestLens(ctx.manifestPath);
    std::vector<FontDesc> fonts = collectFonts(ctx, tables, disk, ekLens);
    if (disk == nullptr)
        std::printf("  audio: note -- EK archive (fzerox-disk.o2r) not found; cart audio only\n");

    std::vector<std::vector<std::string>> manifest;
    std::map<std::string, bool> decodedKeys;
    int dumped = 0, skipped = 0, failed = 0, metaOnly = 0, framingOk = 0;

    for (const FontDesc& fd : fonts) {
        if (!safeOutputComponent(fd.label)) {
            std::fprintf(stderr, "  warn: audio: unsafe font output name '%s'; skipping\n",
                         fd.label.c_str());
            ++failed;
            continue;
        }
        const std::vector<std::uint8_t>& data = fd.data;
        bool sizeMatch = (static_cast<long long>(data.size()) == fd.declared_size);
        if (sizeMatch) ++framingOk;
        ParsedFont pf = parseFont(data, fd.num_inst, fd.num_drums, fd.num_sfx, fd.inst_base);

        // ── build per-font JSON (samples map populated via sample_ref) ──
        std::string samplesJson; // "key":meta pairs
        std::vector<std::string> sampleKeysSeen;
        auto sampleRef = [&](const TunedSample& t) -> std::string {
            if (!t.valid || !t.sample.valid) return "null";
            const Sample& s = t.sample;
            int bank_id = (s.bank_sel != MEDIUM_LBA) ? fd.bank_ids[0] : fd.bank_ids[1];
            BankEntry be = bankEntry(tables, bank_id);
            std::string bankName = be.valid ? be.name : ("bank" + std::to_string(bank_id));
            std::string key = sampleKey(bankName, s.sample_addr, s.size);
            if (std::find(sampleKeysSeen.begin(), sampleKeysSeen.end(), key) == sampleKeysSeen.end()) {
                sampleKeysSeen.push_back(key);
                if (!samplesJson.empty()) samplesJson += ",";
                samplesJson += jStr(key) + ":" + sampleMetaJson(s, t.tuning, be);
            }
            return "{\"sample\":" + jStr(key) + ",\"tuning\":" + jFloat(t.tuning) + "}";
        };

        std::string instrJson;
        for (const Instrument& inst : pf.instruments) {
            if (!instrJson.empty()) instrJson += ",";
            if (!inst.present) { instrJson += "null"; continue; }
            std::string e = "{\"index\":" + std::to_string(inst.index);
            e += ",\"normalRangeLo\":" + std::to_string(inst.normalRangeLo);
            e += ",\"normalRangeHi\":" + std::to_string(inst.normalRangeHi);
            e += ",\"adsrDecayIndex\":" + std::to_string(inst.adsrDecayIndex);
            e += ",\"envelopePoints\":" + std::to_string(inst.envelopePoints);
            e += ",\"lowPitch\":" + sampleRef(inst.lowPitch);
            e += ",\"normalPitch\":" + sampleRef(inst.normalPitch);
            e += ",\"highPitch\":" + sampleRef(inst.highPitch);
            e += "}";
            instrJson += e;
        }
        std::string drumsJson;
        for (const Drum& dr : pf.drums) {
            if (!drumsJson.empty()) drumsJson += ",";
            if (!dr.present) { drumsJson += "null"; continue; }
            std::string e = "{\"index\":" + std::to_string(dr.index);
            e += ",\"adsrDecayIndex\":" + std::to_string(dr.adsrDecayIndex);
            e += ",\"pan\":" + std::to_string(dr.pan);
            e += ",\"sample\":" + sampleRef(dr.tunedSample);
            e += "}";
            drumsJson += e;
        }
        std::string sfxJson;
        for (const SoundEffect& se : pf.soundEffects) {
            if (!sfxJson.empty()) sfxJson += ",";
            if (!se.present) { sfxJson += "null"; continue; }
            std::string e = "{\"index\":" + std::to_string(se.index);
            e += ",\"sample\":" + sampleRef(se.tunedSample);
            e += "}";
            sfxJson += e;
        }

        // sampleBankNames
        std::string bankNamesJson;
        for (int b : {fd.bank_ids[0], fd.bank_ids[1]}) {
            if (!bankNamesJson.empty()) bankNamesJson += ",";
            BankEntry be = bankEntry(tables, b);
            bankNamesJson += jStr(be.valid ? be.name : "none");
        }

        std::string fontJson = "{";
        fontJson += "\"label\":" + jStr(fd.label);
        fontJson += ",\"origin\":" + jStr(fd.origin);
        fontJson += ",\"fontIndex\":" + std::to_string(fd.fontIndex);
        fontJson += ",\"declaredSize\":" + std::to_string(fd.declared_size);
        fontJson += ",\"actualSize\":" + std::to_string(data.size());
        fontJson += ",\"sizeMatchesTable\":" + std::string(sizeMatch ? "true" : "false");
        fontJson += ",\"numInstruments\":" + std::to_string(fd.num_inst);
        fontJson += ",\"numDrums\":" + std::to_string(fd.num_drums);
        fontJson += ",\"numSfx\":" + std::to_string(fd.num_sfx);
        fontJson += ",\"sampleBankIds\":[" + std::to_string(fd.bank_ids[0]) + "," +
                    std::to_string(fd.bank_ids[1]) + "]";
        fontJson += ",\"sampleBankNames\":[" + bankNamesJson + "]";
        fontJson += ",\"instBase\":" + std::to_string(fd.inst_base);
        fontJson += ",\"instruments\":[" + instrJson + "]";
        fontJson += ",\"drums\":[" + drumsJson + "]";
        fontJson += ",\"soundEffects\":[" + sfxJson + "]";
        fontJson += ",\"samples\":{" + samplesJson + "}";
        if (fd.origin == "ek") fontJson += ",\"ekSymbol\":" + jStr(fd.ekSymbol);
        fontJson += "}";

        // ── decode / write each distinct sample this font references ──
        for (const SampleRef& sr : fontSamples(pf)) {
            const Sample& s = sr.sample;
            int bank_id = (s.bank_sel != MEDIUM_LBA) ? fd.bank_ids[0] : fd.bank_ids[1];
            BankEntry be = bankEntry(tables, bank_id);
            std::string bankName = be.valid ? be.name : ("bank" + std::to_string(bank_id));
            std::string key = sampleKey(bankName, s.sample_addr, s.size);
            if (!safeOutputComponent(key)) {
                std::fprintf(stderr, "  warn: audio: unsafe sample output name '%s'; skipping\n",
                             key.c_str());
                continue;
            }
            long long rate = (sr.tuning > 0)
                                 ? pyRound(static_cast<double>(sr.tuning) * SYNTHESIS_RATE_HZ)
                                 : 0;
            std::string wavPath = joinPath(samplesDir, key + ".wav");
            std::string cname = codecName(s.codec);

            if (!be.valid || be.medium != MEDIUM_CART) {
                std::string status = "no-waveform-data(" +
                                     std::string(be.valid ? be.name : "unknown-bank") + ")";
                manifest.push_back({fd.label, sr.role, key, cname, std::to_string(rate), "0", status});
                ++metaOnly;
                continue;
            }
            std::size_t start = static_cast<std::size_t>(be.offset + s.sample_addr);
            std::vector<std::uint8_t> raw;
            if (start <= audioTable.size()) {
                std::size_t stop = start + s.size;
                if (stop > audioTable.size()) stop = audioTable.size();
                raw.assign(audioTable.begin() + static_cast<std::ptrdiff_t>(start),
                           audioTable.begin() + static_cast<std::ptrdiff_t>(stop));
            }
            if (raw.size() < s.size || s.size == 0) {
                manifest.push_back({fd.label, sr.role, key, cname, std::to_string(rate), "0",
                                    "short-read"});
                ++failed;
                continue;
            }
            bool exists = fileExists(wavPath);
            bool inDecoded = decodedKeys.count(key) != 0;
            if (inDecoded || exists) {
                decodedKeys[key] = true;
                long long frames = exists
                                       ? static_cast<long long>(fs::file_size(wavPath)) / 2
                                       : 0;
                std::string status = (decodedKeys.count(key) && !exists) ? "shared" : "ok-existing";
                manifest.push_back({fd.label, sr.role, key, cname, std::to_string(rate),
                                    std::to_string(frames), status});
                ++skipped;
                continue;
            }
            bool ok = true;
            std::vector<int> pcm = decodeSampleBytes(s.codec, raw, s.book, ok);
            if (!ok) {
                manifest.push_back({fd.label, sr.role, key, cname, std::to_string(rate), "0",
                                    "codec-unsupported"});
                ++failed;
                continue;
            }
            long long wavRate = rate > 0 ? rate : SYNTHESIS_RATE_HZ;
            writeWavMono16(wavPath, pcm, wavRate);
            decodedKeys[key] = true;
            ++dumped;
            bool loopOk = true;
            if (s.loop.valid && s.loop.count != 0)
                loopOk = s.loop.end <= static_cast<std::uint32_t>(pcm.size());
            manifest.push_back({fd.label, sr.role, key, cname, std::to_string(wavRate),
                                std::to_string(pcm.size()), loopOk ? "ok" : "ok-LOOPEND-OOB"});
        }

        std::string jsonPath = joinPath(fontsDir, fd.label + ".json");
        if (!fileExists(jsonPath)) writeFileText(jsonPath, fontJson);
    }

    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "font\trole\tsampleKey\tcodec\tplaybackRateHz\tframes\tstatus   "
                  "(WAV = VADPCM decode of the cart audio_table blob; LBA banks metadata-only)",
                  manifest);
    std::printf("  audio: %d WAV decoded, %d skipped/shared, %d metadata-only (LBA), %d failed "
                "(%zu fonts, framing %d/%zu size-matched)\n",
                dumped, skipped, metaOnly, failed, fonts.size(), framingOk, fonts.size());

    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(manifest.size());
    return res;
}

} // namespace gdxdump
