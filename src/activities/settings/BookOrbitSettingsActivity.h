#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Submenu for BookOrbit Sync settings.
 * Shows username, password, server URL, and an authenticate option.
 *
 * Unlike KOReaderSettingsActivity there is no document-matching toggle: BookOrbit
 * always identifies documents by the binary partial-MD5 hash.
 */
class BookOrbitSettingsActivity final : public Activity {
 public:
  explicit BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // FreeInkApp hosts the settings list (themed rows, touch routing); the
  // header stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<12, 4>;

  ButtonNavigator buttonNavigator;

  size_t selectedIndex = 0;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  void handleSelection();

  // Transient state for the foreground "Delivery scope" and "Download pending books
  // now" actions: a short status shown as the last row's value, plus download progress.
  std::string deliveryStatus;
  size_t deliveryProgress = 0;
  size_t deliveryTotal = 0;
  bool deliveryRunning = false;
  bool deliveryCancelRequested = false;
  // One foreground delivery runs at a time on the main task; the static pointer lets
  // the non-capturing CancelCheck (bool (*)()) poll this instance's input.
  static BookOrbitSettingsActivity* s_deliveringSelf;
  static bool deliveryCancelPoll();

  void openDeliveryScopePicker();
  void runDownloadPendingNow();
};
