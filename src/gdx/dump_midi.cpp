// `midi` dump class — music sequences -> .mid (+ per-sequence instrument-map sidecar + linear
// disassembly + manifest). Mirrors tools/gen_dump_all_audio.py SequenceDumpClass line-for-line.
//
// The aseq bytecode is a 3-level script (player -> channels -> layers) decoded per
// decomp/src/audio/disk/lib/seqplayer.c. This is a LINEAR disassembler (control flow is markered, not
// taken). The .mid and .disasm.txt outputs are BYTE-IDENTICAL to the oracle's (VLQ encoding, full
// status bytes, event ordering, text formatting all mirrored exactly); .instrmap.json is compared by
// parsed structure (see verify_native_dump.py).

#include "gdx/dump_all.h"
#include "gdx/dump_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace gdxdump {
namespace {

// aseq control-flow opcodes (aseq.h:5-19)
constexpr int OP_RBLTZ = 0xF2, OP_RBEQZ = 0xF3, OP_RJUMP = 0xF4, OP_BGEZ = 0xF5, OP_BREAK = 0xF6,
              OP_LOOPEND = 0xF7, OP_LOOP = 0xF8, OP_BLTZ = 0xF9, OP_BEQZ = 0xFA, OP_JUMP = 0xFB,
              OP_CALL = 0xFC, OP_DELAY = 0xFD, OP_DELAY1 = 0xFE, OP_END = 0xFF;

// sSeqInstructionArgsTable (seqplayer.c:44-126), byte index = cmd - 0xB0. Bit-packed abcUUUnn: nn=arg
// count, a/b/c=1 if that arg is s16. Verified equal to the decomp-parsed table by the oracle.
const std::uint8_t kSeqArgsTable[80] = {
    0x81, 0x00, 0x81, 0x01, 0x00, 0x00, 0x00, 0x81, 0x01, 0x01, 0x01, 0x42, 0x81, 0x81, 0x00, 0x00,
    0x00, 0x01, 0x81, 0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x81, 0x81,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x81, 0x01, 0x01, 0x01, 0x81, 0x01,
    0x01, 0x03, 0x03, 0x01, 0x00, 0x01, 0x01, 0x81, 0x03, 0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0x82,
    0x00, 0x01, 0x01, 0x01, 0x01, 0x81, 0x00, 0x00, 0x01, 0x81, 0x81, 0x81, 0x81, 0x00, 0x00, 0x00,
};

// SEQ_* enum order (disk config) -> ek_slice_manifest sequence symbol.
const char* kEkSeqSymbols[] = {
    "aAudioSeqDDGuitar", "aAudioSeqDDSE", "aAudioSeqDDMuteCity", "aAudioSeqDDSilence",
    "aAudioSeqDDSandOcean", "aAudioSeqDDPortTown", "aAudioSeqDDBigBlue", "aAudioSeqDDDevilsForest",
    "aAudioSeqDDRedCanyon", "aAudioSeqDDSector", "aAudioSeqDDWhiteLand", "aAudioSeqDDRainbowRoad",
    "aAudioSeqDDNew03", "aAudioSeqDDNew02", "aAudioSeqDDNew01", "aAudioSeqDDNew04",
    "aAudioSeqDDTitle", "aAudioSeqDDSelect", "aAudioSeqDDOption", "aAudioSeqDDDeathRace",
    "aAudioSeqDDCourseEditor", "aAudioSeqDDMachineEditor", "aAudioSeqDDEADDemo",
};

constexpr int kPpqn = 48;

// Python3 round() (round half to even), for the non-negative tempo conversion.
long long pyRound(double x) {
    double f = std::floor(x);
    double diff = x - f;
    if (diff < 0.5) return static_cast<long long>(f);
    if (diff > 0.5) return static_cast<long long>(f) + 1;
    long long fi = static_cast<long long>(f);
    return (fi % 2 == 0) ? fi : fi + 1;
}

// ── bounded PC reader (big-endian). Reading past the end throws (mirrors Python's IndexError, which
// the oracle's run() catches per-sequence and marks ERROR). ──────────────────────────────────────────
class Reader {
  public:
    explicit Reader(const std::vector<std::uint8_t>& d) : d_(d) {}
    std::size_t pc = 0;
    bool eof() const { return pc >= d_.size(); }
    std::size_t size() const { return d_.size(); }
    int u8() {
        if (pc >= d_.size()) throw std::out_of_range("aseq pc past end");
        return d_[pc++];
    }
    int s8() {
        int v = u8();
        return v >= 128 ? v - 256 : v;
    }
    int s16() {
        if (pc + 1 >= d_.size()) throw std::out_of_range("aseq pc past end");
        int v = (d_[pc] << 8) | d_[pc + 1];
        pc += 2;
        return v >= 0x8000 ? v - 0x10000 : v;
    }
    int u16() {
        if (pc + 1 >= d_.size()) throw std::out_of_range("aseq pc past end");
        int v = (d_[pc] << 8) | d_[pc + 1];
        pc += 2;
        return v;
    }
    int compressed_u16() {
        int v = u8();
        if (v & 0x80) v = ((v & 0x7F) << 8) | u8();
        return v;
    }

