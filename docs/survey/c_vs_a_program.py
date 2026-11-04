import sys
from pathlib import Path
ROOT = Path("/data/zhuo.wang/hbf_survey/HBFSim"); sys.path.insert(0, str(ROOT))
import hbfsim
from hbfsim_client import Transaction
PAGE = 4096
OPTS = {"hbf-external-direct-link-enable": "true",
        "hbf-external-direct-link-write-bw": 32, "hbf-external-direct-link-read-bw": 32,
        "hbf-external-direct-link-latency-ns": 100,
        "hbf-read-ns": 4000, "hbf-program-ns": 75000,
        "hbf-channels": 1, "hbf-dies-per-channel": 1, "hbf-planes-per-die": 16,
        "hbf-blocks-per-plane": 16, "hbf-pages-per-block": 16,
        "hbf-gc-low-watermark-pages": 64, "hbf-gc-hard-watermark-pages": 0,
        "hbf-gc-reserved-free-blocks-per-plane": 1, "hbf-write-coalescing": "false",
        "hbf-address-layout": "ocp-v070", "hbf-initial-free-blocks-erased": "true"}

def session():
    return hbfsim.open_session("miniquick/4hbm-4hbf",
        overlays=["ocp-v070-grade2", "hbf/external-direct-lane", "backing/cxl-ssd"],
        options=OPTS, out_dir=Path("/tmp/cva"), overwrite=True, enable_external=True,
        static_hbf_blocks_per_plane=0, published_hbf_blocks_per_plane=16)

def run(pages, kind):
    with session() as s:
        if kind == "external":
            reqs = [Transaction(id=f"x{i}", target="EXTERNAL", op="R", addr=i*PAGE,
                                bytes=PAGE, issue_ns=0.0) for i in range(pages)]
        elif kind == "lane":
            reqs = [Transaction(id=f"l{i}", target="DIRECT_EXTERNAL_TO_HBF", op="W",
                                addr=i*PAGE, bytes=PAGE, issue_ns=0.0, stack=0)
                    for i in range(pages)]
        elif kind == "hbf_write":
            reqs = [Transaction(id=f"p{i}", target="HBF_PHYSICAL", op="W", addr=i*PAGE,
                                bytes=PAGE, issue_ns=0.0) for i in range(pages)]
        else:  # chain
            reqs = []
            for i in range(pages):
                reqs.append(Transaction(id=f"x{i}", target="EXTERNAL", op="R", addr=i*PAGE,
                                        bytes=PAGE, issue_ns=0.0))
                reqs.append(Transaction(id=f"l{i}", target="DIRECT_EXTERNAL_TO_HBF", op="W",
                                        addr=i*PAGE, bytes=PAGE, issue_ns=0.0, stack=0,
                                        dependencies=(f"x{i}",)))
                reqs.append(Transaction(id=f"p{i}", target="HBF_PHYSICAL", op="W", addr=i*PAGE,
                                        bytes=PAGE, issue_ns=0.0, dependencies=(f"l{i}",)))
        return s.run(reqs, completions=False).elapsed_ns

for pages in (64, 256):
    t1 = run(pages, "external"); t2 = run(pages, "lane")
    t3 = run(pages, "hbf_write"); tc = run(pages, "chain")
    gb = lambda t: pages*PAGE/t
    print(f"pages={pages}")
    print(f"  ① EXTERNAL 读     {t1/1000:8.1f} us   {gb(t1):7.2f} GB/s")
    print(f"  ② lane 传输       {t2/1000:8.1f} us   {gb(t2):7.2f} GB/s")
    print(f"  ③ HBF 物理写      {t3/1000:8.1f} us   {gb(t3):7.2f} GB/s   ← A 的写方向")
    print(f"  C 端到端          {tc/1000:8.1f} us   {gb(tc):7.2f} GB/s")
    print(f"  ③ 占 C 的比例       {100*t3/tc:5.1f}%")
