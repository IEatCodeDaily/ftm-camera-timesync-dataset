"""Static, evidence-linked figures and a six-page visual guide."""
from pathlib import Path
import sys,json,hashlib,shutil
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
from reportlab.pdfgen import canvas
from reportlab.lib.colors import HexColor
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import Paragraph
from reportlab.lib.styles import ParagraphStyle
from PIL import Image
HERE=Path(__file__).resolve().parent;ROOT=HERE.parent
sys.path.insert(0,str(ROOT));from analyze import read_edges
DATA=json.loads((ROOT/'analysis/run_metrics.json').read_text())
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':12,'axes.spines.top':False,'axes.spines.right':False})
INK='#173b54';GREEN='#087f68';BLUE='#277da8';ORANGE='#d77c25';RED='#b7544b';GRAY='#596976'
def save(fig,name):
    fig.savefig(HERE/(name+'.png'),dpi=190,bbox_inches='tight',facecolor='white')
    fig.savefig(HERE/(name+'.svg'),bbox_inches='tight',facecolor='white');plt.close(fig)
def box(ax,x,y,w,h,label,color=INK,fs=13):
    ax.add_patch(FancyBboxPatch((x,y),w,h,boxstyle='round,pad=.015,rounding_size=.025',fc='#f1f6f8',ec=color,lw=1.7))
    ax.text(x+w/2,y+h/2,label,ha='center',va='center',fontsize=fs,color=color)
def arrow(ax,x1,y1,x2,y2,label=None,color=GRAY):
    ax.annotate('',(x2,y2),(x1,y1),arrowprops=dict(arrowstyle='->',color=color,lw=2))
    if label:ax.text((x1+x2)/2,(y1+y2)/2+.025,label,ha='center',fontsize=10,color=color)

# Five clock sources: identical firmware/backend; unequal repetitions explicit.
labels=['Live FTM','Frozen FTM','AP TSF','Simple NTP-style','No timesync']
sources=['ftm','frozen','tsf','ntp','mac'];colors=[GREEN,'#57a99a',BLUE,ORANGE,RED]
rows=[]
for source,label in zip(sources,labels):
    rr=[r for r in DATA if r.get('primary_eligible') and r['method']=='mcpwm:'+source]
    vals=[m['max_abs_us'] for r in rr for m in r['pairs'].values()]
    rows.append({'source':source,'label':label,'max_us':max(vals),'runs':len(rr),'gaps':sum(c['intervals_over_1_5_s'] for r in rr for c in r['channels'].values()),'runs_used':[r['run'] for r in rr]})
fig,ax=plt.subplots(figsize=(12,5.4));fig.subplots_adjust(left=.18,right=.72,top=.78,bottom=.20)
for i,(r,col) in enumerate(zip(rows,colors)):
    ax.hlines(i,.7,r['max_us'],color=col,lw=4,alpha=.35);ax.scatter(r['max_us'],i,s=160,color=col,zorder=3)
    value=f"{r['max_us']:.3f} µs" if r['max_us']<1000 else f"{r['max_us']/1000:.3f} ms"
    ax.text(1.02,i,value,transform=ax.get_yaxis_transform(),va='center',weight='bold',color=col)
    ax.text(1.27,i,f"{r['runs']} / {r['gaps']}",transform=ax.get_yaxis_transform(),va='center',color=GRAY)
ax.set_xscale('log');ax.set_xlim(.7,500000);ax.set_ylim(4.6,-.7);ax.set_yticks(range(5),labels)
ax.set_xticks([1,10,100,1000,10000,100000],['1 µs','10 µs','100 µs','1 ms','10 ms','100 ms'])
ax.grid(axis='x',alpha=.16);ax.set_xlabel('Largest observed relative GPIO error  ← smaller is better',labelpad=13)
ax.spines['left'].set_visible(False);ax.tick_params(axis='y',length=0,pad=12)
fig.text(.03,.94,'Which clock method aligned the pulses best?',fontsize=20,weight='bold',color=INK)
fig.text(.03,.875,'Same firmware • MCPWM output • camera OFF • 60-second records',color=GRAY,fontsize=12)
ax.text(1.02,-.72,'Worst error',transform=ax.get_yaxis_transform(),fontsize=10,color=GRAY)
ax.text(1.27,-.72,'Runs / gaps*',transform=ax.get_yaxis_transform(),fontsize=10,color=GRAY)
fig.text(.03,.04,'* Gaps = observed intervals >1.5 s, summed across channels. Log scale: each tick is 10×.\nNine completed trials; finite maxima of delivered pulses, not guaranteed bounds. No-sync phase depends on boot epochs.',fontsize=10,color=GRAY)
save(fig,'method_comparison')

