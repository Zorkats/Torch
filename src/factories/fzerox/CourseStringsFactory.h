#pragma once

/* CourseStringsFactory -- per-course track names, subtitles and BGM assignment.
 *
 * These three tables are the only course-identity data F-Zero X keeps as ordinary compiled C
 * data rather than as an addressable asset, which is why no other recipe reaches them and why a
 * ROM hack's own track names never showed up in the port. They live at fixed offsets in the
 * cart's code/rodata:
 *
 *   sTrackNames      pointer table -> NUL-terminated ASCII   (decomp/src/game/common.c)
 *   sTrackSubtitles  pointer table -> NUL-terminated ASCII   (decomp/src/overlays/course_select)
 *   D_800CF4D8       24 raw u8 BGM ids                       (decomp/src/game/racer.c)
 *
 * Every offset, count and RAM->ROM bias comes from the recipe node, so a different ROM revision
 * is a yaml change and not a code change. The pointer tables store RAM addresses; `bias` is what
 * turns one back into a ROM offset (romOffset = ramAddress - bias).
 *
 * VALIDATION IS THE POINT. A hack that relocated any of these tables would otherwise be
 * extracted as plausible-looking garbage and shown to the player as course names. Instead every
 * pointer must land inside the ROM, every string must be printable and bounded, and every BGM id
 * must be a real BgmId. Any failure throws, Companion::ParseNode records a damaged asset and
 * skips the entry, and the port then finds no resource and keeps its compiled-in vanilla text.
 * A wrong table produces vanilla names, never nonsense.
 *
 * Only the Binary exporter carries data. The Header and Code exporters deliberately emit nothing
 * but a comment: the decomp already defines these tables by hand in C, and a generated
 * definition would collide with them.
 */

#include <factories/BaseFactory.h>

#include <cstdint>
#include <string>
#include <vector>

namespace FZX {

/* Bytes one extracted string may occupy INCLUDING its terminator. Anything longer is a decoding
 * failure rather than a long name: the widest vanilla subtitle ("PSYCHEDELIC EXPERIENCE") is 23
 * bytes and the on-screen font runs out of room well before 64. The port stores strings in fixed
 * slots of exactly this size and refuses a payload that declares more. */
constexpr uint32_t kCourseStringMax = 64;

class CourseStringsData : public IParsedData {
  public:
    std::vector<std::string> mNames;
    std::vector<std::string> mSubtitles;
    std::vector<uint8_t> mBgm;

    CourseStringsData(std::vector<std::string> names, std::vector<std::string> subtitles, std::vector<uint8_t> bgm)
        : mNames(std::move(names)), mSubtitles(std::move(subtitles)), mBgm(std::move(bgm)) {
    }
};

class CourseStringsHeaderExporter : public BaseExporter {
    ExportResult Export(std::ostream& write, std::shared_ptr<IParsedData> data, std::string& entryName,
                        YAML::Node& node, std::string* replacement) override;
};

class CourseStringsCodeExporter : public BaseExporter {
    ExportResult Export(std::ostream& write, std::shared_ptr<IParsedData> data, std::string& entryName,
                        YAML::Node& node, std::string* replacement) override;
};

class CourseStringsBinaryExporter : public BaseExporter {
    ExportResult Export(std::ostream& write, std::shared_ptr<IParsedData> data, std::string& entryName,
                        YAML::Node& node, std::string* replacement) override;
};

class CourseStringsFactory : public BaseFactory {
  public:
    std::optional<std::shared_ptr<IParsedData>> parse(std::vector<uint8_t>& buffer, YAML::Node& data) override;
    inline std::unordered_map<ExportType, std::shared_ptr<BaseExporter>> GetExporters() override {
        return {
            REGISTER(Code, CourseStringsCodeExporter)
            REGISTER(Header, CourseStringsHeaderExporter)
            REGISTER(Binary, CourseStringsBinaryExporter)
        };
    }
};
} // namespace FZX
