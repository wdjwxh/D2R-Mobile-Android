"""Generate complete upstream gameplay hooks with Android host ABI adapters."""
from pathlib import Path
root=Path(__file__).resolve().parent.parent
up=root/'d2r-mobile/dm/host'
out=root/'android-host/generated'
out.mkdir(exist_ok=True)
for name in ('dm_townwp','hle_private_aim'):
    text=(up/(name+'.c')).read_text(encoding='utf-8')
    for header in ('hle.h','dm.h'):
        text=text.replace('#include "'+header+'"','#include "../src/dm_host.h"')
    text=text.replace('#include "dm_unit.h"','#include "../../d2r-mobile/dm/host/dm_unit.h"')
    text=text.replace('#include "dm_input.h"','void dm_input_sampled_right(int32_t *,int32_t *);')
    text=text.replace('#include <mach/mach.h>','#include <time.h>\nint dm_tg_vm_read(uint64_t,void *,size_t);\nstatic uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }')
    text=text.replace('    vm_size_t copied=0;\n    return a && a<=UINT64_MAX-n && vm_read_overwrite(mach_task_self(),a,n,(vm_address_t)out,&copied)==KERN_SUCCESS && copied==n;','    return dm_tg_vm_read(a,out,n);')
    text=text.replace('now_ns(),caller-(uint64_t)m->base,id,rx,ry,active','(unsigned long long)now_ns(),(unsigned long long)(caller-(uint64_t)m->base),id,rx,ry,active')
    (out/(name+'.c')).write_text(text,encoding='utf-8')
print('Complete upstream town waypoint and private aim hooks prepared')
