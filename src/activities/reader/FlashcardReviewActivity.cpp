#include "FlashcardReviewActivity.h"

#include <Arduino.h>  // millis()
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "ReadingTimeHistory.h"  // readingHistoryDayIndex
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictionaryActivityUtils.h"

namespace {

// Small, dependency-free PRNG for the session shuffle (avoids <random> bloat).
uint32_t xorshift32(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

}  // namespace

void FlashcardReviewActivity::onEnter() {
  Activity::onEnter();

  // Resolve today in the user's local calendar day (same convention as reading
  // stats); if the RTC is unavailable, today stays 0 and the scope falls back to
  // all-shuffled inside buildSession.
  uint8_t dayOfWeek = 0, day = 0, month = 0, hour = 0, minute = 0;
  uint16_t year = 0;
  clockOk = halClock.isAvailable() &&
            halClock.getLocalDateTime(SETTINGS.clockUtcOffsetQ, dayOfWeek, day, month, year, hour, minute);
  today = clockOk ? readingHistoryDayIndex(year, month, day) : 0;

  stats = FlashcardDeck::computeStats(cachePath, today);
  cardStyle = SETTINGS.flashcardCardStyle;  // start from the remembered default
  phase = Phase::Overview;                  // pre-session deck stats; the user picks style + scope here
  requestUpdate();
}

void FlashcardReviewActivity::startSession(FlashcardDeck::SessionScope scope) {
  // Persist the picked style + scope as the new defaults (value-change guarded so
  // a single SPIFFS write happens only when something actually changed -- rule 8).
  const uint8_t scopeV = static_cast<uint8_t>(scope);
  if (SETTINGS.flashcardSessionScope != scopeV || SETTINGS.flashcardCardStyle != cardStyle) {
    SETTINGS.flashcardSessionScope = scopeV;
    SETTINGS.flashcardCardStyle = cardStyle;
    SETTINGS.saveToFile();
  }
  buildAndShuffleSession(scope);
  if (!session.empty() && loadCurrentCard()) {
    phase = Phase::Front;
  }
  requestUpdate();
}

void FlashcardReviewActivity::buildAndShuffleSession(FlashcardDeck::SessionScope scope) {
  uint16_t buf[SESSION_CAP];
  const int n = FlashcardDeck::buildSession(cachePath, scope, today, SESSION_CAP, buf);

  session.clear();
  session.reserve(n);
  session.assign(buf, buf + n);

  // Fisher-Yates shuffle for presentation order (selection above is
  // deterministic). Seed from millis so each session differs.
  uint32_t s = static_cast<uint32_t>(millis()) | 1u;
  for (int i = n - 1; i > 0; i--) {
    const int j = static_cast<int>(xorshift32(s) % static_cast<uint32_t>(i + 1));
    std::swap(session[i], session[j]);
  }
}

bool FlashcardReviewActivity::loadCurrentCard() {
  if (cursor >= session.size()) return false;
  return FlashcardDeck::loadWindow(cachePath, session[cursor], 1, &card) == 1;
}

void FlashcardReviewActivity::gradeAndAdvance(bool correctRecall) {
  FlashcardDeck::grade(cachePath, card.word, correctRecall, today);
  reviewed++;
  if (correctRecall) correct++;

  // Recompute the post-grade box locally to count graduations this session
  // (mirrors what grade() persisted).
  uint8_t box = card.box;
  uint32_t due = card.dueDay;
  FlashcardDeck::applyGrade(box, due, correctRecall, today);
  if (FlashcardDeck::isMastered(box)) mastered++;

  // A miss re-queues the card to the session tail for another pass.
  if (!correctRecall && session.size() < static_cast<size_t>(SESSION_CAP) * 2) {
    session.push_back(session[cursor]);
  }

  cursor++;
  if (cursor >= session.size() || !loadCurrentCard()) {
    phase = Phase::Summary;
  } else {
    phase = Phase::Front;
  }
  requestUpdate();
}

void FlashcardReviewActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void FlashcardReviewActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        // Back face = the live definition (never re-recorded into history).
        startActivityForResult(std::make_unique<DictionaryDefinitionActivity>(
                                   renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(),
                                   true, cachePath, controller.getRecordHistory(), controller.getLookupWord(),
                                   DictionaryLookupController::toHistStatus(controller.getFoundStatus())),
                               [this](const ActivityResult&) {
                                 phase = Phase::AwaitingGrade;  // definition viewed -> prompt for grade
                                 requestUpdate();
                               });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        // No definition to show, but the user has "flipped" -> let them grade.
        phase = Phase::AwaitingGrade;
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        requestUpdate();  // flip aborted; stay on the front
        break;
      default:
        break;
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  switch (phase) {
    case Phase::Overview:
      // Up/Down toggle the card style (cloze <-> word+context); Left/Right start
      // the session in that style with due-first / shuffled order respectively.
      if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
          mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        cardStyle = (cardStyle == CrossPointSettings::FLASHCARD_STYLE_CLOZE)
                        ? CrossPointSettings::FLASHCARD_STYLE_WORD_CONTEXT
                        : CrossPointSettings::FLASHCARD_STYLE_CLOZE;
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        startSession(FlashcardDeck::SessionScope::DueFirst);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        startSession(FlashcardDeck::SessionScope::AllShuffled);
      }
      break;
    case Phase::Summary:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        setResult(ActivityResult{});
        finish();
      }
      break;
    case Phase::Front:
      if (cardStyle == CrossPointSettings::FLASHCARD_STYLE_CLOZE) {
        // Cloze hides the word: a front grade first REVEALS the answer (word
        // filled into the excerpt) where the grade can still be re-picked.
        if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
          pendingCorrect = true;
          phase = Phase::Revealed;
          requestUpdate();
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
          pendingCorrect = false;
          phase = Phase::Revealed;
          requestUpdate();
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          controller.startLookup(card.word, /*recordHistory=*/false);  // flip -> back face
        }
      } else {
        // Word+context already shows the word: grade directly, no reveal step.
        if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
          gradeAndAdvance(/*correctRecall=*/true);
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
          gradeAndAdvance(/*correctRecall=*/false);
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          controller.startLookup(card.word, /*recordHistory=*/false);  // flip -> back face
        }
      }
      break;
    case Phase::Revealed:
      // Answer is visible: Left/Right re-pick the grade, Confirm commits + advances.
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        pendingCorrect = true;
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        pendingCorrect = false;
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        gradeAndAdvance(pendingCorrect);
      }
      break;
    case Phase::AwaitingGrade:
      // Back face viewed: grade is available; Confirm re-flips to the definition.
      if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        gradeAndAdvance(/*correctRecall=*/true);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        gradeAndAdvance(/*correctRecall=*/false);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        controller.startLookup(card.word, /*recordHistory=*/false);  // flip -> back face
      }
      break;
  }
}

