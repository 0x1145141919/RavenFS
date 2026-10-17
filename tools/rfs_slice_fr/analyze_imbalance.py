#!/usr/bin/env python3
# analyze_imbalance.py — 清洗 / 研究 rfs_slice_fr 的分片不平衡
#
# 输入：rfs_slice_fr 的 trace（或 --json）。本脚本吃 trace 文本。
# 输出：清洗后的「每片表」+ 不平衡度量 + 连续分配容量曲线（可 --tsv 落盘供 diff）。
#
# 用法： python3 analyze_imbalance.py <slice_fr.log> [--tsv out.tsv]
import re, sys, math, statistics as st

def parse(path):
    geo = None; slices = {}; frs = {}
    rx_geo = re.compile(r'^\[TRACE\]\[BSLICE-GEO\] (.*)$')
    rx_sl  = re.compile(r'^\[TRACE\]\[BSLICE\] (.*)$')
    rx_fr  = re.compile(r'^\[TRACE\]\[BSLICE-FR\] (.*)$')
    def kv(s): return dict(t.split('=',1) for t in s.split())
    with open(path) as f:
        for line in f:
            m = rx_geo.match(line)
            if m: geo = kv(m.group(1)); continue
            m = rx_sl.match(line)
            if m:
                d = kv(m.group(1))
                slices[int(d['idx'])] = {k:int(v) for k,v in d.items()}
                continue
            m = rx_fr.match(line)
            if m:
                d = kv(m.group(1))
                frs.setdefault(int(d['idx']), []).append((int(d['base']), int(d['len'])))
    return geo, slices, frs

def main():
    if len(sys.argv) < 2:
        print("usage: analyze_imbalance.py <slice_fr.log> [--tsv out.tsv]"); return 2
    log = sys.argv[1]
    tsv = None
    if '--tsv' in sys.argv:
        tsv = sys.argv[sys.argv.index('--tsv')+1]

    geo, S, frs = parse(log)
    if not geo or not S:
        print("no BSLICE lines parsed"); return 2

    per   = int(geo['per_slice_bits'])
    nbits = int(geo['nbits'])
    n     = int(geo['slices'])
    GiB   = 1024**3
    idxs  = sorted(S)
    free  = [S[i]['free'] for i in idxs]
    used  = [S[i]['used'] for i in idxs]
    nfr   = [S[i]['fr']   for i in idxs]
    mx    = [S[i]['max_len'] for i in idxs]
    bits  = [S[i]['bits'] for i in idxs]
    fbytes= [x*4096 for x in free]
    tot_free = sum(free); tot_bits = sum(bits)

    print("="*96)
    print(f"GEO  cpu={geo['cpu']}({geo['cpu_src']})  slices={n}  per_slice={per} blk = {per*4096/GiB:.2f} GiB"
          f"  msb={geo['msb']}  nbits={nbits}  defrag={geo['defrag']}")
    print(f"SUM  free={tot_free:,} blk = {tot_free*4096/GiB:.1f} GiB  ({100*tot_free/tot_bits:.2f}% of slice space)")
    print("="*96)

    # ── 清洗后的每片表 ──
    print(f"{'idx':>3} {'begin_GiB':>10} {'span_GiB':>8} {'free_GiB':>8} {'used_GiB':>8} "
          f"{'free%':>6} {'fr':>6} {'maxFR_GiB':>9} {'minFR':>6} {'fr/blk':>8}  note")
    for i in idxs:
        b = S[i]['begin']; free_pct = 100.0*free[i]/bits[i] if bits[i] else 0
        note = ""
        if free[i] == 0: note = "SATURATED"
        elif nfr[i] == 1 and free[i] == bits[i]: note = "pristine"
        elif nfr[i] == 1: note = "1 run"
        print(f"{i:>3} {b*4096/GiB:>10.1f} {bits[i]*4096/GiB:>8.2f} {free[i]*4096/GiB:>8.1f} "
              f"{used[i]*4096/GiB:>8.1f} {free_pct:>5.1f}% {nfr[i]:>6} {mx[i]*4096/GiB:>9.2f} "
              f"{S[i]['min_len']:>6} {nfr[i]/(bits[i]/1024):>8.3f}  {note}")

    # ── 不平衡度量 ──
    def stats(x, name):
        mean = sum(x)/len(x)
        sd = st.pstdev(x)
        print(f"  {name:14s} min={min(x):>12,}  max={max(x):>12,}  mean={mean:>12,.0f}  "
              f"sd={sd:>12,.0f}  cv={sd/mean if mean else 0:>5.2f}")
    print("\n" + "-"*96 + "\n[不平衡度量]")
    stats(free, "free/片")
    stats(nfr,  "fr/片")
    stats(mx,   "maxFR/片")
    print(f"  free%          min={min(100*f/t for f,t in zip(free,bits)):.1f}%  "
          f"max={max(100*f/t for f,t in zip(free,bits)):.1f}%")

    sat = [i for i in idxs if free[i]==0]
    pristine = [i for i in idxs if nfr[i]==1 and free[i]==bits[i]]
    one_run = [i for i in idxs if nfr[i]==1 and free[i]!=bits[i]]
    print(f"  饱和片(free=0)     : {sat}")
    print(f"  全新片(1 run,满free): {pristine}")
    print(f"  单片(1 run,未满)   : {one_run}")
    frag = sorted(idxs, key=lambda i: -nfr[i])[:6]
    print(f"  最碎 6 片(fr 降序) : " + ", ".join(f"#{i}({nfr[i]})" for i in frag))

    # free 集中度
    ordered = sorted(idxs, key=lambda i: -free[i])
    cum = 0; marks = {1,2,4,8,16}
    print("  free 集中度        :", end="")
    for k,i in enumerate(ordered,1):
        cum += free[i]
        if k in marks: print(f" top{k}={100*cum/tot_free:.1f}%", end="")
    print()

    # ── 连续分配容量曲线（片内不拼接）──
    print("\n" + "-"*96 + "\n[连续分配容量曲线]  该请求 N 有多少片能整段吃下、合计可放几次")
    print(f"  {'request':>12} {'slices>=N':>10} {'placements':>12}  limiting")
    for N, lab in [(1,"4 KiB"),(256,"1 MiB"),(16384,"64 MiB"),(262144,"1 GiB"),
                   (2097152,"8 GiB"),(15269888,"58 GiB(perslice)")]:
        sv = 0; pl = 0; worst = None
        for i in idxs:
            cnt = sum(l//N for _,l in frs.get(i, []))
            if cnt: sv += 1; pl += cnt
        for i in idxs:
            if mx[i] < N: worst = (i, mx[i]); break
        print(f"  {lab:>12} {sv:>10} {pl:>12,}  {('idx %d maxFR=%d'%worst) if worst else '-'}")

    if tsv:
        with open(tsv, "w") as o:
            o.write("idx\tbegin\tbits\tfree\tused\tfree_pct\tfr\tmax_len\tmin_len\n")
            for i in idxs:
                o.write(f"{i}\t{S[i]['begin']}\t{bits[i]}\t{free[i]}\t{used[i]}\t"
                        f"{100*free[i]/bits[i]:.4f}\t{nfr[i]}\t{mx[i]}\t{S[i]['min_len']}\n")
        print(f"\n[tsv] wrote {tsv}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
