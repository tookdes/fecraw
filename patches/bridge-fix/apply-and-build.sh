#!/bin/bash
# apply-and-build.sh - 应用 bridge_mode 补丁并重新编译
# 用法: 在 fecraw 项目根目录执行:
#   tar xzf fecraw-bridge-fix.tar.gz
#   cd bridge-fix
#   bash apply-and-build.sh /path/to/fecraw
set -e

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -n "$1" ]; then
    PROJECT_DIR="$(cd "$1" && pwd)"
elif [ -f "../makefile" ] && [ -d "../ref/udp2raw" ]; then
    PROJECT_DIR="$(cd .. && pwd)"
else
    error "用法: bash apply-and-build.sh <fecraw项目目录>"
fi

[ -f "$PROJECT_DIR/makefile" ]           || error "找不到 makefile, 请确认 fecraw 项目目录"
[ -d "$PROJECT_DIR/ref/udp2raw" ]        || error "找不到 ref/udp2raw/, 请确认项目结构"
[ -f "$SCRIPT_DIR/raw_api.cpp" ]         || error "找不到补丁文件 raw_api.cpp"
[ -f "$SCRIPT_DIR/ref/udp2raw/client.cpp" ] || error "找不到补丁文件 ref/udp2raw/client.cpp"
[ -f "$SCRIPT_DIR/ref/udp2raw/server.cpp" ] || error "找不到补丁文件 ref/udp2raw/server.cpp"

info "项目目录: $PROJECT_DIR"

info "备份原文件..."
for f in raw_api.cpp adaptive_fec.h ref/udp2raw/client.cpp ref/udp2raw/server.cpp; do
    if [ -f "$PROJECT_DIR/$f" ]; then
        cp "$PROJECT_DIR/$f" "$PROJECT_DIR/${f}.bak"
        info "  备份 $f -> ${f}.bak"
    fi
done

info "应用补丁文件..."
cp "$SCRIPT_DIR/raw_api.cpp"              "$PROJECT_DIR/raw_api.cpp"
cp "$SCRIPT_DIR/ref/udp2raw/client.cpp"   "$PROJECT_DIR/ref/udp2raw/client.cpp"
cp "$SCRIPT_DIR/ref/udp2raw/server.cpp"   "$PROJECT_DIR/ref/udp2raw/server.cpp"
[ -f "$SCRIPT_DIR/adaptive_fec.h" ] && cp "$SCRIPT_DIR/adaptive_fec.h" "$PROJECT_DIR/adaptive_fec.h"
info "  raw_api.cpp"
info "  adaptive_fec.h (改进自适应算法)"
info "  ref/udp2raw/client.cpp"
info "  ref/udp2raw/server.cpp"

info "清理并重新编译..."
cd "$PROJECT_DIR"
make clean
make -j$(nproc 2>/dev/null || echo 2)

info ""
info "============================================"
info "  bridge_mode 补丁已应用, 编译完成!"
info "============================================"
info ""
info "修复内容:"
info "  - udp2raw client/server 不再覆盖 bridge fd"
info "  - bridge_mode 下直接使用 socketpair 传输数据"
info "  - 修复了握手成功但数据不通的问题"
info "  - 改进自适应 FEC: 防振荡, ±1步进, 3秒冷却, 降低需连续3次低丢包"
info ""
info "请在两端都部署新二进制后重启测试"
