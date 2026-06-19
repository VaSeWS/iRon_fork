// Copyright (c) 2026 V. K. Safronov

// Unit tests for the Delta overlay's dynamic vertical auto-scale math (delta_logic.h).
// These are regression anchors: they capture the *current* behavior of the extracted
// functions so the Phase 4b refactor is provably behavior-preserving.

// MSVC does not pull STL headers in transitively the way clang does -- include every
// standard header we touch explicitly, or CI's MSVC build fails with C2027.
#include <vector>
#include <algorithm>
#include <cmath>
#include <utility>

#include "doctest.h"
#include "delta_logic.h"

using delta::buildLadder;
using delta::pickTargetRung;
using delta::easeRange;
using delta::edgeCrossT;
using delta::signRuns;
using delta::STEP_MERGE_RATIO;

// ----------------------------------------------------------------------------------
// buildLadder
// ----------------------------------------------------------------------------------

TEST_CASE("buildLadder: invariants always hold (non-empty, ascending, front==floor)")
{
    const std::vector<float> steps = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };
    const float floor = 1.0f;
    const std::vector<float> ladder = buildLadder( steps, floor );

    REQUIRE( !ladder.empty() );
    CHECK( ladder.front() == doctest::Approx( floor ) );
    for( size_t i = 1; i < ladder.size(); ++i )
        CHECK( ladder[i] > ladder[i-1] );   // strictly ascending
}

TEST_CASE("buildLadder: floor drops rungs at or below it")
{
    // Default candidate set with floor 1.0: 0.25 and 0.5 are <= floor and dropped, and
    // 1.0 itself is dropped (steps[i] <= floor) -- the floor is re-inserted as front().
    const std::vector<float> steps = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };
    const std::vector<float> ladder = buildLadder( steps, 1.0f );

    // Expected: { 1, 1.5, 2, 3, 5, 10 } (1.5/1.0 = 1.5 >= STEP_MERGE_RATIO -> both kept).
    REQUIRE( ladder.size() == 6 );
    CHECK( ladder[0] == doctest::Approx( 1.0f ) );
    CHECK( ladder[1] == doctest::Approx( 1.5f ) );
    CHECK( ladder[2] == doctest::Approx( 2.0f ) );
    CHECK( ladder[3] == doctest::Approx( 3.0f ) );
    CHECK( ladder[4] == doctest::Approx( 5.0f ) );
    CHECK( ladder[5] == doctest::Approx( 10.0f ) );
    // No rung at or below the floor survived.
    for( float r : ladder )
        CHECK( r >= 1.0f );
}

TEST_CASE("buildLadder: keeps both when the gap is >= STEP_MERGE_RATIO")
{
    // floor=1, stepHigh=1.5 -> ratio 1.5 >= 1.2 -> keep both.
    const std::vector<float> steps = { 1.5f, 2.0f, 3.0f };
    const std::vector<float> ladder = buildLadder( steps, 1.0f );

    REQUIRE( ladder.size() == 4 );
    CHECK( ladder[0] == doctest::Approx( 1.0f ) );
    CHECK( ladder[1] == doctest::Approx( 1.5f ) );
    CHECK( ladder[2] == doctest::Approx( 2.0f ) );
    CHECK( ladder[3] == doctest::Approx( 3.0f ) );
}

TEST_CASE("buildLadder: merges the smallest rung when it is too close to the floor")
{
    // floor=1, stepHigh=1.07 -> ratio ~1.07 < 1.2 -> drop 1.07, keep only rungs > 1.07.
    const std::vector<float> steps = { 1.07f, 2.0f, 3.0f };
    const std::vector<float> ladder = buildLadder( steps, 1.0f );

    REQUIRE( ladder.size() == 3 );
    CHECK( ladder[0] == doctest::Approx( 1.0f ) );   // floor stands in for the lowest band
    CHECK( ladder[1] == doctest::Approx( 2.0f ) );
    CHECK( ladder[2] == doctest::Approx( 3.0f ) );
    // The merged-away rung is not present.
    for( float r : ladder )
        CHECK( r != doctest::Approx( 1.07f ) );
}

