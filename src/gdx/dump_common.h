// Shared internals for the gdx-extract `dump` subcommand (Wave 1). Private to the dump_*.cpp TUs.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace miniz_cpp {
class zip_file;
}

namespace gdxdump {

// ── archive framing constants (verified against generic.o2r / fzerox-disk.o2r) ──────────────────────
constexpr std::size_t kOtrHeaderSize = 64;                       // libultraship ResourceInitData header
constexpr std::size_t kTexSubHeaderSize = 16;                    // [texType,width,height,dataSize]
constexpr std::size_t kTexPayloadOffset = kOtrHeaderSize + kTexSubHeaderSize; // 0x50
constexpr std::size_t kBlobPayloadOffset = kOtrHeaderSize + 4;   // 0x44 (u32 size subheader)
constexpr std::size_t kArraySubHeaderSize = 8;                   // [arrayType u32, count u32]
constexpr std::size_t kArrayPayloadOffset = kOtrHeaderSize + kArraySubHeaderSize; // 0x48
constexpr std::uint32_t kResourceTypeGenericArray = 0x47415252;  // 'GARR', Torch::ResourceType::GenericArray

// ── an opened .o2r archive (read side) ───────────────────────────────────────────────────────────
class Archive {
  public:
    explicit Archive(const std::string& path);
    ~Archive();
    // Owns a raw miniz_cpp::zip_file* freed in the destructor -- copying would double-free. Non-copyable.
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    bool ok() const { return mZip != nullptr; }
    bool has(const std::string& key) const;
    // Full raw entry bytes, or nullopt if absent.
    std::optional<std::vector<std::uint8_t>> readRaw(const std::string& key) const;
    // Entry bytes with the leading `strip` header bytes removed, or nullopt if absent/too short.
    std::optional<std::vector<std::uint8_t>> readStripped(const std::string& key,
                                                          std::size_t strip) const;
    std::vector<std::string> names() const;

  private:
    miniz_cpp::zip_file* mZip = nullptr;
    std::vector<std::string> mNames;
};

// ── MIO0 (uses torch lib/libmio0) ────────────────────────────────────────────────────────────────
// Decodes a standard MIO0 blob (magic "MIO0"). Returns empty vector on failure.
std::vector<std::uint8_t> mio0Decompress(const std::uint8_t* data, std::size_t len);

// ── N64 texel decoders (each mirrors the matching Interpreter::ImportTexture* / Python decoder) ─────
// Every decoder returns width*height*4 RGBA bytes. `pal` is the CI palette payload (big-endian
// RGBA5551 entries) or nullptr. Returns empty on a size problem.
std::vector<std::uint8_t> decodeTexel(const std::string& fmt, const std::uint8_t* payload,
                                      std::size_t payloadLen, int w, int h,
                                      const std::uint8_t* pal, std::size_t palLen);
// Byte count for `count` texels of fmt (rounds 4bpp up). Returns 0 for unknown fmt.
std::size_t texelBytes(const std::string& fmt, std::size_t count);
bool isDecodableFormat(const std::string& fmt);

// ── PNG (uses n64graphics stb_image_write). Writes RGBA. Returns true on success. ───────────────────
bool writePng(const std::string& path, int w, int h, const std::uint8_t* rgba);

// ── filesystem helpers ─────────────────────────────────────────────────────────────────────────────
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
void makeDirs(const std::string& path);          // recursive mkdir -p
std::string joinPath(const std::string& a, const std::string& b);
// Output-path defense-in-depth: returns true only when `component` is safe to use as a single output
// filename/leaf built from an untrusted archive/manifest/yaml-sourced key or symbol. Rejects any
// component containing ".." (parent escape), a leading '/' or '\\' (absolute), or a drive-colon
// (Windows drive/ADS). Callers treat a false result as a per-item failure.
bool safeOutputComponent(const std::string& component);
std::vector<std::string> listYaml(const std::string& dir, bool recursive);
std::string readFile(const std::string& path);   // "" if missing
bool writeFileBytes(const std::string& path, const std::uint8_t* data, std::size_t len);
bool writeFileText(const std::string& path, const std::string& text);

// Writes a manifest.tsv: "# <header>\n" then tab-joined rows. LF newlines.
void writeManifest(const std::string& path, const std::string& header,
                   const std::vector<std::vector<std::string>>& rows);

// ── segment_blob containment map: archive-first raw-ROM byte provider ────────────────────────────────
// Built from segment_blob.yaml (rom_base+size per BLOB) + the cart archive's segment_blob/<name>
// payloads. Serves any absolute ROM byte range [addr,addr+n) that is fully covered by one or more
// adjacent blob slices (blobs are verbatim ROM slices and tile contiguously). Falls back to the raw
// ROM image for ranges no blob covers. This lets dlists/vertexdata (and wave-3 models) read raw
// F3DEX2/VTX bytes with no baserom present — the game's own gdx_lookup_segment_blob does the same
// lookup over the generated sSegmentBlobMap.
class RomByteProvider {
  public:
    // `cartYamlDir` supplies segment_blob.yaml; `archive` supplies the blob payloads; `rom` is the
    // optional raw ROM image (may be empty for archive-only).
    RomByteProvider(const std::string& cartYamlDir, const Archive& archive,
                    std::vector<std::uint8_t> rom);
    // Returns [addr,addr+n) if fully resolvable (blob-first, ROM fallback), else nullopt. `viaRom`
    // (out) is set true when the ROM fallback served the range (census/telemetry).
    std::optional<std::vector<std::uint8_t>> read(std::uint64_t addr, std::size_t n,
                                                  bool* viaRom = nullptr) const;
    // Read up to `n` bytes starting at addr, stopping at the first gap; used by the DL scanner which
    // does not know a length up front. Returns however many contiguous bytes are available.
    std::vector<std::uint8_t> readUpTo(std::uint64_t addr, std::size_t n, bool* viaRom = nullptr) const;
    bool hasRom() const { return !mRom.empty(); }