# Mechanism diagram, schematic rather than measurement.
fig,ax=plt.subplots(figsize=(12,5));ax.set_xlim(0,1);ax.set_ylim(0,1);ax.axis('off')
box(ax,.36,.74,.28,.17,'Node 2\nReference clock',GREEN)
for x,name in [(.04,'Node 0'),(.36,'Node 1'),(.68,'Node 3')]:
    box(ax,x,.34,.28,.18,name+'\nLocal clock → reference time',BLUE,12)
    arrow(ax,.5,.74,x+.14,.54,color=BLUE)
ax.text(.5,.62,'Repeated Wi-Fi timing exchanges',ha='center',fontsize=12,color=INK,bbox=dict(fc='white',ec='none'))
box(ax,.10,.05,.8,.15,'Each node schedules the same reference second → MCPWM raises GPIO41',GREEN,13)
for x in [.18,.5,.82]:arrow(ax,x,.33,x,.22,color=GREEN)
fig.text(.04,.97,'FTM builds a shared time coordinate',fontsize=20,weight='bold',color=INK)
save(fig,'ftm_overview')

fig,ax=plt.subplots(figsize=(12,5));ax.set_xlim(0,1);ax.set_ylim(0,1);ax.axis('off')
ax.plot([.17,.17],[.1,.84],color=BLUE,lw=2);ax.plot([.81,.81],[.1,.84],color=GREEN,lw=2)
ax.text(.17,.91,'Follower clock',ha='center',weight='bold',color=BLUE);ax.text(.81,.91,'Reference clock',ha='center',weight='bold',color=GREEN)
arrow(ax,.17,.74,.81,.59,'Timing message')
arrow(ax,.81,.40,.17,.23,'Reply')
for x,y,s in [(.17,.74,'t1: sent'),(.81,.59,'t2: received'),(.81,.40,'t3: sent'),(.17,.23,'t4: received')]:
    ax.scatter(x,y,s=45,color=INK);ax.text(x+(-.035 if x<.5 else .035),y,s,ha='right' if x<.5 else 'left',va='center',fontsize=11)
ax.text(.49,.47,'Four timestamps help separate\nclock offset from travel time.',ha='center',fontsize=14,color=INK)
fig.text(.04,.98,'Two clocks exchange timestamped messages',fontsize=20,weight='bold',color=INK)
fig.text(.04,.025,'Conceptual request/reply diagram, not an FTM packet trace. Downward = later time. Path asymmetry still causes error.',fontsize=10,color=GRAY)
save(fig,'timestamp_exchange')

# Physical rising-edge zoom from raw CSV (not a fabricated screenshot).
raw=ROOT/'new-acquisition/20260909-065110-campaign/00-00-mcpwm-ftm/digital.csv'
rises,pulses,bad=read_edges(raw);reference=pulses[2][0][0]
fig,ax=plt.subplots(figsize=(12,3.2));edge_offsets={}
for idx,node in enumerate([0,1,2,3]):
    edge=min((v[0] for v in pulses[node]),key=lambda t:abs(t-reference));delta=(edge-reference)*1e6;edge_offsets[str(node)]=delta
    baseline=3-idx;ax.plot([-3,delta,delta,3],[baseline,baseline,baseline+.55,baseline+.55],color=GREEN if node==2 else BLUE,lw=2)
    ax.text(3.08,baseline+.25,f'{delta:+.3f} µs',va='center',fontsize=11)
ax.axvline(0,color=GREEN,ls=':',alpha=.65);ax.set_xlim(-3,3);ax.set_ylim(-.2,3.8)
ax.set_yticks([3.25,2.25,1.25,.25],['Node 0','Node 1','Node 2 (reference)','Node 3'])
ax.set_xlabel('Time relative to the reference rising edge (µs)');ax.grid(axis='x',alpha=.15)
ax.set_title('One actual FTM event, magnified from the raw recording',loc='left',fontsize=15,color=INK,pad=16)
ax.spines['left'].set_visible(False);ax.tick_params(axis='y',length=0)
save(fig,'recorded_edge_zoom')

