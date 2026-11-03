import sys
from pathlib import Path
ROOT = Path("/data/zhuo.wang/hbf_survey/HBFSim"); sys.path.insert(0, str(ROOT))
import hbfsim
from hbfsim_client import Transaction
PAGE = 4096

def burst(layout, grade, read_ns, planes, dies, channels, pages, out, extra=None):
    opts = {"hbf-read-ns": read_ns, "hbf-address-layout": layout,
            "hbf-planes-per-die": planes, "hbf-dies-per-channel": dies,
            "hbf-channels": channels, "hbf-blocks-per-plane": 16,
            "hbf-pages-per-block": 16}
    if extra: opts.update(extra)
    with hbfsim.open_session("4hbm-4hbf", overlays=[f"ocp-v070-grade{grade}"], options=opts,
                             out_dir=out, overwrite=True, initial_hbf_logical_pages=pages) as s:
        reqs = [Transaction(id=f"r{i}", target="HBF_LOGICAL", op="R", addr=i*PAGE,
                            bytes=PAGE, issue_ns=0.0) for i in range(pages)]
        return s.run(reqs, completions=False).elapsed_ns

def steady(layout, grade, read_ns, planes, dies, channels, banks_per_round):
    e1 = burst(layout, grade, read_ns, planes, dies, channels, banks_per_round, Path(f"/tmp/ev_{layout}_{grade}_{read_ns}_1"))
    e5 = burst(layout, grade, read_ns, planes, dies, channels, banks_per_round*5, Path(f"/tmp/ev_{layout}_{grade}_{read_ns}_5"))
    total = 4*banks_per_round*PAGE/(e5-e1)
    return total, total/4, total/4/channels

print("=== 1) 规格带宽复现（4 stacks, 每通道 GB/s）===")
for grade, read_ns, planes, dies, channels in ((1,1000,16,1,8),(2,2000,16,4,4),(2,4000,16,4,4)):
    banks = 4*channels*dies*planes
    total, per_stack, per_ch = steady("bank-major", grade, read_ns, planes, dies, channels, banks)
    print(f"grade{grade} tR={read_ns}ns banks/ch={dies*planes}: per_channel={per_ch:.2f} GB/s (spec 48/96/192)")