  private:
    const std::vector<std::uint8_t>& d_;
};

// Consume the sSeqInstructionArgsTable-declared args for cmd in 0xB0..0xF1; return them.
std::vector<int> readTableArgs(Reader& rdr, int cmd) {
    std::uint8_t packed = kSeqArgsTable[cmd - 0xB0];
    int n = packed & 3;
    std::vector<int> args;
    std::uint8_t hb = packed;
    for (int i = 0; i < n; ++i) {
        if (hb & 0x80)
            args.push_back(rdr.s16());
        else
            args.push_back(rdr.u8());
        hb = static_cast<std::uint8_t>((hb << 1) & 0xFF);
    }
    return args;
}

// ── VLQ (big-endian variable-length quantity, MIDI standard) ──────────────────────────────────────────
std::vector<std::uint8_t> vlq(long long n) {
    if (n < 0) n = 0;
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(n & 0x7F));
    n >>= 7;
    while (n > 0) {
        out.insert(out.begin(), static_cast<std::uint8_t>((n & 0x7F) | 0x80));
        n >>= 7;
    }
    return out;
}

// ── MIDI track: accumulate (tick, order, bytes), serialise with full status bytes ────────────────────
class MidiTrack {
  public:
    void add(long long tick, std::vector<std::uint8_t> data) {
        events_.emplace_back(tick, seq_++, std::move(data));
    }
    void noteOn(long long tick, int ch, int key, int vel) {
        add(tick, {static_cast<std::uint8_t>(0x90 | (ch & 0xF)), static_cast<std::uint8_t>(key & 0x7F),
                   static_cast<std::uint8_t>(vel & 0x7F)});
    }
    void noteOff(long long tick, int ch, int key) {
        add(tick, {static_cast<std::uint8_t>(0x80 | (ch & 0xF)), static_cast<std::uint8_t>(key & 0x7F),
                   0x40});
    }
    void controller(long long tick, int ch, int cc, int val) {
        add(tick, {static_cast<std::uint8_t>(0xB0 | (ch & 0xF)), static_cast<std::uint8_t>(cc & 0x7F),
                   static_cast<std::uint8_t>(val & 0x7F)});
    }
    void program(long long tick, int ch, int prog) {
        add(tick, {static_cast<std::uint8_t>(0xC0 | (ch & 0xF)), static_cast<std::uint8_t>(prog & 0x7F)});
    }
    void metaText(long long tick, int metaType, const std::string& text) {
        std::vector<std::uint8_t> b;
        for (unsigned char c : text) b.push_back(c < 0x80 ? c : static_cast<std::uint8_t>('?'));
        std::vector<std::uint8_t> data = {0xFF, static_cast<std::uint8_t>(metaType)};
        std::vector<std::uint8_t> len = vlq(static_cast<long long>(b.size()));
        data.insert(data.end(), len.begin(), len.end());
        data.insert(data.end(), b.begin(), b.end());
        add(tick, std::move(data));
    }
    void tempo(long long tick, long long usPerQuarter) {
        add(tick, {0xFF, 0x51, 0x03, static_cast<std::uint8_t>((usPerQuarter >> 16) & 0xFF),
                   static_cast<std::uint8_t>((usPerQuarter >> 8) & 0xFF),
                   static_cast<std::uint8_t>(usPerQuarter & 0xFF)});
    }
    std::vector<std::uint8_t> serialise() {
        std::sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
            if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) < std::get<0>(b);
            return std::get<1>(a) < std::get<1>(b);
        });
        std::vector<std::uint8_t> body;
        long long last = 0;
        for (const auto& ev : events_) {
            std::vector<std::uint8_t> dt = vlq(std::get<0>(ev) - last);
            last = std::get<0>(ev);
            body.insert(body.end(), dt.begin(), dt.end());
            const std::vector<std::uint8_t>& data = std::get<2>(ev);
            body.insert(body.end(), data.begin(), data.end());
        }
        std::vector<std::uint8_t> eot = vlq(0);
        eot.push_back(0xFF);
        eot.push_back(0x2F);
        eot.push_back(0x00);
        body.insert(body.end(), eot.begin(), eot.end());
        std::vector<std::uint8_t> out;
        const char* mtrk = "MTrk";
        out.insert(out.end(), mtrk, mtrk + 4);
        std::uint32_t len = static_cast<std::uint32_t>(body.size());
        out.push_back((len >> 24) & 0xFF);
        out.push_back((len >> 16) & 0xFF);
        out.push_back((len >> 8) & 0xFF);
        out.push_back(len & 0xFF);
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

  private:
    using Event = std::tuple<long long, int, std::vector<std::uint8_t>>;
    std::vector<Event> events_;
    int seq_ = 0;
};

