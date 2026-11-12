#!/usr/bin/env python3
"""Compare fused ARGB reading/blending against the original two passes.
Usage: check-fusion.py ORIGINAL_VKMS_DIR PATCHED_VKMS_DIR
"""
from pathlib import Path
import os
import re
import runpy
import subprocess
import sys
import tempfile
if len(sys.argv) != 3:
    sys.exit(__doc__)
function = runpy.run_path(str(Path(__file__).with_name('check-math.py')))['function']
old = (Path(sys.argv[1]) / 'vkms_formats.c').read_text()
blend = (Path(sys.argv[1]) / 'vkms_composer.c').read_text()
new = (Path(sys.argv[2]) / 'vkms_formats.c').read_text()
code = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define DIV_ROUND_CLOSEST(x,d) (((x)+(d)/2)/(d))
#define WARN_ONCE(...) ((void)0)
struct pixel_argb_u16 { u16 a,r,g,b; };
struct line_buffer { size_t n_pixels; struct pixel_argb_u16 *pixels; };
enum pixel_read_direction { READ_LEFT_TO_RIGHT, READ_RIGHT_TO_LEFT, READ_TOP_TO_BOTTOM, READ_BOTTOM_TO_TOP };
struct drm_format_info { int char_per_block[2]; };
struct drm_framebuffer { const struct drm_format_info *format; int pitches[2],offsets[2]; };
struct vkms_frame_info { struct drm_framebuffer *fb; struct { void *vaddr; } map[1]; };
struct vkms_plane_state { struct vkms_frame_info *frame_info; };
static int drm_format_info_block_width(const struct drm_format_info *f,int p) { (void)f;(void)p;return 1; }
static int drm_format_info_block_height(const struct drm_format_info *f,int p) { (void)f;(void)p;return 1; }
'''
for name in ['packed_pixels_offset','packed_pixels_addr_1x1','get_block_step_bytes','argb_u16_from_u8888']:
    code += function(old, name)+'\n'
code += re.search(r'#define READ_LINE\(.*?\n}',old,re.S)[0]+'\n'
code += 'READ_LINE(old_read, px, u8, argb_u16_from_u8888, px[3], px[2], px[1], px[0])\n'
for name in ['pre_mul_blend_channel','pre_mul_alpha_blend']:
    code += function(blend,name)+'\n'
for name in ['vkms_blend_u8_channel','vkms_argb8888_blend_line']:
    code += function(new,name)+'\n'
code += r'''
static uint64_t rng=0x832439ae;
static unsigned random32(void) { rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng; }
int main(void) {
 /* Exhaust every 8-bit alpha/source value; test all 16-bit backgrounds across
    iterations plus extremes and malformed non-premultiplied input. */
 for(unsigned a=0;a<256;a++) for(unsigned s=0;s<256;s++) for(unsigned j=0;j<32;j++) {
  u16 d=j==0?0:j==1?65535:random32();
  if(vkms_blend_u8_channel(s,d,a*257)!=pre_mul_blend_channel(s*257,d,a*257)) return 1;
 }
 enum { W=65,H=67,PITCH=W*4+19,OFFSET=13 };
 u8 bytes[OFFSET+H*PITCH];
 struct drm_format_info fmt={{4,0}};
 struct drm_framebuffer fb={&fmt,{PITCH,0},{OFFSET,0}};
 struct vkms_frame_info info={&fb,{{bytes}}};
 struct vkms_plane_state plane={&info};
 struct pixel_argb_u16 stage[80],a[80],b[80];
 struct line_buffer st={80,stage},out={80,a};
 for(int iteration=0;iteration<256;iteration++) {
  for(unsigned i=0;i<sizeof(bytes);i++) bytes[i]=random32();
  for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
   u8 *p=bytes+OFFSET+y*PITCH+x*4;
   if((x+y)%4==0) memset(p,0,4);
   else if((x+y)%4==1) p[3]=255;
   else if((x+y)%4==2) p[3]=iteration;
  }
  for(int dir=0;dir<4;dir++) for(int n=0;n<=W;n++) {
   int x=(dir==READ_RIGHT_TO_LEFT)?W-1:0;
   int y=(dir==READ_BOTTOM_TO_TOP)?H-1:1;
   for(int i=0;i<80;i++) a[i]=b[i]=(struct pixel_argb_u16){random32(),random32(),random32(),random32()};
   old_read(&plane,x,y,dir,n,stage+3);
   pre_mul_alpha_blend(&st,&out,3,n);
   vkms_argb8888_blend_line(&plane,x,y,dir,n,b+3);
   if(memcmp(a,b,sizeof(a))) { fprintf(stderr,"fusion mismatch dir=%d n=%d iteration=%d\n",dir,n,iteration);return 1; }
  }
 }
 puts("PASS: fused ARGB8888 vs original reader + blend; 2097152 channel cases, all directions, offsets, padded pitch, row guards");
}
'''
with tempfile.TemporaryDirectory(prefix='zenith-vkms-fusion-') as tmp:
    c=Path(tmp)/'check.c';binary=Path(tmp)/'check';c.write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-fwrapv',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
