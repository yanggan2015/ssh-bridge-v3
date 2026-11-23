// agent_contract.cpp — S10/C1：18081 设备 Agent 面的发现、帮助与状态投影。
#include "services/agent/agent_contract.h"

#include <cjson/cJSON.h>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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

bool find_entry(AppCore* core, const std::string& name, AppCore::Entry& out) {
    if (!core) return false;
    for (const AppCore::Entry& e : core->entries()) {
        if (e.name == name) {
            out = e;  // entries() 返回临时快照，必须按值带出
            return true;
        }
    }
    return false;
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
    {"help.get", "GET", "/agent-help", "Agent 控制手册（Markdown/HTML）；/agent、/help、/docs 同内容", "public", "format", "text"},
    {"help.channel.get", "GET", "/agent-help/<channel>", "单通道手册：能力表、按钮 id、已填通道名 curl", "public", "channel,format", "text"},
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
    {"send.keys.post", "POST", "/send_keys", "按 key 数组发送按键/控制序列；间隔 10..1000ms；duration_ms/repeat 支持按住连发", "token", "channel,keys,interval_ms,duration_ms,repeat", "ok,count,repeat,job_id,request_id"},
    {"api.channels.get", "GET", "/api/channels", "四类通道列表（serial/ssh/powershell/telnet）", "token", "", "channels"},
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
    {"action.post", "POST", "/action", "执行板控动作；backend 可选校验，返回 job 与审计 id", "token", "channel,id,backend,session", "ok,job,job_id,queued,request_id"},
    {"lock.post", "POST", "/lock", "按通道获取/续租租约锁，默认 1000ms 最大 86400000ms", "token", "channel,session_id,ttl_ms", "ok,channel,owner,ttl_ms,expires_in_ms,renewed"},
    {"unlock.post", "POST", "/unlock", "只释放自己的通道锁；错误 session 不释放", "token", "channel,session_id", "ok,channel,released,owner"},
    {"lock.takeover.post", "POST", "/lock/takeover", "人工显式接管：清旧锁并记录 last_takeover 审计", "token", "channel,reason,session_id", "ok,channel,previous_owner,reason,at_ms"},
    {"jobs.list.get", "GET", "/jobs", "作业摘要列表；支持 state/channel/limit/before_id 过滤与分页", "public", "state,channel,limit,before_id", "ok,jobs,count,limit,total"},
    {"jobs.get", "GET", "/jobs/{id}", "单个作业详情；不存原始命令全文", "public", "id", "id,job_id,kind,channel,state,ok,output,progress,audit"},
    {"jobs.cancel.post", "POST", "/jobs/{id}/cancel", "请求取消排队/运行中的作业；已结束 409", "token", "id", "ok,id,state,cancel_requested"},
    {"boards.get", "GET", "/boards", "通道列表；与 /agent/scan 同源的 Agent 只读发现", "public", "", "ok,boards"},
    {"boards.post", "POST", "/boards", "新增/编辑四类通道；复用配置落盘与热应用", "token", "kind,old_name,channel", "ok,name,kind"},
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
    cJSON_AddStringToObject(root, "auth", web_token_required
                                                  ? "写接口使用 Authorization: Bearer <web.token>；读接口公开"
                                                  : "未启用 Web token；写接口开放，读接口公开");

    cJSON* surfaces = cJSON_AddObjectToObject(root, "surfaces");
    cJSON* device = cJSON_AddObjectToObject(surfaces, "device");
    cJSON_AddBoolToObject(device, "enabled", true);
    cJSON_AddNumberToObject(device, "port", web_port);
    cJSON_AddStringToObject(device, "base_url", "/");
    cJSON_AddStringToObject(device, "protocol", kProtocol);
    cJSON_AddStringToObject(device, "manual", "/agent-help");
    cJSON_AddStringToObject(device, "manual_md", "/agent-help?format=md");
    cJSON_AddStringToObject(device, "channel_manual", "/agent-help/<channel>");
    cJSON_AddStringToObject(device, "auth", web_token_required ? "bearer-write" : "open-write");
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
                          "nfs", "share", "tcp-forward", "agent", "config"})
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
    cJSON_AddStringToObject(device, "auth", web_token_required ? "bearer-write" : "open-write");
    cJSON_AddNumberToObject(device, "channels", core ? static_cast<double>(core->entries().size()) : 0);
    cJSON* deps = cJSON_AddArrayToObject(device, "endpoints");
    for (const char* e : {"/health", "/api/help", "/agent-help", "/agent", "/help", "/help/<channel>",
                          "/api/agent/status", "/api/agent/resolve", "/status", "/log", "/write",
                          "/action", "/job", "/api/channels"})
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

