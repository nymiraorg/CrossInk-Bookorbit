#include <gtest/gtest.h>

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "BookOrbitCredentialStore.h"
#include "BookOrbitDownloadIndex.h"

// Covers the download index's documented behavior that Automatic Local Book Delivery
// relies on: record -> lookup, stale-file drop, forEachExisting, FIFO eviction at
// MAX_ENTRIES (128), and discarding the index when the server URL changes (CRC scope).
// The 128-entry eviction is why delivery falls back to the canonical-path test.

namespace {

struct Visit {
  std::vector<std::pair<int64_t, std::string>> seen;
};

void visitEntry(int64_t bookId, const std::string& path, void* context) {
  static_cast<Visit*>(context)->seen.emplace_back(bookId, path);
}

}  // namespace

TEST(BookOrbitDownloadIndex, RecordThenLookupReloadsFromDisk) {
  fake::reset();
  BookOrbitDownloadIndex::unload();
  fake::add("/book1.epub", "some epub bytes");

  BookOrbitDownloadIndex::record(1, "/book1.epub");
  BookOrbitDownloadIndex::unload();  // free RAM; force a reload from the stub filesystem

  std::string path;
  EXPECT_TRUE(BookOrbitDownloadIndex::lookup(1, path));
  EXPECT_EQ(path, "/book1.epub");
}

TEST(BookOrbitDownloadIndex, LookupDropsStaleFileEntry) {
  fake::reset();
  BookOrbitDownloadIndex::unload();
  fake::add("/book1.epub", "original");
  BookOrbitDownloadIndex::record(1, "/book1.epub");
  BookOrbitDownloadIndex::unload();

  // Replace the file with different-sized content: the recorded size no longer matches.
  fake::files["/book1.epub"]->bytes.assign(40, 'x');

  std::string path;
  EXPECT_FALSE(BookOrbitDownloadIndex::lookup(1, path));  // stale entry dropped
}

TEST(BookOrbitDownloadIndex, ForEachExistingVisitsIntactEntries) {
  fake::reset();
  BookOrbitDownloadIndex::unload();
  fake::add("/a.epub", "aaa");
  fake::add("/b.epub", "bbb");
  BookOrbitDownloadIndex::record(1, "/a.epub");
  BookOrbitDownloadIndex::record(2, "/b.epub");

  Visit visit;
  BookOrbitDownloadIndex::forEachExisting(visitEntry, &visit);
  EXPECT_EQ(visit.seen.size(), 2u);
}

TEST(BookOrbitDownloadIndex, FifoEvictionAtMaxEntries) {
  fake::reset();
  BookOrbitDownloadIndex::unload();
  constexpr int TOTAL = 130;  // > MAX_ENTRIES (128)

  for (int i = 1; i <= TOTAL; i++) {
    const std::string path = "/book" + std::to_string(i) + ".epub";
    fake::add(path, "content");
    BookOrbitDownloadIndex::record(i, path);
  }
  BookOrbitDownloadIndex::unload();

  // The oldest two (books 1 and 2) were evicted; 3..130 remain (128 entries).
  std::string path;
  EXPECT_FALSE(BookOrbitDownloadIndex::lookup(1, path));
  EXPECT_FALSE(BookOrbitDownloadIndex::lookup(2, path));
  EXPECT_TRUE(BookOrbitDownloadIndex::lookup(3, path));
  EXPECT_TRUE(BookOrbitDownloadIndex::lookup(130, path));
}

TEST(BookOrbitDownloadIndex, ServerUrlChangeDiscardsIndex) {
  fake::reset();
  BookOrbitDownloadIndex::unload();
  BOOKORBIT_STORE.setServerUrl("https://server-a.example.com");
  fake::add("/book1.epub", "content");
  BookOrbitDownloadIndex::record(1, "/book1.epub");
  BookOrbitDownloadIndex::unload();

  // The index is CRC-scoped to server-a; a different server URL must discard it.
  BOOKORBIT_STORE.setServerUrl("https://server-b.example.com");

  std::string path;
  EXPECT_FALSE(BookOrbitDownloadIndex::lookup(1, path));
}
