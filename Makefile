# ssh-bridge-windows3 · 顶层 Makefile
# 用法： mingw32-make -j4       （需 MSYS2 mingw64 在 PATH）
#        mingw32-make clean
# 产物： output/build/ssh_bridge3.exe

.RECIPEPREFIX = >

CXX      ?= g++
CC       ?= gcc
MINGW    ?= C:/msys64/mingw64
OUTDIR   ?= output
BUILDDIR := $(OUTDIR)/build
APP      := $(BUILDDIR)/ssh_bridge3.exe

# 固定使用 MSYS2 的 POSIX sh：避免从 cmd/PowerShell 启动时 make 误用 cmd，
# 导致 mkdir -p / rm -rf 这类 POSIX 命令失败。
SHELL    := $(MINGW)/../usr/bin/sh.exe
DEPFLAGS := -MMD -MP

CXXFLAGS ?= -O2 -Wall -Wextra -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE -DLIBSSH_STATIC \
            -DMG_MAX_RECV_SIZE=20971520 \
            -finput-charset=UTF-8 -fexec-charset=UTF-8 -std=c++17 \
            -Isrc -Isrc/third_party -Isrc/third_party/lua -Isrc/third_party/winnfsd/src -I$(MINGW)/include -DUI_BRIDGE_ENABLE

# 第三方 C 源码（mongoose/win_ui_bridge）：只开 -Wall；mongoose 内部未用变量按规范局部抑制
CFLAGS ?= -O2 -Wall -Wno-unused-variable -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE \
          -DMG_MAX_RECV_SIZE=20971520 \
          -DUI_BRIDGE_ENABLE -finput-charset=UTF-8 -fexec-charset=UTF-8 -pthread \
          -UUNICODE -U_UNICODE \
          -Isrc/third_party -Isrc/third_party/lua -I$(MINGW)/include -I$(MINGW)/include/cjson

LIBS := -mwindows -municode -static-libgcc -static-libstdc++ \
        -luser32 -lcomctl32 -lgdi32 -lshell32 -ladvapi32 -lcomdlg32 \
        -static $(MINGW)/lib/libssh.a -lz -lcrypto -lcjson \
        -lws2_32 -lcrypt32 -lbcrypt -liphlpapi -lshlwapi -lole32 -lpthread

SOURCES := \
  src/app/app_main.cpp \
  src/app/selftest.cpp \
  src/app/selftest_ssh.cpp \
  src/app/selftest_term.cpp \
  src/app/app_core.cpp \
  src/app/lifecycle.cpp \
  src/ui/shell/shell.cpp \
  src/ui/shell/file_browser.cpp \
  src/ui/dialogs/channel_dialog.cpp \
  src/ui/dialogs/quick_dialog.cpp \
  src/ui/sftp/sftp_window.cpp \
  src/ui/services/relay_panel.cpp \
  src/ui/services/file_service_panel.cpp \
  src/ui/tray/tray.cpp \
  src/ui/terminal/term_types.cpp \
  src/ui/terminal/ansi.cpp \
  src/ui/terminal/scrollback.cpp \
  src/ui/terminal/pane.cpp \
  src/ui/terminal/input.cpp \
  src/ui/terminal/view.cpp \
  src/ui/aibridge/bridge_host.cpp \
  src/core/cmd_queue.cpp \
  src/core/ssh/ssh_session.cpp \
  src/core/ssh/sftp_client.cpp \
  src/core/bridge/ssh/ssh_bridge.cpp \
  src/core/util/util.cpp \
  src/core/channel_core.cpp \
  src/core/logbuf/logbuf.cpp \
  src/core/config/config.cpp \
  src/core/bridge/serial/serial_bridge.cpp \
  src/services/relay/relay_ctrl.cpp \
  src/services/nfs/nfs_svc.cpp \
  src/services/nfs/winnfsd_embed.cpp \
  src/services/share/share_server.cpp \
  src/services/board_actions/process_run.cpp \
  src/services/board_actions/board_action_runner.cpp \
  src/services/script/script_lua.cpp \
  src/services/script/script_lines.cpp \
  src/services/inbound/inbound_gateway.cpp \
  src/services/tcpfwd/tcp_forward.cpp \
  src/services/tunnel/tunnel_manager.cpp \
  src/services/discovery/discovery_service.cpp \
  src/services/web/web_console.cpp \
  src/services/web/web_pages.cpp \
  src/services/agent/agent_contract.cpp \
  src/services/agent/agent_lock.cpp \
  src/services/agent/agent_scan.cpp \
  src/services/service_manager.cpp

