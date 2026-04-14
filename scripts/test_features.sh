#!/bin/bash
# test_features.sh - 验证 fecraw 新特性的基本功能
# 在编译完成后运行: bash scripts/test_features.sh
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAIL=$((FAIL+1)); }
info() { echo -e "${YELLOW}[TEST]${NC} $*"; }

FECRAW="./fecraw"
[ -x "$FECRAW" ] || { echo "ERROR: $FECRAW not found. Run 'make' first."; exit 1; }

# ---- Test 1: binary runs and shows help ----
info "1. 帮助文本包含新特性选项"
HELP_TEXT=$($FECRAW --help 2>&1 || true)

for keyword in "adaptive-fec" "aes256gcm" "chacha20poly1305" "enable-pacing" "small-pkt" "max-bandwidth"; do
    if echo "$HELP_TEXT" | grep -q "$keyword"; then
        pass "help 包含 --$keyword"
    else
        fail "help 缺少 --$keyword"
    fi
done

# ---- Test 2: config generation includes new fields ----
info "2. 配置文件生成包含新字段"
TMPCONF="/tmp/fecraw_test_$$.toml"
$FECRAW --gen-config-server "$TMPCONF"

for field in "adaptive" "small_packet_threshold" "small_packet_redundancy" "enable_pacing" "max_bandwidth" "aes256gcm"; do
    if grep -q "$field" "$TMPCONF"; then
        pass "配置包含 $field"
    else
        fail "配置缺少 $field"
    fi
done

rm -f "$TMPCONF"

# ---- Test 3: client config generation ----
info "3. 客户端配置生成"
TMPCONF_C="/tmp/fecraw_test_client_$$.toml"
$FECRAW --gen-config-client "$TMPCONF_C"

if grep -q 'mode = "client"' "$TMPCONF_C"; then
    pass "客户端配置 mode = client"
else
    fail "客户端配置 mode 不正确"
fi

rm -f "$TMPCONF_C"

# ---- Test 4: shared library links OpenSSL ----
info "4. 共享库链接 OpenSSL"
RAW_SO="build/libudp2raw_raw.so"
if [ -f "$RAW_SO" ]; then
    if ldd "$RAW_SO" 2>/dev/null | grep -q "libcrypto\|libssl"; then
        pass "libudp2raw_raw.so 链接了 OpenSSL"
    else
        fail "libudp2raw_raw.so 未链接 OpenSSL"
    fi
else
    fail "libudp2raw_raw.so 未找到（未编译？）"
fi

# ---- Test 5: binary links the .so ----
info "5. 主程序链接共享库"
if [ -f "$FECRAW" ]; then
    if ldd "$FECRAW" 2>/dev/null | grep -q "libudp2raw_raw"; then
        pass "fecraw 链接了 libudp2raw_raw.so"
    else
        fail "fecraw 未链接 libudp2raw_raw.so"
    fi
fi

# ---- Test 6: source files present ----
info "6. 新增源文件存在"
for src in "adaptive_fec.h" "small_packet.h" "pacing.h" "raw_api.h" "raw_api.cpp"; do
    if [ -f "$src" ]; then
        pass "$src 存在"
    else
        fail "$src 缺失"
    fi
done

# ---- Test 7: bridge_mode in udp2raw sources ----
info "7. bridge_mode 已集成到 udp2raw"
if grep -q "bridge_mode" ref/udp2raw/client.cpp 2>/dev/null; then
    pass "client.cpp 包含 bridge_mode"
else
    fail "client.cpp 缺少 bridge_mode"
fi

if grep -q "bridge_mode" ref/udp2raw/server.cpp 2>/dev/null; then
    pass "server.cpp 包含 bridge_mode"
else
    fail "server.cpp 缺少 bridge_mode"
fi

if grep -q "bridge_mode = 1" raw_api.cpp 2>/dev/null; then
    pass "raw_api.cpp 设置 bridge_mode = 1"
else
    fail "raw_api.cpp 未设置 bridge_mode"
fi

# ---- Test 8: AEAD cipher support in encrypt sources ----
info "8. AEAD 加密支持"
if grep -q "cipher_aes256gcm" ref/udp2raw/encrypt.h 2>/dev/null; then
    pass "encrypt.h 包含 cipher_aes256gcm"
else
    fail "encrypt.h 缺少 cipher_aes256gcm"
fi

if grep -q "cipher_chacha20poly1305" ref/udp2raw/encrypt.h 2>/dev/null; then
    pass "encrypt.h 包含 cipher_chacha20poly1305"
else
    fail "encrypt.h 缺少 cipher_chacha20poly1305"
fi

if grep -q "cipher_is_aead" ref/udp2raw/encrypt.h 2>/dev/null; then
    pass "encrypt.h 包含 cipher_is_aead()"
else
    fail "encrypt.h 缺少 cipher_is_aead()"
fi

# ---- Test 9: source packaging ----
info "9. 源码打包"
if [ -f "scripts/package.sh" ]; then
    if bash scripts/package.sh --source 2>/dev/null; then
        SRC_PKG=$(ls -t fecraw-src-*.tar.gz 2>/dev/null | head -1)
        if [ -n "$SRC_PKG" ]; then
            for check_path in "ref/udp2raw" "makefile" "adaptive_fec.h" "pacing.h" "small_packet.h"; do
                if tar tzf "$SRC_PKG" | grep -q "$check_path"; then
                    pass "源码包包含 $check_path"
                else
                    fail "源码包缺少 $check_path"
                fi
            done
            rm -f "$SRC_PKG"
        else
            fail "源码包 tar.gz 未生成"
        fi
    else
        fail "打包脚本执行失败"
    fi
else
    fail "scripts/package.sh 不存在"
fi

# ---- Summary ----
echo ""
echo "================================"
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}"
echo "================================"

[ $FAIL -eq 0 ] && exit 0 || exit 1
