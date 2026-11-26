#include "proxy/detail/buffer/send_buffer.h"

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

using orbit::proxy::detail::AllocationPoint;
using orbit::proxy::detail::BufferBudget;
using orbit::proxy::detail::SendBuffer;

class SendBufferTest : public testing::Test {
protected:
    static constexpr size_t block_size = 64;
    static constexpr size_t max_blocks = 16;
    static constexpr size_t high_watermark = block_size * max_blocks;
    static constexpr size_t low_watermark = block_size * 8;

    SendBufferTest()
        : send_buffer(block_size, high_watermark, low_watermark) {}

    SendBuffer send_buffer;
};

namespace {

std::span<const uint8_t> asBytes(const std::string& data) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

std::span<uint8_t> asBytes(std::string& data) {
    return std::span<uint8_t>(reinterpret_cast<uint8_t*>(data.data()), data.size());
}

} // namespace

TEST_F(SendBufferTest, CorrectlyCopiesDataWithinSingleBlock) {
    std::string data(block_size, 'a');

    send_buffer.write(asBytes(data));
    std::string result(data.size(), 'b');
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    ASSERT_EQ(data.size(), bytes_copied);
    EXPECT_EQ(data, result);
}

TEST_F(SendBufferTest, CorrectlyCopiesDataAcrossMultipleBlocks) {
    std::string data((max_blocks - 1) * block_size, 'a');

    send_buffer.write(asBytes(data));
    std::string result(data.size(), 'b');
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    ASSERT_EQ(data.size(), bytes_copied);
    EXPECT_EQ(data, result);
}

TEST_F(SendBufferTest, CorrectlyConsumesDataWithinSingleBlock) {
    std::string data(block_size, 'a');

    send_buffer.write(asBytes(data));
    send_buffer.consume(data.size());
    std::string result(data.size(), 'b');
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    EXPECT_TRUE(send_buffer.empty());
    EXPECT_EQ(0, bytes_copied);
    EXPECT_EQ(std::string(data.size(), 'b'), result);
}

TEST_F(SendBufferTest, CorrectlyConsumesDataAcrossMultipleBlocks) {
    std::string data((max_blocks - 1) * block_size, 'a');

    send_buffer.write(asBytes(data));
    send_buffer.consume((max_blocks - 3) * block_size);
    std::string result(block_size * 2, 'b');
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    EXPECT_EQ(2 * block_size, bytes_copied);
    EXPECT_EQ(std::string(block_size * 2, 'a'), result);
}

TEST_F(SendBufferTest, RejectsAnOversizedWriteWithoutChangingTheQueue) {
    const std::string prefix = "prefix";
    ASSERT_TRUE(send_buffer.write(asBytes(prefix)));
    const std::string oversized(high_watermark - prefix.size() + 1, 'a');
    EXPECT_FALSE(send_buffer.write(asBytes(oversized)));
    EXPECT_EQ(prefix.size(), send_buffer.size());
    EXPECT_EQ(high_watermark - prefix.size(), send_buffer.remainingCapacity());
    EXPECT_EQ(block_size, send_buffer.allocatedCapacity());
    std::string result(prefix.size(), 'b');
    EXPECT_EQ(prefix.size(), send_buffer.copy(asBytes(result)));
    EXPECT_EQ(prefix, result);
}

TEST_F(SendBufferTest, CountsHeadSlackAndReclaimsThePartialLastBlock) {
    const std::string full(high_watermark, 'a');
    ASSERT_TRUE(send_buffer.write(asBytes(full)));
    EXPECT_EQ(high_watermark, send_buffer.allocatedCapacity());
    send_buffer.consume(1);
    EXPECT_EQ(0u, send_buffer.remainingCapacity());
    EXPECT_FALSE(send_buffer.write(asBytes(full).first(1)));

    send_buffer.consume(high_watermark - low_watermark - 1);
    EXPECT_EQ(high_watermark - low_watermark, send_buffer.remainingCapacity());
    send_buffer.consume(1);
    ASSERT_TRUE(send_buffer.write(asBytes(full).first(high_watermark - low_watermark + 1)));
    EXPECT_EQ(high_watermark, send_buffer.size());
    EXPECT_EQ(high_watermark + block_size, send_buffer.allocatedCapacity());
    send_buffer.consume(high_watermark);
    EXPECT_EQ(0u, send_buffer.allocatedCapacity());
    EXPECT_EQ(high_watermark, send_buffer.remainingCapacity());
    ASSERT_TRUE(send_buffer.write(asBytes(full).first(1)));
    EXPECT_EQ(block_size, send_buffer.allocatedCapacity());
    send_buffer.consume(1);
    EXPECT_EQ(0u, send_buffer.allocatedCapacity());
}

