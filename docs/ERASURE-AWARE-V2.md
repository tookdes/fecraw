# fecraw erasure-aware v2

本分支 `feat/erasure-aware-fec-v2` 是 fecraw 的第二代 WAN 控制实验实现。目标不是把 Queqiao 整套代理协议搬进来，而是把它最有价值的 erasure-channel 思路放到 fecraw 现有的 L3 TUN + Reed-Solomon + udp2raw/fakeTCP 数据面上。

## 状态

- Wire protocol: **v2，和 master 不兼容**。客户端、服务端必须都运行本分支/同协议版本。
- FEC codec: 当前仍使用 UDPspeeder 的 block RS。Sliding-window RLNC 尚未接入。
- Reliability: 不增加外层 ARQ。TCP 的残余丢包由 TUN 内层 TCP 自己恢复；UDP 仍保持 UDP 语义。
- `adaptive_fec` 和 `enable_pacing` 可以分别开启，但 v2 telemetry framing 始终启用，避免两端因本地开关不同而出现 framing mismatch。

## 数据路径

```text
TUN IP packet
    |
    +-- small-packet fast path (optional)
    |
    v
UDPspeeder RS encoder
    |
    v
protocol-v2 DATA sequence
    |
    v
erasure-aware aggregate pacer
    |
    v
fecraw cook / udp2raw AEAD / fakeTCP|UDP|ICMP
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

## Loss model

v2 不再用 FEC decoder 的 `input_packet_num - output_packet_num` 推断物理丢包。发送端只根据自己真正发送过的 outer sequence 和对端 ACK bitmap 判定 arrival/loss。

统计量包括：

- overall loss rate；
- `P(loss | previous packet arrived)`；
- mean loss burst；
- burst factor（相对同 loss rate 的 memoryless channel）；
- 8 个 round 的 loss lower envelope，作为 rate-independent erasure floor。

只有在样本量足够且 loss pattern 与 memoryless erasure 一致时，floor 才被标记为 trusted。FEC 只对 trusted floor 加冗余；floor 以上的 excess/bursty loss 交给 congestion controller 处理，避免“拥塞 -> 加 parity -> 更拥塞”的正反馈。

## Adaptive RS

当前 codec 仍是原 UDPspeeder Reed-Solomon，但 ratio planner 已替换。

planner 根据：

- trusted erasure floor；
- WAN RTT；
- 目标 residual-loss probability；

计算 `k` 个 data shard 所需的最小 parity。目标 residual 不设为 0：极低残余错误用更多 parity 去消灭并不经济，TCP retransmission 更适合收尾。

动态参数通过 UDPspeeder 原有的 `g_fec_par.version` 机制发布，只允许新的 RS 参数在下一个 FEC block 边界生效，避免一个 block 中途切换 `x:y`。

## Erasure-aware pacing

旧 `pacing.h` 有两个根本问题：反向业务数据被当成 ACK，RTT 也没有真实 timestamp。v2 只消费 protocol-v2 ACK feedback。

对 trusted floor `p`，控制器把 delivered-rate estimate 换算为 wire-rate：

```text
wire_rate ~= delivered_rate / (1 - p)
```

因此 rate-independent erasure 不会让 BBR-style bandwidth estimate 每轮乘一次 `(1-p)` 而逐渐塌到 0。

只有 excess loss / burst growth 会降低 congestion scale。

Pacer 不在 libev 主线程 `sleep()`。它只预约下一 packet 的 wire send time，然后把已经完成 v2 framing/cook 的 packet 放回 UDPspeeder 现有 `delay_manager`。这样 ACK/input callbacks 始终可以继续运行。

## 配置

```toml
[fec]
fec = "20:10"
adaptive = true

[advanced]
enable_pacing = true
max_bandwidth = 0
```

或 CLI：

```bash
--adaptive-fec --enable-pacing
```

首次实网测试建议给 `max_bandwidth` 一个接近链路预期 wire capacity 的安全上限，以减少 controller 尚未获得 delivery sample 时的 startup overshoot。稳定后再测试 `0`（自动探测）。

## 测试

```bash
make test
make -j$(nproc)
```

`make test` 当前覆盖：

1. deterministic pseudo-random erasure；
2. outer DATA/ACK framing；
3. reorder-tolerant loss decision；
4. erasure loss estimate；
5. residual-probability RS planner；
6. non-blocking pacing slot reservation。

## 下一步：Sliding-window RLNC

RLNC 刻意没有和 v2 telemetry/control-plane 改造一起提交。先用现有 RS 验证 loss model、方向性反馈、pacing 和真实链路收益，可以把 codec 和 controller 的问题分开。

下一阶段计划新增独立 FEC codec mode：

```text
rs       existing block Reed-Solomon
window   GF(256) sliding-window random linear code
auto     policy-selected codec
```

Window codec 应按 Queqiao protocol-1 的 bounded repair window、deterministic coefficients 和 decoder tests 做近似 1:1 C++ port；不能直接照抄其经验 `windowChaining` 常数而换一个 decoder 实现。

## Attribution

Queqiao: <https://github.com/bojieli/queqiao>, MIT License, Copyright (c) 2026 Bojie Li。具体 attribution 见仓库根目录 `THIRD_PARTY_NOTICES.md`。
