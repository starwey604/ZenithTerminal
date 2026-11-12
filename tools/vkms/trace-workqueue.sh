#!/usr/bin/env bash
# Capture VKMS enqueue events plus worker execution/scheduling, in a private instance.
# Run as root after loading the desired VKMS module. Never changes module/desktop ownership.
set -euo pipefail
if (( $# != 2 )) || [[ ! $1 =~ ^[1-9][0-9]*$ ]]; then
    echo "Usage: sudo $0 SECONDS NEW_OUTPUT_DIRECTORY" >&2; exit 2
fi
[[ $EUID == 0 ]] || { echo 'Tracefs requires root.' >&2; exit 1; }
seconds=$1
mkdir -- "$2"
output=$(realpath -- "$2")
[[ -d /sys/module/vkms ]] || { echo "VKMS is not loaded." >&2; exit 1; }
instance=/sys/kernel/tracing/instances/zenith-vkms-$BASHPID
mkdir -- "$instance"
reader=''
cleanup() {
    echo 0 > "$instance/tracing_on"
    if [[ -n $reader ]]; then kill "$reader" 2>/dev/null || :; wait "$reader" 2>/dev/null || :; fi
    # Entries not yet drained by trace_pipe must also be retained.
    cat "$instance/trace" >> "$output/events.txt"
    for stats in "$instance"/per_cpu/cpu*/stats; do
        printf '%s\n' "$stats"; cat "$stats"
    done > "$output/buffer-stats.txt"
    rmdir -- "$instance"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
echo 0 > "$instance/tracing_on"
echo mono > "$instance/trace_clock"
echo 2048 > "$instance/buffer_size_kb"
echo 'workqueue ~ "*vkms*"' > "$instance/events/workqueue/workqueue_queue_work/filter"
# Start/end events have no queue name; retain them and select VKMS by symbol offline.
# This also works when the host deliberately hides kallsyms addresses.
for event in workqueue_queue_work workqueue_execute_start workqueue_execute_end; do
    echo 1 > "$instance/events/workqueue/$event/enable"
done
echo 'prev_comm ~ "kworker*" || next_comm ~ "kworker*"' > "$instance/events/sched/sched_switch/filter"
echo 1 > "$instance/events/sched/sched_switch/enable"
for event in irq_handler_entry irq_handler_exit softirq_entry softirq_exit; do
    echo 1 > "$instance/events/irq/$event/enable"
done
lscpu -J -e=CPU,CORE,MAXMHZ > "$output/cpu-topology.json"
cat /sys/module/vkms/srcversion > "$output/module-srcversion.txt"
# Queue settings are part of the experiment, not inferred from CPU topology.
{
    printf 'online='; cat /sys/devices/system/cpu/online
    printf 'global_unbound_cpumask='; cat /sys/devices/virtual/workqueue/cpumask
    shopt -s nullglob
    for queue in /sys/bus/workqueue/devices/*vkms*; do
        printf 'queue=%s\n' "$queue"
        for attr in cpumask nice max_active affinity_scope affinity_strict; do
            printf '%s=' "$attr"; cat "$queue/$attr"
        done
    done
} > "$output/workqueue-settings.txt"
cat "$instance/trace_clock" > "$output/trace-clock.txt"
echo 1 > "$instance/tracing_on"
cat "$instance/trace_pipe" > "$output/events.txt" & reader=$!
printf 'TRACE READY %s\n' "$output"
sleep "$seconds"