void FlashcardReviewActivity::displayList() {
  const auto o = renderer.getOrientation();
  const bool landscape =
      o == GfxRenderer::Orientation::LandscapeClockwise || o == GfxRenderer::Orientation::LandscapeCounterClockwise;
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (landscape && --pagesUntilFullRefresh <= 0) {
    mode = HalDisplay::HALF_REFRESH;
    pagesUntilFullRefresh = std::max(1, SETTINGS.getRefreshFrequency());
  }
  renderer.displayBuffer(mode);
}

int FlashcardReviewActivity::drawWrappedCentered(int fontId, int contentTop, int contentBottom, int pageWidth,
                                                 const char* text, const char* highlightWord, bool maskHighlight) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = metrics.listRowHeight;
  const int maxWidth = pageWidth - 2 * metrics.contentSidePadding;
  const int hlLen = highlightWord ? static_cast<int>(strlen(highlightWord)) : 0;

  // Greedy word wrap into a fixed line buffer (excerpt is capped, so bounded).
  char line[FlashcardDeck::EXCERPT_MAX + 1];
  int lineLen = 0;
  int y = contentTop;

  // Underline each case-insensitive occurrence of highlightWord in the just-drawn,
  // centered line. Both span endpoints are measured as cumulative prefixes from the
  // line start (null-terminating in place, no extra buffer) so they match drawText's
  // own left-to-right glyph layout (differential rounding + kerning); measuring the
  // matched word in isolation would drop the boundary kern and drift by ~a char.
  auto underline = [&](int top) {
    if (hlLen == 0) return;
    // Pen origin of the centered line = drawCenteredText's x = (W - inkWidth)/2.
    const int startX = (pageWidth - renderer.getTextWidth(fontId, line)) / 2;
    for (int i = 0; i + hlLen <= lineLen;) {
      bool match = true;
      for (int k = 0; k < hlLen; k++)
        if (std::tolower(static_cast<unsigned char>(line[i + k])) !=
            std::tolower(static_cast<unsigned char>(highlightWord[k]))) {
          match = false;
          break;
        }
      if (!match) {
        i++;
        continue;
      }
      // Pen origin of the match = advance cursor of the prefix (NOT ink width, which
      // loses side bearings/spaces). Then place the underline at the word's actual
      // ink extents from that origin, so it spans exactly the visible glyphs.
      const char saveStart = line[i];
      line[i] = '\0';
      const int penX = startX + renderer.getTextAdvanceWidth(fontId, line);
      line[i] = saveStart;
      const char saveEnd = line[i + hlLen];
      line[i + hlLen] = '\0';
      int inkMinX = 0, inkMaxX = 0;
      renderer.getTextInkBounds(fontId, line + i, &inkMinX, &inkMaxX);
      line[i + hlLen] = saveEnd;
      const int ulX = penX + inkMinX;       // visible left edge of the word
      const int ulW = inkMaxX - inkMinX;    // ink width of the word
      // Cloze front: white-box the word glyphs (small pad), leaving just the underline.
      if (maskHighlight) renderer.fillRect(ulX - 2, top, ulW + 4, lineHeight, false);
      renderer.fillRect(ulX, top + lineHeight - 3, ulW, 2, true);
      i += hlLen;
    }
  };

  auto flush = [&]() {
    if (lineLen == 0) return;
    line[lineLen] = '\0';
    if (y + lineHeight <= contentBottom) {
      renderer.drawCenteredText(fontId, y, line);
      underline(y);
    }
    y += lineHeight;
    lineLen = 0;
  };

  const char* p = text;
  while (*p) {
    const char* wordStart = p;
    while (*p && *p != ' ') p++;
    const int wordLen = static_cast<int>(p - wordStart);
    while (*p == ' ') p++;  // skip spaces

    // Candidate line = current + (space) + word.
    char cand[FlashcardDeck::EXCERPT_MAX + 1];
    int candLen = lineLen;
    memcpy(cand, line, lineLen);
    if (candLen > 0 && candLen < FlashcardDeck::EXCERPT_MAX) cand[candLen++] = ' ';
    const int copy = std::min(wordLen, FlashcardDeck::EXCERPT_MAX - candLen);
    memcpy(cand + candLen, wordStart, copy);
    candLen += copy;
    cand[candLen] = '\0';

    if (lineLen > 0 && renderer.getTextWidth(fontId, cand) > maxWidth) {
      flush();
      const int c2 = std::min(wordLen, FlashcardDeck::EXCERPT_MAX);
      memcpy(line, wordStart, c2);
      lineLen = c2;
    } else {
      memcpy(line, cand, candLen);
      lineLen = candLen;
    }
  }
  flush();
  return y;
}

void FlashcardReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (controller.render()) return;

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int sessionTotal = static_cast<int>(session.size());
  const int pos = std::min(static_cast<int>(cursor) + 1, std::max(1, sessionTotal));
  char titleBuf[64];
  if (phase == Phase::Front || phase == Phase::Revealed || phase == Phase::AwaitingGrade) {
    snprintf(titleBuf, sizeof(titleBuf), "%s  %d/%d", tr(STR_FLASHCARDS_REVIEW), pos, sessionTotal);
  } else {
    snprintf(titleBuf, sizeof(titleBuf), "%s", tr(STR_FLASHCARDS_REVIEW));
  }
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, titleBuf);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight;

  if (stats.total == 0) {
    const int midY = contentTop + (contentBottom - contentTop) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_FLASHCARDS_EMPTY));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayList();
    return;
  }

  switch (phase) {
    case Phase::Overview:
      renderOverview(contentTop, contentBottom, pageWidth);
      break;
    case Phase::Front:
      renderFront(contentTop, contentBottom, pageWidth);
      break;
    case Phase::Revealed:
      renderRevealed(contentTop, contentBottom, pageWidth);
      break;
    case Phase::AwaitingGrade:
      renderAwaitingGrade(contentTop, contentBottom, pageWidth);
      break;
    case Phase::Summary:
      renderSummary(contentTop, contentBottom, pageWidth);
      break;
  }
  displayList();
}

