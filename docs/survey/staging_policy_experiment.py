"""Minimal page-granular staging experiment for the HBF survey.

Two policies are compared on a per-SM page stream:

  P1 demand-paced (SlipStream-like): the staging lead is bounded by a window
     S (an entry retires only when demand first touches the page), and buffer
     slots are reserved as pages are issued.
  P2 retire-on-fill (tensor prefetch, H3-like): staging runs as far ahead as
     the buffer allows and an entry retires when the page returns.

Both are driven by the cold-page latency measured on HBFSim, so the service
model is not a guess: the script first runs a real session at tR = 5/10/20 us,
then replays the policy timeline with that latency.

Usage: python3 docs/survey/staging_policy_experiment.py
"""
from __future__ import annotations

import heapq
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

import hbfsim  # noqa: E402
from hbfsim_client import Transaction  # noqa: E402

PAGE = 4096
TB = 1.0          # demand interval per page (us): one page per us per SM
K = 2000          # pages in one per-SM layer segment
S_WINDOW = 90     # SlipStream-like staging lead (pages)
T_READ_US = (5, 10, 20)


def measure_latency(t_read_ns: int, pages: int = 8) -> float:
    """Cold-page read latency and sustained page rate from a real session."""
    opts = {
        "hbf-read-ns": t_read_ns,
        "hbf-channels": 1, "hbf-dies-per-channel": 1, "hbf-planes-per-die": 16,
        "hbf-blocks-per-plane": 16, "hbf-pages-per-block": 16,
        "hbf-address-layout": "ocp-v070",
        "hbf-blocks-per-plane": 16, "hbf-pages-per-block": 16,
        "hbf-gc-low-watermark-pages": 64, "hbf-gc-hard-watermark-pages": 0,
        "hbf-gc-reserved-free-blocks-per-plane": 1,
        "hbf-write-coalescing": "false",
    }
    with hbfsim.open_session(
        "miniquick/4hbm-4hbf", overlays=["ocp-v070-grade2"], options=opts,
        out_dir=Path(f"/tmp/stage_cal_{t_read_ns}"), overwrite=True,
        initial_hbf_logical_pages=128,
    ) as s:
        single = [Transaction(id="one", target="HBF_LOGICAL", op="R", addr=0,
                              bytes=PAGE, issue_ns=0.0)]
        t_one = s.run(single, completions=False).elapsed_ns / 1000.0
        burst = [Transaction(id=f"b{i}", target="HBF_LOGICAL", op="R",
                             addr=i * PAGE, bytes=PAGE, issue_ns=0.0)
                 for i in range(pages)]
        t_burst = s.run(burst, completions=False).elapsed_ns / 1000.0
    return t_one


@dataclass
class Result:
    policy: str
    latency_us: float
    lead_pages: int
    makespan_us: float

    @property
    def slowdown(self) -> float:
        return self.makespan_us / (K * TB + self.latency_us)


def simulate(latency_us: float, lead_pages: int, k: int = K, tb: float = TB) -> Result:
    """Demand-paced staging with a bounded lead (entries retire at first demand).

    The producer issues page i only while i - consumed < lead_pages, so the lead
    is bounded by the tracking budget rather than by the buffer or the latency.
    A page returns after `latency_us` and the consumer takes one page per `tb`.
    Ideal makespan is k*tb + latency.
    """
    next_issue = 0
    consumed = 0
    now = 0.0
    arrival = [0.0] * k
    while consumed < k:
        while next_issue < k and next_issue - consumed < lead_pages:
            arrival[next_issue] = now + latency_us if next_issue < consumed + lead_pages else now + latency_us
            next_issue += 1
        now = max(now, arrival[consumed]) + tb
        consumed += 1
    return Result("demand-paced", latency_us, lead_pages, now)


def main() -> None:
    print("== HBFSim calibration (1 channel, 16 planes, ocp layout) ==")
    lat = {}
    for t_read in T_READ_US:
        t_one = measure_latency(t_read * 1000)
        lat[t_read] = t_one
        print(f"  tR={t_read:2d} us -> cold page {t_one:6.2f} us")
    print()

    # 4 KB page tracking: a 90-page lead is what a 40 MB buffer covers per SM.
    # Line tracking: 256 entries x 128 B = 8 pages per SM (CAPS/Snake budget).
    schemes = [("page-granular, 90-page lead", 90),
               ("line-granular, 256 entries = 8 pages", 8)]
    print("== slowdown vs ideal-HBF ==")
    print(f"{'scheme':40s}" + "".join(f"{l:>9d}us" for l in T_READ_US))
    for name, lead in schemes:
        line = f"{name:40s}"
        for t_read in T_READ_US:
            r = simulate(lat[t_read], lead)
            line += f"{r.slowdown:>10.3f}x"
        print(line)

    print("\n== lead needed for within-1% of ideal, per latency ==")
    for t_read in T_READ_US:
        need = next(n for n in range(1, 200)
                    if simulate(lat[t_read], n).slowdown <= 1.01)
        print(f"  tR={t_read:2d} us -> {need} pages of lead "
              f"({need * 4} KB per SM)")


if __name__ == "__main__":
    main()
