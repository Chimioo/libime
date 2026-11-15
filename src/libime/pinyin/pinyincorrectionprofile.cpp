/*
 * SPDX-FileCopyrightText: 2024-2024 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "pinyincorrectionprofile.h"
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <fcitx-utils/macros.h>
#include "pinyindata.h"
#include "pinyinencoder.h"

namespace libime {

namespace {

// QWERTY geometry with row stagger. Row 1 is offset by 0.5, row 2 by 1.0,
// matching a real keyboard where `a` sits between `q` and `w`.
struct KeyPos {
    float x;
    float y;
};

std::unordered_map<char, KeyPos> qwertyKeyPositions(float row2Offset = 1.0F) {
    std::unordered_map<char, KeyPos> pos;
    const std::string row0 = "qwertyuiop";
    const std::string row1 = "asdfghjkl";
    const std::string row2 = "zxcvbnm";
    for (size_t i = 0; i < row0.size(); i++) {
        pos[row0[i]] = {static_cast<float>(i), 0.0F};
    }
    for (size_t i = 0; i < row1.size(); i++) {
        pos[row1[i]] = {static_cast<float>(i) + 0.5F, 1.0F};
    }
    for (size_t i = 0; i < row2.size(); i++) {
        pos[row2[i]] = {static_cast<float>(i) + row2Offset, 2.0F};
    }
    return pos;
}

enum class NeighborTier { Close, Far };

using WeightedNeighborMap =
    std::unordered_map<char, std::vector<std::pair<char, NeighborTier>>>;

// Build 2D neighbor map: close neighbors get a small penalty, far neighbors
// get a medium penalty. This covers vertical and diagonal slips that the
// old same-row-only mapping missed.
WeightedNeighborMap smartQwertyNeighbors() {
    // Match mobile 9-key row1 + Caps-padded 7-key row2 geometry (1.5F offset).
    auto pos = qwertyKeyPositions(1.5F);
    WeightedNeighborMap result;
    for (const auto &[c1, p1] : pos) {
        for (const auto &[c2, p2] : pos) {
            if (c1 == c2) {
                continue;
            }
            float dx = p1.x - p2.x;
            float dy = p1.y - p2.y;
            if (std::abs(dy) > 1.01F) {
                continue;
            }
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= 1.15F) {
                result[c1].emplace_back(c2, NeighborTier::Close);
            } else if (dist <= 1.50F) {
                result[c1].emplace_back(c2, NeighborTier::Far);
            }
        }
    }
    return result;
}

/*
 * Helper function to create mapping based on keyboard rows.
 * Function assume that the key can only be corrected to the key adjcent to it.
 */
std::unordered_map<char, std::vector<char>>
mappingFromRows(const std::vector<std::string> &rows) {
    std::unordered_map<char, std::vector<char>> result;
    for (const auto &row : rows) {
        for (size_t i = 0; i < row.size(); i++) {
            std::vector<char> items;
            if (i > 0) {
                items.push_back(row[i - 1]);
            }
            if (i + 1 < row.size()) {
                items.push_back(row[i + 1]);
            }
            result[row[i]] = std::move(items);
        }
    }
    return result;
}

std::unordered_map<char, std::vector<char>>
getProfileMapping(BuiltinPinyinCorrectionProfile profile) {
    switch (profile) {
    case BuiltinPinyinCorrectionProfile::Qwerty:
        return mappingFromRows({"qwertyuiop", "asdfghjkl", "zxcvbnm"});
    case BuiltinPinyinCorrectionProfile::SmartQwerty:
        break;
    }

    return {};
}
} // namespace

class PinyinCorrectionProfilePrivate {
public:
    PinyinMap pinyinMap_;
    std::unordered_map<char, std::vector<char>> correctionMap_;
};

