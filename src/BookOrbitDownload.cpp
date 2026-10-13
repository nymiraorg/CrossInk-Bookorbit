#include "BookOrbitDownload.h"

#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>

#include "BookOrbitCollision.h"
#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"
#include "util/BookCacheUtils.h"
#include "util/BookOrbitDevicePath.h"
#include "util/StringUtils.h"

namespace BookOrbitDownload {

HttpDownloader::DownloadError lastError = HttpDownloader::HTTP_ERROR;
FailureKind lastFailureKind = FailureKind::DOWNLOAD;

namespace {

constexpr size_t DOWNLOAD_BUFFER_SIZE = 2048;
constexpr char STAGED_DOWNLOAD_SUFFIX[] = ".part";
constexpr int MAX_DOWNLOAD_ATTEMPTS = 3;

// The EPUB-container check HttpDownloader runs before it publishes a staged file,
// reused for the collision "confidently same book" test.
bool isValidEpub(const std::string& path) {
  ZipFile zip(path);
  size_t size = 0;
  return zip.getInflatedFileSize("META-INF/container.xml", &size) && size > 0;
}

// Size of the file at path, or 0 when it cannot be opened.
uint32_t fileSizeOf(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("BookOrbit", path, f)) return 0;
  const uint32_t size = static_cast<uint32_t>(f.fileSize());
  f.close();
  return size;
}

// The transfer core shared by deliver() and deliverOver(). `allowOverwrite` controls
// the collision rule at an already-present canonical path: delivery refuses to
// overwrite, the browser's explicit Re-download does not (still staged as ".part").
Outcome transferToPath(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& epubFile,
                       const std::string& finalPath, const ProgressFn& progress, const CancelCheck shouldCancel,
                       const bool allowOverwrite) {
  // Collision: an already-present canonical path is never silently overwritten.
  if (!allowOverwrite) {
    const bool exists = Storage.exists(finalPath.c_str());
    if (exists) {
      const Collision collision =
          classifyCollision(true, isValidEpub(finalPath), static_cast<size_t>(fileSizeOf(finalPath)),
                            epubFile.sizeBytes);
      if (collision == Collision::SAME) {
        // Confidently the same book (size match + valid EPUB container): repair the
        // index — this closes the 128-entry FIFO eviction hole — and skip.
        BookOrbitDownloadIndex::record(detail.id, finalPath);
        return Outcome::ALREADY_LOCAL;
      }
      LOG_ERR("BookOrbit", "Delivery conflict: %s already exists and does not match book %lld; not overwriting",
              finalPath.c_str(), static_cast<long long>(detail.id));
      return Outcome::CONFLICT;
    }
  }

  // Create the download folder and any nested folders the server's naming template
  // asks for. mkdir's pFlag creates the missing parents in one call.
  const size_t lastSlash = finalPath.find_last_of('/');
  if (lastSlash > 0 && lastSlash != std::string::npos) {
    const std::string folder = finalPath.substr(0, lastSlash);
    if (!Storage.exists(folder.c_str()) && !Storage.mkdir(folder.c_str())) {
      LOG_ERR("BookOrbit", "Could not create download folder %s", folder.c_str());
      return Outcome::FAILED;
    }
  }

  HttpDownloader::DownloadOptions options;
  options.shouldCancel = shouldCancel;  // bool (*)() -> std::function<bool()>
  options.bufferSize = DOWNLOAD_BUFFER_SIZE;
  // TLS runs with a few KB of headroom on the C3 and reads can drop mid-transfer;
  // keep the partial file and resume instead of restarting a multi-MB download.
  options.preservePartial = true;
  // Small client RX buffer so the headers-time body cache can't demand a large
  // realloc next to the TLS buffers (see BookOrbitCatalogClient::fetchJson).
  options.clientRxBufferSize = 2048;
  // Stage as "<book>.part", refuse a book the card cannot hold before writing any of
  // it, and only put the file in place once it opens as an EPUB.
  options.stageAsPart = true;
  options.checkFreeSpace = true;
  options.validate = isValidEpub;

  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < MAX_DOWNLOAD_ATTEMPTS; attempt++) {
    // Honor a cancel between attempts too: a failing download otherwise runs all its
    // retries (TLS handshake included) with the cancel request ignored.
    if (shouldCancel && shouldCancel()) {
      result = HttpDownloader::ABORTED;
      break;
    }
    options.resumePartial = attempt > 0;
    result = BookOrbitCatalogClient::downloadFile(epubFile.id, finalPath, progress, nullptr, options);
    // A full card will not have more room on the next attempt.
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED ||
        result == HttpDownloader::INSUFFICIENT_SPACE) {
      break;
    }
    LOG_ERR("BookOrbit", "Download attempt %d/%d failed (err=%d), retrying", attempt + 1, MAX_DOWNLOAD_ATTEMPTS,
            static_cast<int>(result));
  }

  const bool downloadFailed = result != HttpDownloader::OK && result != HttpDownloader::ABORTED;
  if (downloadFailed) {
    // preservePartial kept the staged file for resuming between attempts; don't leave
    // it behind once we give up.
    Storage.remove((finalPath + STAGED_DOWNLOAD_SUFFIX).c_str());
  }

  if (result == HttpDownloader::OK) {
    clearBookCache(finalPath);
    BookOrbitDownloadIndex::record(detail.id, finalPath);
    // A new book, or new contents under a known path: the Library's index is stale.
    library::invalidateLibraryIndex();
    return Outcome::DOWNLOADED;
  }
  if (result == HttpDownloader::ABORTED) {
    return Outcome::ABORTED;
  }
  lastError = result;
  lastFailureKind = FailureKind::DOWNLOAD;
  return Outcome::FAILED;
}

}  // namespace

