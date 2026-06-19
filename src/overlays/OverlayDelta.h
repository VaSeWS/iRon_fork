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

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "Overlay.h"
#include "iracing.h"
#include "Config.h"
#include "OverlayDebug.h"
#include "delta_logic.h"

// Delta Trace overlay (core).
//
// Live visualization of the player's time delta versus a target lap, drawn across the
// whole lap by track distance (NOT by time, unlike OverlayInputs). The x axis is lap
// distance 0..1; the trace is rebuilt every lap. A vertical cursor marks the current
// position; everything to the left of it is the delta accumulated so far this lap.
//
// Sign convention (matches the RaceLab reference visual): positive delta = LOSING time
// vs the target -> trace rises ABOVE the zero line, drawn in the "loss" colour. Negative
// delta = GAINING -> trace drops BELOW the zero line, drawn in the "gain" colour.
//
// This core build implements the trace + header readouts (target lap time, current delta)
// and the target selector (best lap / session best). Ghost mode and the per-section delta
// bar are separate, later features.
class OverlayDelta : public Overlay
{
    public:

        const float DefaultFontSize = 16;

        OverlayDelta()
            : Overlay("OverlayDelta")
        {}

       #ifdef _DEBUG
       virtual bool    canEnableWhileNotDriving() const { return true; }
       virtual bool    canEnableWhileDisconnected() const { return true; }
       #endif

    protected:

        virtual float2 getDefaultSize()
        {
            return float2(600,150);
        }

        virtual void onEnable()
        {
            onConfigChanged();
        }

        virtual void onDisable()
        {
            m_text.reset();
        }

