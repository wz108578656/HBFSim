import sys
from pathlib import Path
ROOT = Path("/data/zhuo.wang/hbf_survey/HBFSim"); sys.path.insert(0, str(ROOT))
import hbfsim
from hbfsim_client import Transaction
PAGE, PAGES = 4096, 64
opts = {"hbf-external-direct-link-enable": "true",
        "hbf-external-direct-link-write-bw": 32, "hbf-external-direct-link-read-bw": 32,
        "hbf-external-direct-link-latency-ns": 100,
        "hbf-read-ns": 4000, "hbf-program-ns": 75000,
        "hbf-channels": 1, "hbf-dies-per-channel": 1, "hbf-planes-per-die": 16,
        "hbf-blocks-per-plane": 16, "hbf-pages-per-block": 16,
        "hbf-gc-low-watermark-pages": 64, "hbf-gc-hard-watermark-pages": 0,
        "hbf-gc-reserved-free-blocks-per-plane": 1, "hbf-write-coalescing": "false",
        "hbf-address-layout": "ocp-v070",
        "hbf-initial-free-blocks-erased": "true"}
with hbfsim.open_session("miniquick/4hbm-4hbf",
        overlays=["ocp-v070-grade2", "hbf/external-direct-lane", "backing/cxl-ssd"],
        options=opts, out_dir=Path("/tmp/extchain"), overwrite=True, enable_external=True,
        static_hbf_blocks_per_plane=0, published_hbf_blocks_per_plane=16) as s:
    reqs = []
    for i in range(PAGES):
        reqs.append(Transaction(id=f"x{i}", target="EXTERNAL", op="R", addr=i*PAGE,
                                bytes=PAGE, issue_ns=0.0))
        reqs.append(Transaction(id=f"l{i}", target="DIRECT_EXTERNAL_TO_HBF", op="W",
                                addr=i*PAGE, bytes=PAGE, issue_ns=0.0, stack=0,
                                dependencies=(f"x{i}",)))
        reqs.append(Transaction(id=f"p{i}", target="HBF_PHYSICAL", op="W", addr=i*PAGE,
                                bytes=PAGE, issue_ns=0.0, dependencies=(f"l{i}",)))
    e = s.run(reqs, completions=False).elapsed_ns
    print(f"external->lane->HBF: {PAGES} pages in {e/1000:.1f} us = {PAGES*PAGE/e:.3f} GB/s")
