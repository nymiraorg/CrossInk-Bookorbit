#pragma once

#include <cstdint>
#include <string>

#include "network/BookOrbitCatalogClient.h"
#include "network/HttpDownloader.h"

/**
 * The shared, activity-independent transfer primitive for fetching a BookOrbit
 * catalog book's EPUB onto the SD card.
 *
 * Extracted from BookOrbitCatalogBrowserActivity::downloadBook so the sleep-sync
 * delivery unit (BookOrbitDeliverySync) and the catalog browser share one
 * implementation. It owns the whole transfer sequence — fetch detail, choose the
 * EPUB, resolve the canonical path, collision check, mkdir, download/stage/validate,
 * publish, record in the download index, invalidate the library index — and does
 * none of the UI, renderer, or input-polling work the browser wraps around it.
 */
namespace BookOrbitDownload {

// Stop-check signature. A plain function pointer: the sleep sync's
// sleepSyncWakeRequested satisfies it directly. std::function is used only where a
// caller genuinely needs captures (the catalog browser's input poll).
using CancelCheck = bool (*)();
using ProgressFn = HttpDownloader::ProgressCallback;

enum class Outcome { DOWNLOADED, ALREADY_LOCAL, CONFLICT, FAILED, ABORTED };

// Detailed transport error behind the last FAILED outcome, mirroring
// BookOrbitCatalogClient::lastFetchBadResponse. INSUFFICIENT_SPACE lets the browser
// show the SD-full message instead of the generic download-failed one.
extern HttpDownloader::DownloadError lastError;

// Why the last deliver()/deliverOver() FAILED, so the browser can show the message
// that matches the stage that failed (catalog fetch vs. no EPUB vs. transfer).
enum class FailureKind { FETCH, NO_EPUB, DOWNLOAD };
extern FailureKind lastFailureKind;

// The filename (leading slash, no folder) a catalog file lands under, preferring the
// server's sanitized devicePath so a book lands where the account's KOReader naming
// template puts it (series folders and all).
std::string catalogBookFilename(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file);

// The filename this firmware falls back to when the server sends no devicePath.
std::string catalogBookFilename(const std::string& title, const std::string& author);

// The full SD path a catalog file downloads to: download folder + filename.
std::string catalogBookPath(const BookOrbitBookDetail& detail, const BookOrbitCatalogFile& file);

// True when `detail` carries an EPUB file (filled into outFile).
bool hasEpubFile(const BookOrbitBookDetail& detail, BookOrbitCatalogFile& outFile);

/**
 * Make the canonical local EPUB for `bookId` exist, record it, and invalidate the
 * library index. One book, no UI.
 *
 * Collision behavior (replaces the browser's old re-download semantics):
 *   canonical path absent                       -> download normally
 *   canonical path exists + confidently same    -> record + skip (ALREADY_LOCAL)
 *     (size match + valid EPUB container)
 *   canonical path exists + uncertain/different -> never overwrite (CONFLICT)
 */
Outcome deliver(int64_t bookId, ProgressFn progress, CancelCheck shouldCancel);

/**
 * The browser's explicit Re-download: the same transfer core, but `destPath` is
 * authoritative and an existing file there is replaced. Still staged as ".part" and
 * only swapped in on success, so a failed redownload keeps the copy the user had.
 */
Outcome deliverOver(int64_t bookId, const std::string& destPath, ProgressFn progress, CancelCheck shouldCancel);

}  // namespace BookOrbitDownload