TEST_CASE("buildLadder: merge branch keeps rungs strictly above stepHigh only")
{
    // floor=1, candidates 1.1 (close, the stepHigh) and 1.15 (also < ratio but > stepHigh).
    // stepHigh=1.1, 1.1/1.0 = 1.1 < 1.2 -> merge branch. It keeps steps strictly > 1.1,
    // so 1.15 survives (even though 1.15/1.0 < ratio) and 1.1 itself is dropped.
    const std::vector<float> steps = { 1.1f, 1.15f, 2.0f };
    const std::vector<float> ladder = buildLadder( steps, 1.0f );

    REQUIRE( ladder.size() == 3 );
    CHECK( ladder[0] == doctest::Approx( 1.0f ) );
    CHECK( ladder[1] == doctest::Approx( 1.15f ) );
    CHECK( ladder[2] == doctest::Approx( 2.0f ) );
}

TEST_CASE("buildLadder: nothing survives above the floor -> single-rung ladder")
{
    // All candidates <= floor: ladder collapses to just { floor }.
    const std::vector<float> steps = { 0.25f, 0.5f, 1.0f };
    const std::vector<float> ladder = buildLadder( steps, 2.0f );

    REQUIRE( ladder.size() == 1 );
    CHECK( ladder[0] == doctest::Approx( 2.0f ) );
}

TEST_CASE("buildLadder: empty input -> single-rung ladder at the floor")
{
    const std::vector<float> steps;
    const std::vector<float> ladder = buildLadder( steps, 1.0f );

    REQUIRE( ladder.size() == 1 );
    CHECK( ladder[0] == doctest::Approx( 1.0f ) );
}

TEST_CASE("buildLadder: single element above the floor")
{
    const std::vector<float> steps = { 5.0f };
    const std::vector<float> ladder = buildLadder( steps, 1.0f );

    // 5.0/1.0 = 5 >= ratio -> keep both.
    REQUIRE( ladder.size() == 2 );
    CHECK( ladder[0] == doctest::Approx( 1.0f ) );
    CHECK( ladder[1] == doctest::Approx( 5.0f ) );
}

// ----------------------------------------------------------------------------------
// pickTargetRung
// ----------------------------------------------------------------------------------

TEST_CASE("pickTargetRung: amplitude below the floor -> floor rung")
{
    const std::vector<float> ladder = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };
    // Tiny amplitude, no ghost, default-ish headroom -> needed << floor -> first rung.
    CHECK( pickTargetRung( ladder, 0.01f, 0.0f, 0.08f ) == doctest::Approx( 1.0f ) );
    // Zero amplitude likewise.
    CHECK( pickTargetRung( ladder, 0.0f, 0.0f, 0.08f ) == doctest::Approx( 1.0f ) );
}

TEST_CASE("pickTargetRung: between rungs picks the next rung up")
{
    const std::vector<float> ladder = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };
    // amplitude 1.6, no headroom -> needed 1.6 -> smallest rung >= 1.6 is 2.0.
    CHECK( pickTargetRung( ladder, 1.6f, 0.0f, 0.0f ) == doctest::Approx( 2.0f ) );
    // amplitude 1.4 -> needed 1.4 -> smallest rung >= 1.4 is 1.5.
    CHECK( pickTargetRung( ladder, 1.4f, 0.0f, 0.0f ) == doctest::Approx( 1.5f ) );
}

TEST_CASE("pickTargetRung: headroom can bump the choice to the next rung up")
{
    const std::vector<float> ladder = { 1.0f, 1.5f, 2.0f, 3.0f };
    // amplitude exactly 1.5: with no headroom it fits rung 1.5 ...
    CHECK( pickTargetRung( ladder, 1.5f, 0.0f, 0.0f ) == doctest::Approx( 1.5f ) );
    // ... but with 8% headroom needed = 1.62 -> bumps to rung 2.0.
    CHECK( pickTargetRung( ladder, 1.5f, 0.0f, 0.08f ) == doctest::Approx( 2.0f ) );
}

TEST_CASE("pickTargetRung: above the top rung clips to the top")
{
    const std::vector<float> ladder = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };
    CHECK( pickTargetRung( ladder, 50.0f, 0.0f, 0.08f ) == doctest::Approx( 10.0f ) );
    // Even amplitude exactly at the top, plus headroom -> still clipped to the top.
    CHECK( pickTargetRung( ladder, 10.0f, 0.0f, 0.08f ) == doctest::Approx( 10.0f ) );
}