PinyinCorrectionProfile::PinyinCorrectionProfile(
    BuiltinPinyinCorrectionProfile profile)
    : d_ptr(std::make_unique<PinyinCorrectionProfilePrivate>()) {
    FCITX_D();
    if (profile == BuiltinPinyinCorrectionProfile::Qwerty) {
        // Legacy basic profile: same-row adjacent only, single-error with
        // Correction flag. Kept for back-compat / opt-out of smart.
        auto mapping = getProfileMapping(profile);
        d->correctionMap_ = mapping;
        d->pinyinMap_ = getPinyinMapV2();
        if (mapping.empty()) {
            return;
        }
        std::vector<PinyinEntry> newEntries;
        for (const auto &item : d->pinyinMap_) {
            for (size_t i = 0; i < item.pinyin().size(); i++) {
                auto chr = item.pinyin()[i];
                auto swap = mapping.find(chr);
                if (swap == mapping.end() || swap->second.empty()) {
                    continue;
                }
                auto newEntry = item.pinyin();
                for (auto sub : swap->second) {
                    newEntry[i] = sub;
                    newEntries.emplace_back(
                        newEntry.data(), item.initial(), item.final(),
                        item.flags() | PinyinFuzzyFlag::Correction);
                    newEntry[i] = chr;
                }
            }
        }
        for (const auto &newEntry : newEntries) {
            d->pinyinMap_.insert(newEntry);
        }
        return;
    }
    if (profile != BuiltinPinyinCorrectionProfile::SmartQwerty) {
        d->pinyinMap_ = getPinyinMapV2();
        return;
    }
    // Smart Qwerty: 2D geometry with tiered penalties.
    // Close neighbors use the SmartCorrection flag, far neighbors use
    // SmartCorrectionFar. Single-error expansion only.
    auto weighted = smartQwertyNeighbors();
    // correctionMap_ keeps the unweighted union for shuangpin/back-compat.
    for (const auto &[chr, neighbors] : weighted) {
        auto &vec = d->correctionMap_[chr];
        for (const auto &[sub, tier] : neighbors) {
            vec.push_back(sub);
        }
    }
    d->pinyinMap_ = getPinyinMapV2();
    std::vector<PinyinEntry> newEntries;
    std::unordered_set<std::string> seen;
    auto dedupInsert = [&newEntries, &seen](
                           std::string typo, PinyinInitial initial,
                           PinyinFinal final_, PinyinFuzzyFlags flags) {
        std::string key;
        key.reserve(typo.size() + 4);
        key += typo;
        key.push_back('|');
        key.push_back(static_cast<char>(initial));
        key.push_back(static_cast<char>(final_));
        key.push_back(static_cast<char>(flags.toInteger() & 0xff));
        key.push_back(static_cast<char>((flags.toInteger() >> 8) & 0xff));
        key.push_back(static_cast<char>((flags.toInteger() >> 16) & 0xff));
        if (seen.insert(key).second) {
            newEntries.emplace_back(typo.data(), initial, final_, flags);
        }
    };
    // Collect base entries first (snapshot, since we insert later).
    std::vector<PinyinEntry> baseEntries;
    baseEntries.reserve(d->pinyinMap_.size());
    for (const auto &item : d->pinyinMap_) {
        // Avoid compounding existing typo/partial variants with SmartCorrection.
        if (item.flags().test(PinyinFuzzyFlag::CommonTypo) ||
            item.flags().test(PinyinFuzzyFlag::AdvancedTypo) ||
            item.flags().test(PinyinFuzzyFlag::PartialFinal)) {
            continue;
        }
        baseEntries.push_back(item);
    }

    auto insertSubstitutions = [&](NeighborTier targetTier,
                                   PinyinFuzzyFlag flag) {
        for (const auto &item : baseEntries) {
            const auto &syllable = item.pinyin();
            for (size_t i = 0; i < syllable.size(); i++) {
                auto chr = syllable[i];
                auto it = weighted.find(chr);
                if (it == weighted.end()) {
                    continue;
                }
                auto typo = syllable;
                for (const auto &[sub, tier] : it->second) {
                    if (tier != targetTier) {
                        continue;
                    }
                    typo[i] = sub;
                    dedupInsert(typo, item.initial(), item.final(),
                                item.flags() | flag);
                    typo[i] = chr;
                }
            }
        }
    };

    // Insert lower-penalty variants first so dedupInsert retains best match.
    insertSubstitutions(NeighborTier::Close, PinyinFuzzyFlag::SmartCorrection);

    for (const auto &item : baseEntries) {
        const auto &syllable = item.pinyin();
        // Cover ui -> iu transposition omitted from AdvancedTypo.
        for (size_t i = 0; i + 1 < syllable.size(); i++) {
            if (syllable[i] == 'u' && syllable[i + 1] == 'i') {
                auto typo = syllable;
                std::swap(typo[i], typo[i + 1]);
                dedupInsert(typo, item.initial(), item.final(),
                            item.flags() | PinyinFuzzyFlag::SmartCorrection);
            }
        }
    }

    for (const auto &item : baseEntries) {
        const auto &syllable = item.pinyin();
        // Absorb accidental double-taps; chars valid across boundaries get Far.
        for (size_t i = 0; i < syllable.size(); i++) {
            const char chr = syllable[i];
            if (i > 0 && chr == syllable[i - 1]) {
                continue;
            }
            const bool canCrossBoundary =
                (chr == 'a' || chr == 'e' || chr == 'o' || chr == 'n' ||
                 chr == 'g' || chr == 'r');
            const auto flag = canCrossBoundary
                                  ? PinyinFuzzyFlag::SmartCorrectionFar
                                  : PinyinFuzzyFlag::SmartCorrection;
            std::string typo;
            typo.reserve(syllable.size() + 1);
            typo.append(syllable, 0, i + 1);
            typo.push_back(chr);
            typo.append(syllable, i + 1, std::string::npos);
            dedupInsert(typo, item.initial(), item.final(),
                        item.flags() | flag);
        }
    }

    insertSubstitutions(NeighborTier::Far, PinyinFuzzyFlag::SmartCorrectionFar);
    for (const auto &newEntry : newEntries) {
        d->pinyinMap_.insert(newEntry);
    }
}

