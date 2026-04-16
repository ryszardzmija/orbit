#pragma once

#include <cassert>

#include <unistd.h>

namespace orbit {

class FileDescriptor {
public:
    explicit FileDescriptor(int fd)
        : fd_(fd) {
        assert(fd >= 0);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_(other.fd_) {
        other.fd_ = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }

        return *this;
    }

    int get() const { return fd_; }

    ~FileDescriptor() {
        if (fd_ != -1) {
            close(fd_);
        }
    }

private:
    int fd_;
};

} // namespace orbit
