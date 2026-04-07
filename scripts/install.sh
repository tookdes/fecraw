#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
INSTALL_DIR="/usr/local/bin"
CONFIG_DIR="/etc/fecraw"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

if [ "$(id -u)" -ne 0 ]; then
    error "This script must be run as root"
fi

info "Checking build dependencies..."
if ! command -v g++ &>/dev/null; then
    error "g++ not found. Install with: apt install g++ build-essential"
fi

info "Building fecraw..."
cd "$PROJECT_DIR"
make clean
make -j$(nproc)

info "Installing binary and shared library..."
install -m 755 fecraw "$INSTALL_DIR/fecraw"
install -m 755 "$PROJECT_DIR/scripts/fecraw-tui" "$INSTALL_DIR/fecraw-tui"
install -d /usr/local/lib/fecraw
install -m 755 build/libudp2raw_raw.so /usr/local/lib/fecraw/
ldconfig 2>/dev/null || true

info "Setting up configuration..."
mkdir -p "$CONFIG_DIR"

if [ ! -f "$CONFIG_DIR/fecraw.toml" ]; then
    echo ""
    echo "No configuration found. Choose mode:"
    echo "  1) Server"
    echo "  2) Client"
    echo "  3) Skip (configure later)"
    read -p "Choice [1/2/3]: " choice

    case "$choice" in
        1)
            fecraw --gen-config-server "$CONFIG_DIR/fecraw.toml"
            info "Server config generated at $CONFIG_DIR/fecraw.toml"
            warn "Edit the config file before starting the service!"
            ;;
        2)
            fecraw --gen-config-client "$CONFIG_DIR/fecraw.toml"
            info "Client config generated at $CONFIG_DIR/fecraw.toml"
            warn "Edit the config: set your server IP and password!"
            ;;
        3)
            info "Skipping config generation. Create $CONFIG_DIR/fecraw.toml manually."
            ;;
        *)
            warn "Invalid choice, skipping config generation."
            ;;
    esac
else
    info "Config already exists at $CONFIG_DIR/fecraw.toml"
fi

info "Installing systemd service..."
cp "$PROJECT_DIR/systemd/fecraw.service" /etc/systemd/system/
systemctl daemon-reload

info ""
info "============================================"
info "  fecraw installed successfully!"
info "============================================"
info ""
info "Quick start:"
info "  1. Edit config:  nano $CONFIG_DIR/fecraw.toml"
info "  2. Start:        systemctl start fecraw"
info "  3. Enable boot:  systemctl enable fecraw"
info "  4. View logs:    journalctl -u fecraw -f"
info ""
info "Or run directly:   fecraw --config $CONFIG_DIR/fecraw.toml"
