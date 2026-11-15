/*
 * SPDX-FileCopyrightText: 2026 Chimioo
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "pinyinpersonalprofile.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace libime {

namespace {

constexpr std::string_view kFileMagicV1 = "LIBIME_PERSONAL_V1";
constexpr std::string_view kFileMagicV2 = "LIBIME_PERSONAL_V2";
constexpr size_t kMaxKeyOps = 512;
constexpr size_t kMaxCandidatePrefs = 2048;
constexpr size_t kMaxBacktrackPrefs = 512;
constexpr float kMinOpScore = -4.0F;
constexpr float kMaxOpScore = 6.0F;
constexpr float kMaxCandScore = 8.0F;
constexpr float kMaxBacktrackScore = 6.0F;
constexpr float kMinBacktrackSimilarity = 0.65F;
constexpr int kMaxBacktrackEditDist = 2;
constexpr uint64_t kDecayIntervalTicks = 64;
constexpr float kPeriodicDecay = 0.985F;

bool isAsciiLower(char c) { return c >= 'a' && c <= 'z'; }

std::string normalizeAlphaLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char uc : s) {
        if (uc >= 'A' && uc <= 'Z') {
            out.push_back(static_cast<char>(uc + ('a' - 'A')));
        } else if (uc >= 'a' && uc <= 'z') {
            out.push_back(static_cast<char>(uc));
        }
    }
    return out;
}

struct BacktrackAnalysis {
    bool valid = false;
    float similarity = 0.0F;
    std::vector<PinyinCorrectionOp> ops;
};

// Analyze whether a failed attempt before backspacing represents a high-similarity
// typo for the committed pinyin, extracting keystroke-level correction ops.
BacktrackAnalysis analyzeBacktrackSimilarity(std::string_view failed,
                                             std::string_view target) {
    BacktrackAnalysis result;
    const size_t n = failed.size();
    const size_t m = target.size();
    if (n < 3 || m < 3 || failed == target) {
        return result;
    }
    const size_t lenDiff = n > m ? (n - m) : (m - n);
    if (lenDiff > static_cast<size_t>(kMaxBacktrackEditDist)) {
        return result;
    }
    // Reject pure extra-suffix cancellation (e.g. "woainima" -> "woaini"),
    // except a trailing adjacent double-tap (e.g. "woainii" -> "woaini").
    if (n > m && failed.starts_with(target)) {
        const bool trailingDouble =
            (n == m + 1 && n >= 2 && failed[n - 1] == failed[n - 2]);
        if (!trailingDouble) {
            return result;
        }
    }

    const size_t cols = m + 1;
    std::vector<int> dp((n + 1) * cols, 0);
    auto at = [&](size_t i, size_t j) -> int & { return dp[i * cols + j]; };
    for (size_t i = 0; i <= n; i++) {
        at(i, 0) = static_cast<int>(i);
    }
    for (size_t j = 0; j <= m; j++) {
        at(0, j) = static_cast<int>(j);
    }
    for (size_t i = 1; i <= n; i++) {
        for (size_t j = 1; j <= m; j++) {
            const int subCost = (failed[i - 1] == target[j - 1]) ? 0 : 1;
            int best = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1,
                                 at(i - 1, j - 1) + subCost});
            if (i > 1 && j > 1 && failed[i - 1] == target[j - 2] &&
                failed[i - 2] == target[j - 1]) {
                best = std::min(best, at(i - 2, j - 2) + 1);
            }
            at(i, j) = best;
        }
    }

    const int dist = at(n, m);
    if (dist < 1 || dist > kMaxBacktrackEditDist) {
        return result;
    }
    const float similarity =
        1.0F - static_cast<float>(dist) / static_cast<float>(std::max(n, m));
    if (similarity < kMinBacktrackSimilarity) {
        return result;
    }

    // Backtrace optimal edit operations to feed generalized correction bias.
    size_t i = n;
    size_t j = m;
    while (i > 0 || j > 0) {
        const int cur = at(i, j);
        if (i > 1 && j > 1 && failed[i - 1] == target[j - 2] &&
            failed[i - 2] == target[j - 1] &&
            cur == at(i - 2, j - 2) + 1) {
            result.ops.push_back(PinyinCorrectionOp{PinyinCorrectionOpType::Swap,
                                                    target[j - 2],
                                                    target[j - 1]});
            i -= 2;
            j -= 2;
            continue;
        }
        if (i > 0 && j > 0) {
            const int subCost = (failed[i - 1] == target[j - 1]) ? 0 : 1;
            if (cur == at(i - 1, j - 1) + subCost) {
                if (subCost == 1) {
                    result.ops.push_back(PinyinCorrectionOp{
                        PinyinCorrectionOpType::Sub, failed[i - 1],
                        target[j - 1]});
                }
                i--;
                j--;
                continue;
            }
        }
        if (i > 0 && cur == at(i - 1, j) + 1) {
            const char deleted = failed[i - 1];
            const bool isAdjacentDup =
                (i >= 2 && failed[i - 2] == deleted) ||
                (i < n && failed[i] == deleted);
            if (isAdjacentDup) {
                result.ops.push_back(PinyinCorrectionOp{
                    PinyinCorrectionOpType::Double, deleted, deleted});
            }
            i--;
            continue;
        }
        j--;
    }

    result.valid = true;
    result.similarity = similarity;
    return result;
}

std::string makePrefKey(char ns, std::string_view scope,
                        std::string_view word) {
    std::string key;
    key.reserve(scope.size() + word.size() + 2);
    key.push_back(ns);
    key.append(scope);
    key.push_back('\t');
    key.append(word);
    return key;
}

std::string escapeField(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string unescapeField(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == '\\') {
                out.push_back('\\');
                i++;
                continue;
            }
            if (next == 't') {
                out.push_back('\t');
                i++;
                continue;
            }
            if (next == 'n') {
                out.push_back('\n');
                i++;
                continue;
            }
            if (next == 'r') {
                out.push_back('\r');
                i++;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

std::optional<PinyinCorrectionOp>
PinyinCorrectionOp::fromPackedKey(uint32_t key) {
    auto typeRaw = static_cast<uint8_t>((key >> 16) & 0xFF);
    if (typeRaw < static_cast<uint8_t>(PinyinCorrectionOpType::Sub) ||
        typeRaw > static_cast<uint8_t>(PinyinCorrectionOpType::Swap)) {
        return std::nullopt;
    }
    char typed = static_cast<char>((key >> 8) & 0xFF);
    char intended = static_cast<char>(key & 0xFF);
    if (!isAsciiLower(typed) || !isAsciiLower(intended)) {
        return std::nullopt;
    }
    return PinyinCorrectionOp{
        .type = static_cast<PinyinCorrectionOpType>(typeRaw),
        .typed = typed,
        .intended = intended,
    };
}

std::optional<PinyinCorrectionOp>
extractCorrectionOp(std::string_view rawSyl, std::string_view targetSyl) {
    if (rawSyl.empty() || targetSyl.empty() || rawSyl == targetSyl) {
        return std::nullopt;
    }
    if (rawSyl.size() == targetSyl.size()) {
        size_t diffCount = 0;
        size_t diffPos[2] = {0, 0};
        for (size_t i = 0; i < rawSyl.size(); i++) {
            if (rawSyl[i] != targetSyl[i]) {
                if (diffCount >= 2) {
                    return std::nullopt;
                }
                diffPos[diffCount++] = i;
            }
        }
        if (diffCount == 1) {
            char t = rawSyl[diffPos[0]];
            char d = targetSyl[diffPos[0]];
            if (isAsciiLower(t) && isAsciiLower(d)) {
                return PinyinCorrectionOp{PinyinCorrectionOpType::Sub, t, d};
            }
            return std::nullopt;
        }
        if (diffCount == 2 && diffPos[1] == diffPos[0] + 1 &&
            rawSyl[diffPos[0]] == targetSyl[diffPos[1]] &&
            rawSyl[diffPos[1]] == targetSyl[diffPos[0]]) {
            char a = targetSyl[diffPos[0]];
            char b = targetSyl[diffPos[1]];
            if (isAsciiLower(a) && isAsciiLower(b)) {
                return PinyinCorrectionOp{PinyinCorrectionOpType::Swap, a, b};
            }
        }
        return std::nullopt;
    }

    if (rawSyl.size() == targetSyl.size() + 1) {
        size_t mismatch = targetSyl.size();
        for (size_t i = 0; i < targetSyl.size(); i++) {
            if (rawSyl[i] != targetSyl[i]) {
                mismatch = i;
                break;
            }
        }
        char dup = rawSyl[mismatch];
        if (!isAsciiLower(dup)) {
            return std::nullopt;
        }
        bool isAdjacentDup =
            (mismatch > 0 && rawSyl[mismatch - 1] == dup) ||
            (mismatch + 1 < rawSyl.size() && rawSyl[mismatch + 1] == dup);
        if (!isAdjacentDup) {
            return std::nullopt;
        }
        if (rawSyl.substr(0, mismatch) == targetSyl.substr(0, mismatch) &&
            rawSyl.substr(mismatch + 1) == targetSyl.substr(mismatch)) {
            return PinyinCorrectionOp{PinyinCorrectionOpType::Double, dup, dup};
        }
    }
    return std::nullopt;
}

class PinyinPersonalProfilePrivate {
public:
    struct OpEntry {
        float score = 0.0F;
        uint64_t lastTick = 0;
    };

    struct CandEntry {
        float hits = 0.0F;
        uint64_t lastTick = 0;
    };

    struct BacktrackEntry {
        std::string targetPinyin;
        float strength = 0.0F;
        uint64_t lastTick = 0;
    };

    std::unordered_map<uint32_t, OpEntry> keyOps_;
    std::unordered_map<std::string, CandEntry> candidatePrefs_;
    std::unordered_map<std::string, BacktrackEntry> backtrackPrefs_;
    uint64_t tick_ = 0;

    template <typename Map, typename FieldPtr>
    static void decayValueMap(Map &map, FieldPtr fieldPtr) {
        for (auto it = map.begin(); it != map.end();) {
            (it->second.*fieldPtr) *= kPeriodicDecay;
            if ((it->second.*fieldPtr) < 0.05F) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }

    void advanceTick() {
        tick_++;
        if (tick_ % kDecayIntervalTicks == 0) {
            for (auto it = keyOps_.begin(); it != keyOps_.end();) {
                it->second.score *= kPeriodicDecay;
                if (std::abs(it->second.score) < 0.05F) {
                    it = keyOps_.erase(it);
                } else {
                    ++it;
                }
            }
            decayValueMap(candidatePrefs_, &CandEntry::hits);
            decayValueMap(backtrackPrefs_, &BacktrackEntry::strength);
        }
    }

    template <typename Map>
    static void pruneMapIfOverLimit(Map &map, size_t maxLimit) {
        if (map.size() <= maxLimit) {
            return;
        }
        std::vector<std::pair<typename Map::key_type, uint64_t>> order;
        order.reserve(map.size());
        for (const auto &[k, v] : map) {
            order.emplace_back(k, v.lastTick);
        }
        std::sort(order.begin(), order.end(),
                  [](const auto &a, const auto &b) {
                      return a.second < b.second;
                  });
        const size_t targetSize = maxLimit - maxLimit / 8;
        for (size_t i = 0; i < order.size() && map.size() > targetSize; i++) {
            map.erase(order[i].first);
        }
    }

    float effectiveValue(float val, uint64_t lastTick,
                         float halfLifeTicks) const {
        if (val <= 0.0F) {
            return 0.0F;
        }
        const uint64_t age = tick_ >= lastTick ? (tick_ - lastTick) : 0;
        return val * std::exp(-static_cast<float>(age) / halfLifeTicks);
    }
};

PinyinPersonalProfile::PinyinPersonalProfile()
    : d_ptr(std::make_unique<PinyinPersonalProfilePrivate>()) {}

PinyinPersonalProfile::~PinyinPersonalProfile() = default;

void PinyinPersonalProfile::recordCorrection(
    std::span<const PinyinCorrectionOp> confirmedOps,
    std::span<const PinyinCorrectionOp> rejectedOps) {
    if (confirmedOps.empty() && rejectedOps.empty()) {
        return;
    }
    d_ptr->advanceTick();
    for (const auto &op : confirmedOps) {
        auto &entry = d_ptr->keyOps_[op.packedKey()];
        entry.score =
            std::clamp(entry.score + 1.0F, kMinOpScore, kMaxOpScore);
        entry.lastTick = d_ptr->tick_;
    }
    for (const auto &op : rejectedOps) {
        auto &entry = d_ptr->keyOps_[op.packedKey()];
        entry.score =
            std::clamp(entry.score - 1.25F, kMinOpScore, kMaxOpScore);
        entry.lastTick = d_ptr->tick_;
    }
    PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->keyOps_,
                                                      kMaxKeyOps);
}

void PinyinPersonalProfile::recordCandidateSelection(
    std::string_view rawInput, std::string_view targetPinyin,
    std::string_view chosenWord, std::string_view skippedTopWord,
    std::string_view skippedTopPinyin, bool wasCorrection) {
    if (chosenWord.empty()) {
        return;
    }
    d_ptr->advanceTick();
    const bool userSkippedTop =
        !skippedTopWord.empty() && skippedTopWord != chosenWord;
    const float gain = userSkippedTop ? 1.5F : 0.6F;

    if (!targetPinyin.empty()) {
        auto &entry = d_ptr->candidatePrefs_[makePrefKey('P', targetPinyin,
                                                         chosenWord)];
        entry.hits = std::min(kMaxCandScore, entry.hits + gain);
        entry.lastTick = d_ptr->tick_;

        if (userSkippedTop && !skippedTopPinyin.empty()) {
            auto skipKey = makePrefKey('P', skippedTopPinyin, skippedTopWord);
            if (auto it = d_ptr->candidatePrefs_.find(skipKey);
                it != d_ptr->candidatePrefs_.end()) {
                it->second.hits = std::max(0.0F, it->second.hits - 0.8F);
                if (it->second.hits < 0.05F) {
                    d_ptr->candidatePrefs_.erase(it);
                }
            }
        }
    }

    if (!rawInput.empty() && (wasCorrection || userSkippedTop)) {
        auto &rawEntry =
            d_ptr->candidatePrefs_[makePrefKey('R', rawInput, chosenWord)];
        rawEntry.hits = std::min(kMaxCandScore, rawEntry.hits + gain);
        rawEntry.lastTick = d_ptr->tick_;

        if (userSkippedTop) {
            auto skipRawKey = makePrefKey('R', rawInput, skippedTopWord);
            if (auto it = d_ptr->candidatePrefs_.find(skipRawKey);
                it != d_ptr->candidatePrefs_.end()) {
                it->second.hits = std::max(0.0F, it->second.hits - 1.0F);
                if (it->second.hits < 0.05F) {
                    d_ptr->candidatePrefs_.erase(it);
                }
            }
        }
    } else if (!rawInput.empty()) {
        // Refresh existing raw preference if user picks it again at top-1.
        auto existingKey = makePrefKey('R', rawInput, chosenWord);
        if (auto it = d_ptr->candidatePrefs_.find(existingKey);
            it != d_ptr->candidatePrefs_.end()) {
            it->second.hits = std::min(kMaxCandScore, it->second.hits + 0.4F);
            it->second.lastTick = d_ptr->tick_;
        }
    }

    if (!rawInput.empty()) {
        // Decay conflicting backtrack predictions if the user later accepts a
        // different candidate for this same raw input.
        const auto normRaw = normalizeAlphaLower(rawInput);
        if (!normRaw.empty()) {
            std::string prefix;
            prefix.reserve(normRaw.size() + 2);
            prefix.push_back('B');
            prefix.append(normRaw);
            prefix.push_back('\t');
            for (auto it = d_ptr->backtrackPrefs_.begin();
                 it != d_ptr->backtrackPrefs_.end();) {
                if (it->first.starts_with(prefix) &&
                    std::string_view(it->first).substr(prefix.size()) !=
                        chosenWord) {
                    it->second.strength =
                        std::max(0.0F, it->second.strength - 1.0F);
                    if (it->second.strength < 0.05F) {
                        it = d_ptr->backtrackPrefs_.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
    }

    PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->candidatePrefs_,
                                                      kMaxCandidatePrefs);
}

bool PinyinPersonalProfile::recordBacktrackCorrection(
    std::string_view failedRaw, std::string_view finalRaw,
    std::string_view targetPinyin, std::string_view chosenWord,
    bool learnKeyOps) {
    if (chosenWord.empty()) {
        return false;
    }
    const auto normFailed = normalizeAlphaLower(failedRaw);
    auto normTarget = normalizeAlphaLower(targetPinyin);
    if (normTarget.empty()) {
        normTarget = normalizeAlphaLower(finalRaw);
    }
    const auto analysis = analyzeBacktrackSimilarity(normFailed, normTarget);
    if (!analysis.valid) {
        return false;
    }

    d_ptr->advanceTick();
    if (learnKeyOps && !analysis.ops.empty()) {
        for (const auto &op : analysis.ops) {
            auto &opEntry = d_ptr->keyOps_[op.packedKey()];
            opEntry.score = std::clamp(
                opEntry.score + 0.9F * analysis.similarity, kMinOpScore,
                kMaxOpScore);
            opEntry.lastTick = d_ptr->tick_;
        }
        PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->keyOps_,
                                                          kMaxKeyOps);
    }

    auto key = makePrefKey('B', normFailed, chosenWord);
    auto &entry = d_ptr->backtrackPrefs_[std::move(key)];
    if (!targetPinyin.empty()) {
        entry.targetPinyin.assign(targetPinyin);
    }
    entry.strength = std::min(kMaxBacktrackScore,
                              entry.strength + 1.8F * analysis.similarity);
    entry.lastTick = d_ptr->tick_;
    PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->backtrackPrefs_,
                                                      kMaxBacktrackPrefs);
    return true;
}

float PinyinPersonalProfile::scoreCorrectionOps(
    std::span<const PinyinCorrectionOp> ops) const {
    float total = 0.0F;
    for (const auto &op : ops) {
        auto it = d_ptr->keyOps_.find(op.packedKey());
        if (it == d_ptr->keyOps_.end()) {
            continue;
        }
        total += std::clamp(it->second.score * 0.14F, -0.50F, 0.90F);
    }
    return std::clamp(total, -0.90F, 1.50F);
}

float PinyinPersonalProfile::scoreCandidate(std::string_view rawInput,
                                            std::string_view targetPinyin,
                                            std::string_view word,
                                            bool includeRawPref) const {
    if (word.empty()) {
        return 0.0F;
    }
    float boost = 0.0F;
    if (!targetPinyin.empty()) {
        auto it = d_ptr->candidatePrefs_.find(
            makePrefKey('P', targetPinyin, word));
        if (it != d_ptr->candidatePrefs_.end()) {
            boost += std::min(
                1.80F,
                d_ptr->effectiveValue(it->second.hits, it->second.lastTick,
                                      256.0F) *
                    0.65F);
        }
    }
    if (includeRawPref && !rawInput.empty()) {
        auto it =
            d_ptr->candidatePrefs_.find(makePrefKey('R', rawInput, word));
        if (it != d_ptr->candidatePrefs_.end()) {
            boost += std::min(
                2.20F,
                d_ptr->effectiveValue(it->second.hits, it->second.lastTick,
                                      384.0F) *
                    0.85F);
        }
    }
    return boost;
}

float PinyinPersonalProfile::scoreBacktrackMatch(
    std::string_view rawInput, std::string_view targetPinyin,
    std::string_view word) const {
    const float strength = backtrackMatchStrength(rawInput, word);
    if (strength <= 0.0F) {
        return 0.0F;
    }
    const auto normRaw = normalizeAlphaLower(rawInput);
    auto it = d_ptr->backtrackPrefs_.find(makePrefKey('B', normRaw, word));
    float multiplier = 1.90F;
    if (it != d_ptr->backtrackPrefs_.end() &&
        !it->second.targetPinyin.empty() && !targetPinyin.empty() &&
        it->second.targetPinyin != targetPinyin) {
        multiplier = 1.25F;
    }
    return std::min(4.20F, strength * multiplier);
}

float PinyinPersonalProfile::rawPreferenceHits(std::string_view rawInput,
                                               std::string_view word) const {
    if (rawInput.empty() || word.empty()) {
        return 0.0F;
    }
    auto it = d_ptr->candidatePrefs_.find(makePrefKey('R', rawInput, word));
    if (it == d_ptr->candidatePrefs_.end()) {
        return 0.0F;
    }
    return d_ptr->effectiveValue(it->second.hits, it->second.lastTick, 384.0F);
}

float PinyinPersonalProfile::backtrackMatchStrength(
    std::string_view rawInput, std::string_view word) const {
    if (rawInput.empty() || word.empty()) {
        return 0.0F;
    }
    const auto normRaw = normalizeAlphaLower(rawInput);
    if (normRaw.empty()) {
        return 0.0F;
    }
    auto it = d_ptr->backtrackPrefs_.find(makePrefKey('B', normRaw, word));
    if (it == d_ptr->backtrackPrefs_.end()) {
        return 0.0F;
    }
    return d_ptr->effectiveValue(it->second.strength, it->second.lastTick,
                                 384.0F);
}

namespace {
template <typename Map>
void eraseEntriesWithSuffix(Map &map, std::string_view suffix) {
    for (auto it = map.begin(); it != map.end();) {
        if (it->first.size() > suffix.size() &&
            std::string_view(it->first).ends_with(suffix)) {
            it = map.erase(it);
        } else {
            ++it;
        }
    }
}
} // namespace

void PinyinPersonalProfile::forgetWord(std::string_view word) {
    if (word.empty()) {
        return;
    }
    std::string suffix;
    suffix.reserve(word.size() + 1);
    suffix.push_back('\t');
    suffix.append(word);
    eraseEntriesWithSuffix(d_ptr->candidatePrefs_, suffix);
    eraseEntriesWithSuffix(d_ptr->backtrackPrefs_, suffix);
}

void PinyinPersonalProfile::clear() {
    d_ptr->keyOps_.clear();
    d_ptr->candidatePrefs_.clear();
    d_ptr->backtrackPrefs_.clear();
    d_ptr->tick_ = 0;
}

void PinyinPersonalProfile::load(std::istream &in) {
    clear();
    std::string line;
    if (!std::getline(in, line) ||
        (line != kFileMagicV1 && line != kFileMagicV2)) {
        return;
    }
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string kind;
        if (!std::getline(iss, kind, '\t')) {
            continue;
        }
        if (kind == "T") {
            uint64_t t = 0;
            if (iss >> t) {
                d_ptr->tick_ = t;
            }
        } else if (kind == "O") {
            uint32_t key = 0;
            float score = 0.0F;
            uint64_t lastTick = 0;
            if ((iss >> key >> score >> lastTick) &&
                PinyinCorrectionOp::fromPackedKey(key).has_value()) {
                d_ptr->keyOps_[key] = {
                    std::clamp(score, kMinOpScore, kMaxOpScore), lastTick};
            }
        } else if (kind == "P" || kind == "R") {
            std::string scopeEsc, wordEsc, hitsStr, tickStr;
            if (!std::getline(iss, scopeEsc, '\t') ||
                !std::getline(iss, wordEsc, '\t') ||
                !std::getline(iss, hitsStr, '\t') ||
                !std::getline(iss, tickStr, '\t')) {
                continue;
            }
            try {
                float hits = std::stof(hitsStr);
                uint64_t lastTick = std::stoull(tickStr);
                if (hits >= 0.05F) {
                    auto key = makePrefKey(kind[0], unescapeField(scopeEsc),
                                           unescapeField(wordEsc));
                    d_ptr->candidatePrefs_[std::move(key)] = {
                        std::min(kMaxCandScore, hits), lastTick};
                }
            } catch (...) {
                continue;
            }
        } else if (kind == "B") {
            std::string scopeEsc, wordEsc, pinyinEsc, strengthStr, tickStr;
            if (!std::getline(iss, scopeEsc, '\t') ||
                !std::getline(iss, wordEsc, '\t') ||
                !std::getline(iss, pinyinEsc, '\t') ||
                !std::getline(iss, strengthStr, '\t') ||
                !std::getline(iss, tickStr, '\t')) {
                continue;
            }
            try {
                float strength = std::stof(strengthStr);
                uint64_t lastTick = std::stoull(tickStr);
                if (strength >= 0.05F) {
                    auto key = makePrefKey('B', unescapeField(scopeEsc),
                                           unescapeField(wordEsc));
                    d_ptr->backtrackPrefs_[std::move(key)] = {
                        unescapeField(pinyinEsc),
                        std::min(kMaxBacktrackScore, strength), lastTick};
                }
            } catch (...) {
                continue;
            }
        }
    }
    PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->keyOps_,
                                                      kMaxKeyOps);
    PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->candidatePrefs_,
                                                      kMaxCandidatePrefs);
    PinyinPersonalProfilePrivate::pruneMapIfOverLimit(d_ptr->backtrackPrefs_,
                                                      kMaxBacktrackPrefs);
}

void PinyinPersonalProfile::save(std::ostream &out) const {
    out << kFileMagicV2 << '\n';
    out << "T\t" << d_ptr->tick_ << '\n';
    for (const auto &[key, entry] : d_ptr->keyOps_) {
        out << "O\t" << key << ' ' << entry.score << ' ' << entry.lastTick
            << '\n';
    }
    auto splitPrefKey = [](std::string_view key, char &ns,
                           std::string_view &scope,
                           std::string_view &word) -> bool {
        if (key.size() < 3) {
            return false;
        }
        ns = key[0];
        auto rest = key.substr(1);
        auto tabPos = rest.find('\t');
        if (tabPos == std::string_view::npos) {
            return false;
        }
        scope = rest.substr(0, tabPos);
        word = rest.substr(tabPos + 1);
        return true;
    };
    for (const auto &[key, entry] : d_ptr->candidatePrefs_) {
        char ns = '\0';
        std::string_view scope, word;
        if (splitPrefKey(key, ns, scope, word)) {
            out << ns << '\t' << escapeField(scope) << '\t' << escapeField(word)
                << '\t' << entry.hits << '\t' << entry.lastTick << '\n';
        }
    }
    for (const auto &[key, entry] : d_ptr->backtrackPrefs_) {
        char ns = '\0';
        std::string_view scope, word;
        if (splitPrefKey(key, ns, scope, word)) {
            out << "B\t" << escapeField(scope) << '\t' << escapeField(word)
                << '\t' << escapeField(entry.targetPinyin) << '\t'
                << entry.strength << '\t' << entry.lastTick << '\n';
        }
    }
}

size_t PinyinPersonalProfile::opCount() const { return d_ptr->keyOps_.size(); }

size_t PinyinPersonalProfile::candidatePrefCount() const {
    return d_ptr->candidatePrefs_.size();
}

size_t PinyinPersonalProfile::backtrackCount() const {
    return d_ptr->backtrackPrefs_.size();
}

} // namespace libime
