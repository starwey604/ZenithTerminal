#!/usr/bin/env python3
"""Compare the extracted original/patched alpha blending, including invalid premultiplication.
Usage: check-blend.py ORIGINAL_VKMS_DIR PATCHED_VKMS_DIR
"""
from pathlib import Path
import os
import runpy
import subprocess
import sys
import tempfile

if len(sys.argv) != 3:
    sys.exit(__doc__)
function = runpy.run_path(str(Path(__file__).with_name('check-math.py')))['function']
old, new = [(Path(p) / 'vkms_composer.c').read_text() for p in sys.argv[1:]]
code = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16;
typedef uint32_t u32;
#define DIV_ROUND_CLOSEST(x,d) (((x)+(d)/2)/(d))
struct pixel_argb_u16 { u16 a,r,g,b; };
struct line_buffer { struct pixel_argb_u16 *pixels; };
'''
code += function(old, 'pre_mul_blend_channel') + '\n'
code += function(old, 'pre_mul_alpha_blend').replace('pre_mul_alpha_blend', 'old_blend') + '\n'
code += function(new, 'pre_mul_alpha_blend').replace('pre_mul_alpha_blend', 'new_blend') + '\n'
code += r'''
static uint64_t rng=0x12344271;
static u16 random16(void) { rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng; }
int main(void) {
    struct pixel_argb_u16 src[257],dst[257],ref[257];
    struct line_buffer a={src},b={dst},c={ref};
    for(int value=0;value<65536;++value) {
        for(int i=0;i<257;++i) {
            src[i]=(struct pixel_argb_u16){i%3==0?65535:i%3==1?0:random16(),value,random16(),random16()};
            if(i%7==0) src[i]=(struct pixel_argb_u16){0,0,0,0};
            dst[i]=(struct pixel_argb_u16){random16(),random16(),random16(),random16()};
        }
        memcpy(ref,dst,sizeof(ref));
        int offset=value%8,count=249;
        old_blend(&a,&c,offset,count);new_blend(&a,&b,offset,count);
        if(memcmp(dst,ref,sizeof(ref))) { fprintf(stderr,"blend mismatch value=%d\n",value);return 1; }
    }
    puts("PASS: original vs optimized blending, all opaque u16 source values, transparent/partial alpha, non-premultiplied values and untouched row guards");
}
'''
with tempfile.TemporaryDirectory(prefix='zenith-vkms-blend-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text(code)
    # Match the kernel's signed-wrap arithmetic in the existing blend helper.
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-fwrapv', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
