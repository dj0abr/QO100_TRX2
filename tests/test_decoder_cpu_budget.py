"""CPU accounting includes threads, separate solver PGID and the guard itself."""
from pathlib import Path
import os
import resource
import signal
import subprocess
import sys
import time

program=sys.argv[1]
for percent,threads in ((10,1),(10,4),(5,2)):
    before=resource.getrusage(resource.RUSAGE_CHILDREN)
    started=time.monotonic()
    result=subprocess.run([program,str(percent),str(threads),'4'],capture_output=True,text=True,timeout=8)
    elapsed=time.monotonic()-started
    after=resource.getrusage(resource.RUSAGE_CHILDREN)
    used=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime
    measured=100*used/elapsed
    assert result.returncode==0,result.stderr+result.stdout
    # Scheduler granularity/final accounting allows a small absolute tolerance.
    assert 0<measured<percent+1,(percent,threads,measured,elapsed)
    print(f'Budget {percent}%: threads={threads}+separate child group; total measured={measured:.2f}% of one core',flush=True)


# An idle monitor must not burn its quota just on CPU accounting.
before=resource.getrusage(resource.RUSAGE_CHILDREN);start=time.monotonic()
subprocess.run([program,'10','0','3'],capture_output=True,check=True,timeout=5)
after=resource.getrusage(resource.RUSAGE_CHILDREN)
percent=100*(after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime)/(time.monotonic()-start)
assert percent<2,('Idle guard overhead',percent)
print(f'Idle CPU guard overhead: {percent:.2f}% of one core',flush=True)

# Stop during a throttled busy decode: no stopped child may be left behind.
process=subprocess.Popen([program,'10','3','30'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
seen=set()
def descendants(pid):
    found=set()
    tasks=Path(f'/proc/{pid}/task')
    for task in tasks.iterdir() if tasks.exists() else ():
        try:
            children=(task/'children').read_text().split()
        except (FileNotFoundError,ProcessLookupError):
            continue
        for child in children:
            child=int(child);found.add(child);found.update(descendants(child))
    return found
try:
    time.sleep(.4);seen=descendants(process.pid);assert len(seen)>=2,seen
    process.terminate();process.communicate(timeout=4)
    end=time.monotonic()+2
    while any(Path(f'/proc/{pid}').exists() for pid in seen) and time.monotonic()<end:time.sleep(.01)
    assert not any(Path(f'/proc/{pid}').exists() for pid in seen),seen
finally:
    if process.poll() is None:process.kill();process.communicate(timeout=3)
print('Throttled shutdown: all worker/solver descendants cleaned up',flush=True)

# An abrupt guard death must kill the stopped worker and its solver as well.
process=subprocess.Popen([program,'10','2','30'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
time.sleep(.4);seen=descendants(process.pid);assert len(seen)>=2,seen
process.kill();process.communicate(timeout=3)
def live(pid):
    try:
        stat=Path(f'/proc/{pid}/stat').read_text()
        return stat[stat.rfind(')')+2:].split()[0]!='Z'
    except (FileNotFoundError,ProcessLookupError):
        return False
end=time.monotonic()+2
while any(live(pid) for pid in seen) and time.monotonic()<end:time.sleep(.01)
assert not any(live(pid) for pid in seen),seen
print('Abrupt guard death: no stopped/running worker or solver survived',flush=True)
