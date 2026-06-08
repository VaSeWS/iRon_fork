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

            // Brake (fill)
            Microsoft::WRL::ComPtr<ID2D1PathGeometry1> brakeFillPath;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink>  brakeFillSink;
            HRCHECK( m_d2dFactory->CreatePathGeometry( &brakeFillPath ) );
            HRCHECK( brakeFillPath->Open( &brakeFillSink ) );
            brakeFillSink->BeginFigure( float2(0,h), D2D1_FIGURE_BEGIN_FILLED );
            for( int j=0; j<n; ++j )
                brakeFillSink->AddLine( ringCoord(m_brakeVtx,j) );
            brakeFillSink->AddLine( float2(float(n-1)+0.5f,h) );
            brakeFillSink->EndFigure( D2D1_FIGURE_END_OPEN );
            HRCHECK( brakeFillSink->Close() );

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

            // Brake (line)
            Microsoft::WRL::ComPtr<ID2D1PathGeometry1> brakeLinePath;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink>  brakeLineSink;
            HRCHECK( m_d2dFactory->CreatePathGeometry( &brakeLinePath ) );
            HRCHECK( brakeLinePath->Open( &brakeLineSink ) );
            brakeLineSink->BeginFigure( ringCoord(m_brakeVtx,0), D2D1_FIGURE_BEGIN_HOLLOW );
            for( int j=1; j<n; ++j )
                brakeLineSink->AddLine( ringCoord(m_brakeVtx,j) );
            brakeLineSink->EndFigure( D2D1_FIGURE_END_OPEN );
            HRCHECK( brakeLineSink->Close() );

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

            m_renderTarget->BeginDraw();
            m_brush->SetColor( g_cfg.getFloat4( m_name, "throttle_fill_col", float4(0.2f,0.45f,0.15f,0.6f) ) );
            m_renderTarget->FillGeometry( throttleFillPath.Get(), m_brush.Get() );
            m_brush->SetColor( g_cfg.getFloat4( m_name, "brake_fill_col", float4(0.46f,0.01f,0.06f,0.6f) ) );
            m_renderTarget->FillGeometry( brakeFillPath.Get(), m_brush.Get() );
            m_brush->SetColor( g_cfg.getFloat4( m_name, "throttle_col", float4(0.38f,0.91f,0.31f,0.8f) ) );
            m_renderTarget->DrawGeometry( throttleLinePath.Get(), m_brush.Get(), thickness );
            m_brush->SetColor( g_cfg.getFloat4( m_name, "brake_col", float4(0.93f,0.03f,0.13f,0.8f) ) );
            m_renderTarget->DrawGeometry( brakeLinePath.Get(), m_brush.Get(), thickness );
            m_brush->SetColor( g_cfg.getFloat4( m_name, "steering_col", float4(1,1,1,0.3f) ) );
            m_renderTarget->DrawGeometry( steeringLinePath.Get(), m_brush.Get(), thickness );
            m_renderTarget->EndDraw();
        }

    protected:

        std::vector<float2> m_throttleVtx;
        std::vector<float2> m_brakeVtx;
        std::vector<float2> m_steerVtx;

        // Ring-buffer write head shared by the three trace buffers (they are always the same
        // size). Index of the OLDEST sample; see onUpdate() for the wrap/append reasoning.
        size_t m_head = 0;
};