// ── disasm helpers matching the oracle's exact format strings ─────────────────────────────────────────
std::string fmtNote(long long tick, int key, int vel, int delay, int gate) {
    char b[128];
    std::snprintf(b, sizeof(b), "    +%-6lld NOTE key=%d vel=%d delay=%d gate=%d", tick, key, vel,
                  delay, gate);
    return b;
}

struct WalkedChannel {
    bool largeNotes = false;
    int instrument = 0, pan = 64, vol = 100, transpose = 0;
    bool hasFont = false;
    int font = 0;
    std::vector<std::tuple<int, int, bool, int, int>> layers; // (low, ptr, large, instrument, transpose)
};

// ── flow marker: consume a control-flow op and drop a MIDI marker; returns true for END ──────────────
bool flowMarker(Reader& rdr, int cmd, MidiTrack& track, int midi_ch, long long tick,
                std::vector<std::string>& disasm, const char* level) {
    char b[128];
    if (cmd == OP_END) {
        std::snprintf(b, sizeof(b), "    +%-6lld END", tick);
        disasm.push_back(b);
        return true;
    }
    if (cmd == OP_DELAY) {
        int d = rdr.compressed_u16();
        std::snprintf(b, sizeof(b), "    +%-6lld DELAY %d", tick, d);
        disasm.push_back(b);
        return false;
    }
    if (cmd == OP_DELAY1) return false;
    if (cmd == OP_LOOP || cmd == OP_RBLTZ || cmd == OP_RBEQZ || cmd == OP_RJUMP)
        rdr.u8();
    else if (cmd == OP_JUMP || cmd == OP_CALL || cmd == OP_BEQZ || cmd == OP_BLTZ || cmd == OP_BGEZ)
        rdr.s16();
    if (cmd == OP_LOOP || cmd == OP_LOOPEND) {
        track.controller(tick, midi_ch, 111, 0);
        std::snprintf(b, sizeof(b), "    +%-6lld LOOP/LOOPEND -> CC111 marker", tick);
        disasm.push_back(b);
    } else {
        char t[32];
        std::snprintf(t, sizeof(t), "%s_flow_%02X", level, cmd);
        track.metaText(tick, 0x06, t);
        std::snprintf(b, sizeof(b), "    +%-6lld FLOW 0x%02X -> marker (not taken)", tick, cmd);
        disasm.push_back(b);
    }
    return false;
}

// ── layer disassembly: produce note events on a track ────────────────────────────────────────────────
int walkLayer(const std::vector<std::uint8_t>& seq, std::size_t startPc, MidiTrack& track, int midi_ch,
              bool largeNotes, int transpose, std::vector<std::string>& disasm, int cap = 8000) {
    Reader rdr(seq);
    rdr.pc = startPc;
    long long tick = 0;
    int lastDelay = 0, shortDefaultDelay = 0, gateTime = 0;
    int noteCount = 0, steps = 0;
    std::vector<std::pair<long long, int>> active; // (off_tick, key)

    auto flushOffs = [&](long long upto) {
        for (const auto& a : active)
            if (a.first <= upto) track.noteOff(a.first, midi_ch, a.second);
        std::vector<std::pair<long long, int>> keep;
        for (const auto& a : active)
            if (a.first > upto) keep.push_back(a);
        active.swap(keep);
    };

    while (!rdr.eof() && steps < cap) {
        ++steps;
        int cmd = rdr.u8();
        if (cmd < 0xC0) {
            int noteType = cmd & 0xC0;
            int velocity = 100;
            int delay;
            if (largeNotes) {
                if (noteType == 0x00) {
                    delay = rdr.compressed_u16();
                    velocity = rdr.u8();
                    gateTime = rdr.u8();
                    lastDelay = delay;
                } else if (noteType == 0x40) {
                    delay = rdr.compressed_u16();
                    velocity = rdr.u8();
                    gateTime = 0;
                    lastDelay = delay;
                } else {
                    delay = lastDelay;
                    velocity = rdr.u8();
                    gateTime = rdr.u8();
                }
            } else {
                if (noteType == 0x00) {
                    delay = rdr.compressed_u16();
                    lastDelay = delay;
                } else if (noteType == 0x40) {
                    delay = shortDefaultDelay;
                } else {
                    delay = lastDelay;
                }
            }
            velocity = std::max(1, std::min(127, velocity));
            int key = (cmd & 0x3F) + 21 + transpose;
            long long gateDelay = (static_cast<long long>(gateTime) * delay) >> 8;
            flushOffs(tick);
            if (key >= 0 && key <= 127) {
                track.noteOn(tick, midi_ch, key, velocity);
                long long offAt = tick + (gateDelay > 0 ? gateDelay : std::max(1, delay - 1));
                active.emplace_back(offAt, key);
                ++noteCount;
                disasm.push_back(fmtNote(tick, key, velocity, delay, gateTime));
            }
            tick += delay;
            continue;
        }
        if (cmd == 0xC0) { // LDELAY (rest)
            int delay = rdr.compressed_u16();
            char b[64];
            std::snprintf(b, sizeof(b), "    +%-6lld REST delay=%d", tick, delay);
            disasm.push_back(b);
            tick += delay;
            continue;
        }
        if (cmd >= OP_RBLTZ) { // 0xF2..0xFF control flow
            if (flowMarker(rdr, cmd, track, midi_ch, tick, disasm, "layer")) break;
            continue;
        }
        if (cmd == 0xC1 || cmd == 0xC2 || cmd == 0xC6 || cmd == 0xC9 || cmd == 0xCA || cmd == 0xCD ||
            cmd == 0xCE || cmd == 0xCF) {
            rdr.u8();
        } else if (cmd == 0xC3) { // SHORTDELAY
            shortDefaultDelay = rdr.compressed_u16();
        } else if (cmd == 0xC4 || cmd == 0xC5 || cmd == 0xC8 || cmd == 0xCC) {
            // LEGATO / NOLEGATO / NOPORTAMENTO / NODRUMPAN (0 args)
        } else if (cmd == 0xC7) { // PORTAMENTO
            int mode = rdr.u8();
            rdr.u8();
            if (mode & 0x80)
                rdr.u8();
            else
                rdr.compressed_u16();
        } else if (cmd == 0xCB) { // ENV
            rdr.s16();
            rdr.u8();
        } else if (cmd >= 0xD0 && cmd <= 0xEF) {
            // LDSHORTVEL / LDSHORTGATE (low nibble is the arg; no extra bytes)
        } else {
            char b[80];
            std::snprintf(b, sizeof(b), "    +%-6lld LAYER_OP 0x%02X (unmodelled -> marker)", tick, cmd);
            disasm.push_back(b);
            char t[32];
            std::snprintf(t, sizeof(t), "layer_op_%02X", cmd);
            track.metaText(tick, 0x06, t);
        }
    }
    flushOffs((tick + 1) << 28); // close any still-open notes at the end
    for (const auto& a : active) track.noteOff(std::max(a.first, tick), midi_ch, a.second);
    return noteCount;
}