TEST_CASE("pickTargetRung: ghost amplitude is taken into account via max()")
{
    const std::vector<float> ladder = { 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 10.0f };
    // Live lap small (0.2) but the best-ghost peaks at 2.8 -> needed = 2.8 -> rung 3.0.
    CHECK( pickTargetRung( ladder, 0.2f, 2.8f, 0.0f ) == doctest::Approx( 3.0f ) );
    // The larger of the two amplitudes wins.
    CHECK( pickTargetRung( ladder, 4.0f, 1.0f, 0.0f ) == doctest::Approx( 5.0f ) );
}

TEST_CASE("pickTargetRung: single-rung ladder always returns that rung")
{
    const std::vector<float> ladder = { 2.0f };
    CHECK( pickTargetRung( ladder, 0.1f, 0.0f, 0.08f ) == doctest::Approx( 2.0f ) );
    CHECK( pickTargetRung( ladder, 100.0f, 0.0f, 0.08f ) == doctest::Approx( 2.0f ) );
}

// ----------------------------------------------------------------------------------
// easeRange
// ----------------------------------------------------------------------------------

TEST_CASE("easeRange: moves monotonically toward the target (up)")
{
    float range = 1.0f;
    const float target = 5.0f;
    float prev = range;
    for( int i = 0; i < 200; ++i )
    {
        range = easeRange( range, target, 1.0f, false );
        CHECK( range >= prev );          // never moves away from the target
        CHECK( range <= target );        // never overshoots
        prev = range;
    }
    CHECK( range == doctest::Approx( target ).epsilon( 0.001 ) );
}

TEST_CASE("easeRange: moves monotonically toward the target (down)")
{
    float range = 5.0f;
    const float target = 1.0f;
    float prev = range;
    for( int i = 0; i < 500; ++i )
    {
        range = easeRange( range, target, 1.0f, false );
        CHECK( range <= prev );          // never moves away from the target
        CHECK( range >= target );        // never undershoots
        prev = range;
    }
    CHECK( range == doctest::Approx( target ).epsilon( 0.001 ) );
}

TEST_CASE("easeRange: a single up step uses kUp = 0.09 at default speed")
{
    // range 1 -> target 2: step = (2-1)*0.09 = 0.09 -> 1.09.
    const float r = easeRange( 1.0f, 2.0f, 1.0f, false );
    CHECK( r == doctest::Approx( 1.09f ) );
}

TEST_CASE("easeRange: a single down step uses kDown = 0.027 at default speed")
{
    // range 2 -> target 1: step = (1-2)*0.027 = -0.027 -> 1.973.
    const float r = easeRange( 2.0f, 1.0f, 1.0f, false );
    CHECK( r == doctest::Approx( 1.973f ) );
}

TEST_CASE("easeRange: up is faster than down for the same distance")
{
    const float upStep   = easeRange( 1.0f, 2.0f, 1.0f, false ) - 1.0f;   // +0.09
    const float downStep = 2.0f - easeRange( 2.0f, 1.0f, 1.0f, false );   // +0.027 magnitude
    CHECK( upStep > downStep );
}

TEST_CASE("easeRange: 30Hz compensation gives a bigger step than 60Hz")
{
    // Same direction (up), same distance: halfRate must cover more ground per frame.
    const float step60 = easeRange( 1.0f, 2.0f, 1.0f, false ) - 1.0f;
    const float step30 = easeRange( 1.0f, 2.0f, 1.0f, true )  - 1.0f;
    CHECK( step30 > step60 );

    // Exact compensated factor: kUp' = 1 - (1-0.09)^2 = 0.1719 -> step = 0.1719.
    CHECK( step30 == doctest::Approx( 0.1719f ) );
}

TEST_CASE("easeRange: 30Hz compensation applies to the down step too")
{
    const float step60 = 2.0f - easeRange( 2.0f, 1.0f, 1.0f, false );
    const float step30 = 2.0f - easeRange( 2.0f, 1.0f, 1.0f, true );
    CHECK( step30 > step60 );
    // kDown' = 1 - (1-0.027)^2 = 0.053271 -> step = 0.053271.
    CHECK( step30 == doctest::Approx( 0.053271f ) );
}