std::string md_cell(const std::string& text) {
    std::string out;
    for (char c : text) out += (c == '|' || c == '\n' || c == '\r') ? ' ' : c;
    return out;
}

std::string join_csv(const char* text, const char* empty = "-") {
    std::string out;
    for (const std::string& part : split_csv(text ? text : "")) {
        if (!out.empty()) out += ", ";
        out += part;
    }
    return out.empty() ? empty : out;
}

const char* kind_text(const std::string& kind) {
    if (kind == "serial") return "串口";
    if (kind == "ssh") return "SSH Link";
    if (kind == "powershell") return "本机 PowerShell";
    if (kind == "telnet") return "Telnet";
    return "通道";
}

const char* op_group(const std::string& path) {
    if (path == "/health" || path == "/api/help" || path == "/agent-help" ||
        path == "/agent-help/<channel>" || path == "/help" || path.find("/help") == 0 ||
        path == "/status" || path.find("/agent/") == 0 || path.find("/api/agent/") == 0)
        return "发现、状态与帮助";
    if (path == "/api/channels" || path == "/api/channels/delete" || path == "/boards" ||
        path == "/boards/delete" || path == "/reload" || path == "/reconnect")
        return "通道与设备";
    if (path == "/log" || path == "/log/download" || path == "/log/clear" || path == "/log/disk" ||
        path == "/ws/log" || path == "/write" || path == "/command" || path == "/command_block" ||
        path == "/agent/exec" || path == "/send_keys" || path == "/job" || path.find("/jobs") == 0)
        return "终端、日志与作业";
    if (path == "/lock" || path == "/unlock" || path == "/lock/takeover")
        return "租约与锁";
    if (path.find("/serial/") == 0)
        return "串口专用";
    if (path == "/action" || path == "/actions" || path == "/api/actions" ||
        path == "/api/actions/delete" || path == "/api/quick/commands" || path.find("/api/relay/") == 0)
        return "按钮、继电器与快捷指令";
    if (path.find("/api/files") == 0 || path.find("/tftp/") == 0 || path == "/send_file/progress")
        return "文件与工件";
    if (path.find("/api/tftp/") == 0 || path.find("/api/nfs/") == 0 ||
        path.find("/api/share/") == 0 || path.find("/api/tcp-forward") == 0)
        return "服务与网络";
    return "分屏与配置";
}

std::string operations_table() {
    static const char* groups[] = {
        "发现、状态与帮助", "通道与设备", "终端、日志与作业", "租约与锁", "串口专用",
        "按钮、继电器与快捷指令", "文件与工件", "服务与网络", "分屏与配置"};
    std::string out;
    for (const char* group : groups) {
        bool first = true;
        for (const OperationSpec& spec : kOperations) {
            if (std::strcmp(group, op_group(spec.path)) != 0) continue;
            if (first) {
                out += "### " + std::string(group) + "\n\n";
                out += "| 方法 | 路径 | 鉴权 | 参数 | 返回 | 说明 |\n";
                out += "|---|---|---|---|---|---|\n";
                first = false;
            }
            out += "| `" + std::string(spec.method) + "` | `" + std::string(spec.path) +
                   "` | " + spec.auth + " | " + join_csv(spec.params) + " | " +
                   join_csv(spec.returns) + " | " + md_cell(spec.description) + " |\n";
        }
        if (!first) out += "\n";
    }
    return out;
}

