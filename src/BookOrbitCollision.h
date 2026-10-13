#pragma once

#include <cstddef>

namespace BookOrbitDownload {

// Classification of a canonical path already present on the SD card. This is the
// collision matrix from the delivery brief, expressed as a pure decision so the
// rule can be unit-tested on the host without the Storage/ZipFile I/O around it.
enum class Collision { NONE, SAME, DIFFERENT };

//   file absent                             -> NONE        (download normally)
//   file present + valid EPUB + size match  -> SAME        (confidently the same book)
//   file present + anything else            -> DIFFERENT   (never overwrite)
inline Collision classifyCollision(bool fileExists, bool validEpub, size_t fileSize, size_t expectedSize) {
  if (!fileExists) return Collision::NONE;
  if (validEpub && expectedSize > 0 && fileSize == expectedSize) return Collision::SAME;
  return Collision::DIFFERENT;
}

}  // namespace BookOrbitDownload
