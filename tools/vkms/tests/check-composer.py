#!/usr/bin/env python3
"""Exercise the extracted composer reference operations with concurrent users.
Usage: check-composer.py PATCHED_VKMS_DIR
"""
from pathlib import Path
import os
import runpy
import subprocess
import sys
import tempfile
if len(sys.argv) != 2:
    sys.exit(__doc__)
function = runpy.run_path(str(Path(__file__).with_name('check-math.py')))['function']
source=(Path(sys.argv[1])/'vkms_composer.c').read_text()
code=r'''
#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
struct drm_crtc { atomic_int references; };
struct vkms_output { pthread_mutex_t lock; unsigned composer_users; struct drm_crtc crtc; };
static atomic_int fail_get, synchronize_get;
static pthread_barrier_t barrier;
#define spin_lock_irq(p) pthread_mutex_lock(p)
#define spin_unlock_irq(p) pthread_mutex_unlock(p)
static int drm_crtc_vblank_get(struct drm_crtc *c) {
 if(atomic_load(&fail_get)) return -5;
 atomic_fetch_add(&c->references,1);
 if(atomic_load(&synchronize_get)) pthread_barrier_wait(&barrier);
 return 0;
}
static void drm_crtc_vblank_put(struct drm_crtc *c) {
 if(atomic_fetch_sub(&c->references,1)<=0) abort();
}
'''
for name in ['vkms_composer_get','vkms_composer_put']:
    code+=function(source,name)+'\n'
code+=r'''
static void check(struct vkms_output *o,unsigned users,int refs) {
 if(o->composer_users!=users||atomic_load(&o->crtc.references)!=refs) abort();
}
static void *get_once(void *p) { if(vkms_composer_get(p)) abort();return NULL; }
static void *stress(void *p) { for(int i=0;i<100000;i++) { if(vkms_composer_get(p)) abort();vkms_composer_put(p); }return NULL; }
int main(void) {
 struct vkms_output out={.lock=PTHREAD_MUTEX_INITIALIZER};
 atomic_store(&fail_get,1);if(vkms_composer_get(&out)!=-5) abort();check(&out,0,0);atomic_store(&fail_get,0);
 vkms_composer_get(&out);vkms_composer_get(&out);check(&out,2,1);
 vkms_composer_put(&out);check(&out,1,1); // old WB cleanup must preserve the next job / CRC
 vkms_composer_put(&out);check(&out,0,0);
 pthread_barrier_init(&barrier,NULL,2);atomic_store(&synchronize_get,1);
 pthread_t threads[4];
 for(int i=0;i<2;i++) pthread_create(&threads[i],NULL,get_once,&out);
 for(int i=0;i<2;i++) pthread_join(threads[i],NULL);
 check(&out,2,1);atomic_store(&synchronize_get,0);
 vkms_composer_put(&out);vkms_composer_put(&out);check(&out,0,0);
 for(int i=0;i<4;i++) pthread_create(&threads[i],NULL,stress,&out);
 for(int i=0;i<4;i++) pthread_join(threads[i],NULL);
 check(&out,0,0);pthread_barrier_destroy(&barrier);pthread_mutex_destroy(&out.lock);
 puts("PASS: concurrent first users, overlapping lifetimes, failed acquisition, 400000 concurrent get/put pairs");
}
'''
with tempfile.TemporaryDirectory(prefix='zenith-vkms-composer-') as tmp:
    c=Path(tmp)/'check.c';binary=Path(tmp)/'check';c.write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror','-pthread',
                    '-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