// ── channel disassembly: gather instrument/pan/vol/layers ─────────────────────────────────────────────
WalkedChannel walkChannel(const std::vector<std::uint8_t>& seq, std::size_t startPc,
                          std::vector<std::string>& disasm, int cap = 8000) {
    Reader rdr(seq);
    rdr.pc = startPc;
    WalkedChannel ch;
    int steps = 0;
    char b[128];
    std::snprintf(b, sizeof(b), "  channel @0x%X", static_cast<unsigned>(startPc));
    disasm.push_back(b);
    while (!rdr.eof() && steps < cap) {
        ++steps;
        int cmd = rdr.u8();
        if (cmd >= 0xB0) {
            if (cmd >= OP_RBLTZ) {
                if (cmd == OP_END) {
                    disasm.push_back("    END");
                    break;
                }
                if (cmd == OP_DELAY) {
                    rdr.compressed_u16();
                    continue;
                }
                if (cmd == OP_DELAY1) continue;
                if (cmd == OP_LOOP || cmd == OP_RBLTZ || cmd == OP_RBEQZ || cmd == OP_RJUMP)
                    rdr.u8();
                else if (cmd == OP_JUMP || cmd == OP_CALL || cmd == OP_BEQZ || cmd == OP_BLTZ ||
                         cmd == OP_BGEZ)
                    rdr.s16();
                continue;
            }
            std::vector<int> args = readTableArgs(rdr, cmd);
            if (cmd == 0xC1)
                ch.instrument = args[0];
            else if (cmd == 0xC3)
                ch.largeNotes = false;
            else if (cmd == 0xC4)
                ch.largeNotes = true;
            else if (cmd == 0xC6) {
                ch.font = args[0];
                ch.hasFont = true;
            } else if (cmd == 0xDB)
                ch.transpose = args[0] < 128 ? args[0] : args[0] - 256;
            else if (cmd == 0xDD)
                ch.pan = args[0];
            else if (cmd == 0xDF)
                ch.vol = args[0];
            continue;
        }
        if (cmd >= 0x70) { // layer/testlayer/stio group
            int grp = cmd & 0xF8;
            int low = cmd & 0x7;
            if (grp == 0x88) { // LDLAYER
                int ptr = rdr.u16();
                ch.layers.emplace_back(low, ptr, ch.largeNotes, ch.instrument, ch.transpose);
                std::snprintf(b, sizeof(b), "    LDLAYER %d -> 0x%X (large=%s instr=%d)", low,
                              static_cast<unsigned>(ptr), ch.largeNotes ? "True" : "False",
                              ch.instrument);
                disasm.push_back(b);
            } else if (grp == 0x78) { // RLDLAYER (relative s16)
                rdr.s16();
            }
            continue;
        }
        int grp = cmd & 0xF0;
        if (grp == 0x00) // CDELAY
            continue;
        if (grp == 0x20) // LDCHAN (+s16)
            rdr.s16();
        else if (grp == 0x30 || grp == 0x40) // STCIO / LDCIO (+u8)
            rdr.u8();
        // 0x10 LDSAMPLE, 0x50 SUBIO, 0x60 LDIO: 0 extra
    }
    return ch;
}

