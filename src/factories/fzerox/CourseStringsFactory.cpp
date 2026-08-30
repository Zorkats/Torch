#include "CourseStringsFactory.h"

#include "Companion.h"
#include "spdlog/spdlog.h"

#include <sstream>
#include <stdexcept>

namespace {

/* Highest BgmId the game defines, plus the sentinel. decomp/include/sfx.h runs BGM_MUTE_CITY (0)
 * to BGM_DEATHRACE2 (29) with BGM_NONE = 99; ids 21-24 are unused by the retail game but are real
 * slots a hack can and does repurpose (F-Zero X Climax assigns 22 and 24). */
constexpr uint8_t kMaxBgmId = 29;
constexpr uint8_t kNoneBgmId = 99;

std::string Where(const std::string& what, size_t index) {
    std::ostringstream out;
    out << what << "[" << index << "]";
    return out.str();
}

uint32_t ReadBigEndianU32(const std::vector<uint8_t>& rom, size_t offset, const std::string& where) {
    if ((offset + 4) > rom.size()) {
        throw std::runtime_error(where + ": pointer table runs past the end of the ROM");
    }
    return (static_cast<uint32_t>(rom[offset + 0]) << 24) | (static_cast<uint32_t>(rom[offset + 1]) << 16) |
           (static_cast<uint32_t>(rom[offset + 2]) << 8) | static_cast<uint32_t>(rom[offset + 3]);
}

/* A byte belongs in a course name if the font can draw it. Control codes cannot appear inside one
 * of these strings, and their presence is the cheapest signal that the pointer did not land on
 * text at all. Bytes above 0x7E stay legal: the retail name "sector \243\301" encodes its alpha and
 * beta glyphs that way. */
bool IsRenderableByte(uint8_t b) {
    return (b >= 0x20) && (b != 0x7F);
}

std::string ReadRomString(const std::vector<uint8_t>& rom, uint32_t ramAddress, uint32_t bias,
                          const std::string& where) {
    if (ramAddress < bias) {
        throw std::runtime_error(where + ": pointer 0x" + std::to_string(ramAddress) + " is below the RAM bias");
    }

    const size_t offset = static_cast<size_t>(ramAddress - bias);
    if (offset >= rom.size()) {
        throw std::runtime_error(where + ": pointer resolves past the end of the ROM");
    }

    std::string value;
    for (size_t i = offset; i < rom.size(); i++) {
        const uint8_t b = rom[i];
        if (b == 0) {
            return value;
        }
        if (!IsRenderableByte(b)) {
            throw std::runtime_error(where + ": string contains a control byte, so the table is not text");
        }
        /* -1 leaves room for the terminator the port writes into its fixed slot. */
        if (value.size() >= (FZX::kCourseStringMax - 1)) {
            throw std::runtime_error(where + ": string is longer than " + std::to_string(FZX::kCourseStringMax - 1) +
                                     " bytes, so the table is not text");
        }
        value.push_back(static_cast<char>(b));
    }

    throw std::runtime_error(where + ": string is unterminated at the end of the ROM");
}

std::vector<std::string> ReadStringTable(const std::vector<uint8_t>& rom, YAML::Node& node, const std::string& key) {
    if (!node[key]) {
        throw std::runtime_error("FZX:COURSE_STRINGS is missing the '" + key + "' block");
    }

    YAML::Node table = node[key];
    const auto offset = GetSafeNode<uint32_t>(table, "offset");
    const auto count = GetSafeNode<uint32_t>(table, "count");
    const auto bias = GetSafeNode<uint32_t>(table, "bias");

    if (count == 0) {
        throw std::runtime_error("FZX:COURSE_STRINGS '" + key + "' declares a count of zero");
    }

    std::vector<std::string> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const std::string where = Where(key, i);
        const uint32_t pointer = ReadBigEndianU32(rom, static_cast<size_t>(offset) + (i * 4), where);
        values.push_back(ReadRomString(rom, pointer, bias, where));
    }

    /* A table of nothing but empty strings resolves cleanly and is still wrong: it means the
     * pointers happened to land on padding. Real tables always carry text. */
    bool anyText = false;
    for (const std::string& value : values) {
        if (!value.empty()) {
            anyText = true;
            break;
        }
    }
    if (!anyText) {
        throw std::runtime_error("FZX:COURSE_STRINGS '" + key + "' resolved to nothing but empty strings");
    }

    return values;
}

