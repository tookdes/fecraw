CC = g++

UDPRAW_DIR = ref/udp2raw
TINYFEC_DIR = ref/tinyfecVPN
UDPSPEEDER_DIR = ref/UDPspeeder
FEC_LIBEV_DIR = $(UDPSPEEDER_DIR)/libev
RAW_LIBEV_DIR = $(UDPRAW_DIR)/libev

CFLAGS_COMMON = -std=c++11 -O2 -Wall -DUDP2RAW_LINUX

CFLAGS_FEC = $(CFLAGS_COMMON) -I$(UDPSPEEDER_DIR) -I$(FEC_LIBEV_DIR) -I$(TINYFEC_DIR) -I.

CFLAGS_RAW = $(CFLAGS_COMMON) -fPIC -fvisibility=hidden -DFECRAW_USE_OPENSSL \
	-I$(UDPRAW_DIR) -I$(RAW_LIBEV_DIR) -I$(UDPRAW_DIR)/lib -I.

CFLAGS_RAW_API = $(CFLAGS_RAW) -DRAW_API_BUILDING_SO

LDFLAGS = -lpthread -lrt

BUILD_DIR = build

TARGET = fecraw
RAW_SO = $(BUILD_DIR)/libudp2raw_raw.so

# ---- FEC side objects (UDPspeeder + tinyfecVPN + fecraw glue) ----
FEC_OBJS = \
	$(BUILD_DIR)/main.o \
	$(BUILD_DIR)/fecraw_client.o \
	$(BUILD_DIR)/fecraw_server.o \
	$(BUILD_DIR)/fec_common.o \
	$(BUILD_DIR)/fec_log.o \
	$(BUILD_DIR)/fec_misc.o \
	$(BUILD_DIR)/fec_connection.o \
	$(BUILD_DIR)/fec_fd_manager.o \
	$(BUILD_DIR)/fec_manager.o \
	$(BUILD_DIR)/rs.o \
	$(BUILD_DIR)/fec_codec.o \
	$(BUILD_DIR)/crc32.o \
	$(BUILD_DIR)/delay_manager.o \
	$(BUILD_DIR)/packet.o \
	$(BUILD_DIR)/tun_dev.o \
	$(BUILD_DIR)/ev_fec.o

# ---- RAW side objects (udp2raw + raw_api) -> shared library ----
RAW_OBJS = \
	$(BUILD_DIR)/raw_api.o \
	$(BUILD_DIR)/raw_common.o \
	$(BUILD_DIR)/raw_misc.o \
	$(BUILD_DIR)/raw_network.o \
	$(BUILD_DIR)/raw_connection.o \
	$(BUILD_DIR)/raw_encrypt.o \
	$(BUILD_DIR)/raw_client.o \
	$(BUILD_DIR)/raw_server.o \
	$(BUILD_DIR)/raw_log.o \
	$(BUILD_DIR)/raw_fd_manager.o \
	$(BUILD_DIR)/aes.o \
	$(BUILD_DIR)/aes_wrapper.o \
	$(BUILD_DIR)/md5.o \
	$(BUILD_DIR)/pbkdf2_sha1.o \
	$(BUILD_DIR)/pbkdf2_sha256.o \
	$(BUILD_DIR)/ev_raw.o

.PHONY: all clean install git_version_headers

all: $(BUILD_DIR) git_version_headers $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

git_version_headers:
	echo 'const char *gitversion = "fecraw-merged";' > $(UDPRAW_DIR)/git_version.h

# ---- Shared library: complete symbol isolation via .so boundary ----
$(RAW_SO): $(RAW_OBJS)
	$(CC) -shared -Wl,-Bsymbolic -o $@ $^ -lpthread -lrt -lssl -lcrypto
	@echo "Built shared library: $@"

# ---- Final executable ----
$(TARGET): $(FEC_OBJS) $(RAW_SO)
	$(CC) $(CFLAGS_FEC) -o $@ $(FEC_OBJS) -L$(BUILD_DIR) -ludp2raw_raw \
		-Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/build' \
		-Wl,-rpath,/usr/local/lib/fecraw $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# ---- fecraw glue (compiled with UDPspeeder headers) ----
$(BUILD_DIR)/main.o: main.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fecraw_client.o: fecraw_client.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fecraw_server.o: fecraw_server.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

