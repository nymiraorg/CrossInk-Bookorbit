#pragma once

#include "network/HttpDownloader.h"

/**
 * The delivery unit: enumerates the configured BookOrbit delivery SmartScope
 * page-at-a-time and makes every missing book's EPUB exist locally via
 * BookOrbitDownload, bounded to a small number of downloads per run. It carries no
 * UI and no reading-progress state; it only reports a compact result, so a delivery
 * failure never affects ordinary progress/highlight/bookmark sync.
 */
namespace BookOrbitDeliverySync {

// Stop-check signature: the sleep sync's sleepSyncWakeRequested satisfies it directly.
using CancelCheck = bool (*)();
using ProgressFn = HttpDownloader::ProgressCallback;

struct Result {
  int considered = 0;
  int alreadyLocal = 0;
  int downloaded = 0;
  int failed = 0;
  int conflicts = 0;
  bool aborted = false;
};

Result fetchMissingBooks(ProgressFn progress, CancelCheck shouldCancel);

}  // namespace BookOrbitDeliverySync
