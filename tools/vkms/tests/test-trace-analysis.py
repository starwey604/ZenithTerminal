#!/usr/bin/env python3
"""Deterministic trace fixtures: requeue, migration, nested IRQ and frame matching."""
from pathlib import Path
import runpy
import unittest
import subprocess
import sys
import tempfile
api=runpy.run_path(str(Path(__file__).resolve().parents[1]/'analyze-workqueue.py'))
parse_trace,correlate=api['parse_trace'],api['correlate']
def event(t,kind,body,pid=100,cpu=0):
    return f' kworker/u1:0-{pid} [{cpu:03}] ..... {t:.6f}: {kind}: {body}\n'
def work(t,kind,cpu=0):
    return event(t,'workqueue_'+kind,'work struct abc: function vkms_composer_worker [vkms]',cpu=cpu)
class TimingTests(unittest.TestCase):
    def fixture(self):
        return [work(.001,'queue_work'),work(.002,'execute_start'),
                event(.0025,'irq_handler_entry','irq=1'),event(.0035,'irq_handler_exit','irq=1'),
                event(.004,'sched_switch','prev_pid=100 ==> next_pid=200'),
                event(.006,'sched_switch','prev_pid=200 ==> next_pid=100',pid=200,cpu=1),
                event(.007,'softirq_entry','vec=1',cpu=1),
                event(.0072,'irq_handler_entry','irq=2',cpu=1),event(.0074,'irq_handler_exit','irq=2',cpu=1),
                event(.0085,'softirq_exit','vec=1',cpu=1),
                work(.009,'queue_work'),work(.010,'execute_end',1),
                work(.012,'execute_start',1),work(.01201,'execute_end',1)]
    def test_queue_execution_migration_and_nested_irq(self):
        w,incomplete=parse_trace(self.fixture());self.assertEqual(incomplete,0);self.assertEqual(len(w),2)
        for k,v in {'queue_ms':1,'wall_ms':8,'off_cpu_ms':2,'running_ms':6,'irq_ms':2.5,'non_irq_running_ms':3.5}.items():
            self.assertAlmostEqual(w[0][k],v,msg=k)
        self.assertAlmostEqual(w[1]['queue_ms'],3)
    def row(self):
        return dict(frame='7',warmup='0',begin_mono_ns='0',fence_mono_ns='10100000',end_mono_ns='11000000',
                    total_ms='11',commit_ms='1',writeback_ms='8',preview_ms='1')
    def test_frame_and_empty_work(self):
        w,_=parse_trace(self.fixture());r=correlate([self.row()],w)[0]
        self.assertEqual(r['matching_workers'],1);self.assertEqual(r['queue_ms'],1)
    def test_fence_can_wake_before_worker_returns(self):
        w,_=parse_trace(self.fixture());row=self.row();row['fence_mono_ns']='9800000'
        r=correlate([row],w)[0]
        self.assertEqual(r['matching_workers'],1)
        self.assertAlmostEqual(r['worker_end_to_fence_ms'],-.2)
    def test_lost_trace_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);(d/'events.txt').write_text('');(d/'buffer-stats.txt').write_text('overrun: 1\n')
            result=subprocess.run([sys.executable,str(Path(__file__).resolve().parents[1]/'analyze-workqueue.py'),
                str(d/'events.txt'),str(d/'unused.csv'),str(d/'out')],capture_output=True,text=True)
            self.assertEqual(result.returncode,2);self.assertIn('trace loss',result.stderr)
    def test_missing_queue_not_fabricated(self):
        w,_=parse_trace(self.fixture()[1:]);r=correlate([self.row()],w)[0]
        self.assertNotIn('queue_ms',r)
    def test_ambiguous_match_not_silently_selected(self):
        w,_=parse_trace(self.fixture());r=correlate([self.row()],[w[0],w[0]])[0]
        self.assertEqual(r['matching_workers'],2);self.assertNotIn('queue_ms',r)
if __name__=='__main__':unittest.main()
