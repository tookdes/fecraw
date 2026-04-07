# fecraw

将 [tinyfecVPN](https://github.com/wangyu-/tinyfecVPN)（TUN VPN + FEC 前向纠错）和 [udp2raw](https://github.com/wangyu-/udp2raw)（UDP 伪装为 TCP/ICMP）合并为单一工具。

## 功能

- **TCP 伪装**：通过 raw socket 将 UDP 流量伪装为 TCP（或 UDP/ICMP），绕过运营商 UDP 限速/QoS
- **FEC 前向纠错**：发送冗余包，在丢包环境下无需等待重传即可恢复数据
- **TUN VPN 隧道**：创建虚拟网卡，应用层无感知
- **AES-128 加密 + HMAC-SHA1 认证**
- **TOML 配置文件** + **命令行参数** 双模式
- **systemd 服务** 一键部署
- **交互式 TUI** 可视化配置（需要 `dialog` 或 `whiptail`）

## 架构

```
应用 → TUN 网卡 → FEC 编码 → socketpair → AES 加密 → fakeTCP 封装 → raw socket → 网络
                                                                                    ↓
网络 → raw socket → fakeTCP 解封 → AES 解密 → socketpair → FEC 解码 → TUN 网卡 → 应用
```

FEC 和 raw socket 运行在两个线程中，通过 `socketpair` 通信。udp2raw 编译为独立的共享库 (`libudp2raw_raw.so`) 实现完全的符号隔离。

## 编译

### 依赖

- Linux（需要 raw socket 和 TUN 支持）
- g++ (C++11)
- make
- binutils

Ubuntu/Debian:
```bash
sudo apt-get install -y g++ make binutils
```

### 编译步骤

```bash
cd fecraw
make -j$(nproc)
```

编译产物：
- `fecraw` — 主程序
- `build/libudp2raw_raw.so` — udp2raw 共享库

### Docker 编译

```bash
# 在包含 Dockerfile.fecraw 的目录
DOCKER_BUILDKIT=1 docker build -f Dockerfile.fecraw --target export --output type=local,dest=./out .
```

## 安装

### 一键安装

```bash
sudo bash scripts/install.sh
```

自动完成编译、安装二进制到 `/usr/local/bin/`、安装 `.so` 到 `/usr/local/lib/fecraw/`、生成配置模板、安装 systemd 服务。

### 手动安装

```bash
sudo install -m 755 fecraw /usr/local/bin/
sudo install -d /usr/local/lib/fecraw
sudo install -m 755 build/libudp2raw_raw.so /usr/local/lib/fecraw/
sudo cp systemd/fecraw.service /etc/systemd/system/
sudo systemctl daemon-reload
```

## 使用

### 快速启动（命令行）

**服务端：**
```bash
sudo ./fecraw -s --listen 0.0.0.0:4096 --key "your_password" --auto-rule
```

**客户端：**
```bash
sudo ./fecraw -c --remote SERVER_IP:4096 --key "your_password" --auto-rule
```

连接后会自动创建 TUN 网卡（默认 `10.22.22.0/24` 子网），服务端 `10.22.22.1`，客户端 `10.22.22.2`。

### 配置文件

**生成配置模板：**
```bash
fecraw --gen-config-server /etc/fecraw/fecraw.toml   # 服务端
fecraw --gen-config-client /etc/fecraw/fecraw.toml   # 客户端
```

**配置文件格式（TOML）：**
```toml
[general]
mode = "server"           # server 或 client
key = "your_password"     # 加密密钥，两端必须一致

[network]
listen = "0.0.0.0:4096"  # 监听地址（服务端必填）
remote = "1.2.3.4:4096"  # 远程地址（客户端必填）
raw_mode = "faketcp"      # faketcp / udp / icmp
cipher = "aes128cbc"      # aes128cbc / aes128cfb / xor / none
auth = "hmac_sha1"        # hmac_sha1 / md5 / crc32 / simple / none

[vpn]
subnet = "10.22.22.0"    # VPN 子网
tun_dev = "tun0"          # TUN 设备名
tun_mtu = 1380            # TUN MTU

[fec]
mode = 0                  # 0=blob模式  1=逐包模式
fec = "20:10"             # 每20个数据包额外发10个冗余包
timeout = 8               # FEC 超时（毫秒）
mtu = 1250                # FEC 内部 MTU

[advanced]
seq_mode = 3              # TCP 序列号模式 (0-4)
auto_iptables = true      # 自动添加 iptables 规则
keep_reconnect = true     # 断线自动重连
log_level = 4             # 0=静默 1=fatal 2=error 3=warn 4=info 5=debug
```

**使用配置文件启动：**
```bash
sudo fecraw --config /etc/fecraw/fecraw.toml
```

### systemd 服务

```bash
sudo systemctl start fecraw      # 启动
sudo systemctl stop fecraw       # 停止
sudo systemctl enable fecraw     # 开机自启
sudo systemctl status fecraw     # 状态
journalctl -u fecraw -f          # 实时日志
```

### 交互式 TUI

```bash
sudo fecraw --tui
```

需要安装 `dialog` 或 `whiptail`（大多数 Linux 发行版默认自带 `whiptail`）。

## 命令行参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-s, --server` | 服务端模式 | — |
| `-c, --client` | 客户端模式 | — |
| `--config <file>` | 加载 TOML 配置文件 | — |
| `--listen <ip:port>` | 监听地址 | `0.0.0.0:4096` |
| `--remote <ip:port>` | 远程服务器地址 | — |
| `--key <string>` | 加密密钥 | — |
| `--raw-mode <mode>` | `faketcp` / `udp` / `icmp` | `faketcp` |
| `--cipher <mode>` | `aes128cbc` / `aes128cfb` / `xor` / `none` | `aes128cbc` |
| `--auth <mode>` | `hmac_sha1` / `md5` / `crc32` / `simple` / `none` | `hmac_sha1` |
| `--subnet <ip>` | VPN 子网 | `10.22.22.0` |
| `--tun-dev <name>` | TUN 设备名 | 自动生成 |
| `--tun-mtu <n>` | TUN MTU | `1380` |
| `--fec <x:y>` | FEC 比例 | `20:10` |
| `--fec-timeout <ms>` | FEC 超时 | `8` |
| `--seq-mode <n>` | TCP 序列号模式 (0-4) | `3` |
| `--auto-rule` | 自动添加 iptables 规则 | 开启 |
| `--keep-reconnect` | 断线自动重连 | 开启 |
| `--disable-fec` | 关闭 FEC | — |
| `--log-level <n>` | 日志等级 (0-5) | `4` |
| `--gen-config-server <file>` | 生成服务端配置模板 | — |
| `--gen-config-client <file>` | 生成客户端配置模板 | — |
| `--tui` | 启动交互式 TUI | — |
| `-h, --help` | 帮助信息 | — |

## 运行环境要求

- 必须以 **root** 运行（或具备 `CAP_NET_RAW` + `CAP_NET_ADMIN` 能力）
- 服务端和客户端的 `--key`、`--raw-mode`、`--cipher`、`--auth` 参数必须一致
- 确保服务端端口的防火墙已放行

## 文件结构

```
fecraw/
├── fecraw              # 主程序
├── build/
│   └── libudp2raw_raw.so  # udp2raw 共享库
├── makefile
├── main.cpp            # 入口、CLI 解析
├── fecraw_config.h     # 配置结构体和 TOML 解析
├── fecraw_client.cpp   # 客户端事件循环
├── fecraw_server.cpp   # 服务端事件循环
├── raw_api.h           # udp2raw C 接口头文件
├── raw_api.cpp         # udp2raw C 接口实现
├── scripts/
│   ├── install.sh      # 一键安装脚本
│   └── fecraw-tui      # TUI 配置脚本
└── systemd/
    └── fecraw.service  # systemd 服务文件
```

## 致谢

- [wangyu-/tinyfecVPN](https://github.com/wangyu-/tinyfecVPN)
- [wangyu-/UDPspeeder](https://github.com/wangyu-/UDPspeeder)
- [wangyu-/udp2raw](https://github.com/wangyu-/udp2raw)
