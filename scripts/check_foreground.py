"""Compare independent foreground timestamps with the persisted native tracker output."""
import csv,json,pathlib,sqlite3,sys
folder=pathlib.Path(sys.argv[1])
events=list(csv.DictReader((folder/'events.csv').open()))
observed=list(csv.DictReader((folder/'events.csv.observed.csv').open()))
db=sqlite3.connect(folder/'data'/'data.db')
results=[]
for index,name in enumerate(('ProbeA.exe','ProbeB.exe')):
    pid=int(events[index]['expected_pid'])
    expected=sum(int(observed[i+1]['utc_ms'])-int(row['utc_ms']) for i,row in enumerate(observed[:-1]) if int(row['pid'])==pid)
    actual,count=db.execute('SELECT COALESCE(SUM(s.duration),0),COUNT(*) FROM segments s JOIN apps a ON s.app=a.id WHERE a.path=? COLLATE NOCASE',(str(folder/name),)).fetchone()
    results.append({'application':name,'expected_ms':expected,'recorded_ms':actual,'error_ms':actual-expected,'recorded_intervals':count})
focused=sum(int(row['focused']) for row in events[:-1])
passed=focused==len(events)-1 and all(abs(r['error_ms'])<=1000 and r['recorded_intervals']>=((len(events)-1)//2) for r in results)
report={'iterations':len(events)-1,'successful_focus_changes':focused,'applications':results,'passed':passed,'reference':'Independent 2 ms foreground polling; excludes intervening third-party focus changes.','note':'Controlled native windows, two executable paths; does not establish compatibility with every third-party application.'}
if (folder/'binary.sha256').exists():report['binary_sha256']=(folder/'binary.sha256').read_text(encoding='utf-8-sig').strip()
(folder/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
sys.exit(0 if passed else 1)
