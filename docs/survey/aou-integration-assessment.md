# AoU（AXI over UCIe）桥引入 HBFSim 的可行性评估

> Status: Proposed
> Last reviewed: 2026-11-05

## 结论

| 方案 | 内容 | 建议 | 预估工作量 |
|---|---|---|---|
| A 参数校准 | 用 AoU 的公开开销/延迟数据校准 HBFSim 接口层常数，加开关与 provenance | **立即做** | 1–2 人日 |
| B 轻量协议层 | 在 `HbfDevice` 通道日历前加一层消息/flit/credit/ID 模型（纯 C++） | **需要时做**（若要回答反压、credit、ID 上限类问题） | 1–2 人周 |
| C Verilator 协同 | 把 `AOU_CORE_TOP` Verilate 成 C++，与 HBFSim 联仿 | 不建议现在做 | 4–6 人周，且依赖仿真器/适配 |
| D SystemC 平台 | 用 `tt-oca-harness-model` 作 host 侧平台，HBFSim 作 TLM 目标 | 延后评估 | ≥3 人周 |

理由：HBF 的页级传输（4 KiB）正好落在 AoU 的高效区间；AoU 能带来的关键可量化影响是「6.25% 固定的 flit/协议开销 + 消息组成 + ~45 cycle 往返 + credit 深度约束」，这些用解析模型（B）就能回答，不需要 RTL 级仿真。HBFSim 现有 75% 效率假设与 AoU 实测同量级，**不存在数量级错误**。

## 1. AoU 是什么