void FlashcardReviewActivity::renderOverview(int contentTop, int contentBottom, int pageWidth) {
  (void)contentBottom;  // seven rungs + subline fit the content area by construction
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = metrics.contentSidePadding;
  const int rowH = metrics.listRowHeight;
  const int countRight = pageWidth - metrics.contentSidePadding;

  int y = contentTop;

  // Summary subline: due-now count, else days until the next card is due.
  char sub[64];
  if (stats.due > 0) {
    snprintf(sub, sizeof(sub), "%s: %d", tr(STR_FLASHCARD_STAT_DUE), stats.due);
  } else if (stats.nextDueDay > 0 && today > 0) {
    snprintf(sub, sizeof(sub), "%s: %u", tr(STR_FLASHCARD_STAT_NEXT_DUE), stats.nextDueDay - today);
  } else {
    snprintf(sub, sizeof(sub), "%s: %d", tr(STR_FLASHCARD_STAT_TOTAL), stats.total);
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, sub);
  y += rowH;

  // Card-style selector: show BOTH options; the active one is drawn as a filled
  // (inverted) pill so the selection is obvious on e-ink. Up/Down toggles it.
  const bool clozeSel = cardStyle == CrossPointSettings::FLASHCARD_STYLE_CLOZE;
  const int sf = UI_10_FONT_ID;
  const int lh = renderer.getLineHeight(sf);
  const char* opt0 = tr(STR_FLASHCARD_STYLE_CLOZE);
  const char* opt1 = tr(STR_FLASHCARD_STYLE_WORD_CONTEXT);
  constexpr int pad = 6;
  constexpr int gap = 18;
  const int box0 = renderer.getTextWidth(sf, opt0) + 2 * pad;
  const int box1 = renderer.getTextWidth(sf, opt1) + 2 * pad;
  int x = (pageWidth - (box0 + gap + box1)) / 2;

  auto drawOpt = [&](const char* t, int bw, bool sel) {
    if (sel) {
      renderer.fillRect(x, y - 1, bw, lh + 2, true);  // inverted pill
      renderer.drawText(sf, x + pad, y, t, false);    // white text on black
    } else {
      renderer.drawText(sf, x + pad, y, t, true);
    }
    x += bw;
  };
  drawOpt(opt0, box0, clozeSel);
  x += gap;
  drawOpt(opt1, box1, !clozeSel);
  y += rowH + metrics.verticalSpacing;

  // The seven Leitner rungs: New (box 0), B1..B5, Done (retired/mastered).
  int counts[7];
  counts[0] = stats.boxHist[0];
  for (int b = 1; b <= 5; b++) counts[b] = stats.boxHist[b];
  counts[6] = stats.mastered;
  int maxCount = 1;
  for (int i = 0; i < 7; i++) maxCount = std::max(maxCount, counts[i]);

  const int barX = left + 56;                                 // fixed column so all bars align
  const int barMaxW = std::max(1, (countRight - 48) - barX);  // leave ~48px for the count

  for (int i = 0; i < 7; i++) {
    // B0 = new, B1..B5 = Leitner boxes, B6 = mastered/retired (uniform width so
    // the labels and bars align).
    char label[8];
    snprintf(label, sizeof(label), "B%d", i);
    renderer.drawText(UI_10_FONT_ID, left, y, label);

    const int barW = counts[i] > 0 ? std::max(3, barMaxW * counts[i] / maxCount) : 0;
    if (barW > 0) renderer.fillRect(barX, y + 3, barW, rowH - 8, true);

    char cbuf[8];
    snprintf(cbuf, sizeof(cbuf), "%d", counts[i]);
    renderer.drawText(UI_10_FONT_ID, countRight - renderer.getTextWidth(UI_10_FONT_ID, cbuf), y, cbuf);

    y += rowH;
  }

  // Pick the session order here: Left = due-first, Right = shuffled.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_FLASHCARD_SCOPE_DUE_FIRST), tr(STR_FLASHCARD_SCOPE_ALL_SHUFFLED));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::renderCardFace(int contentTop, int contentBottom, int pageWidth, bool showWord) {
  const int bodyFont = SETTINGS.getDefinitionFontId();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Bold word header — drawn only when the answer is shown. Cloze front omits it
  // (the row stays blank) so the excerpt below keeps the same position either way.
  if (showWord) {
    renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, contentTop + metrics.listRowHeight, card.word.c_str(), true,
                              EpdFontFamily::BOLD);
  }

  if (!card.excerpt.empty()) {
    // Excerpt with the word underlined in context; masked (white-boxed) when hidden.
    drawWrappedCentered(bodyFont, contentTop + metrics.listRowHeight * 3, contentBottom, pageWidth,
                        card.excerpt.c_str(), card.word.c_str(), /*maskHighlight=*/!showWord);
  } else if (!showWord) {
    // No excerpt to blank into: fall back to a centered "____" placeholder.
    renderer.drawCenteredText(bodyFont, contentTop + metrics.listRowHeight * 3, "____");
  }

  drawChapterFooter(contentBottom);
}