std::string channels_table(AppCore* core) {
    std::string out = "| 通道 | 类型 | 状态 | 设备 / 目标 | 单通道手册 |\n";
    out += "|---|---|---|---|---|\n";
    if (!core || core->entries().empty()) return out + "| - | - | 未配置 | - | - |\n";
    for (const AppCore::Entry& e : core->entries()) {
        std::string state = entry_running(e) ? "running" : "offline";
        if (!e.state.empty()) state += " / " + e.state;
        out += "| `" + md_cell(e.name) + "` | " + kind_text(e.kind) + " | " + state + " | `" +
               md_cell(e.detail) + "` | `/agent-help/" + md_cell(e.name) + "` |\n";
    }
    return out;
}

std::map<std::string, BoardActionConfig> unique_buttons(AppCore* core, const std::string& channel,
                                                        int* disabled) {
    std::map<std::string, BoardActionConfig> by_id;
    if (core) {
        for (const BoardActionConfig& b : core->buttons_of(channel)) {
            auto it = by_id.find(b.id);
            if (it == by_id.end() || (!it->second.enable && b.enable)) by_id[b.id] = b;
        }
    }
    if (disabled) {
        *disabled = 0;
        for (const auto& kv : by_id) if (!kv.second.enable) ++*disabled;
    }
    return by_id;
}

bool button_enabled(const std::map<std::string, BoardActionConfig>& buttons, const std::string& id) {
    auto it = buttons.find(id);
    return it != buttons.end() && it->second.enable;
}

std::string global_help_tail(AppCore* core, int web_port, const UiBridgeInfo& ui);

std::string help_text(AppCore* core, int web_port, const UiBridgeInfo& ui) {
    const bool token = core && core->config().web.require_token;
    const std::string base = "http://<host>:" + std::to_string(web_port);
    std::string out;

    out += "# ssh-bridge v3 — Agent 控制手册（HTTP）\n\n";
    out += "本页是给 Agent 直接读取和执行的唯一入口。网页按钮与本手册调用**同一批 18081 接口**，"
           "没有第二套 Agent 后端。\n\n";
    out += "| 项 | 值 |\n|---|---|\n";
    out += "| Base | `" + base + "` |\n";
    out += "| 原文 | `/agent-help?format=md`（浏览器渲染为 HTML；此参数返回 Markdown 原文） |\n";
    out += "| 兼容入口 | `/agent`、`/help`、`/docs`，内容相同 |\n";
    out += "| 单通道 | `/agent-help/<channel>`（命令已填好通道名） |\n";
    out += "| 机器契约 | `/api/help`，共 " + std::to_string(sizeof(kOperations) / sizeof(kOperations[0])) +
           " 项 operations |\n";
    out += "| 鉴权 | " + std::string(
           token ? "写接口必须 `Authorization: Bearer <web.token>`；读接口公开"
                 : "当前未启用 token；写接口开放，读接口公开") + " |\n";
    out += "| 18096 | 桌面 UI 调试桥（" +
           (ui.enabled ? "`http://127.0.0.1:" + std::to_string(ui.port) + "`" : std::string("disabled")) +
           "），只做窗口自检，**不是板控面**，不得作为 fallback |\n\n";

    out += "## 0. 先读这 6 条\n\n";
    out += "1. 所有板控只走 `" + base + "`；不要打开或调用 18096。\n";
    out += "2. 先发现：`GET /health` → `GET /api/help` → `GET /agent/scan` → 单通道页。\n";
    out += "3. 四类通道统一用 `channel=<name>` 路由；兼容 `board=`（串口）/`link=`（SSH）。\n";
    out += "4. 写通道前先 `POST /lock`；长任务自设 `ttl_ms`，结束 `POST /unlock`。\n";
    out += "5. 短命令用 `POST /command_block`；Linux shell 用 `POST /agent/exec`；"
           "异步用 `POST /command` 后读 `/log` 或 `/jobs/{id}`。\n";
    out += "6. 只执行当前配置里真实存在且 `enable=true` 的按钮 id；无按钮时禁止猜 `reboot`/`power_on`。\n\n";

    out += "## 1. 设备地图\n\n";
    out += "| 功能域 | 入口 | 说明 |\n|---|---|---|\n";
    out += "| 通道（串口/SSH/PowerShell/Telnet） | `/agent/scan`、`/status`、`/agent-help/<channel>` | "
           "全部通道与运行态；单通道页有已填命令 |\n";
    out += "| 板控按钮 / 继电器 | `/actions?view=agent`、`/action`、`/api/relay/devices` | "
           "按钮 id 必须来自当前列表；继电器可配置、测试 |\n";
    out += "| 快捷指令 | `/api/quick/commands` | 每通道底部快捷命令的增改查 |\n";
    out += "| 文件 / 工件 | `/api/files`、`/tftp/*`、`/send_file/progress` | "
           "共享根、TFTP 根同源；支持分片与 MD5 |\n";
    out += "| NFS / HTTP 共享 | `/api/nfs/config`、`/api/share/config` | 板端挂载与下载地址 |\n";
    out += "| TCP 转发 | `/api/tcp-forward`、`/api/tcp-forward/config` | 板网 IP 末段 → 本机端口 |\n";
    out += "| 分屏 | `/api/panes`、`/api/panes/active` | 只读快照与激活画框 |\n";
    out += "| 作业 | `/jobs`、`/jobs/{id}`、`/jobs/{id}/cancel` | 排队、运行、输出、取消 |\n";
    out += "| 通道配置 | `/api/channels`、`/boards`、`/reload`、`/reconnect` | 增改删、热重载、重连 |\n";
    out += "| 人工专用 | `/tunnel`、自升级、全局配置、入站 SSH、UDP 发现 | "
           "不提供 Agent 写接口，必须由人操作 |\n\n";

    out += "### 当前通道\n\n" + channels_table(core) + "\n";
    out += global_help_tail(core, web_port, ui);
    return out;
}

