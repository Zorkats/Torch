// gdx-extract `dump` subcommand (G-Diffuser Native Dump All, Wave 1).
//
// Native, Python-free port of tools/gen_dump_all.py (+ gen_dump_all_extra.py). Decodes the game's
// named assets straight from the extracted archives with no running game and no display, producing
// output SEMANTICALLY IDENTICAL to the proven Python oracle (verified by tools/verify_native_dump.py):
//
//   textures    -> <dumpDir>/<key>.png              (+ manifest.tsv)   cart + 64DD EK (TLUT swatches,
//                                                                      MIO0-compressed EK textures)
//   coursedata  -> <dumpDir>/coursedata/<sym>.bin+.json                26 CourseData records
//   dlists      -> <dumpDir>/dlists/<key>.bin+.txt                     202 raw F3DEX2 display lists
//   vertexdata  -> <dumpDir>/vertexdata/<key>.bin+.json                1563 raw N64Vtx_t arrays
//   tables      -> <dumpDir>/tables/*.bin + *.json                     soundfont/sequence tables
//   ghosts      -> <dumpDir>/ghosts/<sym>.bin+.json+.gdg               24 staff ghost records
//   fonts       -> <dumpDir>/fonts/{fault,kanji}/*.png + sheets        64DD IPL fault/kanji font
//
// The class registry (see dump_all.cpp) is shaped so wave-2/3 kinds (audio/midi/models) slot in as
// one more DumpClass each with no other wiring, mirroring the Python CLASS_REGISTRY.
//
// Reuse-vs-self-contained: archive reads use miniz_cpp (torch's bundled zip), PNG writes use torch's
// n64graphics stb_image_write, MIO0 uses torch's lib/libmio0, yaml walks use yaml-cpp. The N64 texel
// decoders and the F3DEX2/course/ghost/font byte parsers are self-contained (they mirror the Python
// oracle line-for-line; those Python bodies are themselves the inverse of the libultraship Fast3D
// interpreter and torch's own factories), because the Companion factory pipeline is too entangled
// with a running extraction to call from a standalone subcommand.
#pragma once

#include <string>
#include <vector>

// Options resolved from the CLI (see main.cpp registration). Empty path fields trigger auto-discovery
// (next to the executable, then the CWD) inside GdxRunDumpAll.
struct DumpOptions {
    std::vector<std::string> classes; // requested class names (order preserved)
    std::string dumpDir;              // -d / --dump-dir (required unless listClasses)
    std::string romPath;              // --rom            (optional raw baserom.z64; archive-first w/o it)
    std::string archivePath;          // --archive        (cart generic.o2r / fzerox.o2r)
    std::string diskArchivePath;      // --disk-archive   (fzerox-disk.o2r, EK ek/<symbol> entries)
    std::string iplArchivePath;       // --ipl-archive    (n64ddipl.o2r, ipl/font_block)
    std::string manifestPath;         // --manifest       (ek_slice_manifest.txt)
    std::string recipesDir;           // --recipes        (decomp-recipes dir; cart yaml + tables data)
    std::string ekYamlDir;            // --ek-yaml-dir    (fzerox-expansion-kit/assets/yaml/jp)
    bool listClasses = false;         // --list-classes
};

// Prints one registered class name per line (sorted) and returns 0. For tooling/UI.
int GdxDumpListClasses();

// Runs the requested dump classes. Per-class failure isolation: one class failing (missing source,
// bad data) prints a clear error and continues. Returns 0 if all requested classes succeeded, 1 if
// any failed, 2 on a hard setup error (no dump dir, unknown class).
int GdxRunDumpAll(const DumpOptions& opts);
