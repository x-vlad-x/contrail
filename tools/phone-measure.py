#!/usr/bin/env python3
"""Resource use of the running Contrail, for comparing releases (run on the phone, see tools/measure.sh).

  phone-measure.py snapshot LABEL      one TSV line: memory, CPU time, wakeups, network, battery
  phone-measure.py header              the TSV column names
  phone-measure.py inhibitors          what keeps the system from sleeping, and Contrail's part in it
  phone-measure.py threads SECONDS     which threads wake up how often (the background's profile)
  phone-measure.py resources           devices and streams Contrail holds: cameras, audio, sleep and idle locks

Memory comes from /proc/PID/smaps_rollup (RSS, PSS, private, swap, kB). CPU time is user + system in
seconds. Wakeups are the voluntary context switches of all the threads: each one is a thread that went
to sleep and was woken up again. Network bytes are summed over the process's TCP sockets (ss), which
only counts sockets still open. The battery columns are the fuel gauge's charge counter (µAh) and
status: an energy comparison needs the phone discharging, screen off, for an hour or more.
"""
import glob
import os
import re
import subprocess
import sys
import time

COLUMNS = ['time', 'label', 'pid', 'threads', 'rss_kb', 'pss_kb', 'private_kb', 'swap_kb', 'cpu_s', 'wakeups',
           'involuntary', 'tcp_sockets', 'net_sent', 'net_received', 'battery_uah', 'battery_status', 'gpu_kb']


def contrail_pid():
    for path in glob.glob('/proc/[0-9]*/cmdline'):
        try:
            with open(path, 'rb') as f:
                argv = f.read().split(b'\0')
        except OSError:
            continue
        # The development install (bin/contrail) or a package (libexec/contrail/contrail).
        if argv and argv[0].endswith((b'bin/contrail', b'libexec/contrail/contrail')):
            return int(path.split('/')[2])
    return None


def read(path, default=''):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def memory(pid):
    values = {}
    for line in read(f'/proc/{pid}/smaps_rollup').splitlines():
        match = re.match(r'(\w+):\s+(\d+) kB', line)
        if match:
            values[match.group(1)] = int(match.group(2))
    private = values.get('Private_Clean', 0) + values.get('Private_Dirty', 0)
    return values.get('Rss', 0), values.get('Pss', 0), private, values.get('Swap', 0)


def cpu_seconds(pid):
    fields = read(f'/proc/{pid}/stat').rsplit(')', 1)[-1].split()
    ticks = int(fields[11]) + int(fields[12])  # utime, stime
    return ticks / os.sysconf('SC_CLK_TCK')


def switches(pid):
    voluntary = involuntary = threads = 0
    for status in glob.glob(f'/proc/{pid}/task/*/status'):
        text = read(status)
        threads += 1
        voluntary += int(re.search(r'^voluntary_ctxt_switches:\s+(\d+)', text, re.M).group(1))
        involuntary += int(re.search(r'^nonvoluntary_ctxt_switches:\s+(\d+)', text, re.M).group(1))
    return threads, voluntary, involuntary