std::string global_flow() {
    std::string out = "## 2. 最短闭环（全局示例）\n\n";
    out += "```bash\nBASE=http://<host>:18081\nAUTH=()  # token 启用时改成: AUTH=(-H \"Authorization: Bearer $WEB_TOKEN\")\n";
    out += "CH=<channel>\n```\n\n";
    out += "1. 发现：`GET /health`、`GET /api/help`、`GET /agent/scan`、`GET /agent/probe?channel=$CH`。\n";
    out += "2. 锁：`POST /lock`，body `{channel,session_id,ttl_ms}`；他人持锁返回 423。\n";
    out += "3. 执行：`POST /command_block`（短命令同步）、`POST /agent/exec`（Linux 同步）、"
           "`POST /command`（异步）。\n";
    out += "4. 读：`GET /log?channel=$CH&since=..&epoch=..`；异步作业读 `GET /jobs/{id}`。\n";
    out += "5. 放：`POST /unlock`，body `{channel,session_id}`；同 session 可续租。\n\n";
    out += "```bash\n";
    out += "curl -s \"$BASE/health\"\n";
    out += "curl -s \"$BASE/api/help\"\n";
    out += "curl -s \"$BASE/agent/scan\"\n";
    out += "curl -s \"$BASE/agent/probe?channel=$CH\"\n";
    out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" \\\n";
    out += "  -d '{\"channel\":\"'$CH'\",\"command\":\"uname -a\",\"timeout_ms\":3000}' \\\n";
    out += "  \"$BASE/command_block\"\n";
    out += "curl -s \"$BASE/log?channel=$CH&chars=8192\"\n";
    out += "```\n\n";
    return out + "写接口真实下发；不确定是否长任务时先 `POST /lock`，超时用 `ttl_ms` 自保。\n\n";
}