// Player-level op arg-byte counts (explicit dispatcher switch, NOT sSeqInstructionArgsTable).
int playerC0Args(int cmd) {
    switch (cmd) {
        case 0xC4: return 2; case 0xC5: return 2; case 0xC6: return 0; case 0xC7: return 3;
        case 0xC8: return 1; case 0xC9: return 1; case 0xCC: return 1; case 0xCD: return 2;
        case 0xCE: return 1; case 0xD0: return 1; case 0xD1: return 2; case 0xD2: return 2;
        case 0xD3: return 1; case 0xD4: return 0; case 0xD5: return 1; case 0xD6: return 2;
        case 0xD7: return 2; case 0xD9: return 1; case 0xDA: return 3; case 0xDB: return 1;
        case 0xDC: return 1; case 0xDD: return 1; case 0xDE: return 1; case 0xDF: return 1;
        case 0xEF: return 3; case 0xF0: return 0; case 0xF1: return 1;
        default: return 0;
    }
}
int playerLowArgs(int grp) {
    switch (grp) {
        case 0x00: return 0; case 0x40: return 0; case 0x50: return 0; case 0x60: return 2;
        case 0x70: return 0; case 0x80: return 0; case 0x90: return 2; case 0xA0: return 2;
        case 0xB0: return 3; // LDSEQ
        default: return 0;
    }
}

// ── player disassembly: tempo + channel starts ────────────────────────────────────────────────────────
void walkPlayer(const std::vector<std::uint8_t>& seq, std::vector<std::string>& disasm, int& tempoOut,
                std::map<int, int>& channels, int cap = 4000) {
    Reader rdr(seq);
    int tempo = 120;
    int steps = 0;
    char b[64];
    disasm.push_back("player @0x0");
    while (!rdr.eof() && steps < cap) {
        ++steps;
        int cmd = rdr.u8();
        if (cmd >= OP_RBLTZ) { // 0xF2+ control flow
            if (cmd == OP_END) {
                disasm.push_back("  END");
                break;
            }
            if (cmd == OP_DELAY) {
                rdr.compressed_u16();
                continue;
            }
            if (cmd == OP_DELAY1) continue;
            if (cmd == OP_LOOP || cmd == OP_RBLTZ || cmd == OP_RBEQZ || cmd == OP_RJUMP)
                rdr.u8();
            else if (cmd == OP_JUMP || cmd == OP_CALL || cmd == OP_BEQZ || cmd == OP_BLTZ ||
                     cmd == OP_BGEZ)
                rdr.s16();
            continue;
        }
        if (cmd >= 0xC0) {
            int n = playerC0Args(cmd);
            if (cmd == 0xDD) { // TEMPO: u8 BPM
                if (rdr.pc < seq.size()) tempo = rdr.u8();
                tempo = tempo > 0 ? tempo : 120;
                std::snprintf(b, sizeof(b), "  TEMPO %d", tempo);
                disasm.push_back(b);
            } else {
                for (int i = 0; i < n; ++i) rdr.u8();
            }
            if (cmd == 0xC6) { // STOP
                disasm.push_back("  STOP");
                break;
            }
            continue;
        }
        int grp = cmd & 0xF0;
        int low = cmd & 0xF;
        if (grp == 0x90) { // LDCHAN (+u16 ptr)
            int ptr = rdr.u16();
            channels[low] = ptr;
            std::snprintf(b, sizeof(b), "  LDCHAN %d -> 0x%X", low, static_cast<unsigned>(ptr));
            disasm.push_back(b);
        } else {
            for (int i = 0; i < playerLowArgs(grp); ++i) rdr.u8();
        }
    }
    tempoOut = tempo;
}

// ── JSON helpers for the instrmap sidecar (compared structurally) ─────────────────────────────────────
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

struct ConvertResult {
    std::vector<MidiTrack> tracks;
    std::string instrMapJson; // "channel_N":{...} pairs
    std::vector<std::string> disasm;
    int tempo = 120;
    int totalNotes = 0;
};

