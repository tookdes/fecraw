# fecraw erasure-aware v2

本分支 `feat/erasure-aware-fec-v2` 是 fecraw 的第二代 WAN 控制实验实现。目标不是把 Queqiao 整套代理协议搬入 fecraw，而是把已经能在真实长距离高丢包链路上验证的 erasure-channel 思路放到现有 L3 TUN + udp2raw/fakeTCP 数据面中，并保持 Loss Model、FEC codec 与 pacing 尽量解耦。

## 当前状态

- Wire protocol：**v2，与 master 不兼容**。两端必须运行同一 v2 build。
- Loss Model：**已获得真实跨国链路显著正收益验证**。
- Codec：支持 `rs`、`rlnc`、`auto`。
- RS：原 UDPspeeder block Reed-Solomon。
- RLNC：GF(256) sliding-window random linear code，window 最大 256。
- Reliability：不增加外层 ARQ。TCP residual loss 继续由 TUN 内层 TCP 恢复；UDP 保持 UDP 语义。
- Pacing：Stage 2 已解决冷启动死锁；Stage 3 修复真实链路上 0.42 Mbps 的 delivery-rate 低估闭环。
- RLNC rate：Stage 3 已从“复用 RS block `data:repair`”升级到 Queqiao-style WindowRate。

`adaptive_fec`、codec 与 `enable_pacing` 均可独立开关；v2 telemetry framing 始终开启，避免两端因本地 tuning 配置不同而发生 wire framing mismatch。

---

## 实网验证记录

测试链路：北京 Ali-Cloud ↔ GCP 美西，长 RTT，真实物理丢包约 16.6%，Loss Model 稳定后 trusted floor 约 0.175。

### Stage 1：Loss Model + adaptive RS

固定 RS `20:10` 时 TCP 接收 Goodput 为 **11.27 Mbps**。启用基于真实 outer-sequence feedback 的自适应控制后，RS 逐步上调并稳定在约 `20:13`，Goodput 提升至 **15.20 Mbps**，提升约 **34.8%**。

这一结果仍使用原 UDPspeeder RS codec，因此收益可以明确归因于：

```text
真实 wire loss 观测
        ↓
memoryless / burst 判定
        ↓
erasure floor
        ↓
residual-probability redundancy planner
```

即 Queqiao-style Loss Model 与 codec 可以解耦独立发挥作用。

### Stage 2：Sliding-window RLNC + pacing liveness

在相同链路、相同 Loss Model 下：

| 组别 | Codec / 选项 | TCP Goodput | TCP retrans | 结果 |
|---|---|---:|---:|---|
| B | RS, `--adaptive-fec` | **15.20 Mbps** | 192 | 已验证基线，约 `20:13` |
| C | RLNC, window=64, `--adaptive-fec` | **15.30 Mbps**，峰值 >20 Mbps | 427 | codec 接入成功，平滑稳定 |
| D1 | RS + adaptive + pacing | **0.42 Mbps** | 4 | 不再死锁，但严重压速 |
| D2 | RLNC + adaptive + pacing | **0.11 Mbps** | 50 | 不再死锁，但严重压速 |

C 对 B 说明 sliding-window RLNC 在保持同一控制器、同一 `20:13` 冗余预算时可以独立工作，并有轻微 Goodput 增益。它避免了 block RS 的硬边界等待，且 repair 可以持续覆盖此前 source symbol。

但 C 的 TCP retrans 明显高于 RS（427 vs 192）。这说明“持续流平均冗余”不是全部问题。当前 C++ RLNC 尚未移植 Queqiao 在 burst/queue 尾部执行的 `protectBurst()`：一段流量结束时，最后一个窗口没有足够未来 source 来继续产生覆盖它的 repair，因此 tail symbol 更容易落入 TCP 重传。Stage 3 先单独优化持续流 WindowRate；如果下一轮 retrans 仍高，下一步优先做 tail/burst protection，而不是重新提高全程 repair rate。

D1/D2 则验证 Stage 2 的 cold-start fail-open 修复成功：数据流不再变成 0 Mbps 挂死。但吞吐下降到 0.x Mbps，暴露出第二个独立 pacing bug。

---

## Protocol-v2 telemetry

