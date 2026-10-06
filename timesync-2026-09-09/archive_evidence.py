"""Copy retained primary evidence into a self-contained report bundle."""
import hashlib,json,shutil,zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parent
REPO=Path('E:/Projects/wireless-ir-mocap')
DEST=ROOT/'historical-evidence'
paths=[]
for result in (REPO/'test-output/timing-methods').glob('*/*/result.json'):
    paths.extend(p for p in result.parent.iterdir() if p.name in {'result.json','digital.csv','capture.sal','configured.json','armed.json','telemetry.json'})
    settings=result.parent.parent/'settings.json'
    if settings.exists():paths.append(settings)
for name in ['result.json','capture.sal','digital.csv','extra-edges.json','summary.json']:
    paths.append(REPO/'test-output/pipeline/20260908-175800-tracking'/name)
for name in ['TIMESYNC_MEASUREMENT_2026-09-08.md','TIMESYNC_METHOD_RESULTS_2026-09-08.md','TIMESYNC_EXPERIMENT_LOG_2026-09-08.md','PIPELINE_TIMESYNC_2026-09-08.md']:
    paths.append(REPO/'docs'/name)
for ota in ['1788910716585756100','1788911291048392600']:
    paths.extend((REPO/'test-output/ota-fleet'/ota).glob('*/verified.json'))
manifest=[]
for src in sorted(set(paths)):
    if not src.exists():continue
    dest=DEST/src.relative_to(REPO);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
    manifest.append({'original':str(src),'copy':str(dest.relative_to(ROOT)).replace('\\','/'),'sha256':hashlib.sha256(src.read_bytes()).hexdigest()})
(ROOT/'historical_manifest.json').write_text(json.dumps(manifest,indent=2))
print('Archived',len(manifest),'primary evidence files')
