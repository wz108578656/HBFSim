# R1: block read-disturb refresh — design input

> Status: Experimental
> Last reviewed: 2026-11-03

Scope: the refresh mechanism the current model deliberately omits
(`docs/reference/model.md` §"Retention failures, host refresh, disturb counters …
are outside this transaction model").

This note fixes the five ambiguities that the OCP specification leaves to the
product datasheet, so the implementation has one defensible definition per
choice. Every value below is an **exploratory assumption**: no HBF datasheet
publishes read-disturb thresholds, so none of it can claim vendor evidence. The
provenance entries in `configs/parameter-provenance.json` say exactly that.

## What the specification actually requires

| Requirement | Where | What it pins down | What it leaves open |
|---|---|---|---|
| Page read counter per block, reported to the host | p.19 | The counter exists and is per block | Counter width, counting unit |
| Data refresh when a block sees excess reads | p.118 §11.5 | Read count is a refresh trigger alongside retention time | The threshold value |
| Read response carries block health in `ruser[8:6]` | p.59-61 Table 12 | `0x1` = read count above threshold, `0x2` = within health | The threshold value |
| CECC (refresh required) / UECC (refresh required) | p.59-61, p.120 | The host decides when to refresh; the device only signals | How the host schedules it |
| Refresh must not overlap reads/writes to the same die | p.118 | Die-level exclusion during refresh | How to enforce it |
| Retention interval | p.118 | Product publishes it (typical 24–48 h) | The number |

The device never refreshes by itself: OCP §11.4 excludes device-side GC and
copyback, so refresh is a host action. The model therefore needs a host policy,
not a device timer.

## The five decisions

### D1 — What increments the counter

**One NAND sense of a page increments its block's counter by one.**

- A 4 KiB page read that causes a sense: `+1`.
- Several 64 B reads served from the same page's single sense share that one
  increment (the physical disturb happened once).
- A read served from the decoded page cache: `+0`. No array access happened, so
  no disturb was induced. This is also why the counter cannot be derived from
  host request counts.
- Writes, erases, ECC work and metadata traffic: no effect on this counter.

Rationale: read disturb is a property of the array, not of the request stream.
Counting senses keeps the metric identical across 64 B and 4 KiB request
granularity.

### D2 — Multi-plane and multi-die accounting

**Every physical block touched by a command increments on its own.** A
multi-plane sense that reads four planes adds one to each of the four plane
blocks; it never adds four to one block.

Rationale: the disturb is local to the block's wordlines. Plane-level blocks are
the erase units, and the refresh action rewrites one physical block at a time.

### D3 — Threshold value and temperature dependence

**One global threshold per device, default disabled (0), example value 100 000
reads.** The threshold does not change with temperature.

- `hbf-read-count-threshold=0` keeps the model exactly as it is today (no
  counter checks, no status bits, no refresh). This keeps every existing result
  reproducible.
- Temperature affects NAND disturb in silicon, but the model has no calibrated
  coupling between junction temperature and threshold. Inventing one would turn
  an exploratory assumption into a fake measurement, so the threshold stays flat
  and the thermal model keeps affecting throughput only.

### D4 — Retention path

**Separate trigger, same action: `hbf-retention-ns`, default 0 (disabled),
example 24 h.** A block whose **oldest** page is older than the interval is a
refresh candidate even with a low read count. Age is measured from the first
program after the last erase, not from the most recent program: retention risk
follows the oldest data in the block, so programming young pages must never push
the deadline out.

Both triggers feed the same host policy. The retention interval is the other
half of OCP §11.5 and is trivial to support once the read path exists, so it is
included from the start instead of being deferred again.

### D5 — Counter width, action and accounting

- **Width**: `uint64_t` per block, never saturating. A product may publish a
  16-bit counter; widening it cannot lose the threshold crossing and avoids a
  wrap that would silently declare a hot block healthy.
- **Signal**: once the count exceeds the threshold the read response carries
  `0x1` in the table-12 health field; below the threshold it carries `0x2`. The
  device does not error out; it reports.
  Deviation from the letter of the standard: OCP carries that field on a CECC or
  UECC *error* response, because a healthy block normally answers successfully.
  This model has no ECC failure stage, so it annotates the successful read with
  the health code instead of inventing an error. A host policy can therefore act
  on health without the model claiming an error it cannot justify.
- **Action**: host policy reads the block's valid pages and programs them into a
  freshly erased block, then retires the old one. This reuses the existing read
  and program path, so refresh pays a full block read, a full block write and
  one PEC on the destination.
- **Reset**: only an erase clears the counter (together with the PEC of that
  physical block).
- **Accounting**: refresh traffic is reported separately from GC traffic —
  `refresh_pages`, `refresh_reads`, `refresh_programs`, `waf_refresh`, plus
  `waf_total = waf_gc + waf_refresh`. Mixing them into one WAF would hide the
  read-driven write amplification that read-heavy inference workloads induce.

## Proposed configuration keys

| Key | Default | Unit | Grade | Meaning |
|---|---|---|---|---|
| `hbf-read-count-threshold` | 0 (disabled) | reads per block | exploratory assumption | Block health threshold from p.59-61 / p.118 |
| `hbf-retention-ns` | 0 (disabled) | ns | exploratory assumption | Retention interval from p.118 |
| `hbf-refresh-enable` | false | bool | implementation policy | Master switch for the host refresh policy |
| `hbf-refresh-reserve-blocks-per-plane` | 0 | blocks | implementation policy | Free blocks kept out of allocation for refresh destinations |

All four are added to `configs/parameter-provenance.json` now, marked
`exploratory_assumption` (or `implementation_policy`), with `notes` saying the
value is a modelling assumption and that the product datasheet must supply the
real numbers. None of them exists in the parser yet: adding a provenance entry
before the code is the same ordering the upstream project uses for planned
physical parameters, and the documentation gate only checks the forward
direction (every published key has provenance).

## Implementation progress

| Piece | State | Where |
|---|---|---|
| D1 per-sense read counting | done | `HbfDevice::record_page_read`, both read kernels in `HbfController` |
| D2 per physical block, any page | done | same counter, keyed by canonical block slot |
| D3 threshold predicate, default disabled | done | `hbf-read-count-threshold`, `block_read_count_exceeds_threshold` |
| D5 erase clears the count | done | `record_erase` |
| D5 health bits `0x1`/`0x2` on the read response | done | `PhysicalCompletion::block_health`, `HbfDevice::read_health` |
| D4 retention trigger | done | `first_program_ns` per block, `hbf-retention-ns`, `block_exceeds_retention` |
| Refresh candidate detection, `hbf-refresh-enable` | done | `note_refresh_candidate`, `refresh_candidate_blocks()` |
| Host rewrite action and refresh accounting | todo | reuse `start_relocation` with a refresh purpose plus `waf_refresh` |
| Die-level exclusion during refresh | todo | needs a per-die gate |

Tests: `hbf_read_disturb` (device arithmetic for both layouts: per-sense
counting, threshold boundary, erase reset, retention ageing from the first
program, disabled signals), `hbf_read_disturb_controller` (one sense per page
across cached re-reads and 64 B fragments, threshold signalling only when
configured, the retention clock starting at a real program completion).

## Acceptance criteria for the implementation

1. **Default-off is bit-identical**: with all four keys at their defaults, every
   existing scenario and regression trace produces the same results.
2. **Counting is per sense**: a workload of 64 B reads that coalesce into one
   page sense increments the counter once; repeated reads of a cached page add
   nothing.
3. **Threshold signalling**: a block crossing the threshold reports `0x1` on its
   next read; blocks below it report `0x2`.
4. **Reset on erase**: erase clears the counter; PEC increments independently.
5. **Refresh accounting**: a triggered refresh moves exactly the block's valid
   pages, charges a block erase on the destination, and increases `waf_refresh`
   without touching `waf_gc`.
6. **Die exclusion**: no read or program to the same die is admitted between the
   start and end of a refresh (p.118).
7. **Lifetime report**: with refresh enabled, the reported lifetime is lower
   than the PEC-only number, and both numbers are printed side by side so a
   single figure can never be misread.

## Open questions for the product datasheet

- Real `Max_Read_Threshold` and whether it is per die or per block.
- Whether the counter counts pages, commands or 64 B DLUs.
- Retention interval at the operating temperature, and its temperature
  dependence.
- Whether refresh can be a plain host read+program pair or needs a dedicated
  command.