std::string catalogBookFilename(const std::string& title, const std::string& author) {
  const std::string suffix = author.empty() ? "" : (" - " + author);
  return "/" + StringUtils::sanitizeFilename(title + suffix) + ".epub";
}

std::string catalogBookFilename(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file) {
  const std::string fromServer = BookOrbitDevicePath::sanitizeRelativePath(file.devicePath);
  if (!fromServer.empty()) return "/" + fromServer;
  return catalogBookFilename(detail.title, detail.author);
}

std::string catalogBookPath(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file) {
  return BOOKORBIT_STORE.getDownloadFolder() + catalogBookFilename(detail, file);
}

bool hasEpubFile(const BookOrbitBookDetail& detail, BookOrbitCatalogFile& outFile) {
  for (const auto& file : detail.files) {
    std::string format = file.format;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return std::tolower(c); });
    if (format == "epub") {
      outFile = file;
      return true;
    }
  }
  return false;
}

Outcome deliver(const int64_t bookId, ProgressFn progress, const CancelCheck shouldCancel) {
  lastError = HttpDownloader::HTTP_ERROR;
  lastFailureKind = FailureKind::DOWNLOAD;

  BookOrbitBookDetail detail;
  if (!BookOrbitCatalogClient::fetchBookDetail(bookId, detail)) {
    lastFailureKind = FailureKind::FETCH;
    return Outcome::FAILED;
  }

  BookOrbitCatalogFile epubFile;
  if (!hasEpubFile(detail, epubFile)) {
    LOG_ERR("BookOrbit", "No EPUB format for book %lld; skipping", static_cast<long long>(bookId));
    lastFailureKind = FailureKind::NO_EPUB;
    return Outcome::FAILED;
  }

  return transferToPath(detail, epubFile, catalogBookPath(detail, epubFile), progress, shouldCancel,
                        /*allowOverwrite=*/false);
}

Outcome deliverOver(const int64_t bookId, const std::string& destPath, ProgressFn progress,
                    const CancelCheck shouldCancel) {
  lastError = HttpDownloader::HTTP_ERROR;
  lastFailureKind = FailureKind::DOWNLOAD;

  BookOrbitBookDetail detail;
  if (!BookOrbitCatalogClient::fetchBookDetail(bookId, detail)) {
    lastFailureKind = FailureKind::FETCH;
    return Outcome::FAILED;
  }

  BookOrbitCatalogFile epubFile;
  if (!hasEpubFile(detail, epubFile)) {
    LOG_ERR("BookOrbit", "No EPUB format for book %lld; skipping", static_cast<long long>(bookId));
    lastFailureKind = FailureKind::NO_EPUB;
    return Outcome::FAILED;
  }

  return transferToPath(detail, epubFile, destPath, progress, shouldCancel, /*allowOverwrite=*/true);
}

}  // namespace BookOrbitDownload
