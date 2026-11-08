#pragma once

// NotoSans 12 with the IPA Extensions, Spacing Modifier Letters and θ added to
// the usual Latin/Vietnamese set (lib/EpdFont/builtinFonts/notosans_ipa_12_regular.h),
// so phonetic transcriptions such as /ˈθɪŋkɪŋ/ render instead of showing gaps.
// The UI faces (Ubuntu) carry only ə of those blocks.
//
// Regenerate with:
//   python fontconvert.py notosans_ipa_12_regular 12 ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
//     --additional-intervals 0x0250,0x02AF --additional-intervals 0x02B0,0x02FF \
//     --additional-intervals 0x03B8,0x03B8 > ../builtinFonts/notosans_ipa_12_regular.h
//
// Hand-picked id, kept outside the generated fontIds.h; only needs to differ
// from those ids and from 0.
#define IPA_12_FONT_ID (0x1BA12001)
