#include "BookOrbitSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDeliverySync.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/browser/BookOrbitCatalogBrowserActivity.h"
#include "activities/settings/BookOrbitAuthActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/BookOrbitCatalogClient.h"
#include "network/SavedWifiConnect.h"

namespace fui = freeink::ui;

namespace {
constexpr int MENU_ITEMS = 11;
// The folder row reuses the OPDS strings: "Download Folder" and "SD Root" are
// generic and already translated in every language file.
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME,
                                     StrId::STR_PASSWORD,
                                     StrId::STR_BOOKORBIT_SERVER_URL,
                                     StrId::STR_AUTHENTICATE,
                                     StrId::STR_BOOKORBIT_CATALOG,
                                     StrId::STR_OPDS_DOWNLOAD_FOLDER,
                                     StrId::STR_SYNC_BEHAVIOR,
                                     StrId::STR_BOOKORBIT_SYNC_ON_SLEEP,
                                     StrId::STR_BOOKORBIT_AUTO_DELIVERY,
                                     StrId::STR_BOOKORBIT_DELIVERY_SCOPE,
                                     StrId::STR_BOOKORBIT_DOWNLOAD_PENDING};
constexpr fui::ActionId ACTION_ROW = 1;

// Mirrors BookOrbitSyncActivity's wifiOff(): the device returns to its radio-off idle
// state after a foreground network action.
void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// Mirrors OpdsServerListActivity's normalizeDownloadFolder: "" for the SD root,
// otherwise a leading and no trailing slash, so folder + "/file.epub" is a path.
std::string normalizeDownloadFolder(std::string folder) {
  while (!folder.empty() && (folder.front() == ' ' || folder.front() == '\t')) folder.erase(folder.begin());
  while (!folder.empty() && (folder.back() == ' ' || folder.back() == '\t')) folder.pop_back();
  if (folder.empty() || folder == "/") return "";
  if (folder.front() != '/') folder.insert(folder.begin(), '/');
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  return folder;
}
}  // namespace

BookOrbitSettingsActivity::BookOrbitSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("BookOrbitSettings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

BookOrbitSettingsActivity* BookOrbitSettingsActivity::s_deliveringSelf = nullptr;

// Non-capturing stop-check for the foreground delivery actions: polls this instance's
// input so Back and the Home gesture cancel the Wi-Fi scan and the downloads.
bool BookOrbitSettingsActivity::deliveryCancelPoll() {
  BookOrbitSettingsActivity* self = s_deliveringSelf;
  if (self == nullptr) return true;  // no action in flight: nothing to cancel
  if (self->deliveryCancelRequested) return true;
  self->mappedInput.update();
  if (self->mappedInput.wasHomeGesture() || self->mappedInput.isPressed(MappedInputManager::Button::Back) ||
      self->mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      self->mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    self->deliveryCancelRequested = true;
  }
  return self->deliveryCancelRequested;
}

void BookOrbitSettingsActivity::openDeliveryScopePicker() {
  if (!BOOKORBIT_STORE.hasCredentials()) return;

  deliveryStatus = tr(STR_LOADING);
  deliveryCancelRequested = false;
  requestUpdate(true);

  s_deliveringSelf = this;
  const bool connected = SavedWifiConnect::connect(&BookOrbitSettingsActivity::deliveryCancelPoll);
  if (!connected) {
    s_deliveringSelf = nullptr;
    deliveryStatus = tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return;
  }
  sdFontSystem.releaseForNetwork(renderer);

  // The smart-scopes facet is short; collect it across pages (bounded) so the picker
  // can show every scope the server has, not just the first page.
  std::vector<BookOrbitFacetEntry> scopes;
  int page = 1;
  while (page <= 10) {
    if (deliveryCancelPoll()) break;
    BookOrbitFacetPage facetPage;
    if (!BookOrbitCatalogClient::fetchSectionEntries("smart-scopes", page, facetPage)) break;
    for (auto& entry : facetPage.entries) scopes.push_back(std::move(entry));
    if (!facetPage.hasNext) break;
    page++;
  }
  s_deliveringSelf = nullptr;
  wifiOff();

  if (scopes.empty()) {
    deliveryStatus = tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return;
  }

  std::vector<std::string> options;
  options.reserve(scopes.size());
  for (const auto& scope : scopes) options.push_back(scope.title);

  const std::string currentId = BOOKORBIT_STORE.getDeliveryScopeId();
  uint8_t selected = 0;
  for (size_t i = 0; i < scopes.size(); i++) {
    if (scopes[i].id == currentId) {
      selected = static_cast<uint8_t>(i);
      break;
    }
  }

  deliveryStatus.clear();
  requestUpdate();

  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "BookOrbitDeliveryScope",
                                                StrId::STR_BOOKORBIT_DELIVERY_SCOPE, std::move(options), selected),
      [this, scopes](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
          if (selection != nullptr && selection->index < scopes.size()) {
            const auto& scope = scopes[selection->index];
            BOOKORBIT_STORE.setDeliveryScopeId(scope.id);
            BOOKORBIT_STORE.setDeliveryScopeName(scope.title);
            BOOKORBIT_STORE.saveToFile();
          }
        }
        requestUpdate();
      });
}