ConvertResult convertSequence(const std::string& label, const std::vector<std::uint8_t>& seq,
                              int fontIndex) {
    ConvertResult r;
    std::map<int, int> channels;
    walkPlayer(seq, r.disasm, r.tempo, channels);

    MidiTrack meta;
    meta.metaText(0, 0x03, label);
    long long usPerQ = pyRound(60000000.0 / std::max(r.tempo, 1));
    meta.tempo(0, usPerQ);
    r.tracks.push_back(std::move(meta));

    for (const auto& kv : channels) { // std::map iterates sorted by key
        int chIndex = kv.first;
        WalkedChannel ch = walkChannel(seq, static_cast<std::size_t>(kv.second), r.disasm);
        MidiTrack trk;
        int midiCh = chIndex & 0xF;
        trk.metaText(0, 0x03, label + "_ch" + std::to_string(chIndex));
        trk.program(0, midiCh, ch.instrument & 0x7F);
        trk.controller(0, midiCh, 7, std::min(127, ch.vol));
        trk.controller(0, midiCh, 10, std::min(127, ch.pan));
        int chNotes = 0;
        for (const auto& ly : ch.layers) {
            int lpc = std::get<1>(ly);
            bool large = std::get<2>(ly);
            int transp = std::get<4>(ly);
            chNotes += walkLayer(seq, static_cast<std::size_t>(lpc), trk, midiCh, large, transp,
                                 r.disasm);
        }
        r.totalNotes += chNotes;
        r.tracks.push_back(std::move(trk));

        std::string e = "{\"midiChannel\":" + std::to_string(midiCh);
        e += ",\"program\":" + std::to_string(ch.instrument);
        e += ",\"soundfontIndex\":" + std::to_string(fontIndex);
        e += ",\"soundfontInstrument\":" +
             jStr("font" + std::to_string(fontIndex) + "_inst" + std::to_string(ch.instrument));
        e += ",\"pan\":" + std::to_string(ch.pan);
        e += ",\"volume\":" + std::to_string(ch.vol);
        e += ",\"transpose\":" + std::to_string(ch.transpose);
        e += ",\"largeNotes\":" + std::string(ch.largeNotes ? "true" : "false");
        e += ",\"layerCount\":" + std::to_string(ch.layers.size());
        e += ",\"notes\":" + std::to_string(chNotes);
        e += "}";
        if (!r.instrMapJson.empty()) r.instrMapJson += ",";
        r.instrMapJson += jStr("channel_" + std::to_string(chIndex)) + ":" + e;
    }
    return r;
}

std::vector<std::uint8_t> writeMidiBytes(std::vector<MidiTrack>& tracks) {
    std::vector<std::uint8_t> out;
    const char* mthd = "MThd";
    out.insert(out.end(), mthd, mthd + 4);
    // pack(">IHHH", 6, 1, ntracks, ppqn)
    std::uint32_t len = 6;
    out.push_back((len >> 24) & 0xFF);
    out.push_back((len >> 16) & 0xFF);
    out.push_back((len >> 8) & 0xFF);
    out.push_back(len & 0xFF);
    auto put16 = [&](std::uint16_t v) {
        out.push_back((v >> 8) & 0xFF);
        out.push_back(v & 0xFF);
    };
    put16(1); // format 1
    put16(static_cast<std::uint16_t>(tracks.size()));
    put16(kPpqn);
    for (auto& t : tracks) {
        std::vector<std::uint8_t> tb = t.serialise();
        out.insert(out.end(), tb.begin(), tb.end());
    }
    return out;
}

