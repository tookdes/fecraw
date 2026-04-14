#!/bin/bash
# package.sh - 将 fecraw 编译产物或源码打包为可分发的 tar.gz
# Usage:
#   bash scripts/package.sh           # 打包编译产物 (需要先 make)
#   bash scripts/package.sh --source  # 打包源码 (在目标机器上编译)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# 读取版本（从 git tag，没有则用日期）
VERSION="$(git -C "$PROJECT_DIR" describe --tags --always 2>/dev/null || date +%Y%m%d)"
ARCH="$(uname -m)"

if [ "$1" = "--source" ]; then
    PKG_NAME="fecraw-src-${VERSION}"
    PKG_DIR="/tmp/${PKG_NAME}"
    OUT_FILE="${PROJECT_DIR}/${PKG_NAME}.tar.gz"

    RED='\033[0;31m'; GREEN='\033[0;32m'; NC='\033[0m'
    info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
    error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

    info "打包源码: $PKG_NAME"
    rm -rf "$PKG_DIR"
    mkdir -p "$PKG_DIR"

    # fecraw 自有源码
    for f in makefile *.cpp *.h; do
        [ -f "$PROJECT_DIR/$f" ] && cp "$PROJECT_DIR/$f" "$PKG_DIR/"
    done
    cp -r "$PROJECT_DIR/scripts"  "$PKG_DIR/scripts"
    cp -r "$PROJECT_DIR/systemd"  "$PKG_DIR/systemd"

    # ref 依赖项目
    cp -r "$PROJECT_DIR/ref" "$PKG_DIR/ref"

    # 写构建说明
    cat > "$PKG_DIR/BUILD.txt" << 'BEOF'
fecraw 源码包构建说明
=======================
依赖: g++, make, libssl-dev (OpenSSL)

安装依赖 (Debian/Ubuntu):
  sudo apt install -y build-essential libssl-dev

编译:
  make

安装:
  sudo make install
  或
  sudo bash scripts/install.sh

打包二进制:
  bash scripts/package.sh
BEOF

    cd /tmp
    tar czf "$OUT_FILE" "$PKG_NAME"
    rm -rf "$PKG_DIR"

    info "源码包: $OUT_FILE"
    info "大小: $(du -sh "$OUT_FILE" | cut -f1)"
    exit 0
fi

PKG_NAME="fecraw-${VERSION}-linux-${ARCH}"
PKG_DIR="/tmp/${PKG_NAME}"
OUT_FILE="${PROJECT_DIR}/${PKG_NAME}.tar.gz"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# 检查产物是否已编译
[ -f "$PROJECT_DIR/fecraw" ]                        || error "找不到 fecraw 二进制，请先执行 make"
[ -f "$PROJECT_DIR/build/libudp2raw_raw.so" ]       || error "找不到 build/libudp2raw_raw.so，请先执行 make"

info "打包版本: $PKG_NAME"

# 清理并准备暂存目录
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR"

# ── 复制产物 ──────────────────────────────────────────────
cp "$PROJECT_DIR/fecraw"                    "$PKG_DIR/fecraw"
cp "$PROJECT_DIR/build/libudp2raw_raw.so"  "$PKG_DIR/libudp2raw_raw.so"
cp "$PROJECT_DIR/scripts/fecraw-tui"       "$PKG_DIR/fecraw-tui"
cp "$PROJECT_DIR/systemd/fecraw.service"   "$PKG_DIR/fecraw.service"

chmod 755 "$PKG_DIR/fecraw"
chmod 755 "$PKG_DIR/libudp2raw_raw.so"
chmod 755 "$PKG_DIR/fecraw-tui"

# ── 生成纯安装脚本（不需要重新编译）────────────────────────
cat > "$PKG_DIR/install.sh" << 'EOF'
#!/bin/bash
# fecraw 预编译包安装脚本
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INSTALL_BIN="/usr/local/bin"
INSTALL_LIB="/usr/local/lib/fecraw"
CONFIG_DIR="/etc/fecraw"
SERVICE_FILE="/etc/systemd/system/fecraw.service"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

[ "$(id -u)" -eq 0 ] || error "请以 root 运行：sudo bash install.sh"

info "安装 fecraw 二进制..."
install -m 755 "$SCRIPT_DIR/fecraw"       "$INSTALL_BIN/fecraw"
install -m 755 "$SCRIPT_DIR/fecraw-tui"  "$INSTALL_BIN/fecraw-tui"

info "安装共享库..."
install -d "$INSTALL_LIB"
install -m 755 "$SCRIPT_DIR/libudp2raw_raw.so" "$INSTALL_LIB/libudp2raw_raw.so"
ldconfig 2>/dev/null || true

info "安装 systemd 服务..."
cp "$SCRIPT_DIR/fecraw.service" "$SERVICE_FILE"
systemctl daemon-reload

info "生成配置文件..."
mkdir -p "$CONFIG_DIR"
if [ ! -f "$CONFIG_DIR/fecraw.toml" ]; then
    echo ""
    echo "未找到配置文件，请选择运行模式："
    echo "  1) 服务端"
    echo "  2) 客户端"
    echo "  3) 跳过（稍后手动配置）"
    read -p "选择 [1/2/3]: " choice
    case "$choice" in
        1) fecraw --gen-config-server "$CONFIG_DIR/fecraw.toml"
           info "服务端配置已生成：$CONFIG_DIR/fecraw.toml"
           warn "启动前请编辑配置文件，至少设置 key 字段！" ;;
        2) fecraw --gen-config-client "$CONFIG_DIR/fecraw.toml"
           info "客户端配置已生成：$CONFIG_DIR/fecraw.toml"
           warn "请编辑配置文件，填写服务器 IP 和 key！" ;;
        *) info "跳过，请手动创建 $CONFIG_DIR/fecraw.toml" ;;
    esac
else
    info "配置文件已存在：$CONFIG_DIR/fecraw.toml"
fi

info ""
info "============================================"
info "  fecraw 安装完成！"
info "============================================"
info ""
info "使用方法："
info "  1. 编辑配置:  nano $CONFIG_DIR/fecraw.toml"
info "  2. 启动服务:  systemctl start fecraw"
info "  3. 开机自启:  systemctl enable fecraw"
info "  4. 查看日志:  journalctl -u fecraw -f"
info ""
info "或直接运行:    fecraw --config $CONFIG_DIR/fecraw.toml"
EOF
chmod 755 "$PKG_DIR/install.sh"

# ── 写 README ───────────────────────────────────────────────
cat > "$PKG_DIR/README.txt" << EOF
fecraw ${VERSION} - linux/${ARCH}
===================================

安装（需要 root）：
  tar xzf ${PKG_NAME}.tar.gz
  cd ${PKG_NAME}
  sudo bash install.sh

卸载：
  sudo systemctl stop fecraw || true
  sudo systemctl disable fecraw || true
  sudo rm -f /usr/local/bin/fecraw /usr/local/bin/fecraw-tui
  sudo rm -rf /usr/local/lib/fecraw
  sudo rm -f /etc/systemd/system/fecraw.service
  sudo systemctl daemon-reload

文件说明：
  fecraw               主程序
  libudp2raw_raw.so    udp2raw 共享库（运行时加载）
  fecraw-tui           交互式 TUI 配置工具
  fecraw.service       systemd 服务文件
  install.sh           安装脚本
EOF

# ── 打包 ────────────────────────────────────────────────────
cd /tmp
tar czf "$OUT_FILE" "$PKG_NAME"
rm -rf "$PKG_DIR"

info "打包完成：$OUT_FILE"
info "大小：$(du -sh "$OUT_FILE" | cut -f1)"
