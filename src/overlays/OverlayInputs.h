/*
MIT License

Copyright (c) 2021-2022 L. E. Spalt

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

#include "Overlay.h"
#include "Config.h"
#include "OverlayDebug.h"

class OverlayInputs : public Overlay
{
    public:

        OverlayInputs()
            : Overlay("OverlayInputs")
        {}

    protected:

        virtual float2 getDefaultSize()
        {
            return float2(400,100);
        }

        virtual void onConfigChanged()
        {
            // Width might have changed, reset tracker values. The ring's write head must be
            // reset too: the buffers were just resized, so any previous head index could now
            // be out of range. Starting from 0 over the (zero-initialized) buffers is correct.
            m_throttleVtx.resize( m_width );
            m_brakeVtx.resize( m_width );
            m_steerVtx.resize( m_width );
            // Brake-state buffer must stay the same size as m_brakeVtx (it is read through the
            // same m_head). resize() value-initializes new bytes to 0 == BRAKE_NORMAL, which is
            // the correct default for freshly cleared columns. m_brakeRuns never exceeds one run
            // per column, so reserve once to keep its push_back()s allocation-free.
            m_brakeState.resize( m_width );
            m_brakeRuns.reserve( m_width );
            m_head = 0;
            for( int i=0; i<m_width; ++i )
            {
                m_throttleVtx[i].x = float(i);
                m_brakeVtx[i].x = float(i);
                m_steerVtx[i].x = float(i);
            }
        }

        virtual void onUpdate()
        {
            const float w = (float)m_width;
            const float h = (float)m_height;

            // Make code below safe against indexing into size-1 when sizes are zero.
            // (Preserves the original guard: a zero-width trace would underflow the loops.)
            if( m_throttleVtx.empty() )
                m_throttleVtx.resize( 1 );
            if( m_brakeVtx.empty() )
                m_brakeVtx.resize( 1 );
            if( m_steerVtx.empty() )
                m_steerVtx.resize( 1 );
            // Keep the state buffer's size in lockstep with m_brakeVtx so the shared m_head
            // indexing below never reads out of range.
            if( m_brakeState.empty() )
                m_brakeState.resize( 1 );

            // Advance input vertices.
            //
            // Ring buffer instead of an O(width) left-shift: each frame we drop the single
            // oldest sample and append one newest sample, so the work is O(1) per channel.
            //
            // m_head is the index of the OLDEST sample. Logical order (oldest -> newest) is
            //   position j  ->  physical slot (m_head + j) % N,  for j in [0, N).
            // To append a new newest sample we overwrite the current oldest slot (m_head)
            // and then advance m_head by one; the slot we just wrote becomes logical position
            // N-1 (the newest), and the next slot becomes the new oldest. The three channel
            // buffers are always the same size, so they share m_head.
            //
            // Worked example (N=3, slots [a,b,c] with a oldest, head=0): append d ->
            //   write d to slot 0 -> [d,b,c]; head=1. Logical order = slot1,slot2,slot0 = b,c,d.
            // The oldest (a) is dropped, d is newest. Correct, no element duplicated/skipped.
            {
                const size_t n = m_throttleVtx.size();   // all three vectors share this size
                if( m_head >= n )                          // safety: keep head in range if a
                    m_head = 0;                            // resize/empty-guard shrank the buffers
                m_throttleVtx[m_head].y = ir_Throttle.getFloat();
                m_brakeVtx[m_head].y    = ir_Brake.getFloat();
                m_steerVtx[m_head].y    = std::min( 1.0f, std::max( 0.0f, (ir_SteeringWheelAngle.getFloat() / ir_SteeringWheelAngleMax.getFloat()) * -0.5f + 0.5f) );
                // Record the brake state for THIS sample into the same slot, before advancing
                // the head. ir_BrakeABSactive is a direct engine flag ("true if abs is currently
                // reducing brake force pressure"), so no heuristic is needed.
                m_brakeState[m_head] = ir_BrakeABSactive.getBool() ? BRAKE_ABS : BRAKE_NORMAL;
                m_head = ( m_head + 1 ) % n;
            }

            const float thickness = g_cfg.getFloat( m_name, "line_thickness", 2.0f );
            auto vtx2coord = [&]( const float2& v )->float2 {
                return float2( v.x+0.5f, h-0.5f*thickness - v.y*(h-thickness) );
            };

            // Map a logical position j (0 = oldest .. N-1 = newest) to a screen-space vertex.
            // The x coordinate is the logical column j (independent of the physical ring slot),
            // and the y is the sample stored in the physical slot (m_head + j) % N. This is the
            // ring-buffer read in oldest->newest order; it reproduces exactly what the old
            // left-shift produced (slot 0 = oldest at x=0 .. slot N-1 = newest at x=N-1).
            const int n = (int)m_throttleVtx.size();   // all three vectors share this size
            auto ringCoord = [&]( const std::vector<float2>& buf, int j )->float2 {
                const int phys = ( (int)m_head + j ) % n;
                return vtx2coord( float2( float(j), buf[phys].y ) );
            };

            // Throttle (fill)
            Microsoft::WRL::ComPtr<ID2D1PathGeometry1> throttleFillPath;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink>  throttleFillSink;
            HRCHECK( m_d2dFactory->CreatePathGeometry( &throttleFillPath ) );
            HRCHECK( throttleFillPath->Open( &throttleFillSink ) );
            throttleFillSink->BeginFigure( float2(0,h), D2D1_FIGURE_BEGIN_FILLED );
            for( int j=0; j<n; ++j )
                throttleFillSink->AddLine( ringCoord(m_throttleVtx,j) );
            throttleFillSink->AddLine( float2(float(n-1)+0.5f,h) );
            throttleFillSink->EndFigure( D2D1_FIGURE_END_OPEN );
            HRCHECK( throttleFillSink->Close() );

            // Brake: split into colour segments by ABS state.
            //
            // The brake trace must stay visually continuous (same thickness, same alpha, same
            // fill down to the bottom edge h); only the colour of the stretches where ABS was
            // active changes. To achieve that we group the logical positions into maximal runs
            // of identical state and emit one fill geometry and one line geometry per run.
            //
            // State is read in the same oldest->newest ring order as the vertices, so run j
            // describes the colour of the sample drawn at column j.
            auto stateAt = [&]( int j )->uint8_t {
                const int phys = ( (int)m_head + j ) % n;
                return m_brakeState[phys];
            };

            // Build the runs. Each run is the half-open scan [s, j) collapsed to the inclusive
            // span [s, j-1] once the state changes (or the trace ends). m_brakeRuns is reused;
            // clear() keeps its capacity so this loop performs no allocations on a steady width.
            m_brakeRuns.clear();
            {
                int s = 0;
                for( int j=1; j<=n; ++j )
                {
                    const uint8_t cur = stateAt(j-1);
                    if( j==n || stateAt(j) != cur )
                    {
                        m_brakeRuns.push_back( Run{ s, j-1, cur } );
                        s = j;
                    }
                }
            }

            // Hysteresis: ABS toggles can flicker for a single sample, which would spawn tiny
            // 1px colour slivers and extra geometries. Collapse any run shorter than min_run by
            // folding it into the preceding run (whose colour then extends over it). The first
            // run is never folded backwards; if it is itself too short it is absorbed by the
            // next pass when the following run extends. We rewrite m_brakeRuns in place: 'out'
            // is the index of the last kept run, always <= the read index, so no aliasing.
            const int minRun = std::max( 1, g_cfg.getInt( m_name, "brake_state_min_run", 2 ) );
            if( minRun > 1 && m_brakeRuns.size() > 1 )
            {
                size_t out = 0;
                for( size_t i=1; i<m_brakeRuns.size(); ++i )
                {
                    const Run& r = m_brakeRuns[i];
                    const int len = r.end - r.start + 1;
                    if( len < minRun )
                    {
                        // Too short: extend the previous (kept) run over this span, dropping
                        // this run's distinct colour. Adjacent same-state runs that result from
                        // this stay merged because we only ever grow m_brakeRuns[out].end.
                        m_brakeRuns[out].end = r.end;
                    }
                    else if( r.state == m_brakeRuns[out].state )
                    {
                        // Same colour as the kept run (can happen after a fold above): merge.
                        m_brakeRuns[out].end = r.end;
                    }
                    else
                    {
                        m_brakeRuns[++out] = r;
                    }
                }
                m_brakeRuns.resize( out + 1 );
            }

            // Brake (fill) — one geometry per run.
            //
            // Each fill spans the sub-range [s,e]. To avoid gaps between adjacent runs the
            // right edge is dragged one column further to e+1 (the start of the next run), so
            // neighbouring fills share the exact vertical seam at column e+1; the boundary edge
            // belongs to the current run's colour and the next run begins at e+1 without
            // repeating it. The last run has no successor, so it closes at its own end column.
            std::vector<Microsoft::WRL::ComPtr<ID2D1PathGeometry1>> brakeFillPaths;
            brakeFillPaths.reserve( m_brakeRuns.size() );
            for( const Run& run : m_brakeRuns )
            {
                const int s = run.start;
                const int e = run.end;
                Microsoft::WRL::ComPtr<ID2D1PathGeometry1> fillPath;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink>  fillSink;
                HRCHECK( m_d2dFactory->CreatePathGeometry( &fillPath ) );
                HRCHECK( fillPath->Open( &fillSink ) );
                fillSink->BeginFigure( float2(float(s)+0.5f,h), D2D1_FIGURE_BEGIN_FILLED );
                for( int j=s; j<=e; ++j )
                    fillSink->AddLine( ringCoord(m_brakeVtx,j) );
                float xRight;
                if( e+1 < n )   // bridge to the next run's first column to close the seam
                {
                    fillSink->AddLine( ringCoord(m_brakeVtx,e+1) );
                    xRight = float(e+1)+0.5f;
                }
                else            // last run: drop straight down at its own right edge
                {
                    xRight = float(e)+0.5f;
                }
                fillSink->AddLine( float2(xRight,h) );
                fillSink->EndFigure( D2D1_FIGURE_END_CLOSED );
                HRCHECK( fillSink->Close() );
                brakeFillPaths.push_back( std::move(fillPath) );
            }

            // Throttle (line)
            Microsoft::WRL::ComPtr<ID2D1PathGeometry1> throttleLinePath;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink>  throttleLineSink;
            HRCHECK( m_d2dFactory->CreatePathGeometry( &throttleLinePath ) );
            HRCHECK( throttleLinePath->Open( &throttleLineSink ) );
            throttleLineSink->BeginFigure( ringCoord(m_throttleVtx,0), D2D1_FIGURE_BEGIN_HOLLOW );
            for( int j=1; j<n; ++j )
                throttleLineSink->AddLine( ringCoord(m_throttleVtx,j) );
            throttleLineSink->EndFigure( D2D1_FIGURE_END_OPEN );
            HRCHECK( throttleLineSink->Close() );

            // Brake (line) — one geometry per run, matching the fill segmentation above.
            //
            // The polyline for run [s,e] starts at column s and, like the fill, is dragged to
            // column e+1 when a next run exists so the coloured segments meet exactly at the
            // shared vertex with no visible break. The next run redraws from e+1; the doubled
            // vertex at the seam is harmless (the line just changes colour there).
            std::vector<Microsoft::WRL::ComPtr<ID2D1PathGeometry1>> brakeLinePaths;
            brakeLinePaths.reserve( m_brakeRuns.size() );
            for( const Run& run : m_brakeRuns )
            {
                const int s = run.start;
                const int e = run.end;
                Microsoft::WRL::ComPtr<ID2D1PathGeometry1> linePath;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink>  lineSink;
                HRCHECK( m_d2dFactory->CreatePathGeometry( &linePath ) );
                HRCHECK( linePath->Open( &lineSink ) );
                lineSink->BeginFigure( ringCoord(m_brakeVtx,s), D2D1_FIGURE_BEGIN_HOLLOW );
                for( int j=s+1; j<=e; ++j )
                    lineSink->AddLine( ringCoord(m_brakeVtx,j) );
                if( e+1 < n )   // extend to the next run's first column to close the seam
                    lineSink->AddLine( ringCoord(m_brakeVtx,e+1) );
                lineSink->EndFigure( D2D1_FIGURE_END_OPEN );
                HRCHECK( lineSink->Close() );
                brakeLinePaths.push_back( std::move(linePath) );
            }

            // Steering
            Microsoft::WRL::ComPtr<ID2D1PathGeometry1> steeringLinePath;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink>  steeringLineSink;
            HRCHECK( m_d2dFactory->CreatePathGeometry( &steeringLinePath ) );
            HRCHECK( steeringLinePath->Open( &steeringLineSink ) );
            steeringLineSink->BeginFigure( ringCoord(m_steerVtx,0), D2D1_FIGURE_BEGIN_HOLLOW );
            for( int j=1; j<n; ++j )
                steeringLineSink->AddLine( ringCoord(m_steerVtx,j) );
            steeringLineSink->EndFigure( D2D1_FIGURE_END_OPEN );
            HRCHECK( steeringLineSink->Close() );

            // Per-state brake colours. BRAKE_NORMAL keeps the original brake_fill_col/brake_col
            // defaults so existing configs are unaffected; BRAKE_ABS uses the new orange keys.
            const float4 brakeFillNormal = g_cfg.getFloat4( m_name, "brake_fill_col",     float4(0.46f,0.01f,0.06f,0.6f) );
            const float4 brakeLineNormal = g_cfg.getFloat4( m_name, "brake_col",          float4(0.93f,0.03f,0.13f,0.8f) );
            const float4 brakeFillAbs    = g_cfg.getFloat4( m_name, "brake_abs_fill_col", float4(0.50f,0.22f,0.02f,0.6f) );
            const float4 brakeLineAbs    = g_cfg.getFloat4( m_name, "brake_abs_col",      float4(0.95f,0.55f,0.05f,0.8f) );
            auto brakeFillColor = [&]( uint8_t state )->const float4& {
                return state==BRAKE_ABS ? brakeFillAbs : brakeFillNormal;
            };
            auto brakeLineColor = [&]( uint8_t state )->const float4& {
                return state==BRAKE_ABS ? brakeLineAbs : brakeLineNormal;
            };

            m_renderTarget->BeginDraw();
            // Z-order: all fills first, then all lines. Throttle fill stays at the bottom,
            // the brake fill segments sit above it, then the line traces on top.
            m_brush->SetColor( g_cfg.getFloat4( m_name, "throttle_fill_col", float4(0.2f,0.45f,0.15f,0.6f) ) );
            m_renderTarget->FillGeometry( throttleFillPath.Get(), m_brush.Get() );
            for( size_t i=0; i<m_brakeRuns.size(); ++i )
            {
                m_brush->SetColor( brakeFillColor( m_brakeRuns[i].state ) );
                m_renderTarget->FillGeometry( brakeFillPaths[i].Get(), m_brush.Get() );
            }
            m_brush->SetColor( g_cfg.getFloat4( m_name, "throttle_col", float4(0.38f,0.91f,0.31f,0.8f) ) );
            m_renderTarget->DrawGeometry( throttleLinePath.Get(), m_brush.Get(), thickness );
            for( size_t i=0; i<m_brakeRuns.size(); ++i )
            {
                m_brush->SetColor( brakeLineColor( m_brakeRuns[i].state ) );
                m_renderTarget->DrawGeometry( brakeLinePaths[i].Get(), m_brush.Get(), thickness );
            }
            m_brush->SetColor( g_cfg.getFloat4( m_name, "steering_col", float4(1,1,1,0.3f) ) );
            m_renderTarget->DrawGeometry( steeringLinePath.Get(), m_brush.Get(), thickness );
            m_renderTarget->EndDraw();
        }

    protected:

        std::vector<float2> m_throttleVtx;
        std::vector<float2> m_brakeVtx;
        std::vector<float2> m_steerVtx;

        // Per-sample state of the brake channel. Parallel to m_brakeVtx: same size, written
        // into the same physical slot and read through the same m_head, so state[j] always
        // describes the sample stored in m_brakeVtx[j]. One byte per sample keeps the buffer
        // cheap; the enum is deliberately open-ended (a future BRAKE_LOCK could be appended)
        // without changing the storage type.
        enum BrakeState : uint8_t {
            BRAKE_NORMAL = 0,   // ordinary braking
            BRAKE_ABS    = 1,   // ABS is actively reducing brake pressure on this sample
        };
        std::vector<uint8_t> m_brakeState;

        // A contiguous span [start,end] of logical positions sharing one brake state. The
        // brake trace is split into these runs every frame so each colour gets its own
        // geometry; reused across frames (clear() only) to avoid per-frame allocations.
        struct Run { int start, end; uint8_t state; };
        std::vector<Run> m_brakeRuns;

        // Ring-buffer write head shared by the three trace buffers and the brake-state buffer
        // (they are always the same size). Index of the OLDEST sample; see onUpdate() for the
        // wrap/append reasoning.
        size_t m_head = 0;
};
