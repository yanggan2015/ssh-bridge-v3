// agent_contract.cpp — S10/C1：18081 设备 Agent 面的发现、帮助与状态投影。
#include "services/agent/agent_contract.h"

#include <cjson/cJSON.h>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/app_core.h"

namespace sb3 {
namespace agent {
namespace {

const char* kVersion = "3.0";
const char* kBuild = __DATE__ " " __TIME__;
const char* kProtocol = "1.1";

std::string json_text(cJSON* root, bool formatted = false) {
    if (!root) return "{}";
    char* p = formatted ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    std::string out = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(root);
    return out;
}

std::vector<std::string> split_csv(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const AppCore::Entry* find_entry(AppCore* core, const std::string& name) {
    if (!core) return nullptr;
    for (const AppCore::Entry& e : core->entries()) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

bool entry_running(const AppCore::Entry& e) {
    return e.bridge && e.bridge->running();
}

std::string json_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

struct OperationSpec {
    const char* id;
    const char* method;
    const char* path;
    const char* description;
    const char* auth;
    const char* params;
    const char* returns;
};

const OperationSpec kOperations[] = {
    {"health.get", "GET", "/health", "存活、版本、构建与 API 开关", "public", "", "ok,service,version,build,api_enabled"},
    {"api.help.get", "GET", "/api/help", "机器可读发现契约：surfaces + operations", "public", "", "schema,protocol,surfaces,operations"},
    {"help.get", "GET", "/help", "当前设备面的文本总览与闭环顺序", "public", "", "text"},
    {"help.channel.get", "GET", "/help/<channel>", "单通道帮助：状态、按钮 id、可复制 curl", "public", "channel", "text"},
    {"agent.status.get", "GET", "/api/agent/status", "18081 设备面与 18096 UI 桥状态", "token", "", "device,ui_bridge,endpoints"},
    {"agent.resolve.get", "GET", "/api/agent/resolve", "解析 board/link/channel，返回唯一通道", "public", "board,link,channel", "channel,kind,running"},
    {"status.get", "GET", "/status", "应用、通道、服务、分屏状态；?view=agent 精简", "public", "view", "channels,services,web"},
    {"panes.get", "GET", "/api/panes", "分屏只读快照：窗格数、激活画框、各画框通道与运行态", "public", "", "ok,split,active,count,panes"},
    {"panes.active.post", "POST", "/api/panes/active", "受 token 保护激活已绑定画框；UI 在线时同步原生窗口", "token", "index", "ok,active,channel,live"},
    {"agent.scan.get", "GET", "/agent/scan", "全通道只读上下文：连接、终端、缓冲、按钮、features", "public", "", "channels,services,locks,features"},
    {"agent.probe.get", "GET", "/agent/probe", "纯被动读日志尾部，判定终端态", "public", "board,link,channel,tail", "terminal,confidence,evidence"},
    {"agent.state.get", "GET", "/agent/state", "单通道业务状态与游标", "public", "board,link,channel", "channel,terminal,buffer,lock"},
    {"actions.agent.get", "GET", "/actions", "按钮清单；?view=agent 附加 enabled/requires_lock", "public", "view,channel", "items,count"},
    {"log.get", "GET", "/log", "通道尾部或 since/epoch 增量；响应头带游标", "public", "channel,chars,since,epoch", "channel,text,chars,epoch,total,next_since,dropped,reset"},
    {"log.download.get", "GET", "/log/download", "下载通道日志", "public", "channel", "file"},
    {"log.clear.post", "POST", "/log/clear", "清空通道日志缓冲并递增 epoch", "token", "channel", "ok,channel,epoch,total"},
    {"log.disk.post", "POST", "/log/disk", "运行时开关单通道落盘；256MB 上限", "token", "channel,enabled", "ok,channel,enabled,disk_enabled"},
    {"ws.log.get", "GET", "/ws/log", "WebSocket 二进制回显流", "public", "channel", "binary"},
    {"write.post", "POST", "/write", "向运行中的通道原样写入文本/字节，返回 202", "token", "channel,data,text", "ok,bytes,request_id"},
    {"command.post", "POST", "/command", "入队执行一条命令，只返回 job 不等待输出", "token", "channel,command,line_ending", "ok,job_id,queued,request_id"},
    {"command.block.post", "POST", "/command_block", "写入命令并等待回显/空闲，默认 2000ms 最大 60000ms", "token", "channel,command,line_ending,timeout_ms", "ok,output,timed_out,elapsed_ms,marker"},
    {"agent.exec.post", "POST", "/agent/exec", "仅 shell 态执行命令，追加结束标记并等待完成", "token", "channel,command,line_ending,timeout_ms", "ok,output,timed_out,elapsed_ms,marker"},
    {"send.keys.post", "POST", "/send_keys", "按 key 数组发送按键/控制序列，间隔 10..1000ms", "token", "channel,keys,interval_ms", "ok,count,job_id,request_id"},
    {"api.config.get", "GET", "/api/config", "完整配置 JSON（含密钥，需 token）", "token", "", "config"},
    {"config.save.post", "POST", "/api/config/save", "保存完整配置 JSON", "token", "body", "ok"},
    {"api.channels.get", "GET", "/api/channels", "串口/SSH 通道列表", "token", "", "channels"},
    {"api.channels.post", "POST", "/api/channels", "新增或编辑通道", "token", "kind,old_name,channel", "ok,channel"},
    {"channels.delete.post", "POST", "/api/channels/delete", "删除通道", "token", "kind,name", "ok"},
    {"api.actions.get", "GET", "/api/actions", "板控动作列表", "public", "", "items"},
    {"actions.create.post", "POST", "/api/actions", "新增/编辑板控动作", "token", "channel,action", "ok"},
    {"actions.delete.post", "POST", "/api/actions/delete", "删除板控动作", "token", "channel,id", "ok"},
    {"quick.commands.get", "GET", "/api/quick/commands", "快捷指令列表", "public", "", "items"},
    {"quick.commands.post", "POST", "/api/quick/commands", "新增/编辑快捷指令", "token", "channel,index,label,command", "ok"},
    {"relay.devices.get", "GET", "/api/relay/devices", "继电器设备列表", "public", "", "items"},
    {"relay.devices.post", "POST", "/api/relay/devices", "新增/编辑继电器设备", "token", "device", "ok"},
    {"relay.devices.delete.post", "POST", "/api/relay/devices/delete", "删除继电器设备", "token", "name", "ok"},
    {"files.get", "GET", "/api/files", "共享根目录列表", "public", "path", "entries"},
    {"files.upload.post", "POST", "/api/files/upload", "上传 multipart 文件", "token", "file", "ok,path"},
    {"files.download.get", "GET", "/api/files/download", "下载共享文件", "public", "path", "file"},
    {"files.delete.post", "POST", "/api/files/delete", "删除共享文件/空目录", "token", "path", "ok"},
    {"files.mkdir.post", "POST", "/api/files/mkdir", "新建共享目录", "token", "path", "ok"},
    {"tftp.config.get", "GET", "/api/tftp/config", "TFTP 配置", "public", "", "config"},
    {"tftp.config.post", "POST", "/api/tftp/config", "保存 TFTP 配置", "token", "config", "ok"},
    {"nfs.config.get", "GET", "/api/nfs/config", "NFS 配置", "public", "", "config"},
    {"nfs.config.post", "POST", "/api/nfs/config", "保存 NFS 配置", "token", "config", "ok"},
    {"share.config.get", "GET", "/api/share/config", "HTTP 共享配置", "public", "", "config"},
    {"share.config.post", "POST", "/api/share/config", "保存 HTTP 共享配置", "token", "config", "ok"},
    {"tcp.forward.get", "GET", "/api/tcp-forward", "TCP 转发运行状态", "public", "", "running"},
    {"tcp.forward.config.get", "GET", "/api/tcp-forward/config", "TCP 转发配置", "public", "", "config"},
    {"tcp.forward.config.post", "POST", "/api/tcp-forward/config", "保存 TCP 转发配置", "token", "config", "ok"},
    {"tunnel.get", "GET", "/api/tunnel", "隧道运行状态", "public", "", "running"},
    {"tunnel.config.get", "GET", "/api/tunnel/config", "隧道配置", "public", "", "config"},
    {"tunnel.config.post", "POST", "/api/tunnel/config", "保存隧道配置", "token", "config", "ok"},
    {"service.post", "POST", "/service", "启停服务", "token", "name,on", "ok,running"},
    {"action.post", "POST", "/action", "执行板控动作；backend 可选校验，返回 job 与审计 id", "token", "channel,id,backend,session", "ok,job,job_id,queued,request_id"},
    {"lock.post", "POST", "/lock", "按通道获取/续租租约锁，默认 1000ms 最大 86400000ms", "token", "channel,session_id,ttl_ms", "ok,channel,owner,ttl_ms,expires_in_ms,renewed"},
    {"unlock.post", "POST", "/unlock", "只释放自己的通道锁；错误 session 不释放", "token", "channel,session_id", "ok,channel,released,owner"},
    {"lock.takeover.post", "POST", "/lock/takeover", "人工显式接管：清旧锁并记录 last_takeover 审计", "token", "channel,reason,session_id", "ok,channel,previous_owner,reason,at_ms"},
    {"jobs.list.get", "GET", "/jobs", "作业摘要列表；支持 state/channel/limit/before_id 过滤与分页", "public", "state,channel,limit,before_id", "ok,jobs,count,limit,total"},
    {"jobs.get", "GET", "/jobs/{id}", "单个作业详情；不存原始命令全文", "public", "id", "id,job_id,kind,channel,state,ok,output,progress,audit"},
    {"jobs.cancel.post", "POST", "/jobs/{id}/cancel", "请求取消排队/运行中的作业；已结束 409", "token", "id", "ok,id,state,cancel_requested"},
    {"boards.get", "GET", "/boards", "通道列表；与 /agent/scan 同源的 Agent 只读发现", "public", "", "ok,boards"},
    {"boards.post", "POST", "/boards", "新增/编辑串口板或 SSH 通道；复用配置落盘与热应用", "token", "kind,old_name,channel", "ok,name,kind"},
    {"boards.delete.post", "POST", "/boards/delete", "删除通道；清理画框引用并释放端口", "token", "name", "ok,name"},
    {"reload.post", "POST", "/reload", "重新读配置并手术式热应用；返回 added/removed/changed/kept", "token", "", "ok,added,removed,changed,kept"},
    {"reconnect.post", "POST", "/reconnect", "单通道重连；返回 running/attempts/last_error", "token", "channel,session", "ok,channel,running,attempts,last_error"},
    {"serial.open.post", "POST", "/serial/open", "打开串口；失败返回 serial_open_failed", "token", "channel,session", "ok,channel,running,already_open"},
    {"serial.close.post", "POST", "/serial/close", "关闭串口；保留 ring", "token", "channel,session", "ok,channel,running"},
    {"serial.break.post", "POST", "/serial/break", "发送 Break；1..1000ms", "token", "channel,duration_ms", "ok,channel,duration_ms,loop"},
    {"serial.lines.post", "POST", "/serial/lines", "设置/回读 DTR/RTS", "token", "channel,dtr,rts", "ok,channel,dtr,rts"},
    {"serial.configure.post", "POST", "/serial/configure", "改波特率/数据位/校验/停止位/流控并热重开", "token", "channel,baudrate,databits,parity,stopbits_x10,flowcontrol", "ok,channel,running"},
    {"tftp.status.get", "GET", "/tftp/status", "TFTP 工件面状态：运行/端口/根目录/可写", "public", "", "ok,running,protocol,root,port,bind_ip,allow_write,note"},
    {"tftp.files.get", "GET", "/tftp/files", "浏览 TFTP 根目录与子目录文件，返回 dir/size/mtime", "public", "path", "ok,root,path,parent,items"},
    {"tftp.md5.get", "GET", "/tftp/md5", "计算 TFTP 根目录内文件 MD5 与字节数", "public", "path", "ok,path,md5,size"},
    {"tftp.download.get", "GET", "/tftp/download", "直接下载 TFTP 根目录内文件；进度见 /send_file/progress", "public", "path", "binary"},
    {"tftp.upload.post", "POST", "/tftp/upload", "分块上传到 TFTP 根目录，body 最大 16MB，支持 offset/md5 续传校验", "token", "path,offset,md5", "ok,path,size,offset,next_offset,md5"},
    {"tftp.mkdir.post", "POST", "/tftp/mkdir", "在 TFTP 根目录内创建目录", "token", "path", "ok,path"},
    {"tftp.delete.post", "POST", "/tftp/delete", "删除空目录或文件；支持 DELETE 方法", "token", "path", "ok,path,dir"},
    {"sendfile.progress.get", "GET", "/send_file/progress", "最近一次上传/下载的进度快照", "public", "", "ok,current,last"},
    {"job.get", "GET", "/job", "查询 job 状态、输出与审计摘要；不存原始命令全文", "public", "id", "id,job_id,kind,channel,state,done,ok,timed_out,output,audit"},
};

}  // namespace

std::string health_json(int web_port, const UiBridgeInfo& ui) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "ssh-bridge");
    cJSON_AddStringToObject(root, "tool", "ssh-bridge");
    cJSON_AddStringToObject(root, "version", kVersion);
    cJSON_AddStringToObject(root, "build", kBuild);
    cJSON_AddBoolToObject(root, "api_enabled", true);
    cJSON_AddNumberToObject(root, "web_port", web_port);
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON* u = cJSON_AddObjectToObject(root, "ui_bridge");
    cJSON_AddBoolToObject(u, "enabled", ui.enabled);
    cJSON_AddNumberToObject(u, "port", ui.port);
    cJSON_AddBoolToObject(u, "token_required", ui.token_required);
    return json_text(root);
}

std::string help_json(AppCore* core, int web_port, const UiBridgeInfo& ui, bool web_token_required) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "ssh-bridge");
    cJSON_AddStringToObject(root, "version", kVersion);
    cJSON_AddStringToObject(root, "build", kBuild);
    cJSON_AddNumberToObject(root, "schema", 2);
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON_AddStringToObject(root, "auth", "写接口使用 Authorization: Bearer <web.token>；读接口公开");