# ---- UDPspeeder library sources ----
$(BUILD_DIR)/fec_common.o: $(UDPSPEEDER_DIR)/common.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fec_log.o: $(UDPSPEEDER_DIR)/log.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fec_misc.o: $(UDPSPEEDER_DIR)/misc.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fec_connection.o: $(UDPSPEEDER_DIR)/connection.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fec_fd_manager.o: $(UDPSPEEDER_DIR)/fd_manager.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/fec_manager.o: $(UDPSPEEDER_DIR)/fec_manager.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/rs.o: $(UDPSPEEDER_DIR)/lib/rs.cpp
	$(CC) $(CFLAGS_FEC) -I$(UDPSPEEDER_DIR)/lib -c $< -o $@

$(BUILD_DIR)/fec_codec.o: $(UDPSPEEDER_DIR)/lib/fec.cpp
	$(CC) $(CFLAGS_FEC) -I$(UDPSPEEDER_DIR)/lib -c $< -o $@

$(BUILD_DIR)/crc32.o: $(UDPSPEEDER_DIR)/crc32/Crc32.cpp
	$(CC) $(CFLAGS_FEC) -I$(UDPSPEEDER_DIR)/crc32 -c $< -o $@

$(BUILD_DIR)/delay_manager.o: $(UDPSPEEDER_DIR)/delay_manager.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/packet.o: $(UDPSPEEDER_DIR)/packet.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

$(BUILD_DIR)/tun_dev.o: $(TINYFEC_DIR)/tun_dev.cpp
	$(CC) $(CFLAGS_FEC) -c $< -o $@

# ---- libev: separate copies for FEC and RAW (each thread needs its own) ----
$(BUILD_DIR)/ev_fec.o: $(UDPSPEEDER_DIR)/my_ev.cpp
	$(CC) -O2 -w -I$(UDPSPEEDER_DIR) -I$(FEC_LIBEV_DIR) -c $< -o $@

$(BUILD_DIR)/ev_raw.o: $(UDPRAW_DIR)/my_ev.cpp
	$(CC) -O2 -w -fPIC -fvisibility=hidden -I$(UDPRAW_DIR) -I$(RAW_LIBEV_DIR) -c $< -o $@

# ---- raw_api wrapper (compiled with udp2raw headers, visibility default for API) ----
$(BUILD_DIR)/raw_api.o: raw_api.cpp raw_api.h
	$(CC) $(CFLAGS_RAW_API) -c $< -o $@

# ---- udp2raw library sources (all PIC + hidden visibility) ----
$(BUILD_DIR)/raw_common.o: $(UDPRAW_DIR)/common.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_misc.o: $(UDPRAW_DIR)/misc.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_network.o: $(UDPRAW_DIR)/network.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_connection.o: $(UDPRAW_DIR)/connection.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_encrypt.o: $(UDPRAW_DIR)/encrypt.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_client.o: $(UDPRAW_DIR)/client.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_server.o: $(UDPRAW_DIR)/server.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_log.o: $(UDPRAW_DIR)/log.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/raw_fd_manager.o: $(UDPRAW_DIR)/fd_manager.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

# ---- udp2raw crypto (C++ sources, PIC + hidden) ----
$(BUILD_DIR)/aes.o: $(UDPRAW_DIR)/lib/aes_faster_c/aes.cpp
	$(CC) $(CFLAGS_RAW) -I$(UDPRAW_DIR)/lib/aes_faster_c -c $< -o $@

$(BUILD_DIR)/aes_wrapper.o: $(UDPRAW_DIR)/lib/aes_faster_c/wrapper.cpp
	$(CC) $(CFLAGS_RAW) -I$(UDPRAW_DIR)/lib/aes_faster_c -c $< -o $@

$(BUILD_DIR)/md5.o: $(UDPRAW_DIR)/lib/md5.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/pbkdf2_sha1.o: $(UDPRAW_DIR)/lib/pbkdf2-sha1.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

$(BUILD_DIR)/pbkdf2_sha256.o: $(UDPRAW_DIR)/lib/pbkdf2-sha256.cpp
	$(CC) $(CFLAGS_RAW) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

install: $(TARGET)
	install -d /usr/local/bin /usr/local/lib/fecraw
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 755 scripts/fecraw-tui /usr/local/bin/fecraw-tui
	install -m 755 $(RAW_SO) /usr/local/lib/fecraw/
	ldconfig 2>/dev/null || true
	@echo "Installed $(TARGET) to /usr/local/bin/"
	@echo "Installed scripts/fecraw-tui to /usr/local/bin/"
	@echo "Installed $(RAW_SO) to /usr/local/lib/fecraw/"