TEST_CASE("easeRange: rescaleSpeed clamps k to <= 1 (no overshoot past target)")
{
    // kUp = min(1, 0.09*10 = 0.9) = 0.9 -> NOT yet clamped: step = (5-1)*0.9 = 3.6 -> 4.6.
    const float r = easeRange( 1.0f, 5.0f, 10.0f, false );
    CHECK( r == doctest::Approx( 4.6f ) );

    // 0.09 * 20 = 1.8 -> clamps to 1.0 -> single-step exactly to target (no overshoot).
    const float r2 = easeRange( 1.0f, 5.0f, 20.0f, false );
    CHECK( r2 == doctest::Approx( 5.0f ) );

    // Down branch clamp: 0.027 * 50 = 1.35 -> clamps to 1.0 -> lands exactly on target.
    const float r3 = easeRange( 5.0f, 1.0f, 50.0f, false );
    CHECK( r3 == doctest::Approx( 1.0f ) );
}

TEST_CASE("easeRange: at target stays at target (zero step)")
{
    // target == range: down branch (target > range is false), step = 0.
    CHECK( easeRange( 3.0f, 3.0f, 1.0f, false ) == doctest::Approx( 3.0f ) );
    CHECK( easeRange( 3.0f, 3.0f, 1.0f, true )  == doctest::Approx( 3.0f ) );
}

// ----------------------------------------------------------------------------------
// edgeCrossT
// ----------------------------------------------------------------------------------

TEST_CASE("edgeCrossT: midpoint crossing through zero")
{
    // Symmetric segment -10 -> 10 crossing E=0: lands exactly halfway.
    CHECK( edgeCrossT( -10.0f, 10.0f, 0.0f ) == doctest::Approx( 0.5f ) );
}

TEST_CASE("edgeCrossT: leaving the band upward (in -> out at +R)")
{
    // d0=5 inside, d1=15 outside, edge E=R=10: t=(10-5)/(15-5)=0.5.
    CHECK( edgeCrossT( 5.0f, 15.0f, 10.0f ) == doctest::Approx( 0.5f ) );
    // Asymmetric: d0=2 -> d1=12 against E=10: t=(10-2)/(12-2)=0.8.
    CHECK( edgeCrossT( 2.0f, 12.0f, 10.0f ) == doctest::Approx( 0.8f ) );
}

TEST_CASE("edgeCrossT: entering the band (out -> in) is symmetric to leaving")
{
    // d0=15 outside, d1=5 inside, E=10: t=(10-15)/(5-15)=0.5.
    CHECK( edgeCrossT( 15.0f, 5.0f, 10.0f ) == doctest::Approx( 0.5f ) );
}

TEST_CASE("edgeCrossT: crossing the negative edge -R")
{
    // d0=-5 inside, d1=-15 outside, E=-R=-10: t=(-10-(-5))/(-15-(-5))=(-5)/(-10)=0.5.
    CHECK( edgeCrossT( -5.0f, -15.0f, -10.0f ) == doctest::Approx( 0.5f ) );
}

TEST_CASE("edgeCrossT: t reconstructs the edge value (d0 + (d1-d0)*t == E)")
{
    const float d0 = 3.0f, d1 = 41.0f, E = 10.0f;
    const float t  = edgeCrossT( d0, d1, E );
    CHECK( d0 + (d1 - d0) * t == doctest::Approx( E ) );
}

TEST_CASE("edgeCrossT: endpoints give t=0 and t=1")
{
    CHECK( edgeCrossT( 10.0f, 20.0f, 10.0f ) == doctest::Approx( 0.0f ) );  // E==d0
    CHECK( edgeCrossT( 10.0f, 20.0f, 20.0f ) == doctest::Approx( 1.0f ) );  // E==d1
}

TEST_CASE("edgeCrossT: result is clamped to [0,1] for out-of-segment edges")
{
    CHECK( edgeCrossT( 0.0f, 2.0f, 10.0f ) == doctest::Approx( 1.0f ) );   // raw t=5 -> clamp 1
    CHECK( edgeCrossT( 0.0f, 2.0f, -4.0f ) == doctest::Approx( 0.0f ) );   // raw t=-2 -> clamp 0
}

