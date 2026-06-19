/*
MIT License

Copyright (c) 2026 V. K. Safronov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

// Pure math for the Delta overlay's dynamic vertical auto-scale.
//
// This header is deliberately free of Win32 / Direct2D / DirectWrite: it depends
// only on the standard library, so the auto-scale logic is unit-testable with any
// compiler on any platform. OverlayDelta.h includes it and feeds member values in
// as plain arguments; nothing here touches overlay state or the render target.

#include <vector>
#include <algorithm>
#include <cmath>

namespace delta
{
    // Minimum floor->next-step ratio to keep both as distinct rungs (see buildLadder).
    inline constexpr float STEP_MERGE_RATIO = 1.2f;

    // Build the scale ladder once per config reload (no per-frame work). The floor
    // (minRange) is always the first rung; steps at or below it are dropped because
    // zooming tighter than the floor is intentionally disallowed. The first surviving
    // step (stepHigh) is either kept alongside the floor or merged into it, so we don't
    // end up with two near-identical bottom rungs the scale would pointlessly hop between:
    //   floor=1, stepHigh=1.5  -> ratio 1.5 >= 1.2 -> keep both: { 1, 1.5, ... }
    //   floor=1, stepHigh=1.07 -> ratio ~1.07 < 1.2 -> drop 1.07: { 1, <steps > 1.07> }
    //
    // `steps` is the user's candidate scale steps already parsed to positive floats and
    // sorted ascending (the parse/sort step stays in OverlayDelta::onConfigChanged because
    // it reads config strings). The returned ladder is guaranteed non-empty, ascending,
    // with front() == minRange.
    inline std::vector<float> buildLadder( const std::vector<float>& steps, float minRange )
    {
        std::vector<float> ladder;

        size_t i = 0;
        while( i < steps.size() && steps[i] <= minRange )   // (b) drop rungs <= floor
            ++i;
        if( i >= steps.size() )
        {
            // (c) nothing survives above the floor -> single-rung ladder.
            ladder.push_back( minRange );
        }
        else
        {
            const float stepHigh = steps[i];   // smallest rung strictly above the floor
            if( stepHigh / minRange >= STEP_MERGE_RATIO )
            {
                // Gap big enough: floor and stepHigh are distinct rungs, append the rest.
                ladder.push_back( minRange );
                for( size_t j = i; j < steps.size(); ++j )
                    ladder.push_back( steps[j] );
            }
            else
            {
                // stepHigh too close to the floor: replace it, keep only rungs strictly
                // above it (the floor stands in for that lowest band).
                ladder.push_back( minRange );
                for( size_t j = i; j < steps.size(); ++j )
                    if( steps[j] > stepHigh )
                        ladder.push_back( steps[j] );
            }
        }

        // Invariants relied on by onUpdate: non-empty, ascending, front() == floor.
        if( ladder.empty() )
            ladder.push_back( minRange );

        return ladder;
    }

    // Amplitude to fit: this lap's peak |delta|, and -- only when a "best" ghost is
    // shown -- that ghost's peak too, so the frozen ghost curve isn't clipped against a
    // scale chosen from the live lap alone. headroom lifts it off the top edge.
    //
    // Returns the smallest rung >= needed; if needed exceeds the top rung, clips to the
    // top (a brief over-range on an extreme spike is acceptable, see yOf's clamp).
    // `ladder` must be non-empty (buildLadder guarantees this).
    inline float pickTargetRung( const std::vector<float>& ladder, float lapMaxAbs,
                                 float ghostMaxAbs, float headroom )
    {
        const float needed = std::max( lapMaxAbs, ghostMaxAbs ) * (1.0f + headroom);

        // Smallest rung >= needed; if needed exceeds the top rung, clip to the top
        // (a brief over-range on an extreme spike is acceptable, see yOf's clamp).
        float target = ladder.back();
        for( float s : ladder ) { if( s >= needed ) { target = s; break; } }
        return target;
    }

    // Exponential per-frame approach of `range` toward `target`. Up is fast (a new peak
    // must not get clipped while the scale catches up); down is slow and calm (zoom-in
    // happens at the start of a lap, when the driver is busy with the entry to T1 and a
    // jumpy rescale would be distracting).
    //
    // The mechanism is purely per-frame (one update == one frame): no wall-clock dt and
    // no runtime exp() -- the coefficients are taus converted to per-frame easing
    // constants at ~60 Hz. `rescaleSpeed` scales the easing rate.
    //
    // 30Hz compensation (halfRate): this overlay eases only every other frame, so to
    // cover the same wall-clock distance per second we need each step to leave the same
    // residual two 60Hz steps would: (1-k')^1 == (1-k)^2  ->  k' = 1 - (1-k)^2. This
    // naturally stays <= 1 (no extra clamp needed) and degrades to ~2k for the small k here.
    //
    // Returns the new range value (caller assigns it back to m_range).
    inline float easeRange( float range, float target, float rescaleSpeed, bool halfRate )
    {
        float kUp   = std::min( 1.0f, 0.09f  * rescaleSpeed );
        float kDown = std::min( 1.0f, 0.027f * rescaleSpeed );

        if( halfRate )
        {
            kUp   = 1.0f - (1.0f - kUp)   * (1.0f - kUp);
            kDown = 1.0f - (1.0f - kDown) * (1.0f - kDown);
        }

        const float k = ( target > range ) ? kUp : kDown;
        return range + ( target - range ) * k;
    }
}