TEST_F(SendBufferTest, BufferPreservesInputDataOrdering) {
    std::string data;
    for (int i = 0; i < max_blocks / 2; i++) {
        data += std::string(block_size, 'a' + i);
    }

    send_buffer.write(asBytes(data));
    std::string result(data.size(), 0);
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    EXPECT_EQ(data.size(), bytes_copied);
    EXPECT_EQ(data, result);
}

TEST_F(SendBufferTest, BufferCorrectlyHandlesWriteConsumeCycles) {
    std::string data1(block_size / 2, 'a');
    std::string data2(block_size * 2, 'b');
    std::string data3(block_size, 'c');

    std::string result1(block_size / 4, 0);
    std::string result2(block_size / 4, 0);
    std::string result3(block_size, 0);
    std::string result4(block_size, 0);
    std::string result5(block_size / 2, 0);
    std::string result6(block_size / 2, 0);
    send_buffer.write(asBytes(data1));
    send_buffer.write(asBytes(data2));
    send_buffer.copy(asBytes(result1));
    send_buffer.consume(result1.size());
    send_buffer.copy(asBytes(result2));
    send_buffer.consume(result2.size());
    send_buffer.copy(asBytes(result3));
    send_buffer.consume(result3.size());
    send_buffer.write(asBytes(data3));
    send_buffer.copy(asBytes(result4));
    send_buffer.consume(result4.size());
    send_buffer.copy(asBytes(result5));
    send_buffer.consume(result5.size());
    send_buffer.copy(asBytes(result6));
    send_buffer.consume(result6.size());

    EXPECT_EQ(std::string(block_size / 4, 'a'), result1);
    EXPECT_EQ(std::string(block_size / 4, 'a'), result2);
    EXPECT_EQ(std::string(block_size, 'b'), result3);
    EXPECT_EQ(std::string(block_size, 'b'), result4);
    EXPECT_EQ(std::string(block_size / 2, 'c'), result5);
    EXPECT_EQ(std::string(block_size / 2, 'c'), result6);
}

TEST_F(SendBufferTest, CopyDoesNotReadBeyondAvailableData) {
    std::string data(block_size, 'a');

    send_buffer.write(asBytes(data));
    std::string result(2 * block_size, 'b');
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    EXPECT_EQ(block_size, bytes_copied);
    EXPECT_EQ(data + std::string(block_size, 'b'), result);
}

TEST_F(SendBufferTest, InitialBufferIsEmpty) { EXPECT_TRUE(send_buffer.empty()); }

TEST_F(SendBufferTest, InitialBufferIsAccepting) {
    EXPECT_EQ(SendBuffer::BufferStatus::Accepting, send_buffer.status());
}

TEST_F(SendBufferTest, BufferIsPausedAfterHittingHighWatermark) {
    std::string data(max_blocks * block_size, 'a');

    send_buffer.write(asBytes(data));

    EXPECT_EQ(SendBuffer::BufferStatus::Paused, send_buffer.status());
}

TEST_F(SendBufferTest, BufferResumesAfterComingBackToLowWatermark) {
    std::string data(max_blocks * block_size, 'a');

    send_buffer.write(asBytes(data));
    send_buffer.consume(max_blocks * block_size - low_watermark);

    EXPECT_EQ(SendBuffer::BufferStatus::Accepting, send_buffer.status());
}

TEST_F(SendBufferTest, BufferResumesAfterGoingBelowLowWatermark) {
    std::string data(max_blocks * block_size, 'a');

    send_buffer.write(asBytes(data));
    send_buffer.consume(max_blocks * block_size - low_watermark + 1);

    EXPECT_EQ(SendBuffer::BufferStatus::Accepting, send_buffer.status());
}

