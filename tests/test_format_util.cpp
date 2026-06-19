// Copyright (c) 2026 V. K. Safronov

#include "doctest.h"
#include "format_util.h"

#include <cstring>

TEST_CASE("formatLaptime: zero seconds")
{
    CHECK(formatLaptime(0.0f) == "0.000");
}

TEST_CASE("formatLaptime: sub-minute value just below the minute boundary")
{
    CHECK(formatLaptime(59.999f) == "59.999");
}

TEST_CASE("formatLaptime: exactly 60 seconds crosses into minutes formatting")
{
    // At secs == 60.0f, mins == 1, so the "m:ss.mmm" branch is taken,
    // and fmodf(60.0f, 60.0f) == 0.0f.
    CHECK(formatLaptime(60.0f) == "1:00.000");
}

TEST_CASE("formatLaptime: large value with minutes and seconds")
{
    CHECK(formatLaptime(125.5f) == "2:05.500");
}

TEST_CASE("formatLaptime: negative value below one minute in magnitude (regression anchor)")
{
    // Regression anchor: documents ACTUAL current behavior, not "correct" behavior.
    // int(-5.0f/60.0f) truncates toward zero -> mins == 0 -> falls into the
    // no-minutes branch, which simply formats the raw (negative) seconds.
    CHECK(formatLaptime(-5.0f) == "-5.000");
}

TEST_CASE("formatLaptime: negative value with magnitude over one minute (regression anchor)")
{
    // Regression anchor: documents ACTUAL current behavior, not "correct" behavior.
    // int(-65.0f/60.0f) == -1 (truncation toward zero) which is truthy, so the
    // minutes branch is taken with a negative "minutes" value, and fmodf carries
    // the sign of the dividend, producing a negative "seconds" part too.
    CHECK(formatLaptime(-65.0f) == "-1:-5.000");
}

TEST_CASE("celsiusToFahrenheit: known reference points")
{
    CHECK(celsiusToFahrenheit(0.0f) == doctest::Approx(32.0f));
    CHECK(celsiusToFahrenheit(100.0f) == doctest::Approx(212.0f));
    CHECK(celsiusToFahrenheit(-40.0f) == doctest::Approx(-40.0f));
}

TEST_CASE("MurmurHash2: deterministic for identical inputs")
{
    const char* data = "iRon overlay";
    const unsigned int h1 = MurmurHash2(data, (int)strlen(data), 0x12341234);
    const unsigned int h2 = MurmurHash2(data, (int)strlen(data), 0x12341234);
    CHECK(h1 == h2);
}

TEST_CASE("MurmurHash2: known value for empty input with seed 0")
{
    CHECK(MurmurHash2("", 0, 0) == 0u);
}