void BookOrbitSettingsActivity::runDownloadPendingNow() {
  if (!BOOKORBIT_STORE.hasCredentials()) return;
  if (BOOKORBIT_STORE.getDeliveryScopeId().empty()) {
    deliveryStatus = tr(STR_BOOKORBIT_NO_DELIVERY_SCOPE);
    requestUpdate();
    return;
  }

  deliveryStatus = tr(STR_BOOKORBIT_DELIVERY_RUNNING);
  deliveryProgress = deliveryTotal = 0;
  deliveryRunning = true;
  deliveryCancelRequested = false;
  requestUpdate(true);

  s_deliveringSelf = this;
  const bool connected = SavedWifiConnect::connect(&BookOrbitSettingsActivity::deliveryCancelPoll);
  if (!connected) {
    s_deliveringSelf = nullptr;
    deliveryRunning = false;
    deliveryStatus = tr(STR_BOOKORBIT_CATALOG_ERROR);
    requestUpdate();
    return;
  }
  sdFontSystem.releaseForNetwork(renderer);

  int lastPercent = -1;
  unsigned long lastUpdateMs = 0;
  auto progress = [this, &lastPercent, &lastUpdateMs](const size_t downloaded, const size_t total) {
    deliveryProgress = downloaded;
    deliveryTotal = total;
    const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
    const unsigned long now = millis();
    if (percent >= 100 || lastPercent < 0 || percent >= lastPercent + 5 || now - lastUpdateMs > 1000) {
      lastPercent = percent;
      lastUpdateMs = now;
      requestUpdate(true);
    }
  };

  const BookOrbitDeliverySync::Result r =
      BookOrbitDeliverySync::fetchMissingBooks(progress, &BookOrbitSettingsActivity::deliveryCancelPoll);
  s_deliveringSelf = nullptr;
  deliveryRunning = false;
  wifiOff();

  char summary[96];
  snprintf(summary, sizeof(summary), tr(STR_BOOKORBIT_DELIVERY_SUMMARY), r.downloaded, r.alreadyLocal, r.conflicts,
           r.failed);
  deliveryStatus = summary;
  requestUpdate();
}

void BookOrbitSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<BookOrbitSettingsActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEMS) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  // Activation opens a keyboard/sub-activity or repaints a new value; a
  // lingering flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
}

void BookOrbitSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &BookOrbitSettingsActivity::onRowEvent, this);
  app.setScreen(&BookOrbitSettingsActivity::listScreen, this);
  requestUpdate();
}

void BookOrbitSettingsActivity::onExit() { Activity::onExit(); }

void BookOrbitSettingsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    topIndex = followListSelection(static_cast<int>(selectedIndex), topIndex, visibleRows, MENU_ITEMS);
    requestUpdate();
  });
}

void BookOrbitSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_USERNAME),
                                                                   BOOKORBIT_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               BOOKORBIT_STORE.setCredentials(kb.text, BOOKORBIT_STORE.getPassword());
                               BOOKORBIT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // Password
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_PASSWORD),
                                                                   BOOKORBIT_STORE.getPassword(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               BOOKORBIT_STORE.setCredentials(BOOKORBIT_STORE.getUsername(), kb.text);
                               BOOKORBIT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    // Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = BOOKORBIT_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOKORBIT_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               BOOKORBIT_STORE.setServerUrl(urlToSave);
                               BOOKORBIT_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 3) {
    // Authenticate
    if (!BOOKORBIT_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    // Replaces this screen rather than stacking on top of it, so the TLS handshake gets
    // the memory the settings stack was holding (see ActivityManager::goToBookOrbitAuth).
    activityManager.goToBookOrbitAuth();
  } else if (selectedIndex == 4) {
    // Browse Catalog
    if (!BOOKORBIT_STORE.hasCredentials()) {
      return;
    }
    activityManager.goToBookOrbitCatalog();
  } else if (selectedIndex == 5) {
    // Catalog download folder ("" = SD root); created on first download.
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER),
                                                BOOKORBIT_STORE.getDownloadFolder(), 64, InputType::Text),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            BOOKORBIT_STORE.setDownloadFolder(normalizeDownloadFolder(kb.text));
            BOOKORBIT_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 6) {
    const auto current = BOOKORBIT_STORE.getSyncBehavior();
    BOOKORBIT_STORE.setSyncBehavior(current == BookOrbitSyncBehavior::ASK_EVERY_TIME
                                        ? BookOrbitSyncBehavior::SMART
                                        : BookOrbitSyncBehavior::ASK_EVERY_TIME);
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 7) {
    BOOKORBIT_STORE.setSyncOnSleep(!BOOKORBIT_STORE.getSyncOnSleep());
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 8) {
    // "Automatically download new books": Off / On sleep.
    BOOKORBIT_STORE.setAutoDeliveryOnSleep(!BOOKORBIT_STORE.getAutoDeliveryOnSleep());
    BOOKORBIT_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 9) {
    // Delivery scope: a SmartScope picker, not a text field.
    openDeliveryScopePicker();
  } else if (selectedIndex == 10) {
    // Manual "Download pending books now", with visible progress.
    runDownloadPendingNow();
  }
}

void BookOrbitSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<BookOrbitSettingsActivity*>(user)->buildListScreen(screen);
}

void BookOrbitSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // Per-render owned value strings; items point into them for the draw only.
  std::vector<std::string> values(MENU_ITEMS);
  const bool hasCredentials = BOOKORBIT_STORE.hasCredentials();
  for (int i = 0; i < MENU_ITEMS; i++) {
    if (i == 0) {
      const auto username = BOOKORBIT_STORE.getUsername();
      values[i] = username.empty() ? tr(STR_NOT_SET) : username;
    } else if (i == 1) {
      values[i] = BOOKORBIT_STORE.getPassword().empty() ? tr(STR_NOT_SET) : "******";
    } else if (i == 2) {
      const auto serverUrl = BOOKORBIT_STORE.getServerUrl();
      values[i] = serverUrl.empty() ? tr(STR_NOT_SET) : serverUrl;
    } else if (i == 5) {
      const std::string& folder = BOOKORBIT_STORE.getDownloadFolder();
      values[i] = folder.empty() ? tr(STR_OPDS_SD_ROOT) : folder;
    } else if (i == 6) {
      values[i] = BOOKORBIT_STORE.getSyncBehavior() == BookOrbitSyncBehavior::SMART ? tr(STR_SMART_SYNC)
                                                                                    : tr(STR_ASK_EVERY_TIME);
    } else if (i == 7) {
      values[i] = BOOKORBIT_STORE.getSyncOnSleep() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (i == 8) {
      values[i] = BOOKORBIT_STORE.getAutoDeliveryOnSleep() ? tr(STR_BOOKORBIT_ON_SLEEP) : tr(STR_STATE_OFF);
    } else if (i == 9) {
      const std::string& scope = BOOKORBIT_STORE.getDeliveryScopeName();
      values[i] = scope.empty() ? tr(STR_NOT_SET) : scope;
    } else if (i == 10) {
      if (deliveryRunning && deliveryTotal > 0) {
        const int percent = static_cast<int>(static_cast<uint64_t>(deliveryProgress) * 100 / deliveryTotal);
        char buf[40];
        snprintf(buf, sizeof(buf), "%s %d%%", tr(STR_BOOKORBIT_DELIVERY_RUNNING), percent);
        values[i] = buf;
      } else if (!deliveryStatus.empty()) {
        values[i] = deliveryStatus;
      } else if (!hasCredentials) {
        values[i] = std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
      } else if (BOOKORBIT_STORE.getDeliveryScopeId().empty()) {
        values[i] = std::string("[") + tr(STR_BOOKORBIT_NO_DELIVERY_SCOPE) + "]";
      } else {
        values[i] = "";
      }
    } else {
      // Authenticate and Browse Catalog both need credentials to do anything.
      values[i] = hasCredentials ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }
  }

  std::vector<fui::ListItem> items;
  items.reserve(MENU_ITEMS);
  for (int i = 0; i < MENU_ITEMS; i++) {
    fui::ListItem item;
    item.label = I18N.get(menuNames[i]);
    if (!values[i].empty()) item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, MENU_ITEMS);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void BookOrbitSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_BOOKORBIT_SYNC), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_SYNC));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
