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
        constexpr std::array<const char*, 4> environment_variables{
            "ORBIT_LISTEN_HOST",
            "ORBIT_LISTEN_PORT",
            "ORBIT_UPSTREAMS",
            "ORBIT_LOG_LEVEL",
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
