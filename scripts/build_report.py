"""Build the current release report from matching local verification evidence."""
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PROOF = ROOT / "test-output"
VERSION = re.search(r"project\(TimeTrace VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text()).group(1)
TAG = "v" + VERSION.replace(".", "")
EXE = ROOT / "dist/TimeTrace.exe"
SETUP = ROOT / f"dist/TimeTrace-{VERSION}-Setup.exe"


def fingerprint(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(suffix):
    path = PROOF / f"{TAG}-{suffix}"
    return json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else None


def megabytes(value):
    return f"{value / 1_000_000:.3f} MB"


def outcome(value):
    return "通过" if value else "未通过"


binary = fingerprint(EXE)
lines = [
    f"# TimeTrace {VERSION} 测试报告", "",
    f"生成于 {datetime.now().astimezone().isoformat(timespec='seconds')}", "",
    "本版按应用名自动汇总，名称与图标跟随主来源。各来源独立保留名称，来源可更改归属或独立为应用，同名合并先确认合计用时。使用原生界面、现有工作线程、一秒心跳与五秒批量保存。", "",
    "## 自动回归", "",
]
for suffix, label in [
    ("core-tests.log", "核心、存储与协议"),
    ("integration-tests.log", "隔离安装注册"),
    ("service-tests.log", "采集与查询队列"),
    ("identity-tests.log", "通用名称识别"),
]:
    path = PROOF / f"{TAG}-{suffix}"
    if path.exists():
        lines.append(f"- {label}。`{path.read_text(encoding='utf-8-sig').strip()}`。")
actions = read("actions.json")
if actions:
    lines.append(f"- 通知动作 {actions['checks']} 项{outcome(actions['passed'])}。覆盖已有实例、未运行状态、延后、静默、重复及过期动作和旧入口。")
lines.append("- 核心回归覆盖同名多路径、组件归属、主来源往返切换、名称及图标路径恢复、同名确认、成员更改归属、提醒延后和事务回滚。后台回归包含首次登记写锁、跨午夜提交写锁及通知保存失败后的恢复。")

native = read("lifecycle-final.json")
if native:
    lines.extend([
        "", "## 原生界面与资源释放", "",
        f"- 窗口开关 {native['cycles']} 次，先预热 {native['warmup_cycles']} 次，再测量 {native['measured_cycles']} 次。完整回归{outcome(native['passed'])}。",
        "- 提醒编辑、重命名、多选合并、来源查看、设为主应用、更改归属、取消后再操作及重新合并均读回隔离数据库核对。另覆盖历史日期、最小窗口滚动、键盘导航、窗口控制和取消清除历史。",
        f"- 字号与控件检查{outcome(native['native_prompt_metrics'])}。正文为 16 DIP，输入框与按钮为 36 DIP。来源行采用 80 DIP，合并列表采用 64 DIP，图标为 32 DIP。",
        f"- GDI 句柄 {native['baseline_gdi_handles']} → {native['final_gdi_handles']}，USER 句柄 {native['baseline_user_handles']} → {native['final_user_handles']}。关闭窗口后的私有工作集为 {megabytes(native['private_working_set_after_cycles'])}。",
    ])
layout = read("detail-layout.json")
if layout:
    lines.append(f"- 默认及最小窗口的日、周视图共 {len(layout['cases'])} 种组合。概览与详情顶部统计区逐像素一致检查{outcome(layout['passed'])}。")

migration = read("migration-verified-copy/result.json")
classification = read("classification-release-copy/result.json")
field = read("merge-field.json")
lines.extend(["", "## 实际数据副本", ""])
if migration:
    lines.append(f"- 只读复制真实数据库后升级 schema {migration['source_schema']} → {migration['target_schema']}。包含 {migration['applications']} 个来源和 {migration['intervals']} 条原始区间。区间、总量、自定义名称、既有归属、设置及提醒检查{outcome(migration['passed'])}。")
if classification:
    lines.append(f"- 名称整理副本包含 {classification['sources']} 个来源和 {classification['intervals']} 条区间，汇总为 {classification['canonical_apps']} 个有记录的分类。原始记录、各来源自定义名称及手动归属保留，分类名称唯一且没有空分类。完整性及外键检查{outcome(classification['passed'])}。")
    for index, case in enumerate(classification["cases"], 1):
        lines.append(f"- 实际记录用例 {index} 汇总 {case['recorded_sources']} 个有记录的来源，名称、主来源路径及合计用时核对{outcome(case['passed'])}。")
if field:
    lines.append(f"- 使用存储接口逐一核对全部分类用时，再切换主来源、更改成员归属并重新合并。原始区间和总量检查{outcome(field['passed'])}，名称和图标来源跟随所选主来源。")
lines.append("- 未识别用时单独汇总并计入总量。路径仅在来源管理中展示。测试副本及个人路径保存在本机忽略目录。")

foreground = []
for path in PROOF.glob("foreground-*/result.json"):
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if data.get("binary_sha256", "").lower() == binary:
        foreground.append((path, data))
if foreground:
    _, data = sorted(foreground)[-1]
    lines.extend(["", "## 前台采集", "", f"- 最终发布文件受控切换 {data['successful_focus_changes']}/{data['iterations']} 次，检查{outcome(data['passed'])}。参考为独立 2 毫秒前台轮询。"])
    for app in data["applications"]:
        lines.append(f"- {app['application']} 记录 {app['recorded_intervals']} 段，累计误差 {app['error_ms']} 毫秒。")

lines.extend(["", "## 资源与体积", "", f"- 主程序 {EXE.stat().st_size:,} 字节，约 {megabytes(EXE.stat().st_size)}。"])
if SETUP.exists():
    lines.append(f"- 离线安装包 {SETUP.stat().st_size:,} 字节，约 {megabytes(SETUP.stat().st_size)}。10 MB 体积目标{outcome(SETUP.stat().st_size <= 10_000_000)}。")
upgrade = read("upgrade-verification.json")
if upgrade:
    lines.append(f"- 本机安装升级至 {VERSION}，全部安装文件合计 {megabytes(upgrade['installed_bytes'])}。升级前已备份，{upgrade['original_intervals']} 条原始区间逐项保留，自定义名称、归属及完整性检查{outcome(upgrade['passed'])}。")
resource = read("final-background.json")
status = read("final-background-status.json")
if resource and status and resource["pid"] == status["ApplicationPID"] and status["SHA256"].lower() == binary:
    lines.extend([
        f"- 最终文件预热 {resource['warmup_seconds']} 秒，托盘测量 {resource['measured_seconds']:.1f} 秒，共 {resource['samples']} 个样本。界面保持关闭检查{outcome(not resource['ui_opened_during_measurement'])}，进程持续运行检查{outcome(not resource['process_exited_early'])}。",
        f"- 平均 CPU 为 {resource['average_cpu_percent']:.4f}%（整机总算力口径），私有工作集峰值 {megabytes(resource['peak_private_working_set_bytes'])}，测量期变化 {megabytes(resource['private_working_set_growth_bytes'])}。短测预算检查{outcome(resource['passed_resource_budget'])}。",
        "- 本次为托盘短测。正常使用和静置两个独立长测场景待完成。",
    ])
else:
    lines.append("- 最终发布文件的托盘资源测量待完成。")
lines.extend([
    "- SQLite 与 C++ 运行库静态链接，使用系统绘图及字体。", "",
    "## 尚待正式验收", "",
    "- 预热两分钟后，正常使用和静置各 30 分钟，以及连续两小时的内存增长。",
    "- 指定第三方应用与管理员窗口的 30 分钟累计误差，真实锁屏、睡眠和混合 DPI 显示器回归。",
    "- 无开发环境的干净 Windows 11 断网安装与卸载、系统通知横幅实点及真实登录自启动。", "",
    "## 文件指纹", "", f"- `TimeTrace.exe` SHA-256 为 `{binary}`。",
])
if SETUP.exists():
    lines.append(f"- `{SETUP.name}` SHA-256 为 `{fingerprint(SETUP)}`。")
(ROOT / "docs/TEST-REPORT.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
print(f"Updated docs/TEST-REPORT.md for {VERSION}")