PinyinCorrectionProfile::PinyinCorrectionProfile(
    const std::unordered_map<char, std::vector<char>> &mapping)
    : d_ptr(std::make_unique<PinyinCorrectionProfilePrivate>()) {
    FCITX_D();
    d->correctionMap_ = mapping;
    // Fill with the original pinyin map.
    d->pinyinMap_ = getPinyinMapV2();
    if (mapping.empty()) {
        return;
    }
    // Re-map all entry with the correction mapping.
    std::vector<PinyinEntry> newEntries;
    for (const auto &item : d->pinyinMap_) {
        for (size_t i = 0; i < item.pinyin().size(); i++) {
            auto chr = item.pinyin()[i];
            auto swap = mapping.find(chr);
            if (swap == mapping.end() || swap->second.empty()) {
                continue;
            }
            auto newEntry = item.pinyin();
            for (auto sub : swap->second) {
                newEntry[i] = sub;
                newEntries.push_back(
                    PinyinEntry(newEntry.data(), item.initial(), item.final(),
                                item.flags() | PinyinFuzzyFlag::Correction));
                newEntry[i] = chr;
            }
        }
    }
    for (const auto &newEntry : newEntries) {
        d->pinyinMap_.insert(newEntry);
    }
}

PinyinCorrectionProfile::~PinyinCorrectionProfile() = default;

const PinyinMap &PinyinCorrectionProfile::pinyinMap() const {
    FCITX_D();
    return d->pinyinMap_;
}

const std::unordered_map<char, std::vector<char>> &
PinyinCorrectionProfile::correctionMap() const {
    FCITX_D();
    return d->correctionMap_;
}
} // namespace libime
