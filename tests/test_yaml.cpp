// Copyright (c) 2026 V. K. Safronov

#include "doctest.h"
#include "yaml_parser.h"

#include <string>

// Note on path syntax (matches real usage in src/iracing/iracing.cpp):
// every key segment in the path must be terminated with ':', and array
// elements are addressed by indexing a sibling key's value with "{N}",
// e.g. "Drivers:CarIdx:{1}UserName:" finds the array entry whose CarIdx
// equals 1, then reads its UserName field.

TEST_CASE("parseYaml: simple key-value pair")
{
    const char* yaml = "key: value\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "key:", &val, &len));
    REQUIRE(val != nullptr);
    CHECK(std::string(val, len) == "value");
}

TEST_CASE("parseYaml: nested path")
{
    const char* yaml =
        "DriverInfo:\n"
        "  DriverCarIdx: 3\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "DriverInfo:DriverCarIdx:", &val, &len));
    REQUIRE(val != nullptr);
    CHECK(std::string(val, len) == "3");
}

TEST_CASE("parseYaml: array indexing with {N}")
{
    const char* yaml =
        "Drivers:\n"
        "  - CarIdx: 0\n"
        "    UserName: Alice\n"
        "  - CarIdx: 1\n"
        "    UserName: Bob\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "Drivers:CarIdx:{1}UserName:", &val, &len));
    REQUIRE(val != nullptr);
    CHECK(std::string(val, len) == "Bob");
}

TEST_CASE("parseYaml: nonexistent path returns false and clears val/len")
{
    const char* yaml = "key: value\n";
    const char* val = (const char*)0x1; // sentinel to verify it gets reset
    int len = 42;

    CHECK_FALSE(parseYaml(yaml, "nope:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("parseYaml: null arguments do not crash and return false")
{
    const char* val = nullptr;
    int len = 0;

    CHECK_FALSE(parseYaml(nullptr, "key:", &val, &len));
    CHECK_FALSE(parseYaml("key: value", nullptr, &val, &len));
    CHECK_FALSE(parseYaml("key: value", "key:", nullptr, &len));
    CHECK_FALSE(parseYaml("key: value", "key:", &val, nullptr));
}

TEST_CASE("parseYaml: CRLF line endings")
{
    const char* yaml = "key: value\r\nother: 1\r\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "key:", &val, &len));
    REQUIRE(val != nullptr);
    CHECK(std::string(val, len) == "value");
}
