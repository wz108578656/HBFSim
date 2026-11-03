import sys
from pathlib import Path
ROOT = Path("/data/zhuo.wang/hbf_survey/HBFSim"); sys.path.insert(0, str(ROOT))
import hbfsim
from hbfsim_client import Transaction
PAGE = 4096
PAGES = 256

def ab(layout):
    opts = {"hbf-read-ns": 4000, "hbf-program-ns": 75000,
            "hbf-channels": 1, "hbf-dies-per-channel": 1, "hbf-planes-per-die": 16,
            "hbf-blocks-per-plane": 16, "hbf-pages-per-block": 16,
            "hbf-gc-low-watermark-pages": 64, "hbf-gc-hard-watermark-pages": 0,
            "hbf-gc-reserved-free-blocks-per-plane": 1,
            "hbf-write-coalescing": "false",
            "hbf-address-layout": layout}
    with hbfsim.open_session("4hbm-4hbf", overlays=["ocp-v070-grade2"], options=opts,
                             out_dir=Path(f"/tmp/abphys_{layout}"), overwrite=True,
                             static_hbf_blocks_per_plane=0,
                             published_hbf_blocks_per_plane=16) as s:
        writes = [Transaction(id=f"w{i}", target="HBF_PHYSICAL", op="W",
                              addr=i*PAGE, bytes=PAGE, issue_ns=0.0) for i in range(PAGES)]
        w_ns = s.run(writes, completions=False).elapsed_ns
        reads = [Transaction(id=f"r{i}", target="HBF_PHYSICAL", op="R",
                             addr=i*PAGE, bytes=PAGE, issue_ns=0.0) for i in range(PAGES)]
        r_ns = s.run(reads, completions=False).elapsed_ns
    return w_ns, r_ns

print("layout,write_us,read_us,write_GBps,read_GBps")
for layout in ("bank-major", "ocp-v070"):
    try:
        w, r = ab(layout)
        print(f"{layout},{w/1000:.1f},{r/1000:.1f},{PAGES*PAGE/w:.2f},{PAGES*PAGE/r:.2f}")
    except Exception as exc:
        print(f"{layout},ERR,ERR,{str(exc)[:140]}")
