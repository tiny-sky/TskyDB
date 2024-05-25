#include "env.h"

#include <fcntl.h>
#include <unistd.h>
#include <string>

namespace Tskydb {

constexpr const size_t kWritableFileBufferSize = 65536;

Status PosixError(const std::string &context, int error_number) {
  if (error_number == ENOENT) {
    return Status::NotFound(context, std::strerror(error_number));
  } else {
    return Status::IOError(context, std::strerror(error_number));
  }
}

Status Env::NewWritableFile(const std::string &filename,
                            WritableFile **result) {}

WritableFile::WritableFile(std::string filename, int fd)
    : pos_(0),
      fd_(fd),
      is_manifest_(IsManifest(filename)),
      filename_(std::move(filename)),
      dirname_(Dirname(filename)) {}

WritableFile::~WritableFile() {
  if (fd_ >= 0) {
    Close();
  }
}

Status WritableFile::Append(const Slice &data) {
  size_t write_size = data.size();
  const char *write_data = data.data();

  size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
  std::memcpy(buf_ + pos_, write_data, copy_size);
  write_size -= copy_size;
  pos_ += copy_size;
  if (write_size == 0) {
    return Status::OK();
  }

  Status status = FlushBuffer();
  if (!status.ok()) {
    return status;
  }

  if (write_size < kWritableFileBufferSize) {
    std::memcpy(buf_, write_data, write_size);
    pos_ = write_size;
    return Status::OK();
  }
  return WriteUnbuffered(write_data, write_size);
}

Status WritableFile::Close() {
  Status status = FlushBuffer();
  const int close_result = ::close(fd_);
  if (close_result < 0 && status.ok()) {
    status = PosixError(filename_, errno);
  }
  fd_ = -1;
  return status;
}

Status WritableFile::Flush() { return FlushBuffer(); }

Status WritableFile::Sync() {
  Status status = SyncDirIfManifest();
  if (!status.ok()) {
    return status;
  }

  status = FlushBuffer();
  if (!status.ok()) {
    return status;
  }

  return SyncFd(fd_, filename_);
}

Status WritableFile::FlushBuffer() {
  Status status = WriteUnbuffered(buf_, pos_);
  pos_ = 0;
  return status;
}

Status WritableFile::WriteUnbuffered(const char *data, size_t size) {
  while (size > 0) {
    ssize_t write_result = ::write(fd_, data, size);
    if (write_result < 0) {
      if (errno == EINTR) {
        continue;  // Retry
      }
      return PosixError(filename_, errno);
    }
    data += write_result;
    size -= write_result;
  }
  return Status::OK();
}

Status WritableFile::SyncDirIfManifest() {
  Status status;
  if (!is_manifest_) {
    return status;
  }

  int fd = ::open(dirname_.c_str(), O_RDONLY);
  if (fd < 0) {
    status = PosixError(dirname_, errno);
  } else {
    status = SyncFd(fd, dirname_);
    ::close(fd);
  }
  return status;
}

Status WritableFile::SyncFd(int fd, const std::string &fd_path) {
  bool sync_success = ::fsync(fd) == 0;
  if (sync_success) {
    return Status::OK();
  }
  return PosixError(fd_path, errno);
}

SequentialFile::SequentialFile(std::string filename, int fd)
    : fd_(fd), filename_(std::move(filename)) {}

SequentialFile::~SequentialFile() { close(fd_); }

Status SequentialFile::Read(size_t n, Slice *result, char *scratch) {
  Status status;
  while (true) {
    ::ssize_t read_size = ::read(fd_, scratch, n);
    if (read_size < 0) {  // Read error.
      if (errno == EINTR) {
        continue;  // Retry
      }
      status = PosixError(filename_, errno);
      break;
    }
    *result = Slice(scratch, read_size);
    break;
  }
  return status;
}

Status SequentialFile::Skip(uint64_t n) {
  if (::lseek(fd_, n, SEEK_CUR) == static_cast<off_t>(-1)) {
    return PosixError(filename_, errno);
  }
  return Status::OK();
}

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string filename, int fd)
      : filename_(std::move(filename)), fd_(fd) {}

 private:
  const int fd_;  // -1 if has_permanent_fd_ is false.
  const std::string filename_;
};

}  // namespace Tskydb