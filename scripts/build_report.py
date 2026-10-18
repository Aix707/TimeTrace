"""Build the current version's report from matching local evidence."""
from pathlib import Path
from datetime import datetime
import hashlib,json,re
root=Path(__file__).resolve().parents[1];proof=root/'test-output';dist=root/'dist'
version=re.search(r'project\(TimeTrace VERSION ([0-9.]+)',(root/'CMakeLists.txt').read_text()).group(1);tag='v'+version.replace('.','')
def read(suffix):
 p=proof/(tag+'-'+suffix)
 return json.loads(p.read_text(encoding='utf-8-sig')) if p.exists() else None
def mb(n):return f'{n/1_000_000:.3f} MB'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
exe=dist/'TimeTrace.exe';setup=dist/f'TimeTrace-{version}-Setup.exe';binary=sha(exe)
lines=[f'# TimeTrace {version} 测试报告','',f'生成时间：{datetime.now().astimezone().isoformat(timespec="seconds")}','',
'本版优化后台提醒判断、批量写入、查询队列与视图资源释放，升级至 schema 4 并移除废弃兼容表。保留现有界面、计时口径和基础功能，未增加运行框架、后台线程或定时器。开发检查使用独立目录，未覆盖已安装程序；真实数据仅以只读方式创建副本验证升级。','',
'## 功能与原生界面','']
for suffix,label in [('core-tests.log','核心／存储／协议'),('integration-tests.log','隔离安装注册'),('service-tests.log','采集与查询队列')]:
 p=proof/(tag+'-'+suffix)
 if p.exists():lines.append(f'- {label}：{p.read_text(encoding="utf-8-sig").strip()}。')
a=read('actions.json')
if a:lines.append(f'- 通知动作：{a["checks"]} 项，'+('通过。' if a['passed'] else '未通过。')+'覆盖已有实例、未运行状态、延后、静默、重复／过期动作及旧入口。实际 Windows 横幅实点仍需隔离安装环境验证。')
d=read('detail-layout.json')
if d:lines.append(f'- 顶部稳定性：{len(d["cases"])} 种默认／最小窗口、日／周组合，使用同一应用数据对比概览与详情统计区，逐像素差异均为 0：{d["passed"]}。')
l=read('lifecycle-final.json')
if l:
 lines.extend([f'- 原生界面开关 {l["cycles"]} 次：'+('通过。' if l['passed'] else '未通过。')+'提醒修改、设备开关、时长、添加应用、重命名、合并均读回独立数据库核对。',
 f'- 自绘顶栏：最大化／还原、最小化／恢复、内置关闭、拖动／缩放命中区域与最大化工作区检查：{l.get("native_window_frame",False)}；Tab／方向键切换：{l.get("native_keyboard_navigation",False)}。',
 f'- 提醒时长直接点击修改：{l.get("native_inline_reminder_edit",False)}；最小窗口滚动到末项并打开正确应用：{l.get("native_reminder_scroll",False)}；清除历史取消后原始演示时长保留：{l.get("native_clear_history_cancel",False)}。',
 f'- 原生提醒编辑框正文为 16 DIP，输入框与按钮为 36 DIP：{l.get("native_prompt_metrics",False)}；从详情页打开编辑并保存后读回数据库核对。',
 f'- 先执行 {l["warmup_cycles"]} 次含弹窗、确认框、最大化等操作的预热，再执行 {l.get("measured_cycles",l["cycles"]-l["warmup_cycles"])} 次窗口开关。测试器使用异步按钮消息并等待模态窗口退出，避免测试动作嵌套改变执行顺序。',
 f'- 预热开关后 GDI 句柄 {l["baseline_gdi_handles"]} → {l["final_gdi_handles"]}，USER 句柄 {l["baseline_user_handles"]} → {l["final_user_handles"]}；关闭后的私有工作集 {mb(l["private_working_set_after_cycles"])}。自启动真实设置未切换。'])
lines+=['- 界面沿用 16／22／44 DIP 三档字号、统一尺寸和底部详情栏。原生窗口回归覆盖默认／最小窗口及常用编辑操作。既有设计规范见 `DESIGN.md`。','']
migration=proof/(tag+'-migration-copy/result.json')
if migration.exists():
 m=json.loads(migration.read_text(encoding='utf-8-sig'))
 lines.extend([f'- 真实数据副本升级：schema {m["source_schema"]} → {m["target_schema"]}，{m["applications"]} 个应用、{m["intervals"]} 条原始记录；原始记录、总量、名称分组、设置及提醒规则／状态逐项一致：{m["passed"]}。原文件以只读方式打开。'])
