#!/usr/bin/env python3
"""Compile arithmetic extracted from the original and patched sources, then compare.

Usage: check-math.py ORIGINAL_VKMS_DIR PATCHED_VKMS_DIR KERNEL_BUILD_DIR
fls64 is shimmed with the same one-based/zero semantics. NV12 framebuffer
metadata is mocked for reader tests; arithmetic and reader functions are extracted.
No module loading and no DRM access. Requires a host C compiler.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def function(source, name):
    match = re.search(r"(?:static (?:inline )?)?(?:void|int|u16|unsigned(?: int)?|s64|struct pixel_argb_u16|struct vkms_nv12_chroma)\s+" + name + r"\([^;]+?\n\{", source)
    if not match:
        raise ValueError(f"missing function: {name}")
    start = match.start()
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    original, patched, kernel = map(Path, sys.argv[1:])
    old = (original / 'vkms_formats.c').read_text()
    new = (patched / 'vkms_formats.c').read_text()
    fixed = (kernel / 'include/drm/drm_fixed.h').read_text()
    matrices = re.findall(r'static const struct conversion_matrix (yuv_\w+) = \{.*?\n\};', old, re.S)
    definitions = re.findall(r'static const struct conversion_matrix yuv_\w+ = \{.*?\n\};', old, re.S)
    if len(matrices) != 6:
        raise ValueError('expected six YUV color matrices; review source changes')
    code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int64_t s64;
typedef uint64_t u64;
typedef uint16_t u16;
typedef uint8_t u8;
#define DIV_ROUND_CLOSEST(x, d) (((x) + (d)/2)/(d))
struct vkms_nv12_lut { s64 contribution[3][256][3]; };
struct vkms_nv12_chroma { s64 r,g,b; };
#define DRM_FIXED_POINT 32
#define drm_int2fixp(x) ((s64)(x) * INT64_C(4294967296))
#define drm_fixp2int_round(x) (((x) + INT64_C(2147483648)) >> 32)
#define clamp(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
static int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
struct conversion_matrix { s64 matrix[3][3]; int y_offset; };
struct pixel_argb_u16 { u16 a, r, g, b; };
static struct pixel_argb_u16 argb_u16_from_u16161616(u16 a,u16 r,u16 g,u16 b)
{ return (struct pixel_argb_u16){a,r,g,b}; }
'''
    code += '\n' + function(fixed, 'drm_fixp_msbset') + '\n' + function(fixed, 'drm_fixp_mul')
    code += '\n' + function(new, 'vkms_fixp_msbset') + '\n' + function(new, 'vkms_fixp_mul')
    code += '\n' + function(new, 'vkms_build_nv12_lut') + '\n' + function(new, 'vkms_nv12_chroma') + '\n' + function(new, 'argb_u16_from_nv12_lut')
    code += '\n' + function(new, 'argb_u16_to_XRGB8888') + '\n' + function(new, 'vkms_xrgb8888_write_line')
    code += '\n' + function(old, 'argb_u16_from_yuv161616').replace('argb_u16_from_yuv161616', 'old_pixel')
    code += '\n' + function(new, 'argb_u16_from_yuv161616').replace('argb_u16_from_yuv161616', 'new_pixel')
    code += '\n' + '\n'.join(definitions)
    code += '\nstatic const struct conversion_matrix *matrices[] = {' + ','.join('&'+n for n in matrices) + '};\n'
    code += r'''
enum pixel_read_direction { READ_LEFT_TO_RIGHT, READ_RIGHT_TO_LEFT, READ_TOP_TO_BOTTOM, READ_BOTTOM_TO_TOP };
struct drm_format_info { int char_per_block[2]; };
struct drm_framebuffer { const struct drm_format_info *format; int pitches[2], offsets[2]; };
struct vkms_frame_info { struct drm_framebuffer *fb; struct { void *vaddr; } map[1]; };
struct drm_plane_state { int color_encoding, color_range; };
struct vkms_plane_state { struct { struct drm_plane_state base; } base; struct vkms_frame_info *frame_info; };
static struct vkms_nv12_lut nv12_luts[3][2];
#define WARN_ONCE(...) ((void)0)
static int drm_format_info_block_width(const struct drm_format_info *f,int p) { (void)f;(void)p;return 1; }
static int drm_format_info_block_height(const struct drm_format_info *f,int p) { (void)f;(void)p;return 1; }
'''
    for name in ['packed_pixels_offset', 'packed_pixels_addr_1x1', 'get_block_step_bytes', 'get_subsampling_offset', 'NV12_read_line']:
        code += '\n' + function(new, name)
    code += r'''
static u64 rng = 0x8721abcd1234ULL;
static u64 random64(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void check_msb(s64 x) {
    if (drm_fixp_msbset(x) != vkms_fixp_msbset(x)) {
        fprintf(stderr, "MSB mismatch: %lld\n", (long long)x); exit(1);
    }
}
int main(void) {
    check_msb(0); check_msb(-1); check_msb(INT64_MIN); check_msb(INT64_MAX);
    for (int bit=0; bit<64; ++bit) {
        u64 x=UINT64_C(1)<<bit;
        for (int delta=-1;delta<=1;++delta) { check_msb((s64)(x+delta)); check_msb((s64)~(x+delta)); }
    }
    for (int i=0;i<1000000;++i) check_msb((s64)random64());
    unsigned long terms=0;
    // Exhaust every possible 16-bit channel for each coefficient and channel offset.
    // Equality of each term proves equality of the unchanged three-term sums for
    // every YUV triple, including 8/10/12-bit inputs expanded to 16 bits.
    for (int m=0;m<6;++m) for(int row=0;row<3;++row) for(int col=0;col<3;++col) {
        const struct conversion_matrix *matrix=matrices[m];
        s64 a=matrix->matrix[row][col];
        for (int input=0;input<65536;++input) {
            s64 b=drm_int2fixp(input-(col==0?matrix->y_offset:128)*257);
            if (drm_fixp_mul(a,b)!=vkms_fixp_mul(a,b)) {
                fprintf(stderr,"multiply mismatch: matrix=%d row=%d col=%d input=%d\n",m,row,col,input);return 1;
            }
            ++terms;
        }
    }
    for (int m=0;m<6;++m) for(int i=0;i<100000;++i) {
        u16 y=random64(),u=random64(),v=random64();
        struct pixel_argb_u16 a=old_pixel(matrices[m],y,u,v),b=new_pixel(matrices[m],y,u,v);
        if(a.a!=b.a||a.r!=b.r||a.g!=b.g||a.b!=b.b) { fprintf(stderr,"pixel mismatch\n");return 1; }
    }
    for (int m=0;m<6;++m) {
        struct vkms_nv12_lut lut, reference;
        vkms_build_nv12_lut(&lut, matrices[m]);
        for(int c=0;c<3;++c) for(int v=0;v<256;++v) for(int r=0;r<3;++r) {
            s64 input=drm_int2fixp((v-(c?128:matrices[m]->y_offset))*257);
            s64 term=drm_fixp_mul(matrices[m]->matrix[r][c],input);
            reference.contribution[c][v][r]=term;
            if(lut.contribution[c][v][r]!=term) { fprintf(stderr,"LUT term mismatch\n");return 1; }
        }
        // Exhaust all 256^3 NV12 colors for each of the six matrices. Expected
        // terms use the original multiplication, rounding and clipping.
        for(int y=0;y<256;++y) for(int u=0;u<256;++u) for(int v=0;v<256;++v) {
            struct pixel_argb_u16 b=argb_u16_from_nv12_lut(&lut,y,vkms_nv12_chroma(&lut,u,v));
            u16 actual[3]={b.r,b.g,b.b};
            for(int r=0;r<3;++r) {
                s64 sum=reference.contribution[0][y][r]+reference.contribution[1][u][r]+reference.contribution[2][v][r];
                s64 expected=drm_fixp2int_round(sum);
                if(actual[r]!=clamp(expected,0,65535)||b.a!=65535) { fprintf(stderr,"LUT pixel mismatch\n");return 1; }
            }
        }
    }
    // Exercise the actual patched reader with padded pitches, nonzero plane
    // offsets, odd/even crop origins and all four rotation/reflection directions.
    const struct drm_format_info format={{1,2}};
    struct drm_framebuffer fb={&format,{32,40},{13,13+32*10+19}};
    u8 pixels[640];
    for(unsigned i=0;i<sizeof(pixels);++i) pixels[i]=random64();
    struct vkms_frame_info info={&fb,{{pixels}}};
    struct vkms_plane_state plane={{{0,0}},&info};
    struct pixel_argb_u16 output[18];
    for(int m=0;m<6;++m) {
        plane.base.base.color_encoding=m/2;
        plane.base.base.color_range=1-m%2;
        vkms_build_nv12_lut(&nv12_luts[m/2][1-m%2],matrices[m]);
        for(int y=0;y<10;++y) for(int x=0;x<18;++x) for(int dir=0;dir<4;++dir) {
            int dx=dir==0?1:dir==1?-1:0,dy=dir==2?1:dir==3?-1:0;
            int count=dir==0?18-x:dir==1?x+1:dir==2?10-y:y+1;
            NV12_read_line(&plane,x,y,dir,count,output);
            for(int i=0;i<count;++i) {
                int xx=x+i*dx,yy=y+i*dy;
                int yo=fb.offsets[0]+yy*fb.pitches[0]+xx;
                int uv=fb.offsets[1]+(yy/2)*fb.pitches[1]+(xx/2)*2;
                struct pixel_argb_u16 a=old_pixel(matrices[m],pixels[yo]*257,pixels[uv]*257,pixels[uv+1]*257),b=output[i];
                if(a.a!=b.a||a.r!=b.r||a.g!=b.g||a.b!=b.b) { fprintf(stderr,"reader mismatch matrix=%d x=%d y=%d dir=%d i=%d\n",m,x,y,dir,i);return 1; }
            }
        }
    }
    struct pixel_argb_u16 input[129];
    u8 bytes[129*4+32];
    for(int value=0;value<65536;++value) {
        for(int i=0;i<129;++i) input[i]=(struct pixel_argb_u16){i,value,(u16)(value+123),(u16)(value*3)};
        for(unsigned i=0;i<sizeof(bytes);++i) bytes[i]=0xa5;
        vkms_xrgb8888_write_line(bytes+13,input,129);
        for(int i=0;i<129;++i) {
            const u8 *b=bytes+13+i*4;
            if(b[0]!=(input[i].b+128)/257||b[1]!=(input[i].g+128)/257||b[2]!=(input[i].r+128)/257||b[3]!=255) { fprintf(stderr,"writeback mismatch\n");return 1; }
        }
        for(unsigned i=0;i<sizeof(bytes);++i) if((i<13||i>=13+129*4)&&bytes[i]!=0xa5) { fprintf(stderr,"writeback guard mismatch\n");return 1; }
    }
    puts("PASS: direct XRGB8888 writeback, exhaustive u16 channel values + row guards");
    puts("PASS: actual NV12 reader, all directions/crop parities/padded pitches/plane offsets");
    puts("PASS: exact NV12 tables + 100663296 exhaustive 8-bit pixel comparisons");
    printf("PASS: MSB boundaries + 1000000 random values; %lu exhaustive channel terms; 600000 RGB pixel comparisons\n",terms);
}
'''
    with tempfile.TemporaryDirectory(prefix='zenith-vkms-math-') as tmp:
        c = Path(tmp) / 'check.c'
        binary = Path(tmp) / 'check'
        c.write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all', str(c), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