每个实际 wire DATA packet 在 FEC codec 之后获得 outer sequence：

```text
magic(2) | version(1) | type(1) | seq(8) | payload
```

DATA header 共 12 bytes。反馈 ACK frame 为 28 bytes：

```text
magic/version/type | largest-seq | 128-bit receive bitmap
```

ACK 最多按 8 个 DATA packet 聚合，并有 10 ms delayed-ACK timer。ACK control 本身不再递归 ACK，且始终绕过 data pacer。

Loss Model 的观测点位于 codec 之后，所以 RS source/parity、RLNC source/repair、小包副本都会按真正 wire packet 统计。FEC 恢复成功不会把真实物理丢包“抹掉”。

发送端对 gap 采用 reorder tolerance 后才判 loss，避免普通乱序直接污染 physical-loss floor。

---

## Loss Model

v2 不再使用原 UDPspeeder decoder 的：

```text
input_packet_num - output_packet_num
```

来推断物理丢包，因为它会把 parity 本身与网络 loss 混在一起，并且容易把接收方向统计错误应用到反向发送编码器。

当前模型维护：

- overall physical loss；
- `P(loss | previous arrived)`；
- `P(arrival | previous lost)`；
- mean loss burst；
- burst factor；
- memoryless statistical test；
- 512-packet round；
- 8-round lower envelope erasure floor。

只有 loss pattern 在统计上接近 memoryless channel 时，floor 才成为 trusted erasure floor。FEC 只针对 trusted floor 增冗余；floor 以上 excess/burst loss 作为 congestion signal，避免形成：

```text
拥塞 → 丢包 → 增加 repair → 更拥塞
```

的正反馈。

---

## RS redundancy planner

RS 使用 sealed-block binomial residual probability：

```text
P(received_shards < data_shards) <= target_residual
```

RTT 越长，残余丢包引发一次内层 TCP recovery 的代价越高，因此 residual target 更严格：

- 默认：1%
- RTT > 100 ms：0.6%
- RTT > 250 ms：0.3%
- RTT < 40 ms：2%

动态 RS 参数只通过 UDPspeeder 原有 `g_fec_par.version` 发布，并在下一个 block boundary 生效；不会在一个 active block 中途改变 `x:y`。

控制器有 hysteresis：冗余上升较快、下降每次只减一个 parity。因此实网长期状态可以停留在 `20:13`，而仅用当前 `floor=0.175 / RTT=200 ms` 做一次无历史 snapshot 计算时，纯数学 planner 可能给出较低值。两者不矛盾。

---

## Sliding-window RLNC

RLNC source symbol 立即发送，不等待 block 封口。repair symbol 是最近 source window 的随机线性组合：

```text
R = a0*S[first] + a1*S[first+1] + ... + an*S[last]   over GF(256)
```

当前实现：

- GF(256) primitive polynomial：`0x11d`；
- source window 最大 256 symbols；
- decoder width：512；
- coefficient 由 `RID + source-index` 确定性生成，与 Queqiao protocol-1 系数生成逻辑对应；
- wire 上无需发送整行 coefficient matrix；
- decoder 做增量 row reduction；
- equation 只剩一个 unknown 时立即恢复 source；
- 大 TUN packet 可拆成多个 source symbol，并在恢复后按 packet-id / fragment-index 重组。

RLNC frame 在 v2 DATA payload 内自描述，因此发送方向可独立 RS/RLNC 切换，接收端无需额外 codec negotiation。

---

## Stage 3：RLNC WindowRate

Stage 2 为了严格隔离 codec 效果，RLNC 与 RS 共用同一个 block-style `data:repair`。因此实网 C 组在 RS 为 `20:13` 时，RLNC 也按：

```text
13 / 20 = 0.65 repair/source
```

持续产生 repair。

这对 sliding window 是明显过保护，因为一个 repair 恢复相邻 symbol 后可能继续释放更老 equation，编码窗口之间存在 chaining。Stage 3 移植 Queqiao `WindowRate` 的经验模型：

```text
arrival = 1 - erasure_floor
windowChaining = 2.5
effectiveWindow = physicalWindow * windowChaining
```

然后寻找最小发送 symbol 数 `n`，满足：

