#include "config/config.h"

#include <array>
#include <cstdlib>
#include <expected>
#include <initializer_list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using orbit::Config;
using orbit::ConfigError;
using orbit::parseConfig;

namespace {

std::expected<Config, ConfigError>
parseArguments(std::initializer_list<std::string> input_arguments) {
    std::vector<std::string> arguments{"orbit"};
    arguments.insert(arguments.end(), input_arguments.begin(), input_arguments.end());

    std::vector<char*> argument_pointers;
    argument_pointers.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argument_pointers.push_back(argument.data());
    }

    return parseConfig(static_cast<int>(argument_pointers.size()), argument_pointers.data());
}

class ConfigTest : public testing::Test {
protected:
    void SetUp() override {
        constexpr std::array environment_variables{
            "ORBIT_LISTEN_HOST",
            "ORBIT_LISTEN_PORT",
            "ORBIT_UPSTREAMS",
            "ORBIT_LOG_LEVEL",
            "ORBIT_CONNECT_TIMEOUT_MS",
            "ORBIT_IDLE_TIMEOUT_MS",
            "ORBIT_BUFFER_HIGH_WATERMARK_BYTES",
            "ORBIT_BUFFER_LOW_WATERMARK_BYTES",
            "ORBIT_MAX_CONNECTIONS",
            "ORBIT_MAX_PENDING_CONNECTIONS",
            "ORBIT_BUFFER_BUDGET_BYTES",
        };

        for (const char* variable : environment_variables) {
            ASSERT_EQ(0, unsetenv(variable));
        }
    }
};

} // namespace

TEST_F(ConfigTest, ParsesDnsUpstream) {
    const auto result = parseArguments({"--upstream", "example.com:443"});

    ASSERT_TRUE(result);
    ASSERT_EQ(1, result->upstreams.size());
    EXPECT_EQ("example.com", result->upstreams[0].host);
    EXPECT_EQ(443, result->upstreams[0].port);
}

TEST_F(ConfigTest, ParsesIpv4Upstream) {
    const auto result = parseArguments({"--upstream", "127.0.0.1:8080"});

    ASSERT_TRUE(result);
    ASSERT_EQ(1, result->upstreams.size());
    EXPECT_EQ("127.0.0.1", result->upstreams[0].host);
    EXPECT_EQ(8080, result->upstreams[0].port);
}

TEST_F(ConfigTest, ParsesBracketedIpv6Upstream) {
    const auto result = parseArguments({"--upstream", "[2001:db8::1]:443"});

    ASSERT_TRUE(result);
    ASSERT_EQ(1, result->upstreams.size());
    EXPECT_EQ("2001:db8::1", result->upstreams[0].host);
    EXPECT_EQ(443, result->upstreams[0].port);
}

TEST_F(ConfigTest, PreservesRepeatedUpstreamOrder) {
    const auto result = parseArguments({
        "--upstream",
        "first.example:1000",
        "--upstream",
        "second.example:2000",
    });

    ASSERT_TRUE(result);
    ASSERT_EQ(2, result->upstreams.size());
    EXPECT_EQ("first.example", result->upstreams[0].host);
    EXPECT_EQ(1000, result->upstreams[0].port);
    EXPECT_EQ("second.example", result->upstreams[1].host);
    EXPECT_EQ(2000, result->upstreams[1].port);
}

TEST_F(ConfigTest, AcceptsPortRangeBoundaries) {
    const auto result = parseArguments({
        "--upstream",
        "first.example:1",
        "--upstream",
        "last.example:65535",
    });

    ASSERT_TRUE(result);
    ASSERT_EQ(2, result->upstreams.size());
    EXPECT_EQ(1, result->upstreams[0].port);
    EXPECT_EQ(65535, result->upstreams[1].port);
}

