#include "BookOrbitDeliverySync.h"

#include <Logging.h>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownload.h"
#include "BookOrbitDownloadIndex.h"
#include "network/BookOrbitCatalogClient.h"

namespace BookOrbitDeliverySync {

namespace {
// Bounded work per run, not a checkpoint system: this only prevents an accidentally
// broad scope from holding Wi-Fi open through a huge download. The next sync simply
// continues where this one left off (already-present books are skipped).
constexpr int MAX_DELIVERIES_PER_RUN = 5;
}  // namespace

Result fetchMissingBooks(ProgressFn progress, const CancelCheck shouldCancel) {
  Result r;
  if (!BOOKORBIT_STORE.hasCredentials()) {
    LOG_INF("BookOrbit", "Delivery skipped: no BookOrbit credentials");
    return r;
  }
  const std::string& scopeId = BOOKORBIT_STORE.getDeliveryScopeId();
  if (scopeId.empty()) {
    LOG_INF("BookOrbit", "Delivery skipped: no delivery scope configured");
    return r;
  }

  BookOrbitBookQuery query;
  query.sort = "title";  // deterministic order so interrupted runs resume predictably
  query.smartScopeId = scopeId;

  int page = 1;
  while (true) {
    if (shouldCancel && shouldCancel()) {
      r.aborted = true;
      break;
    }

    // Page-at-a-time: fetch, process, discard. The working set stays bounded to one
    // page (PAGE_SIZE = 20); no global queue of missing books is ever built.
    BookOrbitBookPage bookPage;
    if (!BookOrbitCatalogClient::fetchBooks(query, page, bookPage)) {
      r.failed++;  // counted once; the next sync retries from this page
      break;
    }

    const int pageSize = bookPage.pageSize > 0 ? bookPage.pageSize : BookOrbitCatalogClient::PAGE_SIZE;
    for (const auto& book : bookPage.books) {
      if (shouldCancel && shouldCancel()) {
        r.aborted = true;
        break;
      }
      r.considered++;

      // The download index is a best-effort cache, not a source of truth: a stale
      // bookId→path entry (left behind when a book is renamed or replaced on the
      // server) can point at a *different* book's still-present file and hide a book
      // that was never actually delivered. Always run deliver(); its canonical-path
      // collision test (existence + size + valid-EPUB) decides "already local" and
      // self-repairs the index for the 128-entry FIFO eviction case.
      const BookOrbitDownload::Outcome outcome = BookOrbitDownload::deliver(book.id, progress, shouldCancel);
      switch (outcome) {
        case BookOrbitDownload::Outcome::DOWNLOADED:
          r.downloaded++;
          break;
        case BookOrbitDownload::Outcome::ALREADY_LOCAL:
          r.alreadyLocal++;
          break;
        case BookOrbitDownload::Outcome::CONFLICT:
          r.conflicts++;
          break;
        case BookOrbitDownload::Outcome::FAILED:
          r.failed++;
          break;
        case BookOrbitDownload::Outcome::ABORTED:
          r.aborted = true;
          break;
      }
      if (r.aborted || r.downloaded >= MAX_DELIVERIES_PER_RUN) break;
    }

    if (r.aborted || r.downloaded >= MAX_DELIVERIES_PER_RUN) break;

    // Next page when the server says more exist.
    if (bookPage.books.empty() || static_cast<long>(page) * pageSize >= bookPage.total) break;
    page++;
  }

  // The delivery run is done; free the in-RAM download index before the caller turns
  // Wi-Fi off and sleeps.
  BookOrbitDownloadIndex::unload();

  LOG_INF("BookOrbit",
          "Delivery done: considered=%d alreadyLocal=%d downloaded=%d failed=%d conflicts=%d aborted=%d", r.considered,
          r.alreadyLocal, r.downloaded, r.failed, r.conflicts, r.aborted ? 1 : 0);
  return r;
}

}  // namespace BookOrbitDeliverySync