```text
P(Binomial(n, arrival) < effectiveWindow) <= target_residual
```

最后：

```text
window_rate = (n - effectiveWindow) / effectiveWindow
```

其中 `window_rate` 表示平均每个 source 需要多少 repair。

在当前已测链路参数：

```text
floor  = 0.175
RTT    = 200 ms 级
window = 64
```

Stage 3 CI 计算得到：

```text
RLNC WindowRate = 0.3187 repair/source
```

相比 Stage 2 的 0.65 repair/source，repair/source 减少约一半；总 wire symbol 数则从约 `1.65/source` 降到 `1.3187/source`，约减少 20%。

为了确认 Queqiao 的 `windowChaining=2.5` 在 fecraw 当前 C++ decoder 上不是盲目照搬，新增了持续流回归：

```text
physical erasure = 17.5%
window           = 64
measured packets = 3000
clean tail       = 512
WindowRate       = 0.3187
```

x86 CI 实际结果：

```text
window-rate: floor=0.175 RS=20:11 RLNC=0.3187 residual=0.0000 recovered=516
window-rlnc: source recovery + fragmented packet recovery + WindowRate OK
```

即该确定性 17.5% erasure 长流中，3000 个测量 source 的 residual 为 0，decoder 实际恢复 516 个 source symbol。这个结果至少证明 2.5 chaining 在当前实现的持续流 steady state 上是合理的，不需要为了“兼容 Queqiao”人为保留 0.65 的 block 冗余。

RS 和 RLNC rate 现在是独立状态：RS 继续维护整数 block `k:m`；RLNC 使用浮点 repairs/source，通过现有 repair-credit accumulator 发送，因此不修改 RLNC wire format 或 GF(256) algebra。

---

## Stage 3：pacing delivery-rate sampler

### Stage 2 实网压速的真正根因

`--max-bandwidth 0` 的含义是 **没有用户硬带宽上限**，不是“默认低速 cap”。D1 的 0.42 Mbps 来自旧 delivery-rate sampler。

旧逻辑在 telemetry 初始化时就执行：

```text
rate_epoch = process/connection start time
```

第一批 ACK 要等待一个长 WAN RTT 才回来，首个 bandwidth sample 因而近似变成：

```text
少量首批 ACK bytes / 整个 cold-start + RTT
```

在长 RTT 链路上会得到极低速率，和实测 0.42 Mbps 高度一致。pacer 随后基于这个低值开始排队，发送速率降低又让后续 delivery sample 更低，形成自锁低速闭环。

还有第二个问题：旧 `telemetry_feedback_t` 即使没有生成新 bandwidth observation，也会携带上一次缓存的 `delivery_rate`；pacer 把每个 ACK callback 都当成一个新 BBR sample，因此同一个低速样本可能连续推进 STARTUP plateau counter，使 STARTUP 过早结束。

### 新 sampler

Stage 3 改为 Queqiao 当前 BBR 实现同类的 ACK/send slope 采样。

第一个 ACK 只建立 point，不产生 rate：

```text
(first ACK) -> baseline only
```

后续 observation 同时计算：

```text
ack_rate  = Δdelivered_bytes / Δack_time
send_rate = Δsent_bytes      / Δsend_time
sample    = min(ack_rate, send_rate)
```

`min()` 用来抑制 ACK compression：ACK 突然成团到达可能人为放大 `ack_rate`，但不会超过对应数据实际发送 slope。

feedback 新增 `delivery_sampled` 标记。只有真正产生新 ACK/send slope 时：

- 才进入 bandwidth max filter；
- 才推进 BBR STARTUP growth/plateau；
- 才更新 congestion scale。

缓存的旧 `delivery_rate` 仅用于诊断，不再被重复当作新 sample。

### bootstrap

Pacer 仍坚持 fail-open：

```text
0 genuine samples -> direct send
1 genuine sample  -> direct send
2 genuine samples -> establish pacing epoch
```

要求两个独立 sample 的目的，是避免一个异常点立即把 direct path 关闭。bulk flow 中只多等待一个 ACK sampling interval，不需要多等一个完整 RTT。

feedback 超时后会：