TEST_CASE("edgeCrossT: zero-length segment (d0==d1) returns 0 instead of dividing")
{
    CHECK( edgeCrossT( 7.0f, 7.0f, 10.0f ) == doctest::Approx( 0.0f ) );
}

// ----------------------------------------------------------------------------------
// signRuns
// ----------------------------------------------------------------------------------

// Convenience: run-find over a whole vector via an index accessor lambda.
static std::vector<std::pair<int,int>> runsOf( const std::vector<float>& v )
{
    return signRuns( 0, (int)v.size() - 1, [&]( int i ){ return v[i]; } );
}

TEST_CASE("signRuns: all gain (values <= 0) is a single run")
{
    const std::vector<float> v = { -1.0f, -2.0f, 0.0f, -3.0f };  // 0 counts as gain
    const auto r = runsOf( v );
    REQUIRE( r.size() == 1 );
    CHECK( r[0] == std::make_pair( 0, 3 ) );
}

TEST_CASE("signRuns: all loss (values > 0) is a single run")
{
    const std::vector<float> v = { 1.0f, 2.0f, 3.0f };
    const auto r = runsOf( v );
    REQUIRE( r.size() == 1 );
    CHECK( r[0] == std::make_pair( 0, 2 ) );
}

TEST_CASE("signRuns: alternating signs split into singleton runs")
{
    const std::vector<float> v = { 1.0f, -1.0f, 1.0f, -1.0f };
    const auto r = runsOf( v );
    REQUIRE( r.size() == 4 );
    CHECK( r[0] == std::make_pair( 0, 0 ) );
    CHECK( r[1] == std::make_pair( 1, 1 ) );
    CHECK( r[2] == std::make_pair( 2, 2 ) );
    CHECK( r[3] == std::make_pair( 3, 3 ) );
}

TEST_CASE("signRuns: mixed runs are maximal")
{
    const std::vector<float> v = { 2.0f, 1.0f, -1.0f, -2.0f, -3.0f, 5.0f };
    const auto r = runsOf( v );
    REQUIRE( r.size() == 3 );
    CHECK( r[0] == std::make_pair( 0, 1 ) );  // loss
    CHECK( r[1] == std::make_pair( 2, 4 ) );  // gain
    CHECK( r[2] == std::make_pair( 5, 5 ) );  // loss
}

TEST_CASE("signRuns: zero is grouped with the gain run (0 > 0 is false)")
{
    const std::vector<float> v = { 0.0f, -1.0f, 2.0f };
    const auto r = runsOf( v );
    REQUIRE( r.size() == 2 );
    CHECK( r[0] == std::make_pair( 0, 1 ) );  // {0,-1} both non-loss
    CHECK( r[1] == std::make_pair( 2, 2 ) );  // {2} loss
}

TEST_CASE("signRuns: single element yields one run")
{
    const std::vector<float> v = { -4.0f };
    const auto r = runsOf( v );
    REQUIRE( r.size() == 1 );
    CHECK( r[0] == std::make_pair( 0, 0 ) );
}

TEST_CASE("signRuns: inverted range (e < s) yields no runs")
{
    const std::vector<float> v = { 1.0f, -1.0f, 1.0f };
    const auto r = signRuns( 2, 1, [&]( int i ){ return v[i]; } );
    CHECK( r.empty() );
}

TEST_CASE("signRuns: respects a sub-range [s,e]")
{
    const std::vector<float> v = { 1.0f, -1.0f, -2.0f, 3.0f, 4.0f };
    const auto r = signRuns( 1, 3, [&]( int i ){ return v[i]; } );
    REQUIRE( r.size() == 2 );
    CHECK( r[0] == std::make_pair( 1, 2 ) );  // gain within sub-range
    CHECK( r[1] == std::make_pair( 3, 3 ) );  // loss
}

TEST_CASE("signRuns: a positive scale of the accessor does not change the runs")
{
    // The collapsing ghost feeds ghost[b]*cm with cm>0; runs must be identical to raw.
    const std::vector<float> v = { 2.0f, 1.0f, -1.0f, -3.0f, 5.0f };
    const auto raw    = signRuns( 0, (int)v.size()-1, [&]( int i ){ return v[i]; } );
    const auto scaled = signRuns( 0, (int)v.size()-1, [&]( int i ){ return v[i] * 0.25f; } );
    CHECK( raw == scaled );
}
