/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <fcitx-utils/log.h>
#include "libime/core/historybigram.h"
#include "libime/core/lattice.h"
#include "libime/core/userlanguagemodel.h"
#include "libime/pinyin/pinyincontext.h"
#include "libime/pinyin/pinyincorrectionprofile.h"
#include "libime/pinyin/pinyindictionary.h"
#include "libime/pinyin/pinyinencoder.h"
#include "libime/pinyin/pinyinime.h"
#include "libime/pinyin/shuangpinprofile.h"
#include "testdir.h"

using namespace libime;

namespace {

size_t candidateIndex(PinyinContext &c, const std::string &candidate) {
    auto iter =
        std::ranges::find(c.candidates(), candidate, &SentenceResult::toString);
    FCITX_ASSERT(iter != c.candidates().end());
    return std::distance(c.candidates().begin(), iter);
}

void selectCandidate(PinyinContext &c, const std::string &candidate) {
    c.select(candidateIndex(c, candidate));
}

void testPinyin(PinyinIME &ime) {
    PinyinContext c(&ime);

    c.type("nihaozhongguo");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidateSet().count("你好中国"));
    c.setCursor(5);
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidatesToCursor().size() ==
                 c.candidatesToCursorSet().size());
    FCITX_ASSERT(c.candidates().size() != c.candidatesToCursor().size())
        << c.candidatesToCursorSet();
    FCITX_ASSERT(!c.candidatesToCursorSet().count("你好中国"));
    FCITX_ASSERT(c.candidatesToCursorSet().count("你好"));
    c.setCursor(0);
    auto iter = std::ranges::find(c.candidates(), "你好中国",
                                  &SentenceResult::toString);
    FCITX_ASSERT(iter != c.candidates().end());
    FCITX_ASSERT(!ime.dict()->lookupWord(PinyinDictionary::UserDict,
                                         "ni'hao'zhong'guo", "你好中国"));
    c.select(std::distance(c.candidates().begin(), iter));
    c.learn();
    FCITX_ASSERT(ime.model()->history().containsBigram("你", "好"));
    FCITX_ASSERT(ime.model()->history().containsBigram("好", "中国"));
}

void testShuangpin(PinyinIME &ime) {
    PinyinContext c(&ime);

    c.setUseShuangpin(true);

    c.type("bkqilb");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidateSet().count("冰淇淋"));
    c.clear();

    c.type("bkqiln");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(!c.candidateSet().contains("冰淇淋"));
    c.clear();

    ime.setCorrectionProfile(std::make_shared<PinyinCorrectionProfile>(
        BuiltinPinyinCorrectionProfile::Qwerty));
    ime.setShuangpinProfile(std::make_shared<libime::ShuangpinProfile>(
        ShuangpinBuiltinProfile::Xiaohe, ime.correctionProfile().get()));
    ime.setFuzzyFlags({PinyinFuzzyFlag::Inner, PinyinFuzzyFlag::Correction});

    c.type("bkqiln");
    FCITX_ASSERT(c.candidates().size() == c.candidateSet().size());
    FCITX_ASSERT(c.candidateSet().contains("冰淇淋"));
    c.clear();
}

void testHistory(PinyinIME &ime) {
    PinyinContext c(&ime);
    c.type("kuai");
    FCITX_ASSERT(c.candidateSet().contains("会"));
    auto kuaiIndex = candidateIndex(c, "会");
    c.clear();
    c.type("hui");
    FCITX_ASSERT(c.candidateSet().contains("会"));
    selectCandidate(c, "会");
    FCITX_ASSERT(c.selected());
    c.learn();
    c.clear();
    c.type("kuai");
    auto kuaiIndexNew = candidateIndex(c, "会");
    FCITX_ASSERT(kuaiIndexNew == kuaiIndex);
}

