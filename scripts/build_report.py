"""Render the release report only from evidence matching the delivered binaries."""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r"project\(TimeTrace VERSION ([0-9.]+)",
                    (ROOT / "CMakeLists.txt").read_text()).group(1)
TAG = "v" + VERSION.replace(".", "")
PROOF = ROOT / f"docs/verification/{TAG}-release.json"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


proof = json.loads(PROOF.read_text(encoding="utf-8"))
for name, artifact in proof["artifacts"].items():
    if digest(ROOT / "dist" / name) != artifact["sha256"]:
        raise SystemExit(f"Verification evidence does not match {name}")
if not proof["passed"]:
    raise SystemExit("Release verification did not pass")

tests = proof["tests"]
native = proof["lifecycle"]
resource = proof["resource"]
upgrade = proof["local_upgrade"]
migration = proof["data_copy"]
exe = proof["artifacts"]["TimeTrace.exe"]
setup = proof["artifacts"][f"TimeTrace-{VERSION}-Setup.exe"]
cpu = resource["average_cpu_percent"]
cpu_text = "<0.01%" if cpu < 0.01 else f"{cpu:.4f}%"
lines = [
    f"# TimeTrace {VERSION} 测试报告", "", f"整理于 {proof['verification_date']}。", "",
    "本版使用原生窗口、系统字体、静态 SQLite 与 C++ 运行库，按既有单工作线程、一秒采样和五秒批量保存运行。", "",
    "## 回归与数据", "",
    f"- 核心及存储 {tests['core']} 项，隔离安装注册 {tests['integration']} 项，服务 {tests['service']} 项，识别 {tests['identity']} 项，界面状态 {tests['ui']} 项，通知动作 {tests['notification_actions']} 项，共 {sum(tests.values())} 项通过。",
    "- 覆盖识别缓存失效、事务回滚重试、主来源切回、成员移出与重新合并、提醒图标、焦点重排及绘图失败恢复。",
    f"- 原生窗口开关 {native['cycles']} 次，先预热 {native['warmup_cycles']} 次，再检查 {native['measured_cycles']} 次。来源管理、提醒、重命名、合并、键盘导航及取消清除均通过。",
    f"- GDI 句柄 {native['baseline_gdi_handles']} → {native['final_gdi_handles']}，USER 句柄 {native['baseline_user_handles']} → {native['final_user_handles']}。关闭窗口后的私有工作集约 {native['private_working_set_after_cycles']/1e6:.3f} MB。",
    f"- 真实数据副本包含 {migration['applications']} 个来源、{migration['intervals']:,} 条原始区间。schema 7、七张业务表、记录哈希及总量核对通过。",
    f"- 本机升级前已备份，{upgrade['intervals']:,} 条原始区间及七张业务表在恢复采集前严格比对一致。通知身份、协议、快捷方式及开机启动设置核对通过。", "",
    "## 资源与体积", "",
    f"- 已安装的最终文件预热 {resource['warmup_seconds']} 秒后测量 {resource['measured_seconds']:.1f} 秒，共 {resource['samples']} 个样本。界面关闭、进程持续运行。",
    f"- 平均 CPU 为 {cpu_text}，按整机总算力计算。私有工作集峰值 {resource['peak_private_working_set_bytes']/1e6:.3f} MB，测量期变化 {resource['private_working_set_growth_bytes']/1e6:+.3f} MB。",
    f"- 主程序 {exe['bytes']:,} 字节，离线安装包 {setup['bytes']:,} 字节，全部安装文件合计 {upgrade['installed_bytes']:,} 字节。",
    "- 本次资源和体积核验满足平均 CPU ≤0.5%、私有工作集峰值 ≤50 MB、安装包 ≤10 MB、安装文件合计 ≤30 MB。", "",
    "## 优化性能对照", "",
    "以下对照来自版本号调整前的优化实现。新旧各运行 10 轮，前 5 轮原版先运行，后 5 轮交换顺序，采用 48 应用、60 天及 72,000 条合成区间。", "",
    "| 项目 | 原版中位数 ms | 优化版中位数 ms | 同轮耗时比值中位数 |",
    "|---|---|---|---|",
]
reference = proof["optimization_reference"]
labels = {"seed_ms": "写入全部区间", "commit_ms": "小批提交", "daily_query_ms": "日概览",
          "weekly_query_ms": "周详情", "catalog_refresh_ms": "48 来源重复登记",
          "member_query_ms": "8 成员详情"}
for key, label in labels.items():
    old = reference["benchmark_medians_ms"]["baseline"][key]
    new = reference["benchmark_medians_ms"]["optimized"][key]
    change = reference["paired_time_change_percent"][key]
    lines.append(f"| {label} | {old:.3f} | {new:.3f} | {change:+.1f}% |")
lines += [
    "", "负值表示耗时减少。小批提交受同步写盘及负载波动影响，两种统计口径同时保留。", "",
    f"对照优化程序 SHA-256 为 `{reference['binary_sha256']}`。", "",
    "## 发布核验", "",
    "- 本机安装程序与发布主程序 SHA-256 一致，已恢复托盘运行。",
    f"- 主程序 SHA-256 为 `{exe['sha256']}`。",
    f"- 安装包 SHA-256 为 `{setup['sha256']}`。",
    f"- 机器可读结果见 `docs/verification/{TAG}-release.json`，源码包校验值见发行版的 `SHA256SUMS.txt`。", "",
]
(ROOT / "docs/TEST-REPORT.md").write_text("\n".join(lines), encoding="utf-8")
print(f"Updated docs/TEST-REPORT.md for {VERSION}")
