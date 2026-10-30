// Thin wrappers over the Linux file API used for all page I/O.
#ifndef KVSTORE_FILE_H_
#define KVSTORE_FILE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "kvstore/types.h"

namespace kvstore {

// A heap buffer whose address is a multiple of kPageSize.  O_DIRECT requires the
// user buffer, the file offset and the transfer length to all be aligned to the
// device block size; aligning everything to a full page satisfies that on every
// filesystem we care about.
class AlignedBuffer {
 public:
  AlignedBuffer() = default;
  explicit AlignedBuffer(std::size_t num_pages);
  ~AlignedBuffer();

  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&& other) noexcept;
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;

  std::uint8_t* data() { return data_; }
  const std::uint8_t* data() const { return data_; }
  std::size_t size_bytes() const { return num_pages_ * kPageSize; }
  std::uint8_t* page(std::size_t index) { return data_ + index * kPageSize; }

 private:
  std::uint8_t* data_ = nullptr;
  std::size_t num_pages_ = 0;
};

// A page-granular file.  Reads always use pread() so that a single page can be
// fetched without touching the rest of the file and without any shared file
// offset, which is exactly what the buffer pool needs.
class File {
 public:
  File() = default;
  ~File();

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept;
  File& operator=(File&& other) noexcept;

  // Opens an existing file for reading.  If `direct` is true O_DIRECT is
  // requested, falling back to buffered I/O when the filesystem rejects it.
  static File OpenForRead(const std::string& path, bool direct);

  // Creates (or truncates) a file for writing, with the same O_DIRECT handling.
  static File Create(const std::string& path, bool direct);

  bool is_open() const { return fd_ >= 0; }
  bool is_direct() const { return direct_; }
  const std::string& path() const { return path_; }

  // Reads exactly one page.  `out` must be page-aligned and at least kPageSize
  // bytes long.  Throws if the page lies past the end of the file.
  void ReadPage(std::uint64_t page_index, void* out);

  // Writes exactly one page.  `in` must be page-aligned.
  void WritePage(std::uint64_t page_index, const void* in);

  std::uint64_t NumPages();
  void Sync();
  void Close();

 private:
  int fd_ = -1;
  bool direct_ = false;
  std::string path_;
};

// Deletes a file, throwing on any error other than "it was not there".
void RemoveFile(const std::string& path);

}  // namespace kvstore

#endif  // KVSTORE_FILE_H_
