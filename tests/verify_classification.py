"""Verify application-name classification on an isolated copy of recorded usage."""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("auditor", type=Path)
parser.add_argument("--name", action="append", default=[])
args = parser.parse_args()
folder = args.output.resolve()
folder.mkdir(parents=True, exist_ok=True)
copy = folder / "data.db"
if copy.exists() or "test-output" not in folder.parts:
    raise SystemExit("Use a new output folder under test-output")


def rows(connection, table):
    return connection.execute(f"SELECT * FROM {table} ORDER BY 1,2").fetchall()


def digest(items):
    value = json.dumps(items, ensure_ascii=False, separators=(",", ":")).encode()
    return hashlib.sha256(value).hexdigest()


with sqlite3.connect(args.source.resolve().as_uri() + "?mode=ro", uri=True) as source:
    with sqlite3.connect(copy) as target:
        source.backup(target)

with sqlite3.connect(copy) as connection:
    before = {table: rows(connection, table) for table in ("segments", "app_labels", "settings")}
    assignments = connection.execute("SELECT app,target FROM app_links WHERE manual=1").fetchall()

subprocess.run(
    [str(args.auditor.resolve()), str(copy), str(folder / "private-audit.json"), "--apply"],
    check=True,
    creationflags=subprocess.CREATE_NO_WINDOW,
)

with sqlite3.connect(copy) as connection:
    after = {table: rows(connection, table) for table in before}
    links = dict(connection.execute("SELECT app,target FROM app_links"))
    roots = {row[0]: row for row in connection.execute(
        "SELECT a.id,COALESCE(n.name,NULLIF(a.resolved_name,''),a.name),a.path,a.has_usage "
        "FROM apps a LEFT JOIN app_labels n ON n.app=a.id "
        "WHERE a.identified=1 AND NOT EXISTS(SELECT 1 FROM app_links WHERE app=a.id)"
    )}
    usage = dict(connection.execute(
        "SELECT COALESCE(m.target,s.app),SUM(s.duration) FROM segments s "
        "LEFT JOIN app_links m ON m.app=s.app GROUP BY COALESCE(m.target,s.app)"
    ))
    visible = {key: row for key, row in roots.items() if row[3]}
    names = [row[1].strip().lower() for row in visible.values()]
    cases = []
    recorded_sources = {row[1] for row in after["segments"]}
    for name in args.name:
        matches = [row for row in visible.values() if row[1].strip().lower() == name.lower()]
        source_ids = []
        if len(matches) == 1:
            root, _, primary_path, _ = matches[0]
            source_ids = [key for key in recorded_sources if links.get(key, key) == root]
            total = sum(row[4] for row in after["segments"] if row[1] in source_ids)
            valid = bool(primary_path) and total == usage.get(root, 0) and total > 0
        else:
            valid = False
        cases.append({"name": name, "recorded_sources": len(source_ids), "passed": valid})
    result = {
        "sources": connection.execute("SELECT COUNT(*) FROM apps").fetchone()[0],
        "intervals": len(before["segments"]),
        "canonical_apps": len(visible),
        "raw_intervals_unchanged": before["segments"] == after["segments"],
        "source_labels_unchanged": before["app_labels"] == after["app_labels"],
        "settings_unchanged": before["settings"] == after["settings"],
        "manual_assignments_retained": all(links.get(app, app) == links.get(target, target)
                                           for app, target in assignments),
        "unique_application_names": len(names) == len(set(names)),
        "no_empty_applications": all(usage.get(key, 0) > 0 for key in visible),
        "integrity_ok": connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok",
        "foreign_keys_ok": not connection.execute("PRAGMA foreign_key_check").fetchall(),
        "segments_sha256": digest(after["segments"]),
        "cases": cases,
    }
result["passed"] = all(value for value in result.values() if isinstance(value, bool)) and all(
    case["passed"] for case in cases
)
(folder / "result.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps(result, ensure_ascii=True, indent=2))
raise SystemExit(0 if result["passed"] else 1)
