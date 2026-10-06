"""Recompute descriptive relative GPIO phase statistics from transition CSVs.
No simulation, detrending, startup deletion, or reused reference edges.
Width-qualified results are explicitly provisional when any glitches exist.
"""
import csv, hashlib, json, math
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parent
REPO=Path('E:/Projects/wireless-ir-mocap')
if (ROOT/'historical-evidence/test-output/timing-methods').exists():REPO=ROOT/'historical-evidence'
OUT=ROOT/'analysis';OUT.mkdir(exist_ok=True)

def read_edges(path):
    prev={};starts={};rises={i:[] for i in range(4)};pulses={i:[] for i in range(4)};bad={i:[] for i in range(4)}
    last=-math.inf
    with path.open(newline='') as f:
        for row in csv.DictReader(f):
            t=float(row['Time [s]']);assert t>=last;last=t
            for c in range(4):
                v=int(row[f'Channel {c}']);assert v in (0,1)
                if prev.get(c)==0 and v==1:rises[c].append(t);starts[c]=t
                if prev.get(c)==1 and v==0 and c in starts:
                    start=starts.pop(c);w=t-start
                    (pulses[c] if .040<=w<=.065 else bad[c]).append((start,w))
                prev[c]=v
    return rises,pulses,bad

def match(ref,other):
    # Greedy by increasing absolute distance, then sort in reference time.
    # Complete pulses only; nearest phase threshold +/-0.5 s, one-to-one.
    candidates=sorted((abs(t-r),j,k,r,t) for j,r in enumerate(ref) for k,t in enumerate(other) if abs(t-r)<.5)
    used_r=set();used_o=set();pairs=[]
    for _,j,k,r,t in candidates:
        if j not in used_r and k not in used_o:
            used_r.add(j);used_o.add(k);pairs.append((r,(t-r)*1e6))
    return np.array(sorted(pairs),dtype=float).reshape(-1,2)

def metrics(arr):
    if len(arr)==0:return {'n':0}
    x=arr[:,1];t=arr[:,0];slope=float(np.polyfit(t-t.mean(),x,1)[0]) if len(x)>1 else None
    return dict(n=len(x),mean_us=float(x.mean()),sd_us=float(x.std(ddof=0)),rms_us=float(np.sqrt(np.mean(x*x))),
        p95_abs_us=float(np.quantile(abs(x),.95,method='nearest')),max_abs_us=float(max(abs(x))),
        min_us=float(min(x)),max_us=float(max(x)),trend_us_per_s=slope)