def network(pid):
    try:
        output = subprocess.run(['ss', '-tinpH'], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return 0, 0, 0
    sockets = sent = received = 0
    lines = output.splitlines()
    for i, line in enumerate(lines):
        if f'pid={pid},' not in line:
            continue
        sockets += 1
        info = lines[i + 1] if i + 1 < len(lines) else ''
        if m := re.search(r'bytes_sent:(\d+)', info):
            sent += int(m.group(1))
        if m := re.search(r'bytes_received:(\d+)', info):
            received += int(m.group(1))
    return sockets, sent, received


def battery():
    base = '/sys/class/power_supply/battery'
    return read(f'{base}/charge_counter', '0').strip(), read(f'{base}/status', '?').strip()


def gpu(pid):
    # Graphics buffers mapped by the process (dma-buf, Mali/PowerVR device memory): not in smaps' private.
    total = 0
    graphics = False
    for line in read(f'/proc/{pid}/smaps').splitlines():
        if re.match(r'^[0-9a-f]+-[0-9a-f]+ ', line):
            graphics = bool(re.search(r'/dev/(mali|dri|dma_heap)|dmabuf', line))
        elif graphics and line.startswith('Rss:'):
            total += int(line.split()[1])
    return total


def snapshot(label):
    pid = contrail_pid()
    if not pid:
        print('\t'.join([time.strftime('%H:%M:%S'), label, 'not-running']))
        return
    rss, pss, private, swap = memory(pid)
    threads, voluntary, involuntary = switches(pid)
    sockets, sent, received = network(pid)
    charge, status = battery()
    row = [time.strftime('%H:%M:%S'), label, pid, threads, rss, pss, private, swap, f'{cpu_seconds(pid):.2f}', voluntary,
           involuntary, sockets, sent, received, charge, status, gpu(pid)]
    print('\t'.join(str(v) for v in row))


def thread_switches(pid):
    result = {}
    for status in glob.glob(f'/proc/{pid}/task/*/status'):
        text = read(status)
        tid = status.split('/')[4]
        name = re.search(r'^Name:\s+(.*)$', text, re.M).group(1)
        result[tid] = (name, int(re.search(r'^voluntary_ctxt_switches:\s+(\d+)', text, re.M).group(1)),
                       int(fields_ticks(pid, tid)))
    return result


def fields_ticks(pid, tid):
    fields = read(f'/proc/{pid}/task/{tid}/stat').rsplit(')', 1)[-1].split()
    return int(fields[11]) + int(fields[12])


def threads(seconds):
    pid = contrail_pid()
    if not pid:
        print('not running')
        return
    before = thread_switches(pid)
    time.sleep(seconds)
    after = thread_switches(pid)
    rows = []
    for tid, (name, switches, ticks) in after.items():
        old = before.get(tid, (name, 0, 0))
        rows.append((switches - old[1], ticks - old[2], name, tid))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    print(f'{total} wakeups in {seconds} s ({total / seconds:.2f}/s), {len(rows)} threads')
    for wakeups, ticks, name, tid in rows:
        if wakeups:
            print(f'{wakeups:8d} {ticks / os.sysconf("SC_CLK_TCK"):7.2f}s  {name} ({tid})')


def resources():
    pid = contrail_pid()
    if not pid:
        print('not running')
        return
    devices = set()
    for fd in glob.glob(f'/proc/{pid}/fd/*'):
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        if target.startswith('/dev/') and not target.startswith(('/dev/null', '/dev/urandom', '/dev/pts', '/dev/ptmx')):
            devices.add(target)
    print('devices:', ' '.join(sorted(devices)) or 'none')
    for kind in ('source-outputs', 'sink-inputs'):
        try:
            output = subprocess.run(['pactl', 'list', kind], capture_output=True, text=True, timeout=10).stdout
        except (OSError, subprocess.TimeoutExpired):
            output = ''
        streams = [block for block in output.split('\n\n') if f'application.process.id = "{pid}"' in block]
        print(f'audio {kind}: {len(streams)}')
    try:
        paths = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.gnome.SessionManager', '--object-path',
                                '/org/gnome/SessionManager', '--method', 'org.gnome.SessionManager.GetInhibitors'],
                               capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        paths = ''
    ours = 0
    for path in re.findall(r"'(/org/gnome/SessionManager/Inhibitor\d+)'", paths):
        app = subprocess.run(['gdbus', 'call', '--session', '--dest', 'org.gnome.SessionManager', '--object-path', path,
                              '--method', 'org.gnome.SessionManager.Inhibitor.GetAppId'],
                             capture_output=True, text=True, timeout=10).stdout
        if 'Contrail' in app:
            ours += 1
    print(f'session inhibitors: {ours}')
    locks = [lock for lock in read('/sys/power/wake_lock').split() if 'contrail' in lock.lower()]
    print('kernel wake locks:', ' '.join(locks) or 'none')


def inhibitors():
    subprocess.run(['systemd-inhibit', '--list', '--no-pager'])
    pid = contrail_pid()
    if pid:
        # Wake locks taken through the kernel's interface (Android-style kernels) by Contrail's threads.
        locks = read('/sys/power/wake_lock').split()
        print('kernel wake locks:', ' '.join(locks) or 'none')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    if sys.argv[1] == 'header':
        print('\t'.join(COLUMNS))
    elif sys.argv[1] == 'snapshot':
        snapshot(sys.argv[2] if len(sys.argv) > 2 else '-')
    elif sys.argv[1] == 'inhibitors':
        inhibitors()
    elif sys.argv[1] == 'resources':
        resources()
    elif sys.argv[1] == 'threads':
        threads(int(sys.argv[2]) if len(sys.argv) > 2 else 60)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