    cJSON* surfaces = cJSON_AddObjectToObject(root, "surfaces");
    cJSON* device = cJSON_AddObjectToObject(surfaces, "device");
    cJSON_AddBoolToObject(device, "enabled", true);
    cJSON_AddNumberToObject(device, "port", web_port);
    cJSON_AddStringToObject(device, "base_url", "/");
    cJSON_AddStringToObject(device, "protocol", kProtocol);
    cJSON_AddStringToObject(device, "auth", web_token_required ? "bearer-write" : "read-only-write-disabled");
    cJSON* ui_json = cJSON_AddObjectToObject(surfaces, "ui_bridge");
    cJSON_AddBoolToObject(ui_json, "enabled", ui.enabled);
    cJSON_AddNumberToObject(ui_json, "port", ui.port);
    cJSON_AddStringToObject(ui_json, "base_url", ("http://127.0.0.1:" + std::to_string(ui.port)).c_str());
    cJSON_AddStringToObject(ui_json, "protocol", kProtocol);

    cJSON* ops = cJSON_AddArrayToObject(root, "operations");
    for (const OperationSpec& spec : kOperations) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", spec.id);
        cJSON_AddStringToObject(e, "surface", "device");
        cJSON_AddStringToObject(e, "method", spec.method);
        cJSON_AddStringToObject(e, "path", spec.path);
        cJSON_AddStringToObject(e, "description", spec.description);
        cJSON_AddStringToObject(e, "auth", spec.auth);
        cJSON* params = cJSON_AddArrayToObject(e, "params");
        for (const std::string& p : split_csv(spec.params))
            cJSON_AddItemToArray(params, cJSON_CreateString(p.c_str()));
        cJSON* returns = cJSON_AddArrayToObject(e, "returns");
        for (const std::string& r : split_csv(spec.returns))
            cJSON_AddItemToArray(returns, cJSON_CreateString(r.c_str()));
        cJSON_AddBoolToObject(e, "implemented", true);
        cJSON_AddItemToArray(ops, e);
    }

    cJSON* pages = cJSON_AddArrayToObject(root, "pages");
    for (const char* p : {"dashboard", "channels", "actions", "button-script", "relay", "tftp",
                          "nfs", "share", "tcp-forward", "tunnel", "agent", "config"})
        cJSON_AddItemToArray(pages, cJSON_CreateString(p));
    cJSON* channels = cJSON_AddArrayToObject(root, "channels");
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            cJSON* ch = cJSON_CreateObject();
            cJSON_AddStringToObject(ch, "name", e.name.c_str());
            cJSON_AddStringToObject(ch, "kind", e.kind.c_str());
            cJSON_AddBoolToObject(ch, "running", entry_running(e));
            cJSON_AddStringToObject(ch, "state", e.state.c_str());
            cJSON_AddItemToArray(channels, ch);
        }
    }
    return json_text(root);
}