std::string global_features() {
    std::string out = "## 3. 终端、日志与作业\n\n";
    out += "| 接口 | 语义 | 何时用 |\n|---|---|---|\n";
    out += "| `/command_block` | 在通道里执行 command，同步等输出；`timeout_ms` 毫秒 | 短命令、需要立即拿到结果 |\n";
    out += "| `/command` | 异步入队；202 + `job_id` | 长命令、可能超过请求超时 |\n";
    out += "| `/agent/exec` | 专用同步执行语义，`command`+`timeout_ms` | Agent 自己发 Linux 命令 |\n";
    out += "| `/write` | 原始字节，写什么就发什么 | 交互式 shell、U-Boot 提示符 |\n";
    out += "| `/send_keys` | 按键序列 `keys[]`，`interval_ms` 10..1000 | Ctrl+C、方向键、组合键 |\n";
    out += "| `/log` | 增量回显，`since/epoch/chars` 游标 | 读输出、抓启动日志 |\n";
    out += "| `/jobs`、`/jobs/{id}`、`/jobs/{id}/cancel` | 作业列表、详情、取消 | 异步命令跟踪 |\n\n";
    out += "`/log` 的 `since` 必须配 `epoch`；首次可只给 `chars` 取尾部。日志落盘见 `/log/disk`，"
           "`/log/clear` 不可恢复。\n\n";

    out += "## 4. 四类通道（能力与边界）\n\n";
    out += "| 类型 | 典型设备 | 能做什么 | 不能做什么 |\n|---|---|---|---|\n";
    out += "| 串口 `serial` | COM5@115200、COM3@1500000 | 读写、日志、Break、DTR/RTS、改波特率、"
           "板控按钮、U-Boot | 不能假设一定有电控；无按钮不得硬重启 |\n";
    out += "| SSH `ssh` | 远端 Linux `host:port` | 远端 shell 读写、日志、重连 | 无本地硬件电控；"
           "不要擅自 reboot |\n";
    out += "| PowerShell `powershell` | 本机 `powershell.exe`/`pwsh`/`cmd` | 本机命令、日志、重连 | "
           "会真实改本机；禁止未授权系统改动/提权 |\n";
    out += "| Telnet `telnet` | 远端 Telnet `host:port` | 远端 shell 读写、日志、重连 | 无硬件电控；"
           "不要擅自重启 |\n\n";
    out += "每类通道的具体命令见 `/agent-help/<channel>`；串口专属接口是 "
           "`/serial/open|close|break|lines|configure`。U-Boot 流程：`/action id=enter_uboot`（若有）或"
           "重启后轮询 `/log` 并 `send_keys <ctrl+c>`；文件用 TFTP。\n\n";

    out += "## 5. 按钮、继电器与快捷指令\n\n";
    out += "1. `GET /actions?view=agent&channel=<ch>` 拿当前按钮；只使用 `enable=true` 的 `id`。\n";
    out += "2. `POST /action` body `{channel,id}`；id 不存在返回错误，**不要猜** `power_on`/`power_off`/"
           "`reboot`/`enter_uboot`。\n";
    out += "3. 继电器设备：`GET/POST/PUT/DELETE /api/relay/devices`；先看设备再绑定。\n";
    out += "4. 动作配置：`GET/POST/PUT/DELETE /api/actions`；快捷指令：`/api/quick/commands`。\n";
    out += "5. 单通道页只列 `enable=true` 按钮，并按 id 去重；`enable=false` 不提供可执行 curl。\n\n";

    out += "## 6. 文件、工件与服务\n\n";
    out += "- 网页与 Agent 共用一套文件接口：`/api/files` 列表/上传/下载/删除/建目录；"
           "`/tftp/*` 是同一根目录的 TFTP 兼容面。\n";
    out += "- 大文件用 `/api/files/upload` 的 offset/chunk 或 `/tftp/upload`；传完用 `/tftp/md5` 校验；"
           "进度 `/send_file/progress`。\n";
    out += "- 通道工件目录为 `share/<channel>/`；U-Boot 从 TFTP 根取 `uImage`/`dtb`。\n";
    out += "- NFS：`/api/nfs/config`；HTTP 共享：`/api/share/config`；两者只返回配置，不自动挂载。\n";
    out += "- TCP 转发：`/api/tcp-forward` 运行态，`/api/tcp-forward/config` 规则；端口映射按规则返回。\n\n";

    out += "## 7. 分屏、配置与边界\n\n";
    out += "- 分屏只读：`GET /api/panes`、`GET /api/panes/active`；不要用它们下发命令。\n";
    out += "- 通道配置：`/api/channels`（Agent 契约）、`/boards`（兼容）、删除 `/api/channels/delete`、"
           "`/boards/delete`；改完 `POST /reload`，连接失效 `POST /reconnect`。\n";
    out += "- 人工专用，不提供 Agent 写接口：`/tunnel` 启停、自升级、全局配置/密钥、入站 SSH、UDP 发现、"
           "18096 UI 桥。需要这些能力时让用户操作。\n";
    out += "- 18096 只用于窗口自检；Agent 板控唯一入口是 18081。\n\n";
    return out;
}

