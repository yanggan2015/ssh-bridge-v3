// web_console.cpp — S9：单端口 HTTP + WebSocket + 10 个手机可用的网页。
//
// 设计口径：http_port 默认 18081；/ws/log 下行 BINARY；上行 /write 进 cmdq；
// 无 token 只读（status/log），写接口 401；3 个 WS 会话各自独立游标。
#include "services/web/web_console.h"

#include <winsock2.h>
#include <windows.h>
#include <mongoose.h>
#include <cjson/cJSON.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app/app_core.h"
#include "core/bridge/bridge.h"
#include "core/util/util.h"
#include "services/board_actions/board_action_runner.h"
#include "services/service_manager.h"
#include "ui/ui_messages.h"

namespace sb3 {
namespace {

constexpr const char* kCss = R"CSS(
:root{--bg:#eef2f4;--panel:#fff;--ink:#14232d;--muted:#6a7882;--line:#d9e2e7;--accent:#087f78;--accent2:#0b5f5a;--danger:#b5473a}
*{box-sizing:border-box}html,body{margin:0;min-height:100%;background:var(--bg);color:var(--ink);font:15px/1.55 "Segoe UI",system-ui,-apple-system,"Microsoft YaHei",sans-serif}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
.layout{display:grid;grid-template-columns:232px minmax(0,1fr);min-height:100vh}
aside{background:#102b33;color:#d9eceb;padding:18px 14px;position:sticky;top:0;height:100vh;overflow:auto}
.brand{font-size:18px;font-weight:700;letter-spacing:.3px;margin:0 8px 4px}.brand small{display:block;font-size:11px;font-weight:400;color:#8db7b5;margin-top:3px}
nav{display:grid;gap:2px;margin-top:18px}
nav a{color:#c7dfdc;padding:8px 10px;border-radius:8px;font-size:14px}
nav a:hover{background:#1b444b;text-decoration:none}
nav a.on{background:#0b7f76;color:#fff}
.aside-foot{margin:18px 8px 0;font-size:12px;color:#8db7b5;line-height:1.6}
main{padding:22px 26px 48px;min-width:0}
header{display:flex;align-items:flex-start;justify-content:space-between;gap:18px;margin-bottom:18px}
h1{font-size:24px;margin:0 0 4px}.sub{color:var(--muted);font-size:13px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 2px 8px rgba(20,35,45,.04)}
.card h2{font-size:16px;margin:0 0 10px}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
input,select,textarea,button{font:inherit}
input,select,textarea{border:1px solid var(--line);border-radius:8px;padding:8px 10px;background:#fff;color:var(--ink);min-width:0}
textarea{width:100%;min-height:360px;font-family:ui-monospace,Consolas,monospace;font-size:12px}
button{border:0;border-radius:8px;background:var(--accent);color:#fff;padding:8px 13px;cursor:pointer}
button:hover{background:var(--accent2)}
button.ghost{background:#e5eff0;color:#155e5a}
button.danger{background:var(--danger)}
button:disabled{opacity:.48;cursor:not-allowed}
#tokenBox{width:150px;font-size:12px}
.muted{color:var(--muted);font-size:12px}
.pill{display:inline-block;border-radius:999px;padding:2px 9px;font-size:12px;background:#e7f3f1;color:#0b6a63}
#hint{flex:1 1 100%;min-width:0;overflow-wrap:anywhere}
.pill.off{background:#f0f2f3;color:#66727d}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:10px}
.item{border:1px solid var(--line);border-radius:9px;padding:10px;background:#fbfcfc}
.item b{display:block}
.item .meta{font-size:12px;color:var(--muted);margin:3px 0 8px}
pre{white-space:pre-wrap;word-break:break-word;background:#0c1820;color:#dcefe9;border-radius:10px;
padding:13px;min-height:330px;max-height:56vh;overflow:auto;font:13px/1.4 ui-monospace,Consolas,monospace}
.list{display:grid;gap:8px}.list .item{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
table{width:100%;border-collapse:collapse}th,td{text-align:left;padding:8px;border-bottom:1px solid var(--line);font-size:13px}th{color:var(--muted);font-weight:600}
@media(max-width:760px){
.layout{display:block}
aside{position:static;height:auto}
nav{grid-template-columns:repeat(2,1fr)}
main{padding:16px 12px 36px}header{display:block}
#tokenBox{width:100%;margin-top:8px}pre{max-height:45vh}
}
)CSS";

constexpr const char* kCommonJs = R"JS(
const TOKEN_KEY='sb3_web_token';
const $=s=>document.querySelector(s);
const $$=s=>Array.from(document.querySelectorAll(s));
function tok(){return localStorage.getItem(TOKEN_KEY)||''}
function setTok(v){localStorage.setItem(TOKEN_KEY,v||'');const e=$('#tokenBox');if(e)e.value=v||''}
function hdr(extra){const h=Object.assign({},extra||{});if(tok())h.Authorization='Bearer '+tok();return h}
async function api(path,opt){
  opt=opt||{};
  const r=await fetch(path,{...opt,headers:hdr(opt.headers||{})});
  const t=await r.text();let j=null;
  try{j=JSON.parse(t)}catch(e){}
  return {r,t,j,status:r.status};
}
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function bytesText(buf){try{return new TextDecoder('utf-8').decode(buf)}catch(e){return ''}}
window.addEventListener('DOMContentLoaded',()=>{
  const e=$('#tokenBox');
  if(e){e.value=tok();e.onchange=()=>setTok(e.value.trim())}
  const sv=$('#saveToken');if(sv)sv.onclick=()=>setTok(($('#tokenBox')||{}).value||'')
});
)JS";

const char* kNav[][2] = {
    {"dashboard", "仪表盘"}, {"config", "配置"}, {"actions", "板控动作"},
    {"button-script", "快捷/脚本"}, {"relay", "继电器"}, {"tftp", "TFTP"},
    {"nfs", "NFS"}, {"share", "共享目录"}, {"tcp-forward", "TCP 转发"},
    {"tunnel", "隧道"},
};

std::string page_title(const std::string& page) {
    for (const auto& n : kNav) if (page == n[0]) return n[1];
    return "ssh-bridge";
}

std::string nav_html(const std::string& page) {
    std::string out;
    for (const auto& n : kNav) {
        out += "<a href=\"/" + std::string(n[0]) + "\"" +
               (page == n[0] ? " class=\"on\"" : "") + ">" + n[1] + "</a>";
    }
    return out;
}

std::string page_shell(const std::string& page, const std::string& body, const std::string& script) {
    std::string html = "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<title>" + page_title(page) + " · ssh-bridge</title>"
                       "<link rel=\"stylesheet\" href=\"/static/app.css\"></head><body><div class=\"layout\">"
                       "<aside><div class=\"brand\">ssh-bridge<small>v3 · Web 控制台</small></div><nav>" +
                       nav_html(page) + "</nav><div class=\"aside-foot\">S9 · HTTP + WS<br>read-only 无 token</div></aside>"
                       "<main><header><div><h1>" + page_title(page) + "</h1><div class=\"sub\">同一份通道 ring / cmdq，不另开设备连接。</div></div>"
                       "<div class=\"row\"><input id=\"tokenBox\" type=\"password\" placeholder=\"Bearer token\"><button id=\"saveToken\">保存 token</button></div></header>" +
                       body + "<script src=\"/static/app.js\"></script><script>" + script + "</script></main></div></body></html>";
    return html;
}

std::string page_body(const std::string& page) {
    if (page == "dashboard") {
        return R"HTML(
<section class="card"><div class="row">
<select id="ch"></select><button id="reload">刷新</button>
<span class="pill" id="ro">只读</span><span class="muted" id="hint"></span>
</div></section>
<section class="card"><h2>终端 · 实时字节</h2><pre id="term"></pre>
<div class="row"><input id="cmd" style="flex:1" placeholder="输入到当前通道；Enter 发送">
<button id="send">发送</button><button class="ghost" id="clear">清屏</button></div></section>)HTML";
    }
    if (page == "config") {
        return R"HTML(
<section class="card"><div class="row">
<button id="tabBasic">基础</button><button id="tabDetail" class="ghost">详细</button>
<button id="loadCfg">读取</button><button id="saveCfg">保存并重载</button>
<span class="muted" id="cfgNote">写接口需要 token。</span></div>
<div id="cfgForm"></div></section>)HTML";
    }
    if (page == "actions") {
        return R"HTML(
<section class="card"><div class="row">
<button id="loadAct">刷新动作</button>
<span class="muted">按通道分组；执行结果异步返回。</span></div>
<div id="actList" class="list"></div></section>)HTML";
    }
    if (page == "button-script") {
        return R"HTML(
<section class="card"><div class="row">
<button id="loadQuick">刷新快捷指令</button>
<span class="muted">快捷按钮发到当前通道，不新建连接。</span></div>
<div id="quickList" class="list"></div></section>)HTML";
    }
    if (page == "relay") {
        return R"HTML(
<section class="card"><div class="row">
<button id="loadRelay">刷新继电器</button>
<span class="muted">ON/OFF 同步；脉冲后台执行。</span></div>
<div id="relayList" class="list"></div></section>)HTML";
    }
    if (page == "tftp" || page == "nfs" || page == "share") {
        std::string svc = page;
        return "<section class=\"card\"><div class=\"row\"><button id=\"loadSvc\">刷新服务</button>"
               "<button class=\"ghost\" id=\"startSvc\">启动</button><button class=\"danger\" id=\"stopSvc\">停止</button></div>"
               "<div id=\"svcBox\" class=\"card\" style=\"box-shadow:none;margin-top:12px\"></div></section>"
               "<script>window.SB3_SERVICE='" + svc + "';</script>";
    }
    if (page == "tcp-forward") {
        return R"HTML(
<section class="card"><h2>S12 TCP 转发</h2>
<p class="muted">板 IP 最后一段映射到 22000+末段，转发到目标 22。配置保存并重载后即时生效。</p>
<div id="tcpBox" class="muted" style="white-space:pre-wrap;overflow-wrap:anywhere"></div>
</section>)HTML";
    }
    if (page == "tunnel") {
        return R"HTML(
<section class="card"><h2>S12 隧道</h2>
<p class="muted">外部 eadk-tunnel / frpc 子进程；HTTP 映射 18xxx，入站 SSH 映射 28xxx。</p>
<div id="tunBox" class="muted" style="white-space:pre-wrap;overflow-wrap:anywhere"></div>
</section>)HTML";
    }
    return "<section class=\"card\"><p>未知页面。</p></section>";
}

std::string page_script(const std::string& page) {
    if (page == "dashboard") {
        return R"JS(
let ws=null;
function connect(){
  if(ws){try{ws.close()}catch(e){};ws=null}
  const ch=$('#ch').value; if(!ch){$('#term').textContent='没有通道';return}
  const proto=location.protocol==='https:'?'wss':'ws';
  ws=new WebSocket(proto+'://'+location.host+'/ws/log?channel='+encodeURIComponent(ch));
  ws.binaryType='arraybuffer';
  ws.onopen=()=>{$('#hint').textContent='WS 已连接 · '+ch};
  ws.onmessage=e=>{const t=bytesText(e.data);const p=$('#term');p.textContent+=t;if(p.textContent.length>180000)p.textContent=p.textContent.slice(-120000);p.scrollTop=p.scrollHeight};
  ws.onclose=()=>{$('#hint').textContent='WS 已断开'};
}
async function load(){
  const r=await api('/status'); if(!r.j||!r.j.channels){$('#term').textContent=r.t;return}
  const sel=$('#ch');const old=sel.value;sel.innerHTML='';
  for(const c of r.j.channels){const o=document.createElement('option');o.value=c.name;o.textContent=c.name+' · '+c.state;sel.appendChild(o)}
  if(old && r.j.channels.some(c=>c.name===old))sel.value=old;
  const ro=!tok();
  $('#ro').textContent=ro?'只读（无 token）':'可写（带 token）';
  $('#ro').classList.toggle('off',ro);
  connect();
}
$('#reload').onclick=load;
$('#ch').onchange=connect;
$('#clear').onclick=()=>{$('#term').textContent=''};
$('#send').onclick=async()=>{
  const ch=$('#ch').value,data=$('#cmd').value+'\r';if(!ch)return;
  const r=await api('/write',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({channel:ch,data})});
  if(!r.r.ok){$('#hint').textContent='写入失败：'+r.status+' '+r.t}
  else{$('#hint').textContent='已写入 '+ch;$('#cmd').value=''}
};
$('#cmd').onkeydown=e=>{if(e.key==='Enter')$('#send').click()};
load();
)JS";
    }
    if (page == "config") {
        return R"JS(
let CFG=null,TAB='basic';
const BASIC=['default','web','agent','inbound_ssh','ui','boards','ssh'];
const LABELS={enable:'启用',port:'端口',token:'Token',name:'名称',serial_device:'串口设备',
  baudrate:'波特率',auto_open:'自动打开',password:'密码',username:'用户名',
  target_host:'目标主机',target_port:'目标端口',target_username:'目标用户',target_password:'目标密码',
  keepalive:'Keepalive',buffer_size:'缓冲区',log_enabled:'写日志',close_to_tray:'关闭到托盘',
  start_minimized:'启动最小化',window_w:'窗口宽',window_h:'窗口高',font_px:'字号',split:'分屏',
  panes:'画框',channel:'通道',line_ending:'行尾',encoding:'编码',databits:'数据位',parity:'校验',
  stopbits_x10:'停止位',flowcontrol:'流控',dtr:'DTR',rts:'RTS',timestamp:'时间戳',log_dir:'日志目录',
  root:'根目录',alias:'别名',http_port:'HTTP 端口',local:'本机',remote:'远端'};
function labelOf(k){return LABELS[k]||k}
function isObj(v){return v&&typeof v==='object'&&!Array.isArray(v)}
function deep(v){return JSON.parse(JSON.stringify(v))}
function escAttr(s){return esc(s).replace(/`/g,'')}
function field(path,key,val){
  const p=escAttr(path.join('.')), lab=labelOf(key);
  if(typeof val==='boolean'){
    return '<label class="row" style="gap:8px;margin:7px 0">'+
      '<span style="min-width:120px">'+esc(lab)+'</span>'+
      '<select data-path="'+p+'" data-kind="bool">'+
      '<option value="true"'+(val?' selected':'')+'>true</option>'+
      '<option value="false"'+(val?'':' selected')+'>false</option></select></label>';
  }
  if(typeof val==='number'){
    return '<label class="row" style="gap:8px;margin:7px 0">'+
      '<span style="min-width:120px">'+esc(lab)+'</span>'+
      '<input type="number" step="any" data-path="'+p+'" data-kind="num" '+
      'value="'+escAttr(val)+'" style="flex:1"></label>';
  }
  if(val===null){return ''}
  const s=String(val==null?'':val);
  if(s.length>100) return '<label style="display:block;margin:8px 0"><span class="muted">'+esc(lab)+'</span><textarea data-path="'+p+'" style="min-height:80px">'+esc(s)+'</textarea></label>';
  return '<label class="row" style="gap:8px;margin:7px 0">'+
    '<span style="min-width:120px">'+esc(lab)+'</span>'+
    '<input data-path="'+p+'" data-kind="str" value="'+escAttr(s)+'" style="flex:1"></label>';
}
function node(path,key,val){
  if(Array.isArray(val)){
    let h='<div class="item" style="margin:8px 0"><b>'+esc(labelOf(key))+' · '+val.length+'</b>';
    val.forEach((v,i)=>{h+='<div style="border-left:3px solid var(--line);padding-left:10px;margin:8px 0">'+node(path.concat(i),'#'+(i+1),v)+'</div>'});
    if(val.length&&isObj(val[0]))h+='<button class="ghost" data-add="'+escAttr(path.join('.'))+'">+ 新增一项</button>';
    return h+'</div>';
  }
  if(isObj(val)){
    let h='<fieldset style="border:1px solid var(--line);border-radius:9px;margin:10px 0;padding:8px 12px"><legend>'+esc(labelOf(key))+'</legend>';
    for(const k of Object.keys(val))h+=node(path.concat(k),k,val[k]);
    return h+'</fieldset>';
  }
  return field(path,key,val);
}
function render(){
  const box=$('#cfgForm'); if(!CFG){box.innerHTML='<p class="muted">点击“读取”加载配置。</p>';return}
  const keys=TAB==='basic'?BASIC.filter(k=>k in CFG):Object.keys(CFG);  box.innerHTML=keys.map(k=>node([k],k,CFG[k])).join('')||'<p class="muted">没有字段。</p>';
  $$('[data-add]').forEach(b=>b.onclick=()=>{const p=b.dataset.add.split('.');let a=CFG;for(const k of p)a=a[k];const t=a.length?deep(a[0]):{};a.push(t);render()});
}
async function cfgLoad(){
  const r=await api('/api/config');
  if(!r.r.ok){
    $('#cfgNote').textContent='读取失败 '+r.status+'：需要 Bearer token';return;
  }
  try{
    CFG=JSON.parse(r.t);
    $('#cfgNote').textContent='已读取 '+Object.keys(CFG).length+' 个顶层段';
    render();
  }catch(e){$('#cfgNote').textContent='JSON 解析失败'}
}
function collect(){
  const root=deep(CFG||{});
  $$('#cfgForm [data-path]').forEach(el=>{
    const parts=el.dataset.path.split('.');let o=root;
    for(let i=0;i<parts.length-1;i++)o=o[parts[i]];
    const k=parts[parts.length-1];let v=el.value;
    if(el.dataset.kind==='bool')v=(v==='true');
    else if(el.dataset.kind==='num')v=Number(v);
    o[k]=v;
  });
  return root;
}
$('#tabBasic').onclick=()=>{
  TAB='basic';$('#tabBasic').classList.remove('ghost');
  $('#tabDetail').classList.add('ghost');render()
};
$('#tabDetail').onclick=()=>{
  TAB='detail';$('#tabDetail').classList.remove('ghost');
  $('#tabBasic').classList.add('ghost');render()
};
$('#loadCfg').onclick=cfgLoad;
$('#saveCfg').onclick=async()=>{
  if(!CFG)return;
  const r=await api('/api/config/save',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(collect())});
  $('#cfgNote').textContent=r.r.ok?'已保存，UI 线程正在重载':'保存失败 '+r.status+'：'+r.t;
};
)JS";
    }
    if (page == "actions") {
        return R"JS(
async function actLoad(){
  const r=await api('/api/actions');const box=$('#actList');
  if(!r.j||!r.j.items){box.textContent=r.t;return}
  box.innerHTML='';
  for(const a of r.j.items){
    const d=document.createElement('div');d.className='item';
    d.innerHTML='<div><b>'+esc(a.channel)+' · '+esc(a.label)+'</b>'+
      '<div class="meta">'+esc(a.kind||'')+' · '+esc(a.id)+'</div></div><button>执行</button>';
    d.querySelector('button').onclick=async()=>{
      const rr=await api('/action',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({channel:a.channel,id:a.id})});
      if(!rr.r.ok){d.querySelector('.meta').textContent='失败 '+rr.status+' '+rr.t;return}
      const id=rr.j&&rr.j.job;
      d.querySelector('.meta').textContent='已排队 #'+id;
      if(id)polllJob(id,d);
    };
    box.appendChild(d);
  }
}
async function polllJob(id,d){
  for(let i=0;i<30;i++){
    await new Promise(r=>setTimeout(r,400));
    const r=await api('/job?id='+id);
    if(r.j&&r.j.done){
      d.querySelector('.meta').textContent=(r.j.ok?'OK · ':'FAIL · ')+
        (r.j.message||'')+(r.j.output?(' | '+r.j.output.slice(0,120)):'');
      return;
    }
  }
}
$('#loadAct').onclick=actLoad;actLoad();
)JS";
    }
    if (page == "button-script") {
        return R"JS(
async function quickLoad(){
  const r=await api('/quick');const box=$('#quickList');
  if(!r.j||!r.j.items){box.textContent=r.t;return}
  box.innerHTML='';
  for(const q of r.j.items){
    const d=document.createElement('div');d.className='item';
    d.innerHTML='<div><b>'+esc(q.channel)+' · '+esc(q.label)+'</b>'+
      '<div class="meta">'+esc(q.command)+'</div></div><button>发送</button>';
    d.querySelector('button').onclick=async()=>{
      const rr=await api('/quick',{method:'POST',headers:{'Content-Type':'application/json'},
        body:JSON.stringify({channel:q.channel,command:q.command})});
      d.querySelector('.meta').textContent=rr.r.ok?'已发送':'失败 '+rr.status+' '+rr.t;
    };
    box.appendChild(d);
  }
}
$('#loadQuick').onclick=quickLoad;quickLoad();
)JS";
    }
    if (page == "relay") {
        return R"JS(
async function relayLoad(){
  const r=await api('/api/relay');const box=$('#relayList');
  if(!r.j){box.textContent=r.t;return}
  const arr=r.j.items||r.j.devices||[];box.innerHTML='';
  if(!arr.length){box.innerHTML='<div class="muted">没有继电器设备。</div>';return}
  for(const d of arr){
    const el=document.createElement('div');el.className='item';
    el.innerHTML='<div><b>'+esc(d.name||d.device)+'</b><div class="meta">'+
      esc(d.port||d.windows_port||'')+' · CH '+(d.channels||'')+'</div></div>';
    for(let c=1;c<=(d.channels||2);c++){
      const on=document.createElement('button');on.textContent='CH'+c+' ON';
      on.onclick=()=>relayOp(d,c,'on',el);
      const off=document.createElement('button');off.textContent='CH'+c+' OFF';
      off.className='ghost';off.onclick=()=>relayOp(d,c,'off',el);
      const pu=document.createElement('button');pu.textContent='脉冲';
      pu.className='ghost';pu.onclick=()=>relayOp(d,c,'pulse',el);
      el.appendChild(on);el.appendChild(off);el.appendChild(pu);
    }
    box.appendChild(el);
  }
}
async function relayOp(d,ch,op,el){
  const rr=await api('/api/relay',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name:d.name||d.device,channel:ch,op})});
  const m=el.querySelector('.meta');
  m.textContent=rr.r.ok?('CH'+ch+' '+op+' 已提交'):('失败 '+rr.status+' '+rr.t);
}
$('#loadRelay').onclick=relayLoad;relayLoad();
)JS";
    }
    if (page == "tftp" || page == "nfs" || page == "share") {
        return R"JS(
const SVC=window.SB3_SERVICE||'tftp';
async function svcLoad(){
  const r=await api('/services');const box=$('#svcBox');
  if(!r.j){box.textContent=r.t;return}
  const s=(r.j.services||r.j)[SVC]||{};
  box.innerHTML='<pre style="min-height:100px;max-height:220px">'+esc(JSON.stringify(s,null,2))+'</pre>'+
    (s.url?('<p><a href="'+esc(s.url)+'" target="_blank">打开 '+esc(s.url)+'</a></p>'):'');
}
async function svcSet(on){
  const r=await api('/service',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name:SVC,on})});
  if(!r.r.ok)alert('失败 '+r.status+' '+r.t);
  svcLoad();
}
$('#loadSvc').onclick=svcLoad;$('#startSvc').onclick=()=>svcSet(true);$('#stopSvc').onclick=()=>svcSet(false);svcLoad();
)JS";
    }
    if (page == "tcp-forward") {
        return R"JS(
(async()=>{const r=await api('/api/tcp-forward');let t=r.t;try{t=JSON.stringify(JSON.parse(t),null,2)}catch(e){}$('#tcpBox').textContent=t})();
)JS";
    }
    if (page == "tunnel") {
        return R"JS(
(async()=>{const r=await api('/api/tunnel');let t=r.t;try{t=JSON.stringify(JSON.parse(t),null,2)}catch(e){}$('#tunBox').textContent=t})();
)JS";
    }
    return "";
}

std::string json_str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring) : std::string();
}

std::string json_text(cJSON* root, bool formatted = false) {
    if (!root) return "{}";
    char* p = formatted ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    std::string out = p ? p : "{}";
    cJSON_free(p);
    cJSON_Delete(root);
    return out;
}

std::string query_var(const mg_http_message* hm, const char* name, std::string def = "") {
    char buf[256] = "";
    int n = mg_http_get_var(&hm->query, name, buf, sizeof(buf));
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : def;
}

std::string body_string(const mg_http_message* hm) {
    return std::string(hm->body.buf ? hm->body.buf : "", hm->body.len);
}

bool method_is(const mg_http_message* hm, const char* m) {
    return hm && hm->method.len == std::strlen(m) &&
           std::string(hm->method.buf, hm->method.len) == m;
}

void reply_text(mg_connection* c, int code, const std::string& body, const char* ctype) {
    std::string hdrs = std::string("Content-Type: ") + ctype +
                       "\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n";
    mg_http_reply(c, code, hdrs.c_str(), "%s", body.c_str());
}

void reply_json(mg_connection* c, int code, cJSON* root) {
    reply_text(c, code, json_text(root), "application/json; charset=utf-8");
}

void reply_json_text(mg_connection* c, int code, const std::string& text) {
    reply_text(c, code, text, "application/json; charset=utf-8");
}

struct WsState {
    std::string channel;
    uint64_t epoch = 0;
    uint64_t seen = 0;
};

struct Job {
    int id = 0;
    bool done = false;
    bool ok = false;
    std::string message;
    std::string output;
    std::future<void> fut;
};

}  // namespace

struct WebConsole::Impl {
    AppCore* core = nullptr;
    ServiceManager* services = nullptr;
    mg_mgr* mgr = nullptr;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> stop{false};
    int port = 18081;
    std::string token;
    void* notify = nullptr;
    std::string error;
    mutable std::mutex mu;
    std::map<mg_connection*, WsState> ws;
    std::map<int, std::shared_ptr<Job>> jobs;
    int next_job = 1;

    bool write_ok(mg_http_message* hm) const {
        if (token.empty()) return false;
        mg_str* h = mg_http_get_header(hm, "Authorization");
        if (!h) return false;
        std::string v(h->buf, h->len);
        return v == "Bearer " + token;
    }

    std::shared_ptr<IBridge> bridge(const std::string& name) {
        return core ? core->find_shared(name) : nullptr;
    }

    std::string status() const;
    std::string actions() const;
    std::string quick() const;
    std::string jobs_json(int id) const;
    std::string page(const std::string& name) const;
    void handle_http(mg_connection* c, mg_http_message* hm);
    void handle_ws(mg_connection* c, int ev, void* ev_data);
    void push_ws(mg_connection* c, WsState& s);
    void cleanup_job();
    void spawn_action(const std::string& channel, const std::string& id, mg_connection* c);
    void start_thread();
};

std::string WebConsole::Impl::status() const {
    cJSON* root = core ? cJSON_Parse(core->state_json(true).c_str()) : nullptr;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (services) {
        cJSON* svc = cJSON_Parse(services->state_json().c_str());
        if (cJSON_IsObject(svc)) cJSON_AddItemToObject(root, "services", svc);
        else cJSON_Delete(svc);
    }
    cJSON* web = cJSON_AddObjectToObject(root, "web");
    cJSON_AddBoolToObject(web, "on", running.load());
    cJSON_AddNumberToObject(web, "port", port);
    cJSON_AddBoolToObject(web, "readonly", token.empty());
    std::string ip = services ? services->local_ip() : "127.0.0.1";
    cJSON_AddStringToObject(web, "url", ("http://" + ip + ":" + std::to_string(port)).c_str());
    return json_text(root);
}

std::string WebConsole::Impl::actions() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            const std::vector<BoardActionConfig>* btns = core->buttons_of(e.name);
            if (!btns) continue;
            for (const BoardActionConfig& a : *btns) {
                if (!a.enable) continue;
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                cJSON_AddStringToObject(o, "id", a.id.c_str());
                cJSON_AddStringToObject(o, "label", a.label.c_str());
                cJSON_AddStringToObject(o, "kind", a.backend.c_str());
                cJSON_AddItemToArray(items, o);
            }
        }
    }
    return json_text(root);
}

std::string WebConsole::Impl::quick() const {
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    if (core) {
        for (const AppCore::Entry& e : core->entries()) {
            const std::vector<QuickCommandConfig>* qs = core->quick_commands_of(e.name);
            if (!qs) continue;
            for (const QuickCommandConfig& q : *qs) {
                cJSON* o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "channel", e.name.c_str());
                cJSON_AddStringToObject(o, "label", q.label.c_str());
                cJSON_AddStringToObject(o, "command", q.command.c_str());
                cJSON_AddItemToArray(items, o);
            }
        }
    }
    return json_text(root);
}

std::string WebConsole::Impl::jobs_json(int id) const {
    std::lock_guard<std::mutex> g(mu);
    auto it = jobs.find(id);
    if (it == jobs.end()) return "{\"ok\":false,\"error\":\"job not found\"}";
    const auto& j = it->second;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", j->id);
    cJSON_AddBoolToObject(root, "done", j->done);
    cJSON_AddBoolToObject(root, "ok", j->ok);
    cJSON_AddStringToObject(root, "message", j->message.c_str());
    cJSON_AddStringToObject(root, "output", j->output.c_str());
    return json_text(root);
}

std::string WebConsole::Impl::page(const std::string& name) const {
    return page_shell(name, page_body(name), page_script(name));
}

void WebConsole::Impl::push_ws(mg_connection* c, WsState& s) {
    if (!c || !c->is_websocket) return;
    auto b = bridge(s.channel);
    if (!b) return;
    const uint64_t seq = b->notify_seq();
    (void)seq;
    std::string data = b->read_since(&s.epoch, &s.seen);
    if (!data.empty()) {
        mg_ws_send(c, data.data(), data.size(), WEBSOCKET_OP_BINARY);
    }
}

void WebConsole::Impl::spawn_action(const std::string& channel, const std::string& id,
                                    mg_connection* c) {
    (void)c;
    if (!core) return;
    const BoardActionConfig* found = nullptr;
    const std::vector<BoardActionConfig>* btns = core->buttons_of(channel);
    if (btns) {
        for (const BoardActionConfig& a : *btns) {
            if (a.id == id) {
                found = &a;
                break;
            }
        }
    }
    if (!found) return;
    auto job = std::make_shared<Job>();
    {
        std::lock_guard<std::mutex> g(mu);
        job->id = next_job++;
        jobs[job->id] = job;
    }
    std::string ch = channel;
    BoardActionConfig action = *found;
    auto bridge = core->find_shared(ch);
    AppConfig cfg = core->config();
    job->fut = std::async(std::launch::async, [job, cfg, ch, action, bridge, this]() {
        BoardActionResult r = run_board_action(
            cfg, ch, action, bridge.get(),
            [this](const std::string& port) { return core && core->port_in_use(port); }, 0, {});
        std::lock_guard<std::mutex> g(mu);
        job->done = true;
        job->ok = r.ok;
        job->message = r.message;
        job->output = r.output;
    });
}

void WebConsole::Impl::cleanup_job() {
    std::lock_guard<std::mutex> g(mu);
    for (auto it = jobs.begin(); it != jobs.end();) {
        if (it->second->done && it->second->fut.valid()) it->second->fut.get();
        if (it->second->done && it->first + 50 < next_job) it = jobs.erase(it);
        else ++it;
    }
}

void WebConsole::Impl::handle_http(mg_connection* c, mg_http_message* hm) {
    if (!hm) return;
    std::string path(hm->uri.buf, hm->uri.len);
    if (path == "/" || path == "") path = "/dashboard";
    if (path.size() > 5 && path.rfind(".html") == path.size() - 5) path.resize(path.size() - 5);

    if (path == "/ws/log") {
        std::string ch = query_var(hm, "channel");
        if (ch.empty()) {
            reply_text(c, 400, "channel required", "text/plain; charset=utf-8");
            return;
        }
        if (!bridge(ch)) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        WsState st;
        st.channel = ch;
        {
            std::lock_guard<std::mutex> g(mu);
            ws[c] = st;
        }
        mg_ws_upgrade(c, hm, nullptr);
        return;
    }

    if (path == "/static/app.css") {
        mg_str* inm = mg_http_get_header(hm, "If-None-Match");
        if (inm && std::string(inm->buf, inm->len) == "\"sb3-s9-css\"") {
            mg_http_reply(c, 304, "ETag: \"sb3-s9-css\"\r\nCache-Control: max-age=3600\r\n", "");
            return;
        }
        mg_http_reply(c, 200,
                      "Content-Type: text/css; charset=utf-8\r\nETag: \"sb3-s9-css\"\r\n"
                      "Cache-Control: max-age=3600\r\nAccess-Control-Allow-Origin: *\r\n",
                      "%s", kCss);
        return;
    }
    if (path == "/static/app.js") {
        mg_str* inm = mg_http_get_header(hm, "If-None-Match");
        if (inm && std::string(inm->buf, inm->len) == "\"sb3-s9-js\"") {
            mg_http_reply(c, 304, "ETag: \"sb3-s9-js\"\r\nCache-Control: max-age=3600\r\n", "");
            return;
        }
        mg_http_reply(c, 200,
                      "Content-Type: application/javascript; charset=utf-8\r\n"
                      "ETag: \"sb3-s9-js\"\r\nCache-Control: max-age=3600\r\n"
                      "Access-Control-Allow-Origin: *\r\n",
                      "%s", kCommonJs);
        return;
    }
    if (path == "/health") {
        reply_json_text(c, 200, "{\"ok\":true,\"service\":\"ssh-bridge-web\",\"s9\":true}");
        return;
    }
    if (path == "/favicon.ico") {
        mg_http_reply(c, 204, "Cache-Control: max-age=86400\r\n", "");
        return;
    }
    if (path == "/status") {
        reply_json_text(c, 200, status());
        return;
    }
    if (path == "/log") {
        std::string ch = query_var(hm, "channel");
        int chars = std::max(1, std::min(65536, std::atoi(query_var(hm, "chars", "8192").c_str())));
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        uint64_t e = 0, s = 0;
        std::string all = b->read_all(&e, &s);
        if (static_cast<int>(all.size()) > chars) all = all.substr(all.size() - chars);
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "channel", ch.c_str());
        cJSON_AddStringToObject(root, "text", all.c_str());
        cJSON_AddNumberToObject(root, "chars", static_cast<double>(all.size()));
        reply_json(c, 200, root);
        return;
    }
    if (path == "/write") {
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string data = json_str(body, "data");
        cJSON_Delete(body);
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        b->write(data.data(), data.size());
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddNumberToObject(root, "bytes", static_cast<double>(data.size()));
        reply_json(c, 200, root);
        return;
    }
    if (path == "/api/config/save") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        std::string err;
        if (!core || !core->save_config_json(body_string(hm), &err)) {
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", false);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, 400, root);
            return;
        }
        if (notify) PostMessageW(static_cast<HWND>(notify), kMsgWebReload, 0, 0);
        reply_json_text(c, 200, "{\"ok\":true,\"reload\":\"posted\"}");
        return;
    }
    if (path == "/api/config") {
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"config requires Bearer token\"}");
            return;
        }
        reply_text(c, 200, core ? core->config_json_text() : "{}", "application/json; charset=utf-8");
        return;
    }
    if (path == "/api/actions") {
        reply_json_text(c, 200, actions());
        return;
    }
    if (path == "/quick") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            std::string ch = json_str(body, "channel");
            std::string cmd = json_str(body, "command");
            cJSON_Delete(body);
            auto b = bridge(ch);
            if (!b) {
                reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
                return;
            }
            std::string data = cmd;
            if (data.find('\n') == std::string::npos && data.find('\r') == std::string::npos) data += "\r";
            b->write(data.data(), data.size());
            reply_json_text(c, 200, "{\"ok\":true}");
            return;
        }
        reply_json_text(c, 200, quick());
        return;
    }
    if (path == "/action") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string ch = json_str(body, "channel");
        std::string id = json_str(body, "id");
        cJSON_Delete(body);
        const BoardActionConfig* found = nullptr;
        const auto* btns = core ? core->buttons_of(ch) : nullptr;
        if (btns) {
            for (const BoardActionConfig& a : *btns) {
                if (a.id == id) {
                    found = &a;
                    break;
                }
            }
        }
        if (!found) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"action not found\"}");
            return;
        }
        spawn_action(ch, id, c);
        int idnum = 0;
        {
            std::lock_guard<std::mutex> g(mu);
            idnum = next_job - 1;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"job\":%d}", idnum);
        reply_json_text(c, 202, buf);
        return;
    }
    if (path == "/job") {
        int id = std::atoi(query_var(hm, "id", "0").c_str());
        reply_json_text(c, 200, jobs_json(id));
        return;
    }
    if (path == "/api/relay") {
        if (method_is(hm, "POST")) {
            if (!write_ok(hm)) {
                reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
                return;
            }
            cJSON* body = cJSON_Parse(body_string(hm).c_str());
            std::string name = json_str(body, "name");
            int ch = static_cast<int>(cJSON_GetObjectItem(body, "channel")
                                          ? cJSON_GetObjectItem(body, "channel")->valuedouble
                                          : 1);
            std::string op = json_str(body, "op");
            cJSON_Delete(body);
            const RelayDeviceConfig* dev = nullptr;
            if (core) {
                for (const RelayDeviceConfig& d : core->config().relays) {
                    if (d.name == name) {
                        dev = &d;
                        break;
                    }
                }
            }
            if (!dev || !services) {
                reply_json_text(c, 404, "{\"ok\":false,\"error\":\"relay not found\"}");
                return;
            }
            std::string err;
            bool ok = false;
            if (op == "on") ok = services->relay_set(*dev, ch, true, &err);
            else if (op == "off") ok = services->relay_set(*dev, ch, false, &err);
            else if (op == "pulse") ok = services->relay_pulse_async(*dev, ch, 1.0);
            else err = "op must be on/off/pulse";
            cJSON* root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", ok);
            cJSON_AddStringToObject(root, "error", err.c_str());
            reply_json(c, ok ? 200 : 400, root);
            return;
        }
        reply_json_text(c, 200, services ? services->relay_json() : "[]");
        return;
    }
    if (path == "/services") {
        reply_json_text(c, 200, services ? services->state_json() : "{}");
        return;
    }
    if (path == "/service") {
        if (!method_is(hm, "POST")) {
            reply_json_text(c, 405, "{\"ok\":false,\"error\":\"POST required\"}");
            return;
        }
        if (!write_ok(hm)) {
            reply_json_text(c, 401, "{\"ok\":false,\"error\":\"write requires Bearer token\"}");
            return;
        }
        cJSON* body = cJSON_Parse(body_string(hm).c_str());
        std::string name = json_str(body, "name");
        bool on = cJSON_IsTrue(cJSON_GetObjectItem(body, "on"));
        cJSON_Delete(body);
        if (!services || !core) {
            reply_json_text(c, 500, "{\"ok\":false,\"error\":\"services not ready\"}");
            return;
        }
        std::string err;
        bool ok = false;
        const AppConfig& cfg = core->config();
        if (name == "tftp") {
            if (on) ok = services->start_tftp(cfg.tftp, &err);
            else { services->stop_tftp(); ok = true; }
        } else if (name == "share") {
            if (on) ok = services->start_share(cfg.share, &err);
            else { services->stop_share(); ok = true; }
        } else if (name == "nfs") {
            if (on) ok = services->start_nfs(cfg.nfs, &err);
            else { services->stop_nfs(); ok = true; }
        } else if (name == "inbound") {
            if (on) ok = services->start_inbound(cfg.inbound, &err);
            else { services->stop_inbound(); ok = true; }
        } else if (name == "tcp-forward") {
            if (on) ok = services->start_tcp_forward(cfg.tcp_fwd, &err);
            else { services->stop_tcp_forward(); ok = true; }
        } else if (name == "tunnel") {
            if (on) ok = services->start_tunnel(cfg.tunnel, &err);
            else { services->stop_tunnel(); ok = true; }
        } else if (name == "discovery") {
            if (on) ok = services->start_discovery(cfg.discovery, cfg, &err);
            else { services->stop_discovery(); ok = true; }
        } else {
            err = "unknown service";
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddStringToObject(root, "error", err.c_str());
        reply_json(c, ok ? 200 : 400, root);
        return;
    }
    if (path == "/api/tcp-forward" || path == "/api/tunnel" || path == "/api/discovery") {
        const char* key = path == "/api/tcp-forward" ? "tcp_forward"
                          : path == "/api/tunnel"      ? "tunnel"
                                                       : "discovery";
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        bool running = false;
        if (services) {
            cJSON* all = cJSON_Parse(services->state_json().c_str());
            cJSON* sec = all ? cJSON_GetObjectItemCaseSensitive(all, key) : nullptr;
            if (cJSON_IsObject(sec)) {
                running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sec, "on"));
                cJSON* dup = cJSON_Duplicate(sec, 1);
                if (dup) cJSON_AddItemToObject(root, "status", dup);
            }
            cJSON_Delete(all);
        }
        cJSON_AddBoolToObject(root, "running", running);
        reply_json(c, 200, root);
        return;
    }
    if (path == "/help") {
        std::string help = "ssh-bridge Web 控制台\n\n"
                           "读：GET /status /log /actions /quick /relay /services\n"
                           "写：POST /write /action /quick /relay /service，需 Authorization: Bearer <token>\n"
                           "WS：/ws/log?channel=<通道名>，服务端只发 BINARY 帧。\n";
        reply_text(c, 200, help, "text/plain; charset=utf-8");
        return;
    }
    if (path.rfind("/help/", 0) == 0) {
        std::string ch = path.substr(6);
        auto b = bridge(ch);
        if (!b) {
            reply_json_text(c, 404, "{\"ok\":false,\"error\":\"channel not found\"}");
            return;
        }
        reply_text(c, 200, "通道 " + ch + "\n状态：" + (b->running() ? "在线" : "离线") +
                                "\n读写：/log?channel=" + ch + "，/write 需要 token。\n",
                   "text/plain; charset=utf-8");
        return;
    }

    // 10 个页面
    for (const auto& n : kNav) {
        if (path == std::string("/") + n[0]) {
            reply_text(c, 200, page(n[0]), "text/html; charset=utf-8");
            return;
        }
    }
    reply_text(c, 404, "not found", "text/plain; charset=utf-8");
}

void WebConsole::Impl::handle_ws(mg_connection* c, int ev, void* ev_data) {
    (void)ev_data;
    if (ev == MG_EV_CLOSE) {
        std::lock_guard<std::mutex> g(mu);
        ws.erase(c);
        return;
    }
    auto it = ws.find(c);
    if (it == ws.end()) return;
    WsState& s = it->second;
    if (ev == MG_EV_WS_OPEN) {
        auto b = bridge(s.channel);
        if (!b) return;
        uint64_t e = 0, seen = 0;
        std::string tail = b->read_all(&e, &seen);
        if (tail.size() > 8192) tail = tail.substr(tail.size() - 8192);
        if (!tail.empty()) mg_ws_send(c, tail.data(), tail.size(), WEBSOCKET_OP_BINARY);
        s.epoch = e;
        s.seen = seen;
    } else if (ev == MG_EV_POLL) {
        push_ws(c, s);
    }
}

void WebConsole::Impl::start_thread() {
    thread = std::thread([this]() {
        while (!stop.load()) {
            mg_mgr_poll(mgr, 10);
            cleanup_job();
        }
    });
}

WebConsole::WebConsole() : impl_(new Impl()) {}
WebConsole::~WebConsole() { stop(); delete impl_; }

void WebConsole::set_notify(void* hwnd) {
    if (impl_) impl_->notify = hwnd;
}

bool WebConsole::start(AppCore* core, ServiceManager* services, int port,
                       const std::string& token, std::string* err) {
    if (impl_->running.load()) return true;
    if (port <= 0 || port > 65535) {
        if (err) *err = "Web 端口越界";
        return false;
    }
    impl_->core = core;
    impl_->services = services;
    impl_->port = port;
    impl_->token = token;
    impl_->error.clear();
    impl_->stop.store(false);
    impl_->mgr = new mg_mgr();
    mg_mgr_init(impl_->mgr);
    char url[160];
    std::snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);
    mg_connection* lc = mg_http_listen(impl_->mgr, url, [](mg_connection* c, int ev, void* ev_data) {
        Impl* self = static_cast<Impl*>(c->fn_data);
        if (!self) return;
        if (ev == MG_EV_HTTP_MSG) self->handle_http(c, static_cast<mg_http_message*>(ev_data));
        else if (ev == MG_EV_WS_OPEN || ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL ||
                 ev == MG_EV_CLOSE || ev == MG_EV_POLL) self->handle_ws(c, ev, ev_data);
    }, impl_);
    if (!lc) {
        if (err) *err = "Web 端口 " + std::to_string(port) + " 被占用或无法监听";
        impl_->error = *err;
        mg_mgr_free(impl_->mgr);
        delete impl_->mgr;
        impl_->mgr = nullptr;
        return false;
    }

    // WS 升级路由需要先拿到 http message；在 HTTP handler 里登记 state 再升级。
    // mg_http_listen 的 c->fn_data 是 impl_；handle_http 通过 query 判断 /ws/log。
    impl_->running.store(true);
    impl_->start_thread();
    if (err) err->clear();
    return true;
}

void WebConsole::stop() {
    if (!impl_) return;
    impl_->stop.store(true);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->mgr) {
        mg_mgr_free(impl_->mgr);
        delete impl_->mgr;
        impl_->mgr = nullptr;
    }
    impl_->running.store(false);
    std::lock_guard<std::mutex> g(impl_->mu);
    impl_->ws.clear();
    for (auto& kv : impl_->jobs) {
        if (kv.second->fut.valid()) kv.second->fut.wait();
    }
    impl_->jobs.clear();
}

bool WebConsole::running() const { return impl_ && impl_->running.load(); }
int WebConsole::port() const { return impl_ ? impl_->port : 0; }
std::string WebConsole::error() const { return impl_ ? impl_->error : std::string(); }
std::string WebConsole::token() const { return impl_ ? impl_->token : std::string(); }
void WebConsole::configure(int port, const std::string& token) {
    if (!impl_) return;
    impl_->port = port;
    impl_->token = token;
}
std::string WebConsole::status_json() const {
    if (!impl_) return "{}";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "on", impl_->running.load());
    cJSON_AddNumberToObject(root, "port", impl_->port);
    cJSON_AddBoolToObject(root, "readonly", impl_->token.empty());
    cJSON_AddStringToObject(root, "error", impl_->error.c_str());
    return json_text(root);
}

}  // namespace sb3



