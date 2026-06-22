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
#include "activities/util/ConfirmationActivity.h"

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
  // A suspended-review pass is a transient mode, never the remembered default.
  suspendedMode = (scope == FlashcardDeck::SessionScope::Suspended);

  // Persist the picked style + scope as the new defaults (value-change guarded so
  // a single SPIFFS write happens only when something actually changed -- rule 8).
  // The Suspended scope is excluded -- it must not become the startup default.
  if (!suspendedMode) {
    const uint8_t scopeV = static_cast<uint8_t>(scope);
    if (SETTINGS.flashcardSessionScope != scopeV || SETTINGS.flashcardCardStyle != cardStyle) {
      SETTINGS.flashcardSessionScope = scopeV;
      SETTINGS.flashcardCardStyle = cardStyle;
      SETTINGS.saveToFile();
    }
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

  advanceCard();
}

void FlashcardReviewActivity::advanceCard() {
  cursor++;
  if (cursor >= session.size() || !loadCurrentCard()) {
    phase = Phase::Summary;
  } else {
    phase = Phase::Front;
  }
  requestUpdate();
}

void FlashcardReviewActivity::navigateCard(int delta) {
  const int next = static_cast<int>(cursor) + delta;
  if (next < 0 || next >= static_cast<int>(session.size())) return;  // clamp at the ends
  cursor = static_cast<size_t>(next);
  if (!loadCurrentCard()) return;
  phase = Phase::Front;  // always return to the front context when browsing
  requestUpdate();
}

void FlashcardReviewActivity::returnToOverview() {
  // Abandon the in-progress session and return to the deck overview. Reset the
  // session vector + cursor + tallies so a fresh session can be started, and
  // recompute deck stats so any grading/suspending done this session is reflected.
  // cardStyle is kept (it is the overview's picker state).
  session.clear();
  cursor = 0;
  reviewed = correct = mastered = suspended = 0;
  suspendedMode = false;
  stats = FlashcardDeck::computeStats(cachePath, today);
  phase = Phase::Overview;
  requestUpdate();
}

