import sys
from pathlib import Path
ROOT = Path("/data/zhuo.wang/hbf_survey/HBFSim"); sys.path.insert(0, str(ROOT))
import hbfsim
from hbfsim_client import Transaction
PAGE, PAGES = 4096, 256
opts = {"hbf-external-direct-link-enable": "true",
        "hbf-external-direct-link-write-bw": 32, "hbf-external-direct-link-read-bw": 32,
        "hbf-external-direct-link-latency-ns": 100}
try:
    with hbfsim.open_session("miniquick/4hbm-4hbf",
            overlays=["ocp-v070-grade2", "hbf/external-direct-lane", "backing/cxl-ssd"],
            options=opts, out_dir=Path("/tmp/extlane"), overwrite=True,
            enable_external=True) as s:
        reqs = [Transaction(id=f"t{i}", target="DIRECT_EXTERNAL_TO_HBF", op="W",
                            addr=i*PAGE, bytes=PAGE, issue_ns=0.0, stack=0)
                for i in range(PAGES)]
        e = s.run(reqs, completions=False).elapsed_ns
        print(f"lane transport: {PAGES} pages in {e/1000:.1f} us = {PAGES*PAGE/e:.2f} GB/s")
except Exception as exc:
    print("ERR:", str(exc)[:300])
