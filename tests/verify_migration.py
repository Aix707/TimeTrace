"""Read-only backup of a user DB, migrate ONLY the copy, compare raw records."""
import argparse, hashlib, json, pathlib, sqlite3, subprocess
p=argparse.ArgumentParser();p.add_argument('input',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('exe',type=pathlib.Path);a=p.parse_args()
source=a.input.resolve();folder=a.output.resolve();folder.mkdir(parents=True,exist_ok=True);copy=folder/'data.db'
if copy==source or copy.exists():raise SystemExit('Use a new isolated output folder')
def rows(db,table):
    if not db.execute('select 1 from sqlite_master where type="table" and name=?',(table,)).fetchone():return []
    return db.execute('select * from '+table+' order by 1,2').fetchall()
def digest(items):return hashlib.sha256(json.dumps(items,ensure_ascii=False,separators=(',',':')).encode()).hexdigest()
with sqlite3.connect(source.as_uri()+'?mode=ro',uri=True) as original,sqlite3.connect(copy) as target:original.backup(target)
with sqlite3.connect(copy) as db:
    version=db.execute('pragma user_version').fetchone()[0]
    tables=('apps','segments','app_labels','app_links','settings')
    if version>=3:tables+=('reminder_rules','reminder_states')
    before={t:rows(db,t) for t in tables}
    thresholds=rows(db,'limits')
    total=db.execute('select coalesce(sum(duration),0) from segments').fetchone()[0]
subprocess.run([str(a.exe.resolve()),'--data-dir',str(folder),'--render',str(folder/'migration-check.png'),'--page','reminders'],check=True,creationflags=subprocess.CREATE_NO_WINDOW)
with sqlite3.connect(copy) as db:
    after={t:rows(db,t) for t in before}
    schema=db.execute('pragma user_version').fetchone()[0]
    unchanged={t:before[t]==after[t] for t in before}
    totals_equal=total==db.execute('select coalesce(sum(duration),0) from segments').fetchone()[0]
    rules=db.execute('select app,duration from reminder_rules where kind=1').fetchall()
    legacy_ok=version>=3 or all((app,duration) in rules for app,duration in thresholds)
    device=db.execute('select duration,enabled from reminder_rules where kind=0').fetchone()
result={'source_opened_read_only':True,'source_schema':version,'target_schema':schema,'applications':len(before['apps']),'intervals':len(before['segments']),'recorded_milliseconds':total,'unchanged_tables':unchanged,'segments_sha256_before':digest(before['segments']),'segments_sha256_after':digest(after['segments']),'total_unchanged':totals_equal,'legacy_thresholds_preserved':legacy_ok,'new_device_disabled_sixty_minutes':device==(3600000,0)}
result['passed']=schema==4 and all(unchanged.values()) and totals_equal and legacy_ok and (version>=3 or result['new_device_disabled_sixty_minutes'])
(folder/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result,indent=2))
raise SystemExit(0 if result['passed'] else 1)