  private:
    struct Blob { std::uint64_t start; std::uint64_t end; std::vector<std::uint8_t> data; };
    std::vector<Blob> mBlobs;             // sorted by start, non-overlapping
    std::vector<std::uint8_t> mRom;
    // Returns the byte at absolute addr from blobs (preferred) or ROM, or nullopt.
    std::optional<std::uint8_t> byteAt(std::uint64_t addr, bool* viaRom) const;
};

// ── yaml walking (shared by textures + dlists + vertexdata) ─────────────────────────────────────────
// Parse an int scalar node string ("0x8", "8", "-3") -> value. base 0.
long long parseIntStr(const std::string& s);

// A GFX or VTX entry from a segment-configured yaml.
struct SegmentItem {
    std::string yamlStem;
    std::string sym;
    std::uint64_t offset;    // segment-relative
    long long count;         // -1 if absent
    int segmentId;
    std::uint64_t romBase;
};
// Yields every entry of `wantedType` ("GFX"/"VTX") in a yaml carrying a :config: segments: block.
std::vector<SegmentItem> walkSegmentItems(const std::string& cartYamlDir,
                                          const std::string& wantedType);

// Recipe filenames using the flat BLOB-only mechanism (skipped by texture/gfx/vtx walks).
bool isBlobRecipeFilename(const std::string& fname);

// ── context passed to every class ───────────────────────────────────────────────────────────────────
struct Context {
    const DumpOptions* opts = nullptr;
    std::string cartYamlDir;   // <recipes>/assets/yaml/us/rev0
    std::string ekYamlDir;     // fzerox-expansion-kit/assets/yaml/jp (may be "")
    std::string manifestPath;  // ek_slice_manifest.txt (may be "")
    std::string tablesDataPath;// generated soundfont/sequence table data (may be "")
    std::string ekTlutMapPath; // generated EK CI4/CI8 TLUT-resolution map (may be ""); Task C
    Archive* cart = nullptr;   // cart archive (generic.o2r), required for most classes
    Archive* disk = nullptr;   // EK disk archive (fzerox-disk.o2r), optional
    Archive* ipl = nullptr;    // n64ddipl.o2r, optional
    RomByteProvider* rom = nullptr; // archive-first ROM byte provider (never null; may be blob-only)
    std::string dumpDir;
};

// Each class returns a small result. `failed`>0 or a hard error => the class is a failure.
struct ClassResult {
    std::string name;
    bool hardError = false;   // could not run at all (missing required source)
    std::string errorMsg;     // printed when hardError
    int dumped = 0, skipped = 0, failed = 0, total = 0;
};

// ── shared audio tables (loaded from the build-generated audio_tables_data.json "audio" section) ─────
// Every field is pre-evaluated to an integer by tools/gen_dump_tables_data.py using the oracle's own
// _SafeEval, so the native audio/midi classes need no C expression evaluator and cannot diverge.
struct AudioFontEntry {
    std::string name;
    long long size = 0;
    long long sd1 = 0;   // (bankId1 << 8) | bankId2
    long long sd2 = 0;   // (numInstruments << 8) | numDrums
    long long sfx = 0;   // numSoundEffects
    long long offset = 0; // blob offset (cart fonts only; 0 for EK)
};
struct AudioSeqEntry {
    std::string name;
    long long size = 0;
    long long offset = 0; // blob offset (cart seqs only; 0 for EK)
};
struct AudioBankEntry {
    std::string name;
    long long offset = 0;
    long long size = 0;
    long long medium = 0; // SampleMedium: 0=RAM 1=LBA 2=CART 3=DISK_DRIVE
};
struct AudioTables {
    std::vector<AudioFontEntry> romFont, diskFont;
    std::vector<AudioSeqEntry> romSeq, diskSeq;
    std::vector<AudioBankEntry> diskBank;
    bool ok = false;
};
// Loads the "audio" section of audio_tables_data.json. Returns false (out.ok stays false) if the file
// is missing or malformed.
bool loadAudioTables(const std::string& path, AudioTables& out);

// EK slice manifest length map (symbol -> len) for FZX:SOUNDFONT / FZX:SEQUENCE rows.
std::map<std::string, long long> loadEkManifestLens(const std::string& manifestPath);

// Class entry points (implemented in dump_textures.cpp / dump_extra.cpp / dump_arrays.cpp /
// dump_audio.cpp / dump_midi.cpp).
ClassResult runArrays(Context& ctx);
ClassResult runTextures(Context& ctx);
ClassResult runCourseData(Context& ctx);
ClassResult runDlists(Context& ctx);
ClassResult runVertexData(Context& ctx);
ClassResult runTables(Context& ctx);
ClassResult runGhosts(Context& ctx);
ClassResult runFonts(Context& ctx);
ClassResult runAudio(Context& ctx);
ClassResult runMidi(Context& ctx);
ClassResult runModels(Context& ctx);

} // namespace gdxdump
