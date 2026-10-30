#include "kvstore/file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace kvstore {
namespace {

std::string Errno(const std::string& what, int err) {
  return what + ": " + std::strerror(err);
}

}  // namespace

AlignedBuffer::AlignedBuffer(std::size_t num_pages) : num_pages_(num_pages) {
  if (num_pages == 0) return;
  void* raw = nullptr;
  int rc = ::posix_memalign(&raw, kPageSize, num_pages * kPageSize);
  if (rc != 0) throw Error(Errno("posix_memalign", rc));
  data_ = static_cast<std::uint8_t*>(raw);
  std::memset(data_, 0, num_pages * kPageSize);
}

AlignedBuffer::~AlignedBuffer() { std::free(data_); }

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept
    : data_(other.data_), num_pages_(other.num_pages_) {
  other.data_ = nullptr;
  other.num_pages_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
  if (this != &other) {
    std::free(data_);
    data_ = other.data_;
    num_pages_ = other.num_pages_;
    other.data_ = nullptr;
    other.num_pages_ = 0;
  }
  return *this;
}

File::~File() {
  if (fd_ >= 0) ::close(fd_);
}

File::File(File&& other) noexcept
    : fd_(other.fd_), direct_(other.direct_), path_(std::move(other.path_)) {
  other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = other.fd_;
    direct_ = other.direct_;
    path_ = std::move(other.path_);
    other.fd_ = -1;
  }
  return *this;
}

namespace {

// Opens `path` with `flags`, adding O_DIRECT when requested.  Some filesystems
// (tmpfs, some overlay and network mounts) reject O_DIRECT with EINVAL; rather
// than failing outright we retry without it and report the effective mode, so
// that the test suite runs everywhere while real measurement runs on a
// filesystem that supports it still bypass the page cache.
int OpenWithOptionalDirect(const std::string& path, int flags, bool direct,
                           bool* direct_out) {
#ifdef O_DIRECT
  if (direct) {
    int fd = ::open(path.c_str(), flags | O_DIRECT, 0644);
    if (fd >= 0) {
      *direct_out = true;
      return fd;
    }
    if (errno != EINVAL) throw Error(Errno("open " + path, errno));
  }
#endif
  int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) throw Error(Errno("open " + path, errno));
  *direct_out = false;
  return fd;
}

}  // namespace

File File::OpenForRead(const std::string& path, bool direct) {
  File f;
  f.fd_ = OpenWithOptionalDirect(path, O_RDONLY, direct, &f.direct_);
  f.path_ = path;
  return f;
}

File File::Create(const std::string& path, bool direct) {
  File f;
  f.fd_ = OpenWithOptionalDirect(path, O_WRONLY | O_CREAT | O_TRUNC, direct,
                                 &f.direct_);
  f.path_ = path;
  return f;
}

void File::ReadPage(std::uint64_t page_index, void* out) {
  if (fd_ < 0) throw Error("ReadPage on a closed file");
  const off_t offset = static_cast<off_t>(page_index * kPageSize);
  std::size_t done = 0;
  auto* dst = static_cast<std::uint8_t*>(out);
  while (done < kPageSize) {
    ssize_t n = ::pread(fd_, dst + done, kPageSize - done,
                        offset + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw Error(Errno("pread " + path_, errno));
    }
    if (n == 0) {
      throw Error("short read on " + path_ + ": page " +
                  std::to_string(page_index) + " is past end of file");
    }
    done += static_cast<std::size_t>(n);
  }
}

void File::WritePage(std::uint64_t page_index, const void* in) {
  if (fd_ < 0) throw Error("WritePage on a closed file");
  const off_t offset = static_cast<off_t>(page_index * kPageSize);
  std::size_t done = 0;
  const auto* src = static_cast<const std::uint8_t*>(in);
  while (done < kPageSize) {
    ssize_t n = ::pwrite(fd_, src + done, kPageSize - done,
                         offset + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) continue;
      throw Error(Errno("pwrite " + path_, errno));
    }
    done += static_cast<std::size_t>(n);
  }
}

std::uint64_t File::NumPages() {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) throw Error(Errno("fstat " + path_, errno));
  return static_cast<std::uint64_t>(st.st_size) / kPageSize;
}

void File::Sync() {
  if (fd_ >= 0 && ::fsync(fd_) != 0) throw Error(Errno("fsync " + path_, errno));
}

void File::Close() {
  if (fd_ >= 0) {
    int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0) throw Error(Errno("close " + path_, errno));
  }
}

void RemoveFile(const std::string& path) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    throw Error(Errno("unlink " + path, errno));
  }
}

}  // namespace kvstore
