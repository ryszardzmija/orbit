#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace orbit {

class SendBuffer {
public:
    enum class BufferStatus {
        Accepting,
        Paused,
    };

    SendBuffer(size_t block_size, size_t high_watermark, size_t low_watermark);

    SendBuffer(const SendBuffer&) = delete;
    SendBuffer& operator=(const SendBuffer&) = delete;
    SendBuffer(SendBuffer&&) = delete;
    SendBuffer& operator=(SendBuffer&&) = delete;

    ~SendBuffer();

    void write(std::span<const uint8_t> data);
    size_t copy(std::span<uint8_t> dest);
    void consume(size_t num_bytes);
    BufferStatus status() const;
    bool empty() const;

private:
    class ListNode {
    public:
        ListNode(size_t capacity);

        std::span<uint8_t> data();
        std::span<const uint8_t> data() const;

        ListNode* next() const;
        void setNext(std::unique_ptr<ListNode> next);
        std::unique_ptr<ListNode> takeNext() { return std::move(next_); }

    private:
        std::unique_ptr<uint8_t[]> data_;
        size_t capacity_;
        std::unique_ptr<ListNode> next_ = nullptr;
    };

    void writeData(std::span<const uint8_t> data);
    void addBlock();
    uint8_t* getDestPtr();

    void consumeData(size_t num_bytes);
    void discardBlock();

    void updateStatus();

    size_t block_size_;
    size_t high_watermark_;
    size_t low_watermark_;

    std::unique_ptr<ListNode> head_ = nullptr;
    ListNode* tail_ = nullptr;
    size_t size_ = 0;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    BufferStatus status_ = BufferStatus::Accepting;
};

} // namespace orbit