// ── independent minimal MIDI re-parser (mirrors _verify_midi); returns (ok, message) ──────────────────
std::pair<bool, std::string> verifyMidi(const std::vector<std::uint8_t>& data) {
    if (data.size() < 14 || std::string(data.begin(), data.begin() + 4) != "MThd")
        return {false, "no MThd"};
    int ntrk = (data[10] << 8) | data[11];
    std::size_t off = 14;
    for (int ti = 0; ti < ntrk; ++ti) {
        if (off + 8 > data.size() || std::string(data.begin() + off, data.begin() + off + 4) != "MTrk")
            return {false, "track " + std::to_string(ti) + " no MTrk"};
        std::uint32_t tlen = (std::uint32_t(data[off + 4]) << 24) | (std::uint32_t(data[off + 5]) << 16) |
                             (std::uint32_t(data[off + 6]) << 8) | data[off + 7];
        // The declared MTrk length must fit inside the remaining buffer. A truncated file (e.g. one
        // left behind by a killed previous run and picked up on the resume path) commonly has a valid
        // header but a body shorter than tlen -- reject it here rather than walking off the end below.
        if (tlen > data.size() - (off + 8))
            return {false, "track " + std::to_string(ti) + " length exceeds buffer"};
        std::size_t p = off + 8;
        std::size_t end = p + tlen; // guaranteed <= data.size()
        int running = -1;
        bool sawEot = false;
        while (p < end) {
            // delta VLQ (each continuation byte must stay inside the track)
            while (true) {
                if (p >= end) return {false, "track " + std::to_string(ti) + " truncated delta"};
                int b = data[p++];
                if (!(b & 0x80)) break;
            }
            if (p >= end) return {false, "track " + std::to_string(ti) + " truncated status"};
            int status = data[p];
            if (status < 0x80) {
                if (running < 0) return {false, "running status with no prior status"};
                status = running;
            } else {
                ++p;
                if (status < 0xF0) running = status;
            }
            if (status == 0xFF) {
                if (p >= end) return {false, "track " + std::to_string(ti) + " truncated meta type"};
                int mtype = data[p++];
                std::size_t mlen = 0;
                while (true) {
                    if (p >= end) return {false, "track " + std::to_string(ti) + " truncated meta len"};
                    int b = data[p++];
                    mlen = (mlen << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                if (mlen > end - p) return {false, "track " + std::to_string(ti) + " meta len overruns"};
                if (mtype == 0x2F) sawEot = true;
                p += mlen;
            } else if (status == 0xF0 || status == 0xF7) {
                std::size_t mlen = 0;
                while (true) {
                    if (p >= end) return {false, "track " + std::to_string(ti) + " truncated sysex len"};
                    int b = data[p++];
                    mlen = (mlen << 7) | (b & 0x7F);
                    if (!(b & 0x80)) break;
                }
                if (mlen > end - p) return {false, "track " + std::to_string(ti) + " sysex len overruns"};
                p += mlen;
            } else {
                int hi = status & 0xF0;
                std::size_t need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
                if (need > end - p) return {false, "track " + std::to_string(ti) + " truncated event data"};
                p += need;
            }
        }
        if (!sawEot) return {false, "track " + std::to_string(ti) + " no end-of-track"};
        off = end;
    }
    return {true, "ok (" + std::to_string(ntrk) + " tracks)"};
}

// EK payload extraction (same empirical framing as the audio class).
std::vector<std::uint8_t> ekPayload(const Archive* disk, const std::string& symbol, long long expect) {
    std::string key = "ek/" + symbol;
    auto raw = disk->readRaw(key);
    if (!raw) return {};
    std::vector<std::uint8_t>& d = *raw;
    for (std::size_t start : {std::size_t(0), kOtrHeaderSize, kOtrHeaderSize + 4}) {
        if (d.size() >= start && static_cast<long long>(d.size() - start) == expect)
            return std::vector<std::uint8_t>(d.begin() + static_cast<std::ptrdiff_t>(start), d.end());
    }
    return d;
}

struct SeqDesc {
    std::string label, origin;
    std::vector<std::uint8_t> data;
    int fontIndex = 0;
    long long declared = 0;
};

std::vector<SeqDesc> collectSequences(Context& ctx, const AudioTables& t, const Archive* disk,
                                      const std::map<std::string, long long>& ekLens) {
    std::vector<SeqDesc> seqs;
    auto seqBlob = ctx.cart->readStripped("audio_blob/audio_seq", kBlobPayloadOffset);
    if (seqBlob) {
        for (std::size_t idx = 0; idx < t.romSeq.size(); ++idx) {
            const AudioSeqEntry& e = t.romSeq[idx];
            std::size_t start = static_cast<std::size_t>(e.offset);
            std::size_t stop = static_cast<std::size_t>(e.offset + e.size);
            if (start > seqBlob->size()) start = seqBlob->size();
            if (stop > seqBlob->size()) stop = seqBlob->size();
            SeqDesc s;
            s.label = "cart_" + e.name;
            s.origin = "cart";
            s.fontIndex = static_cast<int>(idx);
            s.declared = e.size;
            if (start < stop) s.data.assign(seqBlob->begin() + start, seqBlob->begin() + stop);
            seqs.push_back(std::move(s));
        }
    }
    if (disk != nullptr) {
        int n = static_cast<int>(sizeof(kEkSeqSymbols) / sizeof(kEkSeqSymbols[0]));
        for (int idx = 0; idx < n; ++idx) {
            if (idx >= static_cast<int>(t.diskSeq.size())) break;
            const AudioSeqEntry& e = t.diskSeq[idx];
            std::string symbol = kEkSeqSymbols[idx];
            if (!disk->has("ek/" + symbol)) continue;
            long long expect = e.size;
            auto it = ekLens.find(symbol);
            if (it != ekLens.end()) expect = it->second;
            SeqDesc s;
            s.label = "ek_" + e.name;
            s.origin = "ek";
            s.fontIndex = idx;
            s.declared = e.size;
            s.data = ekPayload(disk, symbol, expect);
            seqs.push_back(std::move(s));
        }
    }
    return seqs;
}

} // namespace

ClassResult runMidi(Context& ctx) {
    ClassResult res;
    res.name = "midi";

    AudioTables tables;
    if (!loadAudioTables(ctx.tablesDataPath, tables)) {
        res.hardError = true;
        res.errorMsg = "audio tables data (audio_tables_data.json 'audio' section) not found -- the "
                       "build step (tools/gen_dump_tables_data.py) must emit it into decomp-recipes";
        return res;
    }
    const Archive* disk = ctx.disk;
    std::map<std::string, long long> ekLens = loadEkManifestLens(ctx.manifestPath);
    std::vector<SeqDesc> seqs = collectSequences(ctx, tables, disk, ekLens);
    if (seqs.empty()) {
        std::printf("  midi: no sequences found (audio_seq blob + EK archive both absent) -- skipping\n");
        res.hardError = true;
        res.errorMsg = "no sequences (audio_seq blob + EK archive both absent)";
        return res;
    }

    std::string outDir = ctx.dumpDir.empty() ? std::string() : joinPath(ctx.dumpDir, "music");
    makeDirs(outDir);
    std::vector<std::vector<std::string>> manifest;
    int dumped = 0, skipped = 0, failed = 0;

    for (const SeqDesc& sd : seqs) {
        if (!safeOutputComponent(sd.label)) {
            std::fprintf(stderr, "  warn: midi: unsafe output name '%s'; skipping\n", sd.label.c_str());
            ++failed;
            continue;
        }
        std::string midPath = joinPath(outDir, sd.label + ".mid");
        std::string mapPath = joinPath(outDir, sd.label + ".instrmap.json");
        std::string disPath = joinPath(outDir, sd.label + ".disasm.txt");
        bool sizeOk = (static_cast<long long>(sd.data.size()) == sd.declared);

        if (fileExists(midPath) && fileExists(mapPath)) {
            std::vector<std::uint8_t> existing;
            {
                std::string s = readFile(midPath);
                existing.assign(s.begin(), s.end());
            }
            auto vr = verifyMidi(existing);
            if (vr.first) {
                // The existing .mid re-parses cleanly -- keep it (resume/skip).
                std::string notes;
                try {
                    ConvertResult cr = convertSequence(sd.label, sd.data, sd.fontIndex);
                    notes = std::to_string(cr.totalNotes);
                } catch (...) {
                    notes = "?";
                }
                manifest.push_back({sd.label, sd.origin, std::to_string(sd.data.size()),
                                    std::to_string(sd.fontIndex), notes, "existing", "verify:PASS"});
                ++skipped;
                continue;
            }
            // A present-but-INVALID .mid (e.g. a truncated file from a killed prior run) must be
            // regenerated, not accepted. Fall through to the (re)dump path below, which overwrites
            // midPath/mapPath with a freshly converted, valid sequence.
            std::fprintf(stderr, "  warn: midi %s: existing file failed verify (%s); regenerating\n",
                         sd.label.c_str(), vr.second.c_str());
        }

        ConvertResult cr;
        std::vector<std::uint8_t> midBytes;
        try {
            cr = convertSequence(sd.label, sd.data, sd.fontIndex);
            midBytes = writeMidiBytes(cr.tracks);
        } catch (const std::exception& exc) {
            std::fprintf(stderr, "  warn: midi %s: %s\n", sd.label.c_str(), exc.what());
            std::string msg = exc.what();
            manifest.push_back({sd.label, sd.origin, std::to_string(sd.data.size()),
                                std::to_string(sd.fontIndex), "0", "ERROR", msg.substr(0, 60)});
            ++failed;
            continue;
        }
        writeFileBytes(midPath, midBytes.data(), midBytes.size());

        // instrmap.json
        std::string mapJson = "{";
        mapJson += "\"sequence\":" + jStr(sd.label);
        mapJson += ",\"origin\":" + jStr(sd.origin);
        mapJson += ",\"soundfontIndex\":" + std::to_string(sd.fontIndex);
        mapJson += ",\"tempoSeqTicksPerMinute\":" + std::to_string(cr.tempo);
        mapJson += ",\"ppqn\":" + std::to_string(kPpqn);
        mapJson += ",\"sizeMatchesTable\":" + std::string(sizeOk ? "true" : "false");
        mapJson += ",\"channels\":{" + cr.instrMapJson + "}";
        mapJson += ",\"note\":" +
                   jStr("channel program = soundfont instrument index (font bound via "
                        "gSequenceFontTable, 1:1 seq->font by enum name); loops/jumps are "
                        "CC111/text markers (lossy linear transcription).");
        mapJson += "}";
        writeFileText(mapPath, mapJson);

        // disasm.txt
        std::string dis = "# linear aseq disassembly of " + sd.label + " (" +
                          std::to_string(sd.data.size()) + " bytes)\n";
        for (std::size_t i = 0; i < cr.disasm.size(); ++i) {
            dis += cr.disasm[i];
            dis += "\n";
        }
        writeFileText(disPath, dis);

        auto vr = verifyMidi(midBytes);
        if (!vr.first)
            ++failed;
        else
            ++dumped;
        manifest.push_back({sd.label, sd.origin, std::to_string(sd.data.size()),
                            std::to_string(sd.fontIndex), std::to_string(cr.totalNotes),
                            sizeOk ? "ok" : "ok-SIZE-MISMATCH",
                            "verify:" + std::string(vr.first ? "PASS" : "FAIL:" + vr.second)});
    }

    writeManifest(joinPath(outDir, "manifest.tsv"),
                  "sequence\torigin\tbytes\tsoundfontIndex\tnotes\tstatus\tselfVerify   "
                  "(aseq -> MIDI, linear transcription; loops/effects = markers, lossy)",
                  manifest);
    std::printf("  midi: %d MIDI written, %d skipped, %d failed (of %zu sequences; all self-verified)\n",
                dumped, skipped, failed, seqs.size());

    res.dumped = dumped;
    res.skipped = skipped;
    res.failed = failed;
    res.total = static_cast<int>(seqs.size());
    return res;
}

} // namespace gdxdump
