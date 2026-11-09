"""Open only a schema-7 copy and verify every stored field and database definition."""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("input", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("exe", type=Path)
args = parser.parse_args()
source = args.input.resolve()
folder = args.output.resolve()
copy = folder / "data.db"
if copy == source or copy.exists():
    raise SystemExit("Use a new isolated output folder")
tables = ("apps", "segments", "app_labels", "app_links", "settings",
          "reminder_rules", "reminder_states")


def rows(db, table):
    return sorted(db.execute("SELECT * FROM " + table).fetchall())


def digest(items):
    data = json.dumps(items, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def metadata(db):
    return {"schema": db.execute("PRAGMA user_version").fetchone()[0],
            "definitions": db.execute("SELECT type,name,tbl_name,sql FROM sqlite_schema "
                                      "ORDER BY type,name").fetchall(),
            "sequences": rows(db, "sqlite_sequence")}


with sqlite3.connect(source.as_uri() + "?mode=ro", uri=True) as original:
    if original.execute("PRAGMA user_version").fetchone()[0] != 7:
        raise SystemExit("Verification requires a schema-7 database")
    folder.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(copy) as target:
        original.backup(target)
with sqlite3.connect(copy) as db:
    before = {table: rows(db, table) for table in tables}
    before_meta = metadata(db)
    total = db.execute("SELECT COALESCE(SUM(duration),0) FROM segments").fetchone()[0]
subprocess.run([str(args.exe.resolve()), "--data-dir", str(folder), "--render",
                str(folder / "database-check.png"), "--page", "reminders"],
               check=True, creationflags=subprocess.CREATE_NO_WINDOW)
with sqlite3.connect(copy) as db:
    after = {table: rows(db, table) for table in tables}
    after_meta = metadata(db)
    totals_equal = total == db.execute("SELECT COALESCE(SUM(duration),0) FROM segments").fetchone()[0]
    integrity = db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
    foreign_keys = not db.execute("PRAGMA foreign_key_check").fetchall()
result = {
    "source_opened_read_only": True, "source_schema": before_meta["schema"],
    "target_schema": after_meta["schema"], "applications": len(before["apps"]),
    "intervals": len(before["segments"]), "recorded_milliseconds": total,
    "unchanged_tables": {table: before[table] == after[table] for table in tables},
    "metadata_unchanged": before_meta == after_meta,
    "segments_sha256_before": digest(before["segments"]),
    "segments_sha256_after": digest(after["segments"]),
    "total_unchanged": totals_equal, "integrity_ok": integrity, "foreign_keys_ok": foreign_keys,
    "binary_sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
}
result["passed"] = (after_meta["schema"] == 7 and all(result["unchanged_tables"].values())
                    and result["metadata_unchanged"] and totals_equal and integrity and foreign_keys)
(folder / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["passed"] else 1)
