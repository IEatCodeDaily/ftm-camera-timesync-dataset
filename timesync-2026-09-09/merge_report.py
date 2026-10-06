"""Build the combined visual and technical report without altering delivered originals."""
from pathlib import Path
import json,hashlib
import pymupdf as fitz
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.lib.colors import HexColor
ROOT=Path(__file__).resolve().parent
TMP=ROOT/'combined-work';TMP.mkdir(exist_ok=True)

# Regenerate a corrected technical part to a separate output; preserve the originals.
source=(ROOT/'build_report.py').read_text(encoding='utf-8')
source=source.replace('identifies an original Saleae Logic, device ID','identifies a software-reported Logic device, device ID')
source=source.replace('and Logic 2.4.46. Successful comparison recordings',
    'and Logic 2.4.46. The supplied setup photo shows an analyzer marked 24 MHz / 8CH; physical manufacturer authenticity is unverified. Successful comparison recordings')
source=source.replace("ROOT/'timesync_report.pdf'","ROOT/'combined-work/technical.pdf'")
source=source.replace("ROOT/'timesync_report.md'","ROOT/'combined-work/technical.md'")
exec(compile(source,str(ROOT/'build_report.py'),'exec'),{'__file__':str(ROOT/'build_report.py'),'__name__':'__main__'})

pdfmetrics.registerFont(TTFont('Cover','C:/Windows/Fonts/arial.ttf'))
pdfmetrics.registerFont(TTFont('CoverBold','C:/Windows/Fonts/arialbd.ttf'))
cv=canvas.Canvas(str(TMP/'cover.pdf'),pagesize=(597.6,842.4))
cv.setFillColor(HexColor('#173b54'));cv.setFont('CoverBold',32)
cv.drawString(46,752,'Time synchronization')
cv.setFont('Cover',20);cv.drawString(46,717,'Visual guide and experimental report')
cv.setFont('Cover',12);cv.drawString(46,674,'Wireless IR motion capture  |  Four ESP32-S3 nodes')
cv.drawString(46,652,'Experiments: 8-9 September 2026')
cv.setFillColor(HexColor('#eaf4f1'));cv.roundRect(46,514,505,105,9,fill=1,stroke=0)
cv.setFillColor(HexColor('#087f68'));cv.setFont('CoverBold',18)
cv.drawString(62,586,'Best-supported choice: live FTM + MCPWM')
cv.setFont('Cover',12);cv.drawString(62,559,'1.688 microseconds: largest observed error in the new FTM trial.')
cv.drawString(62,538,'Limited repetitions; results measure GPIO phase, not exposure.')
cv.setFillColor(HexColor('#173b54'));cv.setFont('CoverBold',18);cv.drawString(46,460,'Reading guide')
entries=[('2','Comparison of the five clock methods'),('3-4','Simple explanation and timing diagrams'),('5','Setup photograph and hardware identification'),('6','Logic screenshot and recorded edge detail'),('7','Comparison of the six output backends'),('8 onward','Full methods, results, failures, limitations and references')]
for i,(n,label) in enumerate(entries):
    y=423-i*34;cv.setFont('CoverBold',12);cv.drawString(46,y,n);cv.setFont('Cover',12);cv.drawString(113,y,label)
cv.setFont('Cover',11)
for i,line in enumerate(['Includes the visual supplement and the detailed report in one document.',
    'Nine of fifteen planned new 60-second comparison trials completed.',
    'Acquisition failures and unequal repetition counts remain explicit.',
    'The analyzer identification is clarified using the supplied photograph.']):cv.drawString(46,173-i*19,line)
cv.save()

parts=[TMP/'cover.pdf',ROOT/'visual-summary/timesync_visual_guide.pdf',TMP/'technical.pdf']
out=fitz.open()
for path in parts:
    with fitz.open(path) as d:out.insert_pdf(d)
assert len(out)==17,len(out)
# Continuous numbering, preserving each part's readable page orientation.
for i,page in enumerate(out):
    w,h=page.rect.width,page.rect.height
    if i>0:
        page.add_redact_annot(fitz.Rect(0,h-29,w,h),fill=(1,1,1))
        page.apply_redactions()
    page.insert_text((38,h-15),'TIMESYNC | COMBINED VISUAL AND EXPERIMENTAL REPORT',fontsize=7,color=(.32,.40,.46))
    number=f'{i+1} / {len(out)}'
    page.insert_text((w-38-fitz.get_text_length(number,fontsize=8),h-15),number,fontsize=8,color=(.32,.40,.46))
toc=[[1,'Overview and reading guide',1],[1,'Visual guide',2],[2,'Five clock methods: comparison',2],
     [2,'How FTM works',3],[2,'What changes between methods',4],[2,'Setup photograph',5],
     [2,'Logic analyzer screenshot',6],[2,'Six output backends',7],[1,'Detailed experimental report',8]]
for i in range(7,len(out)):
    t=out[i].get_text()
    for title in ['2. Methods and definitions','3. Results from the 8 September campaign','4. Additional measurements on 9 September','5. Camera load, interpretation and remaining limits','6. Conclusions and next qualification','7. Reproducibility and evidence map','References']:
        if title in t:toc.append([2,title,i+1])
out.set_toc(toc)
out.set_metadata({'title':'Time synchronization: visual guide and experimental report','author':'Wireless IR motion-capture project','subject':'Five clock sources, six output backends, setup photo, Logic capture, methods and evidence'})
dest=ROOT/'timesync_combined_report.pdf';out.save(dest,garbage=4,deflate=True);out.close()
with fitz.open(dest) as check:
    text=' '.join(' '.join(p.get_text() for p in check).split())
    assert len(check)==17
    assert all(f'{i+1} / 17' in p.get_text() for i,p in enumerate(check))
    assert '1.688' in text and '55.503' in text and 'software-reported Logic device' in text
    assert len(check.get_toc())>=9
manifest={'output':dest.name,'pages':17,'sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),
          'parts':[str(x.relative_to(ROOT)) for x in parts],'changes':['Unified continuous page numbers and bookmarks','Corrected technical apparatus attribution using user photo','Original separately delivered PDFs preserved']}
(ROOT/'combined_report_manifest.json').write_text(json.dumps(manifest,indent=2))
print(dest)
