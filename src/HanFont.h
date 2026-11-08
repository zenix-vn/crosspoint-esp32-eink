#pragma once

// Noto Sans SC 18, regular, holding only ASCII and the 2 968 Han characters of
// the HSK decks (lib/EpdFont/builtinFonts/notosans_sc_18_hsk.h): the Chinese
// vocabulary trainer draws its words with it when the reader font has no Han
// characters, so the HSK decks work without installing an SD-card font. About
// 380 KB of flash. Noto Sans SC is under the SIL Open Font License 1.1.
//
// Regenerate after scripts/vocab/build_zh_decks.py rewrites han_intervals.txt,
// with NotoSansSC[wght].ttf from github.com/google/fonts (ofl/notosanssc) copied
// to ../builtinFonts/source/NotoSansSC/:
//   python fontconvert.py notosans_sc_18_hsk 18 ../builtinFonts/source/NotoSansSC/NotoSansSC[wght].ttf \
//     --2bit --compress --zopfli --basic-intervals-only \
//     --intervals-file ../../../scripts/vocab/han_intervals.txt > ../builtinFonts/notosans_sc_18_hsk.h
//
// Hand-picked id, kept outside the generated fontIds.h; only needs to differ
// from those ids and from 0.
#define HAN_18_FONT_ID (0x1BA18002)
