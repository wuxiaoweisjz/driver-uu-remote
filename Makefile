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

.PHONY: all clean probe smoke capture-smoke capture-idle-smoke quality-switch-smoke \
	capture-role-smoke session-guard-smoke server-compat-smoke

all: $(BUILD_DIR)/amfrt64.dll $(BUILD_DIR)/d3d11.dll $(BUILD_DIR)/uu-amf-helper \
	$(BUILD_DIR)/uu-wayland-capture-helper $(BUILD_DIR)/amf_pe_smoke.exe \
	$(BUILD_DIR)/uu-server-launcher.exe $(BUILD_DIR)/uu-server-compat.dll

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/amfrt_bridge.pe.o: src/amfrt_bridge.c src/helper_protocol.h | $(BUILD_DIR)
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

$(BUILD_DIR)/d3d11_proxy.pe.o: src/d3d11_proxy.c src/wayland_capture_hook.h | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/wayland_capture_hook.pe.o: src/wayland_capture_hook.c src/wayland_capture_hook.h src/capture_protocol.h | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -Isrc -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/d3d11.dll: $(BUILD_DIR)/d3d11_proxy.pe.o $(BUILD_DIR)/wayland_capture_hook.pe.o src/d3d11_proxy.def
	$(PE_LD) -mi386pep --no-insert-timestamp --dll -e DllMain -o $@ \
		$(BUILD_DIR)/d3d11_proxy.pe.o $(BUILD_DIR)/wayland_capture_hook.pe.o src/d3d11_proxy.def \
		$(PE_LIBDIR)/libd3d11.a $(PE_LIBDIR)/libdxgi.a $(PE_LIBDIR)/libgdi32.a \
		$(PE_LIBDIR)/libuser32.a $(PE_LIBDIR)/libws2_32.a \
		$(PE_LIBDIR)/libkernel32.a $(PE_LIBDIR)/libmsvcrt.a

$(BUILD_DIR)/server_compat_hook.pe.o: src/server_compat_hook.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/uu-server-compat.dll: $(BUILD_DIR)/server_compat_hook.pe.o src/server_compat_hook.def
	$(PE_LD) -mi386pep --no-insert-timestamp --dll -e DllMain -o $@ $< \
		src/server_compat_hook.def $(PE_LIBDIR)/libkernel32.a $(PE_LIBDIR)/libmsvcrt.a

$(BUILD_DIR)/server_launcher.pe.o: src/server_launcher.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/uu-server-launcher.exe: $(BUILD_DIR)/server_launcher.pe.o
	$(PE_LD) -mi386pep --no-insert-timestamp --subsystem console -e mainCRTStartup \
		-o $@ $< $(PE_LIBDIR)/libkernel32.a

$(BUILD_DIR)/server_compat_fixture.o: tests/server_compat_fixture.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/server_compat_fixture.exe: $(BUILD_DIR)/server_compat_fixture.o
	$(PE_LD) -mi386pep --no-insert-timestamp --subsystem console -e mainCRTStartup \
		-o $@ $< $(PE_LIBDIR)/libsetupapi.a $(PE_LIBDIR)/libkernel32.a

server-compat-smoke: $(BUILD_DIR)/uu-server-launcher.exe \
	$(BUILD_DIR)/uu-server-compat.dll $(BUILD_DIR)/server_compat_fixture.exe
	@set -e; \
	test_dir=$(BUILD_DIR)/server-compat-test; \
	rm -rf "$$test_dir"; mkdir -p "$$test_dir"; \
	cp $(BUILD_DIR)/uu-server-launcher.exe "$$test_dir/GameViewerServer.exe"; \
	cp $(BUILD_DIR)/uu-server-compat.dll "$$test_dir/uu-server-compat.dll"; \
	cp $(BUILD_DIR)/server_compat_fixture.exe "$$test_dir/GameViewerServer.real.exe"; \
	WINEPREFIX="$(UU_WINEPREFIX)" WINEDEBUG=-all wine "$$test_dir/GameViewerServer.exe"; \
	echo 'server compatibility hook smoke passed'

