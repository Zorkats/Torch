#include <iostream>
#include <sstream>
#include "CLI11.hpp"
#include "Companion.h"
#include "gdx/ipl_extract.h" // G-Diffuser R3: `ipl` subcommand (64DD IPL font-block archive)
#include "gdx/disk_extract.h" // G-Diffuser R8: `disk` subcommand (64DD EK disk-image archive)
#include "gdx/dump_all.h" // G-Diffuser Native Dump All: `dump` subcommand

#if defined(STANDALONE) && !defined(__EMSCRIPTEN__)

int main(int argc, char* argv[]) {
    CLI::App app{ "Torch - [T]orch is [O]ur [R]esource [C]onversion [H]elper\n\
        * It extracts from a baserom and generates code or an otr.\n\
        * It can also generate an otr from a folder of assets.\n" };
    std::string mode;
    std::string filename;
    std::string target;
    std::string folder;
    std::string archive;
    std::string version;
    std::string configKeyOverride; // G-Diffuser: extract a modified ROM with a known dump's recipes
    std::uint32_t versionCrcOverride = 0; // G-Diffuser: stamp the recipe tree's CRC, not the ROM's
    ArchiveType otrMode = ArchiveType::None;
    bool otrModeSelected = false;
    bool xmlMode = false;
    bool debug = false;
    std::string srcdir;
    std::string destdir;
    std::string diskManifest; // R8 Step 2: optional EK slice manifest for the `disk` subcommand
    std::vector<std::string> additionalFiles;
    int gExitCode = 0; // process exit code driven by the `dump` subcommand

    app.require_subcommand();

    /* Generate an OTR */
    const auto otr = app.add_subcommand("otr", "OTR - Generates an otr\n");

    otr->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    otr->add_flag("-v,--verbose", debug, "Verbose Debug Mode");
    otr->add_option("-s,--srcdir", srcdir,
                    "Set source directory to locate config.yml and asset metadata for processing")
        ->check(CLI::ExistingDirectory);
    otr->add_option("-d,--destdir", destdir, "Set destination directory for export");

    otr->parse_complete_callback([&] {
        const auto instance = Companion::Instance = new Companion(filename, ArchiveType::OTR, debug, srcdir, destdir);
        instance->Init(ExportType::Binary);
    });

    /* Generate an O2R */
    const auto o2r = app.add_subcommand("o2r", "O2R - Generates an o2r\n");

    o2r->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    o2r->add_flag("-v,--verbose", debug, "Verbose Debug Mode");
    o2r->add_option("-s,--srcdir", srcdir,
                    "Set source directory to locate config.yml and asset metadata for processing")
        ->check(CLI::ExistingDirectory);
    o2r->add_option("-d,--destdir", destdir, "Set destination directory for export");
    o2r->add_option("-a,--additional-files", additionalFiles,
                    "Additional files to include in the o2r archive (e.g., mods.toml)")
        ->check(CLI::ExistingFile);
    o2r->add_option("-u,--version", version, "Version to set in the o2r archive");
    o2r->add_option("--version-crc", versionCrcOverride,
                    "Stamp the archive version entry with this ROM CRC instead of the cartridge's own. "
                    "Pair it with --config-key: an archive built from a hack using stock recipes is "
                    "structurally a stock archive, and the version entry describes layout, not origin.");
    o2r->add_option("--config-key", configKeyOverride,
                    "Select the config.yml recipe tree by this key instead of the ROM's own hash. "
                    "For ROM hacks, whose hash no config.yml describes. Assets the hack relocated "
                    "will be read from the original ROM's offsets.");

    o2r->parse_complete_callback([&] {
        const auto instance = Companion::Instance = new Companion(filename, ArchiveType::O2R, debug, srcdir, destdir);
        instance->SetAdditionalFiles(additionalFiles);
        instance->SetVersion(version);
        instance->SetConfigKeyOverride(configKeyOverride);
        instance->SetVersionCrcOverride(versionCrcOverride);
        instance->Init(ExportType::Binary);
    });

    /* Extract the 64DD IPL font block into a dedicated O2R archive (G-Diffuser R3) */
    const auto ipl = app.add_subcommand(
        "ipl", "IPL - Extracts the 64DD IPL font block into n64ddipl.o2r\n");

    ipl->add_option("<N64DDIPLROM.n64>", filename, "")->required()->check(CLI::ExistingFile);
    ipl->add_option("-d,--destdir", destdir, "Destination directory for n64ddipl.o2r")->required();

    ipl->parse_complete_callback([&] {
        // Throw on failure so the shared handler in app.parse() reports it and returns non-zero — the
        // runtime launcher (gdx_extract_launch) treats any non-zero exit as "no archive, raw fallback".
        if (GdxRunIplExtract(filename, destdir) != 0) {
            throw std::runtime_error("IPL extraction failed");
        }
    });

    /* Pack the 64DD Expansion Kit disk image into a dedicated O2R archive (G-Diffuser R8 Step 1) */
    const auto disk = app.add_subcommand(
        "disk", "DISK - Packs the 64DD EK disk image into fzerox-disk.o2r\n");

    disk->add_option("<disk.ndd>", filename, "")->required()->check(CLI::ExistingFile);
    disk->add_option("-d,--destdir", destdir, "Destination directory for fzerox-disk.o2r")->required();
    // R8 Step 2: optional EK slice manifest (port/gen/ek_slice_manifest.txt). When supplied, one
    // verbatim ek/<symbol> entry is appended per named EK disk asset; omitted -> two disk/* entries.
    disk->add_option("-m,--manifest", diskManifest,
                     "Optional EK slice manifest (adds ek/<symbol> per-asset entries)")
        ->check(CLI::ExistingFile);

    disk->parse_complete_callback([&] {
        // Same failure discipline as the ipl step: a non-zero exit tells the runtime launcher to keep
        // the raw-disk/managed-copy fallback rather than mount a bad archive.
        if (GdxRunDiskExtract(filename, destdir, diskManifest) != 0) {
            throw std::runtime_error("Disk extraction failed");
        }
    });

    /* Native Dump All: decode named game assets from the extracted archives (no game, no Python) */
    const auto dump = app.add_subcommand(
        "dump", "DUMP - Decodes named game assets from the extracted archives\n");
    std::string dumpClasses;
    std::string dumpDir;
    std::string dumpRom, dumpArchive, dumpDiskArchive, dumpIplArchive, dumpManifest, dumpRecipes,
        dumpEkYamlDir;
    bool dumpListClasses = false;
    dump->add_option("-c,--classes", dumpClasses,
                     "Comma-separated dump classes (textures,coursedata,dlists,vertexdata,tables,"
                     "ghosts,fonts)");
    dump->add_option("-d,--dump-dir", dumpDir, "Output dump directory");
    dump->add_option("--rom", dumpRom, "Optional raw baserom.z64 (archive-first when omitted)");
    dump->add_option("-a,--archive", dumpArchive, "Cart asset archive (generic.o2r)");
    dump->add_option("--disk-archive", dumpDiskArchive, "64DD EK disk archive (fzerox-disk.o2r)");
    dump->add_option("--ipl-archive", dumpIplArchive, "64DD IPL archive (n64ddipl.o2r)");
    dump->add_option("-m,--manifest", dumpManifest, "EK slice manifest (ek_slice_manifest.txt)");
    dump->add_option("--recipes", dumpRecipes, "Recipe tree dir (decomp-recipes)");
    dump->add_option("--ek-yaml-dir", dumpEkYamlDir,
                     "EK asset recipe tree (fzerox-expansion-kit/assets/yaml/jp)");
    dump->add_flag("--list-classes", dumpListClasses, "Print one class name per line and exit");

    dump->parse_complete_callback([&] {
        DumpOptions opts;
        opts.listClasses = dumpListClasses;
        if (!dumpClasses.empty()) {
            std::stringstream ss(dumpClasses);
            std::string item;
            while (std::getline(ss, item, ',')) {
                // trim
                std::size_t a = item.find_first_not_of(" \t");
                std::size_t b = item.find_last_not_of(" \t");
                if (a != std::string::npos) opts.classes.push_back(item.substr(a, b - a + 1));
            }
        }
        opts.dumpDir = dumpDir;
        opts.romPath = dumpRom;
        opts.archivePath = dumpArchive;
        opts.diskArchivePath = dumpDiskArchive;
        opts.iplArchivePath = dumpIplArchive;
        opts.manifestPath = dumpManifest;
        opts.recipesDir = dumpRecipes;
        opts.ekYamlDir = dumpEkYamlDir;
        gExitCode = GdxRunDumpAll(opts);
    });

    /* Generate C code */
    const auto code = app.add_subcommand("code", "Code - Generates C code\n");

    code->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    code->add_flag("-v,--verbose", debug, "Verbose Debug Mode; adds offsets to C code");
    code->add_option("-s,--srcdir", srcdir,
                     "Set source directory to locate config.yml and asset metadata for processing")
        ->check(CLI::ExistingDirectory);
    code->add_option("-d,--destdir", destdir, "Set destination directory to place C code to");

    code->parse_complete_callback([&]() {
        const auto instance = Companion::Instance = new Companion(filename, ArchiveType::None, debug, srcdir, destdir);
        instance->Init(ExportType::Code);
    });

    /* Generate a binary */
    const auto binary = app.add_subcommand("binary", "Binary - Generates a binary\n");

    binary->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    binary
        ->add_option("-s,--srcdir", srcdir,
                     "Set source directory to locate config.yml and asset metadata for processing")
        ->check(CLI::ExistingDirectory);
    binary->add_option("-d,--destdir", destdir, "Set destination directory to place binary to");

    binary->parse_complete_callback([&] {
        const auto instance = Companion::Instance = new Companion(filename, ArchiveType::None, debug, srcdir, destdir);
        instance->Init(ExportType::Binary);
    });

    /* Generate headers */
    const auto header = app.add_subcommand("header", "Header - Generates headers only\n");

    header->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    header->add_flag("-o,--otr", otrModeSelected, "OTR/O2R Mode");
    header
        ->add_option("-s,--srcdir", srcdir,
                     "Set source directory to locate config.yml and asset metadata for processing")
        ->check(CLI::ExistingDirectory);
    header->add_option("-d,--destdir", destdir, "Set destination directory to place headers to");

    header->parse_complete_callback([&] {
        if (otrModeSelected) {
            otrMode = ArchiveType::OTR;
        } else {
            otrMode = ArchiveType::None;
        }

        const auto instance = Companion::Instance = new Companion(filename, otrMode, debug, srcdir, destdir);
        instance->Init(ExportType::Header);
    });

    /* Pack an archive from a folder */
    const auto pack = app.add_subcommand("pack", "Pack - Packs an archive from a folder\n");

    pack->add_option("<folder>", folder, "Generate OTR from a directory of assets")
        ->required()
        ->check(CLI::ExistingDirectory);
    pack->add_option("<target>", target, "Archive output destination")->required();
    pack->add_option("<archive-type>", archive, "Archive type: otr or o2r")->required();
    pack->add_option("-u,--version", version, "Version to set in the o2r archive");

    pack->parse_complete_callback([&] {
        if (archive == "otr") {
            otrMode = ArchiveType::OTR;
        } else if (archive == "o2r") {
            otrMode = ArchiveType::O2R;
        } else {
            std::cout << "Invalid archive type" << std::endl;
        }

        if (!folder.empty()) {
            Companion::Pack(folder, target, otrMode, version);
        } else {
            std::cout << "The folder is empty" << std::endl;
        }
    });

    /* Generate modding files */
    const auto modding_root = app.add_subcommand("modding", "Modding - Generates modding files like png\n");
    const auto modding_import =
        modding_root->add_subcommand("import", "Import - Import modified files to generate C code\n");
    const auto modding_export = modding_root->add_subcommand("export", "Export - Export modified files to a folder\n");

    modding_import->add_option("mode", mode, "code, otr, o2r or header")->required();
    modding_import->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    modding_import->add_flag("-v,--verbose", debug, "Verbose Debug Mode");
    modding_import
        ->add_option(
            "-s,--srcdir", srcdir,
            "Set source directory to locate config.yml and asset metadata for processing, including modified files")
        ->check(CLI::ExistingDirectory);
    modding_import->add_option("-d,--destdir", destdir, "Set destination directory to place for generating C code");

    modding_import->parse_complete_callback([&] {
        ArchiveType otrMode;

        if (mode == "otr") {
            otrMode = ArchiveType::OTR;
        } else if (mode == "o2r") {
            otrMode = ArchiveType::O2R;
        } else {
            otrMode = ArchiveType::None;
        }

        const auto instance = Companion::Instance = new Companion(filename, otrMode, debug, true, srcdir, destdir);
        if (mode == "code") {
            instance->Init(ExportType::Code);
        } else if (mode == "otr" || mode == "o2r") {
            instance->Init(ExportType::Binary);
        } else if (mode == "header") {
            instance->Init(ExportType::Header);
        } else {
            std::cout << "Invalid mode" << std::endl;
        }
    });

    modding_export->add_flag("-x,--xml", xmlMode, "XML Mode");
    modding_export->add_option("<baserom.z64>", filename, "")->required()->check(CLI::ExistingFile);
    modding_export
        ->add_option(
            "-s,--srcdir", srcdir,
            "Set source directory to locate config.yml and asset metadata for processing, including modified files")
        ->check(CLI::ExistingDirectory);
    modding_export->add_option("-d,--destdir", destdir,
                               "Set destination directory to place for generating modified files");

    modding_export->parse_complete_callback([&] {
        const auto instance = Companion::Instance = new Companion(filename, ArchiveType::None, debug, srcdir, destdir);
        if (xmlMode) {
            instance->Init(ExportType::XML);
        } else {
            instance->Init(ExportType::Modding);
        }
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        std::cout << app.help() << std::endl;
        return app.exit(e);
    } catch (const std::exception& e) {
        // Asset-processing exceptions escape the parse_complete_callbacks through app.parse().
        // Without this handler they escalate to an opaque fail-fast (0xC0000409 on MSVC) with no
        // message — report the reason and exit nonzero so callers (gdx_extract_launch) can log it.
        std::cerr << "FATAL: " << e.what() << std::endl;
        return 3;
    } catch (...) {
        std::cerr << "FATAL: unknown exception during extraction" << std::endl;
        return 3;
    }

    // No arguments --> display help.
    if (argc == 1) {
        std::cout << app.help() << std::endl;
    }

    return gExitCode;
}
#endif
