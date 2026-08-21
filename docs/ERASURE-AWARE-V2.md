# fecraw erasure-aware v2

本分支 `feat/erasure-aware-fec-v2` 是 fecraw 的第二代 WAN 控制实验实现。目标不是把 Queqiao 整套代理协议搬进来，而是把其中已经能在真实长距离高丢包链路上验证的 erasure-channel 思路放到 fecraw 现有的 L3 TUN + udp2raw/fakeTCP 数据面上，并把 Loss Model、FEC codec 和 pacing 尽量保持解耦。

## 当前状态

- Wire protocol: **v2，和 master 不兼容**。客户端、服务端必须都运行本分支/同协议版本。
- Loss Model: **已获得真实跨国链路正收益验证**。
- FEC codec: 支持 `rs`、`rlnc`、`auto`。RS 是原 UDPspeeder block Reed-Solomon；RLNC 是 GF(256) sliding-window random linear code。
- Reliability: 不增加外层 ARQ。TCP 的 residual loss 由 TUN 内层 TCP 自己恢复；UDP 仍保持 UDP 语义。
- `adaptive_fec`、codec 和 `enable_pacing` 可分别控制；v2 telemetry framing 始终启用，避免两端因本地控制开关不同而出现 framing mismatch。
- Pacing: 第一版在真实链路发现冷启动挂死；当前代码已改为 **feedback-free fail-open**，需要重新做实网吞吐验证。

## 已确认的真实链路结果

2026-08-21，北京 ↔ GCP 美西跨国链路测试：

- 实测物理丢包约 **16.6%**；
- 原固定 RS：`20:10`；
- Queqiao-style Loss Model 根据真实 outer-sequence feedback 将发送方向调整到 `20:13`；
- TCP 接收吞吐量由 **11.27 Mbps** 提升至 **15.20 Mbps**；
- 提升约 **34.8%**。

这一结果是在继续使用原 UDPspeeder RS codec 的情况下得到的，因此可以把收益明确归因到“真实物理丢包测量 + erasure-floor 判定 + redundancy planner”这一控制模块，而不是新 codec。也就是说，Loss Model 已经证明可以和 codec 解耦独立发挥效果。

这也是第二阶段引入 RLNC 时的对照基线：RLNC 必须在相同 Loss Model、相近 wire redundancy 下和 RS 比较，不能把 controller 收益重复记到 codec 上。

## 数据路径

```text
TUN IP packet
    |
    +-- small-packet fast path (optional)
    |
    +-- codec = rs ------------------------+
    |       UDPspeeder block RS            |
    |                                      |
    +-- codec = rlnc ----------------------+--> protocol-v2 DATA sequence
    |       sliding-window GF(256) RLNC     |            |
    |                                      |            v
    +-- codec = auto -----------------------+     erasure-aware pacer
            per-direction policy                         |
                                                        v
                                      fecraw cook / udp2raw AEAD
                                                        |
                                                fakeTCP|UDP|ICMP
                                                        |
                                                        v
                                                       WAN
```

反馈方向：

```text
WAN arrival
    |
protocol-v2 sequence observation
    |
128-bit ACK bitmap (control; not recursively ACKed)
    |
    +--> RTT / delivered-rate sample
    +--> loss outcome stream
             |
             +--> memoryless test
             +--> burst factor
             +--> erasure floor (lower envelope)
             +--> excess loss = congestion signal
```

ACK 最多按 8 个 DATA 包聚合，并有 10 ms delayed-ACK timer，因此稀疏 SSH/API 流量不会等到连接 keepalive timer 才产生 RTT 样本。

## Loss Model

v2 不再用 FEC decoder 的 `input_packet_num - output_packet_num` 推断物理丢包。发送端只根据自己真正发送过的 outer sequence 和对端 ACK bitmap 判定 arrival/loss。这个观测点在 FEC codec 之后，因此 source、parity/repair、小包副本等都会按真实 wire packet 被统计，而 FEC 恢复成功不会反过来“抹掉”物理丢包。

统计量包括：

- overall loss rate；
- `P(loss | previous packet arrived)`；
- mean loss burst；
- burst factor（相对同 loss rate 的 memoryless channel）；
- 8 个 round 的 loss lower envelope，作为 rate-independent erasure floor。

只有在样本量足够且 loss pattern 与 memoryless erasure 一致时，floor 才被标记为 trusted。FEC 只对 trusted floor 加冗余；floor 以上的 excess/bursty loss 交给 congestion controller 处理，避免“拥塞 -> 加 repair -> 更拥塞”的正反馈。

## Adaptive redundancy planner

planner 仍以 `data:repair` 作为统一控制量，但不再绑定某一个 codec。它根据：

- trusted erasure floor；
- WAN RTT；
- 目标 residual-loss probability；

计算所需 repair 数。目标 residual 不设为 0：极低残余错误用更多 repair 去消灭并不经济，TCP retransmission 更适合收尾。

对于 RS，动态参数通过 UDPspeeder 原有的 `g_fec_par.version` 机制发布，只允许新的参数在下一个 FEC block 边界生效，避免一个 block 中途切换 `x:y`。

对于 RLNC，同一个 planner 的 `data:repair` 结果直接转换成 repair-credit rate。这样第一轮 RS/RLNC A/B 比较使用同一 Loss Model 和同一 redundancy budget，先单独回答“滑动窗口 codec 是否比 block RS 更适合这条链路”。后续才考虑引入 Queqiao `WindowRate`/window-chaining 一类专门针对 RLNC 的经验修正。

## Sliding-window RLNC

RLNC 源符号立即发送，不等待 block 封口。repair symbol 是最近 source window 的随机线性组合：