TEST_F(ConfigTest, ParsesCommaSeparatedUpstreamsFromEnvironment) {
    ASSERT_EQ(0, setenv("ORBIT_UPSTREAMS", "first.example:1000,[::1]:2000", 1));

    const auto result = parseArguments({});

    ASSERT_TRUE(result);
    ASSERT_EQ(2, result->upstreams.size());
    EXPECT_EQ("first.example", result->upstreams[0].host);
    EXPECT_EQ(1000, result->upstreams[0].port);
    EXPECT_EQ("::1", result->upstreams[1].host);
    EXPECT_EQ(2000, result->upstreams[1].port);
}

TEST_F(ConfigTest, RejectsMissingUpstream) {
    auto result = parseArguments({});

    ASSERT_FALSE(result);
    EXPECT_NE(0, result.error().exit_code);
}

TEST_F(ConfigTest, RejectsInvalidUpstreams) {
    constexpr std::array invalid_upstreams{
        "",
        "host",
        ":80",
        "host:",
        "host:not-a-number",
        "host:80x",
        "host:0",
        "host:65536",
        "host:42949672960",
        "::1:443",
        "[::1:443",
        "[::1]443",
        "[[::1]:443",
        "host]:443",
    };

    for (const char* invalid_upstream : invalid_upstreams) {
        SCOPED_TRACE(invalid_upstream);
        auto result = parseArguments({"--upstream", invalid_upstream});

        ASSERT_FALSE(result);
        EXPECT_NE(0, result.error().exit_code);
    }
}

TEST_F(ConfigTest, ConnectTimeoutDefaultsAndBoundaries) {
    auto defaults = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(10000, defaults->timeouts.connect.count());
    for (auto ms : {1, 2147483647}) {
        auto result =
            parseArguments({"--upstream", "backend:1", "--connect-timeout-ms", std::to_string(ms)});
        ASSERT_TRUE(result);
        EXPECT_EQ(ms, result->timeouts.connect.count());
    }
}

TEST_F(ConfigTest, RejectsInvalidConnectTimeoutFromCliOrEnvironment) {
    for (auto invalid : {"0", "-1", "1s", "0.5", "2147483648", "999999999999999999999"}) {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1", "--connect-timeout-ms", invalid}));
        ASSERT_EQ(0, setenv("ORBIT_CONNECT_TIMEOUT_MS", invalid, 1));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1"}));
    }
    auto override = parseArguments({"--upstream", "backend:1", "--connect-timeout-ms", "7"});
    ASSERT_TRUE(override);
    EXPECT_EQ(7, override->timeouts.connect.count());
    ASSERT_EQ(0, setenv("ORBIT_CONNECT_TIMEOUT_MS", "17", 1));
    auto env = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(env);
    EXPECT_EQ(17, env->timeouts.connect.count());
}

TEST_F(ConfigTest, IdleTimeoutDefaultsBoundariesAndEnvironment) {
    auto defaults = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(0, defaults->timeouts.idle.count());
    ASSERT_EQ(0, setenv("ORBIT_IDLE_TIMEOUT_MS", "23000", 1));
    auto env = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(env);
    EXPECT_EQ(23000, env->timeouts.idle.count());
    for (auto ms : {0, 1, 2147483647}) {
        auto result =
            parseArguments({"--upstream", "backend:1", "--idle-timeout-ms", std::to_string(ms)});
        ASSERT_TRUE(result);
        EXPECT_EQ(ms, result->timeouts.idle.count());
    }
    for (auto invalid : {"-1", "0.1", "1ms", "2147483648", "999999999999999999999"}) {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1", "--idle-timeout-ms", invalid}));
        ASSERT_EQ(0, setenv("ORBIT_IDLE_TIMEOUT_MS", invalid, 1));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1"}));
    }
}

