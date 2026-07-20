#include "TorchUtils.h"

#include <stack>
#include <algorithm>
#include <Companion.h>
#include <factories/BaseFactory.h>
#include "spdlog/spdlog.h"

namespace fs = std::filesystem;

uint32_t Torch::translate(const uint32_t offset) {
    if (SEGMENT_NUMBER(offset) > 0x01) {
        auto segment = SEGMENT_NUMBER(offset);
        const auto addr = Companion::Instance->GetFileOffsetFromSegmentedAddr(segment);
        if (!addr.has_value()) {
            SPDLOG_ERROR("Segment data missing from game config\nPlease add an entry for segment {}", segment);
            throw std::runtime_error("Failed to find offset");
        }

        return addr.value() + SEGMENT_OFFSET(offset);
    }

    return offset;
}

int getFileDepth(const fs::path& base, const fs::path& p) {
    return std::distance(base.begin(), p.begin());
}

std::vector<fs::directory_entry> Torch::getRecursiveEntries(const fs::path baseDir) {
    std::vector<fs::directory_entry> entries;

    // GDX determinism: canonicalize every walked path to one separator convention. File-identity
    // maps (gAddrMap/gProcessedFiles) are keyed by path STRINGS, and mixed-separator variants of
    // the same file (srcdir passed with '\', yaml refs with '/') defeated the dedup on Windows —
    // externally-referenced yamls were processed twice, changing the archive. lexically_normal +
    // make_preferred gives the same canonical string to every producer of these keys.
    for (const auto& entry : fs::recursive_directory_iterator(baseDir)) {
        entries.push_back(fs::directory_entry(entry.path().lexically_normal().make_preferred()));
    }

    // Deterministic, cross-platform ordering. recursive_directory_iterator yields
    // entries in unspecified, filesystem-dependent order; sorting by the generic
    // (forward-slash) path string guarantees the archive entry order is byte-identical
    // on Windows and Linux. Required for reproducible O2R output (GDX determinism).
    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().generic_string() < b.path().generic_string();
              });

    return entries;
}