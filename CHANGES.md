# fecraw 改动记录

## 项目路径重组

- 依赖项目（udp2raw、UDPspeeder、tinyfecVPN）从同级目录 `../xxx` 迁移到 `ref/xxx` 子目录
- `ref/` 已加入 `.gitignore`，不纳入版本控制
- Makefile 中 `UDPRAW_DIR`、`TINYFEC_DIR`、`UDPSPEEDER_DIR` 路径已更新
- 源码中 `#include "../tinyfecVPN/tun_dev.h"` 改为 `#include "tun_dev.h"`（通过 `-I` 解析）

## 架构改造：socketpair bridge 模式

fecraw 将 tinyfecVPN（TUN + FEC）和 udp2raw（raw socket 伪装）合并为单进程双线程架构：

```
Thread 1 (main):  App → TUN → FEC encode → socketpair[0]
Thread 2 (raw):   socketpair[1] → AES encrypt → fakeTCP → raw socket → network
```

### bridge_mode 机制

原始 udp2raw 的 `client_event_loop` / `server_event_loop` 会自行创建 UDP socket，
会覆盖 fecraw 通过 `raw_api_client_loop(bridge_fd)` 传入的 socketpair fd。

**修复**：引入全局 `bridge_mode` 标志，在 bridge 模式下：

- **client.cpp**：跳过 UDP socket 创建，用 `recv()`/`send()` 替代 `recvfrom()`/`sendto()`
- **server.cpp**：将 bridge fd 注册到 epoll，映射 client conv_id 到 bridge fd，
  通过 bridge fd 直接收发数据
- **raw_api.cpp**：在调用 `client_event_loop`/`server_event_loop` 前设置 `bridge_mode = 1`

涉及文件：
- `raw_api.cpp` → `bridge_mode = 1`
- `ref/udp2raw/client.cpp` → 条件跳过 UDP socket 创建 + bridge 收发
- `ref/udp2raw/server.cpp` → bridge fd epoll 注册 + conv 映射 + 数据转发

## 新特性 1：自适应 FEC（参考 NRUP `fec_adaptive.go`）

- 新文件 `adaptive_fec.h`
- 维护滑动窗口丢包统计（sent / lost 计数器，每次评估后清零）
- 根据丢包率分段调整冗余比例：`<2%` → 最小冗余，`>20%` → 最大冗余
- 高 RTT 时额外增加冗余（RTT > 100ms × 1.3，RTT > 300ms × 1.6）
- 配置：`[fec] adaptive = true`，CLI `--adaptive-fec`
- 默认关闭（`adaptive = false`）

### 防振荡机制

- 每次最多调整 ±1 个 parity shard（不再跳变）
- 降低保护需要连续 3 次低丢包评估确认
- 最小调整间隔 3 秒
- 最低保护 parity = 2（不会降到 1）

## 新特性 2：小包特殊处理（参考 NRUP 小包冗余逻辑）

- 新文件 `small_packet.h`
- 小于阈值的包（默认 256 字节，可配置）跳过 FEC 编码等待
- 直接发送 `[0xFE][4B seq][payload]` 格式，根据平滑丢包率发 2-5 份冗余
- 接收端通过序号 map 去重（保留最近 1000 条）
- 配置：`[fec] small_packet_threshold = 256`，`small_packet_redundancy = 2`

## 新特性 3：BBR 精简版流量整形（参考 NRUP `congestion.go`）

- 新文件 `pacing.h`
- 四状态 BBR 状态机：startup → drain → probe_bw → probe_rtt
- 带宽采样：滑动窗口 10 样本取 max → `maxBW`
- RTT 采样：10 秒窗口取 min → `minRTT`
- BDP = maxBW × minRTT，cwnd = BDP × gain
- 发送前 pacing：sleep(packet_size / pacingRate)，最大阻塞 500ms
- 配置：`[advanced] enable_pacing = true`，`max_bandwidth = 0`

## 新特性 4：现代加密套件（修改 udp2raw + OpenSSL）