$(BUILD_DIR)/uu-amf-helper: src/amf_helper.c src/helper_protocol.h | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra $(shell pkg-config --cflags libavcodec libavutil) \
		-o $@ src/amf_helper.c $(shell pkg-config --libs libavcodec libavutil) -pthread

$(BUILD_DIR)/wayland_capture_helper.moc: src/wayland_capture_helper.cpp | $(BUILD_DIR)
	$(shell pkg-config --variable=libexecdir Qt6Core)/moc $< -o $@

$(BUILD_DIR)/uu-wayland-capture-helper: src/wayland_capture_helper.cpp src/capture_protocol.h \
	$(BUILD_DIR)/wayland_capture_helper.moc | $(BUILD_DIR)
	$(CXX) -O2 -g -Wall -Wextra -std=c++17 -fPIC -Isrc \
		-I$(BUILD_DIR) $(shell pkg-config --cflags Qt6DBus Qt6Gui libei-1.0 gstreamer-app-1.0 gstreamer-video-1.0) \
		-o $@ src/wayland_capture_helper.cpp \
		$(shell pkg-config --libs Qt6DBus Qt6Gui libei-1.0 gstreamer-app-1.0 gstreamer-video-1.0) -pthread

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

$(BUILD_DIR)/gdi_capture_smoke.o: tests/gdi_capture_smoke.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/gdi_capture_smoke.exe: $(BUILD_DIR)/gdi_capture_smoke.o
	$(PE_LD) -mi386pep --no-insert-timestamp --subsystem console -e mainCRTStartup -o $@ $< \
		$(PE_LIBDIR)/libgdi32.a $(PE_LIBDIR)/libuser32.a \
		$(PE_LIBDIR)/libkernel32.a $(PE_LIBDIR)/libmsvcrt.a

$(BUILD_DIR)/streamer_capture_stub.o: tests/streamer_capture_stub.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/streamer.dll: $(BUILD_DIR)/streamer_capture_stub.o tests/streamer_capture_stub.def
	$(PE_LD) -mi386pep --no-insert-timestamp --dll -e DllMain -o $@ $< \
		tests/streamer_capture_stub.def $(PE_LIBDIR)/libd3d11.a \
		$(PE_LIBDIR)/libgdi32.a $(PE_LIBDIR)/libuser32.a \
		$(PE_LIBDIR)/libkernel32.a

$(BUILD_DIR)/wayland_capture_auto_smoke.o: tests/wayland_capture_auto_smoke.c | $(BUILD_DIR)
	$(PE_CC) $(PE_CFLAGS) -c -o $@ $<
	objcopy --remove-section=.comment --remove-section=.note.gnu.property $@

$(BUILD_DIR)/wayland_capture_auto_smoke.exe: $(BUILD_DIR)/wayland_capture_auto_smoke.o
	$(PE_LD) -mi386pep --no-insert-timestamp --subsystem console -e mainCRTStartup \
		-o $@ $< $(PE_LIBDIR)/libuser32.a $(PE_LIBDIR)/libkernel32.a

$(BUILD_DIR)/capture_idle_smoke: tests/capture_idle_smoke.c src/capture_protocol.h | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra -Isrc -o $@ $<

$(BUILD_DIR)/capture_click: tests/capture_click.c src/capture_protocol.h | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra -Isrc -o $@ $<

$(BUILD_DIR)/capture_key: tests/capture_key.c src/capture_protocol.h | $(BUILD_DIR)
	$(CC) -O2 -g -Wall -Wextra -Isrc -o $@ $<

capture-idle-smoke: $(BUILD_DIR)/uu-wayland-capture-helper $(BUILD_DIR)/capture_idle_smoke
	@set -e; \
	port=47893; \
	QT_QPA_PLATFORM=offscreen ./$(BUILD_DIR)/uu-wayland-capture-helper \
		--test-pattern $$port >/tmp/uu-wayland-capture-idle-smoke.log 2>&1 & helper_pid=$$!; \
	trap 'kill $$helper_pid 2>/dev/null || true' EXIT; \
	./$(BUILD_DIR)/uu-wayland-capture-helper --wait-ready $$port 10; \
	./$(BUILD_DIR)/capture_idle_smoke $$port

