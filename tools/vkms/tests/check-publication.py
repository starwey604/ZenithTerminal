#!/usr/bin/env python3
"""Check actual writeback publication ordering with a worker at unlock time.
Usage: check-publication.py PATCHED_VKMS_DIR
"""
from pathlib import Path
import os,runpy,subprocess,sys,tempfile
if len(sys.argv)!=2: sys.exit(__doc__)
function=runpy.run_path(str(Path(__file__).with_name('check-math.py')))['function']
s=(Path(sys.argv[1])/'vkms_writeback.c').read_text()
code=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint16_t u16;typedef uint32_t u32;
struct rect { int x,y,w,h; };
struct format { u32 format; };
struct drm_framebuffer { struct format *format; };
struct vkms_frame_info { struct rect src,dst; };
struct vkms_writeback_job { struct vkms_frame_info wb_frame_info; int pixel_write; bool composer_ref; };
struct drm_writeback_job { struct drm_framebuffer *fb; void *priv; int *out_fence; };
struct drm_connector_state { void *crtc; struct drm_writeback_job *writeback_job; };
struct drm_connector { struct drm_connector_state *state; };
struct drm_writeback_connector { struct drm_connector base; };
struct drm_atomic_commit { int unused; };
struct vkms_crtc_state { struct { struct { int vdisplay,hdisplay; } mode; } base; struct vkms_writeback_job *active_writeback; bool wb_pending; };
struct vkms_output { struct drm_writeback_connector wb_connector; struct vkms_crtc_state *composer_state; int composer_lock; };
static struct vkms_output *current;static int queued,fail_get,cleaned,signaled,error,observed;
static struct drm_connector_state *drm_atomic_get_new_connector_state(struct drm_atomic_commit *s,struct drm_connector *c) {(void)s;return c->state;}
static struct vkms_output *drm_crtc_to_vkms_output(void *p) {return p;}
static int vkms_composer_get(struct vkms_output *o) {(void)o;return fail_get?-5:0;}
static void dma_fence_set_error(int *f,int e) {(void)f;error=e;}
static void dma_fence_signal(int *f) {(void)f;signaled++;}
static void drm_writeback_cleanup_job(struct drm_writeback_job *j) {(void)j;cleaned++;}
static int get_pixel_write_function(u32 format) {(void)format;return 42;}
static void drm_rect_init(struct rect *r,int x,int y,int w,int h) {*r=(struct rect){x,y,w,h};}
static void drm_writeback_queue_job(struct drm_writeback_connector *c,struct drm_connector_state *s) {(void)c;queued++;s->writeback_job=NULL;}
static void spin_lock_irq(int *p) {(void)p;}
static void spin_unlock_irq(int *p) {
 (void)p;struct vkms_crtc_state *s=current->composer_state;
 if(s->wb_pending) {
  struct vkms_writeback_job *j=s->active_writeback;
  if(!queued||!j||!j->composer_ref||j->pixel_write!=42||j->wb_frame_info.src.w!=1920||j->wb_frame_info.dst.h!=1080) abort();
  observed++;
 }
}
'''
code+=function(s,'vkms_wb_atomic_commit')+r'''
int main(void) {
 for(int fail=0;fail<2;fail++) {
  struct format fmt={1};struct drm_framebuffer fb={&fmt};struct vkms_writeback_job private={0};int fence=0;
  struct drm_writeback_job job={&fb,&private,&fence};struct vkms_crtc_state state={.base.mode={1080,1920}};
  struct vkms_output output={.composer_state=&state};struct drm_connector_state conn={&output,&job};
  output.wb_connector.base.state=&conn;current=&output;queued=cleaned=signaled=observed=error=0;fail_get=fail;
  vkms_wb_atomic_commit(&output.wb_connector.base,NULL);
  if(fail) {if(queued||observed||!cleaned||!signaled||error!=-5||private.composer_ref||conn.writeback_job) abort();}
  else if(queued!=1||observed!=1||cleaned||signaled||conn.writeback_job) abort();
 }
 puts("PASS: worker observes only initialized/queued job; acquisition failure signals own fence and cleans unqueued job");
}
'''
with tempfile.TemporaryDirectory(prefix='zenith-vkms-publish-') as tmp:
 c=Path(tmp)/'check.c';binary=Path(tmp)/'check';c.write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
