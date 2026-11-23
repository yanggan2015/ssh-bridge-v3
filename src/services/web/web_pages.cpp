// web_pages.cpp — S9/S14：Web 控制台页面体、样式与前端脚本。
// 设计目标：工程感、清晰层级、可操作闭环；所有写操作仍由 Web API 统一鉴权。
#include "services/web/web_pages.h"

namespace sb3 {
namespace web_pages {

extern const char* const kCss = R"CSS(
:root{
  --bg:#f4f7f6; --bg-soft:#eef5f2; --surface:#ffffff; --surface-2:#f8fbfa;
  --ink:#102a32; --ink-2:#28444b; --muted:#65777d; --line:#d8e4e1;
  --accent:#0b7a70; --accent-2:#075e56; --accent-soft:#e0f1ed; --accent-ink:#07524b;
  --warn:#9a651e; --warn-soft:#fbf1df; --danger:#b33a31; --danger-soft:#fbe9e6;
  --success:#187a58; --success-soft:#e3f3ec;
  --shadow:0 12px 30px rgba(16,42,50,.06); --shadow-sm:0 2px 8px rgba(16,42,50,.04);
  --radius:16px; --radius-sm:10px; --sidebar:248px; --max:1440px;
}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;background:var(--bg);color:var(--ink);
  font:15px/1.55 "Segoe UI",system-ui,-apple-system,"Microsoft YaHei",sans-serif}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
button,input,select,textarea{font:inherit}
button{cursor:pointer}
.layout{display:grid;grid-template-columns:var(--sidebar) minmax(0,1fr);min-height:100vh}
aside.sidebar{position:sticky;top:0;height:100vh;overflow:auto;padding:24px 16px;
  background:linear-gradient(180deg,#0b252b 0%,#0d3034 66%,#0b252b 100%);color:#dceceb;
  border-right:1px solid rgba(255,255,255,.06)}
.brand{display:flex;align-items:center;gap:11px;padding:0 8px 18px;color:#f1fbf8;text-decoration:none}
.brand:hover{text-decoration:none}
.brand-mark{width:34px;height:34px;border-radius:11px;display:grid;place-items:center;
  background:linear-gradient(145deg,#23b5a1,#0b6d65);color:#fff;font-size:13px;font-weight:800;
  letter-spacing:.5px;box-shadow:0 6px 18px rgba(11,122,112,.28)}
.brand strong{display:block;font-size:18px;letter-spacing:.2px;line-height:1.2}
.brand small{display:block;font-size:13px;font-weight:400;color:#8fb9b4;margin-top:3px}
.nav-title{margin:15px 10px 6px;font-size:13px;letter-spacing:1.8px;text-transform:uppercase;color:#7ea9a5}
nav{display:grid;gap:2px}
nav a{display:flex;align-items:center;gap:9px;color:#c9e3df;padding:9px 11px;border-radius:9px;font-size:14px;
  transition:background .15s ease,color .15s ease}
nav a::before{content:"";width:5px;height:5px;border-radius:50%;background:#628c89;flex:none}
nav a:hover{background:#153b41;text-decoration:none;color:#fff}
nav a.on{background:#116f68;color:#fff;box-shadow:inset 0 0 0 1px rgba(255,255,255,.08)}
nav a.on::before{background:#9ef1e4}
.sidebar-foot{margin:22px 8px 0;padding-top:16px;border-top:1px solid rgba(255,255,255,.08);
  font-size:13px;color:#9bc0bc;line-height:1.7}
body.pg-dashboard .layout{display:block}
body.pg-dashboard aside.sidebar{position:sticky;top:0;z-index:40;height:auto;max-height:none;
  display:flex;flex-direction:row;align-items:center;gap:14px;padding:5px 14px;overflow:visible;
  border-right:0;border-bottom:1px solid rgba(255,255,255,.08);box-shadow:0 4px 18px rgba(4,25,29,.16)}
body.pg-dashboard .brand{padding:0;flex:0 0 auto}
body.pg-dashboard .brand small{display:none}
body.pg-dashboard .brand-mark{width:28px;height:28px;border-radius:9px}
body.pg-dashboard nav{display:flex;gap:2px;flex:1 1 auto;overflow-x:auto;scrollbar-width:thin}
body.pg-dashboard nav a{padding:5px 9px;font-size:13px;border-radius:7px}
body.pg-dashboard .nav-title,body.pg-dashboard .sidebar-foot{display:none}
body.pg-dashboard main{padding:8px 12px 12px}
body.pg-dashboard .shell{max-width:none}
body.pg-dashboard #svcOverviewCard{display:none}
body.pg-dashboard header.page-head{margin:0 0 6px;align-items:center;min-height:40px}
body.pg-dashboard h1{font-size:18px;margin:0}
body.pg-dashboard .sub{display:none}
body.pg-dashboard .token-box input{width:120px}
body.pg-dashboard .terminal-card{height:calc(100vh - 140px);min-height:0;padding:9px 10px 7px}
body.pg-dashboard.nav-hidden .terminal-card{height:calc(100vh - 68px)}
.nav-toggle-global{display:none;border:1px solid var(--line);background:var(--surface-2);color:var(--ink-2);
  border-radius:8px;min-height:32px;padding:3px 10px;font-size:13px;flex:0 0 auto}
.nav-menu-btn{display:none}
body.pg-dashboard .nav-toggle-global{display:inline-flex}
body.pg-dashboard.nav-hidden aside.sidebar{display:none}
.side-status{display:flex;align-items:center;gap:8px;color:#d8eeea}
.side-meta{color:#7da7a3;font-size:13px;margin-top:5px}
.status-dot{width:8px;height:8px;border-radius:50%;background:#7b8b90;flex:none}
.status-dot.on{background:#32c49a;box-shadow:0 0 0 4px rgba(50,196,154,.14)}
.status-dot.warn{background:#d39a45}.status-dot.off{background:#718288}
main{min-width:0;padding:22px 30px 52px}
.shell{max-width:var(--max);margin:0 auto}
header.page-head{display:flex;justify-content:space-between;gap:22px;align-items:flex-start;margin-bottom:20px}
.title-wrap{min-width:0}
h1{font-size:27px;line-height:1.2;margin:0 0 5px;letter-spacing:-.3px}
.sub{color:var(--muted);font-size:13px;max-width:760px}
.header-tools{display:flex;align-items:center;gap:9px;flex-wrap:wrap;justify-content:flex-end}
.token-box{display:flex;align-items:center;background:var(--surface);border:1px solid var(--line);
  border-radius:11px;padding:3px 4px 3px 10px;box-shadow:var(--shadow-sm)}
.token-box input{border:0;background:transparent;width:145px;padding:7px 4px;outline:none;font-size:13px}
.token-box .btn{border-radius:8px;min-height:32px;padding:5px 9px;font-size:13px}
.token-state{white-space:nowrap}
.card{background:var(--surface);border:1px solid var(--line);border-radius:var(--radius);padding:18px;
  margin-bottom:16px;box-shadow:var(--shadow-sm)}
.card-head{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;margin-bottom:13px}
.card-title{display:flex;align-items:center;gap:9px;min-width:0}
.card h2,.card h3{font-size:17px;line-height:1.3;margin:0;letter-spacing:-.1px}
.card h3{font-size:15px}
.card p{margin:0}
.card-sub{color:var(--muted);font-size:13px;margin-top:3px}
.row{display:flex;gap:9px;flex-wrap:wrap;align-items:center}
.row-end{display:flex;gap:9px;flex-wrap:wrap;align-items:center;justify-content:flex-end}
.muted{color:var(--muted);font-size:13px}
.tiny{font-size:13px;color:var(--muted)}
.mono{font-family:ui-monospace,Consolas,"Cascadia Mono",monospace}
.btn{border:0;border-radius:10px;background:var(--accent);color:#fff;padding:9px 14px;min-height:38px;
  font-weight:600;transition:background .15s ease,transform .15s ease}
.btn:hover{background:var(--accent-2);transform:translateY(-1px)}
.btn:active{transform:none}
.btn:disabled{opacity:.45;cursor:not-allowed;transform:none}
.btn.ghost{background:#e8f1ef;color:var(--accent-ink)}
.btn.ghost:hover{background:#d8ebe7}
.btn.danger{background:var(--danger)}
.btn.danger:hover{background:#922d27}
.btn.soft{background:var(--accent-soft);color:var(--accent-ink)}
.btn.small{min-height:30px;padding:5px 10px;font-size:13px;border-radius:8px}
.icon-btn{width:34px;height:34px;min-height:0;padding:0;display:inline-grid;place-items:center}
input,select,textarea{border:1px solid var(--line);border-radius:9px;padding:9px 11px;background:#fff;color:var(--ink);
  min-width:0;outline:none;transition:border-color .15s ease,box-shadow .15s ease}
input:focus,select:focus,textarea:focus{border-color:#79bdb2;box-shadow:0 0 0 3px rgba(11,122,112,.12)}
textarea{width:100%;min-height:110px;font-family:ui-monospace,Consolas,monospace;font-size:13px;line-height:1.55;resize:vertical}
input[type=checkbox]{width:18px;height:18px;accent-color:var(--accent);padding:0}
.chip{display:inline-flex;align-items:center;gap:6px;border-radius:999px;padding:3px 9px;font-size:13px;
  background:var(--bg-soft);color:var(--ink-2);white-space:nowrap;border:1px solid transparent}
.chip .status-dot{width:6px;height:6px}
.chip.on{background:var(--success-soft);color:var(--success)}
.chip.off{background:#f0f2f3;color:#67777d}
.chip.warn{background:var(--warn-soft);color:var(--warn)}
.chip.danger{background:var(--danger-soft);color:var(--danger)}
.chip.accent{background:var(--accent-soft);color:var(--accent-ink)}
.pill{display:inline-flex;align-items:center;border-radius:999px;padding:3px 9px;font-size:13px;background:var(--accent-soft);color:var(--accent-ink)}
.pill.off{background:#f0f2f3;color:#66727d}
.hint{min-height:18px;font-size:13px;color:var(--muted);margin-top:8px;overflow-wrap:anywhere}
.empty{border:1px dashed #c9d8d5;background:var(--surface-2);border-radius:13px;padding:28px 20px;text-align:center;color:var(--muted)}
.empty .empty-mark{width:44px;height:44px;border-radius:50%;display:grid;place-items:center;margin:0 auto 10px;
  background:var(--accent-soft);color:var(--accent);font-size:18px;font-weight:700}
.empty strong{display:block;color:var(--ink);font-size:15px;margin-bottom:4px}
.empty p{font-size:13px;max-width:520px;margin:0 auto}
.empty .btn{margin-top:13px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:11px}
.grid-2{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:11px}
.grid-3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:11px}
.list{display:grid;gap:9px}
.item{border:1px solid var(--line);border-radius:12px;padding:13px;background:var(--surface-2)}
.item-head{display:flex;justify-content:space-between;gap:10px;align-items:flex-start;flex-wrap:wrap}
.item b{font-size:14px}
.item .meta{font-size:13px;color:var(--muted);margin:4px 0 8px;overflow-wrap:anywhere}
.item .actions{display:flex;gap:6px;flex-wrap:wrap}
pre.terminal{white-space:pre-wrap;word-break:break-word;background:#081820;color:#d9efe9;border-radius:12px;
  padding:14px 15px;min-height:340px;max-height:56vh;overflow:auto;font-family:ui-monospace,Consolas,monospace;
  font-size:var(--term-font,13px);line-height:1.48;
  border:1px solid #12343a;box-shadow:inset 0 1px 0 rgba(255,255,255,.03)}
pre.terminal mark{background:#f7d774;color:#173033;border-radius:3px;padding:0 1px}
pre.terminal mark.active{background:#7ee0c8;color:#05332e;box-shadow:0 0 0 2px rgba(126,224,200,.45)}
.terminal-shell{display:flex;flex-direction:column;flex:1;min-height:0;border:1px solid #12343a;border-radius:13px;
  overflow:hidden;background:#081820;box-shadow:inset 0 1px 0 rgba(255,255,255,.03)}
.terminal-shell .terminal{border:0;border-radius:0;box-shadow:none;margin:0;padding:14px 16px;
  min-height:0;max-height:none;flex:1}
.terminal-shell>.xterm-host,.terminal-shell>pre.terminal{order:1}
/* Desktop input is the terminal itself, exactly like the Win32 TerminalView. The keybar is
   only a mobile assist strip and never a second text field. */
.terminal-keybar{order:2;display:none}
.xterm-host{display:flex;flex:1;min-height:0;padding:8px 10px 4px;background:#081820;overflow:hidden}
.xterm-host[hidden]{display:none}
.xterm-host .xterm{width:100%;height:100%}
.xterm-host .xterm-viewport{overflow-y:auto!important;scrollbar-width:thin}
.xterm-host .xterm-screen{width:100%}
.terminal-keybar{display:none;gap:4px;align-items:center;flex-wrap:nowrap;overflow-x:auto;padding:6px 8px;
  background:#0c2028;border-top:1px solid #16383f;flex:0 0 auto;scrollbar-width:thin}
.terminal-keybar button{flex:0 0 auto;border:1px solid #24525a;background:#102c34;color:#b9ded6;
  border-radius:7px;padding:4px 8px;font:13px/1.2 ui-monospace,Consolas,monospace;cursor:pointer;min-height:26px}
.terminal-keybar button:hover{background:#17424b;border-color:#3c7c84;color:#e9fbf7}
.terminal-tools{display:flex;gap:6px;align-items:center;flex-wrap:wrap;margin:8px 0 7px}
.terminal-tools input{flex:1 1 150px;min-width:110px;font-size:13px}
.terminal-tools .btn{padding:5px 9px;min-height:30px;font-size:13px}
.pane-strip{display:flex;gap:7px;align-items:center;flex-wrap:wrap;margin:8px 0 0}
.terminal-bar{display:flex;align-items:center;gap:10px;min-width:0;margin:5px 0 4px}
#rxBar{display:none}
.terminal-bar .pane-strip{margin:0;flex:0 1 auto;min-width:0}
.terminal-bar .terminal-tools{margin:0;flex:1 1 auto;min-width:0;flex-wrap:nowrap;overflow-x:auto;justify-content:flex-end;scrollbar-width:thin}
.pane-strip .pane-label{font-size:13px;color:var(--muted);white-space:nowrap}
.pane-chip{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);background:var(--surface-2);
  color:var(--ink);border-radius:10px;padding:5px 9px;font-size:13px;min-height:29px}
.pane-chip.active{border-color:#9fd6cc;background:var(--accent-soft);color:var(--accent);font-weight:600}
.pane-chip.empty{opacity:.55;cursor:not-allowed}
.pane-chip .pane-dot{width:7px;height:7px;border-radius:50%;background:#aab9b6}
.pane-chip.on .pane-dot{background:#34a98f}
.terminal-card:fullscreen{background:var(--surface);padding:16px;min-height:100vh}
.terminal-card:fullscreen .terminal{max-height:none;min-height:0;flex:1}
pre{white-space:pre-wrap;word-break:break-word;background:#0c1820;color:#dcefe9;border-radius:10px;padding:12px;
  font:13px/1.5 ui-monospace,Consolas,monospace;max-height:300px;overflow:auto}
.terminal-card{display:flex;flex-direction:column;min-height:540px;height:calc(100vh - 205px);padding:14px 14px 12px}
.dash .terminal-card{grid-column:2}
.terminal-card .terminal{flex:1}
.terminal-meta{display:flex;align-items:center;gap:7px;flex-wrap:wrap;margin-top:4px}
.terminal-foot{display:flex;gap:8px;align-items:center;margin-top:11px}
.terminal-foot input{flex:1;font-family:ui-monospace,Consolas,monospace}
.dash{display:grid;grid-template-columns:244px minmax(0,1fr);gap:14px;align-items:stretch;min-height:540px}
.dash.nav-collapsed{grid-template-columns:0 minmax(0,1fr);gap:0}
.dash.nav-collapsed .channel-card{display:none}
.nav-toggle{border:1px solid var(--line);background:var(--surface-2);color:var(--ink-2);border-radius:8px;
  padding:5px 9px;font-size:13px;cursor:pointer;white-space:nowrap}
.nav-toggle:hover{border-color:#9fd6cc;background:var(--accent-soft);color:var(--accent)}
.channel-card{padding:13px;height:calc(100vh - 205px);min-height:540px;overflow:hidden;display:flex;flex-direction:column}
.channel-card .channel-list{flex:1;overflow:auto;min-height:0}
.channel-list{display:grid;gap:6px}
.channel-item{display:flex;align-items:center;gap:9px;width:100%;text-align:left;border:1px solid transparent;
  background:transparent;border-radius:10px;padding:9px;color:var(--ink);min-height:0;
  flex-wrap:wrap;row-gap:3px;overflow:hidden}
.channel-item:hover{background:var(--surface-2);border-color:var(--line)}
.channel-item.active{background:var(--accent-soft);border-color:#a8d8d0}
.channel-item .ch-main{min-width:0;flex:1}
.channel-item .ch-name{font-size:13px;font-weight:600;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.channel-item .ch-detail{font-size:13px;color:var(--muted);display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.channel-item .ch-num{flex:0 0 100%;padding-left:19px;font-size:13px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.svc-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(145px,1fr));gap:9px}
.svc-card{display:flex;align-items:center;justify-content:space-between;gap:8px;border:1px solid var(--line);
  background:var(--surface-2);border-radius:11px;padding:10px 11px;color:var(--ink);text-decoration:none}
.svc-card:hover{border-color:#a8d8d0;background:var(--accent-soft);text-decoration:none}
.svc-card .svc-name{font-size:13px;font-weight:600}
.svc-card .svc-note{font-size:13px;color:var(--muted);margin-top:2px}
.svc-card .chip{font-size:13px;padding:2px 7px}
.form-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.field{display:flex;flex-direction:column;gap:5px;min-width:0}
.field.full{grid-column:1/-1}
.field > span,.field > label{font-size:13px;font-weight:600;color:var(--ink-2)}
.field .help{font-size:13px;color:var(--muted);font-weight:400}
.field input,.field select,.field textarea{width:100%}
.check-row{display:flex;align-items:center;gap:7px;font-size:13px;color:var(--ink-2);padding-top:2px}
.check-row input{width:auto;accent-color:var(--accent)}
.secret-wrap{display:flex;gap:5px;align-items:stretch}
.secret-wrap input{flex:1}
.secret-wrap .btn{min-height:0;padding:5px 9px;font-size:13px}
.editor{background:var(--surface-2);border:1px solid var(--line);border-radius:13px;padding:15px;margin:0 0 14px}
.editor[hidden]{display:none}
.channel-cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:10px}
.chan-card{border:1px solid var(--line);border-radius:12px;background:#fff;padding:13px;display:flex;flex-direction:column;gap:8px}
.chan-card .chan-top{display:flex;align-items:flex-start;gap:8px}
.chan-card .chan-name{font-weight:700;font-size:14px;overflow-wrap:anywhere;flex:1}
.chan-card .chan-detail{font-size:13px;color:var(--muted);line-height:1.5;overflow-wrap:anywhere}
.chan-card .chan-actions{display:flex;gap:6px;flex-wrap:wrap}
.chan-card .chan-actions .btn{padding:4px 8px;min-height:0;font-size:13px}
.chan-form-section{border-top:1px solid var(--line);margin-top:12px;padding-top:12px}
.agent-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:10px}
.agent-card{border:1px solid var(--line);border-radius:12px;background:var(--surface-2);padding:13px}
.agent-card .k{font-size:13px;color:var(--muted)}
.agent-card .v{font-size:17px;font-weight:700;margin-top:3px;overflow-wrap:anywhere}
.agent-log{max-height:300px}
.agent-table{width:100%;border-collapse:collapse;font-size:13px;margin-top:10px}
.agent-table th{text-align:left;color:var(--muted);font-weight:600;padding:6px 8px;border-bottom:1px solid var(--line);white-space:nowrap}
.agent-table td{padding:7px 8px;border-bottom:1px solid var(--line);vertical-align:top;overflow-wrap:anywhere}
.agent-table tr:last-child td{border-bottom:0}
.agent-table .mono{font-family:ui-monospace,Consolas,monospace;font-size:13px}
.agent-table .state{display:inline-flex;align-items:center;gap:5px}
.lock-banner{display:flex;align-items:center;gap:10px;flex-wrap:wrap;border:1px solid #e7b03b;background:rgba(231,176,59,.12);border-radius:10px;padding:8px 11px;margin:0 0 9px;font-size:13px}
.lock-banner[hidden]{display:none}
.lock-banner strong{color:#7a4b00}
.lock-banner .btn{padding:5px 9px;min-height:30px;font-size:13px}
.help-list{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:8px}
.help-item{border:1px solid var(--line);border-radius:10px;padding:10px;background:var(--surface-2)}
.help-item code{font-size:13px;color:var(--accent-ink);overflow-wrap:anywhere}
.editor-actions{display:flex;gap:9px;align-items:center;flex-wrap:wrap;margin-top:13px;padding-top:12px;border-top:1px solid var(--line)}
.tabs{display:inline-flex;background:#e9f1ef;border-radius:10px;padding:3px;gap:2px}
.tabs button{background:transparent;color:var(--ink-2);min-height:32px;padding:5px 11px;border-radius:8px;font-size:13px;font-weight:600}
.tabs button.on{background:var(--surface);color:var(--accent-ink);box-shadow:var(--shadow-sm)}
.toolbar{display:flex;gap:9px;align-items:center;flex-wrap:wrap}
.toolbar .spacer{flex:1}
.search{max-width:210px}
.form-status{font-size:13px;color:var(--muted);min-height:17px}
.status-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:9px;margin-top:12px}
.status-cell{border:1px solid var(--line);border-radius:10px;background:var(--surface-2);padding:10px 11px}
.status-cell .k{font-size:13px;color:var(--muted)}
.status-cell .v{font-size:13px;font-weight:600;margin-top:2px;overflow-wrap:anywhere}
.status-cell.wide{grid-column:span 2}
.service-summary{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:9px}
.file-toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.file-upload{position:relative;overflow:hidden;display:inline-flex;align-items:center}
.file-upload input{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}
.breadcrumb{display:flex;gap:4px;align-items:center;flex-wrap:wrap;font-size:13px;margin:13px 0 8px;color:var(--muted)}
.breadcrumb button{background:none;border:0;color:var(--accent);padding:3px 5px;border-radius:5px;font-size:13px}
.breadcrumb button:hover{background:var(--accent-soft)}
.file-list{border:1px solid var(--line);border-radius:12px;overflow:hidden}
.file-row{display:flex;align-items:center;gap:10px;padding:9px 12px;border-bottom:1px solid #edf2f0;background:#fff}
.file-row:last-child{border-bottom:0}
.file-row:hover{background:var(--surface-2)}
.file-icon{width:28px;height:28px;border-radius:8px;display:grid;place-items:center;background:var(--bg-soft);font-size:13px;flex:none}
.file-main{min-width:0;flex:1}
.file-name{font-size:13px;font-weight:600;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.file-name.dir{color:var(--accent-ink);cursor:pointer}
.file-meta{font-size:13px;color:var(--muted);display:block}
.file-actions{display:flex;gap:4px;flex:none}
.file-actions a,.file-actions button{font-size:13px;padding:4px 7px;min-height:0;border-radius:6px}
.file-actions a{color:var(--accent);background:var(--accent-soft);display:inline-flex;align-items:center}
.file-actions a:hover{text-decoration:none;background:#d2ece6}
.toast{position:fixed;right:16px;left:auto;top:50%;transform:translateY(-50%);z-index:50;display:flex;flex-direction:column;gap:8px;max-width:340px}
.toast-item{background:#123b3d;color:#e9fbf7;border-radius:11px;padding:11px 14px;font-size:13px;box-shadow:0 12px 30px rgba(5,30,33,.26);
  animation:toast-in .18s ease-out}
.toast-item.error{background:#7f2e28}.toast-item.ok{background:#145b49}
@keyframes toast-in{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:none}}
dialog{border:0;border-radius:15px;padding:0;background:var(--surface);color:var(--ink);box-shadow:0 24px 80px rgba(7,30,34,.28);max-width:min(460px,92vw);width:92vw}
dialog::backdrop{background:rgba(5,23,27,.45);backdrop-filter:blur(2px)}
.dialog-body{padding:19px 20px 17px}
.dialog-body h3{margin:0 0 7px;font-size:17px}
.dialog-body p{margin:0;color:var(--muted);font-size:13px;line-height:1.55}
.dialog-actions{display:flex;justify-content:flex-end;gap:8px;padding:0 20px 19px}
dialog input{width:100%;margin-top:11px}
.sr-only{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
.footer-note{color:var(--muted);font-size:13px;margin-top:18px;text-align:right}
@media(max-width:1020px){
  main{padding:20px 18px 40px}
  .dash{grid-template-columns:1fr}
  .channel-list{grid-template-columns:repeat(2,minmax(0,1fr))}
  .header-tools{justify-content:flex-start}
}
@media(max-width:760px){
  :root{--sidebar:0px}
  .layout{display:block}
  aside.sidebar{position:static;height:auto;padding:8px 10px}
  .brand{padding-bottom:6px}
  .brand small{display:none}
  nav{display:flex;flex-wrap:nowrap;gap:5px;padding-bottom:2px;overflow-x:auto;scrollbar-width:thin}
  nav a{flex:0 0 auto;padding:5px 8px;font-size:13px}
  .nav-title{display:none}
  .sidebar-foot{display:none}
  main{padding:10px 12px 24px}
  header.page-head{display:flex;align-items:center;gap:8px;margin-bottom:8px}
  header.page-head .sub{display:none}
  header.page-head .header-tools .chip{display:none}
  h1{font-size:18px}
  .header-tools{margin-top:0;justify-content:flex-end}
  .token-box{width:190px}.token-box input{width:100%;flex:1}
  .card{padding:14px;border-radius:13px}
  .card-head{flex-direction:column;align-items:stretch;gap:10px}
  .card-head>.row-end{justify-content:flex-start}
  .form-grid,.grid-2,.grid-3{grid-template-columns:1fr}
  .dash{min-height:0}
  .dash .terminal-card .card-head{flex-direction:column;gap:6px;margin-bottom:7px}
  .dash .terminal-card .card-title h2,.dash .terminal-card .card-sub{display:none}
  .dash .terminal-card .terminal-meta{margin-top:0}
  .dash .terminal-card #reload,.dash .terminal-card #ro{display:none}
  body.pg-dashboard h1{display:none}
  body.pg-dashboard header.page-head{display:none}
  body.pg-dashboard.token-open header.page-head{display:flex;min-height:34px;margin:4px 0 5px}
  body.pg-dashboard .token-box{width:170px}
  body.pg-dashboard .hint{margin-top:3px;line-height:1.3}
  .toast{max-width:none;left:10px;right:10px;top:46%;transform:translateY(-50%)}
  body.pg-dashboard main{padding:5px 6px 0}
  body.pg-dashboard .nav-toggle-global{display:none}
  body.pg-dashboard .nav-menu-btn{display:inline-flex;align-items:center;min-height:30px;padding:3px 10px;
    border:1px solid rgba(255,255,255,.28);background:rgba(255,255,255,.12);color:#fff;border-radius:8px;font-size:13px;flex:0 0 auto}
  body.pg-dashboard nav{display:none}
  body.pg-dashboard.nav-open nav{display:grid;grid-template-columns:1fr 1fr;gap:3px;position:absolute;left:8px;right:8px;top:100%;
    padding:8px;background:#0b3b3e;border:1px solid rgba(255,255,255,.18);border-top:0;border-radius:0 0 12px 12px;
    box-shadow:0 16px 32px rgba(2,18,22,.4);z-index:90;max-height:66vh;overflow:auto}
  body.pg-dashboard.nav-open nav a{background:rgba(255,255,255,.06)}
  body.pg-dashboard .terminal-card{height:calc(100vh - 65px);padding:5px 6px}
  body.pg-dashboard.token-open .terminal-card{height:calc(100vh - 136px)}
  body.pg-dashboard.nav-hidden .terminal-card{height:calc(100vh - 8px)}
  body.pg-dashboard .terminal-card .terminal-meta .chip{display:none}
  body.pg-dashboard .terminal-bar{flex-wrap:nowrap;overflow-x:auto;scrollbar-width:thin}
  body.pg-dashboard .terminal-bar .pane-strip{flex:0 0 auto;flex-wrap:nowrap;white-space:nowrap}
  body.pg-dashboard .terminal-bar .terminal-tools{flex:0 0 auto}
  body.pg-dashboard .terminal-card #keyHelp{display:none}
  .pane-strip{flex-wrap:nowrap;overflow-x:auto;margin-top:3px;scrollbar-width:thin}
  #rxBar{display:inline-flex;flex:0 0 auto}
  .terminal-bar{flex-wrap:wrap;margin:3px 0}
  .terminal-bar .terminal-tools{flex-basis:100%;justify-content:flex-start}
  .terminal-tools{flex-wrap:nowrap;overflow-x:auto;margin:5px 0;scrollbar-width:thin}
  .terminal-tools input{flex:1 1 105px;min-width:95px}
  .terminal-tools .btn{flex:0 0 auto}
  .terminal-card{min-height:0;height:calc(100vh - 190px);padding:12px}
  .terminal-shell .terminal{min-height:0;max-height:none;padding:10px 11px}
  .terminal-keybar{display:flex;padding:3px 5px;-webkit-overflow-scrolling:touch;scroll-snap-type:x proximity}
  .terminal-keybar button{flex:0 0 auto;padding:2px 6px;min-height:24px;scroll-snap-align:start}
  .channel-card{height:auto;min-height:0;max-height:42vh}
  .channel-list{grid-template-columns:1fr}
  .channel-cards,.agent-grid{grid-template-columns:1fr}
  .svc-grid{grid-template-columns:repeat(2,minmax(0,1fr))}
  .status-grid{grid-template-columns:1fr 1fr}
  .status-cell.wide{grid-column:1/-1}
  .file-row{gap:7px;padding:8px}
  .file-actions{max-width:45%}
  .terminal-foot{flex-wrap:wrap}
  .terminal-foot input{flex:1 1 100%}
  .footer-note{text-align:left}
}
/* 桌面工作台布局：左侧通道栏常驻，右侧会话区吃满剩余空间。
   顶部导航压到 44px，页面标题不再占行；token 改为“令牌”浮层。 */
@media(min-width:1021px){
  body.pg-dashboard .layout{display:flex;flex-direction:column;height:100vh;height:100dvh;min-height:0}
  body.pg-dashboard aside.sidebar{flex:0 0 auto;min-height:44px;padding:3px 10px;gap:8px}
  body.pg-dashboard .brand{padding:0;gap:7px}
  body.pg-dashboard .brand-mark{width:24px;height:24px;border-radius:7px;font-size:11px}
  body.pg-dashboard .brand strong{font-size:14px}
  body.pg-dashboard .brand small{font-size:10px}
  body.pg-dashboard nav{gap:2px;padding:0;align-items:center}
  body.pg-dashboard nav a{padding:4px 7px;font-size:12px;border-radius:7px;line-height:1.25}
  body.pg-dashboard #tokenMenuBtn{display:inline-flex;align-items:center;min-height:26px;padding:2px 8px;
    border:1px solid rgba(255,255,255,.26);background:rgba(255,255,255,.10);color:#fff;border-radius:7px;font-size:12px}
  body.pg-dashboard main{display:flex;flex-direction:column;flex:1 1 auto;min-height:0;padding:6px 8px 8px}
  body.pg-dashboard .shell{display:flex;flex-direction:column;flex:1 1 auto;min-height:0;width:100%;max-width:none;margin:0}
  body.pg-dashboard header.page-head{display:none}
  body.pg-dashboard.token-open header.page-head{display:flex;position:fixed;z-index:95;top:48px;right:8px;
    width:min(430px,calc(100vw - 16px));margin:0;padding:8px 9px;background:var(--surface);
    border:1px solid var(--line);border-radius:10px;box-shadow:0 14px 32px rgba(2,18,22,.22)}
  body.pg-dashboard.token-open header.page-head .title-wrap,
  body.pg-dashboard.token-open header.page-head .nav-toggle-global{display:none}
  body.pg-dashboard.token-open header.page-head .header-tools{width:100%;margin:0;justify-content:flex-end}
  body.pg-dashboard .dash{flex:1 1 auto;height:auto;min-height:0;grid-template-columns:196px minmax(0,1fr);gap:8px}
  body.pg-dashboard .dash.nav-collapsed{grid-template-columns:0 minmax(0,1fr);gap:0}
  body.pg-dashboard .channel-card{height:auto;min-height:0;margin:0;padding:8px 8px 7px;border-radius:10px}
  body.pg-dashboard .channel-card .card-head{align-items:center;margin-bottom:5px}
  body.pg-dashboard .channel-card h2{font-size:14px;line-height:1.2}
  body.pg-dashboard .channel-card .card-sub{display:none}
  body.pg-dashboard .channel-card #onlineCount{font-size:10px;padding:1px 6px}
  body.pg-dashboard .channel-card .channel-list{flex:1 1 auto;min-height:0;align-content:start;grid-auto-rows:max-content}
  body.pg-dashboard .channel-list{gap:3px}
  body.pg-dashboard .channel-item{flex-wrap:nowrap;gap:7px;padding:6px;border-radius:8px}
  body.pg-dashboard .channel-item .ch-name{font-size:12px;line-height:1.25}
  body.pg-dashboard .channel-item .ch-detail{font-size:10px;line-height:1.25;margin-top:1px}
  body.pg-dashboard .channel-item .ch-num{display:none}
  body.pg-dashboard .terminal-card{height:auto;min-height:0;margin:0;padding:3px 4px 4px;border-radius:10px}
  body.pg-dashboard .terminal-card .card-head{min-height:22px;margin:0 0 1px;align-items:center;gap:6px}
  body.pg-dashboard .terminal-card .card-title h2,
  body.pg-dashboard .terminal-card .card-title .card-sub{display:none}
  body.pg-dashboard .terminal-card .terminal-meta{margin:0;gap:4px}
  body.pg-dashboard .terminal-card .terminal-meta .chip{padding:1px 5px;font-size:11px;line-height:1.3}
  body.pg-dashboard .terminal-card .row-end{gap:3px}
  body.pg-dashboard .terminal-card .row-end .btn,
  body.pg-dashboard .terminal-card .row-end .nav-toggle{padding:2px 6px;min-height:22px;font-size:11px;line-height:1.2}
  body.pg-dashboard .terminal-card #ch{width:104px;min-height:22px;padding:1px 4px;font-size:11px}
  body.pg-dashboard .terminal-card #ro{display:none}
  body.pg-dashboard .terminal-bar{gap:5px;margin:0 0 2px;min-height:22px}
  body.pg-dashboard .terminal-bar .pane-strip{gap:3px}
  body.pg-dashboard .pane-strip .pane-label{font-size:11px}
  body.pg-dashboard .pane-chip{padding:2px 5px;min-height:20px;font-size:11px;border-radius:6px}
  body.pg-dashboard .terminal-tools{gap:3px}
  body.pg-dashboard .terminal-tools .btn{padding:2px 5px;min-height:21px;font-size:11px;line-height:1.2;border-radius:6px}
  body.pg-dashboard #termFind{flex:1 1 110px;min-height:22px;padding:1px 5px;font-size:11px}
  body.pg-dashboard .hint{display:none}
  body.pg-dashboard .lock-banner{margin:0 0 2px;padding:3px 7px;font-size:10px}
  body.pg-dashboard .terminal-shell{border-radius:8px}
  body.pg-dashboard .terminal-shell .terminal{padding:7px 9px}
  body.pg-dashboard .xterm-host{padding:3px 5px 2px}
}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
)CSS";

extern const char* const kCommonJs = R"JS(
const TOKEN_KEY='sb3_web_token';
const $=s=>document.querySelector(s);
const $$=s=>Array.from(document.querySelectorAll(s));
function tok(){return localStorage.getItem(TOKEN_KEY)||''}
function setTok(v){localStorage.setItem(TOKEN_KEY,v||'');const e=$('#tokenBox');if(e)e.value=v||'';syncTokenUI()}
function syncTokenUI(){
  const on=!!tok(), chip=$('#tokenState'), side=$('#sideState');
  if(chip){chip.textContent=on?'可写 · 已带 token':'只读 · 未设 token';chip.className='chip '+(on?'on':'off')}
  if(side){side.textContent=on?'Bearer token 已加载':'只读模式 · 无 token'}
  const dot=$('#sideDot');if(dot)dot.className='status-dot '+(on?'on':'warn');
}
function hdr(extra){const h=Object.assign({},extra||{});if(tok())h.Authorization='Bearer '+tok();return h}
async function api(path,opt){
  opt=opt||{};
  try{
    const r=await fetch(path,{...opt,headers:hdr(opt.headers||{})});
    const t=await r.text();let j=null;
    try{j=JSON.parse(t)}catch(e){}
    return {r,t,j,status:r.status};
  }catch(e){return {r:{ok:false},t:String(e),j:null,status:0}}
}
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function bytesText(buf){try{return new TextDecoder('utf-8').decode(buf)}catch(e){return ''}}
function fmtBytes(n){n=Number(n||0);if(n<1024)return n+' B';if(n<1024*1024)return (n/1024).toFixed(1)+' KB';if(n<1024*1024*1024)return (n/1048576).toFixed(1)+' MB';return (n/1073741824).toFixed(1)+' GB'}
function updateRxBadge(ch){const b=$('#rxBadge'),b2=$('#rxBar');if(!b&&!b2)return;const c=LAST&&LAST.channels?LAST.channels.find(x=>x.name===ch):null;const txt='RX '+fmtBytes(c?c.rx_bytes:0);const tip='当前通道累计接收字节'+(c?' · TX '+fmtBytes(c.tx_bytes):'');[b,b2].forEach(x=>{if(!x)return;x.textContent=txt;x.title=tip})}
function isOnline(c){return !!c&&(c.state==='ready'||c.state==='online'||c.state==='connected')}
function toast(msg,kind){
  let box=$('#toast');if(!box){box=document.createElement('div');box.id='toast';box.className='toast';box.setAttribute('aria-live','polite');document.body.appendChild(box)}
  const d=document.createElement('div');d.className='toast-item '+(kind||'ok');d.textContent=String(msg);box.appendChild(d);
  setTimeout(()=>{d.style.opacity='0';d.style.transform='translateY(4px)';setTimeout(()=>d.remove(),200)},3200);
}
function askConfirm(title,message,okText,danger){
  return new Promise(resolve=>{
    const d=$('#confirmDlg');if(!d||!d.showModal){resolve(confirm(title+'\n\n'+message));return}
    $('#confirmTitle').textContent=title;$('#confirmText').textContent=message;
    const b=$('#confirmOk');b.textContent=okText||'确认';b.className='btn '+(danger===false?'':'danger');
    d.returnValue='cancel';d.showModal();
    d.onclose=()=>resolve(d.returnValue==='ok');
  });
}
function askText(title,label,value,placeholder,okText){
  return new Promise(resolve=>{
    const d=$('#promptDlg');if(!d||!d.showModal){const v=prompt(label,value||'');resolve(v==null?null:v);return}
    $('#promptTitle').textContent=title;$('#promptLabel').textContent=label;
    const inp=$('#promptInput');inp.value=value||'';inp.placeholder=placeholder||'';
    const b=$('#promptOk');b.textContent=okText||'确定';
    d.returnValue='cancel';d.showModal();
    setTimeout(()=>inp.focus(),30);
    d.onclose=()=>resolve(d.returnValue==='ok'?inp.value.trim():null);
  });
}
function qs(o){const p=new URLSearchParams();Object.keys(o||{}).forEach(k=>{if(o[k]!==undefined&&o[k]!==null)p.set(k,String(o[k]))});return p.toString()}
function pagePath(){return location.pathname.replace(/\/+$/,'')||'/dashboard'}
function setBusy(btn,busy,label){
  if(!btn)return; if(busy){btn.dataset.old=btn.textContent;btn.disabled=true;btn.textContent=label||'处理中…'}
  else{btn.disabled=false;if(btn.dataset.old)btn.textContent=btn.dataset.old;delete btn.dataset.old}
}
window.addEventListener('DOMContentLoaded',()=>{
  const e=$('#tokenBox');if(e){e.value=tok();e.onchange=()=>setTok(e.value.trim())}
  const sv=$('#saveToken');
  if(sv)sv.onclick=()=>{setTok(($('#tokenBox')||{}).value.trim());toast('Token 已保存，正在刷新页面');setTimeout(()=>location.reload(),450)};
  const tm=$('#tokenMenuBtn');
  if(tm)tm.onclick=ev=>{ev.stopPropagation();document.body.classList.toggle('token-open');tm.setAttribute('aria-expanded',document.body.classList.contains('token-open')?'true':'false')};
  if(!tok()&&document.body.classList.contains('pg-dashboard')&&matchMedia('(max-width:760px)').matches)document.body.classList.add('token-open');
  syncTokenUI();
});
)JS";

static const char* kFileBrowserHtml = R"HTML(
<section class="card" id="fileCard">
  <div class="card-head">
    <div class="card-title"><div><h2>共享文件</h2><div class="card-sub">TFTP / NFS / HTTP 共享使用同一根目录；上传和删除需要 token。</div></div></div>
    <div class="file-toolbar">
      <button class="btn ghost" id="fileRefresh" type="button">刷新</button>
      <button class="btn ghost" id="fileMkdir" type="button">新建目录</button>
      <label class="btn file-upload" title="上传文件">上传<input id="fileUpload" type="file" multiple></label>
    </div>
  </div>
  <div class="breadcrumb" id="fileCrumb"></div>
  <div class="file-list" id="fileList"></div>
  <div class="form-status" id="fileNote"></div>
</section>
)HTML";

static const char* kFileBrowserJs = R"JS(
let FILE_PATH='';
function fileEnc(p){return encodeURIComponent(p||'')}
function fileParent(p){const parts=(p||'').split('/').filter(Boolean);parts.pop();return parts.join('/')}
function fileCrumb(path){
  const box=$('#fileCrumb');if(!box)return;box.innerHTML='';
  const root=document.createElement('button');root.type='button';root.textContent='共享根';root.onclick=()=>fileLoad('');box.appendChild(root);
  (path||'').split('/').filter(Boolean).forEach((part,i,arr)=>{
    const sep=document.createElement('span');sep.textContent='/';box.appendChild(sep);
    const b=document.createElement('button');b.type='button';b.textContent=part;
    b.onclick=()=>fileLoad(arr.slice(0,i+1).join('/'));box.appendChild(b);
  });
}
function fileRow(e){
  const row=document.createElement('div');row.className='file-row';
  const ic=document.createElement('div');ic.className='file-icon';ic.textContent=e.dir?'▸':'·';row.appendChild(ic);
  const main=document.createElement('div');main.className='file-main';
  const name=document.createElement('span');name.className='file-name'+(e.dir?' dir':'');name.textContent=e.name;main.appendChild(name);
  const meta=document.createElement('span');meta.className='file-meta';meta.textContent=(e.dir?'目录':fmtBytes(e.size))+(e.mtime?' · '+e.mtime:'');main.appendChild(meta);
  row.appendChild(main);
  const act=document.createElement('div');act.className='file-actions';
  if(e.dir){
    const open=document.createElement('button');open.type='button';open.className='btn ghost small';open.textContent='打开';open.onclick=()=>fileLoad(e.path);act.appendChild(open);
  }else{
    const a=document.createElement('a');a.href='/api/files/download?path='+fileEnc(e.path);a.textContent='下载';a.download=e.name;act.appendChild(a);
  }
  const del=document.createElement('button');del.type='button';del.className='btn danger small';del.textContent='删除';del.onclick=async()=>{
    const ok=await askConfirm('删除'+(e.dir?'目录':'文件'),'确定删除“'+e.name+'”吗？此操作不可撤销。','删除',true);
    if(!ok)return;
    const r=await api('/api/files/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:e.path})});
    if(!r.r.ok){toast('删除失败：'+(r.j&&r.j.error||r.t),'error');return}
    toast('已删除 '+e.name);fileLoad(FILE_PATH);
  };act.appendChild(del);row.appendChild(act);
  return row;
}
async function fileLoad(path){
  if(!$('#fileList'))return;
  FILE_PATH=path||'';fileCrumb(FILE_PATH);
  const box=$('#fileList');const note=$('#fileNote');box.innerHTML='<div class="empty"><div class="empty-mark">…</div><strong>正在读取</strong><p>共享目录</p></div>';
  const r=await api('/api/files?'+qs({path:FILE_PATH}));
  if(!r.r.ok||!r.j){box.innerHTML='<div class="empty"><div class="empty-mark">!</div><strong>读取失败</strong><p>'+esc(r.j&&r.j.error||r.t)+'</p></div>';return}
  box.innerHTML='';
  const items=r.j.items||[];
  if(!items.length){box.innerHTML='<div class="empty"><div class="empty-mark">+</div><strong>这个目录还是空的</strong><p>上传一个文件，或新建子目录。</p></div>'}
  else items.forEach(e=>box.appendChild(fileRow(e)));
  note.textContent=items.length+' 项 · 根目录 '+r.j.root;
}
async function fileUpload(files){
  if(!files||!files.length)return;
  if(!tok()){toast('请先保存 Bearer token 再上传','error');return}
  for(const f of Array.from(files)){
    const r=await api('/api/files/upload?'+qs({path:FILE_PATH,name:f.name}),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});
    if(!r.r.ok){toast('上传失败：'+f.name+' · '+(r.j&&r.j.error||r.t),'error')}
  }
  toast('上传完成');$('#fileUpload').value='';fileLoad(FILE_PATH);
}
function fileInit(){
  if(!$('#fileList'))return;
  const rf=$('#fileRefresh');if(rf)rf.onclick=()=>fileLoad(FILE_PATH);
  const up=$('#fileUpload');if(up)up.onchange=()=>fileUpload(up.files);
  const mk=$('#fileMkdir');if(mk)mk.onclick=async()=>{
    const name=await askText('新建目录','目录名称','','例如 logs / boards','创建');
    if(!name)return;
    const r=await api('/api/files/mkdir',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:FILE_PATH,name})});
    if(!r.r.ok){toast('创建失败：'+(r.j&&r.j.error||r.t),'error');return}
    toast('目录已创建');fileLoad(FILE_PATH);
  };
  fileLoad('');
}
)JS";

std::string page_body(const std::string& page) {
    if (page == "dashboard") {
        return R"HTML(
<div class="dash" id="dash">
  <aside class="card channel-card" id="channelCard">
    <div class="card-head"><div class="card-title"><div><h2>通道</h2><div class="card-sub">点击切换终端</div></div></div><span class="chip" id="onlineCount">--</span></div>
    <div class="channel-list" id="channelList"></div>
  </aside>
  <section class="card terminal-card">
    <div class="card-head">
      <div class="card-title"><div><h2>终端</h2><div class="terminal-meta"><span class="chip accent" id="activeName">未选择</span><span class="chip" id="wsState">WS 未连接</span><span class="chip" id="rxBadge" title="当前通道累计接收字节">RX --</span><span class="chip" id="termLock">无 Agent 锁</span></div></div></div>
      <div class="row-end"><button class="nav-toggle" id="toggleNav" type="button" title="显示/隐藏左侧通道面板">通道面板</button><select id="ch" aria-label="选择通道"></select><button class="btn ghost" id="reload" type="button">刷新</button><button class="btn ghost" id="keyHelp" type="button" aria-keyshortcuts="F5 Control+F F3 Control+Shift+B" title="F5 刷新 · Ctrl+F 查找 · F3 下一个 · Ctrl+Shift+B 导航">快捷键</button><span class="pill" id="ro">只读</span></div>
    </div>
    <div class="terminal-bar">
      <div class="pane-strip" id="paneStrip"><span class="pane-label">分屏</span><span class="form-status">正在读取画框…</span></div>
      <span class="chip" id="rxBar" title="当前通道累计接收字节">RX --</span>
      <div class="terminal-tools">
        <input id="termFind" placeholder="查找回显…" aria-label="查找回显">
        <button class="btn ghost" id="findNext" type="button" title="F3 / Enter 定位下一个命中">下一个</button>
        <button class="btn ghost" id="copyTerm" type="button" title="Ctrl+Shift+C 复制选区">复制</button>
        <a class="btn ghost" id="dlTerm" href="#" download>下载日志</a>
        <button class="btn ghost" id="fontMinus" type="button" title="减小字号">A−</button>
        <button class="btn ghost" id="fontPlus" type="button" title="增大字号">A+</button>
        <button class="btn ghost" id="fullTerm" type="button" title="全屏终端">全屏</button>
        <button class="btn ghost" id="clearBuf" type="button">清空缓冲</button>
        <button class="btn ghost" id="clearView" type="button" title="仅清空当前网页显示">清屏</button>
      </div>
    </div>
    <div class="lock-banner" id="lockBanner" hidden><span id="lockText"></span><button class="btn" id="takeLock" type="button">人工接管</button></div>
    <div class="terminal-shell">
      <div class="xterm-host" id="xtermHost" hidden></div>
      <pre class="terminal" id="term" aria-live="polite"></pre>
      <div class="terminal-keybar" id="keyBar" role="toolbar" aria-label="移动端特殊键（桌面直接键盘输入）">
        <button type="button" data-key="esc" title="ESC">Esc</button>
        <button type="button" data-key="tab" title="Tab">Tab</button>
        <button type="button" data-key="ctrl-c" title="中断当前命令">Ctrl+C</button>
        <button type="button" data-key="up" title="上箭头">↑</button>
        <button type="button" data-key="down" title="下箭头">↓</button>
        <button type="button" data-key="left" title="左箭头">←</button>
        <button type="button" data-key="right" title="右箭头">→</button>
        <button type="button" data-key="home" title="Home">Home</button>
        <button type="button" data-key="end" title="End">End</button>
        <button type="button" data-key="pgup" title="Page Up">PgUp</button>
        <button type="button" data-key="pgdn" title="Page Down">PgDn</button>
        <button type="button" data-key="ctrl-d" title="EOF / 退出">Ctrl+D</button>
        <button type="button" data-key="ctrl-z" title="挂起">Ctrl+Z</button>
        <button type="button" data-key="ctrl-l" title="清屏">Ctrl+L</button>
        <button type="button" data-key="ctrl-u" title="删除整行">Ctrl+U</button>
        <button type="button" data-key="ctrl-r" title="历史搜索">Ctrl+R</button>
      </div>
    </div>
    <div class="hint" id="hint"></div>
  </section>
</div>
<section class="card" id="svcOverviewCard">
  <div class="card-head"><div class="card-title"><div><h2>服务状态</h2><div class="card-sub">服务开关在对应页面操作；这里只做总览。</div></div></div></div>
  <div class="svc-grid" id="svcBadges"></div>
</section>
)HTML";
    }
    if (page == "channels") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>通道</h2><div class="card-sub">串口、SSH、PowerShell 与 Telnet 通道的新增、编辑、改名、删除；保存后由 UI 线程重载。</div></div></div>
    <div class="row-end"><button class="btn ghost" id="loadChannels" type="button">刷新</button><button class="btn" id="newSerial" type="button">新增串口</button><button class="btn" id="newSsh" type="button">新增 SSH</button><button class="btn" id="newPowershell" type="button">新增 PowerShell</button><button class="btn" id="newTelnet" type="button">新增 Telnet</button></div>
  </div>
  <div class="row-end" style="margin-bottom:11px"><span class="chip" id="chanCount">--</span><span class="chip" id="chanOnline">--</span><span class="form-status" id="chanNote">写接口需要 token。</span></div>
  <div class="channel-cards" id="chanCards"></div>
</section>
<section class="card editor" id="chanEditor" hidden>
  <div class="card-head">
    <div class="card-title"><div><h2 id="chanEditorTitle">编辑通道</h2><div class="card-sub" id="chanEditorSub">串口和 SSH 字段会按类型切换。</div></div></div>
    <span class="chip" id="chanKindChip">串口</span>
  </div>
  <div class="form-grid">
    <label class="field"><span>通道名</span><input id="cName" maxlength="40" placeholder="例如 ok3568-com5"><span class="help">≤40 字符，不能含 |，不能与现有通道重名。</span></label>
    <label class="field"><span>类型</span><input id="cKind" readonly></label>
    <label class="field"><span>串口设备</span><input id="cDevice" placeholder="COM5 或 loop:ai"></label>
    <label class="field"><span>波特率</span><input id="cBaud" type="number" min="50" max="4000000" step="1" value="115200"></label>
    <label class="field"><span>数据位</span><select id="cDataBits"><option>5</option><option>6</option><option>7</option><option selected>8</option></select></label>
    <label class="field"><span>校验</span><select id="cParity"><option value="N">N · 无</option><option value="E">E · 偶</option><option value="O">O · 奇</option><option value="M">M · Mark</option><option value="S">S · Space</option></select></label>
    <label class="field"><span>停止位</span><select id="cStopBits"><option value="10">1</option><option value="15">1.5</option><option value="20">2</option></select></label>
    <label class="field"><span>流控</span><select id="cFlow"><option value="none">none</option><option value="rtscts">rtscts</option><option value="xonxoff">xonxoff</option></select></label>
    <label class="field"><span>行尾</span><select id="cLineEnding"><option value="none">none</option><option value="lf">LF</option><option value="crlf">CRLF</option><option value="cr">CR</option></select></label>
    <label class="field"><span>编码</span><input id="cEncoding" placeholder="utf-8"></label>
    <label class="field"><span>用户名（可选）</span><input id="cUsername" placeholder="串口登录用户"></label>
    <label class="field"><span>密码（可选）</span><input id="cPassword" type="password" autocomplete="off"></label>
    <label class="field"><span>主机</span><input id="cHost" placeholder="192.168.1.10"></label>
    <label class="field"><span>SSH 端口</span><input id="cPort" type="number" min="1" max="65535" value="22"></label>
    <label class="field"><span>SSH 用户名</span><input id="cSshUser" placeholder="root"></label>
    <label class="field"><span>SSH 密码</span><input id="cSshPassword" type="password" autocomplete="off"></label>
    <label class="field"><span>PowerShell 程序</span><input id="cShell" placeholder="powershell.exe"></label>
    <label class="field"><span>PowerShell 参数</span><input id="cArgs" placeholder="-NoLogo -NoProfile -ExecutionPolicy Bypass"></label>
    <label class="field"><span>工作目录（可选）</span><input id="cCwd" placeholder="留空用进程当前目录"></label>
    <label class="field"><span>TERM</span><input id="cTerm" placeholder="xterm-256color"></label>
    <label class="field"><span>Keepalive 秒</span><input id="cKeepalive" type="number" min="0" max="3600" value="3"></label>
    <label class="field"><span>终端列</span><input id="cWidth" type="number" min="40" max="500" value="120"></label>
    <label class="field"><span>终端行</span><input id="cHeight" type="number" min="10" max="200" value="32"></label>
    <label class="field"><span>连接超时 ms</span><input id="cTimeout" type="number" min="100" max="120000" step="100" value="5000"></label>
    <label class="check-row"><input id="cAutoOpen" type="checkbox"> 自动打开</label>
    <label class="check-row"><input id="cTimestamp" type="checkbox"> 回显时间戳</label>
    <label class="check-row"><input id="cDtr" type="checkbox" checked> DTR</label>
    <label class="check-row"><input id="cRts" type="checkbox" checked> RTS</label>
    <label class="check-row"><input id="cDiscoverable" type="checkbox"> 允许局域网发现</label>
    <label class="check-row"><input id="cLogEnabled" type="checkbox" checked> 写入日志</label>
  </div>
  <div class="editor-actions">
    <button class="btn" id="saveChannel" type="button">保存通道</button>
    <button class="btn ghost" id="cancelChannel" type="button">取消</button>
    <span class="form-status" id="chanEditNote"></span>
  </div>
</section>
)HTML";
    }
    if (page == "agent") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>18081 设备控制面</h2><div class="card-sub">Agent 唯一板控主路径：scan → probe → lock → command/action → log → unlock。此页只读展示，不直接代发板控命令。</div></div></div>
    <span class="chip" id="deviceChip">读取中</span>
  </div>
  <div class="agent-grid" id="deviceMetrics">
    <div class="agent-card"><div class="k">通道 / 在线</div><div class="v" id="devChannels">--</div></div>
    <div class="agent-card"><div class="k">作业（运行 / 总数）</div><div class="v" id="devJobs">--</div></div>
    <div class="agent-card"><div class="k">租约锁</div><div class="v" id="devLocks">--</div></div>
    <div class="agent-card"><div class="k">TFTP 工件面</div><div class="v" id="devTftp">--</div></div>
    <div class="agent-card"><div class="k">已公布操作</div><div class="v" id="devOps">--</div></div>
    <div class="agent-card"><div class="k">后端边界</div><div class="v" id="devBoundary">18081 only</div></div>
  </div>
  <div class="editor-actions">
    <button class="btn" id="devRefresh" type="button">刷新设备面</button>
    <a class="btn ghost" href="/api/help" target="_blank">机器契约 /api/help</a>
    <a class="btn ghost" href="/help" target="_blank">闭环帮助 /help</a>
    <span class="form-status" id="devNote"></span>
  </div>
  <div class="card-head" style="margin-top:14px"><div class="card-title"><div><h2>通道与终端态</h2><div class="card-sub">通道、运行状态、终端探测与当前持锁 owner。</div></div></div></div>
  <div id="deviceChannels"><div class="empty"><strong>正在读取</strong><p>GET /agent/scan</p></div></div>
  <div class="card-head" style="margin-top:14px"><div class="card-title"><div><h2>租约锁</h2><div class="card-sub">写路由被 Agent 锁住时返回 423；人工接管在仪表盘终端里执行。</div></div></div></div>
  <div id="deviceLocks"><div class="empty"><strong>无锁</strong><p>当前没有 Agent 持有通道。</p></div></div>
</section>
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>18096 桌面 UI 桥（状态只读）</h2><div class="card-sub">18081 不调用、不代理 18096；这里只显示 /api/agent/status 返回的启用状态与端口，窗口自检在桌面端完成。</div></div></div>
    <span class="chip" id="agentChip">读取中</span>
  </div>
  <div class="agent-grid" id="agentMetrics">
    <div class="agent-card"><div class="k">启用</div><div class="v" id="agEnabled">--</div></div>
    <div class="agent-card"><div class="k">端口</div><div class="v" id="agPort">--</div></div>
    <div class="agent-card"><div class="k">鉴权</div><div class="v" id="agToken">--</div></div>
    <div class="agent-card"><div class="k">边界</div><div class="v" id="agBoundary">--</div></div>
  </div>
  <div class="form-status" id="agNote"></div>
</section>
<section class="card">
  <div class="card-head"><div class="card-title"><div><h2>18081 接口速查</h2><div class="card-sub">优先从 /api/help 动态读取；下方只列闭环主路径。写操作需要 Bearer token，通道写还要过租约锁。</div></div></div><a class="btn ghost" href="/help" target="_blank">文本帮助</a></div>
  <div class="help-list" id="deviceHelp"></div>
</section>
<section class="card">
  <div class="card-head"><div class="card-title"><div><h2>18096 边界</h2><div class="card-sub">18081 不代理桌面窗口；下列仅说明边界，不是 Agent 可调用接口。</div></div></div></div>
  <div class="help-list" id="agentHelp"></div>
</section>
)HTML";
    }
    if (page == "config") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>配置表单</h2><div class="card-sub">基础字段和完整叶子字段；密钥默认掩码，保存后由 UI 线程重载。</div></div></div>
    <div class="tabs"><button id="tabBasic" class="on" type="button">基础</button><button id="tabDetail" type="button">详细</button></div>
  </div>
  <div class="toolbar">
    <input class="search" id="cfgSearch" placeholder="搜索字段名…" aria-label="搜索配置字段">
    <button class="btn ghost" id="loadCfg" type="button">读取配置</button>
    <button class="btn" id="saveCfg" type="button">保存并重载</button>
    <span class="spacer"></span>
    <span class="form-status" id="cfgNote">写接口需要 token。</span>
  </div>
  <div id="cfgForm" class="cfg-form" style="margin-top:14px"></div>
</section>
)HTML";
    }
    if (page == "actions") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>板控按钮</h2><div class="card-sub">按通道编辑 commands / relay / script 按钮，执行结果异步返回。</div></div></div>
    <div class="row-end"><select id="actFilter" aria-label="筛选通道"><option value="">全部通道</option></select><button class="btn" id="newAction" type="button">新增按钮</button></div>
  </div>
  <div class="editor" id="actionEditor" hidden>
    <div class="form-grid">
      <label class="field"><span>所属通道</span><select id="aChannel"></select></label>
      <label class="field"><span>按钮 ID</span><input id="aId" placeholder="例如 board-reset" autocomplete="off"><span class="help">通道内唯一，建议短横线命名。</span></label>
      <label class="field"><span>按钮文字</span><input id="aLabel" placeholder="例如 重启板卡"></label>
      <label class="field"><span>类型</span><select id="aBackend"><option value="commands">commands · 命令行</option><option value="relay">relay · 继电器</option><option value="script">script · 脚本</option></select></label>
      <label class="field"><span>执行位置</span><select id="aRun"><option value="channel">channel · 当前通道</option><option value="host">host · 本机执行</option></select></label>
      <label class="field"><span>启用</span><select id="aEnable"><option value="true">启用</option><option value="false">停用</option></select></label>
      <label class="field full only-commands"><span>命令（一行一条）</span><textarea id="aCommands" placeholder="help&#10;status"></textarea><span class="help">按顺序发送到通道，每条之间等待下面间隔。</span></label>
      <label class="field only-commands"><span>每条间隔 ms</span><input id="aDelay" type="number" min="0" step="50" value="200"></label>
      <label class="field only-relay"><span>继电器设备</span><select id="aRelay"></select></label>
      <label class="field only-relay"><span>继电器通道</span><input id="aRelayCh" type="number" min="1" max="16" value="1"></label>
      <label class="field only-relay"><span>模式</span><select id="aMode"><option value="pulse">pulse · 脉冲</option><option value="hold_on">hold_on · 保持吸合</option><option value="hold_off">hold_off · 保持断开</option></select></label>
      <label class="field only-relay"><span>脉冲/保持秒</span><input id="aHold" type="number" min="0" step="0.1" value="1"></label>
      <label class="field full only-script"><span>脚本正文</span><textarea id="aScript" style="min-height:160px" placeholder="-- Lua 或旧 DSL"></textarea><span class="help">script 模式的测试会走真实执行器。</span></label>
    </div>
    <div class="editor-actions"><button class="btn" id="saveAction" type="button">保存按钮</button><button class="btn ghost" id="cancelAction" type="button">取消</button><span class="form-status" id="actionNote"></span></div>
  </div>
  <div id="actList" class="list"></div>
</section>
<details class="card">
  <summary style="cursor:pointer;font-weight:700">Lua / DSL 速查（v3）</summary>
  <div class="help-list" style="margin-top:12px">
    <div class="help-item"><code>channel.mark()</code><div class="card-sub">记录日志游标</div></div>
    <div class="help-item"><code>channel.write(s)</code><div class="card-sub">原样写入，换行自己写 \r\n</div></div>
    <div class="help-item"><code>channel.send("&lt;ctrl+c&gt;")</code><div class="card-sub">单键/字符</div></div>
    <div class="help-item"><code>channel.wait_mark(m, "login:", 30)</code><div class="card-sub">等回显关键字，超时返回 nil</div></div>
    <div class="help-item"><code>channel.exec(cmd, timeout)</code><div class="card-sub">发命令并等空闲</div></div>
    <div class="help-item"><code>relay_mode("relay1", 1, "pulse", 0.5)</code><div class="card-sub">hold_on / hold_off / pulse</div></div>
    <div class="help-item"><code>host_exec(cmd, timeout_ms)</code><div class="card-sub">本机执行，返回 ok/exit_code/output</div></div>
    <div class="help-item"><code>action("ID")</code><div class="card-sub">调用同通道按钮，嵌套上限 6</div></div>
  </div>
  <div class="hint">整段脚本约 180 秒上限；send_keys 和 msleep 单次 ≤120 秒。简单脚本一行一条：write / send / keys / login / relay / msleep / remotecmd / hostcmd / expect / action。完整说明见 docs/脚本参考-S12.md。</div>
</details>
)HTML";
    }
    if (page == "button-script") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>快捷指令</h2><div class="card-sub">底部快捷条的数据源；发送只走当前通道 ring，不新建连接。</div></div></div>
    <div class="row-end"><select id="quickChannel" aria-label="快捷指令通道"></select><button class="btn" id="newQuick" type="button">新增指令</button></div>
  </div>
  <div class="editor" id="quickEditor" hidden>
    <div class="form-grid">
      <label class="field"><span>按钮文字</span><input id="qLabel" placeholder="例如 查看版本" maxlength="40"></label>
      <label class="field"><span>命令</span><input id="qCommand" placeholder="例如 uname -a" autocomplete="off"></label>
    </div>
    <div class="editor-actions"><button class="btn" id="saveQuick" type="button">保存指令</button><button class="btn ghost" id="cancelQuick" type="button">取消</button><span class="form-status" id="quickNote"></span></div>
  </div>
  <div id="quickList" class="list"></div>
</section>
)HTML";
    }
    if (page == "relay") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>继电器设备</h2><div class="card-sub">设备配置与通道测试；没有真实硬件时，CRUD 和错误反馈仍可验收。</div></div></div>
    <div class="row-end"><button class="btn ghost" id="loadRelay" type="button">刷新</button><button class="btn" id="newRelay" type="button">新增设备</button></div>
  </div>
  <div class="editor" id="relayEditor" hidden>
    <div class="form-grid">
      <label class="field"><span>设备名称</span><input id="rName" placeholder="例如 relay1"></label>
      <label class="field"><span>Windows 端口</span><input id="rPort" placeholder="例如 COM3"></label>
      <label class="field"><span>通道数</span><input id="rChannels" type="number" min="1" max="16" value="2"></label>
      <label class="field"><span>波特率</span><input id="rBaud" type="number" min="1200" step="1" value="9600"></label>
      <label class="field"><span>使能</span><select id="rEnable"><option value="true">启用</option><option value="false">停用</option></select></label>
      <label class="field"><span>IO 反相</span><select id="rIoInv"><option value="false">正常</option><option value="true">反相</option></select></label>
      <label class="field"><span>极性强反</span><select id="rPolInv"><option value="false">正常</option><option value="true">反相</option></select></label>
      <label class="field"><span>写后延时 ms</span><input id="rDelay" type="number" min="0" step="10" value="100"></label>
    </div>
    <div class="editor-actions"><button class="btn" id="saveRelay" type="button">保存设备</button><button class="btn ghost" id="cancelRelay" type="button">取消</button><span class="form-status" id="relayNote"></span></div>
  </div>
  <div id="relayList" class="grid"></div>
</section>
)HTML";
    }
    if (page == "tftp" || page == "nfs" || page == "share") {
        std::string svc = page;
        std::string title = svc == "tftp" ? "TFTP 服务" : (svc == "nfs" ? "NFS 服务" : "共享目录");
        std::string desc = svc == "tftp" ? "TFTP 配置、启停、根目录状态，以及共享文件管理。"
                           : svc == "nfs" ? "NFS 别名/UID/GID 配置、导出状态与共享文件管理。"
                                          : "HTTP 共享端口/根目录配置、访问地址与共享文件管理。";
        std::string fields = R"HTML(
    <label class="field"><span>启用</span><select id="svcEnable"><option value="false">关闭</option><option value="true">开启</option></select></label>
    <label class="field"><span>根目录</span><input id="svcRoot" placeholder="留空使用默认共享根"><span class="help">相对路径按应用工作目录解析；保存后由服务重载。</span></label>
)HTML";
        if (svc == "tftp") fields += R"HTML(
    <label class="field"><span>端口</span><input id="svcPort" type="number" min="1" max="65535" value="69"></label>
    <label class="field"><span>绑定 IP</span><input id="svcBind" placeholder="留空监听全部网卡"></label>
    <label class="check-row"><input id="svcWrite" type="checkbox" checked> 允许客户端写入</label>
)HTML";
        else if (svc == "nfs") fields += R"HTML(
    <label class="field"><span>导出别名</span><input id="svcAlias" placeholder="/share"><span class="help">必须以 / 开头。</span></label>
    <label class="field"><span>UID</span><input id="svcUid" type="number" min="0" max="65535" value="0"></label>
    <label class="field"><span>GID</span><input id="svcGid" type="number" min="0" max="65535" value="0"></label>
)HTML";
        else fields += R"HTML(
    <label class="field"><span>HTTP 端口</span><input id="svcPort" type="number" min="1" max="65535" value="18082"></label>
)HTML";
        std::string html = R"HTML(
<section class="card" id="svcConfigCard">
  <div class="card-head">
    <div class="card-title"><div><h2>__TITLE__ 配置</h2><div class="card-sub">__DESC__</div></div></div>
    <span class="chip" id="svcCfgState">读取中</span>
  </div>
  <div class="form-grid">__FIELDS__</div>
  <div class="editor-actions"><button class="btn" id="saveSvcCfg" type="button">保存配置</button><span class="form-status" id="svcCfgNote"></span></div>
</section>
<section class="card" id="svcCard">
  <div class="card-head">
    <div class="card-title"><div><h2>__TITLE__ 运行状态</h2><div class="card-sub">服务开关也可单独启停；配置启用后由 apply_config 自动同步。</div></div></div>
    <div class="row-end"><button class="btn ghost" id="loadSvc" type="button">刷新</button><button class="btn ghost" id="startSvc" type="button">启动</button><button class="btn danger" id="stopSvc" type="button">停止</button></div>
  </div>
  <div class="service-summary" id="svcBox"></div>
  <div class="hint" id="svcHint"></div>
</section>
)HTML";
        std::size_t p = std::string::npos;
        while ((p = html.find("__TITLE__")) != std::string::npos) html.replace(p, 9, title);
        p = html.find("__DESC__");
        if (p != std::string::npos) html.replace(p, 8, desc);
        p = html.find("__FIELDS__");
        if (p != std::string::npos) html.replace(p, 10, fields);
        return html + std::string(kFileBrowserHtml);
    }
    if (page == "tcp-forward") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>TCP 转发</h2><div class="card-sub">板卡 IP 最后一段映射到本地端口，转发到目标端口；保存后由 UI 线程重载。</div></div></div>
    <span class="chip" id="tcpState">读取中</span>
  </div>
  <div class="form-grid">
    <label class="field"><span>启用</span><select id="tEnable"><option value="false">关闭</option><option value="true">开启</option></select></label>
    <label class="field"><span>监听 IP</span><input id="tListen" placeholder="0.0.0.0"></label>
    <label class="field"><span>端口基数</span><input id="tPortBase" type="number" min="1" max="65500" value="22000"></label>
    <label class="field"><span>目标端口</span><input id="tTarget" type="number" min="1" max="65535" value="22"></label>
    <label class="field full"><span>板卡 CIDR</span><input id="tCidr" placeholder="192.168.1.0/24"><span class="help">用于识别哪些来源算板卡；留空表示按配置执行器策略。</span></label>
  </div>
  <div class="editor-actions"><button class="btn" id="saveTcp" type="button">保存配置</button><button class="btn ghost" id="startTcp" type="button">启动</button><button class="btn danger" id="stopTcp" type="button">停止</button><span class="form-status" id="tcpNote"></span></div>
  <div class="status-grid" id="tcpStatus"></div>
</section>
)HTML";
    }
    if (page == "tunnel") {
        return R"HTML(
<section class="card">
  <div class="card-head">
    <div class="card-title"><div><h2>外网隧道</h2><div class="card-sub">外部 eadk-tunnel / frpc 子进程；HTTP 映射与入站 SSH 映射分别配置。</div></div></div>
    <span class="chip" id="tunState">读取中</span>
  </div>
  <div class="form-grid">
    <label class="field"><span>启用</span><select id="uEnable"><option value="false">关闭</option><option value="true">开启</option></select></label>
    <label class="field"><span>执行文件</span><input id="uExe" placeholder="留空 = 自动查找"></label>
    <label class="field"><span>服务器地址</span><input id="uServer" placeholder="tunnel.example.com"></label>
    <label class="field"><span>服务器端口</span><input id="uPort" type="number" min="1" max="65535" value="7000"></label>
    <label class="field"><span>Token</span><div class="secret-wrap"><input id="uToken" type="password" autocomplete="off"><button class="btn ghost" id="uTokenEye" type="button">显示</button></div></label>
    <label class="field"><span>HTTP 映射</span><select id="uHttp"><option value="true">启用</option><option value="false">关闭</option></select></label>
    <label class="field"><span>SSH 映射</span><select id="uSsh"><option value="false">关闭</option><option value="true">启用</option></select></label>
    <label class="field"><span>HTTP 本地端口</span><input id="uLocal" type="number" min="1" max="65535" value="18081"></label>
    <label class="field"><span>HTTP 远端端口</span><input id="uRemote" type="number" min="0" max="65535" value="0"><span class="help">0 = 18xxx 自动分配</span></label>
    <label class="field"><span>SSH 本地端口</span><input id="uSshLocal" type="number" min="1" max="65535" value="2222"></label>
    <label class="field"><span>SSH 远端端口</span><input id="uSshRemote" type="number" min="0" max="65535" value="0"><span class="help">0 = 28xxx 自动分配</span></label>
  </div>
  <div class="editor-actions"><button class="btn" id="saveTun" type="button">保存配置</button><button class="btn ghost" id="startTun" type="button">启动</button><button class="btn danger" id="stopTun" type="button">停止</button><span class="form-status" id="tunNote"></span></div>
  <div class="status-grid" id="tunStatus"></div>
</section>
)HTML";
    }
    return "<section class=\"card\"><h2>未知页面</h2><p class=\"muted\">请从左侧导航进入。</p></section>";
}

std::string page_script(const std::string& page) {
    if (page == "dashboard") {
        return R"JS(
let ws=null, WS_GEN=0, WS_RETRY=0, WS_TIMER=0, TERM_RAF=0;
let LAST=null, LAST_CH='', TERM_TEXT='', TERM_FIND='', TERM_HIT=0, TERM_MARKS=[], TERM_LOCK=null, TERM_TAKE_SID='web-'+Date.now().toString(36);
const FONT_KEY='sb3_term_font';
const NAV_KEY='sb3_dash_nav_collapsed_v2';
const KEY_SEQ={
  esc:'\x1b',tab:'\t','ctrl-c':'\x03','ctrl-d':'\x04','ctrl-z':'\x1a','ctrl-l':'\x0c',
  'ctrl-u':'\x15','ctrl-r':'\x12',up:'\x1b[A',down:'\x1b[B',left:'\x1b[D',right:'\x1b[C',
  home:'\x1b[H',end:'\x1b[F',pgup:'\x1b[5~',pgdn:'\x1b[6~'
};
let XTERM=null,FIT=null,XTERM_MODE=false,XTERM_FAIL=false,XTERM_PROMISE=null,TERM_FIT_RAF=0;
const XTERM_CSS='https://cdn.jsdelivr.net/npm/xterm@5.3.0/css/xterm.css';
const XTERM_JS='https://cdn.jsdelivr.net/npm/xterm@5.3.0/lib/xterm.js';
const XTERM_FIT='https://cdn.jsdelivr.net/npm/xterm-addon-fit@0.8.0/lib/xterm-addon-fit.js';
function loadCssOnce(href){
  return new Promise((resolve,reject)=>{
    if(document.querySelector('link[data-sb3="'+href+'"]')){resolve();return}
    const l=document.createElement('link');l.rel='stylesheet';l.href=href;l.dataset.sb3=href;
    l.onload=()=>resolve();l.onerror=()=>reject(new Error('css'));document.head.appendChild(l);
  });
}
function loadJsOnce(src){
  return new Promise((resolve,reject)=>{
    const old=document.querySelector('script[data-sb3="'+src+'"]');
    if(old){if(old.dataset.loaded==='1')resolve();else{old.addEventListener('load',()=>resolve(),{once:true});old.addEventListener('error',()=>reject(new Error('js')),{once:true})}return}
    const s=document.createElement('script');s.src=src;s.async=true;s.dataset.sb3=src;
    s.onload=()=>{s.dataset.loaded='1';resolve()};s.onerror=()=>reject(new Error('js'));document.head.appendChild(s);
  });
}
function ensureXterm(){
  if(XTERM_PROMISE)return XTERM_PROMISE;
  XTERM_PROMISE=(async()=>{
    if(window.Terminal&&window.FitAddon&&window.FitAddon.FitAddon)return true;
    try{
      await loadCssOnce(XTERM_CSS);
      await loadJsOnce(XTERM_JS);
      await loadJsOnce(XTERM_FIT);
      return !!(window.Terminal&&window.FitAddon&&window.FitAddon.FitAddon);
    }catch(e){return false}
  })();
  return XTERM_PROMISE;
}
function focusTerm(auto){
  if(!XTERM_MODE||!XTERM)return false;
  if(auto&&window.matchMedia&&window.matchMedia('(pointer:coarse)').matches)return false;
  try{XTERM.focus();return true}catch(e){return false}
}
function enterBytes(){
  const ch=$('#ch')?$('#ch').value:'';
  const meta=LAST&&LAST.channels?LAST.channels.find(x=>x.name===ch):null;
  const mode=(meta&&meta.line_ending)||'crlf';
  if(mode==='none')return '';
  if(meta&&meta.kind==='ssh')return mode==='lf'?'\n':'\r';
  if(mode==='cr')return '\r';
  if(mode==='lf')return '\n';
  return '\r\n';
}
function queueXtermData(data){
  if(!data)return;
  // Same as Win32 TerminalView WM_CHAR: every key is sent immediately; 8ms only coalesces
  // a burst, it never waits for Enter and never renders a local edit box.
  queueInputData(data,'键盘');
}
function fitXterm(){
  if(!XTERM||!FIT)return;
  const apply=()=>{try{FIT.fit()}catch(e){}};
  apply();
  if(TERM_FIT_RAF)return;
  TERM_FIT_RAF=setTimeout(()=>{TERM_FIT_RAF=0;apply()},16);
}
function xtermPaintAll(){
  if(!XTERM_MODE||!XTERM)return false;
  try{XTERM.reset();if(TERM_TEXT)XTERM.write(TERM_TEXT);XTERM.scrollToBottom()}catch(e){}
  return true;
}
function xtermFindNext(advance){
  if(!XTERM_MODE||!XTERM)return false;
  const q=TERM_FIND.trim();if(!q)return true;
  const buf=XTERM.buffer.active;let hits=[];
  for(let r=0;r<buf.length&&hits.length<500;r++){
    const line=buf.getLine(r);if(!line)continue;
    const text=line.translateToString(true);const lower=text.toLowerCase();const ql=q.toLowerCase();
    let at=lower.indexOf(ql);while(at>=0&&hits.length<500){hits.push({row:r,col:at,len:q.length});at=lower.indexOf(ql,at+ql.length)}
  }
  if(!hits.length){toast('没有匹配内容');return true}
  if(advance)TERM_HIT=(TERM_HIT+1)%hits.length;else TERM_HIT=0;
  const h=hits[TERM_HIT];try{XTERM.select(h.col,h.row,h.len);XTERM.scrollToLine(h.row)}catch(e){}
  $('#hint').textContent='匹配 '+hits.length+' 处 · 第 '+(TERM_HIT+1)+' 处';
  return true;
}
function doFindNext(){
  if(XTERM_MODE&&XTERM){xtermFindNext(true);return}
  if(!TERM_MARKS.length){toast('没有匹配内容');return}
  TERM_HIT=(TERM_HIT+1)%TERM_MARKS.length;renderTerm();
}
function focusTermFind(){
  const f=$('#termFind');if(!f)return;f.focus();f.select();
}
async function initXterm(){
  const host=$('#xtermHost'),fb=$('#term');if(!host||!fb)return;
  if(!await ensureXterm()){XTERM_FAIL=true;return}
  try{
    const storedFont=parseInt(localStorage.getItem(FONT_KEY)||'13',10);
    const initialFont=Math.max(13,Math.min(20,storedFont||13));
    XTERM=new Terminal({
      cursorBlink:true,fontSize:initialFont,
      fontFamily:'Cascadia Mono, Consolas, "Courier New", monospace',
      theme:{background:'#081820',foreground:'#dcefe9',cursor:'#7ee0c8',cursorAccent:'#081820',
        selectionBackground:'#1d5a5a',black:'#0b1c22',red:'#ff7b72',green:'#7ee787',yellow:'#e3b341',
        blue:'#79c0ff',magenta:'#d2a8ff',cyan:'#56d4dd',white:'#dcefe9',
        brightBlack:'#6e8b92',brightRed:'#ffa198',brightGreen:'#9be9a8',brightYellow:'#f2cc60',
        brightBlue:'#a5d6ff',brightMagenta:'#e2c5ff',brightCyan:'#8ce8ef',brightWhite:'#ffffff'},
      convertEol:false,scrollback:5000
    });
    FIT=new FitAddon.FitAddon();XTERM.loadAddon(FIT);XTERM.open(host);
    host.hidden=false;fb.hidden=true;XTERM_MODE=true;
    XTERM.onData(data=>{if(data)queueXtermData(data)});
    window.__sb3term={mode:()=>XTERM_MODE,cols:()=>XTERM?XTERM.cols:0,rows:()=>XTERM?XTERM.rows:0,
      fit:()=>{try{FIT.fit();return {cols:XTERM.cols,rows:XTERM.rows}}catch(e){return {err:String(e)}}},
      cursor:()=>(XTERM?{x:XTERM.buffer.active.cursorX,y:XTERM.buffer.active.cursorY,baseY:XTERM.buffer.active.baseY}:null),
      text:()=>{if(!XTERM)return '';const b=XTERM.buffer.active,out=[];for(let i=0;i<b.length;i++){const l=b.getLine(i);out.push(l?l.translateToString(true):'')}return out.join('\n')}};
    XTERM.attachCustomKeyEventHandler(ev=>{
      if(ev.type!=='keydown')return true;
      if(ev.ctrlKey&&!ev.shiftKey&&!ev.altKey&&(ev.key==='f'||ev.key==='F')){focusTermFind();return false}
      if(!ev.ctrlKey&&!ev.altKey&&!ev.metaKey&&ev.key==='F3'){doFindNext();return false}
      if(!ev.ctrlKey&&!ev.altKey&&!ev.metaKey&&ev.key==='Enter'){
        const b=enterBytes();if(b)queueInputData(b,'Enter');return false;
      }
      // Keep the Win32 mapping explicit for keys whose browser/xterm defaults differ.
      if(!ev.ctrlKey&&!ev.altKey&&!ev.metaKey&&(ev.key==='Home'||ev.key==='End')){
        queueInputData(ev.key==='Home'?'\x1b[H':'\x1b[F',ev.key);return false;
      }
      if(!ev.ctrlKey&&!ev.altKey&&!ev.metaKey&&ev.key==='Backspace'){
        queueInputData('\x7f','退格');return false;
      }
      if(ev.ctrlKey&&ev.shiftKey&&(ev.key==='C'||ev.key==='c')){
        if(XTERM.hasSelection()){copyTerm();return false}
      }
      if(ev.ctrlKey&&ev.shiftKey&&(ev.key==='V'||ev.key==='v')){pasteTerm();return false}
      return true;
    });
    fitXterm();
    setTimeout(fitXterm,60);setTimeout(fitXterm,350);setTimeout(fitXterm,900);
    setTimeout(fitXterm,1500);setTimeout(fitXterm,3000);
    setTimeout(()=>focusTerm(true),120);
    window.addEventListener('resize',fitXterm);
    document.addEventListener('visibilitychange',()=>{if(!document.hidden)fitXterm()});
    if(typeof ResizeObserver!=='undefined'){new ResizeObserver(()=>fitXterm()).observe(host)}
    xtermPaintAll();
    $('#hint').textContent='xterm 5.3 已启用 · 点会话区直接输入，字符立即下发并在同区回显';
  }catch(e){XTERM_FAIL=true;XTERM_MODE=false;host.hidden=true;fb.hidden=false}
}
function copyTerm(){
  let text='';
  if(XTERM_MODE&&XTERM&&XTERM.hasSelection())text=XTERM.getSelection();
  if(!text)text=cleanTerm(TERM_TEXT);
  const done=()=>toast('回显已复制');
  if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(text).then(done).catch(()=>legacyCopy(text,done))}
  else legacyCopy(text,done);
}
function legacyCopy(text,done){
  const t=document.createElement('textarea');t.value=text;document.body.appendChild(t);t.select();
  try{document.execCommand('copy')}catch(e){}
  t.remove();done();
}
function pasteTerm(){
  if(navigator.clipboard&&navigator.clipboard.readText){
    navigator.clipboard.readText().then(t=>{if(t)sendRawBytes(t,'粘贴')}).catch(()=>toast('浏览器拒绝读取剪贴板，请用底部输入框粘贴','error'));
  }else toast('请用底部输入框粘贴','error');
}
function cleanTerm(raw){
  let out='',i=0;
  while(i<raw.length){
    const c=raw[i];
    if(c==='\x1b'){
      i++;
      if(raw[i]==='['){
        i++;
        while(i<raw.length&&!(raw[i]>='@'&&raw[i]<='~'))i++;
        if(i<raw.length)i++;
      }else if(raw[i]===']'){
        i++;
        while(i<raw.length&&raw[i]!=='\x07'&&!(raw[i]==='\x1b'&&raw[i+1]==='\\'))i++;
        if(raw[i]==='\x07')i++;else i+=2;
      }else i++;
      continue;
    }
    if(c==='\r'){
      if(raw[i+1]==='\n'){i++;continue}
      const nl=out.lastIndexOf('\n');out=out.slice(0,nl+1);i++;continue;
    }
    if(c==='\b'){
      if(out.length&&out[out.length-1]!=='\n')out=out.slice(0,-1);
      i++;continue;
    }
    if(c==='\n'||c==='\t'||c>='\x20')out+=c;
    i++;
  }
  return out;
}
const SVC_DEFS=[['tftp','TFTP','/tftp'],['share','共享','/share'],['nfs','NFS','/nfs'],['inbound','入站 SSH','/tunnel'],['tcp_forward','TCP','/tcp-forward'],['tunnel','隧道','/tunnel'],['discovery','发现','/tcp-forward']];
function applyNav(collapsed){
  const dash=$('#dash'),btn=$('#toggleNav');if(!dash)return;
  dash.classList.toggle('nav-collapsed',!!collapsed);
  if(btn)btn.textContent=collapsed?'显示通道':'隐藏通道';
  localStorage.setItem(NAV_KEY,collapsed?'1':'0');
  if(TERM_RAF===0)scheduleTermRender();
}
$('#toggleNav').onclick=()=>applyNav(!$('#dash').classList.contains('nav-collapsed'));
const NAV_SAVED=localStorage.getItem(NAV_KEY);
const NAV_NARROW=matchMedia('(max-width:1020px)').matches;
applyNav(NAV_SAVED===null?NAV_NARROW:NAV_SAVED==='1');
const GNAV_KEY='sb3_dash_page_nav_hidden';
function applyGlobalNav(hidden){
  document.body.classList.toggle('nav-hidden',!!hidden);
  const b=$('#navToggle');if(b)b.textContent=hidden?'展开导航':'收起导航';
  try{localStorage.setItem(GNAV_KEY,hidden?'1':'0')}catch(e){}
}
if($('#navToggle')){
  applyGlobalNav(!matchMedia('(max-width:760px)').matches&&localStorage.getItem(GNAV_KEY)==='1');
  $('#navToggle').onclick=()=>applyGlobalNav(!document.body.classList.contains('nav-hidden'));
}
function closeNavMenu(){document.body.classList.remove('nav-open');if($('#navMenuBtn'))$('#navMenuBtn').setAttribute('aria-expanded','false')}
if($('#navMenuBtn'))$('#navMenuBtn').onclick=e=>{
  e.stopPropagation();const open=!document.body.classList.contains('nav-open');
  document.body.classList.toggle('nav-open',open);$('#navMenuBtn').setAttribute('aria-expanded',open?'true':'false');
};
document.addEventListener('click',e=>{
  if(document.body.classList.contains('nav-open')&&!e.target.closest('aside.sidebar'))closeNavMenu();
});
document.addEventListener('keydown',e=>{if(e.key==='Escape')closeNavMenu()});
document.addEventListener('keydown',e=>{
  if(!document.body.classList.contains('pg-dashboard'))return;
  if(e.key==='F5'){e.preventDefault();dashLoad();return}
  if(e.ctrlKey&&e.shiftKey&&(e.key==='b'||e.key==='B')){
    e.preventDefault();
    if(matchMedia('(max-width:760px)').matches){document.body.classList.toggle('nav-open');if($('#navMenuBtn'))$('#navMenuBtn').setAttribute('aria-expanded',document.body.classList.contains('nav-open')?'true':'false')}
    else applyGlobalNav(!document.body.classList.contains('nav-hidden'));
    return;
  }
  if((e.ctrlKey||e.metaKey)&&!e.shiftKey&&!e.altKey&&(e.key==='f'||e.key==='F')){
    e.preventDefault();focusTermFind();return;
  }
  if(e.key==='F3'&&!e.ctrlKey&&!e.altKey&&!e.metaKey){
    e.preventDefault();doFindNext();return;
  }
});
function chType(c){return c.kind==='ssh'?'SSH':(c.kind==='powershell'?'PowerShell':(c.kind==='telnet'?'Telnet':'串口'))}
function renderChannels(st){
  const box=$('#channelList'), sel=$('#ch'); if(!box||!st||!st.channels)return;
  box.innerHTML=''; const active=LAST_CH||(sel?sel.value:'');
  st.channels.forEach(c=>{
    const b=document.createElement('button');b.type='button';b.className='channel-item'+(c.name===active?' active':'');
    b.innerHTML='<span class="status-dot '+(isOnline(c)?'on':'off')+'"></span><span class="ch-main"><span class="ch-name"></span><span class="ch-detail"></span></span><span class="ch-num"></span>';
    b.querySelector('.ch-name').textContent=c.name;
    b.querySelector('.ch-detail').textContent=chType(c)+' · '+c.state+(c.detail?' · '+c.detail:'');
    b.querySelector('.ch-num').textContent='TX '+fmtBytes(c.tx_bytes||0)+' · RX '+fmtBytes(c.rx_bytes||0);
    b.onclick=()=>{if(sel)sel.value=c.name;if(c.name!==LAST_CH)connect();renderChannels(st)};
    box.appendChild(b);
  });
  const online=st.channels.filter(isOnline).length;
  $('#onlineCount').textContent=online+' / '+st.channels.length+' 在线';
  if(!st.channels.length)box.innerHTML='<div class="empty" style="padding:18px 8px"><strong>没有通道</strong><p><a href="/channels">去通道管理新增</a></p></div>';
}
function renderServices(st){
  const box=$('#svcBadges');if(!box||!st)return;
  const svc=st.services||{};
  box.innerHTML='';
  SVC_DEFS.forEach(d=>{
    const s=svc[d[0]]||{};
    const a=document.createElement('a');a.className='svc-card';a.href=d[2];
    a.innerHTML='<div><span class="svc-name">'+esc(d[1])+'</span><div class="svc-note">'+esc(s.note||(s.on?'运行中':'已停止'))+'</div></div><span class="chip '+(s.on?'on':'off')+'">'+(s.on?'ON':'OFF')+'</span>';
    box.appendChild(a);
  });
}
function renderTerm(){
  if(XTERM_MODE&&XTERM){
    if(TERM_FIND.trim())xtermFindNext(false);
    else{try{XTERM.clearSelection()}catch(e){}}
    return;
  }
  const p=$('#term');if(!p)return;
  const display=cleanTerm(TERM_TEXT);
  const q=TERM_FIND.trim();
  if(!q){p.textContent=display;TERM_MARKS=[];TERM_HIT=0;return}
  let html='',lower=display.toLowerCase(),ql=q.toLowerCase(),pos=0,hits=0;
  while(pos<display.length&&hits<500){
    const idx=lower.indexOf(ql,pos);if(idx<0)break;
    html+=esc(display.slice(pos,idx));
    html+='<mark class="'+(hits===TERM_HIT?'active':'')+'" data-hit="'+hits+'">'+esc(display.slice(idx,idx+q.length))+'</mark>';
    pos=idx+q.length;hits++;
  }
  html+=esc(display.slice(pos));
  p.innerHTML=html;TERM_MARKS=Array.from(p.querySelectorAll('mark'));
  if(!TERM_MARKS.length)p.textContent=display;
  else if(TERM_MARKS[TERM_HIT])TERM_MARKS[TERM_HIT].scrollIntoView({block:'nearest'});
}
function scheduleTermRender(){
  if(TERM_RAF)return;
  TERM_RAF=requestAnimationFrame(()=>{TERM_RAF=0;renderTerm();const p=$('#term');if(p&&!TERM_FIND.trim())p.scrollTop=p.scrollHeight});
}
function appendTerm(t){
  TERM_TEXT+=t;if(TERM_TEXT.length>180000)TERM_TEXT=TERM_TEXT.slice(-120000);
  if(XTERM_MODE&&XTERM){try{XTERM.write(t)}catch(e){};return}
  scheduleTermRender();
}
function renderTermLock(){
  const chip=$('#termLock'),banner=$('#lockBanner'),text=$('#lockText'),take=$('#takeLock');
  const l=TERM_LOCK;
  if(!chip)return;
  if(!l){
    chip.textContent='无 Agent 锁';chip.className='chip on';
    if(banner)banner.hidden=true;
    if(take)take.hidden=true;
    return;
  }
  const owner=l.owner||'agent';
  const left=Math.max(0,Math.round((l.expires_in_ms||0)/1000));
  chip.textContent='Agent 操作中';chip.className='chip warn';
  if(banner)banner.hidden=false;
  if(text)text.textContent='通道 '+l.channel+' 被 '+owner+' 持有；剩余 '+left+'s。人类写入前需接管。';
  if(take)take.hidden=!tok();
}
async function lockRefresh(ch){
  if(!ch)return;
  const r=await api('/agent/scan');
  if(!r.j||!r.j.locks){return}
  const leases=r.j.locks.leases||[];
  TERM_LOCK=leases.find(l=>l.channel===ch)||null;
  renderTermLock();
}
async function takeLock(){
  const ch=$('#ch').value;
  if(!ch)return;
  if(!tok()){toast('人工接管需要先保存 Bearer token','error');return}
  const who=(TERM_LOCK&&TERM_LOCK.owner)||'Agent';
  if(!await askConfirm('人工接管通道','将清掉 '+who+' 在 '+ch+' 上的租约，并把后续写操作交给网页终端。','接管'))return;
  const r=await api('/lock/takeover',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:ch,reason:'web-terminal-human',session_id:TERM_TAKE_SID})});
  if(r.status===200&&r.j&&r.j.ok){
    toast('已接管 '+ch);TERM_LOCK=null;renderTermLock();
    $('#hint').textContent='人工已接管 '+ch+'；现在可发送命令。';lockRefresh(ch);
  }else{
    toast('接管失败：'+r.status+' '+(r.j&&(r.j.error&&(r.j.error.message||r.j.error.code)||r.j.error)||r.t),'error');
  }
}
function applyFont(px){
  px=Math.max(10,Math.min(22,px||13));localStorage.setItem(FONT_KEY,String(px));
  const card=$('.terminal-card');if(card)card.style.setProperty('--term-font',px+'px');
  if(XTERM_MODE&&XTERM){try{XTERM.options.fontSize=px}catch(e){}fitXterm()}
}
function setWsState(text,cls){const b=$('#wsState');if(b){b.textContent=text;b.className='chip '+cls}}
async function syncTermLog(ch){
  const r=await api('/log?'+qs({channel:ch,chars:16000}));
  if($('#ch').value!==ch)return 'stale';
  const err=r.j&&(r.j.error||(r.j.error&&r.j.error.code));
  if(r.status===404||err==='channel not found')return 'missing';
  if(r.j&&typeof r.j.text==='string'){
    TERM_TEXT=r.j.text;
    if(XTERM_MODE&&XTERM)xtermPaintAll();
    else{renderTerm();const p=$('#term');if(p)p.scrollTop=p.scrollHeight}
  }
  return 'ok';
}
function wsRetryLater(ch){
  const gen=WS_GEN;
  WS_RETRY=Math.min(WS_RETRY+1,5);
  const delay=Math.min(1000*Math.pow(2,WS_RETRY-1),15000);
  $('#hint').textContent='WS 已断开 · '+ch+' · '+Math.ceil(delay/1000)+'s 后重连';
  setWsState('等待重连', 'warn');
  WS_TIMER=setTimeout(async()=>{
    WS_TIMER=0;
    if(gen!==WS_GEN||$('#ch').value!==ch)return;
    const st=await syncTermLog(ch);
    if(st==='missing'){$('#hint').textContent='通道不存在或已删除：'+ch;setWsState('通道不可用','danger');return}
    if(st==='stale')return;
    openWs(ch,gen);
  },delay);
}
function openWs(ch,gen){
  if(gen!==WS_GEN||$('#ch').value!==ch)return;
  const proto=location.protocol==='https:'?'wss':'ws';
  const sock=new WebSocket(proto+'://'+location.host+'/ws/log?channel='+encodeURIComponent(ch));
  ws=sock;
  sock.binaryType='arraybuffer';
  sock.onopen=()=>{
    if(gen!==WS_GEN){try{sock.close()}catch(e){};return}
    WS_RETRY=0;$('#hint').textContent='WS 已连接 · '+ch;setWsState('WS 已连接','on');
  };
  sock.onmessage=e=>{if(gen===WS_GEN)appendTerm(bytesText(e.data))};
  sock.onclose=()=>{
    if(gen!==WS_GEN)return;
    ws=null;setWsState('WS 已断开','warn');wsRetryLater(ch);
  };
  sock.onerror=()=>{if(gen===WS_GEN){$('#hint').textContent='WS 连接失败 · '+ch;setWsState('连接失败','danger')}};
}
async function connect(){
  WS_GEN++;const gen=WS_GEN;
  if(WS_TIMER){clearTimeout(WS_TIMER);WS_TIMER=0}
  WS_RETRY=0;
  if(ws){try{ws.close()}catch(e){};ws=null}
  const ch=$('#ch').value; if(!ch){$('#term').textContent='没有通道';return}
  LAST_CH=ch;TERM_TEXT='';TERM_FIND='';TERM_HIT=0;
  if(XTERM_MODE&&XTERM)xtermPaintAll();else renderTerm();
  lockRefresh(ch);
  $('#activeName').textContent=ch;
  if(LAST)renderChannels(LAST);
  updateRxBadge(ch);
  $('#hint').textContent='正在连接 '+ch+' …';
  const dl=$('#dlTerm');if(dl)dl.href='/log/download?channel='+encodeURIComponent(ch);
  const st=await syncTermLog(ch);
  if(gen!==WS_GEN)return;
  if(st==='missing'){$('#hint').textContent='通道不存在或已删除：'+ch;setWsState('通道不可用','danger');return}
  openWs(ch,gen);
  setTimeout(()=>focusTerm(true),80);
}
let PANES=null;
function renderPaneStrip(){
  const box=$('#paneStrip');if(!box)return;
  if(!PANES||!PANES.ok){box.innerHTML='<span class="pane-label">分屏</span><span class="form-status">画框读取失败</span>';return}
  let html='<span class="pane-label">分屏 '+PANES.split+'</span>';
  for(const p of (PANES.panes||[])){
    const cls='pane-chip '+(p.active?'active ':'')+(p.bound?'':'empty ')+(p.running?'on':'');
    html+='<button type="button" class="'+cls+'" data-pane="'+p.index+'"'+(p.bound?'':' disabled')+
      ' title="'+(p.bound?esc(p.channel):'空画框')+'"><span class="pane-dot"></span>画框 '+(p.index+1)+' · '+
      (p.bound?esc(p.channel):'空')+'</button>';
  }
  box.innerHTML=html;
  box.querySelectorAll('[data-pane]').forEach(b=>{b.onclick=()=>paneActivate(Number(b.dataset.pane))});
}
async function paneLoad(){
  const r=await api('/api/panes');
  if(!r.j||!r.j.ok){PANES=null;renderPaneStrip();return}
  PANES=r.j;renderPaneStrip();
}
async function paneActivate(index){
  const r=await api('/api/panes/active',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index})});
  if(r.status===401){toast('激活画框需要先设置 Bearer token','error');return}
  if(!r.j||!r.j.ok){toast('激活失败：'+((r.j&&r.j.error&&(r.j.error.message||r.j.error.code))||r.status),'error');return}
  const ch=r.j.channel||'';
  toast(r.j.live?'已激活画框 '+(index+1)+' · '+ch:'已保存画框 '+(index+1)+'，原生窗口未运行');
  await paneLoad();
  if(ch){const sel=$('#ch');if(!Array.from(sel.options).some(o=>o.value===ch)){const o=document.createElement('option');o.value=ch;o.textContent=ch;sel.appendChild(o)}sel.value=ch;if(LAST_CH!==ch)connect()}
}
async function dashLoad(){
  const r=await api('/status'); if(!r.j||!r.j.channels){$('#term').textContent=r.t;toast('状态读取失败','error');return}
  LAST=r.j;const sel=$('#ch');const old=sel.value;sel.innerHTML='';
  for(const c of r.j.channels){const o=document.createElement('option');o.value=c.name;o.textContent=c.name+' · '+c.state;sel.appendChild(o)}
  if(old&&r.j.channels.some(c=>c.name===old))sel.value=old;else if(!sel.value&&r.j.channels[0])sel.value=r.j.channels[0].name;
  renderChannels(r.j);renderServices(r.j);paneLoad();
  updateRxBadge(sel.value);
  const ro=!tok();$('#ro').textContent=ro?'只读（无 token）':'可写（带 token）';$('#ro').className='pill '+(ro?'off':'');
  if(sel.value&&(sel.value!==LAST_CH||(!ws&&!WS_TIMER)))connect();
  else if(sel.value)lockRefresh(sel.value);
}
$('#reload').onclick=dashLoad;
if($('#keyHelp'))$('#keyHelp').onclick=()=>toast('F5 刷新通道 · Ctrl+F 查找回显 · Enter/F3 下一个命中 · Ctrl+Shift+B 收起/展开导航 · Ctrl+Shift+C/V 复制/粘贴');
$('#ch').onchange=connect;
$('#termFind').oninput=e=>{TERM_FIND=e.target.value;TERM_HIT=0;renderTerm()};
$('#termFind').onkeydown=e=>{
  if(e.key==='Enter'){e.preventDefault();doFindNext()}
  if(e.key==='Escape'){e.target.value='';TERM_FIND='';TERM_HIT=0;renderTerm();e.target.blur()}
};
$('#findNext').onclick=()=>doFindNext();
$('#copyTerm').onclick=()=>copyTerm();
$('#fontMinus').onclick=()=>{applyFont(parseInt(localStorage.getItem(FONT_KEY)||'13',10)-1)};
$('#fontPlus').onclick=()=>{applyFont(parseInt(localStorage.getItem(FONT_KEY)||'13',10)+1)};
$('#fullTerm').onclick=async()=>{const c=$('.terminal-card');if(!document.fullscreenElement){if(c.requestFullscreen)await c.requestFullscreen()}else await document.exitFullscreen()};
document.addEventListener('fullscreenchange',()=>{const b=$('#fullTerm');if(b)b.textContent=document.fullscreenElement?'退出全屏':'全屏';setTimeout(fitXterm,80)});
$('#clearView').onclick=()=>{TERM_TEXT='';if(XTERM_MODE&&XTERM)xtermPaintAll();else renderTerm();$('#hint').textContent='已清屏';focusTerm(false)};
$('#clearBuf').onclick=async()=>{
  const ch=$('#ch').value;if(!ch)return;if(!await askConfirm('清空通道缓冲','将清空 '+ch+' 的共享 ring，所有终端/网页都会看不到旧回显。','清空',true))return;
  const r=await api('/log/clear?channel='+encodeURIComponent(ch),{method:'POST'});if(r.status===200){TERM_TEXT='';if(XTERM_MODE&&XTERM)xtermPaintAll();else renderTerm();toast('缓冲已清空')}else toast('清空失败：'+r.status+' '+r.t,'error');
};
async function sendRawBytes(data,label){
  const ch=$('#ch').value;
  if(!ch)return false;
  if(TERM_LOCK){
    const msg='Agent 操作中：'+ch+' 被 '+(TERM_LOCK.owner||'agent')+' 持有，请先人工接管。';
    $('#hint').textContent=msg;toast(msg,'error');return false;
  }
  const r=await api('/write',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:ch,data})});
  if(r.status===423){$('#hint').textContent='写入被 Agent 锁拦截';toast('通道被 Agent 锁住，请先人工接管','error');lockRefresh(ch);return false}
  if(r.status===401){$('#hint').textContent='写入需要 Bearer token';toast('只读模式：先设置 token','error');return false}
  if(!r.r.ok){$('#hint').textContent='写入失败：'+r.status+' '+r.t;toast('发送失败','error');return false}
  $('#hint').textContent='已写入 '+ch+(label?' · '+label:'');
  return true;
}
let IN_BUF='',IN_LABEL='键盘',IN_TIMER=0,IN_CHAIN=Promise.resolve();
function flushInput(){
  if(!IN_BUF)return;
  const data=IN_BUF,label=IN_LABEL;IN_BUF='';IN_LABEL='键盘';
  IN_CHAIN=IN_CHAIN.then(()=>sendRawBytes(data,label)).catch(()=>{});
}
function queueInputData(data,label){
  if(!data)return;
  IN_BUF+=data;if(label)IN_LABEL=label;
  if(IN_TIMER)return;
  IN_TIMER=setTimeout(()=>{IN_TIMER=0;flushInput()},8);
}
function inputNow(data,label){if(!data)return;queueInputData(data,label||'按键')}
$$('#keyBar [data-key]').forEach(b=>{
  b.onclick=()=>{const seq=KEY_SEQ[b.dataset.key];if(seq===undefined)return;queueInputData(seq,b.textContent.trim());focusTerm(false)};
});
$('#term').onclick=()=>focusTerm(false);
if($('#xtermHost'))$('#xtermHost').addEventListener('mousedown',()=>setTimeout(()=>focusTerm(false),0));
$('#takeLock').onclick=takeLock;
applyFont(parseInt(localStorage.getItem(FONT_KEY)||'13',10));
initXterm().catch(()=>{XTERM_FAIL=true});
setInterval(async()=>{const r=await api('/status');if(r.j&&r.j.channels){LAST=r.j;renderChannels(r.j);renderServices(r.j);updateRxBadge($('#ch').value)}const ch=$('#ch').value;if(ch)lockRefresh(ch)},5000);
dashLoad();
)JS";
    }
    if (page == "channels") {
        return R"JS(
let CHAN={boards:[],ssh:[],powershell:[],telnet:[]}, EDIT={kind:'serial',old:''}, CHAN_STATUS={};
const SERIAL_IDS=['cDevice','cBaud','cDataBits','cParity','cStopBits','cFlow','cEncoding','cUsername','cPassword','cAutoOpen','cDtr','cRts','cDiscoverable'];
const SSH_IDS=['cSshUser','cSshPassword','cTerm','cKeepalive'];
const NET_IDS=['cHost','cPort','cWidth','cHeight','cTimeout'];
const PS_IDS=['cShell','cArgs','cCwd'];
const KIND_LABELS={serial:'串口',ssh:'SSH',powershell:'PowerShell',telnet:'Telnet'};
const KIND_KEYS={serial:'boards',ssh:'ssh',powershell:'powershell',telnet:'telnet'};
function chOnline(name){const c=(CHAN_STATUS.channels||[]).find(x=>x.name===name);return c&&(c.state==='ready'||c.state==='online')}
function showField(el,on){if(!el)return;const box=el.closest('.field')||el.closest('.check-row')||el;box.style.display=on?'':'none'}
function setVal(id,v){const e=$('#'+id);if(e)e.value=v==null?'':String(v)}
function setChk(id,v){const e=$('#'+id);if(e)e.checked=!!v}
function kindLabel(kind){return KIND_LABELS[kind]||'串口'}
function showKind(kind){
  $('#cKind').value=kindLabel(kind);
  $('#chanKindChip').textContent=kindLabel(kind);
  const serial=kind==='serial',ssh=kind==='ssh',ps=kind==='powershell',tn=kind==='telnet';
  SERIAL_IDS.forEach(id=>showField($('#'+id),serial));
  SSH_IDS.forEach(id=>showField($('#'+id),ssh));
  NET_IDS.forEach(id=>showField($('#'+id),ssh||tn));
  PS_IDS.forEach(id=>showField($('#'+id),ps));
  showField($('#cLineEnding'),serial||ps||tn);
  showField($('#cTimestamp'),serial||ps||tn);
  showField($('#cLogEnabled'),true);
  $('#chanEditorTitle').textContent=(EDIT.old?'编辑 ':'新增 ')+kindLabel(kind)+' 通道';
  $('#chanEditorSub').textContent=serial?'串口设备参数；loop: 设备可用于无硬件验收。':
    ssh?'SSH 出站连接参数；保存后由 UI 线程重载并重连。':
    ps?'本机持久 PowerShell 会话：程序 + 参数 + 可选工作目录。':'Telnet TCP 连接：主机 + 端口 + 最小 IAC 协商。';
}
function findChannel(kind,name){return (CHAN[KIND_KEYS[kind]]||[]).find(x=>x.name===name)}
function renderChannelsPage(){
  const box=$('#chanCards');if(!box)return;
  const all=[];
  ['serial','ssh','powershell','telnet'].forEach(k=>{
    (CHAN[KIND_KEYS[k]]||[]).forEach(c=>all.push({...c,_kind:k}));
  });
  $('#chanCount').textContent=all.length+' 个通道';
  $('#chanOnline').textContent=all.filter(c=>chOnline(c.name)).length+' 在线';
  if(!all.length){box.innerHTML='<div class="empty" style="grid-column:1/-1"><div class="empty-mark">+</div><strong>还没有通道</strong><p>新增一个 loop: 串口、SSH、PowerShell 或 Telnet 通道开始。</p></div>';return}
  box.innerHTML='';
  all.forEach(c=>{
    const d=document.createElement('div');d.className='chan-card';
    let detail='';
    if(c._kind==='serial')detail=(c.serial_device||'')+' · '+(c.baudrate||115200)+' baud';
    else if(c._kind==='ssh')detail=(c.username||'')+'@'+(c.host||'')+':'+(c.port||22);
    else if(c._kind==='powershell')detail=(c.shell||'powershell.exe')+(c.cwd?' · '+c.cwd:'');
    else detail=(c.host||'')+':'+(c.port||23);
    d.innerHTML='<div class="chan-top"><span class="status-dot '+(chOnline(c.name)?'on':'off')+'"></span><span class="chan-name"></span><span class="chip"></span></div><div class="chan-detail"></div><div class="chan-actions"><button class="btn ghost" data-edit="1" type="button">编辑</button><button class="btn danger" data-del="1" type="button">删除</button></div>';
    d.querySelector('.chan-name').textContent=c.name;
    d.querySelector('.chip').textContent=kindLabel(c._kind);
    d.querySelector('.chan-detail').textContent=detail+' · '+(chOnline(c.name)?'在线':'离线')+(c.auto_open===false?' · 手动打开':'');
    d.querySelector('[data-edit]').onclick=()=>editChannel(c._kind,c.name);
    d.querySelector('[data-del]').onclick=()=>delChannel(c._kind,c.name);
    box.appendChild(d);
  });
}
async function loadChannels(){
  const [a,b]=await Promise.all([api('/api/channels'),api('/status')]);
  if(a.status===401){$('#chanCards').innerHTML='<div class="empty" style="grid-column:1/-1"><strong>需要 token</strong><p>在右上角填入 web.token 后再读取通道。</p></div>';$('#chanNote').textContent='只读模式：通道列表需要 token。';return}
  if(!a.j||!a.j.ok){$('#chanCards').innerHTML='<div class="empty" style="grid-column:1/-1"><strong>读取失败</strong></div>';$('#chanNote').textContent='读取失败';return}
  CHAN=a.j;CHAN_STATUS=b.j||{};$('#chanNote').textContent='配置读写需要 token；保存后约 1 秒重载。';renderChannelsPage();
}
function blankChannel(kind){
  EDIT={kind,old:''};showKind(kind);
  setVal('cName','');setVal('cDevice','loop:ai');setVal('cBaud',115200);setVal('cDataBits','8');setVal('cParity','N');setVal('cStopBits','10');setVal('cFlow','none');setVal('cLineEnding',kind==='powershell'?'cr':(kind==='telnet'?'crlf':'none'));setVal('cEncoding','utf-8');setVal('cUsername','');setVal('cPassword','');
  setChk('cAutoOpen',true);setChk('cTimestamp',false);setChk('cDtr',true);setChk('cRts',true);
  setVal('cHost','');setVal('cPort',kind==='telnet'?23:22);setVal('cSshUser','root');setVal('cSshPassword','');setVal('cTerm','xterm-256color');setVal('cKeepalive',3);setVal('cWidth',120);setVal('cHeight',32);setVal('cTimeout',5000);
  setVal('cShell','powershell.exe');setVal('cArgs','-NoLogo -NoProfile -ExecutionPolicy Bypass');setVal('cCwd','');
  setChk('cDiscoverable',false);setChk('cLogEnabled',true);
  $('#chanEditNote').textContent='';$('#chanEditor').hidden=false;$('#chanEditor').scrollIntoView({behavior:'smooth',block:'start'});setTimeout(()=>$('#cName').focus(),80);
}
function editChannel(kind,name){
  const c=findChannel(kind,name);if(!c)return;EDIT={kind,old:name};showKind(kind);
  setVal('cName',c.name);setVal('cDevice',c.serial_device);setVal('cBaud',c.baudrate);setVal('cDataBits',c.databits||8);setVal('cParity',c.parity||'N');setVal('cStopBits',c.stopbits_x10||10);setVal('cFlow',c.flowcontrol||'none');setVal('cLineEnding',c.line_ending||'none');setVal('cEncoding',c.encoding||'utf-8');setVal('cUsername',c.username);setVal('cPassword',c.password);
  setChk('cAutoOpen',c.auto_open!==false);setChk('cTimestamp',!!c.timestamp);setChk('cDtr',c.dtr!==false);setChk('cRts',c.rts!==false);
  setVal('cHost',c.host);setVal('cPort',c.port||(kind==='telnet'?23:22));setVal('cSshUser',c.username);setVal('cSshPassword',c.password);setVal('cTerm',c.term||'xterm-256color');setVal('cKeepalive',c.keepalive==null?3:c.keepalive);setVal('cWidth',c.width||120);setVal('cHeight',c.height||32);setVal('cTimeout',c.connect_timeout_ms||5000);
  setVal('cShell',c.shell||'powershell.exe');setVal('cArgs',c.args||'-NoLogo -NoProfile -ExecutionPolicy Bypass');setVal('cCwd',c.cwd||'');
  setChk('cDiscoverable',!!c.discoverable);setChk('cLogEnabled',c.log_enabled!==false);
  $('#chanEditNote').textContent='';$('#chanEditor').hidden=false;$('#chanEditor').scrollIntoView({behavior:'smooth',block:'start'});
}
function collectChannel(){
  const kind=EDIT.kind,ch={name:$('#cName').value.trim()};
  if(kind==='serial'){
    ch.serial_device=$('#cDevice').value.trim();ch.baudrate=Number($('#cBaud').value||115200);ch.databits=Number($('#cDataBits').value||8);ch.parity=$('#cParity').value;ch.stopbits_x10=Number($('#cStopBits').value||10);ch.flowcontrol=$('#cFlow').value;ch.line_ending=$('#cLineEnding').value;ch.encoding=$('#cEncoding').value.trim();ch.username=$('#cUsername').value.trim();ch.password=$('#cPassword').value;
    ch.auto_open=$('#cAutoOpen').checked;ch.timestamp=$('#cTimestamp').checked;ch.dtr=$('#cDtr').checked;ch.rts=$('#cRts').checked;
    ch.discoverable=$('#cDiscoverable').checked;
  }else if(kind==='ssh'){
    ch.host=$('#cHost').value.trim();ch.port=Number($('#cPort').value||22);ch.username=$('#cSshUser').value.trim();ch.password=$('#cSshPassword').value;ch.term=$('#cTerm').value.trim()||'xterm-256color';ch.width=Number($('#cWidth').value||120);ch.height=Number($('#cHeight').value||32);ch.connect_timeout_ms=Number($('#cTimeout').value||5000);ch.keepalive=Number($('#cKeepalive').value||0);
    ch.discoverable=$('#cDiscoverable').checked;
  }else if(kind==='powershell'){
    ch.shell=$('#cShell').value.trim()||'powershell.exe';ch.args=$('#cArgs').value.trim();ch.cwd=$('#cCwd').value.trim();ch.line_ending=$('#cLineEnding').value;ch.timestamp=$('#cTimestamp').checked;
  }else{
    ch.host=$('#cHost').value.trim();ch.port=Number($('#cPort').value||23);ch.connect_timeout_ms=Number($('#cTimeout').value||5000);ch.width=Number($('#cWidth').value||120);ch.height=Number($('#cHeight').value||32);ch.line_ending=$('#cLineEnding').value;ch.timestamp=$('#cTimestamp').checked;
  }
  ch.log_enabled=$('#cLogEnabled').checked;
  return {kind,old_name:EDIT.old,channel:ch};
}
async function saveChannel(){
  const btn=$('#saveChannel'),payload=collectChannel();
  if(!payload.channel.name){$('#chanEditNote').textContent='通道名不能为空';return}
  setBusy(btn,true,'保存中…');
  const r=await api('/api/channels',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
  setBusy(btn,false);
  if(!r.j||!r.j.ok){$('#chanEditNote').textContent=(r.j&&r.j.error)||('HTTP '+r.status+' '+r.t);toast('保存失败','error');return}
  toast('通道已保存，正在重载');$('#chanEditor').hidden=true;setTimeout(loadChannels,1400);setTimeout(loadChannels,2600);
}
async function delChannel(kind,name){
  if(!await askConfirm('删除通道','确定删除 '+name+'？该通道的按钮、快捷指令和分屏引用会一起清理，此操作不可撤销。','删除',true))return;
  const r=await api('/api/channels/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({kind:kind,name})});
  if(!r.j||!r.j.ok){toast('删除失败：'+((r.j&&r.j.error)||r.status),'error');return}
  toast('通道已删除，正在重载');setTimeout(loadChannels,1400);setTimeout(loadChannels,2600);
}
$('#newSerial').onclick=()=>blankChannel('serial');
$('#newSsh').onclick=()=>blankChannel('ssh');
$('#newPowershell').onclick=()=>blankChannel('powershell');
$('#newTelnet').onclick=()=>blankChannel('telnet');
$('#loadChannels').onclick=loadChannels;
$('#saveChannel').onclick=saveChannel;
$('#cancelChannel').onclick=()=>{$('#chanEditor').hidden=true};
loadChannels();
)JS";
    }
    if (page == "agent") {
        return R"JS(
let AG_CFG=null, DEV_LAST=null;
const AG_HELP=[
 ['边界','18096','仅桌面窗口调试；18081 不调用、不代理、不下发该端口端点'],
 ['边界','关闭','18096 关闭不影响 18081、设备面、锁与作业'],
 ['边界','板控','Agent 只走 18081；不得用 18096 代做板控']
];
const DEV_HELP=[
 ['GET','/health','发现入口：版本、构建、18081/18096 边界'],
 ['GET','/api/help','机器契约；后续只调用 operations 公布的路径'],
 ['GET','/agent/scan','统一通道对象：运行态、终端探测、按钮、features'],
 ['GET','/agent/probe','被动探测终端类型；不写串口、不发 Enter'],
 ['POST','/lock','按通道获取/续租租约；写路由先过锁'],
 ['POST','/command','异步下发命令；202 + job_id，进度与输出走 /jobs'],
 ['GET','/jobs','作业列表；支持 state/channel/limit/before_id'],
 ['GET','/log','尾部/增量日志；增量需 since + epoch'],
 ['POST','/action','板控按钮；backend 可校验；返回 job 与审计 id'],
 ['GET','/tftp/files','TFTP 工件面：列表/上传/下载/MD5'],
 ['GET','/send_file/progress','最近一次上传/下载的 sent/total/state'],
 ['POST','/unlock','只释放自己的租约；人工接管走 /lock/takeover']
];
function agMetric(id,v){const e=$('#'+id);if(e)e.textContent=v}
function helpCards(box,rows,label){
  if(!box)return;box.innerHTML='';
  rows.forEach(row=>{const d=document.createElement('div');d.className='help-item';d.innerHTML='<code></code><div class="card-sub" style="margin-top:5px"></div><div class="tiny" style="margin-top:4px"></div>';d.querySelector('code').textContent=row[0]+' '+row[1];d.querySelector('.card-sub').textContent=row[2];d.querySelector('.tiny').textContent=label||'';box.appendChild(d)})
}
function agRenderHelp(){helpCards($('#agentHelp'),AG_HELP.map(r=>[r[0],r[1],r[2]]),'边界说明；18081 不提供 18096 控制入口')}
function devRenderHelp(ops){
  const wanted=['health.get','api.help.get','agent.scan.get','agent.probe.get','lock.post','command.post','jobs.list.get','log.get','action.post','tftp.files.get','sendfile.progress.get','unlock.post'];
  const byId={};(ops||[]).forEach(o=>byId[o.id]=o);
  const rows=DEV_HELP.map(r=>{
    const o=byId[wanted.find(id=>{const x=byId[id];return x&&x.path===r[1]})];
    return [r[0],r[1],r[2]+(o&&o.auth==='token'?' · 需要 Bearer token':'')];
  });
  helpCards($('#deviceHelp'),rows,'18081 唯一板控路径；18096 不参与');
}
function devCell(row,text,mono){
  const td=document.createElement('td');td.textContent=text==null?'':text;if(mono)td.className='mono';row.appendChild(td);return td
}
function devRenderChannels(scan,leases){
  const box=$('#deviceChannels');if(!box)return;box.innerHTML='';
  const list=(scan&&scan.channels)||[];
  if(!list.length){box.innerHTML='<div class="empty"><strong>没有通道</strong><p>先到通道管理新增。</p></div>';return}
  const lockBy={};(leases||[]).forEach(l=>lockBy[l.channel]=l);
  const table=document.createElement('table');table.className='agent-table';
  table.innerHTML='<thead><tr><th>通道</th><th>类型</th><th>运行 / 状态</th><th>终端探测</th><th>Agent 锁</th><th>按钮 / 工件</th></tr></thead>';
  const tb=document.createElement('tbody');
  list.forEach(c=>{
    const tr=document.createElement('tr');
    devCell(tr,c.name,true);
    devCell(tr,c.kind==='ssh'?'SSH':(c.kind==='powershell'?'PowerShell':(c.kind==='telnet'?'Telnet':'串口')));
    const st=devCell(tr,(c.running?'running':'offline')+' · '+(c.state||'-'));
    const term=(c.terminal&&c.terminal.state)||'-', conf=(c.terminal&&c.terminal.confidence)||'';
    devCell(tr,term+(conf?' / '+conf:''));
    const l=lockBy[c.name];
    const lock=devCell(tr,l?((l.owner||'?')+' · '+(Math.round((l.expires_in_ms||0)/1000))+'s'):'无锁');
    if(l)lock.style.color='#9a5b00';
    const btnCount=(c.buttons||[]).length;
    const f=c.features||{};
    devCell(tr,'按钮 '+btnCount+' · TFTP '+(f.tftp_artifacts?'on':'off')+' · 串口控制 '+(f.serial_control?'on':'off'));
    tb.appendChild(tr);
  });
  table.appendChild(tb);box.appendChild(table);
}
function devRenderLocks(scan){
  const box=$('#deviceLocks');if(!box)return;
  const leases=(scan&&scan.locks&&scan.locks.leases)||[];
  if(!leases.length){box.innerHTML='<div class="empty" style="padding:14px"><strong>无锁</strong><p>所有通道可写；长任务前建议先 POST /lock。</p></div>';return}
  const table=document.createElement('table');table.className='agent-table';
  table.innerHTML='<thead><tr><th>通道</th><th>owner</th><th>TTL</th><th>剩余</th></tr></thead>';
  const tb=document.createElement('tbody');
  leases.forEach(l=>{const tr=document.createElement('tr');devCell(tr,l.channel,true);devCell(tr,l.owner||'-');devCell(tr,Math.round((l.ttl_ms||0)/1000)+'s');devCell(tr,Math.round((l.expires_in_ms||0)/1000)+'s');tb.appendChild(tr)});
  table.appendChild(tb);box.appendChild(table);
}
async function devLoad(){
  const btn=$('#devRefresh');if(btn)setBusy(btn,true,'刷新中…');
  const [scan,jobsResp,tftp,help,health]=await Promise.all([api('/agent/scan'),api('/jobs?limit=100'),api('/tftp/status'),api('/api/help'),api('/health')]);
  const ok=scan.status===200&&help.status===200&&health.status===200;
  const s=scan.j||{}, jb=jobsResp.j||{}, tf=tftp.j||{}, hp=help.j||{}, hh=health.j||{};
  const channels=s.channels||[], leases=(s.locks&&s.locks.leases)||[];
  const online=channels.filter(c=>c.running).length;
  const jobs=(jb.jobs||[]), running=jobs.filter(j=>j.state==='running'||j.state==='queued').length;
  agMetric('devChannels',channels.length+' / '+online);
  agMetric('devJobs',running+' / '+(jb.count==null?jobs.length:jb.count));
  agMetric('devLocks',leases.length);
  agMetric('devTftp',tf.running?'运行 '+(tf.port||'')+' · '+(tf.root||''):'未启动 · 工件可用');
  agMetric('devOps',(hp.operations||[]).length);
  agMetric('devBoundary',(s.features&&s.features.tftp_artifacts)?'18081 全能力':'18081 基础');
  const chip=$('#deviceChip');if(chip){chip.textContent=ok?'设备面正常':'部分读取失败';chip.className='chip '+(ok?'on':'warn')}
  $('#devNote').textContent='协议 '+(s.protocol||hp.protocol||'-')+' · 服务 '+(hh.service||s.service||'-')+' · 18096 '+(hh.ui_bridge&&hh.ui_bridge.enabled?'启用':'关闭（不影响）');
  devRenderChannels(s,leases);devRenderLocks(s);devRenderHelp(hp.operations);
  DEV_LAST={scan:s,jobs:jb,tftp:tf,help:hp,health:hh};setBusy(btn,false);
}
async function agLoadCfg(){
  const r=await api('/api/agent/status');
  if(!r.j||!r.j.ok){
    $('#agentChip').textContent='需要 Web token';$('#agentChip').className='chip warn';
    $('#agNote').textContent='读取 18096 配置需要右上角 web token；设备面不受影响。';return
  }
  AG_CFG=r.j;const ui=r.j.ui_bridge||r.j;
  agMetric('agEnabled',ui.enabled?'启用':'关闭');
  agMetric('agPort',ui.port||r.j.port||'--');
  agMetric('agToken',ui.token_required?'需要 UI token':'无 token');
  agMetric('agBoundary','仅桌面调试');
  $('#agentChip').textContent=ui.enabled?'已启用':'未启用（不影响设备面）';
  $('#agentChip').className='chip '+(ui.enabled?'on':'off');
  $('#agNote').textContent='端口 '+(ui.port||r.j.port)+' · 18081 不探测、不代理；桌面窗口自检请在本机完成。';
}
$('#devRefresh').onclick=devLoad;
agRenderHelp();devLoad();agLoadCfg();
)JS";
    }
    if (page == "config") {
        return R"JS(
let CFG=null,TAB='basic';
const BASIC=['default','web','agent','inbound_ssh','ui','boards','ssh'];
const LABELS={enable:'启用',port:'端口',token:'Token',name:'名称',serial_device:'串口设备',
  baudrate:'波特率',auto_open:'自动打开',password:'密码',username:'用户名',target_password:'目标密码',
  target_host:'目标主机',target_port:'目标端口',target_username:'目标用户',keepalive:'Keepalive',buffer_size:'缓冲区',
  log_enabled:'写日志',close_to_tray:'关闭到托盘',start_minimized:'启动最小化',window_w:'窗口宽',window_h:'窗口高',font_px:'字号',
  split:'分屏',panes:'画框',pane_channels:'画框通道',active_pane:'激活画框',channel:'通道',line_ending:'行尾',encoding:'编码',
  databits:'数据位',parity:'校验',stopbits_x10:'停止位',flowcontrol:'流控',dtr:'DTR',rts:'RTS',timestamp:'时间戳',log_dir:'日志目录',
  root:'根目录',alias:'别名',http_port:'HTTP 端口',local:'本机',remote:'远端',bind_ip:'绑定 IP',allow_write:'允许写入',work_dir:'工作目录',
  relay_devices:'继电器设备',windows_port:'Windows 端口',linux_port:'Linux 端口',channels:'通道数',io_inverted:'IO 反相',
  post_write_delay_ms:'写后延时',polarity_inverted:'极性强反',quick_commands:'快捷指令',buttons:'板控按钮',backend:'类型',
  run:'执行位置',commands:'命令',delay_ms:'间隔 ms',relay:'继电器',mode:'模式',hold_seconds:'保持秒',script:'脚本',
  map_http:'HTTP 映射',map_inbound_ssh:'入站 SSH 映射',exe_path:'执行文件',server_addr:'服务器地址',server_port:'服务器端口',
  port_base:'端口基数',board_cidr:'板卡 CIDR',listen_ip:'监听 IP',local_port:'本地端口',remote_port:'远端端口',
  ssh_local_port:'SSH 本地端口',ssh_remote_port:'SSH 远端端口'};
function labelOf(k){return LABELS[k]||k}
function isObj(v){return v&&typeof v==='object'&&!Array.isArray(v)}
function deep(v){return JSON.parse(JSON.stringify(v))}
function isSecret(k){return /token|password/i.test(k)}
function escAttr(s){return esc(s).replace(/`/g,'')}
function inputField(p,val,secret){
  const type=secret?'password':'text';
  const input='<input type="'+type+'" data-path="'+p+'" data-kind="str" value="'+escAttr(val)+'" style="flex:1" autocomplete="off">';
  return secret?'<div class="secret-wrap">'+input+'<button class="btn ghost" type="button" data-eye="1" aria-label="显示或隐藏">显示</button></div>':input;
}
function field(path,key,val){
  const p=escAttr(path.join('.')), lab=labelOf(key);
  if(typeof val==='boolean'){
    return '<label class="field"><span>'+esc(lab)+'</span><select data-path="'+p+'" data-kind="bool">'+
      '<option value="true"'+(val?' selected':'')+'>true</option><option value="false"'+(val?'':' selected')+'>false</option></select></label>';
  }
  if(typeof val==='number'){
    return '<label class="field"><span>'+esc(lab)+'</span><input type="number" step="any" data-path="'+p+'" data-kind="num" value="'+escAttr(val)+'"></label>';
  }
  if(val===null)return '';
  const s=String(val==null?'':val);
  if(s.length>100)return '<label class="field full"><span>'+esc(lab)+'</span><textarea data-path="'+p+'">'+esc(s)+'</textarea></label>';
  return '<label class="field"><span>'+esc(lab)+'</span>'+inputField(p,s,isSecret(key))+'</label>';
}
function node(path,key,val){
  if(Array.isArray(val)){
    let h='<div class="item"><div class="item-head"><b>'+esc(labelOf(key))+' · '+val.length+'</b></div>';
    val.forEach((v,i)=>{h+='<div style="border-left:3px solid var(--line);padding-left:10px;margin:8px 0">'+
      '<div class="row" style="justify-content:space-between;margin-bottom:5px"><span class="tiny">#'+(i+1)+'</span>'+
      '<button class="btn danger small" type="button" data-del="'+escAttr(path.concat(i).join('.'))+'">删除</button></div>'+node(path.concat(i),'#'+(i+1),v)+'</div>'});
    if(val.length&&isObj(val[0]))h+='<button class="btn ghost small" type="button" data-add="'+escAttr(path.join('.'))+'">+ 新增一项</button>';
    return h+'</div>';
  }
  if(isObj(val)){
    let h='<fieldset class="cfg-group"><legend>'+esc(labelOf(key))+'</legend><div class="form-grid">';
    for(const k of Object.keys(val))h+=node(path.concat(k),k,val[k]);
    return h+'</div></fieldset>';
  }
  return field(path,key,val);
}
function render(){
  const box=$('#cfgForm'); if(!CFG){box.innerHTML='<div class="empty"><div class="empty-mark">i</div><strong>还没有读取配置</strong><p>先保存 token，再点“读取配置”。</p></div>';return}
  const keys=TAB==='basic'?BASIC.filter(k=>k in CFG):Object.keys(CFG);
  const q=($('#cfgSearch').value||'').trim().toLowerCase();
  box.innerHTML=keys.map(k=>node([k],k,CFG[k])).join('')||'<div class="empty"><strong>没有字段</strong><p>当前 Tab 没有可编辑字段。</p></div>';
  const isVisible=el=>el&&el.style.display!=='none';
  box.querySelectorAll('[data-path]').forEach(el=>{if(q&&!el.dataset.path.toLowerCase().includes(q)&&!((el.closest('.field')||{}).innerText||'').toLowerCase().includes(q))(el.closest('.field')||el).style.display='none'});
  $$('#cfgForm .cfg-group').forEach(g=>{
    const any=Array.from(g.querySelectorAll('[data-path]')).some(e=>{const f=e.closest('.field');return f&&f.style.display!=='none'});
    if(!any)g.style.display='none';
  });
  $$('#cfgForm .item').forEach(it=>{
    const any=Array.from(it.querySelectorAll('[data-path]')).some(e=>{const f=e.closest('.field');return f&&f.style.display!=='none'});
    if(!any)it.style.display='none';
  });
  $$('[data-add]').forEach(b=>b.onclick=()=>{const p=b.dataset.add.split('.');let a=CFG;for(const k of p)a=a[k];const t=a.length?deep(a[0]):{};a.push(t);render()});
  $$('[data-del]').forEach(b=>b.onclick=async()=>{const p=b.dataset.del.split('.');let a=CFG;for(let i=0;i<p.length-1;i++)a=a[Number(p[i])];const idx=Number(p[p.length-1]);const ok=await askConfirm('删除配置项','确定删除数组中的这一项吗？保存后才会写入磁盘。','删除',true);if(ok){a.splice(idx,1);render()}});
  $$('[data-eye]').forEach(b=>b.onclick=()=>{const inp=b.parentElement.querySelector('input');if(!inp)return;const show=inp.type==='password';inp.type=show?'text':'password';b.textContent=show?'隐藏':'显示'});
}
async function cfgLoad(){
  const r=await api('/api/config');
  if(!r.r.ok){$('#cfgNote').textContent='读取失败 '+r.status+'：需要 Bearer token';toast('配置读取失败，请先保存 token','error');return}
  try{CFG=JSON.parse(r.t);$('#cfgNote').textContent='已读取 '+Object.keys(CFG).length+' 个顶层段';render();toast('配置已读取')}
  catch(e){$('#cfgNote').textContent='JSON 解析失败';toast('配置 JSON 解析失败','error')}
}
function collect(){
  const root=deep(CFG||{});
  $$('#cfgForm [data-path]').forEach(el=>{const parts=el.dataset.path.split('.');let o=root;for(let i=0;i<parts.length-1;i++)o=o[parts[i]];const k=parts[parts.length-1];let v=el.value;if(el.dataset.kind==='bool')v=(v==='true');else if(el.dataset.kind==='num')v=Number(v);o[k]=v});
  return root;
}
$('#tabBasic').onclick=()=>{TAB='basic';$('#tabBasic').classList.add('on');$('#tabDetail').classList.remove('on');render()};
$('#tabDetail').onclick=()=>{TAB='detail';$('#tabDetail').classList.add('on');$('#tabBasic').classList.remove('on');render()};
$('#loadCfg').onclick=cfgLoad;
$('#cfgSearch').oninput=render;
$('#saveCfg').onclick=async()=>{
  if(!CFG){toast('先读取配置','error');return}
  const ok=await askConfirm('保存配置','保存后会立即重载 UI 配置，可能刷新通道和服务状态。继续吗？','保存并重载',true);if(!ok)return;
  const r=await api('/api/config/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(collect())});
  if(!r.r.ok){$('#cfgNote').textContent='保存失败 '+r.status+'：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  $('#cfgNote').textContent='已保存，UI 线程正在重载';toast('配置已保存');
  setTimeout(cfgLoad,1400);setTimeout(cfgLoad,2400);
};
)JS";
    }
    if (page == "actions") {
        return R"JS(
let ACTIONS=[], ACT_EDIT=null;
function channelOptions(sel,includeAll){
  if(!sel)return;const old=sel.value;sel.innerHTML='';
  if(includeAll){const o=document.createElement('option');o.value='';o.textContent='全部通道';sel.appendChild(o)}
  const st=LAST_STATUS;if(st&&st.channels)st.channels.forEach(c=>{const o=document.createElement('option');o.value=c.name;o.textContent=c.name+' · '+c.kind;sel.appendChild(o)});
  if(old&&Array.from(sel.options).some(o=>o.value===old))sel.value=old;
}
let LAST_STATUS=null;
async function loadStatus(){const r=await api('/status');if(r.j)LAST_STATUS=r.j}
function actionMatches(a){const f=$('#actFilter').value;return !f||a.channel===f}
function fillRelayOptions(selected){
  const sel=$('#aRelay');if(!sel)return;sel.innerHTML='<option value="">（无设备）</option>';
  (window.__relayNames||[]).forEach(n=>{const o=document.createElement('option');o.value=n;o.textContent=n;if(n===selected)o.selected=true;sel.appendChild(o)});
}
async function loadRelayNames(){
  const r=await api('/api/relay/devices');
  window.__relayNames=(r.j&&r.j.items||[]).map(d=>d.name);
  fillRelayOptions($('#aRelay').value);
}
function syncActionFields(){
  const be=$('#aBackend').value;
  $$('.only-commands').forEach(e=>e.style.display=be==='commands'?'':'none');
  $$('.only-relay').forEach(e=>e.style.display=be==='relay'?'':'none');
  $$('.only-script').forEach(e=>e.style.display=be==='script'?'':'none');
  $('#aRun').disabled=be==='relay'||be==='script';
}
function emptyState(title,text,btn,fn){
  const d=document.createElement('div');d.className='empty';d.innerHTML='<div class="empty-mark">+</div><strong>'+esc(title)+'</strong><p>'+esc(text)+'</p>';
  if(btn){const b=document.createElement('button');b.className='btn';b.type='button';b.textContent=btn;b.onclick=fn;d.appendChild(b)}
  return d;
}
function renderActions(){
  const box=$('#actList');box.innerHTML='';
  const items=ACTIONS.filter(actionMatches);
  if(!items.length){box.appendChild(emptyState('还没有板控按钮','新建一个 commands / relay / script 按钮，即可在 Web 和原生按钮条执行。','新增按钮',()=>openActionEditor()));return}
  const groups={};items.forEach(a=>{(groups[a.channel]=groups[a.channel]||[]).push(a)});
  Object.keys(groups).forEach(ch=>{
    const sec=document.createElement('div');sec.style.marginBottom='12px';
    const h=document.createElement('div');h.className='row';h.style.margin='6px 0 8px';h.innerHTML='<span class="chip accent">'+esc(ch)+'</span><span class="tiny">'+groups[ch].length+' 个按钮</span>';
    sec.appendChild(h);
    groups[ch].forEach(a=>{
      const d=document.createElement('div');d.className='item';
      d.innerHTML='<div class="item-head"><div><b>'+esc(a.label||a.id)+'</b><div class="meta">'+esc(a.id)+' · '+esc(a.kind||a.backend||'')+(a.enable?'':' · 已停用')+'</div></div><div class="actions"></div></div>';
      if(a.backend==='commands'&&a.commands&&a.commands.length)d.querySelector('.meta').textContent+=' · '+a.commands.length+' 条命令';
      if(a.backend==='script'&&a.script)d.querySelector('.meta').textContent+=' · 脚本 '+a.script.length+' 字符';
      const ex=document.createElement('button');ex.className='btn small';ex.textContent='执行';ex.onclick=()=>runAction(a,d);d.querySelector('.actions').appendChild(ex);
      const ed=document.createElement('button');ed.className='btn ghost small';ed.textContent='编辑';ed.onclick=()=>openActionEditor(a);d.querySelector('.actions').appendChild(ed);
      const del=document.createElement('button');del.className='btn danger small';del.textContent='删除';del.onclick=()=>deleteAction(a);d.querySelector('.actions').appendChild(del);
      sec.appendChild(d);
    });
    box.appendChild(sec);
  });
}
async function loadActions(){
  const s=await loadStatus();const r=await api('/api/actions');
  if(!r.r.ok||!r.j){toast('按钮读取失败','error');return}
  ACTIONS=r.j.items||[];channelOptions($('#actFilter'),true);channelOptions($('#aChannel'),false);
  await loadRelayNames();
  renderActions();
}
function resetForm(a){
  ACT_EDIT=a||null;
  $('#actionEditor').hidden=false;$('#actionNote').textContent='';
  $('#aChannel').value=a?a.channel:($('#actFilter').value||(LAST_STATUS&&LAST_STATUS.channels[0]&&LAST_STATUS.channels[0].name)||'');
  $('#aId').value=a?a.id:'';$('#aLabel').value=a?a.label||'':'';$('#aBackend').value=a?(a.backend||a.kind||'commands'):'commands';
  $('#aRun').value=a?a.run||'channel':'channel';$('#aEnable').value=String(a?a.enable!==false:true);
  $('#aCommands').value=a&&a.commands?a.commands.join('\n'):'';$('#aDelay').value=a?a.delay_ms||200:200;
  $('#aRelay').value=a?a.relay||'':'';$('#aRelayCh').value=a?(a.relay_channel||1):1;$('#aMode').value=a?a.mode||'pulse':'pulse';$('#aHold').value=a?a.hold_seconds||1:1;
  $('#aScript').value=a?a.script||'':'';syncActionFields();
}
function openActionEditor(a){resetForm(a);$('#actionEditor').scrollIntoView({behavior:'smooth',block:'start'});setTimeout(()=>$('#aLabel').focus(),120)}
function closeActionEditor(){$('#actionEditor').hidden=true;ACT_EDIT=null}
async function saveAction(){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const channel=$('#aChannel').value,id=$('#aId').value.trim(),label=$('#aLabel').value.trim(),backend=$('#aBackend').value;
  if(!channel||!id||!label){$('#actionNote').textContent='通道、ID、按钮文字都不能为空';toast('请补齐必填字段','error');return}
  if(!/^[A-Za-z0-9_.-]{1,64}$/.test(id)){$('#actionNote').textContent='ID 只允许字母、数字、点、下划线、短横线';toast('ID 格式不正确','error');return}
  const action={channel,id,label,backend,enable:$('#aEnable').value==='true',run:$('#aRun').value};
  if(backend==='commands'){action.commands=$('#aCommands').value.split(/\r?\n/).map(x=>x.trim()).filter(Boolean);action.delay_ms=Number($('#aDelay').value||200);if(!action.commands.length){toast('commands 至少一条命令','error');return}}
  if(backend==='script'){action.script=$('#aScript').value.trim();if(!action.script){toast('script 不能为空','error');return}}
  if(backend==='relay'){action.relay=$('#aRelay').value;action.relay_channel=Number($('#aRelayCh').value||1);action.mode=$('#aMode').value;action.hold_seconds=Number($('#aHold').value||1);if(!action.relay){toast('先选择继电器设备','error');return}}
  const r=await api('/api/actions',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel,old_id:ACT_EDIT?ACT_EDIT.id:'',action})});
  if(!r.r.ok){$('#actionNote').textContent='保存失败：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  toast('按钮已保存，配置正在重载');closeActionEditor();setTimeout(loadActions,1300);setTimeout(loadActions,2400);
}
async function deleteAction(a){
  const ok=await askConfirm('删除按钮','确定删除“'+a.label+'”（'+a.id+'）吗？','删除',true);if(!ok)return;
  const r=await api('/api/actions/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:a.channel,id:a.id})});
  if(!r.r.ok){toast('删除失败：'+(r.j&&r.j.error||r.t),'error');return}
  toast('按钮已删除');setTimeout(loadActions,1300);setTimeout(loadActions,2400);
}
async function runAction(a,d){
  const meta=d.querySelector('.meta');meta.textContent='已提交，等待执行…';
  const r=await api('/action',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:a.channel,id:a.id})});
  if(!r.r.ok){meta.textContent='失败 '+r.status+' '+(r.j&&r.j.error||r.t);toast('执行失败','error');return}
  const id=r.j&&r.j.job;if(!id){meta.textContent='已提交';return}
  for(let i=0;i<40;i++){await new Promise(x=>setTimeout(x,350));const rr=await api('/job?id='+id);if(rr.j&&rr.j.done){meta.textContent=(rr.j.ok?'OK · ':'FAIL · ')+(rr.j.message||'')+(rr.j.output?' · '+String(rr.j.output).slice(0,160):'');toast(rr.j.ok?'执行成功':'执行失败',rr.j.ok?'ok':'error');return}}
  meta.textContent='执行超时，请查看原生状态';
}
$('#newAction').onclick=()=>openActionEditor();
$('#cancelAction').onclick=closeActionEditor;
$('#saveAction').onclick=saveAction;
$('#aBackend').onchange=syncActionFields;
$('#actFilter').onchange=renderActions;
loadStatus().then(()=>loadActions());
)JS";
    }
    if (page == "button-script") {
        return R"JS(
let QUICK=[], QUICK_EDIT=null, LAST_STATUS=null;
async function loadQuickStatus(){const r=await api('/status');if(r.j)LAST_STATUS=r.j}
function quickChannelOptions(){
  const sel=$('#quickChannel');if(!sel)return;const old=sel.value;sel.innerHTML='';
  (LAST_STATUS&&LAST_STATUS.channels||[]).forEach(c=>{const o=document.createElement('option');o.value=c.name;o.textContent=c.name;sel.appendChild(o)});
  if(old&&Array.from(sel.options).some(o=>o.value===old))sel.value=old;
}
function renderQuick(){
  const box=$('#quickList');box.innerHTML='';const ch=$('#quickChannel').value;
  const items=QUICK.filter(q=>q.channel===ch);
  if(!items.length){const d=document.createElement('div');d.className='empty';d.innerHTML='<div class="empty-mark">+</div><strong>这个通道还没有快捷指令</strong><p>新增一条命令，它会出现在原生窗口底部快捷条。</p><button class="btn" type="button">新增第一条</button>';d.querySelector('button').onclick=()=>openQuickEditor();box.appendChild(d);return}
  items.forEach(q=>{
    const d=document.createElement('div');d.className='item';
    d.innerHTML='<div class="item-head"><div><b>'+esc(q.label)+'</b><div class="meta mono"></div></div><div class="actions"></div></div>';
    d.querySelector('.meta').textContent=q.command;
    const send=document.createElement('button');send.className='btn small';send.textContent='发送';send.onclick=async()=>{const r=await api('/quick',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:q.channel,command:q.command})});if(!r.r.ok)toast('发送失败：'+(r.j&&r.j.error||r.t),'error');else toast('已发送到 '+q.channel)};d.querySelector('.actions').appendChild(send);
    const ed=document.createElement('button');ed.className='btn ghost small';ed.textContent='编辑';ed.onclick=()=>openQuickEditor(q);d.querySelector('.actions').appendChild(ed);
    const del=document.createElement('button');del.className='btn danger small';del.textContent='删除';del.onclick=()=>deleteQuick(q);d.querySelector('.actions').appendChild(del);
    box.appendChild(d);
  });
}
async function loadQuick(){
  const r=await api('/api/quick/commands');if(!r.r.ok||!r.j){toast('快捷指令读取失败','error');return}
  QUICK=r.j.items||[];await loadQuickStatus();quickChannelOptions();if(!$('#quickChannel').value&&LAST_STATUS&&LAST_STATUS.channels[0])$('#quickChannel').value=LAST_STATUS.channels[0].name;renderQuick();
}
function openQuickEditor(q){QUICK_EDIT=q||null;$('#quickEditor').hidden=false;$('#qLabel').value=q?q.label:'';$('#qCommand').value=q?q.command:'';$('#quickNote').textContent='';setTimeout(()=>$('#qLabel').focus(),80)}
function closeQuickEditor(){$('#quickEditor').hidden=true;QUICK_EDIT=null}
async function saveQuick(){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const channel=$('#quickChannel').value,label=$('#qLabel').value.trim(),command=$('#qCommand').value.trim();
  if(!channel||!label||!command){$('#quickNote').textContent='通道、按钮文字、命令都不能为空';toast('请补齐必填字段','error');return}
  const r=await api('/api/quick/commands',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel,index:QUICK_EDIT?QUICK_EDIT.index:null,label,command})});
  if(!r.r.ok){$('#quickNote').textContent='保存失败：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  toast('快捷指令已保存');closeQuickEditor();setTimeout(loadQuick,1300);setTimeout(loadQuick,2400);
}
async function deleteQuick(q){
  const ok=await askConfirm('删除快捷指令','确定删除“'+q.label+'”吗？','删除',true);if(!ok)return;
  const r=await api('/api/quick/commands',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({channel:q.channel,index:q.index,op:'delete'})});
  if(!r.r.ok){toast('删除失败：'+(r.j&&r.j.error||r.t),'error');return}
  toast('快捷指令已删除');setTimeout(loadQuick,1300);setTimeout(loadQuick,2400);
}
$('#quickChannel').onchange=renderQuick;
$('#newQuick').onclick=()=>openQuickEditor();
$('#cancelQuick').onclick=closeQuickEditor;
$('#saveQuick').onclick=saveQuick;
loadQuick();
)JS";
    }
    if (page == "relay") {
        return R"JS(
let RELAYS=[], RELAY_EDIT=null, RELAY_STATUS=null;
function relayState(d){return Array.isArray(d.state)?d.state:[]}
function renderRelays(){
  const box=$('#relayList');box.innerHTML='';
  if(!RELAYS.length){const d=document.createElement('div');d.className='empty';d.innerHTML='<div class="empty-mark">+</div><strong>还没有继电器设备</strong><p>新增一个设备后，可以配置端口和通道并做 ON/OFF/脉冲测试。</p><button class="btn" type="button">新增设备</button>';d.querySelector('button').onclick=()=>openRelayEditor();box.appendChild(d);return}
  RELAYS.forEach(d=>{
    const card=document.createElement('div');card.className='item';
    card.innerHTML='<div class="item-head"><div><b>'+esc(d.name)+'</b><div class="meta">'+esc(d.port||d.windows_port||'未配置端口')+' · '+esc(d.channels||2)+' 通道'+(d.enable===false?' · 未启用':'')+'</div></div><div class="actions"></div></div>';
    const ed=document.createElement('button');ed.className='btn ghost small';ed.textContent='编辑';ed.onclick=()=>openRelayEditor(d);card.querySelector('.actions').appendChild(ed);
    const del=document.createElement('button');del.className='btn danger small';del.textContent='删除';del.onclick=()=>deleteRelay(d);card.querySelector('.actions').appendChild(del);
    const chWrap=document.createElement('div');chWrap.className='grid';chWrap.style.marginTop='10px';
    relayState(d).forEach(st=>{
      const c=document.createElement('div');c.className='status-cell';
      c.innerHTML='<div class="row" style="justify-content:space-between"><span class="k">CH'+st.channel+'</span><span class="chip '+(st.on?'on':'off')+'">'+(st.on?'ON':'OFF')+'</span></div><div class="v"></div>';
      c.querySelector('.v').textContent=st.error?('错误：'+st.error):(st.busy?'执行中…':('模式 '+st.mode||'idle'));
      const row=document.createElement('div');row.className='row';row.style.marginTop='7px';
      const on=document.createElement('button');on.className='btn small';on.textContent='ON';on.onclick=()=>relayOp(d,st.channel,'on',c);
      const off=document.createElement('button');off.className='btn ghost small';off.textContent='OFF';off.onclick=()=>relayOp(d,st.channel,'off',c);
      const pulse=document.createElement('button');pulse.className='btn ghost small';pulse.textContent='脉冲';pulse.onclick=()=>relayOp(d,st.channel,'pulse',c);
      row.appendChild(on);row.appendChild(off);row.appendChild(pulse);c.appendChild(row);chWrap.appendChild(c);
    });
    if(!relayState(d).length)chWrap.innerHTML='<div class="muted">没有通道状态；先确认设备和端口。</div>';
    card.appendChild(chWrap);box.appendChild(card);
  });
}
async function loadRelay(){
  const r=await api('/api/relay/devices');if(!r.r.ok||!r.j){toast('继电器读取失败','error');return}
  RELAYS=r.j.items||[];renderRelays();
}
function openRelayEditor(d){
  RELAY_EDIT=d||null;$('#relayEditor').hidden=false;$('#relayNote').textContent='';
  $('#rName').value=d?d.name:'';$('#rPort').value=d?d.port||d.windows_port||'':'';$('#rChannels').value=d?d.channels||2:2;$('#rBaud').value=d?d.baudrate||9600:9600;
  $('#rEnable').value=String(d?d.enable!==false:true);$('#rIoInv').value=String(d?!!d.io_inverted:false);$('#rPolInv').value=String(d?!!d.polarity_inverted:false);$('#rDelay').value=d?d.post_write_delay_ms||100:100;
  setTimeout(()=>$('#rName').focus(),80);
}
function closeRelayEditor(){$('#relayEditor').hidden=true;RELAY_EDIT=null}
async function saveRelay(){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const device={name:$('#rName').value.trim(),windows_port:$('#rPort').value.trim(),channels:Number($('#rChannels').value||2),baudrate:Number($('#rBaud').value||9600),enable:$('#rEnable').value==='true',io_inverted:$('#rIoInv').value==='true',polarity_inverted:$('#rPolInv').value==='true',post_write_delay_ms:Number($('#rDelay').value||100)};
  if(!device.name||!device.windows_port){$('#relayNote').textContent='设备名称和端口不能为空';toast('请补齐必填字段','error');return}
  const r=await api('/api/relay/devices',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({old_name:RELAY_EDIT?RELAY_EDIT.name:'',device})});
  if(!r.r.ok){$('#relayNote').textContent='保存失败：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  toast('继电器设备已保存');closeRelayEditor();setTimeout(loadRelay,1300);setTimeout(loadRelay,2400);
}
async function deleteRelay(d){
  const ok=await askConfirm('删除继电器','确定删除“'+d.name+'”吗？如果被板控按钮引用，会被拒绝。','删除',true);if(!ok)return;
  const r=await api('/api/relay/devices/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:d.name})});
  if(!r.r.ok){toast('删除失败：'+(r.j&&r.j.error||r.t),'error');return}
  toast('设备已删除');setTimeout(loadRelay,1300);setTimeout(loadRelay,2400);
}
async function relayOp(d,ch,op,cell){
  const meta=cell.querySelector('.v');meta.textContent='正在提交 '+op+'…';
  const r=await api('/api/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:d.name,channel:ch,op})});
  if(!r.r.ok){meta.textContent='失败：'+(r.j&&r.j.error||r.t);toast('继电器操作失败','error');return}
  toast('CH'+ch+' '+op+' 已提交');setTimeout(loadRelay,1300);setTimeout(loadRelay,2400);
}
$('#loadRelay').onclick=loadRelay;$('#newRelay').onclick=()=>openRelayEditor();$('#cancelRelay').onclick=closeRelayEditor;$('#saveRelay').onclick=saveRelay;loadRelay();
)JS";
    }
    if (page == "tftp" || page == "nfs" || page == "share") {
        return R"JS(
const SVC=location.pathname.replace(/^\//,'')||'share';
const SVC_LABEL={tftp:'TFTP',nfs:'NFS',share:'共享'};
function svcRender(st){
  const box=$('#svcBox'),s=(st.services||st)[SVC]||{};
  if(!box)return;
  const cells=[['状态',s.on?'运行中':'已停止'],['端口',s.port||''],['根目录',s.root||''],['说明',s.note||s.error||'']];
  if(s.alias)cells.splice(2,0,['别名',s.alias]);
  if(s.url)cells.push(['访问地址',s.url]);
  box.innerHTML='';
  cells.forEach(([k,v])=>{const c=document.createElement('div');c.className='status-cell';c.innerHTML='<div class="k">'+esc(k)+'</div><div class="v"></div>';c.querySelector('.v').textContent=v;box.appendChild(c)});
  const chip=$('#svcState');if(chip){chip.textContent=s.on?'运行中':'已停止';chip.className='chip '+(s.on?'on':'off')}
}
async function svcLoad(){
  const r=await api('/services');if(!r.j){toast('服务状态读取失败','error');return}
  svcRender(r.j);const s=(r.j.services||r.j)[SVC]||{};
  const st=$('#svcHint');if(st)st.textContent=s.error?('错误：'+s.error):(s.on?'服务运行中':'服务已停止');
}
function svcCfgApply(c){
  c=c||{};const en=$('#svcEnable');if(en)en.value=String(!!c.enable);
  const root=$('#svcRoot');if(root)root.value=c.root||'';
  if($('#svcPort'))$('#svcPort').value=c.port||(SVC==='tftp'?69:18082);
  if($('#svcBind'))$('#svcBind').value=c.bind_ip||'';
  if($('#svcWrite'))$('#svcWrite').checked=c.allow_write!==false;
  if($('#svcAlias'))$('#svcAlias').value=c.alias||'/share';
  if($('#svcUid'))$('#svcUid').value=c.uid==null?0:c.uid;
  if($('#svcGid'))$('#svcGid').value=c.gid==null?0:c.gid;
  const chip=$('#svcCfgState');if(chip){chip.textContent=c.enable?'配置启用':'配置关闭';chip.className='chip '+(c.enable?'on':'off')}
}
async function svcCfgLoad(){
  const r=await api('/api/'+SVC+'/config');
  if(!r.j||!r.j.ok){$('#svcCfgNote').textContent='读取失败：'+(r.j&&r.j.error||r.t);return}
  svcCfgApply(r.j.config||r.j);$('#svcCfgNote').textContent='配置已读取；保存后由 UI 线程重载。';
}
async function svcCfgSave(){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const btn=$('#saveSvcCfg');setBusy(btn,true,'保存中…');
  const cfg={enable:$('#svcEnable').value==='true',root:$('#svcRoot').value.trim()};
  if($('#svcPort'))cfg.port=Number($('#svcPort').value||(SVC==='tftp'?69:18082));
  if($('#svcBind'))cfg.bind_ip=$('#svcBind').value.trim();
  if($('#svcWrite'))cfg.allow_write=$('#svcWrite').checked;
  if($('#svcAlias'))cfg.alias=$('#svcAlias').value.trim();
  if($('#svcUid'))cfg.uid=Number($('#svcUid').value||0);
  if($('#svcGid'))cfg.gid=Number($('#svcGid').value||0);
  const r=await api('/api/'+SVC+'/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});
  setBusy(btn,false);
  if(!r.j||!r.j.ok){$('#svcCfgNote').textContent='保存失败：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  $('#svcCfgNote').textContent='已保存，配置正在重载';toast(SVC_LABEL[SVC]+' 配置已保存');setTimeout(()=>{svcCfgLoad();svcLoad()},1400);setTimeout(()=>{svcCfgLoad();svcLoad()},2600);
}
async function svcSet(on){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const note=$('#svcHint');if(note)note.textContent=on?'正在启动…':'正在停止…';
  const r=await api('/service',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:SVC,on})});
  if(!r.r.ok){if(note)note.textContent='失败：'+(r.j&&r.j.error||r.t);toast('服务操作失败','error');return}
  toast(SVC_LABEL[SVC]+(on?' 已启动':' 已停止'));svcLoad();setTimeout(svcLoad,700);
}
$('#loadSvc').onclick=svcLoad;$('#startSvc').onclick=()=>svcSet(true);$('#stopSvc').onclick=()=>svcSet(false);
$('#saveSvcCfg').onclick=svcCfgSave;
svcLoad();svcCfgLoad();
)JS" + std::string(kFileBrowserJs) + "\nfileInit();";
    }
    if (page == "tcp-forward") {
        return R"JS(
let TCP_STATUS=null;
function tcpRenderConfig(c){
  $('#tEnable').value=String(!!c.enable);$('#tListen').value=c.listen_ip||'';$('#tPortBase').value=c.port_base||22000;$('#tTarget').value=c.target_port||22;$('#tCidr').value=c.board_cidr||'';
}
function tcpRenderStatus(st){
  const box=$('#tcpStatus');if(!box)return;box.innerHTML='';
  const cells=[['运行中',st.on?'是':'否'],['监听数',st.listen_count||0],['活动连接',st.active||0],['累计接受',st.accepted||0],['累计字节',fmtBytes(st.bytes||0),'wide'],['错误',st.error||'无','wide']];
  cells.forEach(([k,v,wide])=>{const c=document.createElement('div');c.className='status-cell'+(wide?' wide':'');c.innerHTML='<div class="k">'+esc(k)+'</div><div class="v"></div>';c.querySelector('.v').textContent=v;box.appendChild(c)});
  const chip=$('#tcpState');if(chip){chip.textContent=st.on?'运行中':'已停止';chip.className='chip '+(st.on?'on':'off')}
}
async function tcpLoad(){
  const c=await api('/api/tcp-forward/config');if(c.j)tcpRenderConfig(c.j.config||c.j);
  const s=await api('/api/tcp-forward');if(s.j){TCP_STATUS=s.j.status||{};tcpRenderStatus(TCP_STATUS)}
}
async function tcpSave(){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const cfg={enable:$('#tEnable').value==='true',listen_ip:$('#tListen').value.trim(),port_base:Number($('#tPortBase').value||22000),target_port:Number($('#tTarget').value||22),board_cidr:$('#tCidr').value.trim()};
  const r=await api('/api/tcp-forward/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});
  if(!r.r.ok){$('#tcpNote').textContent='保存失败：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  $('#tcpNote').textContent='已保存，配置正在重载';toast('TCP 配置已保存');setTimeout(tcpLoad,1400);setTimeout(tcpLoad,2400);
}
async function tcpSvc(on){if(!tok()){toast('写操作需要 Bearer token','error');return}const r=await api('/service',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:'tcp-forward',on})});if(!r.r.ok){toast('服务操作失败：'+(r.j&&r.j.error||r.t),'error');return}toast(on?'TCP 转发已启动':'TCP 转发已停止');setTimeout(tcpLoad,700)}
$('#saveTcp').onclick=tcpSave;$('#startTcp').onclick=()=>tcpSvc(true);$('#stopTcp').onclick=()=>tcpSvc(false);tcpLoad();
)JS";
    }
    if (page == "tunnel") {
        return R"JS(
function tunRenderConfig(c){
  $('#uEnable').value=String(!!c.enable);$('#uExe').value=c.exe_path||'';$('#uServer').value=c.server_addr||'';$('#uPort').value=c.server_port||7000;$('#uToken').value=c.token||'';$('#uHttp').value=String(c.map_http!==false);$('#uSsh').value=String(!!c.map_inbound_ssh);$('#uLocal').value=c.local_port||18081;$('#uRemote').value=c.remote_port||0;$('#uSshLocal').value=c.ssh_local_port||2222;$('#uSshRemote').value=c.ssh_remote_port||0;
}
function tunRenderStatus(st){
  const box=$('#tunStatus');if(!box)return;box.innerHTML='';
  const cells=[['总状态',st.on?'运行中':'已停止'],['健康',st.healthy?'健康':'未健康'],['HTTP 映射',st.http&&st.http.on?'ON':'OFF'],['HTTP 端口',st.http?((st.http.local_port||'-')+' → '+(st.http.remote_port||'-')):'-'],['SSH 映射',st.ssh&&st.ssh.on?'ON':'OFF'],['SSH 端口',st.ssh?((st.ssh.local_port||'-')+' → '+(st.ssh.remote_port||'-')):'-'],['最近错误',st.last_error||'无','wide']];
  cells.forEach(([k,v,wide])=>{const c=document.createElement('div');c.className='status-cell'+(wide?' wide':'');c.innerHTML='<div class="k">'+esc(k)+'</div><div class="v"></div>';c.querySelector('.v').textContent=v;box.appendChild(c)});
  const chip=$('#tunState');if(chip){chip.textContent=st.on?(st.healthy?'运行中 · 健康':'运行中 · 异常'):'已停止';chip.className='chip '+(st.on?(st.healthy?'on':'warn'):'off')}
}
async function tunLoad(){
  const c=await api('/api/tunnel/config');if(c.j)tunRenderConfig(c.j.config||c.j);
  const s=await api('/api/tunnel');if(s.j)tunRenderStatus(s.j.status||{});
}
async function tunSave(){
  if(!tok()){toast('写操作需要 Bearer token','error');return}
  const cfg={enable:$('#uEnable').value==='true',map_http:$('#uHttp').value==='true',map_inbound_ssh:$('#uSsh').value==='true',exe_path:$('#uExe').value.trim(),server_addr:$('#uServer').value.trim(),server_port:Number($('#uPort').value||7000),token:$('#uToken').value,local_port:Number($('#uLocal').value||18081),remote_port:Number($('#uRemote').value||0),ssh_local_port:Number($('#uSshLocal').value||2222),ssh_remote_port:Number($('#uSshRemote').value||0)};
  const r=await api('/api/tunnel/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});
  if(!r.r.ok){$('#tunNote').textContent='保存失败：'+(r.j&&r.j.error||r.t);toast('保存失败','error');return}
  $('#tunNote').textContent='已保存，配置正在重载';toast('隧道配置已保存');setTimeout(tunLoad,1400);setTimeout(tunLoad,2400);
}
async function tunSvc(on){if(!tok()){toast('写操作需要 Bearer token','error');return}const r=await api('/service',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({name:'tunnel',on})});if(!r.r.ok){toast('服务操作失败：'+(r.j&&r.j.error||r.t),'error');return}toast(on?'隧道已启动':'隧道已停止');setTimeout(tunLoad,700)}
$('#uTokenEye').onclick=()=>{const i=$('#uToken');const show=i.type==='password';i.type=show?'text':'password';$('#uTokenEye').textContent=show?'隐藏':'显示'};
$('#saveTun').onclick=tunSave;$('#startTun').onclick=()=>tunSvc(true);$('#stopTun').onclick=()=>tunSvc(false);tunLoad();
)JS";
    }
    return "";
}

}  // namespace web_pages
}  // namespace sb3
