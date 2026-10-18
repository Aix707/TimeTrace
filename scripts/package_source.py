"""Package reviewable source, current docs and non-private verification summaries."""
import hashlib,pathlib,zipfile,re
root=pathlib.Path(__file__).resolve().parents[1]
version=re.search(r'project\(TimeTrace VERSION ([0-9.]+)',(root/'CMakeLists.txt').read_text()).group(1)
tag='v'+version.replace('.','')
output=root/'dist'/f'TimeTrace-{version}-Source.zip'
files=[]
for directory in ('src','tests','scripts','assets','third_party'):
 files.extend(p for p in (root/directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
for name in ('CMakeLists.txt','README.md','LICENSE.txt','.clang-format','.gitignore','design-qa.md','VALIDATION.md'):
 p=root/name
 if p.is_file():files.append(p)
for name in ('README.md','ARCHITECTURE.md','DESIGN.md',f'RELEASE-{version}.md','TEST-REPORT.md','overview.png','week.png','detail.png','reminders.png','settings.png','ui-reference.png','ui-comparison.png','native-overview.png'):
 files.append(root/'docs'/name)
files.extend(p for p in (root/'docs'/'verification').glob(tag+'*') if p.is_file())
with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
 for p in sorted(set(files)):archive.write(p,p.relative_to(root))
with zipfile.ZipFile(output) as archive:
 if archive.testzip():raise SystemExit('Invalid source archive')
 if any(n.startswith(('test-output/','build/','dist/')) or n.endswith(('.db','.db-wal','.db-shm')) for n in archive.namelist()):raise SystemExit('Unexpected data in source archive')
print(f'{output.name}: {output.stat().st_size:,} bytes, {len(files)} files')
paths=[root/'dist'/'TimeTrace.exe',root/'dist'/f'TimeTrace-{version}-Setup.exe',output]
(root/'dist'/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n' for p in paths),encoding='utf-8')
