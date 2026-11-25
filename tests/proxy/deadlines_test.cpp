#include <gtest/gtest.h>

#include "proxy/detail/deadlines.h"

namespace orbit::proxy::detail {
namespace {

using namespace std::chrono_literals;

TEST(DeadlinesTest, OrdersCancelsAndReschedulesOnlyLiveKeys) {
    Deadlines deadlines;
    auto now = Deadline{};
    DeadlineKey first{DeadlineKind::Connect, 1};
    DeadlineKey second{DeadlineKind::Connect, 2};
    DeadlineKey shutdown{DeadlineKind::Shutdown, 0};
    deadlines.schedule(first, now + 10ms);
    deadlines.schedule(second, now + 3ms);
    deadlines.schedule(shutdown, now + 5ms);
    deadlines.cancel(second);
    for (int i = 0; i < 1000; ++i) {
        deadlines.schedule(first, now + std::chrono::milliseconds(i));
    }
    deadlines.schedule(first, now + 5ms);
    EXPECT_EQ(now + 5ms, deadlines.next());
    EXPECT_FALSE(deadlines.popDue(now + 4ms));
    EXPECT_EQ(shutdown, deadlines.popDue(now + 5ms));
    EXPECT_EQ(first, deadlines.popDue(now + 5ms));
    EXPECT_FALSE(deadlines.next());
    EXPECT_FALSE(deadlines.popDue(now + 10s));
}

} // namespace
} // namespace orbit::proxy::detail
