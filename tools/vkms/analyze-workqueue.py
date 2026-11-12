#!/usr/bin/env python3
"""Join trace-workqueue.sh events to monotonic timestamps in --profile CSV.
Worker wall time includes VKMS composition/CRC/writeback; running time subtracts
sched_switch intervals, then subtracts the union of recorded IRQ/softirq handler intervals.
"""
import argparse
import bisect
from collections import defaultdict, deque
import csv
import json
import math
from pathlib import Path
import re
import statistics

LINE = re.compile(r'^\s*.*-(\d+)\s+\[(\d+)\]\s+\S+\s+(\d+\.\d+): (\w+): (.*)$')
WORK = re.compile(r'work struct[= ]([0-9a-f]+)')
SWITCH = re.compile(r'prev_pid=(\d+).*next_pid=(\d+)')

def parse_trace(lines):
    events=[]
    for line in lines:
        m=LINE.match(line)
        if not m: continue
        pid,cpu,stamp,kind,body=m.groups()
        if kind=='sched_switch' or kind in ('irq_handler_entry','irq_handler_exit','softirq_entry','softirq_exit') or 'vkms_composer_worker' in body:
            events.append((round(float(stamp)*1e9),len(events),int(pid),int(cpu),kind,body))
    events.sort()
    queues=defaultdict(deque); active={}; workers=[]; incomplete=0
    irq_stacks=defaultdict(list); irq_intervals=defaultdict(list)
    for ns,_,pid,cpu,kind,body in events:
        if kind in ('irq_handler_entry','irq_handler_exit','softirq_entry','softirq_exit'):
            family=kind.rsplit('_',1)[0]; stack=irq_stacks[cpu,family]
            if kind.endswith('_entry'): stack.append(ns)
            elif stack: irq_intervals[cpu].append((stack.pop(),ns))
            continue
        if kind=='sched_switch':
            m=SWITCH.search(body)
            if not m: raise ValueError('unrecognized sched_switch: '+body)
            prev,nxt=map(int,m.groups())
            if prev in active:
                job=active[prev];job['off_start_ns']=ns
                job['segments'].append((job['resident_start_ns'],ns,cpu))
            if nxt in active:
                job=active[nxt]
                if job['off_start_ns'] is not None:
                    job['off_ns']+=ns-job['off_start_ns'];job['off_start_ns']=None
                job['resident_start_ns']=ns
            continue
        work=WORK.search(body).group(1)
        if kind=='workqueue_queue_work': queues[work].append(ns)
        elif kind=='workqueue_execute_start':
            if pid in active: raise ValueError('missing end event for worker')
            queued=queues[work].popleft() if queues[work] else None
            active[pid]={'work':work,'pid':pid,'cpu':cpu,'queued_ns':queued,'start_ns':ns,'off_ns':0,'off_start_ns':None,'resident_start_ns':ns,'segments':[]}
        elif kind=='workqueue_execute_end':
            job=active.pop(pid,None)
            if job is None: incomplete+=1;continue
            if job['work']!=work: raise ValueError('work identity mismatch')
            if job['off_start_ns'] is not None: raise ValueError('end event while worker off CPU')
            job['segments'].append((job['resident_start_ns'],ns,cpu))
            job['end_ns']=ns
            job['wall_ms']=(ns-job['start_ns'])/1e6
            job['off_cpu_ms']=job['off_ns']/1e6
            job['running_ms']=job['wall_ms']-job['off_cpu_ms']
            job['queue_ms']=None if job['queued_ns'] is None else (job['start_ns']-job['queued_ns'])/1e6
            workers.append(job)
    # Merge nested hard/soft IRQ intervals so interrupted softirq is counted once.
    merged={}
    for cpu,intervals in irq_intervals.items():
        spans=[]
        for a,b in sorted(intervals):
            if spans and a<=spans[-1][1]:spans[-1]=(spans[-1][0],max(b,spans[-1][1]))
            else:spans.append((a,b))
        merged[cpu]=([a for a,b in spans],spans)
    for job in workers:
        irq_ns=0
        for a,b,cpu in job['segments']:
            starts,spans=merged.get(cpu,([],[]))
            i=max(0,bisect.bisect_right(starts,a)-1)
            while i<len(spans) and spans[i][0]<b:
                x,y=spans[i];irq_ns+=max(0,min(b,y)-max(a,x));i+=1
        job['irq_ms']=irq_ns/1e6
        job['irq_events_observed']=bool(irq_intervals)
        path=[];residency=defaultdict(float)
        for a,b,cpu in job['segments']:
            residency[cpu]+=(b-a)/1e6
            if not path or path[-1]!=cpu:path.append(cpu)
        job['cpu_path']='>'.join(map(str,path))
        job['running_by_cpu_ms']=dict(residency)
        job['non_irq_running_ms']=job['running_ms']-job['irq_ms']
    return sorted(workers,key=lambda j:j['start_ns']),incomplete+len(active)

