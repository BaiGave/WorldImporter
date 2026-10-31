#pragma once

#include <string>
#include "model.h"
#include "nbtutils.h"

// Bake vanilla sign text (front_text / back_text, or legacy Text1..Text4) into
// one quad per non-empty side and merge it into `out` in world space.
// Only minecraft:*_sign, *_wall_sign, *_hanging_sign, *_wall_hanging_sign.
// Returns false when every line is empty or the font cannot be loaded.
//
// Styles follow the 1.21.1 client (Style / TextColor / ChatFormatting /
// StringDecomposer / Font.StringRenderOutput). Each glyph uses its own colour
// (named formatting colours and #RRGGBB), bold, italic, underline, and
// strikethrough. "§" codes inside a literal are applied the same way; §r
// returns to that component's style. Child components ("extra") inherit the
// parent style unless they set a field. Obfuscated (§k) is drawn as the real
// glyph. A glowing outline is the darkened dye colour for every glyph.
// translate/score/selector are not resolved; a "translate" key is copied
// literally when the component has no text. Custom fonts are not selected.
bool BuildSignTextModel(const std::string& ns, const std::string& blockNameWithState,
    const NbtTagPtr& blockEntityNbt, int x, int y, int z, ModelData& out);
