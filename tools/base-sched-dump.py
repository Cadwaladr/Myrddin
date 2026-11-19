# Run by perf itself (perf script -s tools/base-sched-dump.py -i sched.data), for tools/base-critpath.py:
# writes the scheduler events of a recording as short lines, with the context a wake-up came from
# (flags: 0x08 = from a hardware interrupt, 0x10 = from a soft interrupt).
#   S time_ns cpu prev_tid prev_state next_tid
#   W time_ns cpu waker_tid woken_tid target_cpu flags
#   N tid name
import sys

try:
    from perf_trace_context import common_flags
except ImportError:  # (not inside perf)
    def common_flags(context):
        return -1

out = sys.stdout
names = {}


def name(tid, comm):
    if tid and names.get(tid) != comm:
        names[tid] = comm
        out.write("N %d %s\n" % (tid, comm))


def sched__sched_switch(event_name, context, common_cpu, common_secs, common_nsecs, common_pid, common_comm,
                        common_callchain, prev_comm, prev_pid, prev_prio, prev_state, next_comm, next_pid, next_prio, *rest):
    name(prev_pid, prev_comm)
    name(next_pid, next_comm)
    out.write("S %d %d %d %d %d\n" % (common_secs * 1000000000 + common_nsecs, common_cpu, prev_pid, prev_state, next_pid))


def sched__sched_waking(event_name, context, common_cpu, common_secs, common_nsecs, common_pid, common_comm,
                        common_callchain, comm, pid, prio, target_cpu, *rest):
    name(pid, comm)
    out.write("W %d %d %d %d %d %d\n" % (common_secs * 1000000000 + common_nsecs, common_cpu, common_pid, pid, target_cpu, common_flags(context)))


def trace_end():
    out.flush()
