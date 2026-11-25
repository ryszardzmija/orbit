#include <array>

#include <gtest/gtest.h>
#include <sys/signalfd.h>
#include <sys/socket.h>

#include "proxy/detail/signal_fd.h"
#include "socket_pair.h"

namespace orbit::proxy::detail {
namespace {

TEST(ShutdownSignalTest, ReadsAtMostTwoRequestsBeforeReturningToTheReactor) {
    auto stream = test::socketPair();
    std::array<signalfd_siginfo, 3> records{};
    ASSERT_EQ(ssize_t(sizeof(records)),
              send(stream.peer.get(), records.data(), sizeof(records), 0));
    for (int expected : {2, 1, 0}) {
        auto count = readShutdownRequests(stream.local.get());
        ASSERT_TRUE(count) << count.error().message();
        EXPECT_EQ(expected, *count);
    }
}

} // namespace
} // namespace orbit::proxy::detail