std::string status_json(AppCore* core, int web_port, const UiBridgeInfo& ui, bool web_token_required) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "service", "ssh-bridge");
    cJSON_AddStringToObject(root, "protocol", kProtocol);
    cJSON_AddNumberToObject(root, "web_port", web_port);
    cJSON* device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddBoolToObject(device, "enabled", true);
    cJSON_AddNumberToObject(device, "port", web_port);
    cJSON_AddStringToObject(device, "base_url", "/");
    cJSON_AddStringToObject(device, "auth", web_token_required ? "bearer-write" : "read-only-write-disabled");
    cJSON_AddNumberToObject(device, "channels", core ? static_cast<double>(core->entries().size()) : 0);
    cJSON* deps = cJSON_AddArrayToObject(device, "endpoints");
    for (const char* e : {"/health", "/api/help", "/help", "/help/<channel>", "/api/agent/status",
                          "/api/agent/resolve", "/status", "/log", "/write", "/action", "/job", "/api/channels"})
        cJSON_AddItemToArray(deps, cJSON_CreateString(e));

    cJSON* ui_json = cJSON_AddObjectToObject(root, "ui_bridge");
    cJSON_AddBoolToObject(ui_json, "enabled", ui.enabled);
    cJSON_AddNumberToObject(ui_json, "port", ui.port);
    cJSON_AddBoolToObject(ui_json, "token_required", ui.token_required);
    cJSON_AddStringToObject(ui_json, "base_url", ("http://127.0.0.1:" + std::to_string(ui.port)).c_str());
    cJSON* ueps = cJSON_AddArrayToObject(ui_json, "endpoints");
    for (const char* e : {"/ping", "/capabilities", "/state", "/logs?since=N"})
        cJSON_AddItemToArray(ueps, cJSON_CreateString(e));

    // 兼容旧 Agent 页面字段；这些字段只描述 18096，不代表 18081 板控面。
    cJSON_AddBoolToObject(root, "enable", ui.enabled);
    cJSON_AddNumberToObject(root, "port", ui.port);
    cJSON_AddBoolToObject(root, "token_required", ui.token_required);
    cJSON_AddStringToObject(root, "base_url", ("http://127.0.0.1:" + std::to_string(ui.port)).c_str());
    cJSON* eps = cJSON_AddArrayToObject(root, "endpoints");
    for (const char* e : {"/ping", "/capabilities", "/state", "/logs?since=N"})
        cJSON_AddItemToArray(eps, cJSON_CreateString(e));
    return json_text(root);
}

