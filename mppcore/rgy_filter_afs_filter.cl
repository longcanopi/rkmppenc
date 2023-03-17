﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

//FILTER_BLOCK_INT_X  (32) //work groupサイズ(x) = スレッド数/work group
//FILTER_BLOCK_Y       (8) //work groupサイズ(y) = スレッド数/work group

#define u8x4(x)  (((uint)x) | (((uint)x) <<  8) | (((uint)x) << 16) | (((uint)x) << 24))
#ifndef clamp
#define clamp(x, low, high) (((x) <= (high)) ? (((x) >= (low)) ? (x) : (low)) : (high))
#endif

void filter_h_1(__local uint shared[2][FILTER_BLOCK_Y+4][FILTER_BLOCK_INT_X+2], int sx, int sy) {
    uint x0 = shared[0][sy][sx-1];
    uint x1 = shared[0][sy][sx+0];
    uint x2 = shared[0][sy][sx+1];
    uint x3 = x0 >> 24 | x1 <<  8;
    uint x4 = x1 >>  8 | x2 << 24;
    shared[1][sy][sx] = x1 | ((x3 | x4) & u8x4(0x03)) | ((x3 & x4) & u8x4(0x04));
}

void filter_h_1_edge(__local uint shared[2][FILTER_BLOCK_Y+4][FILTER_BLOCK_INT_X+2], int sx, int sy) {
    uint x0 = (sx-1 >= 0)                 ? shared[0][sy][sx-1] : 0;
    uint x1 =                               shared[0][sy][sx+0];
    uint x2 = (sx+1<FILTER_BLOCK_INT_X+2) ? shared[0][sy][sx+1] : 0;
    uint x3 = x0 >> 24 | x1 <<  8;
    uint x4 = x1 >>  8 | x2 << 24;
    shared[1][sy][sx] = x1 | ((x3 | x4) & u8x4(0x03)) | ((x3 & x4) & u8x4(0x04));
}

void filter_v_1(__local uint shared[2][FILTER_BLOCK_Y+4][FILTER_BLOCK_INT_X+2], int sx, int sy) {
    uint x0 = shared[1][sy-1][sx];
    uint x1 = shared[1][sy+0][sx];
    uint x2 = shared[1][sy+1][sx];
    shared[0][sy][sx] = x1 | (x0 & x2 & u8x4(0x03 | 0x04));
}

void filter_h_2(__local uint shared[2][FILTER_BLOCK_Y+4][FILTER_BLOCK_INT_X+2], int sx, int sy) {
    uint x0 = shared[0][sy][sx-1];
    uint x1 = shared[0][sy][sx+0];
    uint x2 = shared[0][sy][sx+1];
    uint x3 = x0 >> 24 | x1 <<  8;
    uint x4 = x1 >>  8 | x2 << 24;
    shared[1][sy][sx] = x1 & ((x3 & x4) | u8x4(0xf8));
}

uint get_filter_v_2(__local uint shared[2][FILTER_BLOCK_Y+4][FILTER_BLOCK_INT_X+2], int sx, int sy) {
    uint x0 = shared[1][sy-1][sx];
    uint x1 = shared[1][sy+0][sx];
    uint x2 = shared[1][sy+1][sx];
    return x1 & ((x0 & x2) | u8x4(0xf8));
}

__kernel void kernel_afs_analyze_map_filter(
    __global uint *__restrict__ ptr_dst,
    __global const uint *__restrict__ ptr_src,
    const int si_w_type, const int pitch_type, const int height) {
    const int lx = get_local_id(0); //スレッド数=FILTER_BLOCK_INT_X
    const int ly = get_local_id(1); //スレッド数=FILTER_BLOCK_Y
    const int imgx = get_group_id(0) * FILTER_BLOCK_INT_X /*blockDim.x*/ + lx;
    const int imgy = get_group_id(1) * FILTER_BLOCK_Y     /*blockDim.y*/ + ly;

    //左右の縁 lx(0)=0, lx(1)=FILTER_BLOCK_INT_X+1
    const int sx_edge = (lx) ? FILTER_BLOCK_INT_X+1 : 0;

    __local uint shared[2][FILTER_BLOCK_Y+4][FILTER_BLOCK_INT_X+2];

    //sharedメモリへのロード
#define SRCPTR(ix, iy) *(__global const uint *)(ptr_src + clamp((iy), 0, height) * pitch_type + clamp((ix), 0, si_w_type))
    //中央部分のロード
    shared[0][ly][lx+1] = SRCPTR(imgx, imgy-2);
    if (lx < 2) {
        //x方向の左(lx=0)右(lx=1)の縁をロード
        shared[0][ly][sx_edge] = SRCPTR(imgx-1+sx_edge, imgy-2);
    }
    if (ly < 4) {
        shared[0][ly + FILTER_BLOCK_Y][lx+1] = SRCPTR(imgx, imgy-2+FILTER_BLOCK_Y);
        if (lx < 2) {
            //x方向の左(lx=0)右(lx=1)の縁をロード
            shared[0][ly + FILTER_BLOCK_Y][sx_edge] = SRCPTR(imgx-1+sx_edge, imgy-2+FILTER_BLOCK_Y);
        }
    }
    barrier(CLK_LOCAL_MEM_FENCE);
#undef SRCPTR

    //filter_h(1)
    filter_h_1(shared, lx+1, ly);
    if (lx < 2) {
        //x方向の縁
        filter_h_1_edge(shared, sx_edge, ly);
    }
    if (ly < 4) {
        filter_h_1(shared, lx+1, ly+FILTER_BLOCK_Y);
        if (lx < 2) {
            //x方向の縁
            filter_h_1_edge(shared, sx_edge, ly+FILTER_BLOCK_Y);
        }
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    //filter_v(1)
    filter_v_1(shared, lx, ly+1);
    if (lx < 2) {
        //x方向の縁
        filter_v_1(shared, lx+FILTER_BLOCK_INT_X, ly+1);
    }
    if (ly < 2) {
        filter_v_1(shared, lx, ly+1+FILTER_BLOCK_Y);
        if (lx < 2) {
            //x方向の縁
            filter_v_1(shared, lx+FILTER_BLOCK_INT_X, ly+1+FILTER_BLOCK_Y);
        }
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    //filter_h(2)
    filter_h_2(shared, lx+1, ly+1);
    if (ly < 2) {
        filter_h_2(shared, lx+1, ly+1+FILTER_BLOCK_Y);
    }
    barrier(CLK_LOCAL_MEM_FENCE);

    //filter_v(2)
    if (imgx < si_w_type && imgy < height) {
        uint ret = get_filter_v_2(shared, lx+1, ly+2);
        ptr_dst[imgy * pitch_type + imgx] = ret;
    }
}