std::vector<uint8_t> ReadBgmTable(const std::vector<uint8_t>& rom, YAML::Node& node) {
    if (!node["bgm"]) {
        throw std::runtime_error("FZX:COURSE_STRINGS is missing the 'bgm' block");
    }

    YAML::Node table = node["bgm"];
    const auto offset = GetSafeNode<uint32_t>(table, "offset");
    const auto count = GetSafeNode<uint32_t>(table, "count");

    if (count == 0) {
        throw std::runtime_error("FZX:COURSE_STRINGS 'bgm' declares a count of zero");
    }
    if ((static_cast<size_t>(offset) + count) > rom.size()) {
        throw std::runtime_error("FZX:COURSE_STRINGS 'bgm' runs past the end of the ROM");
    }

    std::vector<uint8_t> values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t id = rom[static_cast<size_t>(offset) + i];
        if ((id > kMaxBgmId) && (id != kNoneBgmId)) {
            throw std::runtime_error(Where("bgm", i) + ": " + std::to_string(id) + " is not a BgmId");
        }
        values.push_back(id);
    }

    return values;
}

/* Zero-fills up to the next 4-byte boundary so the reader can keep taking aligned words. */
void PadTo4(LUS::BinaryWriter& writer, size_t written) {
    for (size_t i = written % 4; (i != 0) && (i < 4); i++) {
        writer.Write(static_cast<uint8_t>(0));
    }
}

} // namespace

ExportResult FZX::CourseStringsHeaderExporter::Export(std::ostream& write, std::shared_ptr<IParsedData> raw,
                                                      std::string& entryName, YAML::Node& node,
                                                      std::string* replacement) {
    (void) raw;
    (void) entryName;
    (void) node;
    (void) replacement;

    /* Nothing to declare: the decomp defines sTrackNames, sTrackSubtitles and D_800CF4D8 itself,
     * and the port overrides them at runtime from the binary resource. */
    write << "/* FZX:COURSE_STRINGS carries no C declarations; see port/gdx_course_strings.c. */\n";
    return std::nullopt;
}

ExportResult FZX::CourseStringsCodeExporter::Export(std::ostream& write, std::shared_ptr<IParsedData> raw,
                                                    std::string& entryName, YAML::Node& node,
                                                    std::string* replacement) {
    (void) raw;
    (void) entryName;
    (void) node;
    (void) replacement;

    /* Emitting definitions here would duplicate the hand-written tables in
     * decomp/src/game/common.c and decomp/src/game/racer.c and fail to link. */
    write << "/* FZX:COURSE_STRINGS is a binary-only resource; the tables stay hand-written. */\n";
    return std::nullopt;
}

ExportResult FZX::CourseStringsBinaryExporter::Export(std::ostream& write, std::shared_ptr<IParsedData> raw,
                                                      std::string& entryName, YAML::Node& node,
                                                      std::string* replacement) {
    (void) entryName;
    (void) node;
    (void) replacement;

    auto writer = LUS::BinaryWriter();
    const auto strings = std::static_pointer_cast<CourseStringsData>(raw);

    WriteHeader(writer, Torch::ResourceType::CourseStrings, 0);

    writer.Write(static_cast<uint32_t>(strings->mNames.size()));
    writer.Write(static_cast<uint32_t>(strings->mSubtitles.size()));
    writer.Write(static_cast<uint32_t>(strings->mBgm.size()));
    writer.Write(static_cast<uint32_t>(kCourseStringMax));

    for (const std::string& value : strings->mNames) {
        writer.Write(static_cast<uint32_t>(value.size()));
        writer.Write(value, false);
        PadTo4(writer, value.size());
    }
    for (const std::string& value : strings->mSubtitles) {
        writer.Write(static_cast<uint32_t>(value.size()));
        writer.Write(value, false);
        PadTo4(writer, value.size());
    }
    for (uint8_t id : strings->mBgm) {
        writer.Write(id);
    }
    PadTo4(writer, strings->mBgm.size());

    writer.Finish(write);
    return std::nullopt;
}

std::optional<std::shared_ptr<IParsedData>> FZX::CourseStringsFactory::parse(std::vector<uint8_t>& buffer,
                                                                            YAML::Node& node) {
    auto names = ReadStringTable(buffer, node, "names");
    auto subtitles = ReadStringTable(buffer, node, "subtitles");
    auto bgm = ReadBgmTable(buffer, node);

    SPDLOG_INFO("FZX:COURSE_STRINGS: {} names, {} subtitles, {} bgm ids (first name \"{}\")", names.size(),
                subtitles.size(), bgm.size(), names[0]);

    return std::make_shared<CourseStringsData>(std::move(names), std::move(subtitles), std::move(bgm));
}
