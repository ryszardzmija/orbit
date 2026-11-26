#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "proxy/detail/allocation.h"
#include "proxy/detail/buffer/buffer_budget.h"

namespace orbit::proxy::detail {

class SendBuffer {
public:
    enum class BufferStatus {
        Accepting,
        Paused,
    };

    class AppendReservation {
    public:
        AppendReservation(const AppendReservation&) = delete;
        AppendReservation& operator=(const AppendReservation&) = delete;

        size_t capacity() const { return capacity_; }

    private:
        friend class SendBuffer;

        AppendReservation(SendBuffer& buffer, size_t capacity, BufferBudget::Reservation blocks)
            : buffer_(&buffer),
              capacity_(capacity),
              blocks_(std::move(blocks)) {}

        SendBuffer* buffer_;
        size_t capacity_;
        BufferBudget::Reservation blocks_;
    };

    SendBuffer(size_t block_size, size_t high_watermark, size_t low_watermark,
               AllocationChecks allocation = {}, BufferBudget* budget = nullptr);

    SendBuffer(const SendBuffer&) = delete;
    SendBuffer& operator=(const SendBuffer&) = delete;
    SendBuffer(SendBuffer&&) = delete;
    SendBuffer& operator=(SendBuffer&&) = delete;

    ~SendBuffer();

    // Rejects inadmissible writes without changing the queue.
    bool write(std::span<const uint8_t> data);
    // Reserves only the blocks needed beyond reusable capacity in the current tail.
    AppendReservation reserveAppend(size_t requested, bool allow_new_blocks = true);
    bool write(std::span<const uint8_t> data, AppendReservation& reservation);
    size_t copy(std::span<uint8_t> dest);
    void consume(size_t num_bytes);
    BufferStatus status() const;
    bool empty() const;
    size_t size() const { return size_; }
    size_t remainingCapacity() const;
    size_t allocatedCapacity() const { return allocated_capacity_; }

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

    size_t blocksNeeded(size_t bytes) const;
    size_t tailCapacity() const;
    void writeData(std::span<const uint8_t> data, BufferBudget::Reservation& blocks);
    void addBlock(BufferBudget::Reservation& blocks);
    uint8_t* getDestPtr();

    void consumeData(size_t num_bytes);
    void discardBlock();

    void updateStatus();

    size_t block_size_;
    size_t high_watermark_;
    size_t low_watermark_;
    AllocationChecks allocation_;
    BufferBudget* budget_;

    std::unique_ptr<ListNode> head_ = nullptr;
    ListNode* tail_ = nullptr;
    size_t size_ = 0;
    size_t allocated_capacity_ = 0;
    size_t read_pos_ = 0;
    size_t write_pos_ = 0;
    BufferStatus status_ = BufferStatus::Accepting;
};

} // namespace orbit::proxy::detail
