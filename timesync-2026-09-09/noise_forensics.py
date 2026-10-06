import csv,json,collections
from pathlib import Path
root=Path(__file__).resolve().parent
out=[]
for p in sorted((root/'new-acquisition').glob('*static/*/digital.csv')):
 rate=int(p.parent.name.split('-')[0]); expected=int(p.parent.name.endswith('high')); prev=None;starts={}; widths=[[] for _ in range(4)]; counts=[0]*4; simultaneous=collections.Counter(); residues=collections.Counter();late=0
 for row in csv.DictReader(p.open()):
  t=float(row['Time [s]']);v=[int(row[f'Channel {c}']) for c in range(4)]
  if prev is not None:
   changed=[c for c in range(4) if v[c]!=prev[c]]
   if changed:
    simultaneous[len(changed)]+=1;residues[round(t*rate)%2048]+=1
    if t>.001:late+=1
   for c in changed:
    counts[c]+=1
    if v[c]!=expected:starts[c]=t
    elif c in starts:widths[c].append(round((t-starts.pop(c))*rate))
  prev=v
 out.append({'run':str(p.parent.relative_to(root)),'counts':counts,'late_rows_gt_1ms':late,'simultaneous_channel_changes':dict(simultaneous),'top_sample_mod2048':residues.most_common(4),'excursion_width_samples_top':[collections.Counter(w).most_common(5) for w in widths],'one_sample_excursions':[sum(x==1 for x in w) for w in widths],'excursion_counts':[len(w) for w in widths]})
(root/'noise_forensics_metrics.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