THIRD_SOURCES := \
  src/third_party/winnfsd/src/conv.cpp \
  src/third_party/winnfsd/src/DatagramSocket.cpp \
  src/third_party/winnfsd/src/FileTable.cpp \
  src/third_party/winnfsd/src/FileTree.cpp \
  src/third_party/winnfsd/src/MountProg.cpp \
  src/third_party/winnfsd/src/NFS3Prog.cpp \
  src/third_party/winnfsd/src/NFSProg.cpp \
  src/third_party/winnfsd/src/PortmapProg.cpp \
  src/third_party/winnfsd/src/RPCProg.cpp \
  src/third_party/winnfsd/src/RPCServer.cpp \
  src/third_party/winnfsd/src/ServerSocket.cpp \
  src/third_party/winnfsd/src/Socket.cpp \
  src/third_party/winnfsd/src/SocketStream.cpp

LUA_DIR := src/third_party/lua
LUA_SOURCES := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c $(LUA_DIR)/onelua.c $(LUA_DIR)/ltests.c, $(wildcard $(LUA_DIR)/*.c))

CSOURCES := \
  src/services/tftp/tftp_svc.c \
  src/core/util/key_chars.c \
  src/third_party/mongoose.c \
  src/third_party/win_ui_bridge.c \
  $(LUA_SOURCES)

OBJS := $(patsubst src/%.cpp,$(BUILDDIR)/%.o,$(SOURCES)) \
        $(patsubst src/%.c,$(BUILDDIR)/%.o,$(CSOURCES)) \
        $(patsubst src/%.cpp,$(BUILDDIR)/%.o,$(THIRD_SOURCES))
DEPS := $(OBJS:.o=.d)

RC      := $(BUILDDIR)/app/app.res.o
WINDRES ?= $(MINGW)/bin/windres.exe

.PHONY: all dirs clean run rebuild

all: dirs $(APP)

DIRS := $(BUILDDIR) \
        $(BUILDDIR)/app \
        $(BUILDDIR)/core \
        $(BUILDDIR)/core/util \
        $(BUILDDIR)/core/logbuf \
        $(BUILDDIR)/core/config \
        $(BUILDDIR)/core/ssh \
        $(BUILDDIR)/core/bridge \
        $(BUILDDIR)/core/bridge/serial \
        $(BUILDDIR)/core/bridge/ssh \
        $(BUILDDIR)/ui \
        $(BUILDDIR)/ui/shell \
        $(BUILDDIR)/ui/dialogs \
        $(BUILDDIR)/ui/sftp \
        $(BUILDDIR)/ui/tray \
        $(BUILDDIR)/ui/terminal \
        $(BUILDDIR)/ui/aibridge \
        $(BUILDDIR)/ui/services \
        $(BUILDDIR)/services \
        $(BUILDDIR)/services/relay \
        $(BUILDDIR)/services/tftp \
        $(BUILDDIR)/services/nfs \
        $(BUILDDIR)/services/share \
        $(BUILDDIR)/services/board_actions \
        $(BUILDDIR)/services/script \
        $(BUILDDIR)/services/inbound \
        $(BUILDDIR)/services/tcpfwd \
        $(BUILDDIR)/services/tunnel \
        $(BUILDDIR)/services/discovery \
        $(BUILDDIR)/services/web \
        $(BUILDDIR)/services/agent \
        $(BUILDDIR)/third_party \
        $(BUILDDIR)/third_party/winnfsd \
        $(BUILDDIR)/third_party/winnfsd/src \
        $(BUILDDIR)/third_party/lua

dirs:
> @mkdir -p $(DIRS)

$(BUILDDIR)/third_party/winnfsd/src/%.o: src/third_party/winnfsd/src/%.cpp | dirs
> $(CXX) $(CXXFLAGS) $(DEPFLAGS) -UUNICODE -U_UNICODE -w -c -o $@ $<

$(BUILDDIR)/%.o: src/%.cpp | dirs
> $(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILDDIR)/%.o: src/%.c | dirs
> $(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(APP): $(OBJS) $(RC)
> $(CXX) $(OBJS) $(RC) -o $@ $(LIBS)
> @echo "built: $(APP)"

$(RC): src/app/app.rc src/app/app.manifest | dirs
> $(WINDRES) -O coff -o $@ $<

clean:
> rm -rf "$(BUILDDIR)"

.NOTPARALLEL: rebuild

rebuild:
> $(MAKE) clean
> $(MAKE) all

run: all
> "$(APP)"

-include $(DEPS)