void FlashcardReviewActivity::promptSuspendToggle() {
  // Capture the word now -- `card` is overwritten as soon as we advance.
  const std::string word = card.word;
  const bool unsuspending = suspendedMode;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(
          renderer, mappedInput,
          unsuspending ? tr(STR_FLASHCARD_UNSUSPEND_TITLE) : tr(STR_FLASHCARD_SUSPEND_TITLE), word),
      [this, word, unsuspending](const ActivityResult& res) {
        if (res.isCancelled) {
          requestUpdate();  // back to the card, unchanged
          return;
        }
        if (unsuspending) {
          FlashcardDeck::unsuspend(cachePath, word);
        } else {
          FlashcardDeck::suspend(cachePath, word);
        }
        suspended++;
        advanceCard();
      });
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
    // Back from a card abandons the in-progress session and returns to the deck
    // overview (the flashcard "home" page) rather than exiting to the reader; from
    // the overview/summary Back exits the activity.
    if (phase == Phase::Front || phase == Phase::Revealed || phase == Phase::AwaitingGrade) {
      returnToOverview();
    } else {
      DictUtils::cancelAndFinish(*this);
    }
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
      } else if (stats.suspended > 0 && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startSession(FlashcardDeck::SessionScope::Suspended);  // review/unsuspend set-aside cards
      }
      break;
    case Phase::Summary:
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        setResult(ActivityResult{});
        finish();
      }
      break;
    case Phase::Front:
      // The "prev" side button (PageBack -- follows Side Button Layout + CW swap)
      // sets the card aside (or restores it, in a suspended-review session), after
      // a confirmation prompt. Available on every card face.
      if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        promptSuspendToggle();
      } else if (suspendedMode) {
        // Suspended review: no grading. Left/Right page through the set-aside
        // cards, Confirm flips to the definition, Up (handled above) resumes.
        if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
          navigateCard(-1);
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
          navigateCard(+1);
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          controller.startLookup(card.word, /*recordHistory=*/false);  // flip -> back face
        }
      } else if (cardStyle == CrossPointSettings::FLASHCARD_STYLE_CLOZE) {
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
      // Answer is visible. Confirm is reserved for flip (back-face definition). The
      // two side buttons follow the Side Button Layout + CW swap: "prev" (PageBack)
      // suspends, "next" (PageForward) commits the picked grade and advances.
      // Left/Right re-pick the grade.
      // (suspendedMode never reaches this phase -- it skips the cloze reveal.)
      if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        promptSuspendToggle();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
        gradeAndAdvance(pendingCorrect);  // "next": commit the grade + advance
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        pendingCorrect = true;
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        pendingCorrect = false;
        requestUpdate();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        controller.startLookup(card.word, /*recordHistory=*/false);  // flip -> back face
      }
      break;
    case Phase::AwaitingGrade:
      // Back face viewed. The "prev" side button (PageBack) suspends/unsuspends;
      // Confirm re-flips to the definition. In suspendedMode grading is disabled
      // (Left/Right ignored).
      if (mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
        promptSuspendToggle();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        controller.startLookup(card.word, /*recordHistory=*/false);  // flip -> back face
      } else if (suspendedMode) {
        // Suspended review: Left/Right page through cards instead of grading.
        if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
          navigateCard(-1);
        } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
          navigateCard(+1);
        }
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
        gradeAndAdvance(/*correctRecall=*/true);
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
        gradeAndAdvance(/*correctRecall=*/false);
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
    // Overview / summary: show the whole-deck card count alongside the title.
    snprintf(titleBuf, sizeof(titleBuf), "%s (%d)", tr(STR_FLASHCARDS_REVIEW), stats.total);
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
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = metrics.contentSidePadding;
  const int rowH = metrics.listRowHeight;
  const int countRight = pageWidth - metrics.contentSidePadding;

  int y = contentTop;

  // Card-style selector: two equal half-width buttons spanning the content width.
  // The active one is filled (inverted), the other outlined. Up/Down toggles it.
  const bool clozeSel = cardStyle == CrossPointSettings::FLASHCARD_STYLE_CLOZE;
  const int sf = UI_10_FONT_ID;
  const int lh = renderer.getLineHeight(sf);
  const char* opt0 = tr(STR_FLASHCARD_STYLE_CLOZE);
  const char* opt1 = tr(STR_FLASHCARD_STYLE_WORD_CONTEXT);
  const int halfW = (pageWidth - 2 * left) / 2;

  auto drawHalf = [&](const char* t, int boxX, bool sel) {
    if (sel) {
      renderer.fillRect(boxX, y - 1, halfW, lh + 2, true);  // inverted (filled)
    } else {
      renderer.drawRect(boxX, y - 1, halfW, lh + 2, true);  // outlined
    }
    const int tw = renderer.getTextWidth(sf, t);
    renderer.drawText(sf, boxX + (halfW - tw) / 2, y, t, !sel);  // white text when filled
  };
  drawHalf(opt0, left, clozeSel);
  drawHalf(opt1, left + halfW, !clozeSel);
  y += rowH + metrics.verticalSpacing * 2;

  // Cards due for review right now -- the only "do this now" number; New/Mastered
  // are conveyed by the box ladder below. Same font (UI_10) as the progress row.
  char stat[48];
  snprintf(stat, sizeof(stat), "%s %d", tr(STR_FLASHCARD_STAT_DUE), stats.due);
  renderer.drawCenteredText(UI_10_FONT_ID, y, stat);
  y += rowH + metrics.verticalSpacing * 2;

  // Box-distribution ladder: one cell per Leitner stage -- New (box 0), boxes 1..5,
  // then Mastered -- each showing how many cards currently sit there. Cards march
  // rightward as they are promoted, so the distribution visibly shifts session to
  // session: progress you can see long before anything graduates. Counts come
  // straight from boxHist[] + mastered, no new storage.
  constexpr int LADDER_CELLS = 7;  // New, box1..box5, Mastered
  const int counts[LADDER_CELLS] = {stats.boxHist[0], stats.boxHist[1], stats.boxHist[2], stats.boxHist[3],
                                    stats.boxHist[4], stats.boxHist[5], stats.mastered};
  const int cellW = (countRight - left) / LADDER_CELLS;
  const int cellH = lh + 2;
  for (int i = 0; i < LADDER_CELLS; i++) {
    const int cx = left + i * cellW;
    const bool fill = (i == LADDER_CELLS - 1) && counts[i] > 0;  // Mastered cell filled solid
    if (fill) {
      renderer.fillRect(cx, y - 1, cellW, cellH, true);
    } else {
      renderer.drawRect(cx, y - 1, cellW, cellH, true);
    }
    char c[8];
    snprintf(c, sizeof(c), "%d", counts[i]);
    const int cw = renderer.getTextWidth(sf, c);
    renderer.drawText(sf, cx + (cellW - cw) / 2, y, c, !fill);  // white digit when filled
  }
  // Caption row under the ladder: New / Mastered at the ends only (the middle cells
  // are the intermediate Leitner boxes -- left to right = least to most learned).
  const int capY = y + cellH;
  renderer.drawText(SMALL_FONT_ID, left, capY, tr(STR_FLASHCARD_STAT_NEW), true);
  const char* mLabel = tr(STR_FLASHCARD_SUMMARY_MASTERED);
  renderer.drawText(SMALL_FONT_ID, countRight - renderer.getTextWidth(SMALL_FONT_ID, mLabel), capY, mLabel, true);
  y += cellH + renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing * 2;

  // Weighted learning-progress bar: every card contributes its box position toward
  // the bar (box b of 6, Mastered = 6/6), so it advances on each promotion rather
  // than only when a card graduates -- far more responsive than mastered/total. The
  // denominator is the active deck (suspended cards excluded).
  int active = stats.mastered;
  long weighted = stats.mastered * 6L;
  for (int b = 0; b < 6; b++) {
    active += stats.boxHist[b];
    weighted += static_cast<long>(stats.boxHist[b]) * b;
  }
  const int pct = active > 0 ? static_cast<int>(weighted * 100 / (active * 6L)) : 0;
  renderer.drawText(UI_10_FONT_ID, left, y, tr(STR_FLASHCARD_PROGRESS));
  char pctBuf[8];
  snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
  const int pctW = renderer.getTextWidth(UI_10_FONT_ID, pctBuf);
  renderer.drawText(UI_10_FONT_ID, countRight - pctW, y, pctBuf);
  const int barX = left + renderer.getTextWidth(UI_10_FONT_ID, tr(STR_FLASHCARD_PROGRESS)) + 12;
  const int barMaxW = std::max(1, (countRight - pctW - 12) - barX);
  const int barH = std::max(6, lh - 4);       // a little shorter than the text line
  const int barY = y + (lh - barH) / 2;        // vertically centred with the label/percent
  renderer.drawRect(barX, barY, barMaxW, barH, true);  // outline
  if (pct > 0) renderer.fillRect(barX, barY, barMaxW * pct / 100, barH, true);
  y += rowH + metrics.verticalSpacing;

  // When cards are set aside, centre a compact "(N)" directly above the Suspend
  // button. The Suspend label (Confirm) is right-aligned inside the left hint
  // group; mirror the hint-row geometry (guide font == SMALL_FONT_ID) to find its
  // centre. Constants track RoundedRaffTheme::drawButtonHints.
  if (stats.suspended > 0) {
    constexpr int sidePadding = 20;
    constexpr int groupGap = 10;
    constexpr int innerEdgePadding = 16;
    const int groupWidth = (pageWidth - sidePadding * 2 - groupGap) / 2;
    const int selectW = renderer.getTextWidth(SMALL_FONT_ID, tr(STR_FLASHCARD_SCOPE_SUSPENDED));
    const int buttonCenter = sidePadding + groupWidth - innerEdgePadding - selectW / 2;
    char badge[12];
    snprintf(badge, sizeof(badge), "(%d)", stats.suspended);
    const int bw = renderer.getTextWidth(SMALL_FONT_ID, badge);
    renderer.drawText(SMALL_FONT_ID, buttonCenter - bw / 2, contentBottom - renderer.getLineHeight(SMALL_FONT_ID) - 2,
                      badge, true);
  }

  // Pick the session order here: Left = due-first, Right = shuffled. When cards
  // are set aside, Confirm enters the suspended-review (unsuspend) pass.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), stats.suspended > 0 ? tr(STR_FLASHCARD_SCOPE_SUSPENDED) : "",
                            tr(STR_FLASHCARD_SCOPE_DUE_FIRST), tr(STR_FLASHCARD_SCOPE_ALL_SHUFFLED));
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
  // Suspended review always shows the word (recall isn't being tested).
  const bool showWord = suspendedMode || cardStyle != CrossPointSettings::FLASHCARD_STYLE_CLOZE;
  renderCardFace(contentTop, contentBottom, pageWidth, showWord);
  drawSuspendHint(contentTop, contentBottom);

  // Suspended review: flip (read definition) + Prev/Next browsing + resume (Up).
  // Normal review: flip on Confirm; pass/fail always available without flipping.
  const auto labels =
      suspendedMode
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PREV), tr(STR_FLASHCARD_NEXT))
          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PASS), tr(STR_FLASHCARD_FAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::renderRevealed(int contentTop, int contentBottom, int pageWidth) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Reveal in place: same layout as the cloze front, but the word now shows (bold
  // header + filled, underlined in the excerpt) -- the only change is the blank
  // resolving, no vertical jump.
  renderCardFace(contentTop, contentBottom, pageWidth, /*showWord=*/true);
  drawSuspendHint(contentTop, contentBottom, /*showNextHint=*/true);

  // Live grade pick as a small footer line (above the chapter footer / hints) so it
  // never displaces the excerpt.
  char pick[48];
  snprintf(pick, sizeof(pick), "%s", pendingCorrect ? tr(STR_FLASHCARD_PASS) : tr(STR_FLASHCARD_FAIL));
  renderer.drawCenteredText(UI_10_FONT_ID, contentBottom - metrics.listRowHeight * 2, pick, true,
                            EpdFontFamily::ITALIC);

  // Left/Right re-pick the grade; Confirm flips to the definition. "Next" (commit +
  // advance) is the Down side button, clued top-right by drawSuspendHint above.
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PASS), tr(STR_FLASHCARD_FAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::drawSuspendHint(int contentTop, int contentBottom, bool showNextHint) {
  // Small, plain side-button clues placed at the physical position of the button
  // they label, so each clue sits next to the button that triggers it. Suspend is
  // the "prev" (PageBack) button, Next the "next" (PageForward) button; which
  // physical button is which folds in the Side Button Layout + CW swap via
  // usesUpButton(). The two devices arrange their side buttons differently:
  //   X3: side-by-side along the top edge  -> UP=top-left,  DOWN=top-right
  //   X4: stacked on the right edge        -> UP=top-right, DOWN=bottom-right
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int leftX = metrics.contentSidePadding;
  const bool x4 = gpio.deviceIsX4();
  // On X4 the DOWN-button clue sits at the bottom-right, aligned to the centered
  // "Missed / Got it" grade line (renderRevealed draws it at this same row). That
  // row is one above the chapter footer (contentBottom - listRowHeight), so the clue
  // never overlaps / cuts the chapter title.
  const int bottomY = contentBottom - metrics.listRowHeight * 2;
  auto place = [&](const char* text, bool isUpButton) {
    int x, ypos;
    if (x4) {
      x = renderer.getScreenWidth() - metrics.contentSidePadding - renderer.getTextWidth(SMALL_FONT_ID, text);
      ypos = isUpButton ? contentTop : bottomY;
    } else {
      ypos = contentTop;
      x = isUpButton ? leftX
                     : renderer.getScreenWidth() - metrics.contentSidePadding - renderer.getTextWidth(SMALL_FONT_ID, text);
    }
    renderer.drawText(SMALL_FONT_ID, x, ypos, text, true);
  };
  const char* hint = suspendedMode ? tr(STR_FLASHCARD_UNSUSPEND_HINT) : tr(STR_FLASHCARD_SUSPEND_HINT);
  place(hint, mappedInput.usesUpButton(MappedInputManager::Button::PageBack));
  if (showNextHint) {
    place(tr(STR_FLASHCARD_NEXT_HINT), mappedInput.usesUpButton(MappedInputManager::Button::PageForward));
  }
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
  drawSuspendHint(contentTop, contentBottom);

  // Left = pass, Right = fail; Confirm re-flips. Suspended review swaps grading
  // for Prev/Next browsing.
  const auto labels =
      suspendedMode
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PREV), tr(STR_FLASHCARD_NEXT))
          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_FLASHCARD_FLIP), tr(STR_FLASHCARD_PASS), tr(STR_FLASHCARD_FAIL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardReviewActivity::renderSummary(int contentTop, int contentBottom, int pageWidth) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  int y = contentTop + metrics.listRowHeight;
  char buf[64];
  renderer.drawCenteredText(NOTOSERIF_16_FONT_ID, y, tr(STR_FLASHCARD_SUMMARY_TITLE), true, EpdFontFamily::BOLD);
  y += metrics.listRowHeight * 2;

  if (suspendedMode) {
    // Suspended review has no grading tally -- only the restored count is meaningful.
    snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_UNSUSPENDED), suspended);
    renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
  } else {
    snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_REVIEWED), reviewed);
    renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
    y += metrics.listRowHeight;
    snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_CORRECT), correct);
    renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
    y += metrics.listRowHeight;
    snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_MASTERED), mastered);
    renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
    if (suspended > 0) {
      y += metrics.listRowHeight;
      snprintf(buf, sizeof(buf), "%s: %d", tr(STR_FLASHCARD_SUMMARY_SUSPENDED), suspended);
      renderer.drawCenteredText(UI_12_FONT_ID, y, buf);
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