std::string global_catalog() {
    const std::size_t count = sizeof(kOperations) / sizeof(kOperations[0]);
    std::string out = "## 8. 全部接口目录（自动生成，共 " + std::to_string(count) + " 项）\n\n";
    out += "下表与 `/api/help` 的 `operations[]` 同源；网页按钮也调用这些 handler。"
           "`<channel>` 是路径占位符，实际用通道名。\n\n";
    out += operations_table();
    out += "## 9. 错误与安全\n\n";
    out += "| 现象 | 处理 |\n|---|---|\n";
    out += "| 401 / `unauthorized` | 写接口需要 `Authorization: Bearer <web.token>` |\n";
    out += "| 423 / `agent_lock` | 他人持锁；读状态或等 TTL，不要抢写 |\n";
    out += "| 400 / `invalid_argument` | 参数越界；按接口说明修正 |\n";
    out += "| 404 / `channel_not_found` | 先 `/agent/scan`，不要猜通道名 |\n";
    out += "| 202 + `job_id` | 异步已入队；轮询 `/jobs/{id}` 或 `/log` |\n";
    out += "| 5xx | 记录 `request_id`，停止重试并反馈用户/维护者 |\n\n";
    out += "红线：`/write`、`/command`、`/action` 真实下发；`/log/clear`、删除通道、删除文件不可逆；"
           "无按钮不模拟上下电；SSH/Telnet/PowerShell 不做未授权破坏性操作；先锁后写。\n";
    return out;
}

std::string global_help_tail(AppCore* core, int web_port, const UiBridgeInfo& ui) {
    (void)core;
    (void)web_port;
    (void)ui;
    return global_flow() + global_features() + global_catalog();
}

std::string channel_commands(AppCore* core, const AppCore::Entry& e, int web_port) {
    (void)core;
    const std::string base = "http://<host>:" + std::to_string(web_port);
    const std::string ch = json_escape(e.name);
    std::string out = "## 2. 最短闭环（命令已填通道名）\n\n";
    out += "```bash\nBASE=" + base + "\nAUTH=()  # token 启用时改成: AUTH=(-H \"Authorization: Bearer $WEB_TOKEN\")\n"
           "```\n\n";
    out += "### 2.1 看状态与终端类型\n\n";
    out += "```bash\ncurl -s \"$BASE/agent/state?channel=" + ch + "\"\n";
    out += "curl -s \"$BASE/agent/probe?channel=" + ch + "\"\n";
    out += "curl -s \"$BASE/actions?view=agent&channel=" + ch + "\"\n```\n\n";
    out += "### 2.2 加锁（长任务先锁）\n\n";
    out += "```bash\ncurl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" \\\n";
    out += "  -d '{\"channel\":\"" + ch + "\",\"session_id\":\"agent-1\",\"ttl_ms\":3600000}' \\\n";
    out += "  \"$BASE/lock\"\n```\n\n";
    out += "### 2.3 执行并等待输出（推荐）\n\n";
    out += "```bash\ncurl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" \\\n";
    out += "  -d '{\"channel\":\"" + ch + "\",\"command\":\"uname -a\",\"timeout_ms\":3000}' \\\n";
    out += "  \"$BASE/command_block\"\n```\n\n";
    out += "### 2.4 增量读日志\n\n";
    out += "```bash\ncurl -s \"$BASE/log?channel=" + ch + "&since=0&epoch=0\"\n";
    out += "curl -s \"$BASE/log?channel=" + ch + "&chars=8192\"\n```\n\n";
    out += "### 2.5 解锁\n\n";
    out += "```bash\ncurl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" \\\n";
    out += "  -d '{\"channel\":\"" + ch + "\",\"session_id\":\"agent-1\"}' \"$BASE/unlock\"\n```\n\n";
    return out;
}

