// Copyright (c) 2026 V. K. Safronov

#include "doctest.h"
#include "yaml_parser.h"

#include <string>

// These tests document the ACTUAL observed behavior of the hand-rolled
// state-machine parser in third_party/irsdk/yaml_parser.cpp, including
// quirks/UB that are not "correct" YAML semantics but are load-bearing
// for callers in src/iracing/iracing.cpp. They are regression anchors,
// not a specification of desired behavior.

TEST_CASE("parseYaml: empty input returns false")
{
    const char* val = (const char*)0x1;
    int len = 42;

    CHECK_FALSE(parseYaml("", "key:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("parseYaml: path deeper than available data returns false")
{
    // "key:" exists at depth 0 with a scalar value, but the path asks
    // for a child key "sub:" underneath it, which never appears.
    const char* yaml = "key: value\n";
    const char* val = nullptr;
    int len = 0;

    CHECK_FALSE(parseYaml(yaml, "key:sub:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("parseYaml: exiting a nested block falls back to sibling at shallower depth")
{
    const char* yaml =
        "a:\n"
        "  b: 1\n"
        "c: 2\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "a:b:", &val, &len));
    CHECK(std::string(val, len) == "1");

    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(yaml, "c:", &val, &len));
    CHECK(std::string(val, len) == "2");
}

TEST_CASE("parseYaml: depth decreasing below pathdepth correctly fails the search")
{
    // Once we've matched into "outer:inner:" (pathdepth advances), if the
    // next line's depth drops back below pathdepth before the remaining
    // path segment "notexist:" is found, the parser must bail out with
    // false rather than continuing to scan past the block (it would
    // otherwise risk matching a same-named key in an unrelated sibling
    // block at shallower depth).
    const char* yaml =
        "outer:\n"
        "  inner:\n"
        "    deep: 1\n"
        "sibling: 2\n";
    const char* val = nullptr;
    int len = 0;

    CHECK_FALSE(parseYaml(yaml, "outer:inner:notexist:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("parseYaml: {N} filter matches the requested sibling value")
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
    CHECK(std::string(val, len) == "Bob");
}

TEST_CASE("parseYaml: {N} filter with no matching sibling value returns false")
{
    const char* yaml =
        "Drivers:\n"
        "  - CarIdx: 0\n"
        "    UserName: Alice\n"
        "  - CarIdx: 1\n"
        "    UserName: Bob\n";
    const char* val = nullptr;
    int len = 0;

    // CarIdx 5 never appears in the fixture.
    CHECK_FALSE(parseYaml(yaml, "Drivers:CarIdx:{5}UserName:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

// NOTE: We intentionally do NOT have a TEST_CASE that calls parseYaml()
// with an unclosed "{N" filter in `path`, because doing so reliably
// triggers a confirmed out-of-bounds read (see comment block below and
// .claude/bugs/known-issues.md). Exercising it here would crash the
// whole test binary under ASan/UBSan (and is genuine undefined
// behavior even without a sanitizer). The finding is documented as a
// comment instead of as an executable regression anchor, since there
// is no safe way to invoke the buggy code path and assert on its
// result without relying on UB.
//
// KNOWN BUG (candidate for .claude/bugs/known-issues.md):
// If the caller-supplied `path` contains an opening '{' for a
// {N}-style filter that is never closed with '}' before the path's
// NUL terminator, the "find closing brace" loop in yaml_parser.cpp
// (the `while(*(pathptr+pathvaluelen) && *(pathptr+pathvaluelen) !=
// '}') pathvaluelen++;` loop) stops at the path's NUL terminator, but
// in doing so it counts the NUL position itself as part of
// `pathvaluelen`. That makes `pathvaluelen - (keylen+1)` equal to the
// number of real characters after '{' PLUS ONE (off-by-one caused by
// the NUL being "consumed" by the loop's terminating check). If the
// matched value's length in `data` happens to equal that inflated
// count, the bogus "found" branch is taken and
// `pathptr += valuelen + 2` advances `pathptr` to a position at or
// past the path string's NUL terminator. All subsequent reads through
// `pathptr` (e.g. the `strncmp` against `keystr` on the very next
// line in the data, or any further '{' search) then read out of
// bounds past the end of the caller's `path` buffer.
//
// Confirmed via AddressSanitizer with path "Drivers:CarIdx:{0" against
// data containing "CarIdx: 0" (i.e. a single-digit, single-character
// value): global-buffer-overflow, out-of-bounds READ on the `path`
// string literal/buffer.
//
// This is NOT reachable through the current codebase, because every
// caller in src/iracing/iracing.cpp builds `path` via sprintf() with a
// matched "{%d}" pair. It is a latent robustness/security gap if a
// path is ever hand-built, truncated (e.g. by a too-small sprintf
// buffer), or otherwise corrupted before reaching parseYaml().

TEST_CASE("parseYaml: unclosed brace in DATA (not path) is handled safely")
{
    // A '{' appearing in the VALUE portion of the data (as opposed to
    // the path) is just an ordinary character consumed by the "value"
    // state and never causes any brace-matching logic to run; the
    // parser only inspects path syntax for '{'/'}'. This anchors that
    // malformed-looking data does not crash and is read as a literal
    // value.
    const char* yaml =
        "Drivers:\n"
        "  - CarIdx: {0\n"
        "    UserName: Alice\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "Drivers:CarIdx:", &val, &len));
    CHECK(std::string(val, len) == "{0");
}

TEST_CASE("parseYaml: value with internal/trailing spaces is NOT trimmed")
{
    const char* yaml = "key: value   \nother: 1\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "key:", &val, &len));
    // Regression anchor: trailing spaces before the newline are kept
    // verbatim in valuelen; the parser performs no trimming.
    CHECK(std::string(val, len) == "value   ");
}

TEST_CASE("parseYaml: multiple top-level keys selects the correct one")
{
    const char* yaml = "a: 1\nb: 2\nc: 3\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "b:", &val, &len));
    CHECK(std::string(val, len) == "2");

    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(yaml, "c:", &val, &len));
    CHECK(std::string(val, len) == "3");
}

TEST_CASE("parseYaml: value terminated by \\r\\n vs plain \\n")
{
    const char* yamlCrlf = "key: value\r\nother: 1\n";
    const char* yamlLf = "key: value\nother: 1\n";

    const char* val = nullptr;
    int len = 0;
    REQUIRE(parseYaml(yamlCrlf, "key:", &val, &len));
    CHECK(std::string(val, len) == "value");

    val = nullptr;
    len = 0;
    REQUIRE(parseYaml(yamlLf, "key:", &val, &len));
    CHECK(std::string(val, len) == "value");
}

TEST_CASE("parseYaml: final line without a trailing newline is not parsed")
{
    // Regression anchor / known limitation: the parser's main loop is
    // `while(*data)` and only evaluates a pending key/value pair when it
    // hits '\n' or '\r'. If the input ends without a trailing newline,
    // the last line's key/value pair is never committed, so a path that
    // only matches the last (newline-less) line is NOT found.
    const char* yaml = "key: value";
    const char* val = nullptr;
    int len = 0;

    CHECK_FALSE(parseYaml(yaml, "key:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("parseYaml: short key path does not falsely match a longer key with the same prefix")
{
    // "key:" must not match the unrelated key "keylonger".
    const char* yaml = "keylonger: 99\n";
    const char* val = nullptr;
    int len = 0;

    CHECK_FALSE(parseYaml(yaml, "key:", &val, &len));
    CHECK(val == nullptr);
    CHECK(len == 0);
}

TEST_CASE("parseYaml: longer key path is not confused by a shorter-prefixed sibling key")
{
    // With a short key "key:" appearing before the longer "keylonger:",
    // requesting "keylonger:" must still resolve to the correct entry
    // rather than stopping early at the "key" prefix match.
    const char* yaml = "key: 5\nkeylonger: 99\n";
    const char* val = nullptr;
    int len = 0;

    REQUIRE(parseYaml(yaml, "keylonger:", &val, &len));
    CHECK(std::string(val, len) == "99");
}