void testSmartQwertyAndPersonalLearning(PinyinIME &ime) {
    ime.clearPersonalProfile();
    ime.setCorrectionProfile(std::make_shared<PinyinCorrectionProfile>(
        BuiltinPinyinCorrectionProfile::SmartQwerty));
    ime.setFuzzyFlags({
        PinyinFuzzyFlag::VE_UE,
        PinyinFuzzyFlag::CommonTypo,
        PinyinFuzzyFlag::AdvancedTypo,
        PinyinFuzzyFlag::Inner,
        PinyinFuzzyFlag::Correction,
        PinyinFuzzyFlag::SmartCorrection,
        PinyinFuzzyFlag::SmartCorrectionFar,
    });

    // Double-tap, ui->iu swap, and mobile row2 alignment.
    {
        PinyinContext c(&ime);
        c.type("niihao");
        FCITX_ASSERT(!c.candidates().empty());
        FCITX_ASSERT(c.candidates()[0].toString() == "你好");
    }
    {
        PinyinContext c(&ime);
        c.type("shiu");
        FCITX_ASSERT(!c.candidates().empty());
        FCITX_ASSERT(c.candidates()[0].toString() == "水");
    }
    // Single-syllable exact protection keeps "否" above "都".
    {
        PinyinContext c(&ime);
        c.type("fou");
        FCITX_ASSERT(!c.candidates().empty());
        FCITX_ASSERT(c.candidates()[0].toString() == "否");
    }

    // Personalized learning: selecting non-top-1 candidate promotes it on retry,
    // cancel() does not pollute, forget/clear/switches work cleanly.
    ime.setAdaptiveCorrectionLearning(true);
    ime.setAdaptiveCandidateLearning(true);
    {
        PinyinContext c(&ime);
        c.type("zhudao");
        FCITX_ASSERT(c.candidates().size() >= 2);
        const size_t zhiIdx = candidateIndex(c, "知道");
        FCITX_ASSERT(zhiIdx < c.candidates().size());
        c.select(zhiIdx);
        c.cancel(); // Must roll back pending observation cleanly.
        c.clear();
        FCITX_ASSERT(ime.personalProfile().opCount() == 0);
        FCITX_ASSERT(ime.personalProfile().candidatePrefCount() == 0);
    }
    {
        PinyinContext c(&ime);
        c.type("zhudao");
        const size_t zhiIdx = candidateIndex(c, "知道");
        c.select(zhiIdx);
        FCITX_ASSERT(c.selected());
        c.learn();
    }
    FCITX_ASSERT(ime.personalProfile().opCount() > 0);
    FCITX_ASSERT(ime.personalProfile().candidatePrefCount() > 0);
    {
        PinyinContext c(&ime);
        c.type("zhudao");
        FCITX_ASSERT(!c.candidates().empty());
        FCITX_ASSERT(c.candidates()[0].toString() == "知道");
    }
    // Roundtrip serialization + forget + toggle off.
    std::stringstream ss;
    ime.savePersonalProfile(ss);
    ime.clearPersonalProfile();
    FCITX_ASSERT(ime.personalProfile().candidatePrefCount() == 0);
    ss.seekg(0);
    ime.loadPersonalProfile(ss);
    FCITX_ASSERT(ime.personalProfile().candidatePrefCount() > 0);

    ime.setAdaptiveCandidateLearning(false);
    ime.setAdaptiveCorrectionLearning(false);
    {
        PinyinContext c(&ime);
        c.type("zhudao");
        FCITX_ASSERT(!c.candidates().empty());
        FCITX_ASSERT(c.candidates()[0].toString() == "主导");
    }
    ime.forgetPersonalWord("知道");
    ime.setAdaptiveCandidateLearning(true);
    ime.setAdaptiveCorrectionLearning(true);

    // Backspace similarity learning: low-similarity change of mind is ignored;
    // high-similarity typo correction before backspace is learned & obeys switch.
    ime.setAdaptiveBackspaceLearning(true);
    {
        PinyinContext c(&ime);
        c.type("pingguo");
        while (!c.empty()) {
            c.backspace();
        }
        c.type("xiangjiao");
        if (!c.candidates().empty()) {
            c.select(0);
            c.learn();
        }
    }
    FCITX_ASSERT(ime.personalProfile().backtrackCount() == 0);
}

} // namespace

int main() {
    fcitx::Log::setLogRule("libime=5");
    libime::PinyinIME ime(
        std::make_unique<PinyinDictionary>(),
        std::make_unique<UserLanguageModel>(LIBIME_BINARY_DIR "/data/sc.lm"));
    ime.setNBest(2);
    ime.dict()->load(PinyinDictionary::SystemDict,
                     LIBIME_BINARY_DIR "/data/sc.dict",
                     PinyinDictFormat::Binary);
    PinyinFuzzyFlags flags = PinyinFuzzyFlag::Inner;
    ime.setFuzzyFlags(flags);
    ime.setScoreFilter(1.0F);
    ime.setShuangpinProfile(
        std::make_shared<ShuangpinProfile>(ShuangpinBuiltinProfile::Xiaohe));

    testPinyin(ime);
    testShuangpin(ime);
    testHistory(ime);
    testSmartQwertyAndPersonalLearning(ime);

    return 0;
}
