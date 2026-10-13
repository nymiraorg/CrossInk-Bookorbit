#include <gtest/gtest.h>

#include "BookOrbitCollision.h"

// Covers the collision matrix from the delivery brief: an already-present
// canonical path is skipped only when it is confidently the same book, never
// overwritten otherwise. classifyCollision is the pure decision behind the
// Storage/ZipFile I/O in BookOrbitDownload::transferToPath.

using BookOrbitDownload::Collision;
using BookOrbitDownload::classifyCollision;

TEST(BookOrbitDownloadCollision, AbsentPathDownloads) {
  EXPECT_EQ(classifyCollision(false, false, 0, 0), Collision::NONE);
}

TEST(BookOrbitDownloadCollision, PresentSameBookSkips) {
  // Valid EPUB + matching size -> confidently the same book -> repair + skip.
  EXPECT_EQ(classifyCollision(true, true, 100, 100), Collision::SAME);
}

TEST(BookOrbitDownloadCollision, PresentInvalidEpubConflicts) {
  // The file is there but does not open as an EPUB -> not confidently ours.
  EXPECT_EQ(classifyCollision(true, false, 100, 100), Collision::DIFFERENT);
}

TEST(BookOrbitDownloadCollision, PresentSizeMismatchConflicts) {
  // Valid EPUB but a different size -> identity uncertain -> never overwrite.
  EXPECT_EQ(classifyCollision(true, true, 99, 100), Collision::DIFFERENT);
}

TEST(BookOrbitDownloadCollision, NoServerSizeConflicts) {
  // Server sent no sizeBytes, so identity cannot be confirmed -> conservative.
  EXPECT_EQ(classifyCollision(true, true, 100, 0), Collision::DIFFERENT);
}

TEST(BookOrbitDownloadCollision, EmptyLocalFileConflicts) {
  // A zero-byte local file cannot match a non-empty expected book.
  EXPECT_EQ(classifyCollision(true, true, 0, 100), Collision::DIFFERENT);
}