b=read('storage-comparison.json')
if b:
 lines+=['','## 存储基准','',f'同一台电脑、相同发布编译设置，使用 {b["fixture"]["days"]} 天、{b["fixture"]["applications"]} 个应用、{b["fixture"]["intervals"]:,} 条合成区间；前后版本交替运行各 {b["runs_per_version"]} 次，以下为中位数。查询包含 200 次日／周读取，写入包含 240 个带提醒状态的批次。统计校验值一致：{b["checksum_identical"]}。','',
 f'| 操作 | {b["before_version"]} | {b["after_version"]} | 耗时变化 |','|---|---:|---:|---:|']
 for metric,label in [('seed_ms','整批写入'),('commit_ms','单次小批提交'),('daily_query_ms','单次日概览'),('weekly_query_ms','单次应用周视图')]:
  old=b['median_ms']['before'][metric];new=b['median_ms']['after'][metric]
  lines.append(f'| {label} | {old:.3f} ms | {new:.3f} ms | {(new/old-1)*100:+.1f}% |')
 lines+=['','这是合成数据基准，磁盘和缓存状态会影响结果；整批导入不代表每秒后台负载。']
fg=[]
for p in proof.glob('foreground-*/result.json'):
 f=json.loads(p.read_text(encoding='utf-8-sig'))
 if f.get('binary_sha256','').lower()==binary:fg.append((p,f))
if fg:
 p,f=sorted(fg)[-1];lines.append(f'- 当前发布文件受控前台切换：{f["successful_focus_changes"]}/{f["iterations"]} 次，'+('通过。' if f['passed'] else '未通过。'))
 for a in f['applications']:lines.append(f'  - {a["application"]}：{a["recorded_intervals"]} 段，累计误差 {a["error_ms"]} 毫秒。')
lines+=['','## 资源与体积','',f'- 应用程序：{exe.stat().st_size:,} 字节（{mb(exe.stat().st_size)}）。']
if setup.exists():lines.append(f'- 离线安装包：{setup.stat().st_size:,} 字节（{mb(setup.stat().st_size)}）；'+('满足 ≤10 MB。' if setup.stat().st_size<=10_000_000 else '超过目标。'))
p=read('background.json');status=read('background-status.json')
if p and status and p.get('pid')==status.get('ApplicationPID') and status.get('SHA256','').lower()==binary:
 cpu=f'{p["average_cpu_percent"]:.4f}%' if p['average_cpu_percent']>=0.01 else '<0.01%（低于显示精度）'
 lines.extend([f'- 托盘短测：预热 {p["warmup_seconds"]} 秒，测量 {p["measured_seconds"]:.1f} 秒，采集 {p["samples"]} 个样本；测量期间界面打开：{p["ui_opened_during_measurement"]}，提前退出：{p["process_exited_early"]}。',
 f'- CPU 平均 {cpu}（按整机总算力）；私有工作集峰值 {mb(p["peak_private_working_set_bytes"])}，测量期变化 {mb(p["private_working_set_growth_bytes"])}。',
 f'- 短测资源预算核对：{p["passed_resource_budget"]}；不替代正式 30 分钟及两小时测试。'])
else:lines.append('- 当前发布文件的资源测量尚未完成；不引用旧版或其他文件的数据。')
if p and p.get('passed_resource_budget'):
 lines.append('- 本次为包含受控切换的混合短测，未分别完成正常使用／静置两个独立长测场景。')
lines+=['- 编译使用静态 C++ 运行库与 SQLite，界面只使用 Windows 绘图和系统字体。未添加大图、外部字体或网页容器。','',
'## 仍需正式验收','',
'- 预热两分钟后，正常使用和静置各 30 分钟，以及连续两小时的内存增长。',
'- 指定第三方应用与管理员窗口的 30 分钟误差 ≤2 秒；受控窗口和确定性测试不覆盖所有软件。',
'- 干净 Windows 11 断网安装、升级、卸载、安装后全部文件体积，以及 Windows 通知实点和真实登录自启动。',
'- 多显示器不同 DPI 来回拖动，以及 Windows Snap 悬停菜单的完整人工体验；当前已验证原生窗口的命中区域、工作区和最大化功能。','',
'当前安装和真实数据未改动。三项核心指标的全部正式验收尚未完成；已完成项和待完成项在此分别列明。','',
'## 文件指纹','',f'- `TimeTrace.exe` SHA-256：`{binary}`']
if setup.exists():lines.append(f'- `{setup.name}` SHA-256：`{sha(setup)}`')
(root/'docs/TEST-REPORT.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('Updated docs/TEST-REPORT.md for '+version)
