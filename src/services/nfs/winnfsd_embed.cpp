/*
 * Embed WinNFSd NFSv3 engine in-process (no external haneWIN / WinNFSd.exe).
 * Based on winnfsd.cpp start() loop; stop via event + socket Close().
 */
#include "winnfsd_embed.h"

#ifdef _WIN32

#include "Socket.h"
#include "RPCServer.h"
#include "PortmapProg.h"
#include "NFSProg.h"
#include "MountProg.h"
#include "ServerSocket.h"
#include "DatagramSocket.h"

#include <stdio.h>
#include <string.h>
#include <process.h>
#include <windows.h>

enum { PORTMAP_PORT = 111, MOUNT_PORT = 1058, NFS_PORT = 2049 };
enum { PROG_PORTMAP = 100000, PROG_NFS = 100003, PROG_MOUNT = 100005 };
enum { SOCKET_NUM = 3 };

static CRPCServer* g_RPCServer = nullptr;
static CPortmapProg* g_PortmapProg = nullptr;
static CNFSProg* g_NFSProg = nullptr;
static CMountProg* g_MountProg = nullptr;

static HANDLE g_thread = NULL;
static HANDLE g_stop = NULL;
static HANDLE g_ready = NULL;
static volatile LONG g_running = 0;
static char g_start_err[256];
static volatile LONG g_start_ok = 0;

struct StartArgs {
    char export_path[MAX_PATH];
    char alias_path[256];
    unsigned uid;
    unsigned gid;
};

static unsigned __stdcall server_thread(void *param) {
    StartArgs *a = (StartArgs *)param;
    CDatagramSocket dgrams[SOCKET_NUM];
    CServerSocket servers[SOCKET_NUM];
    CRPCServer rpc;
    CPortmapProg portmap;
    CNFSProg nfs;
    CMountProg mount;
    int i;
    bool ok = false;

    // winnfsd 只读使用该全局地址串；const_cast 仅消除第三方签名的 const 不匹配。
    g_sInAddr = const_cast<char *>("0.0.0.0");
    portmap.Set(PROG_MOUNT, MOUNT_PORT);
    portmap.Set(PROG_NFS, NFS_PORT);
    nfs.SetUserID(a->uid, a->gid);
    mount.Export(a->export_path, a->alias_path);

    rpc.Set(PROG_PORTMAP, &portmap);
    rpc.Set(PROG_NFS, &nfs);
    rpc.Set(PROG_MOUNT, &mount);
    rpc.SetLogOn(false);

    for (i = 0; i < SOCKET_NUM; i++) {
        dgrams[i].SetListener(&rpc);
        servers[i].SetListener(&rpc);
    }

    if (servers[0].Open(PORTMAP_PORT, 3) && dgrams[0].Open(PORTMAP_PORT)) {
        if (servers[1].Open(NFS_PORT, 10) && dgrams[1].Open(NFS_PORT)) {
            if (servers[2].Open(MOUNT_PORT, 3) && dgrams[2].Open(MOUNT_PORT)) {
                ok = true;
            } else {
                snprintf(g_start_err, sizeof(g_start_err),
                         "Mount 端口 1058 绑定失败（可能被占用）");
            }
        } else {
            snprintf(g_start_err, sizeof(g_start_err),
                     "NFS 端口 2049 绑定失败（若已安装 haneWIN，请先停止其服务以释放端口）");
        }
    } else {
        snprintf(g_start_err, sizeof(g_start_err),
                 "Portmap 端口 111 绑定失败（若已安装 haneWIN，请先停止其服务以释放端口）");
    }

    if (!ok) {
        for (i = 0; i < SOCKET_NUM; i++) {
            dgrams[i].Close();
            servers[i].Close();
        }
        InterlockedExchange(&g_start_ok, 0);
        InterlockedExchange(&g_running, 0);
        SetEvent(g_ready);
        delete a;
        return 1;
    }

    g_RPCServer = &rpc;
    g_PortmapProg = &portmap;
    g_NFSProg = &nfs;
    g_MountProg = &mount;

    InterlockedExchange(&g_start_ok, 1);
    InterlockedExchange(&g_running, 1);
    SetEvent(g_ready);

    WaitForSingleObject(g_stop, INFINITE);

    g_RPCServer = nullptr;
    g_PortmapProg = nullptr;
    g_NFSProg = nullptr;
    g_MountProg = nullptr;

    for (i = 0; i < SOCKET_NUM; i++) {
        dgrams[i].Close();
        servers[i].Close();
    }
    InterlockedExchange(&g_running, 0);
    delete a;
    return 0;
}

int winnfsd_embed_running(void) { return InterlockedCompareExchange(&g_running, 0, 0) != 0; }

int winnfsd_embed_stop(void) {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        DWORD wait = WaitForSingleObject(g_thread, 15000);
        // 线程未退出时不能关闭它仍在使用的 stop/ready 句柄，也不能继续下一次 start
        if (wait != WAIT_OBJECT_0) return -1;
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = NULL;
    }
    if (g_ready) {
        CloseHandle(g_ready);
        g_ready = NULL;
    }
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_start_ok, 0);
    return 0;
}

int winnfsd_embed_start(const char *export_path, const char *alias_path, unsigned uid, unsigned gid,
                        char *err, size_t errcap) {
    StartArgs *a;
    unsigned tid;
    WSADATA wsa;

    if (err && errcap) err[0] = '\0';
    if (!export_path || !export_path[0] || !alias_path || !alias_path[0]) {
        if (err) snprintf(err, errcap, "export_path/alias 无效");
        return -1;
    }
    if (winnfsd_embed_running()) return 0;

    // 上一次停止超时说明旧线程仍持有全局句柄/端口；此时绝不能覆盖全局状态再启动
    if (winnfsd_embed_stop() != 0) {
        if (err) snprintf(err, errcap, "上一次 NFS 停止未完成，请稍后重试");
        return -1;
    }

    WSAStartup(MAKEWORD(2, 2), &wsa);

    g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_stop || !g_ready) {
        winnfsd_embed_stop();
        if (err) snprintf(err, errcap, "CreateEvent failed");
        return -1;
    }

    a = new StartArgs();
    memset(a, 0, sizeof(*a));
    strncpy(a->export_path, export_path, sizeof(a->export_path) - 1);
    strncpy(a->alias_path, alias_path, sizeof(a->alias_path) - 1);
    a->uid = uid;
    a->gid = gid;
    g_start_err[0] = '\0';
    InterlockedExchange(&g_start_ok, 0);

    g_thread = (HANDLE)_beginthreadex(NULL, 0, server_thread, a, 0, &tid);
    if (!g_thread) {
        delete a;
        winnfsd_embed_stop();
        if (err) snprintf(err, errcap, "_beginthreadex failed");
        return -1;
    }

    if (WaitForSingleObject(g_ready, 8000) != WAIT_OBJECT_0 || !g_start_ok) {
        if (err)
            snprintf(err, errcap, "%s", g_start_err[0] ? g_start_err : "NFS 引擎启动超时");
        winnfsd_embed_stop();
        return -1;
    }
    return 0;
}

#else

int winnfsd_embed_running(void) { return 0; }
int winnfsd_embed_stop(void) { return 0; }
int winnfsd_embed_start(const char *export_path, const char *alias_path, unsigned uid, unsigned gid,
                        char *err, size_t errcap) {
    (void)export_path;
    (void)alias_path;
    (void)uid;
    (void)gid;
    if (err && errcap) snprintf(err, errcap, "Windows only");
    return -1;
}

#endif
