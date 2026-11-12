#!/usr/bin/env python3
"""Compare the extracted color pipeline loop before/after the no-pipeline fast path.
Usage: check-color.py ORIGINAL_VKMS_DIR PATCHED_VKMS_DIR
Color operations are deterministic test doubles; active pipeline logic is unchanged.
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
struct pixel_argb_u16 { u16 a,r,g,b; };
struct pixel_argb_s32 { int32_t a,r,g,b; };
struct line_buffer { size_t n_pixels; struct pixel_argb_u16 *pixels; };
struct drm_colorop_state { int bypass; };
struct drm_colorop { struct drm_colorop_state *state; struct drm_colorop *next; int offset; };
struct vkms_plane_state { struct { struct { struct drm_colorop *color_pipeline; } base; } base; };
#define clamp_val(x,l,h) ((x)<(l)?(l):(x)>(h)?(h):(x))
static void apply_colorop(struct pixel_argb_s32 *p,struct drm_colorop *op) {
 p->r+=op->offset;p->g-=op->offset;p->b+=op->offset/2;
}
'''
code += function(old,'pre_blend_color_transform').replace('pre_blend_color_transform','old_color')+'\n'
code += function(new,'pre_blend_color_transform').replace('pre_blend_color_transform','new_color')+'\n'
code += r'''
static uint64_t rng=0x871342ab;
static u16 random16(void) { rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng; }
int main(void) {
 struct pixel_argb_u16 a[257],b[257];
 struct drm_colorop_state enabled={0},bypass={1};
 struct drm_colorop second={&enabled,0,-50000},first={&enabled,&second,80000},skip={&bypass,&second,30000},missing={0,0,0};
 struct drm_colorop *cases[]={0,&first,&skip,&missing};
 for(unsigned k=0;k<4;++k) for(int n=0;n<=257;++n) for(int repeat=0;repeat<30;++repeat) {
  for(int i=0;i<257;++i) a[i]=(struct pixel_argb_u16){random16(),random16(),random16(),random16()};
  memcpy(b,a,sizeof(a));
  struct line_buffer x={n,a},y={n,b};
  struct vkms_plane_state plane={{{cases[k]}}};
  old_color(&plane,&x);new_color(&plane,&y);
  if(memcmp(a,b,sizeof(a))) { fprintf(stderr,"color mismatch case=%u length=%d\n",k,n);return 1; }
 }
 puts("PASS: no-pipeline identity and unchanged active/bypass/missing-state color traversal, clamp and row bounds");
}
'''
with tempfile.TemporaryDirectory(prefix='zenith-vkms-color-') as tmp:
    source=Path(tmp)/'test.c';binary=Path(tmp)/'test';source.write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