std::string channel_buttons(AppCore* core, const AppCore::Entry& e, int web_port) {
    const bool token = core && core->config().web.require_token;
    const std::string base = "http://<host>:" + std::to_string(web_port);
    const std::string ch = json_escape(e.name);
    const std::string auth = token ? "-H \"Authorization: Bearer $WEB_TOKEN\" " : "";
    int disabled = 0;
    const std::map<std::string, BoardActionConfig> buttons = unique_buttons(core, e.name, &disabled);
    std::string out = "## 3. 板控按钮 / 继电器\n\n";
    bool any = false;
    for (const auto& kv : buttons) {
        const BoardActionConfig& b = kv.second;
        if (!b.enable) continue;
        any = true;
        out += "- `" + b.id + "` — " + b.label + "，backend=`" + b.backend + "`\n";
        out += "```bash\ncurl -s -X POST " + auth + "-H \"Content-Type: application/json\" \\\n";
        out += "  -d '{\"channel\":\"" + ch + "\",\"id\":\"" + json_escape(b.id) + "\"}' \\\n";
        out += "  \"" + base + "/action\"\n```\n";
    }
    if (!any) {
        out += "本通道**没有已使能的按钮**；不要猜 `power_on`/`power_off`/`reboot`/`enter_uboot`，"
               "也不要用命令模拟断电。\n";
    }
    if (disabled > 0) out += "\n另有 " + std::to_string(disabled) + " 个 `enable=false` 按钮未列出；不要调用。\n";
    return out + "\n";
}

std::string channel_kind_section(const AppCore::Entry& e, int web_port) {
    const std::string ch = json_escape(e.name);
    (void)web_port;
    std::string out = "## 4. 本类型专属能力\n\n";
    if (e.kind == "serial") {
        out += "串口通道：可以开关串口、发 Break、控制 DTR/RTS、改波特率。\n\n";
        out += "```bash\n";
        out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\"}' \"$BASE/serial/open\"\n";
        out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\",\"duration_ms\":200}' \"$BASE/serial/break\"\n";
        out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\",\"dtr\":true,\"rts\":true}' \"$BASE/serial/lines\"\n";
        out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" \\\n";
        out += "  -d '{\"channel\":\"" + ch + "\",\"baudrate\":115200,\"databits\":8,"
               "\"parity\":\"N\",\"stopbits_x10\":10,\"flowcontrol\":\"none\"}' \"$BASE/serial/configure\"\n";
        out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\"}' \"$BASE/serial/close\"\n```\n\n";
    } else if (e.kind == "ssh") {
        out += "SSH Link：本工具对目标维护一条出站 SSH/PTY；写命令等于在远端 shell 输入。\n\n";
        out += "- 没有硬件上电/下电/重启；不要为了抓启动日志对远端发 `reboot`/`poweroff`。\n";
        out += "- 网络断了用 `POST /reconnect`；输出仍从 `/log` 读。\n\n";
        out += "```bash\ncurl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\"}' \"$BASE/reconnect\"\n```\n\n";
    } else if (e.kind == "powershell") {
        out += "PowerShell：在本机启动 `powershell.exe`/`pwsh`/`cmd` 子进程，命令会真实改本机。\n\n";
        out += "- 只执行用户明确要求的命令；不要自行改系统设置、删文件或提权。\n";
        out += "- 无硬件按钮；不要用 PowerShell 命令模拟板卡上下电。\n\n";
        out += "```bash\ncurl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\"}' \"$BASE/reconnect\"\n```\n\n";
    } else if (e.kind == "telnet") {
        out += "Telnet：出站连接到远端 Telnet 服务；命令在远端 shell 执行。\n\n";
        out += "- 没有硬件按钮；断线用 `/reconnect`；不要主动重启远端。\n\n";
        out += "```bash\ncurl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
               "-d '{\"channel\":\"" + ch + "\"}' \"$BASE/reconnect\"\n```\n\n";
    } else {
        out += "通用通道：按 `/status` 的 kind 字段判断能力；未知类型只做读写，不猜硬件动作。\n\n";
    }
    return out + "所有写接口真实下发；`/log/clear` 不可恢复；先锁后写，结束解锁。\n\n";
}

