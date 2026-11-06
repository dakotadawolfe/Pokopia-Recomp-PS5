#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise the actual patched retirement/destroy functions with a controlled GPU queue.

Run after build_graphics_radv_partition.py --retire-buffers, passing its
radv_ps5_bo.c. This is a host lifetime test, not PS5 submission validation.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patched_source", type=Path)
    args = parser.parse_args()
    source = args.patched_source.read_text()
    retirement = source[source.index("struct radv_ps5_retired {"):source.index("/* Memory reports")]
    destroy = source[source.index("static void\nradv_ps5_buffer_destroy("):source.index("static void *\nradv_ps5_buffer_map(")]
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define OS_TIMEOUT_INFINITE UINT64_MAX
#define RADEON_DOMAIN_VRAM 1
#define p_atomic_read(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define p_atomic_add(p,v) __atomic_fetch_add(p,v,__ATOMIC_ACQ_REL)
typedef pthread_mutex_t simple_mtx_t;
#define SIMPLE_MTX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define simple_mtx_lock pthread_mutex_lock
#define simple_mtx_unlock pthread_mutex_unlock
struct util_dynarray { _Alignas(16) uint8_t data[65536]; unsigned size; };
#define util_dynarray_foreach(a,t,p) for(t *p=(t *)(a)->data; (uint8_t *)p<(a)->data+(a)->size; ++p)
#define util_dynarray_element(a,t,n) (((t *)(a)->data)+(n))
#define util_dynarray_append(a,v) do { assert((a)->size+sizeof(v)<=sizeof((a)->data)); memcpy((a)->data+(a)->size,&(v),sizeof(v)); (a)->size+=sizeof(v); } while(0)
struct radv_ps5_memory { void *cpu; uint64_t bytes; int64_t physical; };
struct radv_ps5_queue { uint64_t submitted_seq, completed; bool wait_ok; unsigned waits; };
struct radeon_winsys { int unused; };
struct radeon_winsys_bo { unsigned initial_domain; };
struct radv_ps5_winsys { struct radeon_winsys base; struct radv_ps5_queue *queue; int64_t allocated_vram, allocated_gtt; };
struct radv_ps5_bo { struct radeon_winsys_bo base; struct radv_ps5_memory memory; bool sparse, imported; void *placed; };
static uint64_t radv_ps5_allocated_bytes;
static unsigned freed, sparse_freed, placed_unmapped;
static int64_t released[32];
static struct radv_ps5_winsys *radv_ps5_winsys(struct radeon_winsys *w) { return (void *)w; }
static struct radv_ps5_bo *radv_ps5_bo(struct radeon_winsys_bo *b) { return (void *)b; }
static uint64_t radv_ps5_queue_poll(struct radv_ps5_queue *q) { return q->completed; }
static bool radv_ps5_queue_wait_seq(struct radv_ps5_queue *q,uint64_t seq,uint64_t timeout) {
    assert(timeout==OS_TIMEOUT_INFINITE); q->waits++;
    if (!q->wait_ok) return false;
    q->completed=seq; return true;
}
static void radv_ps5_memory_free(struct radv_ps5_memory *m) { released[freed++]=m->physical; }
static void radv_ps5_vrange_release(struct radv_ps5_memory *m) { (void)m; sparse_freed++; }
static void radv_ps5_memory_unmap_at(void *p,uint64_t size,bool replace) { assert(p && size && !replace); placed_unmapped++; }
'''
    checks = r'''
int main(void) {
    struct radv_ps5_queue q={.submitted_seq=3,.completed=1,.wait_ok=true};
    struct radv_ps5_memory a={.physical=101,.bytes=16384}, b={.physical=102,.bytes=16384};
    radv_ps5_memory_retire(&q,&a,false);
    q.submitted_seq=4; radv_ps5_memory_retire(&q,&b,false);
    assert(freed==0 && radv_ps5_retired.size==2*sizeof(struct radv_ps5_retired));
    q.completed=3;
    assert(radv_ps5_retired_free(&q)==4 && freed==1 && released[0]==101);
    q.wait_ok=false;
    assert(!radv_ps5_retired_wait(&q) && freed==1 && q.waits==1);
    q.wait_ok=true;
    assert(radv_ps5_retired_wait(&q) && freed==2 && released[1]==102 && q.waits==2);
    assert(!radv_ps5_retired_wait(&q) && q.waits==2);
    radv_ps5_memory_retire(&q,&a,false);
    assert(freed==3 && radv_ps5_retired.size==0);
    struct radv_ps5_winsys ws={.queue=&q,.allocated_vram=16384};
    struct radv_ps5_bo *bo=calloc(1,sizeof(*bo));
    bo->imported=true; bo->memory=a;
    radv_ps5_buffer_destroy(&ws.base,&bo->base);
    assert(freed==3 && sparse_freed==0);
    q.submitted_seq=5;
    bo=calloc(1,sizeof(*bo)); bo->sparse=true; bo->memory=b;
    radv_ps5_buffer_destroy(&ws.base,&bo->base);
    assert(sparse_freed==0);
    bo=calloc(1,sizeof(*bo)); bo->base.initial_domain=RADEON_DOMAIN_VRAM;
    bo->memory=a; bo->placed=(void *)0x10000; radv_ps5_allocated_bytes=16384;
    radv_ps5_buffer_destroy(&ws.base,&bo->base);
    assert(freed==3 && placed_unmapped==1 && ws.allocated_vram==0);
    q.completed=5; assert(radv_ps5_retired_free(&q)==0);
    assert(freed==4 && sparse_freed==1 && radv_ps5_retired.size==0);
    puts("retirement: pending, partial completion, wait failure/retry, imported ownership, sparse and placed mappings passed");
}
'''
    with tempfile.TemporaryDirectory(prefix="pokopia-retirement-") as directory:
        root = Path(directory)
        path = root / "test.c"
        path.write_text(prelude + retirement + destroy + checks)
        executable = root / "test"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-pthread", str(path), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