void FlashcardReviewActivity::renderFront(int contentTop, int contentBottom, int pageWidth) {
  // Word+context shows the word; cloze hides it (masked excerpt + blank header).
  const bool showWord = cardStyle != CrossPointSettings::FLASHCARD_STYLE_CLOZE;
  renderCardFace(contentTop, contentBottom, pageWidth, showWord);

  // Flip on Confirm; pass/fail always available without flipping first.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PASS), tr(STR_FLASHCARD_FAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::renderRevealed(int contentTop, int contentBottom, int pageWidth) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Reveal in place: same layout as the cloze front, but the word now shows (bold
  // header + filled, underlined in the excerpt) -- the only change is the blank
  // resolving, no vertical jump.
  renderCardFace(contentTop, contentBottom, pageWidth, /*showWord=*/true);

  // Live grade pick as a small footer line (above the chapter footer / hints) so it
  // never displaces the excerpt.
  char pick[48];
  snprintf(pick, sizeof(pick), "%s", pendingCorrect ? tr(STR_FLASHCARD_PASS) : tr(STR_FLASHCARD_FAIL));
  renderer.drawCenteredText(UI_10_FONT_ID, contentBottom - metrics.listRowHeight * 2, pick, true,
                            EpdFontFamily::ITALIC);

  // Left/Right re-pick the grade; Confirm commits it and advances.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEXT_FIELD), tr(STR_FLASHCARD_PASS), tr(STR_FLASHCARD_FAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::drawChapterFooter(int contentBottom) {
  if (card.chapter.empty()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.drawCenteredText(UI_10_FONT_ID, contentBottom - metrics.listRowHeight, card.chapter.c_str(), true,
                            EpdFontFamily::ITALIC);
}

void FlashcardReviewActivity::renderAwaitingGrade(int contentTop, int contentBottom, int pageWidth) {
  // Back face viewed: same revealed card face (word shown, underlined in context).
  renderCardFace(contentTop, contentBottom, pageWidth, /*showWord=*/true);

  // Left = pass, Right = fail; Confirm re-flips. Consistent with the front face.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PASS), tr(STR_FLASHCARD_FAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::renderSummary(int contentTop, int contentBottom, int pageWidth) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  int y = contentTop + metrics.listRowHeight;
  char buf[64];
  renderer.drawCenteredText(NOTOSERIF_16_FONT_ID, y, tr(STR_FLASHCARD_SUMMARY_TITLE), true, EpdFontFamily::BOLD);
  y += metrics.listRowHeight * 2;

  snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_REVIEWED), reviewed);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += metrics.listRowHeight;
  snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_CORRECT), correct);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  y += metrics.listRowHeight;
  snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_MASTERED), mastered);
  renderer.drawCenteredText(UI_12_FONT_ID, y, buf);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