std::string channel_help(AppCore* core, const std::string& channel, int web_port) {
    AppCore::Entry e;
    if (!find_entry(core, channel, e)) return "channel not found";
    const bool token = core && core->config().web.require_token;
    const std::string base = "http://<host>:" + std::to_string(web_port);
    int disabled = 0;
    const std::map<std::string, BoardActionConfig> buttons = unique_buttons(core, e.name, &disabled);
    std::string out = "# 通道手册：`" + e.name + "`\n\n";
    out += "全局手册：`" + base + "/agent-help`。本页命令已填通道名。\n\n";
    out += "| 项 | 值 |\n|---|---|\n";
    out += "| 通道 | `" + md_cell(e.name) + "` |\n";
    out += "| 类型 | " + std::string(kind_text(e.kind)) + "（`" + e.kind + "`） |\n";
    out += "| 状态 | " + std::string(entry_running(e) ? "running" : "offline") +
           (e.state.empty() ? "" : " / " + e.state) + " |\n";
    out += "| 设备 / 目标 | `" + md_cell(e.detail) + "` |\n";
    out += "| 鉴权 | " + std::string(token ? "写接口需 Bearer token" : "token 未启用，写接口开放") + " |\n\n";
    out += "**能力表**\n\n| 能力 | 可用 | 用法 |\n|---|---|---|\n";
    out += "| 命令 / 读日志 | + | `/command`、`/command_block`、`/agent/exec`、`/log` |\n";
    out += "| 原始写 / 按键 | + | `/write`、`/send_keys` |\n";
    out += "| 串口开关 / Break / DTR/RTS | " +
           std::string(e.kind == "serial" ? "+" : "−") + " | " +
           std::string(e.kind == "serial" ? "`/serial/*`" : "仅串口通道") + " |\n";
    const bool pwr_on = button_enabled(buttons, "power_on");
    const bool pwr_off = button_enabled(buttons, "power_off");
    const bool reboot = button_enabled(buttons, "reboot");
    const bool uboot = button_enabled(buttons, "enter_uboot");
    const bool hardware = e.kind == "serial";
    const std::string power_state = (pwr_on && pwr_off && reboot) ? "+" : (hardware ? "部分/无" : "−");
    const std::string power_note = hardware
                                       ? (reboot ? "优先 `POST /action id=reboot`" : "无按钮时禁止猜 id")
                                       : "本通道无硬件电控";
    out += "| 上电 / 下电 / 重启 | " + power_state + " | " + power_note + " |\n";
    out += "| 进 U-Boot | " + std::string(uboot ? "+" : "−") + " | " +
           (hardware ? (uboot ? "`POST /action id=enter_uboot`" : "无按钮；串口可重启 + Ctrl+C 轮询")
                     : "仅串口通道，本通道不可") + " |\n";
    out += "| 文件上板 | + | `/api/files/upload` 或 `/tftp/upload`；板端再从 share 取 |\n";
    out += "| 租约锁 | + | `/lock` → 写 → `/unlock`；他人持锁返回 423 |\n\n";
    out += channel_commands(core, e, web_port);
    out += channel_buttons(core, e, web_port);
    out += channel_kind_section(e, web_port);
    out += "## 5. 常用补充\n\n";
    out += "```bash\n";
    out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
           "-d '{\"channel\":\"" + json_escape(e.name) + "\",\"command\":\"uname -a\"}' \"$BASE/command\"\n";
    out += "curl -s \"$BASE/jobs?channel=" + json_escape(e.name) + "&limit=20\"\n";
    out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" \\\n";
    out += "  -d '{\"channel\":\"" + json_escape(e.name) + "\",\"keys\":[\"<ctrl+c>\"],"
           "\"interval_ms\":100,\"repeat\":1}' \"$BASE/send_keys\"\n";
    out += "curl -s -X POST \"${AUTH[@]}\" -H \"Content-Type: application/json\" "
           "-d '{\"channel\":\"" + json_escape(e.name) + "\",\"command\":\"uname -a\","
           "\"timeout_ms\":3000}' \"$BASE/agent/exec\"\n";
    out += "```\n\n";
    out += "危险项：`/write`、`/command`、`/action` 都会真实下发；`/log/clear` 不可恢复。"
           "无按钮禁止模拟上下电；SSH/Telnet 非用户明确要求不要 reboot。\n";
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
    const AppCore::Entry* e = nullptr;
    AppCore::Entry snapshot;
    if (find_entry(core, name, snapshot)) e = &snapshot;
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