- 清空 pacing schedule；
- 清空 bandwidth sample ring；
- 重置 BBR state；
- 重新进入 direct-send bootstrap。

ACK control 始终不进入 data pacer。

Stage 3 还增加了每秒一次真实 pacing sample 日志：

```text
[client/server] pacing sample delivered=X Mbps wire_bw=Y Mbps rate=Z Mbps state=startup|drain|probe_bw|probe_rtt ready=0|1 floor=P rtt=Rms
```

因此下一轮若仍出现压速，可以直接判断问题是在 sampler、erasure compensation、BBR state 还是 pacing rate，而不再只从最终 TCP Goodput 反推。

---

## `auto` codec policy

当前 `auto` 仍故意保守：

1. 启动走已验证 RS；
2. 等本发送方向建立 trusted floor；
3. `floor >= 2%` 且 smoothed RTT `>= 60 ms` 时切 RLNC；
4. 不满足则走 RS。

RS → RLNC 前 flush 当前 RS block；RLNC → RS 时清掉 sender coding-window history。接收端同时识别两种格式，因此两个方向可以独立选择 codec。

Stage 3 暂不调整 AUTO threshold。先用固定 codec 把 WindowRate 与 pacing sampler 两个变量分别实测，再决定最终 policy。

---

## 当前配置

```toml
[fec]
codec = "auto"       # rs / rlnc / auto
fec = "20:10"        # base ratio
rlnc_window = 64      # 4..256
adaptive = true

[advanced]
enable_pacing = false
max_bandwidth = 0     # 0 = unlimited hard cap
```

CLI：

```bash
--fec-codec rs   --adaptive-fec
--fec-codec rlnc --rlnc-window 64 --adaptive-fec
--fec-codec auto --rlnc-window 64 --adaptive-fec
```

---

## CI / tests

```bash
make test
make -j$(nproc)
```

当前测试覆盖：

1. deterministic physical erasure；
2. outer DATA/ACK framing；
3. reorder-tolerant loss decision；
4. erasure-floor / burst estimation；
5. RS residual-probability planner；
6. pacing 无 sample 时必须 fail-open；
7. cached delivery-rate 不得伪装成新 BBR sample；
8. 一个 genuine sample 后仍保持 fail-open；
9. 两个 genuine samples 后才启用 non-blocking pacing；
10. RLNC source recovery；
11. RLNC 大包 fragmentation/reassembly recovery；
12. 17.5% erasure、window=64、WindowRate=0.3187 的 3000-packet 持续流恢复。

当前 Stage 3 x86_64 CI 已通过 unit tests、完整 `make` 和 artifact upload。ARM64 QEMU job 在本文更新时仍在执行，未宣称通过。

---

## Stage 3 推荐实网矩阵

下一轮不要先测 AUTO，也不要给 pacing 人工指定 `--max-bandwidth`。按变量隔离顺序：

```text
E1. RLNC adaptive, pacing OFF
    --fec 20:10 --fec-codec rlnc --rlnc-window 64 --adaptive-fec

    目标：验证 log 中 WindowRate 稳定到约 0.32，并比较：
          old C = 15.30 Mbps / retrans 427
          new E1 goodput / retrans / wire overhead

E2. RS adaptive, pacing ON, max_bandwidth=0
    --fec 20:10 --fec-codec rs --adaptive-fec --enable-pacing

    目标：单独验证新 delivery sampler；比较：
          old D1 = 0.42 Mbps
          new E2 goodput
          pacing delivered/wire_bw/rate/state 日志

E3. RLNC adaptive + pacing ON
    只在 E2 的 pacing rate 已恢复到合理 Mbps 级后测试。
```

如果 E1 Goodput 保持或上升，但 TCP retrans 仍显著高于 RS，下一阶段优先移植 Queqiao `protectBurst()` / tail protection；不应简单把持续 WindowRate 再提高回 0.65。

如果 E2 仍明显低速，直接根据新增 pacing sample 日志定位，而不是先用 `--max-bandwidth` 人工覆盖 estimator。

---

## Attribution

Queqiao: <https://github.com/bojieli/queqiao>, MIT License, Copyright (c) 2026 Bojie Li。具体 attribution 见根目录 `THIRD_PARTY_NOTICES.md`。