std::string help_text(AppCore* core, int web_port, const UiBridgeInfo& ui) {
    std::string out;
    out += "ssh-bridge Agent 设备控制面\n";
    out += "base: http://<host>:" + std::to_string(web_port) + "\n";
    out += "protocol: " + std::string(kProtocol) + "  build: " + kBuild + "\n";
    out += "发现: GET /health -> GET /api/help -> GET /help/<channel>\n";
    out += "通道路由: board=串口, link=SSH Link, channel=任意；三选一，冲突 400。\n\n";
    out += "闭环（短命令）:\n";
    out += "  1. GET /agent/scan 或 GET /agent/probe?channel=.. 确认运行态与终端类型\n";
    out += "  2. POST /lock {channel,session_id,ttl_ms} 占板；默认 1000ms，长任务自设 TTL\n";
    out += "  3. POST /command 或 /command_block；异步 202 后用 GET /jobs/{id} 拉输出\n";
    out += "  4. GET /log?channel=..&since=..&epoch=.. 增量读回显\n";
    out += "  5. POST /unlock {channel,session_id} 释放；同 session 可续租\n";
    out += "板控按钮: POST /action {channel,id}；不要用 18096 的 UI 点击代替。\n";
    out += "工件: GET /tftp/files -> POST /tftp/upload(offset/md5) -> GET /tftp/download；进度 GET /send_file/progress。\n\n";
    out += "危险 / 不进入 Agent 闭环:\n";
    out += "  - Tunnel 启停、网络/服务全局配置：只允许人工或管理接口，不通过 Agent 帮助代做。\n";
    out += "  - /boards/delete、/tftp/delete、/log/clear、配置保存：不可逆或影响共享状态，调用前必须显式确认。\n";
    out += "  - 18096 桌面 UI 桥只做窗口调试；Agent 不得把它作为板控代理或 fallback。\n\n";
    out += "18096 UI 调试桥（独立系统，不用于板控）: " +
           (ui.enabled ? ("http://127.0.0.1:" + std::to_string(ui.port)) : std::string("disabled")) + "\n\n";
    out += "通道:\n";
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            out += "  - " + e.name + "  kind=" + e.kind + "  " +
                   (entry_running(e) ? "running" : "offline") + "  " + e.detail + "\n";
        }
    } else {
        out += "  (core not ready)\n";
    }
    out += "\n写操作需要 Authorization: Bearer <web.token>。通道写还要过租约锁；他人持锁返回 423 agent_lock。\n";
    out += "每通道的可复制 curl 见 GET /help/<channel>。\n";
    return out;
}

