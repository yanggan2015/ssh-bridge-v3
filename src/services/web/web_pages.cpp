#include "services/web/web_pages.h"

namespace sb3 {
namespace web_pages {

extern const char* const kCss = R"CSS(
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

extern const char* const kCommonJs = R"JS(
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

}  // namespace web_pages
}  // namespace sb3
