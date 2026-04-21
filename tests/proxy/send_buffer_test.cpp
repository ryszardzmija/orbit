#include <cstddef>
#include <gtest/gtest.h>
#include <span>
#include <stdexcept>
#include <string>

#include "send_buffer.h"

class SendBufferTest : public testing::Test {
protected:
    static constexpr size_t block_size = 64;
    static constexpr size_t max_blocks = 16;
    static constexpr size_t high_watermark = block_size * max_blocks;
    static constexpr size_t low_watermark = block_size * 8;

    SendBufferTest()
        : send_buffer(block_size, high_watermark, low_watermark) {}

    orbit::SendBuffer send_buffer;
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

TEST_F(SendBufferTest, WriteCanExceedHighWatermarkInSingleCall) {
    std::string data((max_blocks + 1) * block_size, 'a');

    send_buffer.write(asBytes(data));
    std::string result(data.size(), 'b');
    size_t bytes_copied = send_buffer.copy(asBytes(result));

    EXPECT_EQ(data.size(), bytes_copied);
    EXPECT_EQ(data, result);
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
    EXPECT_EQ(orbit::SendBuffer::BufferStatus::Accepting, send_buffer.status());
}

TEST_F(SendBufferTest, BufferIsPausedAfterHittingHighWatermark) {
    std::string data(max_blocks * block_size, 'a');

    send_buffer.write(asBytes(data));

    EXPECT_EQ(orbit::SendBuffer::BufferStatus::Paused, send_buffer.status());
}

TEST_F(SendBufferTest, BufferResumesAfterComingBackToLowWatermark) {
    std::string data(max_blocks * block_size, 'a');

    send_buffer.write(asBytes(data));
    send_buffer.consume(max_blocks * block_size - low_watermark);

    EXPECT_EQ(orbit::SendBuffer::BufferStatus::Accepting, send_buffer.status());
}

TEST_F(SendBufferTest, BufferResumesAfterGoingBelowLowWatermark) {
    std::string data(max_blocks * block_size, 'a');

    send_buffer.write(asBytes(data));
    send_buffer.consume(max_blocks * block_size - low_watermark + 1);

    EXPECT_EQ(orbit::SendBuffer::BufferStatus::Accepting, send_buffer.status());
}

TEST_F(SendBufferTest, BufferConstructionFailsForBlockSizeOfZero) {
    EXPECT_THROW(orbit::SendBuffer(0, high_watermark, low_watermark), std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionFailsForZeroHighWatermark) {
    EXPECT_THROW(orbit::SendBuffer(block_size, 0, 0), std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionSucceedsForZeroLowWatermark) {
    EXPECT_NO_THROW(orbit::SendBuffer(block_size, high_watermark, 0));
}

TEST_F(SendBufferTest, BufferConstructionFailsForBlockSizeGreaterThanHighWatermark) {
    EXPECT_THROW(orbit::SendBuffer(high_watermark + 1, high_watermark, low_watermark),
                 std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionFailsForLowWatermarkEqualToHighWatermark) {
    EXPECT_THROW(orbit::SendBuffer(block_size, high_watermark, high_watermark),
                 std::invalid_argument);
}

TEST_F(SendBufferTest, BufferConstructionFailsForLowWatermarkGreaterThanHighWatermark) {
    EXPECT_THROW(orbit::SendBuffer(block_size, high_watermark, high_watermark + 1),
                 std::invalid_argument);
}