TEST_F(ConfigTest, ValidatesDirectionalWatermarksAfterCliAndEnvironmentSelection) {
    const auto defaults = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(262144u, defaults->resources.buffer_high_watermark);
    EXPECT_EQ(196608u, defaults->resources.buffer_low_watermark);

    ASSERT_EQ(0, setenv("ORBIT_BUFFER_HIGH_WATERMARK_BYTES", "4096", 1));
    ASSERT_EQ(0, setenv("ORBIT_BUFFER_LOW_WATERMARK_BYTES", "0", 1));
    const auto minimum = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(minimum);
    EXPECT_EQ(4096u, minimum->resources.buffer_high_watermark);
    EXPECT_EQ(0u, minimum->resources.buffer_low_watermark);

    for (auto invalid : {"", "-1", "4095", "4096B", "16777217", "99999999999999999999999"}) {
        SCOPED_TRACE(invalid);
        ASSERT_EQ(0, setenv("ORBIT_BUFFER_HIGH_WATERMARK_BYTES", invalid, 1));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1"}));
        EXPECT_FALSE(
            parseArguments({"--upstream", "backend:1", "--buffer-high-watermark-bytes", invalid}));
    }
    ASSERT_EQ(0, setenv("ORBIT_BUFFER_LOW_WATERMARK_BYTES", "16777216", 1));
    EXPECT_FALSE(
        parseArguments({"--upstream", "backend:1", "--buffer-high-watermark-bytes", "16777216"}));

    const auto maximum = parseArguments({"--upstream", "backend:1", "--buffer-high-watermark-bytes",
                                         "16777216", "--buffer-low-watermark-bytes", "16777215"});
    ASSERT_TRUE(maximum);
    EXPECT_EQ(16777216u, maximum->resources.buffer_high_watermark);
    EXPECT_EQ(16777215u, maximum->resources.buffer_low_watermark);
}

TEST_F(ConfigTest, ValidatesTotalAndPendingAdmissionLimits) {
    const auto defaults = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(256u, defaults->resources.max_connections);
    EXPECT_EQ(64u, defaults->resources.max_pending_connections);

    ASSERT_EQ(0, setenv("ORBIT_MAX_CONNECTIONS", "2", 1));
    ASSERT_EQ(0, setenv("ORBIT_MAX_PENDING_CONNECTIONS", "3", 1));
    EXPECT_FALSE(parseArguments({"--upstream", "backend:1"}));
    for (auto invalid : {"", "-1", "0", "65536", "9999999999999999999999"}) {
        SCOPED_TRACE(invalid);
        ASSERT_EQ(0, setenv("ORBIT_MAX_CONNECTIONS", invalid, 1));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1", "--max-pending-connections", "1"}));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1", "--max-connections", "2",
                                     "--max-pending-connections", invalid}));
    }
    const auto validated = parseArguments(
        {"--upstream", "backend:1", "--max-connections", "2", "--max-pending-connections", "1"});
    ASSERT_TRUE(validated);
    EXPECT_EQ(2u, validated->resources.max_connections);
    EXPECT_EQ(1u, validated->resources.max_pending_connections);
}

TEST_F(ConfigTest, ValidatesGlobalBufferBudgetAndAllocationGranularity) {
    const auto defaults = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(defaults);
    EXPECT_EQ(67108864u, defaults->resources.buffer_budget);
    for (auto invalid : {"", "0", "4095", "4097", "1073745920", "9999999999999999999999"}) {
        SCOPED_TRACE(invalid);
        ASSERT_EQ(0, setenv("ORBIT_BUFFER_BUDGET_BYTES", invalid, 1));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1"}));
        EXPECT_FALSE(parseArguments({"--upstream", "backend:1", "--buffer-budget-bytes", invalid}));
    }
    const auto minimum =
        parseArguments({"--upstream", "backend:1", "--buffer-budget-bytes", "4096"});
    ASSERT_TRUE(minimum);
    EXPECT_EQ(4096u, minimum->resources.buffer_budget);
    ASSERT_EQ(0, setenv("ORBIT_BUFFER_BUDGET_BYTES", "1073741824", 1));
    const auto maximum = parseArguments({"--upstream", "backend:1"});
    ASSERT_TRUE(maximum);
    EXPECT_EQ(1073741824u, maximum->resources.buffer_budget);
}
