#pragma once
#include <cstdint>
#include <string>

class GfxRenderer;

// Shared flashcard "card face" renderer: a bold centered word header (optional)
// plus the page-local excerpt with the word underlined in context, plus the
// chapter footer. Extracted from FlashcardReviewActivity so the review session
// and the deck-browser detail view draw one identical layout (CLAUDE.md dict
// rule: reuse over duplication; one copy keeps the binary small).
//
// When `showWord` is false (cloze front), the bold header is omitted and the
// word is white-boxed in the excerpt, leaving only its underline -- so front and
// reveal share one layout and the answer fills into place with no vertical jump.
namespace FlashcardCardFace {

// Render the card face into [contentTop, contentBottom) across `pageWidth`.
// `lookupCount` draws a small "xN" beside the word when > 1 (and the word is shown);
// pass 1 (or 0) to omit it.
void render(GfxRenderer& renderer, int contentTop, int contentBottom, int pageWidth, const std::string& word,
            const std::string& excerpt, const std::string& chapter, bool showWord, uint32_t lookupCount = 1);

}  // namespace FlashcardCardFace