std::string channel_help(AppCore* core, const std::string& channel, int web_port) {
    const AppCore::Entry* e = find_entry(core, channel);
    if (!e) return "channel not found";
    std::string out;
    out += "通道 " + e->name + "\n";
    out += "kind: " + e->kind + "  state: " + e->state + "  " +
           (entry_running(*e) ? "running" : "offline") + "\n";
    if (!e->detail.empty()) out += "detail: " + e->detail + "\n";
    out += "base: http://<host>:" + std::to_string(web_port) + "\n";
    out += "\n读:\n";
    out += "  GET /log?channel=" + e->name + "&chars=8192\n";
    out += "  GET /log?channel=" + e->name + "&since=..&epoch=..\n";
    out += "  GET /status\n";
    out += "\n写:\n";
    out += "  curl -H \"Authorization: Bearer $WEB_TOKEN\" -H \"Content-Type: application/json\" \\\n";
    out += "    -d '{\"channel\":\"" + json_escape(e->name) + "\",\"data\":\"<text>\"}' \\\n";
    out += "    http://127.0.0.1:" + std::to_string(web_port) + "/write\n";
    out += "  curl -H \"Authorization: Bearer $WEB_TOKEN\" -H \"Content-Type: application/json\" \\\n";
    out += "    -d '{\"channel\":\"" + json_escape(e->name) + "\",\"command\":\"uname -a\"}' \\\n";
    out += "    http://127.0.0.1:" + std::to_string(web_port) + "/command\n";
    out += "\n占板与释放:\n";
    out += "  curl -H \"Authorization: Bearer $WEB_TOKEN\" -H \"Content-Type: application/json\" \\\n";
    out += "    -d '{\"channel\":\"" + json_escape(e->name) + "\",\"session_id\":\"$SID\",\"ttl_ms\":3600000}' \\\n";
    out += "    http://127.0.0.1:" + std::to_string(web_port) + "/lock\n";
    out += "  curl -H \"Authorization: Bearer $WEB_TOKEN\" -H \"Content-Type: application/json\" \\\n";
    out += "    -d '{\"channel\":\"" + json_escape(e->name) + "\",\"session_id\":\"$SID\"}' \\\n";
    out += "    http://127.0.0.1:" + std::to_string(web_port) + "/unlock\n";
    out += "\n按钮:\n";
    const std::vector<BoardActionConfig>* btns = core ? core->buttons_of(e->name) : nullptr;
    if (!btns || btns->empty()) {
        out += "  (none)\n";
    } else {
        for (const BoardActionConfig& b : *btns) {
            out += "  - " + b.id + "  " + b.label + "  backend=" + b.backend +
                   (b.enable ? "" : "  disabled") + "\n";
            out += "    curl -H \"Authorization: Bearer $WEB_TOKEN\" -H \"Content-Type: application/json\" \\\n";
            out += "      -d '{\"channel\":\"" + json_escape(e->name) + "\",\"id\":\"" +
                   json_escape(b.id) + "\"}' http://127.0.0.1:" + std::to_string(web_port) + "/action\n";
        }
    }
    out += "\n危险项: /write、/command、/action 都会真实下发；/log/clear 不可恢复；先 /lock 再长任务，结束 /unlock。\n";
    out += "注意: 18096 是桌面 UI 调试桥，不是本通道的板控路径。\n";
    return out;
}