- 仓库：[tenstorrent/tt-oca-harness-aou](https://github.com/tenstorrent/tt-oca-harness-aou)，Apache-2.0，SystemVerilog
- 规模：`RTL/` 43 个 SystemVerilog 文件、约 2.0 万行；UVM 验证环境 45 个 SV 文件
- 成熟度：**pre-release**，官方标注 "evaluation quality"，未完成验证收敛，接口/寄存器可能变动
- 接口：AXI4 主/从 + APB3 配置 + UCIe 3.0 FDI（32/64/128 B，单/双 PHY），仅支持 UCIe 256 B Format 6 flit
- 功能：AXI→AoU 消息打包、消息→granule→flit、credit 管理（`AOU_CRD_CTRL`）、RP（1–4 个虚拟 AXI 口）与 QoS 仲裁、位宽转换、突发拆分、early response、AXI 聚合器、FDI bringup 状态机（`AOU_TOP`）
- 规范：[AoU Protocol Specification v0.7](https://cdn.sanity.io/files/jpb4ed5r/production/728b2f8cfc09023466cf350db53764890b4f2343.pdf)（公开 PDF）

## 2. 关键量化数据

### 2.1 延迟（B2B，1 GHz 目标频率）

| 通道 | AOU_CORE 初始延迟（cycle） | 单向（overview） |
|---|---|---|
| AR | 9 | 7 |
| R | 11 | 8 |
| AW | 12 | 9 |
| W | 12 | 9 |
| B | 10 | 6 |

UCIe 环境下的读请求往返（RTT）约 **45 cycle**（不含远端内存延迟）。1 GHz 下约 45 ns。

### 2.2 开销分解（规范第 7 章）

- 链路层（头 + CRC）：6 B / 256 B = **2.34%**
- 协议头：10 B / 256 B = **3.91%**
- 合计固定开销 **6.25% / flit**
- 消息级应用数据占比：ReadReq/WriteReq 98.3%；WriteResp 85%；WriteData 256b 80.0% / 512b 85.3% / 1024b 85.3%

### 2.3 实测效率（16-beat 突发）

| 方向 | 实测 | 规范上限 |
|---|---|---|
| 读数据（单向） | 85.3% | 85.7% |
| 写数据（单向） | 79.0% | 79.0% |
| 写数据（双向） | 78.5% | 78.7% |

1-beat 突发显著下降：写 66.7%（单向）/ 63.3%（双向），写满 70.3%/67.0%。**小事务是效率杀手**。

### 2.4 资源与实现

- FIFO：TX 2 项；RX 数据 88 项（1024b FDI 时 140 项），AW/AR/B 各 44 项
- 面积：1 RP（AXI 512b）约 97k µm²，2 RP 约 197k µm²（v0.4 数据，64B FDI 配置）
- 频率目标 1 GHz；RX 每 cycle 可处理 4 读请求 / 4 写请求 / 2 读数据 / 2 写数据 / 12 写响应

## 3. HBFSim 现状

| 维度 | HBFSim 现状 |
|---|---|
| AXI 通道（AR/AW/W/R/B） | 无 |
| AXI ID / outstanding 表 | 无（只有 `page_read_queue_depth_per_stack=4096`、`outstanding_write_pages_per_channel=64`） |
| flit / granule / 打包 | 无 |
| credit / CrdtGrant | 无 |
| RP / QoS / 重映射 | 无 |
| 接口开销 | 唯一折损：OCP p.16 的 **75% AXI 效率**，折入 48/96/192 GB/s |
| 命令开销 | 每命令 **64 B**（`command_address_bytes`） |
| 数据通路 | 每通道 command / rx / tx 三条 `ResourceTimeline`，按 payload 字节计费 |
| 队列 | 读 4096 页/stack、写 64 页/通道 |

即：HBFSim 是「字节率 + 命令开销」的抽象，不是协议模型。

## 4. 差距对结论的影响

### 4.1 带宽：量级一致，无需推翻现有结论

- HBF 一页 4 KiB；按 AoU 的 1024b DLENGTH 是 32 条数据消息，512b 是 64 条。
- 消息级 85.3% × flit 级 93.75% ≈ **80%** 有效载荷率（1024b/512b 数据消息）；256b 时 ≈ 75%。
- HBFSim 采用 75%（OCP 规范值），**处于保守一侧**：即模型没有高估带宽；若按 AoU 口径做敏感性分析，读路径最多还有 +5% 空间，写路径因消息小、写响应开销反而更低（79%）。
- 结论：现有带宽结论稳健；可在 A 方案中把 75% 拆成「链路 93.75% × 消息组成」并加开关，供敏感性分析。

### 4.2 延迟：RTT 45 ns 相对 4 µs tR 仅约 1%

- 对页级延迟结论无实质影响；但它是「维持满带宽所需 in-flight 页数」的一部分。
- 通道带宽-延迟积：96 GB/s × 4 µs ≈ **384 KB/通道 ≈ 94 页**。HBFSim 每通道读队列 4096/16 = 256 页，够用。
- AoU 侧：请求 FIFO 44 项、数据 FIFO 88 项——**是否足以承载 94 页的 BDP，取决于 AXI 突发长度（一条 AR 能覆盖多少字节）**。这是引入协议层模型最值得回答的问题；目前仅凭 FIFO 深度无法定论，需要用 B 方案算。

### 4.3 现有模型回答不了、AoU 能回答的问题

1. credit 深度/归还 RTT 是否把 HBF 通道带宽压到接口以下；
2. AXI ID 与 outstanding 上限对乱序与延迟分布的影响；
3. RP（1/2/4 个虚拟 AXI 接口）与 QoS 仲裁对多租户/多流场景的影响；
4. flit 级开销对小事务（<1 KiB）的影响（1-beat 写只有 66.7%）；
5. ARUSER/AWUSER 语义（batch read hint、scratchpad 命令类型）的协议承载。

## 5. 方案对比

| | A 参数校准 | B 轻量协议层 | C Verilator 联仿 | D SystemC 平台 |
|---|---|---|---|---|
| 产出 | 文档 + 常数/开关 | 可运行的协议层模型 | 周期级时序 | 平台级集成 |
| 成本 | 1–2 人日 | 1–2 人周 | 4–6 人周 + 仿真器 | ≥3 人周 |
| 风险 | 低 | 中（模型假设需自证） | 高（RTL pre-release、UVM 依赖商业仿真器） | 中高 |
| 能回答 | 带宽敏感性 | credit/ID/小事务/反压 | 逐周期微架构 | 固件/驱动联调 |
| 建议 | **做** | 视需求 | 不做 | 延后 |

## 6. 建议的任务拆解（A + B）

**A-1**（0.5 人日）：把 AoU 的 6.25% flit 开销、消息组成与 44/88/140 FIFO 深度写入 provenance，标注证据等级（厂商开源 MAS，非第三方实测）。

**A-2**（0.5–1 人日）：新增两个可选配置键：
- `hbf-aou-link-overhead`（默认 0，取值 0.0625 表示计入 AoU flit 开销）
- `hbf-aou-data-message-bytes`（默认 0=不建模；64/128 表示 DLENGTH）

默认关闭时结果与现在完全一致（保持默认等价性），开启时按 `有效载荷率 = 92.75% × 消息占比` 缩放接口带宽。

**B-1**（0.5 人周）：在 `HbfDevice` 通道模型前插入 `AouChannel` 类：请求→消息→granule→flit 排队，维护 credit 表（RX FIFO 深度）与请求 outstanding 表；时间用 `ResourceTimeline`（与现有日历一致）。

**B-2**（0.5 人周）：测试与校准：
- 单通道 4 KiB 顺序读，验证达成率 ≈ 80%（对 75% 基线 +5%）；
- 1-beat 写流，验证降到 ~67%；
- credit 减半后带宽随之下降，证明 credit 是有效约束；
- 默认关闭时与现有结果逐位一致。

**验收标准**：开启 AoU 后，页级带宽变化在 ±10% 内、延迟变化 < 5%，且能复现「小事务效率下降」这一已知现象。

## 7. 风险与未决问题

1. **RTL 处于 pre-release**：接口/寄存器可能变，不适合作为长期二进制依赖；只作为参考实现与数值来源。
2. **UVM 验证环境依赖商业仿真器**（VCS/Xcelium 等），本机没有；C 方案的成本主要在这里。
3. **75%（OCP）与 AoU 的 6.25%+消息组成是否重叠**：OCP 的 75% 是「AXI link layer efficiency」，AoU 的分解是「flit 组成」。两者口径不同，不能直接相乘后宣称精确值；A 方案只做敏感性与上下界。
4. **HBF 的 AXI 用法未在 AoU 文档中覆盖**：HBF 的 64 B 粒度命令、4 KiB 页写、batch read hint、scratchpad 命令类型都是 OCP 侧约定；AoU 规范未必定义其映射，需要自行约定。
5. **数据消息宽度选择**：HBF 应优先 1024b DLENGTH（32 条消息/页）以拿到 85.3% 而非 80.0%。
