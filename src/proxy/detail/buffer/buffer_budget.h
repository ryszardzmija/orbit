#pragma once

#include <cassert>
#include <cstddef>
#include <utility>

namespace orbit::proxy::detail {

class SendBuffer;

// The reactor thread owns the budget. Buffers and their reservations borrow it.
class BufferBudget {
public:
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        Reservation(Reservation&& other) noexcept
            : budget_(std::exchange(other.budget_, nullptr)),
              bytes_(std::exchange(other.bytes_, 0)) {}

        ~Reservation() {
            if (budget_) {
                budget_->reserved_ -= bytes_;
            }
        }

        size_t bytes() const { return bytes_; }

    private:
        friend class BufferBudget;
        friend class SendBuffer;

        Reservation(BufferBudget& budget, size_t bytes)
            : budget_(&budget),
              bytes_(bytes) {}

        // The allocation replaces its charge in the reservation without changing total usage.
        void commit(size_t bytes) noexcept {
            assert(budget_ && bytes <= bytes_);
            bytes_ -= bytes;
            budget_->reserved_ -= bytes;
            budget_->allocated_ += bytes;
        }

        BufferBudget* budget_ = nullptr;
        size_t bytes_ = 0;
    };

    explicit BufferBudget(size_t limit)
        : limit_(limit) {}
    BufferBudget(const BufferBudget&) = delete;
    BufferBudget& operator=(const BufferBudget&) = delete;

    size_t allocated() const { return allocated_; }
    size_t reserved() const { return reserved_; }
    size_t available() const { return limit_ - allocated_ - reserved_; }

private:
    friend class SendBuffer;

    Reservation reserve(size_t bytes) {
        if (bytes > available()) {
            return {};
        }
        reserved_ += bytes;
        return Reservation(*this, bytes);
    }

    void release(size_t bytes) noexcept {
        assert(bytes <= allocated_);
        allocated_ -= bytes;
    }

    size_t limit_;
    size_t allocated_ = 0;
    size_t reserved_ = 0;
};

} // namespace orbit::proxy::detail