def collect():
    runs=[];manifest=[];offset_rows=[]
    files=list((REPO/'test-output/timing-methods').glob('*/*/result.json'))+list((ROOT/'new-acquisition').glob('*campaign/*/result.json'))
    files+=[REPO/'test-output/pipeline/20260908-175800-tracking/result.json']
    for file in sorted(files):
        obj=json.loads(file.read_text());path=file.parent/'digital.csv'
        new=(ROOT/'new-acquisition') in file.parents
        rid=str(file.parent.relative_to(ROOT if new else REPO)).replace('\\','/')
        settings=file.parent.parent/'settings.json';cfg=json.loads(settings.read_text()) if settings.exists() else {}
        row=dict(run=rid,new=new,method=obj.get('method','mcpwm:ftm-camera-load'),duration=obj.get('duration',60),
            build=obj.get('build',cfg.get('build',cfg.get('build_id','unknown'))),error=obj.get('error'),pairs={},channels={})
        if path.exists():
            rises,pulses,bad=read_edges(path)
            row['signal_clean']=all(not bad[c] for c in range(4))
            row['channels']={str(c):dict(raw=len(rises[c]),qualified=len(pulses[c]),rejected=len(bad[c])) for c in range(4)}
            for c in range(4):
                periods=np.diff([p[0] for p in pulses[c]])
                row['channels'][str(c)]['cadence_bad_intervals']=int(np.count_nonzero((periods<.99)|(periods>1.01)))
                row['channels'][str(c)]['intervals_over_1_5_s']=int(np.count_nonzero(periods>1.5))
                row['channels'][str(c)]['period_min_s']=float(min(periods)) if len(periods) else None
                row['channels'][str(c)]['period_max_s']=float(max(periods)) if len(periods) else None
                row['channels'][str(c)]['unclosed_observed_rises']=len(rises[c])-len(pulses[c])-len(bad[c])
            for c in (0,1,3):
                arr=match([p[0] for p in pulses[2]],[p[0] for p in pulses[c]])
                row['pairs'][str(c)]=metrics(arr)
                row['pairs'][str(c)].update(unmatched_reference=len(pulses[2])-len(arr),unmatched_follower=len(pulses[c])-len(arr))
                for t,delta in arr:offset_rows.append(dict(run=rid,method=row['method'],node=c,time_s=t,offset_us=delta))
            row['minimum_matched']=min(x['n'] for x in row['pairs'].values())
            row['classification']='clean descriptive' if row['signal_clean'] and row['minimum_matched']>=30 else 'provisional glitches' if row['minimum_matched']>=30 else 'insufficient/full-fleet inconclusive'
            if row['classification']=='clean descriptive' and (any(x['cadence_bad_intervals'] for x in row['channels'].values()) or row['minimum_matched']<row['duration']-2):
                row['classification']='clean signal; cadence/completeness criterion failed'
            if row['error']:row['classification']='acquisition error with partial raw data'
        else:row['classification']='acquisition failed / no raw CSV'
        for artifact in (file,path,file.parent/'capture.sal',settings):
            if artifact.exists():manifest.append(dict(path=str(artifact.relative_to(ROOT if new else REPO)),root='report' if new else 'historical-evidence',sha256=hashlib.sha256(artifact.read_bytes()).hexdigest(),bytes=artifact.stat().st_size))
        if '172251' in rid:row['classification']='inconclusive disconnected probe / reconnection'
        if '171431' in rid:row['classification']='known pre-repair epoch failure'
        health_file=file.parent/'health-after.json'
        if health_file.exists():
            hh=json.loads(health_file.read_text())
            row['observed_builds_after']={k:v.get('build_id') for k,v in hh.items()}
            if any(v.get('build_id')!=row['build'] for v in hh.values()):
                row['classification']='INVALID: firmware changed during campaign'
        row['primary_eligible']=(new and '20260909-065110-campaign' in rid and not row['error'] and path.exists() and row.get('signal_clean',False) and row.get('minimum_matched',0)>=30 and row['classification'].startswith('clean') and row.get('observed_builds_after')=={str(i):'507f8d74b8d34447' for i in range(4)})
        if row['primary_eligible']:row['block']=int(file.parent.name.split('-')[0])+1
        runs.append(row)
    (OUT/'run_metrics.json').write_text(json.dumps(runs,indent=2))
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2))
    with (OUT/'offsets.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=['run','method','node','time_s','offset_us']);w.writeheader();w.writerows(offset_rows)
    with (OUT/'per_run_node.csv').open('w',newline='') as f:
        fields=['run','method','build','duration','classification','node','n','mean_us','sd_us','rms_us','p95_abs_us','max_abs_us','trend_us_per_s']
        w=csv.DictWriter(f,fieldnames=fields,extrasaction='ignore');w.writeheader()
        for r in runs:
            for c,m in r['pairs'].items():w.writerow({**r,**m,'node':c})
    return runs,offset_rows

def figures(runs,offsets):
    colors={0:'#0077BB',1:'#EE7733',3:'#009988'}
    fig,axs=plt.subplots(1,3,figsize=(10,3.3),sharey=True)
    old=['173016/00-mcpwm-ftm','173016/01-mcpwm-ftm','174159/00-mcpwm-ftm']
    for ax,key in zip(axs,old):
        for c,col in colors.items():
            data=[x for x in offsets if key in x['run'] and x['node']==c]
            if data:ax.plot([x['time_s'] for x in data],[x['offset_us'] for x in data],'.-',ms=3,lw=.7,label=f'Node {c}',color=col)
        ax.set_title('8 Sept '+key.split('/')[0][-6:]);ax.set_xlabel('Capture time (s)');ax.axhline(0,color='gray',lw=.5);ax.grid(alpha=.15)
    axs[0].set_ylabel('Follower minus node 2 (µs)');axs[-1].legend(fontsize=8)
    fig.suptitle('Epoch-repaired FTM + MCPWM: all retained pulse offsets',fontsize=12);fig.tight_layout()
    fig.savefig(OUT/'historical_ftm.png',dpi=200);fig.savefig(OUT/'historical_ftm.svg');plt.close(fig)
    selected=[r for r in runs if r.get('primary_eligible')]
    if selected:
        fig,axs=plt.subplots(2,1,figsize=(10,6),gridspec_kw={'height_ratios':[1,1]})
        for idx,r in enumerate(selected):
            for c,col in colors.items():
                axs[0].scatter(idx+(c-1)*.055,r['pairs'][str(c)]['p95_abs_us'],color=col,s=25,label=f'Node {c}' if idx==0 else None)
                axs[1].scatter(idx+(c-1)*.055,r['pairs'][str(c)]['sd_us'],color=col,s=25)
        labels=[r['method'].split(':')[-1]+'\n'+str(r['block']) for r in selected]
        for ax,lab in zip(axs,['p95 absolute phase (µs)','Offset SD (µs)']):
            ax.set_yscale('log');ax.set_ylabel(lab);ax.set_xticks(range(len(selected)),labels,fontsize=8);ax.grid(axis='y',alpha=.2)
        axs[0].legend(ncol=3,fontsize=8);axs[1].set_xlabel('Source / repeat (chronological campaign order)')
        clean=all(r['signal_clean'] for r in selected)
        fig.suptitle('Same firmware after reset: '+('clean signals' if clean else 'PROVISIONAL subsets; glitches present'),fontsize=12);fig.tight_layout()
        fig.savefig(OUT/'new_comparison.png',dpi=200);fig.savefig(OUT/'new_comparison.svg');plt.close(fig)
    print(json.dumps([dict(run=r['run'],method=r['method'],status=r['classification'],counts=r['channels'],pairs=r['pairs']) for r in runs if r['new']],indent=2))

if __name__=='__main__':
    assert np.allclose(match([1,2],[1.000001,2.000002])[:,1],[1,2])
    assert len(match([1],[.9,1.1]))==1
    assert len(match([1],[1.6]))==0
    figures(*collect())