# All six timer backends, partitioned by same-build exploratory block.
fig,axs=plt.subplots(1,2,figsize=(12,4.8));fig.subplots_adjust(left=.18,right=.97,wspace=.55,top=.74,bottom=.2)
groups=[('170156',[('task','Task'),('esp','esp_timer'),('gpt','GPTimer')],'Build 34e1a3df • one 25-s trial each'),('171055',[('high','High-priority task'),('gptpair','Paired GPTimer'),('mcpwm','MCPWM')],'Build 8dc05893 • two 25-s trials each')]
backend_rows=[]
for ax,(stamp,methods,title) in zip(axs,groups):
    for i,(code,label) in enumerate(methods):
        rr=[r for r in DATA if stamp in r['run'] and r['method']==code+':ftm' and r.get('signal_clean')]
        vals=[max(m['max_abs_us'] for m in r['pairs'].values()) for r in rr]
        ax.scatter(vals,[i]*len(vals),s=80,color=BLUE)
        ax.text(max(vals)*1.15,i,' / '.join(f'{v:.3f}' for v in vals),va='center',fontsize=10)
        backend_rows.append({'backend':label,'block':stamp,'run_max_us':vals,'runs':[r['run'] for r in rr]})
    ax.set_xscale('log');ax.set_xlim(.8,250);ax.set_ylim(2.6,-.6);ax.set_yticks(range(3),[x[1] for x in methods]);ax.set_title(title,fontsize=11,pad=15)
    ax.set_xlabel('Largest error per trial (µs)');ax.grid(axis='x',alpha=.18);ax.tick_params(axis='y',length=0);ax.spines['left'].set_visible(False)
fig.text(.03,.94,'The timer used to generate the pulse matters too',fontsize=19,weight='bold',color=INK)
fig.text(.03,.86,'All use FTM. Compare within each panel; firmware differs between panels.',fontsize=12,color=GRAY)
fig.text(.03,.035,'Exploratory 25-second records, 25 pulses per follower. Later epoch failures and capture failures remain in the full report.\nThese short pre-repair trials do not replace the longer repaired MCPWM validation.',fontsize=10,color=GRAY)
save(fig,'backend_comparison')

provenance={'primary':rows,'backend':backend_rows,'edge_example':{'file':str(raw.relative_to(ROOT)),'reference_time_s':reference,'offsets_us':edge_offsets},'screenshot':'Actual Logic window after loading the same archived FTM capture; 2-second view. Disabled channels D4-D7 are not measurements.','photo':'User-supplied photograph; physical board order not assigned to node IDs. Analyzer label shows24MHz8CH, while software reports Logic; manufacturer authenticity not verified.'}
(HERE/'figure_data.json').write_text(json.dumps(provenance,indent=2))

# Landscape visual brief, fixed page layout.
pdfmetrics.registerFont(TTFont('Arial','C:/Windows/Fonts/arial.ttf'));pdfmetrics.registerFont(TTFont('ArialBold','C:/Windows/Fonts/arialbd.ttf'))
W,H=842,595;c=canvas.Canvas(str(HERE/'timesync_visual_guide.pdf'),pagesize=(W,H));c.setTitle('Timesync: visual comparison and explanation')
style=ParagraphStyle('body',fontName='Arial',fontSize=12,leading=17,textColor=HexColor(INK))
def text(s,x,y,width=750,size=12):
    st=ParagraphStyle('p',parent=style,fontSize=size,leading=size*1.4);p=Paragraph(s,st);_,h=p.wrap(width,800);p.drawOn(c,x,y-h);return y-h
def pic(name,x,y,w,h):
    p=HERE/name;im=Image.open(p);iw,ih=im.size;scale=min(w/iw,h/ih);dw,dh=iw*scale,ih*scale
    c.drawImage(str(p),x+(w-dw)/2,y+(h-dh)/2,dw,dh,mask='auto')
def page(n,title,kicker):
    c.setFillColor(HexColor(INK));c.setFont('ArialBold',24);c.drawString(38,H-45,title)
    c.setFont('Arial',10);c.setFillColor(HexColor(GRAY));c.drawString(39,H-65,kicker)
    c.setStrokeColor(HexColor('#d5e2e8'));c.line(38,32,W-38,32);c.setFont('Arial',9)
    c.drawString(38,18,'WIRELESS IR MOCAP  |  Visual experiment guide  |  9 September 2026');c.drawRightString(W-38,18,str(n))
