import sys
from pathlib import Path
ROOT = Path("/data/zhuo.wang/hbf_survey/HBFSim"); sys.path.insert(0, str(ROOT))
import hbfsim
from hbfsim_client import Transaction
PAGE, PAGES = 4096, 256
OPTS = {"hbf-read-ns": 4000, "hbf-program-ns": 75000,
        "hbf-channels": 1, "hbf-dies-per-channel": 1, "hbf-planes-per-die": 16,
        "hbf-blocks-per-plane": 16, "hbf-pages-per-block": 16,
        "hbf-gc-low-watermark-pages": 64, "hbf-gc-hard-watermark-pages": 0,
        "hbf-gc-reserved-free-blocks-per-plane": 1, "hbf-write-coalescing": "false"}
def ab(layout):
    opts = dict(OPTS, **{"hbf-address-layout": layout})
    with hbfsim.open_session("4hbm-4hbf", overlays=["ocp-v070-grade2"], options=opts,
                             out_dir=Path(f"/tmp/forkab_{layout}"), overwrite=True,
                             static_hbf_blocks_per_plane=0,
                             published_hbf_blocks_per_plane=16) as s:
        w = [Transaction(id=f"w{i}", target="HBF_PHYSICAL", op="W", addr=i*PAGE,
                         bytes=PAGE, issue_ns=0.0) for i in range(PAGES)]
        wn = s.run(w, completions=False).elapsed_ns
        r = [Transaction(id=f"r{i}", target="HBF_PHYSICAL", op="R", addr=i*PAGE,
                         bytes=PAGE, issue_ns=0.0) for i in range(PAGES)]
        rn = s.run(r, completions=False).elapsed_ns
    return wn, rn
print("layout,write_ms,read_us,read_GBps")
for layout in ("bank-major", "ocp-v070"):
    w, r = ab(layout)
    print(f"{layout},{w/1e6:.1f},{r/1e3:.1f},{PAGES*PAGE/r:.2f}")