def stats(values):
    v=sorted(values)
    if not v:return {'count':0}
    return {'count':len(v),'mean':statistics.mean(v),'p50':statistics.median(v),
            'p95':v[math.ceil(len(v)*.95)-1],'p99':v[math.ceil(len(v)*.99)-1],'max':v[-1]}

def correlate(rows,workers):
    starts=[w['start_ns'] for w in workers];output=[]
    for r in rows:
        if r['warmup']!='0':continue
        begin,fence,end=(int(r[k]) for k in ('begin_mono_ns','fence_mono_ns','end_mono_ns'))
        # The one in-flight writeback starts in this frame before fence completion.
        # Its worker may return AFTER the fence wakes userspace; do not cap that tail.
        candidates=[w for w in workers[bisect.bisect_left(starts,begin):bisect.bisect_right(starts,fence)]
                    if w['wall_ms']>.05]
        row={'frame':int(r['frame']),'total_ms':float(r['total_ms']),'commit_ms':float(r['commit_ms']),
             'writeback_ms':float(r['writeback_ms']),'preview_ms':float(r['preview_ms']),
             'matching_workers':len(candidates)}
        if len(candidates)==1 and candidates[0]['queue_ms'] is not None:
            w=candidates[0]
            row.update({k:w[k] for k in ('queue_ms','wall_ms','off_cpu_ms','running_ms','irq_ms','non_irq_running_ms','cpu','cpu_path','running_by_cpu_ms')})
            row['begin_to_queue_ms']=(w['queued_ns']-begin)/1e6
            row['worker_end_to_fence_ms']=(fence-w['end_ns'])/1e6
            row['fence_to_end_ms']=(end-fence)/1e6
        output.append(row)
    return output

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace',type=Path);p.add_argument('profile',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--cpu-topology',type=Path,help='lscpu -J -e=CPU,CORE,MAXMHZ snapshot; optional frequency-group comparison')
    a=p.parse_args()
    stats_path=a.trace.parent/'buffer-stats.txt'
    if not stats_path.exists():p.error('capture is not complete: missing buffer-stats.txt')
    losses=re.findall(r'(?:^|\n)(?:commit overrun|overrun|dropped events):\s*(\d+)',stats_path.read_text())
    if not losses or any(int(n) for n in losses):p.error('trace loss or missing counters; do not infer timings from this capture')
    with a.trace.open() as f: workers,incomplete=parse_trace(f)
    with a.profile.open() as f: rows=list(csv.DictReader(f))
    joined=correlate(rows,workers)
    matched=[r for r in joined if 'queue_ms' in r];long=[r for r in matched if r['total_ms']>25]
    if not matched:p.error('no unambiguous matched frames; check trace clock and capture coverage')
    metrics=['total_ms','commit_ms','writeback_ms','preview_ms','queue_ms','wall_ms','off_cpu_ms','running_ms','irq_ms','non_irq_running_ms','begin_to_queue_ms','worker_end_to_fence_ms','fence_to_end_ms']
    report={'trace_loss':False,'irq_events_observed':any(w['irq_events_observed'] for w in workers),'measured_frames':len(joined),'matched_frames':len(matched),'incomplete_workers':incomplete,
            'all_workers':len(workers),'short_workers_under_50us':sum(w['wall_ms']<=.05 for w in workers),
            'all_matched':{k:stats([r[k] for r in matched]) for k in metrics},
            'long_frames':{k:stats([r[k] for r in long]) for k in metrics},
            'long_frame_details':long}
    if a.cpu_topology:
        topology=json.loads(a.cpu_topology.read_text())['cpus']
        maximums={int(c['cpu']):float(c['maxmhz']) for c in topology}
        highest=max(maximums.values())
        group=lambda r: ('higher_max_freq_only' if all(maximums[c]==highest for c in r['running_by_cpu_ms'])
                         else 'touched_lower_max_freq')
        report['cpu_groups']={}
        for name in ('higher_max_freq_only','touched_lower_max_freq'):
            subset=[r for r in matched if group(r)==name]
            report['cpu_groups'][name]={'frames':len(subset),'long_frames':sum(r['total_ms']>25 for r in subset),
                'non_irq_running_ms':stats([r['non_irq_running_ms'] for r in subset])}
        by_frame={r['frame']:r for r in matched}
        report['fast_long_after_lower_group']=[r['frame'] for r in long
            if group(r)=='higher_max_freq_only' and r['frame']-1 in by_frame
            and group(by_frame[r['frame']-1])=='touched_lower_max_freq']
        report['cpu_max_mhz']=maximums
    a.output.with_suffix('.json').write_text(json.dumps(report,indent=2)+'\n')
    with a.output.with_suffix('.csv').open('w',newline='') as f:
        keys=list(dict.fromkeys(k for r in joined for k in r));writer=csv.DictWriter(f,fieldnames=keys);writer.writeheader()
        writer.writerows({k:json.dumps(v,separators=(',',':')) if isinstance(v,dict) else v for k,v in r.items()} for r in joined)
    print(json.dumps({k:v for k,v in report.items() if k!='long_frame_details'},indent=2))
if __name__=='__main__':main()
