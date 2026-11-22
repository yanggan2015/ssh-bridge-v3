# -*- coding: utf-8 -*-
"""生成阶梯架构图（S0–S13）。

唯一数据源：NODES / EDGES。改这里，重跑本脚本，重新生成 docs\阶梯架构图.md。
颜色规则：
  ext  外部设备/客户端        灰蓝实线
  done 已完成（≤ 当前步）      绿色实线
  cur  本步新增                橙色粗线
  plan 未实现（后续步骤预留）  灰色虚线
这样相邻两张图只有颜色差异，一眼能看出"这一步加了什么"。
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT_MD = os.path.join(ROOT, 'docs', '阶梯架构图.md')


# (id, 标签, 所属步骤, 分组)
NODES = [
    ('E_COM', 'COM / tty 板', 0, 'ext'),
    ('E_HOST', '远程主机', 0, 'ext'),
    ('E_CLI', '外部 ssh', 8, 'ext'),
    ('E_BR', '浏览器 / 手机', 9, 'ext'),
    ('E_AG', 'Agent / 脚本', 10, 'ext'),

    ('APP', '装配', 0, 'ui1'),
    ('SHELL', '窗口壳 / 托盘', 3, 'ui1'),
    ('TERM', '终端窗格', 4, 'ui1'),
    ('MGMT', '通道管理', 5, 'ui1'),
    ('PREFS', '偏好 / 键', 6, 'ui2'),
    ('SFTP', 'SFTP 面板', 7, 'ui2'),
    ('BTN', '按钮条', 7, 'ui2'),
    ('LUA', 'Lua', 7, 'ui2'),

    ('CFG', 'config', 1, 'core'),
    ('LOGB', 'logbuf', 1, 'core'),
    ('SER', 'serial', 1, 'core'),
    ('WR', 'writer', 1, 'core'),
    ('SSHOB', 'ssh 出站', 2, 'core'),
    ('XMOD', 'xmodem', 7, 'core'),
    ('UPD', '打包 / 更新', 13, 'core'),

    ('INB', '入站 :2222', 8, 'svc1'),
    ('HTTP', 'http_core', 9, 'svc1'),
    ('WS', 'ws /ws/log', 9, 'svc1'),
    ('WEB', '网页 UI', 9, 'svc1'),
    ('AGT', 'Agent :18096', 10, 'svc1'),
    ('REL', '继电器', 11, 'svc2'),
    ('TFTP', 'TFTP', 11, 'svc2'),
    ('NFS', 'NFS', 11, 'svc2'),
    ('SHR', '共享', 11, 'svc2'),
    ('FWD', '转发', 12, 'svc2'),
    ('TUN', '隧道', 12, 'svc2'),
    ('DSC', '发现', 12, 'svc2'),
]

EDGES = [
    ('APP', 'CFG'),
    ('CFG', 'SER'), ('CFG', 'SSHOB'), ('CFG', 'DSC'),
    ('WR', 'SER'), ('WR', 'SSHOB'),
    ('SER', 'LOGB'), ('SSHOB', 'LOGB'),
    ('TERM', 'LOGB'), ('TERM', 'WR'),
    ('SHELL', 'TERM'), ('SHELL', 'PREFS'),
    ('MGMT', 'CFG'), ('MGMT', 'SHELL'),
    ('SFTP', 'SSHOB'),
    ('BTN', 'WR'), ('BTN', 'LUA'), ('BTN', 'REL'),
    ('XMOD', 'SER'),
    ('INB', 'SER'),
    ('HTTP', 'LOGB'), ('HTTP', 'WR'),
    ('WS', 'LOGB'), ('WEB', 'WS'), ('WEB', 'HTTP'),
    ('AGT', 'HTTP'),
    ('TFTP', 'SHR'), ('NFS', 'SHR'),
    ('TUN', 'HTTP'), ('TUN', 'INB'),
    ('UPD', 'APP'),
]

GROUPS = {
    'ui1':  ('GUI1', 'ui 界面 · 窗格 / 通道'),
    'ui2':  ('GUI2', 'ui 界面 · 工具面板'),
    'svc1': ('GSVC1', '扩展 · 入站 / 网页 / Agent'),
    'svc2': ('GSVC2', '扩展 · 文件 / 按钮 / 转发'),
    'ext':  ('OUT',  '外部设备 / 客户端'),
    'core': ('GCORE', 'core 核心（与界面无关）'),
    'ui':   ('GUI',  'ui 界面'),
    'svc':  ('GSVC', 'services 扩展（端口 / 可关）'),
}

TITLES = {
    0: 'S0 工程地基',
    1: 'S1 配置 / 环形日志 / 串口桥',
    2: 'S2 SSH 出站链路',
    3: 'S3 窗口壳 + 生命周期',
    4: 'S4 终端窗格',
    5: 'S5 通道管理 + 热应用',
    6: 'S6 界面偏好 / 会话操作 / 快捷键',
    7: 'S7 文件与脚本（本体出口）',
    8: 'S8 SSH 入站网关',
    9: 'S9 HTTP 基元 + 网页端',
    10: 'S10 Agent 接口',
    11: 'S11 继电器 / TFTP / NFS',
    12: 'S12 TCP 转发 / 隧道 / 发现',
    13: 'S13 自更新 / 打包 / 全量回归',
}

STEP = {n[0]: n[2] for n in NODES}
GROUP = {n[0]: n[3] for n in NODES}
LABEL = {n[0]: n[1] for n in NODES}


SHOW_EDGES = False   # 阶梯图用纯层带（层内不连线），连线细节见 ④ 逐单元卡片


def cls_of(nid, s):
    if GROUP[nid] == 'ext':
        return 'ext'
    st = STEP[nid]
    if st == s:
        return 'cur'
    return 'done' if st < s else 'plan'


MM_INIT = ("%%{init: {'theme':'base','themeVariables':{'clusterBkg':'#ffffff',"
           "'clusterBorder':'#d7e1ec','fontFamily':'Microsoft YaHei, sans-serif',"
           "'fontSize':'14px','lineColor':'#8794a3'},"
           "'flowchart':{'htmlLabels':false,'useMaxWidth':true}}}%%")


def diagram(s):
    lines = [MM_INIT, 'flowchart LR']
    lines.append('  classDef ext fill:#eef2f7,stroke:#8794a3,stroke-width:1.5px,color:#33414f')
    lines.append('  classDef done fill:#e6f6ec,stroke:#2f9e63,stroke-width:2px,color:#14532d')
    lines.append('  classDef cur fill:#fff0d2,stroke:#e08900,stroke-width:3px,color:#7a4400')
    lines.append('  classDef plan fill:#f6f7f9,stroke:#b9c0c9,stroke-width:1px,'
                 'color:#9aa4af,stroke-dasharray:6 4')
    for g in ('ext', 'svc2', 'svc1', 'core', 'ui2', 'ui1'):
        sid, title = GROUPS[g]
        lines.append('  subgraph %s["%s"]' % (sid, title))
        lines.append('    direction TB')
        ids = []
        for nid, label, step, grp in NODES:
            if grp != g:
                continue
            ids.append(nid)
            tag = ('S%d' % step) if grp != 'ext' else '外部'
            lines.append('    %s["%s<br/>%s"]:::%s' % (nid, label, tag, cls_of(nid, s)))
        # 组内用隐形链把节点压成一列，避免自动布局摊开成一张大网
        pass
        lines.append('  end')
    for a, b in (EDGES if SHOW_EDGES else []):
        sa = 0 if GROUP[a] == 'ext' else STEP[a]
        sb = 0 if GROUP[b] == 'ext' else STEP[b]
        # 只画"至少一端已存在"的边：图随步骤自然生长，早期不织网
        if min(sa, sb) > s:
            continue
        arrow = '-->' if (sa <= s and sb <= s) else '-.->'
        lines.append('  %s %s %s' % (a, arrow, b))
    return '\n'.join(lines)


def added(s):
    names = [LABEL[nid] for nid in STEP if STEP[nid] == s and GROUP[nid] != 'ext']
    total = sum(1 for nid in STEP if GROUP[nid] != 'ext')
    done = sum(1 for nid in STEP if GROUP[nid] != 'ext' and STEP[nid] <= s)
    text = '、'.join(names) if names else '（无新增，仅收尾）'
    return '%s　｜　累计落地 %d / %d 个模块' % (text, done, total)


def main():
    md = []
    md.append('# 阶梯架构图（S0 – S13）')
    md.append('')
    md.append('每张图节点集合完全相同，**只有颜色变化**，所以把相邻两张并排看，橙色就是这一步新增的东西。')
    md.append('')
    md.append('图是**分层带**：一个色块 = 一个模块，按 ui / core / services / 外部 四层排列；层内不画连线'
              '（连线与数据流见 `docs\\架构总览.md` 的数据面图与 `docs\\详细设计.md` 各步接口），这样相邻两张只差颜色，最容易对比。')
    md.append('')
    md.append('| 颜色 | 含义 |')
    md.append('|---|---|')
    md.append('| 灰蓝实线 | 外部设备 / 客户端（一直存在） |')
    md.append('| 绿色实线 | 已完成（≤ 当前步） |')
    md.append('| 橙色粗线 | **本步新增** |')
    md.append('| 灰色虚线 | 未实现，后续步骤预留的挂点 |')
    md.append('')
    md.append('## 两层结构：先做完整终端工具，再挂扩展')
    md.append('')
    md.append('S1–S7 造出 `desktop.exe` 本体——一个原生 Win32 的完整终端工具（通道 / 终端 / 文件 / 按钮 / 托盘 / 日志），'
              'S7 结束时可以独立交付，且不监听任何端口。S8–S13 是挂在这个工具上的扩展，每项独立开关；'
              '全关掉，本体照常工作。')
    md.append('')
    md.append('| 层 | 步骤 | 内容 |')
    md.append('|---|---|---|')
    for s in range(14):
        layer = '—' if s == 0 else ('本体' if s <= 7 else '扩展')
        md.append('| %s | S%d | %s |' % (layer, s, TITLES[s].split(' ', 1)[1]))
    md.append('')
    md.append('> 本文件由 `scripts/gen_arch_mermaid.py` 生成，请改脚本里的 NODES / EDGES 后重跑，不要手改。')
    md.append('')
    md.append('> 打印建议：默认每两页一组（如 S0+S1），便于同页对照；转 PDF 时用 `docs/pdf-print.css`，'
              '`<div class="pb">` 是不可见的分页标记。')
    md.append('')
    for s in range(14):
        if s % 2 == 0:
            md.append('<div class="pb"></div>')
            md.append('')
        md.append('## %s' % TITLES[s])
        md.append('')
        md.append('本步新增：**%s**' % added(s))
        md.append('')
        md.append('```mermaid')
        md.append(diagram(s))
        md.append('```')
        md.append('')

    with open(OUT_MD, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(md))

    print('wrote', OUT_MD)


if __name__ == '__main__':
    main()
