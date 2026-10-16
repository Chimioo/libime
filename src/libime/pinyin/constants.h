/*
 * SPDX-FileCopyrightText: 2017-2017 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef _FCITX_LIBIME_PINYIN_CONSTANTS_H_
#define _FCITX_LIBIME_PINYIN_CONSTANTS_H_

namespace libime {
constexpr float PINYIN_DISTANCE_PENALTY_FACTOR = 1.8;
constexpr int PINYIN_ADVACNED_TYPO_FUZZY_FACTOR = 5;
constexpr int PINYIN_CORRECTION_FUZZY_FACTOR = 10;
// Smart correction uses smaller penalties than legacy Correction (10) so a
// single close-key typo can be outscored by the language model, while
// multi-error corrections stay penalized enough for exact matches to win.
constexpr int PINYIN_SMART_CORRECTION_CLOSE_FACTOR = 4;
constexpr int PINYIN_SMART_CORRECTION_FAR_FACTOR = 8;
} // namespace libime

#endif // _FCITX_LIBIME_PINYIN_CONSTANTS_H_