ResolveResult resolve_channel(AppCore* core, const std::string& board, const std::string& link,
                              const std::string& channel) {
    ResolveResult r;
    if (!core) {
        r.http_status = 500;
        r.error_code = "core_not_ready";
        r.error_message = "core not ready";
        return r;
    }
    int provided = (!board.empty() ? 1 : 0) + (!link.empty() ? 1 : 0) + (!channel.empty() ? 1 : 0);
    if (provided == 0) {
        r.http_status = 400;
        r.error_code = "channel_required";
        r.error_message = "one of board/link/channel is required";
        return r;
    }
    if (provided > 1) {
        r.http_status = 400;
        r.error_code = "channel_conflict";
        r.error_message = "board, link and channel are mutually exclusive";
        return r;
    }
    const std::string name = !board.empty() ? board : (!link.empty() ? link : channel);
    const std::string want_kind = !board.empty() ? "serial" : (!link.empty() ? "ssh" : "");
    const AppCore::Entry* e = find_entry(core, name);
    if (!e) {
        r.http_status = 404;
        r.error_code = "channel_not_found";
        r.error_message = "channel not found: " + name;
        return r;
    }
    if (!want_kind.empty() && e->kind != want_kind) {
        r.http_status = 400;
        r.error_code = "channel_kind_mismatch";
        r.error_message = "expect " + want_kind + " but channel is " + e->kind;
        return r;
    }
    r.ok = true;
    r.http_status = 200;
    r.name = e->name;
    r.kind = e->kind;
    r.running = entry_running(*e);
    return r;
}

std::string error_json(const std::string& code, const std::string& message,
                       const std::string& request_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON* error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", code.c_str());
    cJSON_AddStringToObject(error, "message", message.c_str());
    if (!request_id.empty()) cJSON_AddStringToObject(root, "request_id", request_id.c_str());
    return json_text(root);
}

}  // namespace agent
}  // namespace sb3

