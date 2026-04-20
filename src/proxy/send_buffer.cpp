#include "send_buffer.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>

namespace orbit {

// ListNode
SendBuffer::ListNode::ListNode(size_t capacity)
    : data_(std::make_unique<uint8_t[]>(capacity)),
      capacity_(capacity) {}

std::span<uint8_t> SendBuffer::ListNode::data() { return {data_.get(), capacity_}; }

std::span<const uint8_t> SendBuffer::ListNode::data() const { return {data_.get(), capacity_}; }

SendBuffer::ListNode* SendBuffer::ListNode::next() const { return next_.get(); }

void SendBuffer::ListNode::setNext(std::unique_ptr<ListNode> next) { next_ = std::move(next); }

// SendBuffer
SendBuffer::SendBuffer(size_t block_size, size_t high_watermark, size_t low_watermark)
    : block_size_(block_size),
      high_watermark_(high_watermark),
      low_watermark_(low_watermark) {
    if (block_size == 0) {
        throw std::invalid_argument("block_size cannot be zero, got block_size=" +
                                    std::to_string(block_size));
    }
    if (low_watermark >= high_watermark) {
        throw std::invalid_argument(
            "low_watermark must be less than high_watermark, got low_watermark=" +
            std::to_string(low_watermark) + " high_watermark=" + std::to_string(high_watermark));
    }
    if (block_size > high_watermark) {
        throw std::invalid_argument("block_size cannot exceed high_watermark, got block_size=" +
                                    std::to_string(block_size) +
                                    " high_watermark=" + std::to_string(high_watermark));
    }
}

SendBuffer::~SendBuffer() {
    while (head_ != nullptr) {
        // Destroy the current node and take the resources of the next one.
        // This prevents the class from relying on cascading destructor calls
        // (which for very deep buffers could cause stack overflow) for cleanup.
        head_ = head_->takeNext();
    }
}

bool SendBuffer::empty() const { return size_ == 0; }

SendBuffer::BufferStatus SendBuffer::status() const { return status_; }

void SendBuffer::write(std::span<const uint8_t> data) {
    assert(status_ == BufferStatus::Accepting);
    writeData(data);
    updateStatus();
}

void SendBuffer::writeData(std::span<const uint8_t> data) {
    while (!data.empty()) {
        if (tail_ == nullptr || write_pos_ == block_size_) {
            addBlock();
        }

        size_t to_copy = std::min(data.size(), block_size_ - write_pos_);
        std::memcpy(getDestPtr(), data.data(), to_copy);
        write_pos_ += to_copy;
        size_ += to_copy;
        data = data.subspan(to_copy);
    }
}

void SendBuffer::addBlock() {
    if (tail_ == nullptr) {
        head_ = std::make_unique<ListNode>(block_size_);
        tail_ = head_.get();
    } else {
        tail_->setNext(std::make_unique<ListNode>(block_size_));
        tail_ = tail_->next();
    }
    write_pos_ = 0;
}

uint8_t* SendBuffer::getDestPtr() { return tail_->data().subspan(write_pos_).data(); }

size_t SendBuffer::copy(std::span<uint8_t> dest) {
    size_t bytes_copied = 0;
    size_t copy_read_pos = read_pos_;
    ListNode* copy_head = head_.get();

    while (bytes_copied < size_ && !dest.empty()) {
        size_t to_copy = std::min({size_ - bytes_copied, dest.size(), block_size_ - copy_read_pos});
        const uint8_t* copy_ptr = copy_head->data().data() + copy_read_pos;
        std::memcpy(dest.data(), copy_ptr, to_copy);

        bytes_copied += to_copy;
        copy_read_pos += to_copy;
        dest = dest.subspan(to_copy);

        if (copy_read_pos == block_size_) {
            copy_head = copy_head->next();
            copy_read_pos = 0;
        }
    }

    return bytes_copied;
}

void SendBuffer::consume(size_t num_bytes) {
    consumeData(num_bytes);
    updateStatus();
}

void SendBuffer::consumeData(size_t num_bytes) {
    assert(num_bytes <= size_);

    size_t bytes_consumed = 0;
    num_bytes = std::min(num_bytes, size_);

    while (bytes_consumed < num_bytes) {
        size_t to_consume = std::min(num_bytes - bytes_consumed, block_size_ - read_pos_);
        read_pos_ += to_consume;
        size_ -= to_consume;
        bytes_consumed += to_consume;

        if (read_pos_ == block_size_) {
            discardBlock();
        }
    }
}

void SendBuffer::discardBlock() {
    head_ = head_->takeNext();
    if (head_ == nullptr) {
        tail_ = nullptr;
    }
    read_pos_ = 0;
}

void SendBuffer::updateStatus() {
    if (status_ == BufferStatus::Accepting && size_ >= high_watermark_) {
        status_ = BufferStatus::Paused;
    } else if (status_ == BufferStatus::Paused && size_ <= low_watermark_) {
        status_ = BufferStatus::Accepting;
    }
}

} // namespace orbit
