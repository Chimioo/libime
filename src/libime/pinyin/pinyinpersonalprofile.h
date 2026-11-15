/*
 * SPDX-FileCopyrightText: 2026 Chimioo
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _FCITX_LIBIME_PINYIN_PINYINPERSONALPROFILE_H_
#define _FCITX_LIBIME_PINYIN_PINYINPERSONALPROFILE_H_

#include <cstddef>
#include <cstdint>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <libime/pinyin/libimepinyin_export.h>

namespace libime {

enum class PinyinCorrectionOpType : uint8_t {
    Sub = 1,
    Double = 2,
    Swap = 3,
};

struct LIBIMEPINYIN_EXPORT PinyinCorrectionOp {
    PinyinCorrectionOpType type;
    char typed = '\0';
    char intended = '\0';

    uint32_t packedKey() const {
        return (static_cast<uint32_t>(type) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(typed)) << 8) |
               static_cast<uint32_t>(static_cast<unsigned char>(intended));
    }

    static std::optional<PinyinCorrectionOp> fromPackedKey(uint32_t key);
};

/// Classify a single-error relationship between raw typed syllable and target syllable.
LIBIMEPINYIN_EXPORT std::optional<PinyinCorrectionOp>
extractCorrectionOp(std::string_view rawSyl, std::string_view targetSyl);

class PinyinPersonalProfilePrivate;

/**
 * \brief Bounded per-user learning store for smart correction bias and
 *        candidate preferences (recency + frequency).
 */
class LIBIMEPINYIN_EXPORT PinyinPersonalProfile {
public:
    PinyinPersonalProfile();
    ~PinyinPersonalProfile();
    PinyinPersonalProfile(const PinyinPersonalProfile &) = delete;
    PinyinPersonalProfile &operator=(const PinyinPersonalProfile &) = delete;

    void recordCorrection(std::span<const PinyinCorrectionOp> confirmedOps,
                          std::span<const PinyinCorrectionOp> rejectedOps);

    void recordCandidateSelection(std::string_view rawInput,
                                  std::string_view targetPinyin,
                                  std::string_view chosenWord,
                                  std::string_view skippedTopWord,
                                  std::string_view skippedTopPinyin,
                                  bool wasCorrection);

    bool recordBacktrackCorrection(std::string_view failedRaw,
                                   std::string_view finalRaw,
                                   std::string_view targetPinyin,
                                   std::string_view chosenWord,
                                   bool learnKeyOps);

    float scoreCorrectionOps(std::span<const PinyinCorrectionOp> ops) const;

    float scoreCandidate(std::string_view rawInput,
                         std::string_view targetPinyin, std::string_view word,
                         bool includeRawPref = true) const;

    float scoreBacktrackMatch(std::string_view rawInput,
                              std::string_view targetPinyin,
                              std::string_view word) const;

    float rawPreferenceHits(std::string_view rawInput,
                            std::string_view word) const;

    float backtrackMatchStrength(std::string_view rawInput,
                                 std::string_view word) const;

    void forgetWord(std::string_view word);
    void clear();

    void load(std::istream &in);
    void save(std::ostream &out) const;

    size_t opCount() const;
    size_t candidatePrefCount() const;
    size_t backtrackCount() const;

private:
    std::unique_ptr<PinyinPersonalProfilePrivate> d_ptr;
};

} // namespace libime

#endif // _FCITX_LIBIME_PINYIN_PINYINPERSONALPROFILE_H_
