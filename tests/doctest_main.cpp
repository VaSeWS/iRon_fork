// Copyright (c) 2026 V. K. Safronov
//
// Single translation unit that owns doctest's main(). All other test TUs
// must only #include "doctest.h" without defining DOCTEST_CONFIG_IMPLEMENT*.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
