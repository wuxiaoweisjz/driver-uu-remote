AMF_INCLUDE := third_party/AMF/amf/public/include
BUILD_DIR := build
XDG_DATA_HOME ?= $(HOME)/.local/share
UU_WINEPREFIX ?= $(if $(WINEPREFIX),$(WINEPREFIX),$(XDG_DATA_HOME)/uuyc-wine/wineprefix)
UU_BIN ?= $(UU_WINEPREFIX)/drive_c/Program Files/Netease/GameViewer/bin

PE_CC ?= gcc
PE_LD ?= ld
PE_CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter -m64 -mabi=ms -fshort-wchar \
	-fno-stack-protector -ffile-prefix-map=$(CURDIR)=. -fdebug-prefix-map=$(CURDIR)=. \
	-D_WIN32 -D_WIN64 -D_M_AMD64 -DBRIDGE_NATIVE_PE \
	-I/usr/include/wine/windows -I$(AMF_INCLUDE)
PE_LIBDIR := /usr/lib/wine/x86_64-windows

.PHONY: all clean probe smoke

all: $(BUILD_DIR)/amfrt64.dll $(BUILD_DIR)/d3d11.dll $(BUILD_DIR)/uu-amf-helper $(BUILD_DIR)/amf_pe_smoke.exe

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/amfrt_bridge.pe.o: src/amfrt_bridge.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/dxva_bridge.pe.o: src/dxva_bridge.c src/dxva_bridge.h src/helper_protocol.h | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/amfrt64.dll: $(BUILD_DIR)/amfrt_bridge.pe.o $(BUILD_DIR)/dxva_bridge.pe.o src/amfrt64.def
	$(PE_LD) -mi386pep --no-insert-timestamp --dll -e DllMain -o $@ $(BUILD_DIR)/amfrt_bridge.pe.o $(BUILD_DIR)/dxva_bridge.pe.o src/amfrt64.def \
		$(PE_LIBDIR)/libd3d11.a $(PE_LIBDIR)/libdxgi.a $(PE_LIBDIR)/libole32.a \
		$(PE_LIBDIR)/libdxguid.a $(PE_LIBDIR)/libuuid.a \
		$(PE_LIBDIR)/libws2_32.a $(PE_LIBDIR)/libkernel32.a $(PE_LIBDIR)/libmsvcrt.a

$(BUILD_DIR)/d3d11_proxy.pe.o: src/d3d11_proxy.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/d3d11.dll: $(BUILD_DIR)/d3d11_proxy.pe.o src/d3d11_proxy.def
	$(PE_LD) -mi386pep --no-insert-timestamp --dll -e DllMain -o $@ $< src/d3d11_proxy.def \
		$(PE_LIBDIR)/libd3d11.a $(PE_LIBDIR)/libdxgi.a $(PE_LIBDIR)/libkernel32.a $(PE_LIBDIR)/libmsvcrt.a

$(BUILD_DIR)/uu-amf-helper: src/amf_helper.c src/helper_protocol.h | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra $(shell pkg-config --cflags libavcodec libavutil) \
		-o $@ src/amf_helper.c $(shell pkg-config --libs libavcodec libavutil) -pthread

$(BUILD_DIR)/helper_decode_smoke: tests/helper_decode_smoke.c src/helper_protocol.h | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra -Isrc -o $@ tests/helper_decode_smoke.c

$(BUILD_DIR)/uinput_escape: tests/uinput_escape.c | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra -o $@ $<

$(BUILD_DIR)/x11_activate: tests/x11_activate.c | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra $(shell pkg-config --cflags x11) \
		-o $@ $< $(shell pkg-config --libs x11)

$(BUILD_DIR)/amf_pe_smoke.o: tests/amf_pe_smoke.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/amf_pe_smoke.exe: $(BUILD_DIR)/amf_pe_smoke.o
	$(PE_LD) -mi386pep --no-insert-timestamp --subsystem console -e mainCRTStartup -o $@ $< \
		$(PE_LIBDIR)/libd3d11.a $(PE_LIBDIR)/libdxgi.a $(PE_LIBDIR)/libole32.a \
		$(PE_LIBDIR)/libdxguid.a $(PE_LIBDIR)/libuuid.a \
		$(PE_LIBDIR)/libkernel32.a $(PE_LIBDIR)/libmsvcrt.a

$(BUILD_DIR)/win_click.o: tests/win_click.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/win_click.exe: $(BUILD_DIR)/win_click.o
	$(PE_LD) -mi386pep --no-insert-timestamp --subsystem console -e mainCRTStartup -o $@ $< \
		$(PE_LIBDIR)/libuser32.a $(PE_LIBDIR)/libkernel32.a

$(BUILD_DIR)/d3d11_dxvk.dll: | $(BUILD_DIR)
	cp "$(UU_BIN)/d3d11_dxvk.dll" $@

$(BUILD_DIR)/dxgi.dll: | $(BUILD_DIR)
	cp "$(UU_BIN)/dxgi.dll" $@

smoke: $(BUILD_DIR)/amfrt64.dll $(BUILD_DIR)/d3d11.dll $(BUILD_DIR)/d3d11_dxvk.dll $(BUILD_DIR)/dxgi.dll $(BUILD_DIR)/uu-amf-helper $(BUILD_DIR)/amf_pe_smoke.exe
	@set -e; \
	./$(BUILD_DIR)/uu-amf-helper 47891 >/tmp/uu-amf-helper-make-smoke.log 2>&1 & helper_pid=$$!; \
	trap 'kill $$helper_pid 2>/dev/null || true' EXIT; \
	sleep 1; \
	WINEPREFIX="$${UU_WINEPREFIX:-$${WINEPREFIX:-$${XDG_DATA_HOME:-$$HOME/.local/share}/uuyc-wine/wineprefix}}" \
	WINEDLLOVERRIDES=amfrt64=n UU_AMF_HELPER_PORT=47891 WINEDEBUG=-all \
	wine ./$(BUILD_DIR)/amf_pe_smoke.exe

probe:
	./scripts/probe-host.sh

clean:
	$(RM) -r $(BUILD_DIR)