```text
R = a0*S[first] + a1*S[first+1] + ... + an*S[last]   over GF(256)
```

当前实现：

- GF(256) primitive polynomial：`0x11d`；
- repair window 最大 256 个 source symbol；
- decoder 固定 512-symbol width；
- coefficient 由 `RID + index` 确定性生成，与 Queqiao protocol-1 的生成规则一致，wire 上不传 coefficient row；
- decoder 维护增量 reduced linear system，收到 source 时做 substitution，收到 repair 时做 row reduction；
- equation 降到一个 unknown 时立即恢复 source symbol；
- TUN 大包会先拆成 source symbols，恢复后按 packet-id / fragment-index 重组。

相比 block RS，这个结构没有“等一个 block 凑齐/封口”的硬边界，后发送的 repair 仍可以覆盖前面已经丢掉的 symbol，也更容易把 burst loss 分散到连续 repair window 中。

RLNC frame 在 v2 DATA payload 内自描述，因此 `auto` 可以按发送方向独立切换，不需要额外 codec negotiation；接收端根据 frame magic 判断进入 RLNC decoder，否则继续走 RS decoder。

## `auto` codec policy

`auto` 当前故意采用保守策略：

1. 启动时使用已经通过实网验证的 RS；
2. 等本发送方向 Loss Model 建立 trusted floor；
3. 当 `floor >= 2%` 且 smoothed RTT `>= 60 ms` 时切到 RLNC；
4. 不满足条件时使用 RS。

RS → RLNC 时先 flush 当前 RS block；RLNC → RS 时丢弃 sender 的历史 coding window。接收端同时接受两种自描述数据格式，因此两个方向可以处于不同 codec。

这个策略目前只是为了安全 A/B，不代表最终最优 policy。后续应根据真实测试决定是否需要 hysteresis、交互流/大流分类以及“传输越大越偏向 RS/ARQ”的策略。

## Erasure-aware pacing

第一版 pacing 在真实链路测试中出现 **吞吐降为 0 / 数据流挂起**。问题不是 Loss Model，而是 cold start：发送数据进入 Libev delay queue，但 pacing 又依赖尚未建立的 ACK / RTT / delivery feedback，timer queue 因而成为反馈启动链的一部分。

当前实现改成明确的 fail-open 状态机：

- 尚无真实 RTT + delivery-rate sample：`pacing_rate=0`，所有数据直接发送，**不进入 delay queue**；
- 第一份有效 feedback 到达后才建立 pacing epoch；
- feedback 长时间中断时，自动清空 pacing schedule / inflight accounting，重新退回 direct-send；
- 如果 userspace timer schedule 已积压到明显超过 RTT 尺度，丢弃旧 schedule，从 `now` 重新开始；
- ACK control traffic 始终绕过 data pacer。

获得 trusted floor `p` 后，控制器仍使用：

```text
wire_rate ~= delivered_rate / (1 - p)
```

因此 rate-independent erasure 不会让 BBR-style bandwidth estimate 每轮乘一次 `(1-p)` 而逐渐塌到 0。只有 excess loss / burst growth 会降低 congestion scale。

这个修复首先解决的是 **liveness**，不是宣称 pacing 已经有效提升吞吐。必须在原北京 ↔ 美西链路重新开启 `--enable-pacing` 验证：至少先确认不再挂死，再比较 throughput / RTT / queueing。

## 配置

TOML：

```toml
[fec]
codec = "auto"       # rs / rlnc / auto
fec = "20:10"        # 两种 codec 共用的 base data:repair ratio
rlnc_window = 64      # 4..256
adaptive = true

[advanced]
enable_pacing = false
max_bandwidth = 0
```

CLI：

```bash
--fec-codec rs   --adaptive-fec
--fec-codec rlnc --rlnc-window 64 --adaptive-fec
--fec-codec auto --rlnc-window 64 --adaptive-fec
```

需要重新验证 pacing 时再加：

```bash
--enable-pacing
```

建议先分别测 `rs` 和 `rlnc`，保持其余参数完全相同；`auto` 放到两种固定 codec 的结果确认以后再测，否则切换策略本身会成为额外变量。

## 测试

```bash
make test
make -j$(nproc)
```

`make test` 覆盖：

1. deterministic pseudo-random physical erasure；
2. outer DATA/ACK framing；
3. reorder-tolerant loss decision；
4. erasure loss estimate；
5. residual-probability redundancy planner；
6. pacing cold-start 必须 fail-open；
7. feedback 建立后 non-blocking pacing slot reservation；
8. RLNC source symbol 丢失后的 repair recovery；
9. RLNC 大 TUN packet 分片、单 symbol 丢失、恢复与完整重组。

## 推荐下一轮实网矩阵

保持同一北京 ↔ GCP 美西链路、同一 TCP 测试方法：

```text
A. RS   20:13 fixed, pacing off
B. RS   adaptive, pacing off          <- 已有 15.20 Mbps 基线
C. RLNC adaptive, pacing off          <- 本阶段最关键 codec A/B
D. AUTO adaptive, pacing off
E. RS   adaptive, pacing on           <- 验证 cold-start 修复
F. RLNC adaptive, pacing on           <- E 正常后再测
```

优先看 C 对 B：如果 RLNC 在相同 Loss Model 下没有收益，就不要先复杂化 auto/pacing；如果 C 能降低 tail/retransmission penalty 或继续提高 TCP goodput，再针对 RLNC 做 window/rate 专门调优。

## Attribution

Queqiao: <https://github.com/bojieli/queqiao>, MIT License, Copyright (c) 2026 Bojie Li。具体 attribution 见仓库根目录 `THIRD_PARTY_NOTICES.md`。