        virtual void onConfigChanged()
        {
            // Fonts
            m_text.reset( m_dwriteFactory.Get() );
            const std::string font = g_cfg.getString( m_name, "font", "Arial" );
            const float fontSize = g_cfg.getFloat( m_name, "font_size", DefaultFontSize );

            HRCHECK(m_dwriteFactory->CreateTextFormat( toWide(font).c_str(), NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", &m_textFormat ));
            m_textFormat->SetParagraphAlignment( DWRITE_PARAGRAPH_ALIGNMENT_CENTER );
            m_textFormat->SetWordWrapping( DWRITE_WORD_WRAPPING_NO_WRAP );

            HRCHECK(m_dwriteFactory->CreateTextFormat( toWide(font).c_str(), NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize*1.4f, L"en-us", &m_textFormatLarge ));
            m_textFormatLarge->SetParagraphAlignment( DWRITE_PARAGRAPH_ALIGNMENT_CENTER );
            m_textFormatLarge->SetWordWrapping( DWRITE_WORD_WRAPPING_NO_WRAP );

            // Cache styling/config that is invariant between config reloads, so onUpdate()
            // doesn't hit the config map (string hashing + float4 parsing) every frame.
            m_useBest   = (g_cfg.getString( m_name, "target", "session_best" ) == "best");
            m_thickness = g_cfg.getFloat( m_name, "line_thickness", 2.0f );
            m_gainFill  = g_cfg.getFloat4( m_name, "gain_fill_col", float4(0.18f,0.50f,0.16f,0.55f) );
            m_gainLine  = g_cfg.getFloat4( m_name, "gain_col",      float4(0.38f,0.91f,0.31f,0.90f) );
            m_lossFill  = g_cfg.getFloat4( m_name, "loss_fill_col", float4(0.55f,0.08f,0.10f,0.55f) );
            m_lossLine  = g_cfg.getFloat4( m_name, "loss_col",      float4(0.93f,0.20f,0.22f,0.90f) );
            m_zeroCol   = g_cfg.getFloat4( m_name, "zero_line_col", float4(1,1,1,0.25f) );
            m_cursorCol = g_cfg.getFloat4( m_name, "cursor_col",    float4(1,1,1,0.7f) );
            m_textCol   = g_cfg.getFloat4( m_name, "text_col",      float4(1,1,1,0.9f) );

            // --- Dynamic vertical auto-scale ------------------------------------------------
            // The trace no longer uses a fixed ±range. m_range is now an animated *current*
            // scale that tracks the lap's delta amplitude: each frame it eases toward the
            // smallest ladder step that still contains the running max |delta| (plus headroom).
            // delta_range_sec keeps its key but its meaning changes from "fixed scale" to the
            // MINIMUM scale (floor): the trace never zooms in tighter than this, so a near-zero
            // delta doesn't get amplified into visual noise. Lower clamp unchanged (>=0.05).
            m_minRange = std::max( 0.05f, g_cfg.getFloat( m_name, "delta_range_sec", 1.0f ) );

            // Headroom keeps the peak off the very top edge of the trace (e.g. 0.08 = +8%
            // before choosing a ladder step). rescale_speed scales the per-frame easing rate
            // (1.0 = the tuned default; <1 calmer, >1 snappier), clamped to a sane band.
            m_headroom     = std::max( 0.0f, g_cfg.getFloat( m_name, "range_headroom", 0.08f ) );
            m_rescaleSpeed = std::clamp( g_cfg.getFloat( m_name, "rescale_speed", 1.0f ), 0.1f, 10.0f );

            // In performance_mode_30hz each overlay updates every other frame (~30Hz), so the
            // per-frame easing constants below (kUp/kDown, tuned for ~60Hz) would otherwise reach
            // their target ~2x slower. Cache the flag here and compensate the factors in onUpdate.
            m_halfRate = g_cfg.getBool( "General", "performance_mode_30hz", false );

            // Parse the user's candidate scale steps: drop unparseable / non-positive entries
            // and sort ascending. These are the discrete "rungs" the animated scale can land on.
            std::vector<float> steps;
            {
                const std::vector<std::string> raw = g_cfg.getStringVec(
                    m_name, "range_steps",
                    std::vector<std::string>{ "0.25","0.5","1","1.5","2","3","5","10" } );
                steps.reserve( raw.size() );
                for( const std::string& s : raw )
                {
                    const float v = std::strtof( s.c_str(), nullptr );
                    if( v > 0.0f )
                        steps.push_back( v );
                }
                std::sort( steps.begin(), steps.end() );
            }

            // Build the scale ladder once per config reload (no per-frame work). The floor
            // (m_minRange) is always the first rung; steps at or below it are dropped, and the
            // smallest surviving step is merged into the floor if it is too close to it (see
            // delta::buildLadder for the full merge rule). Result: non-empty, ascending,
            // front() == m_minRange -- the invariants onUpdate relies on.
            m_ladder = delta::buildLadder( steps, m_minRange );

            // Bring the animated scale and amplitude trackers to a consistent starting state:
            // start zoomed all the way in at the floor; the first laps' peaks will ease it out.
            m_range          = m_minRange;
            m_lapMaxAbs      = 0.0f;
            m_ghostBestMaxAbs = 0.0f;

            // Ghost mode: "off" (default) | "last" | "best" | "session_best". "best" and
            // "session_best" are currently the same (see captureGhostCandidate for why).
            const std::string ghost = g_cfg.getString( m_name, "ghost", "last" );
            m_ghostMode      = (ghost=="last") ? GHOST_LAST : (ghost=="best" || ghost=="session_best") ? GHOST_BEST : GHOST_OFF;
            m_ghostCol       = g_cfg.getFloat4( m_name, "ghost_col", float4(1,1,1,0.55f) );
            m_ghostFill      = g_cfg.getFloat4( m_name, "ghost_fill_col", float4(1,1,1,0.12f) );
            m_ghostThickness = g_cfg.getFloat( m_name, "ghost_thickness", m_thickness );

            // New-best celebration: a "double breath" pulse (two quick attack/decay cycles plus a
            // slower final fade) of the center/zero line when a completed lap becomes the new
            // target (see captureGhostCandidate / the phase-machine block in onUpdate).
            m_pbPulse    = g_cfg.getBool  ( m_name, "new_best_pulse",     true );
            m_pbPulseCol = g_cfg.getFloat4( m_name, "new_best_pulse_col", float4(1.0f,0.80f,0.30f,0.85f) );

            // Dashed stroke style for the ghost trace. Created once here (not per frame) and
            // reused for every DrawGeometry call on the ghost path. Fields left at their D2D
            // defaults (cap/join/miter/dash offset) except dashStyle.
            m_ghostStrokeStyle.Reset();
            D2D1_STROKE_STYLE_PROPERTIES ghostStrokeProps = {};
            ghostStrokeProps.startCap   = D2D1_CAP_STYLE_FLAT;
            ghostStrokeProps.endCap     = D2D1_CAP_STYLE_FLAT;
            ghostStrokeProps.dashCap    = D2D1_CAP_STYLE_FLAT;
            ghostStrokeProps.lineJoin   = D2D1_LINE_JOIN_MITER;
            ghostStrokeProps.miterLimit = 10.0f;
            ghostStrokeProps.dashStyle  = D2D1_DASH_STYLE_DASH;
            ghostStrokeProps.dashOffset = 0.0f;
            HRCHECK( m_d2dFactory->CreateStrokeStyle( &ghostStrokeProps, nullptr, 0, &m_ghostStrokeStyle ) );

            // Section delta bar (bottom strip): splits the lap into the sim's official timing
            // sectors and colours each by time gained/lost; see onUpdate for the layout/colours.
            m_sectionBar         = g_cfg.getBool ( m_name, "section_bar", true );
            m_sectionBarHeight   = g_cfg.getFloat( m_name, "section_bar_height", 22.0f );
            m_showSectionValues  = g_cfg.getBool ( m_name, "show_section_values", true );
            m_sectionGainCol     = g_cfg.getFloat4( m_name, "section_gain_col",     float4(0.20f,0.62f,0.18f,0.90f) );
            m_sectionLossCol     = g_cfg.getFloat4( m_name, "section_loss_col",     float4(0.78f,0.16f,0.18f,0.90f) );
            m_sectionUndrivenCol = g_cfg.getFloat4( m_name, "section_undriven_col", float4(0.25f,0.25f,0.25f,0.55f) );

            // Distance-indexed trace buffers. m_resolution buckets span lap distance [0,1).
            // Reset the whole tracker: a config reload may have changed the resolution, and
            // any in-flight lap state would now be inconsistent with the new bucket count.
            // Clamp both ends: floor keeps the geometry loop sane, ceiling prevents a typo in
            // config.json from allocating gigabytes / walking a huge per-frame loop.
            m_resolution = std::clamp( g_cfg.getInt( m_name, "trace_resolution", 300 ), 16, 5000 );
            m_delta.assign( m_resolution, 0.0f );
            resetLap();
            m_sessionBestLapTime = 0.0f;
            m_lastLapCompleted = -1;

            // The resolution may have changed, which would invalidate any ghost snapshot taken
            // at the old bucket count (its size would no longer match m_resolution). Drop it.
            m_ghost.clear();
            m_ghostBest.clear();
            m_ghostBestMaxAbs = 0.0f;
            m_ghostBestLapTime = -1.0f;

            // A config reload is a hard discontinuity for the collapse/pulse animation too:
            // forget any in-flight collapse/pulse and the edge-detection baseline.
            m_ghostCollapse = 0.0f;
            m_ghostMerging = false;
            m_zeroHighlight = 0.0f;
            m_pbPhase = PB_IDLE;
            m_pbPhaseFrame = 0;
            m_prevTargetLapTime = -1.0f;
        }

        virtual void onSessionChanged()
        {
            // New session: discard the carried-over session-best reference and any in-flight lap.
            // m_lastLapCompleted must reset too, else the first frame of the new session (where
            // ir_LapCompleted restarts near 0) would fold the previous session's last lap time in.
            m_sessionBestLapTime = 0.0f;
            m_lastLapCompleted = -1;
            resetLap();
            // Zero the buffer too (parity with onConfigChanged): resetLap() only invalidates the
            // [min,max] range, but the section-bar subtraction reads m_delta directly, so a stale
            // half of the previous session's lap must not survive into the new one.
            std::fill( m_delta.begin(), m_delta.end(), 0.0f );

            // Ghost snapshots and the running best-lap-time tracker are session-scoped: a new
            // session means a new track/car/setup, so a previous ghost would no longer be a
            // meaningful comparison.
            m_ghost.clear();
            m_ghostBest.clear();
            m_ghostBestMaxAbs = 0.0f;
            m_ghostBestLapTime = -1.0f;

            // New session -> the collapse/pulse animation and its edge-detection baseline are
            // no longer meaningful (the ghost itself was just cleared above).
            m_ghostCollapse = 0.0f;
            m_ghostMerging = false;
            m_zeroHighlight = 0.0f;
            m_pbPhase = PB_IDLE;
            m_pbPhaseFrame = 0;
            m_prevTargetLapTime = -1.0f;

            // New session -> re-arm the dynamic scale: zoom back to the floor and forget the
            // previous session's amplitude so a stale peak can't keep the trace zoomed out.
            m_range     = m_minRange;
            m_lapMaxAbs = 0.0f;
        }

        virtual void onUpdate()
        {
            const float w = (float)m_width;
            const float h = (float)m_height;

            // --- Resolve the selected target into a delta channel + a target-lap time -------
            float delta = 0.0f;
            bool  deltaOk = false;
            if( m_useBest )
            {
                delta   = ir_LapDeltaToBestLap.getFloat();
                deltaOk = ir_LapDeltaToBestLap_OK.getBool();
            }
            else
            {
                delta   = ir_LapDeltaToSessionBestLap.getFloat();
                deltaOk = ir_LapDeltaToSessionBestLap_OK.getBool();
            }

            // --- Track the session-best lap time (used for the header readout) --------------
            // Source of truth: the min of our own observed completed laps, seeded/cross-checked
            // by the sim's per-car best (covers laps run before the overlay started). For the
            // "best lap" target we instead show the sim's own best-lap time directly.
            const int playerIdx = ir_session.driverCarIdx;
            if( playerIdx >= 0 && playerIdx < IR_MAX_CARS )
            {
                const float carBest = ir_CarIdxBestLapTime.getFloat( playerIdx );
                if( carBest > 0.0f && (m_sessionBestLapTime <= 0.0f || carBest < m_sessionBestLapTime) )
                    m_sessionBestLapTime = carBest;
            }
            const int lapCompleted = ir_LapCompleted.getInt();
            if( lapCompleted != m_lastLapCompleted )
            {
                m_lastLapCompleted = lapCompleted;
                const float last = ir_LapLastLapTime.getFloat();
                if( last > 0.0f && (m_sessionBestLapTime <= 0.0f || last < m_sessionBestLapTime) )
                    m_sessionBestLapTime = last;
            }

            const float targetLapTime = m_useBest ? ir_LapBestLapTime.getFloat() : m_sessionBestLapTime;

            // --- Advance the distance-indexed trace -----------------------------------------
            const float lapPct = std::clamp( ir_LapDistPct.getFloat(), 0.0f, 1.0f );

            // Detect a lap rollover by a large backwards jump in lap distance and clear the
            // current-lap trace so the new lap starts from a clean buffer. Assumes the only
            // >0.5 backward jump is the lap-start wrap (a mid-lap teleport/reset is rare and
            // would, acceptably, just restart the trace).
            if( m_lastLapPct >= 0.0f && lapPct < m_lastLapPct - 0.5f )
            {
                captureGhostCandidate();
                resetLap();
            }
            m_lastLapPct = lapPct;

            if( deltaOk )
            {
                // Track this lap's running peak |delta| from the RAW frame value (before the
                // bucket clamp/fill). Updated here, ahead of the scale computation below, so a
                // fresh peak lifts the scale's target in the SAME frame it appears -- the fast
                // up-easing then reacts immediately and the spike is never clipped.
                m_lapMaxAbs = std::max( m_lapMaxAbs, std::fabs( delta ) );

                int bucket = (int)( lapPct * m_resolution );
                if( bucket >= m_resolution ) bucket = m_resolution - 1;
                if( bucket < 0 )             bucket = 0;

                if( m_minBucket < 0 )   // first valid sample of this lap
                {
                    m_minBucket = bucket;
                    m_maxBucket = bucket;
                    m_delta[bucket] = delta;
                }
                else if( bucket > m_maxBucket )
                {
                    // Forward-fill buckets skipped since the last sample (frame hitch / high
                    // speed), linearly interpolating so the trace stays a smooth line rather
                    // than a vertical step across the gap.
                    const int   span = bucket - m_maxBucket;
                    const float prev = m_delta[m_maxBucket];
                    for( int b = m_maxBucket + 1; b <= bucket; ++b )
                        m_delta[b] = prev + (delta - prev) * float(b - m_maxBucket) / float(span);
                    m_maxBucket = bucket;
                }
                else if( bucket >= m_minBucket )
                {
                    // Backward jitter / same bucket within the already-drawn range: just refresh
                    // the value. Samples that fall BELOW m_minBucket (a spin before the lap's
                    // first sampled point) are ignored rather than corrupting the drawn range.
                    m_delta[bucket] = delta;
                }
            }

            // --- Render ---------------------------------------------------------------------
            // Reserve a bottom strip for the section bar when it is enabled and we actually have
            // sector data; otherwise the trace uses the full height (unchanged behavior). The
            // trace/ghost/zero-line all derive their geometry from center/halfSpan below, so
            // shrinking those to traceH is all that's needed to make room for the bar.
            const bool  showBar = m_sectionBar && !ir_session.sectorStartPct.empty();
            const float barH    = showBar ? std::min( m_sectionBarHeight, h*0.5f ) : 0.0f;
            const float traceH  = h - barH;
            const float center   = traceH * 0.5f;
            const float halfSpan = std::max( 1.0f, traceH * 0.5f - 2.0f );

            // --- Per-frame dynamic scale animation ------------------------------------------
            // Drive m_range toward the ladder rung that contains the current amplitude. Done
            // every frame (independent of deltaOk) so the scale keeps easing during gaps in
            // valid delta. The mechanism is purely per-frame (one update == one frame), matching
            // the moving elements in OverlayInputs: no wall-clock dt and no runtime exp() -- the
            // coefficients below are taus converted to per-frame easing constants at ~60 Hz.
            //
            // Amplitude to fit: this lap's peak |delta|, and -- only when a "best" ghost is
            // shown -- that ghost's peak too, so the frozen ghost curve isn't clipped against a
            // scale chosen from the live lap alone. headroom lifts it off the top edge.
            {
                // As the GHOST_BEST ghost merges onto the centre (m_ghostCollapse->1) it no longer
                // needs vertical range, so its contribution to the scale fades with it.
                const float ghostMaxAbs = (m_ghostMode == GHOST_BEST) ? m_ghostBestMaxAbs * (1.0f - m_ghostCollapse) : 0.0f;

                // Smallest rung that fits the amplitude (this lap's peak, plus the best-ghost
                // peak when shown) with headroom; clips to the top rung on an extreme spike.
                const float target = delta::pickTargetRung( m_ladder, m_lapMaxAbs, ghostMaxAbs, m_headroom );

                // Exponential per-frame approach toward that rung (fast up, slow/calm down,
                // 30Hz-compensated when m_halfRate). See delta::easeRange.
                m_range = delta::easeRange( m_range, target, m_rescaleSpeed, m_halfRate );
            }

            // --- Ghost collapse / new-best pulse animation -----------------------------------
            // Ease the ghost-collapse amount toward its target. Frame-driven like the scale
            // animation above (one update == one frame, ~60 Hz). k=0.08 settles the collapse in ~1 s.
            // captureGhostCandidate() sets the collapse target at lap rollovers.
            m_ghostCollapse += ( (m_ghostMerging ? 1.0f : 0.0f) - m_ghostCollapse ) * 0.08f;

            // New-best "double breath" pulse: a small per-frame phase machine drives m_zeroHighlight
            // through two quick attack/decay cycles followed by a slower final fade back to 0, instead
            // of a single instant pulse. All frame counts and easing rates below are pre-computed
            // constants tuned for ~60 Hz (no wall-clock dt, no runtime exp()/pow() in the hot path).
            // captureGhostCandidate() arms the machine (PB_IDLE -> PB_P1_ATTACK) on a genuine new best;
            // it free-runs to completion (or gets reset to PB_IDLE on config/session changes) from there.
            switch( m_pbPhase )
            {
                case PB_IDLE:
                    // Nothing to animate; m_zeroHighlight stays wherever it was left (0).
                    break;

                case PB_P1_ATTACK:
                    // Quick rise to the first peak: 6 frames, ease toward 1.0 at k=0.40.
                    m_zeroHighlight += ( 1.00f - m_zeroHighlight ) * 0.40f;
                    if( ++m_pbPhaseFrame >= 6 ) { m_pbPhase = PB_P1_DECAY; m_pbPhaseFrame = 0; }
                    break;

                case PB_P1_DECAY:
                    // First settle: 12 frames, ease toward a mid-level 0.35 at k=0.15 (doesn't fully
                    // die out -- this is the "inhale" before the second breath).
                    m_zeroHighlight += ( 0.35f - m_zeroHighlight ) * 0.15f;
                    if( ++m_pbPhaseFrame >= 12 ) { m_pbPhase = PB_P2_ATTACK; m_pbPhaseFrame = 0; }
                    break;

                case PB_P2_ATTACK:
                    // Second quick rise, identical shape to the first: 6 frames, ease toward 1.0 at k=0.40.
                    m_zeroHighlight += ( 1.00f - m_zeroHighlight ) * 0.40f;
                    if( ++m_pbPhaseFrame >= 6 ) { m_pbPhase = PB_P2_DECAY; m_pbPhaseFrame = 0; }
                    break;

                case PB_P2_DECAY:
                    // Final fade: ~33 frames, ease toward 0.0 at k=0.08 for a slow, calm afterglow.
                    // Once the phase's frame budget is spent, snap to idle and zero the highlight
                    // outright (the easing will have it very close to 0 by then regardless).
                    m_zeroHighlight += ( 0.00f - m_zeroHighlight ) * 0.08f;
                    if( ++m_pbPhaseFrame >= 33 ) { m_pbPhase = PB_IDLE; m_pbPhaseFrame = 0; m_zeroHighlight = 0.0f; }
                    break;
            }

            const float scale    = halfSpan / m_range;   // pixels per second

            auto xOf = [&]( int b )->float { return (float(b) + 0.5f) / float(m_resolution) * w; };
            auto yOf = [&]( float d )->float {
                // Clamp to ±m_range. With the dynamic scale this rarely bites -- only in the
                // brief frames where target > m_range while the up-easing is still catching up
                // to a sharp new peak -- but it still guards the trace from drawing off-frame.
                const float c = std::clamp( d, -m_range, m_range );
                return center - c * scale;          // d>0 (loss) -> above center, d<0 (gain) -> below
            };

            // Draws a polyline over buckets [s..e] using valAt(b) for the value, but only across
            // spans where |value| <= m_range. Where the trace leaves/enters the visible band it
            // is clipped exactly at the ±m_range boundary, so the line meets the fill edge
            // cleanly and then vanishes beyond it -- instead of being clamped (by yOf) into a
            // flat line pinned along the frame edge. strokeStyle may be null (live trace) or the
            // dashed ghost style. All in-band spans share one geometry (multiple figures),
            // matching the one-geometry-per-line-segment cost of the old code.
            auto drawClippedLine = [&]( int s, int e, auto&& valAt, const float4& col,
                                        float thick, ID2D1StrokeStyle* strokeStyle )
            {
                if( e < s ) return;
                const float R = m_range;

                Microsoft::WRL::ComPtr<ID2D1PathGeometry1> path;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink>  sink;
                HRCHECK( m_d2dFactory->CreatePathGeometry( &path ) );
                HRCHECK( path->Open( &sink ) );

                bool open = false;
                auto edgeCross = [&]( int b0, int b1, float E )->float2 {
                    // delta::edgeCrossT gives t in [0,1] where the d0->d1 segment hits edge E
                    // (one of +/-R); map that onto screen x. See delta_logic.h for the invariant.
                    const float t = delta::edgeCrossT( valAt(b0), valAt(b1), E );
                    return float2( xOf(b0) + (xOf(b1) - xOf(b0)) * t, yOf(E) );
                };

                if( std::fabs( valAt(s) ) <= R ) {            // first point in band: start a figure
                    sink->BeginFigure( float2(xOf(s), yOf(valAt(s))), D2D1_FIGURE_BEGIN_HOLLOW );
                    open = true;
                }
                for( int b = s+1; b <= e; ++b ) {
                    const float d0 = valAt(b-1), d1 = valAt(b);
                    const bool in0 = std::fabs(d0) <= R, in1 = std::fabs(d1) <= R;
                    if( in0 && in1 ) {
                        sink->AddLine( float2(xOf(b), yOf(d1)) );
                    } else if( in0 && !in1 ) {                // leaving band: clip to edge, close
                        const float E = (d1 > R) ? R : -R;
                        sink->AddLine( edgeCross(b-1, b, E) );
                        sink->EndFigure( D2D1_FIGURE_END_OPEN );
                        open = false;
                    } else if( !in0 && in1 ) {                // entering band: open at crossing
                        const float E = (d0 > R) ? R : -R;
                        sink->BeginFigure( edgeCross(b-1, b, E), D2D1_FIGURE_BEGIN_HOLLOW );
                        sink->AddLine( float2(xOf(b), yOf(d1)) );
                        open = true;
                    }
                    // both out of band: line vanishes, nothing emitted
                }
                if( open )
                    sink->EndFigure( D2D1_FIGURE_END_OPEN );
                HRCHECK( sink->Close() );

                m_brush->SetColor( col );
                m_renderTarget->DrawGeometry( path.Get(), m_brush.Get(), thick, strokeStyle );
            };

            // Fills the signed area between a trace and the centre line. Splits [s..e] into maximal runs of
            // one sign (each a simple polygon from the centre up/down to the curve) and fills each with
            // fillColorFor(loss). Shared by the live trace (gain/loss colours) and the ghost (one neutral
            // colour). yOf()'s clamp keeps a run pinned to the frame edge where the trace leaves the band.
            auto drawSignRunFills = [&]( int s, int e, auto&& valAt, auto&& fillColorFor )
            {
                // delta::signRuns splits [s,e] into maximal one-sign runs (the pure logic, unit
                // tested); each run becomes one filled polygon from the centre up/down to the curve.
                for( const auto& run : delta::signRuns( s, e, valAt ) )
                {
                    const int rs = run.first, re = run.second;
                    const bool loss = valAt(rs) > 0.0f;
                    Microsoft::WRL::ComPtr<ID2D1PathGeometry1> fillPath;
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink>  fillSink;
                    HRCHECK( m_d2dFactory->CreatePathGeometry( &fillPath ) );
                    HRCHECK( fillPath->Open( &fillSink ) );
                    fillSink->BeginFigure( float2(xOf(rs),center), D2D1_FIGURE_BEGIN_FILLED );
                    for( int b=rs; b<=re; ++b )
                        fillSink->AddLine( float2(xOf(b), yOf(valAt(b))) );
                    fillSink->AddLine( float2(xOf(re),center) );
                    fillSink->EndFigure( D2D1_FIGURE_END_CLOSED );
                    HRCHECK( fillSink->Close() );
                    m_brush->SetColor( fillColorFor(loss) );
                    m_renderTarget->FillGeometry( fillPath.Get(), m_brush.Get() );
                }
            };

            m_renderTarget->BeginDraw();

            // Zero (reference) line across the full width: the plain resting baseline, drawn UNDER
            // the ghost and trace (the trace fills run from each curve point down to this line).
            // The new-best gold celebration is deliberately NOT drawn here -- drawn under the trace
            // it would be overpainted by the fills exactly where the lap accumulated delta. It is
            // instead drawn ON TOP, after the trace, in the m_zeroHighlight block further below.
            const float hl = m_zeroHighlight;
            m_brush->SetColor( m_zeroCol );
            m_renderTarget->DrawLine( float2(0,center), float2(w,center), m_brush.Get(), 1.0f );

            // Ghost: a frozen full-lap delta curve from a previously completed lap (see
            // captureGhostCandidate). Drawn as a single dashed polyline across its whole
            // m_resolution range, after the zero line but before the live trace so the live
            // trace stays on top. Uses the same xOf()/yOf() mapping as the live trace, so a
            // perfectly repeated lap would draw the ghost exactly under the live line.
            const std::vector<float>& ghost = (m_ghostMode==GHOST_LAST) ? m_ghost
                                             : (m_ghostMode==GHOST_BEST) ? m_ghostBest
                                             : m_ghost /*unused, GHOST_OFF*/;
            // m_ghostCollapse==1 means the ghost's lap has become the target lap (a new best):
            // it is logically zero-delta vs itself, so the whole ghost has eased onto the
            // center line and merged with it (see captureGhostCandidate / the easing block
            // above). Skip the ghost entirely once fully merged -- there's nothing left to draw
            // that isn't already the (possibly pulsing) zero line.
            if( m_ghostMode != GHOST_OFF && (int)ghost.size() == m_resolution && m_ghostCollapse < 0.99f )
            {
                // cm ("collapse multiplier") shrinks every ghost sample toward the center line as
                // m_ghostCollapse goes 0->1. Applying it to ghost[b] BEFORE yOf() drives both the
                // fill (its height -> 0) and the line (drawn below) from the SAME scaled values,
                // keeping them perfectly synced with no separate opacity fade for the fill.
                // Sign-run splitting below stays on the RAW ghost[b]: multiplying by cm>0 never
                // changes its sign, so the runs are identical to the uncollapsed case.
                const float cm = 1.0f - m_ghostCollapse;

                // Ghost fill: split [0, m_resolution-1] into maximal runs of one sign (same
                // run-splitting the live trace uses below) and fill each run from the curve down
                // to the zero line, exactly like the live trace's per-run fill geometry. Unlike
                // the live trace, every run uses the SAME neutral colour (m_ghostFill) -- this is
                // a single monochrome fill, not a gain/loss split. Drawn before the ghost line so
                // the line stays on top of its own fill. yOf's clamp keeps the fill pinned to the
                // top/bottom edge where the ghost leaves the visible band, even though the line
                // itself vanishes there (see drawClippedLine below).
                drawSignRunFills( 0, m_resolution-1, [&](int b){ return ghost[b]*cm; }, [&](bool){ return m_ghostFill; } );

                // Ghost line: a single dashed polyline across its whole m_resolution range, after
                // the ghost fill and the zero line but before the live trace so the live trace
                // stays on top. Uses drawClippedLine so the line vanishes (rather than pinning
                // flat to the frame edge) wherever |ghost[b]*cm| > m_range. The line's alpha is
                // also faded by cm so it dissolves into the (possibly pulsing) zero line as it
                // collapses, rather than leaving a brighter double line on top of it.
                const float4 gcol = float4( m_ghostCol.r, m_ghostCol.g, m_ghostCol.b, m_ghostCol.a * cm );
                drawClippedLine( 0, m_resolution-1, [&](int b){ return ghost[b]*cm; }, gcol, m_ghostThickness, m_ghostStrokeStyle.Get() );
            }

            // Trace: split [m_minBucket, m_maxBucket] into maximal runs of one sign and draw a
            // fill (down to the zero line) plus a line per run, so the colour flips exactly at
            // each zero crossing. Same idea as the ABS-state runs in OverlayInputs.
            if( m_minBucket >= 0 && m_maxBucket >= m_minBucket )
            {
                // Fills: one per sign run, coloured by gain/loss.
                drawSignRunFills( m_minBucket, m_maxBucket, [&](int b){ return m_delta[b]; },
                                  [&](bool loss){ return loss ? m_lossFill : m_gainFill; } );

                // Lines: one clipped polyline per sign run, on top of the fills. Via
                // drawClippedLine so a line vanishes (rather than pinning flat to the frame edge)
                // wherever |m_delta[b]| > m_range.
                int s = m_minBucket;
                while( s <= m_maxBucket )
                {
                    const bool loss = m_delta[s] > 0.0f;
                    int e = s;
                    while( e + 1 <= m_maxBucket && (m_delta[e+1] > 0.0f) == loss )
                        ++e;
                    drawClippedLine( s, e, [&](int b){ return m_delta[b]; }, loss ? m_lossLine : m_gainLine, m_thickness, nullptr );
                    s = e + 1;
                }
            }

            // New-best gold celebration overlay. The "double breath" highlight (a wide low-alpha
            // halo glow plus a thicker gold core line) is drawn HERE, on top of the ghost and trace,
            // so it actually reads: drawn under the trace it would be overpainted by the fills (which
            // run from each curve point to the centre line) exactly where delta accumulated. hl is
            // driven by the phase machine in onUpdate; skipped while negligible (idle) so we don't add
            // DrawLine calls every frame for no visible effect.
            if( hl > 0.01f )
            {
                const float4 haloCol = float4( m_pbPulseCol.x, m_pbPulseCol.y, m_pbPulseCol.z, 0.16f*hl );
                m_brush->SetColor( haloCol );
                m_renderTarget->DrawLine( float2(0,center), float2(w,center), m_brush.Get(), 1.0f + 12.0f*hl );

                const float4 zc = lerp( m_zeroCol, m_pbPulseCol, hl );
                m_brush->SetColor( zc );
                m_renderTarget->DrawLine( float2(0,center), float2(w,center), m_brush.Get(), 1.0f + 1.2f*hl );
            }

            // Cursor at the current track position. Drawn independently of the trace so it is
            // visible from the first frame, even before any valid delta sample has landed.
            {
                const float xCur = lapPct * w;
                m_brush->SetColor( m_cursorCol );
                m_renderTarget->DrawLine( float2(xCur,0), float2(xCur,traceH), m_brush.Get(), 1.0f );
            }

            // Section delta bar: one box per official timing sector in the reserved bottom strip,
            // coloured by time gained/lost in that sector (cumulative delta at the sector's end
            // minus at its start, read from the current-lap buffer). Sectors not yet completed
            // this lap are greyed; on lap reset they all return to grey with the trace.
            if( showBar )
            {
                const std::vector<float>& sec = ir_session.sectorStartPct;
                const float y0  = traceH;
                const float gap = 2.0f;
                auto bucketOf = [&]( float pct )->int {
                    int b = (int)( pct * m_resolution );
                    if( b < 0 ) b = 0;
                    if( b >= m_resolution ) b = m_resolution - 1;
                    return b;
                };
                for( size_t i=0; i<sec.size(); ++i )
                {
                    const float startPct = sec[i];
                    const float endPct   = (i+1 < sec.size()) ? sec[i+1] : 1.0f;
                    if( endPct <= startPct )
                        continue;

                    const int startB = bucketOf( startPct );
                    const int endB   = bucketOf( endPct - 0.5f/float(m_resolution) );  // last bucket inside the sector

                    // "Driven" only once the trace has covered the WHOLE sector this lap (both
                    // its start and end buckets sampled). A sector only partially driven — e.g.
                    // the lap's first valid sample landed inside it — stays grey rather than
                    // showing a delta computed against a stale/zero baseline bucket.
                    const bool driven = (m_minBucket >= 0) && (startB >= m_minBucket) && (endB <= m_maxBucket);
                    float  secDelta = 0.0f;
                    float4 col      = m_sectionUndrivenCol;
                    if( driven )
                    {
                        secDelta = m_delta[endB] - m_delta[startB];
                        col = (secDelta <= 0.0f) ? m_sectionGainCol : m_sectionLossCol;
                    }

                    const float bx0 = startPct * w + gap*0.5f;
                    const float bx1 = endPct   * w - gap*0.5f;
                    if( bx1 <= bx0 )
                        continue;

                    D2D1_RECT_F rect = { bx0, y0 + gap, bx1, h - gap };
                    m_brush->SetColor( col );
                    m_renderTarget->FillRectangle( &rect, m_brush.Get() );

                    if( m_showSectionValues && driven )
                    {
                        char sbuf[32];
                        snprintf( sbuf, sizeof(sbuf), "%+.1f", secDelta );
                        m_brush->SetColor( m_textCol );
                        m_text.render( m_renderTarget.Get(), toWide(sbuf).c_str(), m_textFormat.Get(),
                                       bx0, bx1, (y0 + h)*0.5f, m_brush.Get(), DWRITE_TEXT_ALIGNMENT_CENTER );
                    }
                }
            }

            // Header readouts: target lap time (top-left), current delta (top-right).
            const float pad = 6.0f;
            const float ytext = m_textFormatLarge->GetFontSize();

            if( targetLapTime > 0.0f )
            {
                m_brush->SetColor( m_textCol );
                m_text.render( m_renderTarget.Get(), toWide(formatLaptime(targetLapTime)).c_str(),
                               m_textFormat.Get(), pad, w*0.5f, ytext, m_brush.Get(), DWRITE_TEXT_ALIGNMENT_LEADING );
            }

            if( deltaOk )
            {
                char buf[32];
                snprintf( buf, sizeof(buf), "%+.2f", delta );
                m_brush->SetColor( delta > 0.0f ? m_lossLine : m_gainLine );
                m_text.render( m_renderTarget.Get(), toWide(buf).c_str(),
                               m_textFormatLarge.Get(), w*0.5f, w-pad, ytext, m_brush.Get(), DWRITE_TEXT_ALIGNMENT_TRAILING );
            }

            m_renderTarget->EndDraw();
        }

    protected:

        void resetLap()
        {
            m_minBucket = -1;
            m_maxBucket = -1;
            m_lastLapPct = -1.0f;
            // New lap -> forget the previous lap's amplitude so the scale can ease back in if
            // the new lap is cleaner. m_range itself is left alone: it eases down on its own,
            // calmly, rather than snapping to the floor at every lap boundary.
            m_lapMaxAbs = 0.0f;
        }

        // Called right at lap rollover, BEFORE resetLap() wipes [m_minBucket, m_maxBucket].
        // Snapshots m_delta as the ghost candidate for the lap that just ended, if the trace
        // covered (almost) the whole lap and ghost mode is enabled.
        //
        // ir_LapLastLapTime is read here, at the same frame as the rollover; the sim is
        // expected to have already latched the completed lap's time by this point (the same
        // assumption the existing session-best tracker above relies on for ir_LapCompleted).
        // If the engine updates it a frame late, the ghost would briefly lag one lap behind
        // for the "best"/"session_best" selection -- cosmetic only, self-corrects next lap.
        void captureGhostCandidate()
        {
            if( m_ghostMode == GHOST_OFF )
                return;

            // Require near-full lap coverage so out-laps / in-laps / spins that only sampled a
            // fraction of the lap don't become the ghost. "Near" full because the very first
            // and last buckets of a lap are sometimes missed by a single sample (frame timing).
            const int minSpan = (int)( 0.9f * float(m_resolution) );
            if( m_minBucket < 0 || m_maxBucket < m_minBucket )
                return;
            const bool coversWholeLap = (m_minBucket <= (m_resolution - 1 - minSpan))   // m_minBucket close to 0
                                      && ((m_maxBucket - m_minBucket + 1) >= minSpan);   // and spans most of the lap
            if( !coversWholeLap )
                return;

            const float lapTime = ir_LapLastLapTime.getFloat();

            // Update the ghost snapshot for each mode -- both modes fall through to the shared
            // collapse/pulse detection below (neither early-returns), since either can become
            // the target lap.
            if( m_ghostMode == GHOST_LAST )
            {
                m_ghost = m_delta;          // full m_resolution-sized snapshot
            }
            else // GHOST_BEST ("best" / "session_best", see onConfigChanged comment)
            {
                // Keep the snapshot of the completed lap with the smallest lap time seen so far
                // this run.
                if( lapTime > 0.0f && (m_ghostBestLapTime <= 0.0f || lapTime < m_ghostBestLapTime) )
                {
                    m_ghostBestLapTime = lapTime;
                    m_ghostBest = m_delta;

                    // Cache the ghost's peak |delta| so onUpdate can keep the GHOST_BEST curve in
                    // frame: the dynamic scale fits both the live lap and this frozen ghost.
                    // Computed once here at capture (a full-lap snapshot is rare), not per frame.
                    float m = 0.0f;
                    for( float d : m_ghostBest )
                        m = std::max( m, std::fabs( d ) );
                    m_ghostBestMaxAbs = m;
                }
            }

            // --- Did the ghost just become the best? (collapse + optional pulse) --------------------
            // Definition: the ghost's lap time dropped strictly below the target's. Detected as -- this
            // just-completed lap (which the ghost shows: always in LAST mode, or as the freshly-updated
            // m_ghostBest in BEST mode) IS the current best AND strictly beat the best known BEFORE this lap.
            //   * Strict '<' (not '=='): a mere time-tie with a DIFFERENT lap must NOT collapse the ghost.
            //   * isCurrentBest (lap <= the live target): if the real best is a lap the overlay never
            //     captured (set before it was enabled, or any external best), the ghost stays shown normally
            //     until we actually beat it -- it is only "<= curTarget" once this lap really is the fastest.
            //   * Compared against m_prevTargetLapTime (the baseline as of the previous lap), because the
            //     live target may already have absorbed this lap, which would make a '< curTarget' test miss.
            {
                float curTarget = m_useBest ? ir_LapBestLapTime.getFloat() : m_sessionBestLapTime;
                // Skew guard: on this exact rollover frame the sim's best (ir_LapBestLapTime) or our
                // session-best tracker may not have folded THIS just-completed lap in yet -- and
                // ir_LapBestLapTime reads 0 before the very first lap latches. If this lap is at
                // least as fast as the tracked best, it IS the effective current best, so fold it in.
                // Without this a genuine first/best lap would have isCurrentBest==false and never
                // collapse (a permanent miss for GHOST_BEST, whose ghost then stays full-size while
                // actually being the target).
                if( lapTime > 0.0f && (curTarget <= 0.0f || lapTime < curTarget) )
                    curTarget = lapTime;
                const float EPS = 0.001f;   // 1 ms: lap times carry ms precision
                const bool isCurrentBest = lapTime > 0.0f && curTarget > 0.0f && lapTime <= curTarget + EPS;
                const bool becameBest    = isCurrentBest
                                         && ( m_prevTargetLapTime <= 0.0f || lapTime < m_prevTargetLapTime - EPS );
                const bool hadPriorBest  = m_prevTargetLapTime > 0.0f;

                // Advance the baseline to the best known now (this lap and/or any external best); monotonic.
                if( curTarget > 0.0f )
                    m_prevTargetLapTime = (m_prevTargetLapTime <= 0.0f) ? curTarget
                                                                        : std::min( m_prevTargetLapTime, curTarget );

                if( becameBest )
                {
                    // The ghost's lap is now the best == the target (zero vs itself): collapse it onto the
                    // centre. Replay from the full winning curve; pulse only for a genuine improvement over a
                    // PRIOR best (not the very first lap, which has no prior). Arming just starts the phase
                    // machine at its first attack -- it drives m_zeroHighlight itself from there (see the
                    // switch in onUpdate), so we don't touch m_zeroHighlight directly here.
                    m_ghostCollapse = 0.0f;
                    if( m_pbPulse && hadPriorBest )
                    {
                        m_pbPhase      = PB_P1_ATTACK;
                        m_pbPhaseFrame = 0;
                    }
                    m_ghostMerging = true;
                }
                else if( m_ghostMode == GHOST_LAST )
                {
                    // LAST mode: the ghost is THIS lap, and it is not the best -> show it normally.
                    // Snap (not ease) collapse to 0: this is a different lap than the one that may
                    // have just collapsed, so it appears immediately at full shape rather than
                    // easing up out of the centre line (which would imply a false continuity).
                    m_ghostCollapse = 0.0f;
                    m_ghostMerging  = false;
                }
                // BEST mode + not a new best: m_ghostBest is unchanged, so whether it is the target is
                // unchanged too -- leave m_ghostMerging as-is (stays merged if it already was).
            }
        }

        TextCache m_text;
        Microsoft::WRL::ComPtr<IDWriteTextFormat>  m_textFormat;
        Microsoft::WRL::ComPtr<IDWriteTextFormat>  m_textFormatLarge;

        // Cached config (populated in onConfigChanged).
        bool   m_useBest = false;
        float  m_thickness = 2.0f;
        float4 m_gainFill, m_gainLine, m_lossFill, m_lossLine;
        float4 m_zeroCol, m_cursorCol, m_textCol;

        // --- Dynamic vertical auto-scale -------------------------------------------------
        // m_range is the *current*, per-frame-animated vertical scale (±seconds shown), eased
        // toward a discrete rung of m_ladder that fits the live (and best-ghost) amplitude.
        // m_minRange is the floor rung from delta_range_sec; the scale never zooms tighter than
        // it. m_ladder is built once per config reload (ascending, front()==m_minRange).
        // m_lapMaxAbs / m_ghostBestMaxAbs are the running peak |delta| of the current lap and of
        // the best ghost, the two amplitudes the scale is fitted to.
        // (STEP_MERGE_RATIO and the ladder/rung/ease math now live in delta::, see delta_logic.h.)
        float  m_range = 1.0f;          // animated current scale
        float  m_minRange = 1.0f;       // floor (delta_range_sec)
        float  m_headroom = 0.08f;      // fraction of slack above the peak before picking a rung
        float  m_rescaleSpeed = 1.0f;   // multiplier on the per-frame easing rate
        bool   m_halfRate = false;      // true in performance_mode_30hz: this overlay updates every
                                        // other frame, so the per-frame easing constants (tuned for
                                        // ~60Hz) are compensated to keep the same wall-clock speed
        std::vector<float> m_ladder;    // candidate scale rungs, ascending, front()==m_minRange
        float  m_lapMaxAbs = 0.0f;      // running max |delta| this lap
        float  m_ghostBestMaxAbs = 0.0f;// max |delta| of m_ghostBest (GHOST_BEST scaling)

        // Distance-indexed delta trace for the current lap. m_delta has m_resolution buckets
        // spanning lap distance [0,1); [m_minBucket, m_maxBucket] is the contiguous range that
        // has been sampled so far this lap (both -1 before the first valid sample).
        std::vector<float> m_delta;
        int   m_resolution = 300;
        int   m_minBucket = -1;
        int   m_maxBucket = -1;
        float m_lastLapPct = -1.0f;

        // Session-best lap time for the header readout (see onUpdate for how it is tracked).
        float m_sessionBestLapTime = 0.0f;
        int   m_lastLapCompleted = -1;

        // --- Ghost mode ------------------------------------------------------------------
        // A ghost is a frozen snapshot of m_delta (size m_resolution) from a previously
        // completed full lap, captured in captureGhostCandidate() at lap rollover. Empty
        // vectors mean "no ghost captured yet"; rendering checks size()==m_resolution before
        // drawing so a stale/cleared snapshot (e.g. after a resolution change) is never drawn.
        enum GhostMode { GHOST_OFF, GHOST_LAST, GHOST_BEST };
        GhostMode m_ghostMode = GHOST_OFF;
        float4    m_ghostCol = float4(1,1,1,0.55f);
        float4    m_ghostFill = float4(1,1,1,0.12f);   // single neutral fill colour for the whole ghost
        float     m_ghostThickness = 1.5f;
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> m_ghostStrokeStyle;

        std::vector<float> m_ghost;        // GHOST_LAST: most recently completed full lap
        std::vector<float> m_ghostBest;    // GHOST_BEST: completed lap with smallest lap time so far
        float m_ghostBestLapTime = -1.0f;  // lap time associated with m_ghostBest, -1 = none yet

        // --- "Ghost became the target" collapse + new-best pulse -----------------------------
        // When a completed lap makes the ghost's lap identical to the current target lap (a new
        // best), the ghost is logically zero-delta vs itself, so it eases onto the center line and
        // merges with it. m_ghostCollapse is the animated 0..1 amount (1 = fully merged); it eases
        // toward 1 while m_ghostMerging is true (and toward 0 otherwise). m_zeroHighlight is the
        // center line's current 0..1 highlight intensity, driven every frame by the m_pbPhase state
        // machine below (a "double breath" celebration: two quick attack/decay pulses followed by a
        // slower final fade -- see onUpdate for the per-phase easing). m_prevTargetLapTime is the
        // target lap time seen at the previous rollover, used to edge-detect a real improvement (so
        // degenerate configs where ghost==target every lap don't pulse repeatedly). m_pbPulse/
        // m_pbPulseCol are config.
        float  m_ghostCollapse       = 0.0f;
        bool   m_ghostMerging        = false;
        float  m_zeroHighlight       = 0.0f;
        float  m_prevTargetLapTime   = -1.0f;
        bool   m_pbPulse             = true;
        float4 m_pbPulseCol          = float4(1.0f,0.80f,0.30f,0.85f);

        // Phase machine driving the new-best "double breath" pulse: P1 attack/decay, then P2
        // attack/decay (a slower final fade back to idle). See onUpdate for the per-phase easing
        // and frame counts, and captureGhostCandidate for where it's armed.
        enum PbPhase { PB_IDLE, PB_P1_ATTACK, PB_P1_DECAY, PB_P2_ATTACK, PB_P2_DECAY };
        PbPhase m_pbPhase      = PB_IDLE;
        int     m_pbPhaseFrame = 0;

        // --- Section delta bar -----------------------------------------------------------
        bool   m_sectionBar = true;
        float  m_sectionBarHeight = 22.0f;
        bool   m_showSectionValues = true;
        float4 m_sectionGainCol, m_sectionLossCol, m_sectionUndrivenCol;
};