- 修改 `ref/udp2raw/encrypt.h`：新增 `cipher_aes256gcm`、`cipher_chacha20poly1305` 枚举
- 修改 `ref/udp2raw/encrypt.cpp`：使用 OpenSSL EVP API 实现 AES-256-GCM 和 ChaCha20-Poly1305
- `cipher_is_aead()` 内联函数判断是否 AEAD 模式
- AEAD 模式自动将 auth_mode 设为 none（AEAD 自带认证）
- 自动选择：`cipher = "auto"` 根据 CPU 是否支持 AES-NI 选择最优算法
- Wire 格式：`[12B nonce][ciphertext][16B tag]`
- 密钥派生复用 udp2raw 已有的 PBKDF2-SHA256 + HKDF 流程
- 编译依赖：`libssl-dev`（OpenSSL）

## 配置参考

```toml
[general]
mode = "client"               # server / client
key = "your_password"

[network]
listen = "0.0.0.0:4096"
remote = "1.2.3.4:4096"
raw_mode = "faketcp"           # faketcp / udp / icmp
cipher = "aes256gcm"           # aes256gcm / chacha20poly1305 / aes128cbc / xor / none / auto
auth = "none"                  # auto-none for AEAD ciphers

[vpn]
subnet = "10.22.22.0"
tun_dev = "tun0"
tun_mtu = 1380

[fec]
mode = 0
fec = "20:10"
timeout = 8
mtu = 1250
adaptive = false               # 自适应 FEC（实验性，默认关闭）
small_packet_threshold = 0     # 0=禁用, 256=推荐用于 SSH/游戏
small_packet_redundancy = 2

[advanced]
seq_mode = 3
auto_iptables = true
keep_reconnect = true
log_level = 4
enable_pacing = false           # BBR 流量整形（实验性）
max_bandwidth = 0               # bytes/s 硬限制, 0=不限
```

## 源码打包

- `scripts/package.sh --source`：打包完整源码（含 `ref/`）为 tar.gz
- `scripts/package.sh`：打包编译产物为可分发 tar.gz
- 在目标 Linux 服务器解压后 `make` 即可编译

## 涉及文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `makefile` | 修改 | 路径更新 + OpenSSL 链接 + .so 构建 |
| `.gitignore` | 新建 | 排除 ref/, build/, *.tar.gz 等 |
| `adaptive_fec.h` | 新建 | 自适应 FEC 控制器（含防振荡） |
| `small_packet.h` | 新建 | 小包冗余发送 + 去重 |
| `pacing.h` | 新建 | BBR-lite 拥塞控制 / pacer |
| `fecraw_config.h` | 修改 | 新增配置字段 + TOML 解析 |
| `fecraw_client.cpp` | 修改 | 特性集成 + bridge 事件循环 |
| `fecraw_server.cpp` | 修改 | 特性集成 + bridge 事件循环 |
| `main.cpp` | 修改 | CLI 选项 + CPUID 检测 + FEC 初始化 |
| `raw_api.h` | 修改 | cipher_mode 注释更新 |
| `raw_api.cpp` | 修改 | cipher_map 扩展 + bridge_mode 设置 |
| `ref/udp2raw/encrypt.h` | 修改 | 新增 AEAD 枚举 + cipher_is_aead() |
| `ref/udp2raw/encrypt.cpp` | 修改 | AES-GCM + ChaCha20 实现 |
| `ref/udp2raw/client.cpp` | 修改 | bridge_mode 条件分支 |
| `ref/udp2raw/server.cpp` | 修改 | bridge_mode 条件分支 + epoll 注册 |
| `ref/udp2raw/common.h` | 修改 | UDP2RAW_LINUX 宏守卫 |
| `scripts/package.sh` | 新建 | 源码/二进制打包脚本 |
| `scripts/test_features.sh` | 新建 | 功能验证脚本 |

## 已知问题 & 调优建议

- **GRO 干扰**：如果服务端日志出现 `huge packet, data_len > 1800 dropped`，
  需关闭 GRO：`ethtool -K eth0 gro off`
- **FEC 比例**：高丢包环境（>10%）建议 `20:10` 或更高；低丢包环境可用 `20:4`
- **自适应 FEC**：当前为实验性功能，高流量场景建议保持关闭
- **GLIBC 兼容**：在目标运行服务器上编译，避免 GLIBC 版本不匹配