page(1,'Best-supported choice: live FTM + MCPWM','Clock synchronization methods • measured relative to node 2')
pic('method_comparison.png',20,180,802,330)
text('<b>Why FTM?</b> Its new trial had a largest observed error of 1.688 microseconds and no missed cycles. Three separate historical repaired trials stayed below 1.937 microseconds. Frozen FTM was similarly close for short periods, but had missing cycles and stops updating its clock model.',40,174,760,13)
text('<b>Limits:</b> One new FTM trial versus two per other source; analyzer timeouts stopped the planned repeats. These are GPIO results, not camera-exposure accuracy. Simple NTP-style is our basic private UDP estimator, not a complete NTP client.',40,97,760,11)
c.showPage()
page(2,'How FTM synchronizes the nodes','A clock can have the wrong starting value and run at a slightly different speed.')
pic('ftm_overview.png',34,200,774,310)
text('<b>1. Measure.</b> Nodes exchange Wi-Fi timing messages. FTM timestamps are captured near the radio events, reducing the uncertainty associated with application send/receive timing.',40,190,750,13)
text('<b>2. Learn the clock mapping.</b> Repeated measurements estimate: <b>reference time = a × local time + b</b>. Here <b>a</b> corrects clock speed and <b>b</b> corrects the starting offset. The local clock need not be physically reset.',40,130,750,13)
text('<b>3. Schedule.</b> Convert the next common reference second into each node\'s timer deadline. MCPWM generates the GPIO edge. The analyzer checks how far apart the real edges arrived.',40,69,750,12)
c.showPage()
page(3,'What changes between the five methods?','Same output mechanism; different ways of estimating a common time.')
pic('timestamp_exchange.png',30,240,455,265)
text('<b>Software NTP-style:</b> timestamps before send and after receive. This trial uses only the latest offset, with no frequency correction or delay filter.<br/><br/><b>FTM:</b> radio-event timestamps feed a repeatedly updated offset-and-speed model. Timestamp units alone do not guarantee accuracy.',510,475,288,12)
text('<b>No timesync:</b> use each local clock as-is. Different starting values produce arbitrary phase differences.<br/><br/><b>Frozen FTM:</b> synchronize once, then retain the mapping. This tests holdover, not an unsynchronized clock.<br/><br/><b>AP TSF:</b> read the hotspot\'s shared Wi-Fi timer. This uses the access-point clock rather than the node-2 MAC clock.',42,224,754,13)
text('For the simple NTP-style exchange: offset = [(t2 − t1) + (t3 − t4)] / 2. The calculation assumes roughly symmetric travel times. Unequal paths and scheduling delay can bias it. Sources: RFC 5905 §8; archived implementation and full report.',42,72,754,10)
c.showPage()
page(4,'The physical setup','Your supplied photograph • retained without image alteration')
pic('setup.jpg',30,81,547,433)
text('<b>Four ESP32-S3 camera boards</b><br/>USB provides power/connectivity.<br/><br/><b>Wi-Fi carries time information</b><br/>The probe wires measure GPIO outputs; they do not synchronize the clocks.<br/><br/><b>Four analyzer inputs</b><br/>D0-D3 correspond to nodes 0-3 in the experiment, with node 2 as reference and a common ground.',601,493,207,12)
text('<b>Identification correction</b><br/>The photo shows a USB analyzer marked 24 MHz / 8CH. “Logic” is its software-reported identity, not verified proof of manufacturer. Successful trials used 16 MS/s.',601,245,207,11)
text('Physical left-to-right board order and exact manufacturer/revision cannot be confirmed from this photo. The earlier report\'s “original Saleae Logic” wording should be read as a software device classification.',42,66,754,10)
c.showPage()
page(5,'What the analyzer actually recorded','Archived live-FTM trial: 20260909-065110 / 00-00-mcpwm-ftm • no new acquisition')
pic('logic-capture.png',35,269,505,250)
text('<b>Actual Logic screenshot</b><br/>D0-D3 show aligned 50-ms pulses. D4-D7 are unused.<br/><br/>At a two-second viewing scale, microsecond differences are invisible. The plot below magnifies one recorded rising edge.',566,470,238,12)
pic('recorded_edge_zoom.png',38,65,767,194)
text('The lower plot is reconstructed from the same raw CSV, not a screenshot or simulated result. One event illustrates alignment; the comparison chart uses all qualifying matched pulses. Sampling interval: 62.5 ns, not calibrated accuracy.',40,58,760,9)
c.showPage()
page(6,'The six output backends we also tested','Exploratory comparison • clock method held at FTM within each block')
pic('backend_comparison.png',15,190,812,320)
text('<b>Interpretation:</b> GPTimer was the best of the first three short trials. In the later short block, paired GPTimer and MCPWM were close; high-priority task results varied. These panels use different firmware and cannot supply one fair six-way ranking.',40,177,760,13)
text('<b>Why recommend MCPWM overall?</b> It is the backend with the retained clean, longer validation after the clock-epoch repair. Earlier short results, epoch failures and acquisition failures remain documented; they are not silently pooled into the five-source chart.',40,108,760,12)
text('Data and definitions: ../timesync_report.pdf, ../analysis/run_metrics.json, figure_data.json. Mechanism sources: RFC 5905 §8; Espressif ESP-IDF v5.5.1 Wi-Fi FTM documentation; archived firmware. All diagrams are explanatory, not additional measurements.',40,57,760,9)
c.showPage();c.save()
print(HERE/'timesync_visual_guide.pdf')