capture-smoke: $(BUILD_DIR)/gdi_capture_smoke.exe
	@set -e; \
	display=$${UU_X11_DISPLAY:-:99}; \
	xauthority=$${UU_X11_XAUTHORITY:-}; \
	env -u WAYLAND_DISPLAY DISPLAY="$$display" XAUTHORITY="$$xauthority" \
		WINEPREFIX="$(UU_WINEPREFIX)" WINEDEBUG=-all \
		wine ./$(BUILD_DIR)/gdi_capture_smoke.exe

quality-switch-smoke: $(BUILD_DIR)/d3d11.dll $(BUILD_DIR)/streamer.dll \
	$(BUILD_DIR)/wayland_capture_auto_smoke.exe $(BUILD_DIR)/uu-wayland-capture-helper
	@set -e; \
	port=47894; \
	token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef; \
	UU_CAPTURE_AUTH_TOKEN=$$token QT_QPA_PLATFORM=offscreen \
		./$(BUILD_DIR)/uu-wayland-capture-helper \
		--test-pattern $$port >/tmp/uu-wayland-quality-switch-smoke.log 2>&1 & helper_pid=$$!; \
	trap 'kill $$helper_pid 2>/dev/null || true' EXIT; \
	UU_CAPTURE_AUTH_TOKEN=$$token ./$(BUILD_DIR)/uu-wayland-capture-helper \
		--wait-ready $$port 10; \
	cd $(BUILD_DIR) && WAYLAND_DISPLAY=wayland-test \
		UU_CAPTURE_AUTH_TOKEN=$$token UU_CAPTURE_HELPER_PORT=$$port \
		UU_WAYLAND_CAPTURE_FORCE=1 \
		UU_WAYLAND_CAPTURE_STRESS=1 \
		WINEDLLOVERRIDES=d3d11=n WINEDEBUG=-all wine ./wayland_capture_auto_smoke.exe

capture-role-smoke: $(BUILD_DIR)/d3d11.dll $(BUILD_DIR)/streamer.dll \
	$(BUILD_DIR)/wayland_capture_auto_smoke.exe $(BUILD_DIR)/uu-wayland-capture-helper
	@set -e; \
	port=47895; \
	token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef; \
	cp $(BUILD_DIR)/wayland_capture_auto_smoke.exe $(BUILD_DIR)/GameViewerServer.exe; \
	cp $(BUILD_DIR)/wayland_capture_auto_smoke.exe $(BUILD_DIR)/GameViewer.exe; \
	UU_CAPTURE_AUTH_TOKEN=$$token QT_QPA_PLATFORM=offscreen \
		./$(BUILD_DIR)/uu-wayland-capture-helper --test-pattern $$port \
		>/tmp/uu-wayland-capture-role-smoke.log 2>&1 & helper_pid=$$!; \
	trap 'kill $$helper_pid 2>/dev/null || true; \
		rm -f $(BUILD_DIR)/GameViewerServer.exe $(BUILD_DIR)/GameViewer.exe' EXIT; \
	UU_CAPTURE_AUTH_TOKEN=$$token ./$(BUILD_DIR)/uu-wayland-capture-helper \
		--wait-ready $$port 10; \
	cd $(BUILD_DIR); \
	WAYLAND_DISPLAY=wayland-test UU_CAPTURE_AUTH_TOKEN=$$token \
		UU_CAPTURE_HELPER_PORT=$$port WINEDLLOVERRIDES=d3d11=n WINEDEBUG=-all \
		wine ./GameViewerServer.exe; \
	if WAYLAND_DISPLAY=wayland-test UU_CAPTURE_AUTH_TOKEN=$$token \
		UU_CAPTURE_HELPER_PORT=$$port WINEDLLOVERRIDES=d3d11=n WINEDEBUG=-all \
		wine ./GameViewer.exe; then \
		echo 'controller role unexpectedly installed capture hook' >&2; exit 1; \
	fi; \
	echo 'capture role gate passed'

session-guard-smoke:
	bash tests/session_guard_smoke.sh

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