TEST_F(SendBufferTest, BufferConstructionFailsForBlockSizeOfZero) {
    EXPECT_THROW(SendBuffer(0, high_watermark, low_watermark), std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionFailsForZeroHighWatermark) {
    EXPECT_THROW(SendBuffer(block_size, 0, 0), std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionSucceedsForZeroLowWatermark) {
    EXPECT_NO_THROW(SendBuffer(block_size, high_watermark, 0));
}

TEST_F(SendBufferTest, BufferConstructionFailsForBlockSizeGreaterThanHighWatermark) {
    EXPECT_THROW(SendBuffer(high_watermark + 1, high_watermark, low_watermark),
                 std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionFailsForLowWatermarkEqualToHighWatermark) {
    EXPECT_THROW(SendBuffer(block_size, high_watermark, high_watermark), std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionFailsForLowWatermarkGreaterThanHighWatermark) {
    EXPECT_THROW(SendBuffer(block_size, high_watermark, high_watermark + 1), std::invalid_argument);
}

TEST_F(SendBufferTest, RejectsAnUnrepresentableBlockCapacity) {
    EXPECT_THROW(SendBuffer(block_size, std::numeric_limits<size_t>::max(), 0),
                 std::invalid_argument);
}

TEST(BufferBudgetTest, CommittedBlocksAndActiveReservationsShareTheSameLimit) {
    BufferBudget budget(8);
    SendBuffer first(4, 8, 4, {}, &budget);
    SendBuffer second(4, 8, 4, {}, &budget);
    const std::string bytes = "12345678";
    {
        auto reservation = first.reserveAppend(bytes.size());
        EXPECT_EQ(8u, reservation.capacity());
        EXPECT_EQ(8u, budget.reserved());
        EXPECT_EQ(0u, budget.available());
        EXPECT_FALSE(second.write(asBytes(bytes).first(1)));
        ASSERT_TRUE(first.write(asBytes(bytes).first(3), reservation));
        EXPECT_EQ(first.allocatedCapacity(), budget.allocated());
        EXPECT_EQ(4u, budget.reserved());
        auto tail = first.reserveAppend(4, false);
        EXPECT_EQ(1u, tail.capacity());
        ASSERT_TRUE(first.write(asBytes(bytes).subspan(3, 1), tail));
        EXPECT_FALSE(first.write(asBytes(bytes).first(1), tail));
    }
    EXPECT_EQ(0u, budget.reserved());
    EXPECT_EQ(4u, budget.available());
    ASSERT_TRUE(second.write(asBytes(bytes).first(1)));
    EXPECT_EQ(first.allocatedCapacity() + second.allocatedCapacity(), budget.allocated());
    EXPECT_EQ(0u, budget.available());
    first.consume(first.size());
    second.consume(second.size());
    EXPECT_EQ(0u, budget.allocated());
    EXPECT_EQ(8u, budget.available());
}

TEST(BufferBudgetTest, AllocationFailureCancelsOnlyTheUncommittedReservation) {
    BufferBudget budget(8);
    int calls = 0;
    {
        SendBuffer buffer(
            4, 8, 4,
            {&calls, [](void* context,
                        AllocationPoint) noexcept { return ++*static_cast<int*>(context) == 2; }},
            &budget);
        const std::string bytes = "abcdefgh";
        {
            auto reservation = buffer.reserveAppend(bytes.size());
            EXPECT_THROW(buffer.write(asBytes(bytes), reservation), std::bad_alloc);
            EXPECT_EQ(4u, budget.reserved());
            EXPECT_EQ(4u, buffer.size());
            EXPECT_EQ(buffer.allocatedCapacity(), budget.allocated());
        }
        EXPECT_EQ(0u, budget.reserved());
        EXPECT_EQ(4u, budget.available());
        std::string retained(4, '\0');
        EXPECT_EQ(4u, buffer.copy(asBytes(retained)));
        EXPECT_EQ("abcd", retained);
    }
    EXPECT_EQ(8u, budget.available());
}
