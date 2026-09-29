import os, sys, subprocess, bisect, collections
elf, prof = sys.argv[1], sys.argv[2]
syms = []
for l in subprocess.run([os.environ.get("CROSS", "riscv-none-elf-") + "nm", "-n", "-S", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and p[2] in "tTwW": syms.append((int(p[0],16), int(p[1],16), p[3]))
addrs = [s[0] for s in syms]
agg = collections.Counter(); tot = 0
for l in open(prof):
    pc, n = l.split(); pc = int(pc,16); n = int(n); tot += n
    i = bisect.bisect_right(addrs, pc) - 1
    name = syms[i][2] if i >= 0 and pc < syms[i][0] + max(syms[i][1],1) else "?%x" % pc
    agg[name] += n
for name, n in agg.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 20):
    print("%10d %5.1f%%  %s" % (n, 100.0*n/tot, name))
print("total", tot)
