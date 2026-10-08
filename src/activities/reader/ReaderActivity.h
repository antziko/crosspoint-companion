#pragma once
#include <memory>

#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  bool allowFastInitialRefresh;
  // Why the last loadEpub() refused a protected book; empty for any other failure.
  std::string loadProtectionError;
  // A loaded book held while its loan-due reminder is on screen.
  std::unique_ptr<Epub> pendingEpub;
  // Non-static (unlike the other loaders): draws the first-open indexing popup, which needs the renderer.
  std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isImageFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();
  // A protected book refused to open (loan expired / date unverified): explain it, and for
  // an unverified date offer a Wi-Fi time sync that reopens the book.
  void showProtectionError();
  void beginLoanTimeSync();
  // Loan ends within a few days: say when, then open the book.
  void showLoanDueReminder(std::unique_ptr<Epub> epub);
  int initialRefreshCountdown() const;

 public:
  // Defined out of line: pendingEpub needs the complete Epub type.
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          bool allowFastInitialRefresh);
  ~ReaderActivity() override;
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
