"""Graphical abstract (IEEE: 660x295 landscape, labels only, sans-serif). Hand-built SVG in one illustration style,
read left -> right:
  (1) wireless IR camera nodes around a tracked LED, Wi-Fi FTM clock alignment to a reference node
  (2) per-node trigger -> frame-start lanes at a shared slot, software vs hardware path (schematic, not to scale)
  (3) the real tracked LED path from the robot-arm sweep (dataset arm-reference/dynamic)
Every printed number is read from figures/numbers.tex (make_figures.py). Drawn in a 1320x590 viewBox (2x IEEE),
rasterised at 2640x1180. Output: figures/graphical_abstract.{svg,png,pdf}."""
import json, math, re
from pathlib import Path
import cairosvg

HERE = Path(__file__).resolve().parent
DATA = next(d for d in (HERE.parent, Path("/mnt/e/Projects/ftm-camera-timesync-dataset")) if (d / "arm-reference").is_dir())
N = dict(re.findall(r"\\newcommand\{\\(\w+)\}\{([^}]*)\}", (HERE / "figures/numbers.tex").read_text()))
SD = f"{N['LiveFourSdLo']}–{N['LiveFourSdHi']}"
HW, SW, VMAX, LEN, LLO, LHI = N["HwJitMed"], N["SwJitMed"], N["DynVmax"], N["LenRms"], N["LenLo"], N["LenHi"]

W, H = 1320, 620
NAVY, BLUE, TEAL, IR, RED, GREY, INK = "#14365d", "#2563a8", "#0f766e", "#f97316", "#c0392b", "#6b7280", "#111827"
FONT = "DejaVu Sans, Arial, sans-serif"
S = []


def text(x, y, s, size=22, fill=INK, weight="normal", anchor="middle", extra=""):
    S.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{FONT}" font-size="{size}" fill="{fill}" '
             f'font-weight="{weight}" text-anchor="{anchor}" {extra}>{s}</text>')


def camera(x, y, ang, ref=False):
    """Node glyph: board + lens with IR-pass (dark red) filter, looking along ang (deg, SVG coords)."""
    S.append(f'<g transform="translate({x},{y}) rotate({ang:.1f})">'
             f'<rect x="-26" y="-31" width="42" height="62" rx="7" fill="{NAVY if ref else "#334155"}"/>'
             f'<rect x="12" y="-17" width="16" height="34" rx="4" fill="#0b0f19"/>'
             f'<circle cx="27" cy="0" r="11" fill="#4a0e0e" stroke="#cbd5e1" stroke-width="3"/>'
             f'<circle cx="24" cy="-3" r="3" fill="#fff" opacity=".6"/></g>')


def wifi(x, y, col=BLUE):
    for i in range(3):
        r = 12 + 11 * i
        S.append(f'<path d="M{x - r * .7:.1f},{y - r * .7:.1f} A{r},{r} 0 0 1 {x + r * .7:.1f},{y - r * .7:.1f}" '
                 f'fill="none" stroke="{col}" stroke-width="4.5" stroke-linecap="round"/>')
    S.append(f'<circle cx="{x}" cy="{y}" r="4.5" fill="{col}"/>')


def chevron(x, y):
    S.append(f'<path d="M{x},{y - 34} L{x + 26},{y} L{x},{y + 34}" fill="none" stroke="#94a3b8" stroke-width="8" '
             f'stroke-linecap="round" stroke-linejoin="round"/>')


def head(x, title, sub):
    text(x, 96, title, 25, INK, "bold")
    text(x, 124, sub, 21, "#4b5563")                                          # darker grey: legible at 660 px


S.append(f'<rect width="{W}" height="{H}" fill="white"/>')
RATIO = round(float(SW) / float(HW)); assert RATIO == 10                     # headline ratio from the paper's macros
text(W / 2, 44, "Low-cost wireless infrared motion capture, clock-aligned over Wi-Fi", 32, INK, "bold")
text(W / 2, 80, f"Hardware path: frame-start delay {RATIO}× more consistent · lengths within {LEN} mm RMS", 23, TEAL)
S.append('<g transform="translate(0,30)">')
for x in (470, 925):
    chevron(x, 330)

# ---------------- (1) scene ------------------------------------------------------------------------------------
head(235, "Wireless IR camera nodes", "ESP32-S3 + OV5640 per node")
cx, cy = 235, 330
nodes = [(95, 232, "N1"), (375, 232, "N2"), (95, 420, "N3"), (375, 420, "N0")]
for x, y, _ in nodes:                                                       # view cones under everything
    a = math.atan2(cy - y, cx - x); d = math.hypot(cx - x, cy - y) - 4
    (x1, y1), (x2, y2) = [(x + d * math.cos(a + s), y + d * math.sin(a + s)) for s in (.2, -.2)]
    S.append(f'<path d="M{x},{y} L{x1:.1f},{y1:.1f} L{x2:.1f},{y2:.1f} Z" fill="{IR}" opacity=".11"/>')
rx, ry = nodes[3][:2]
for x, y, lab in nodes:
    camera(x, y, math.degrees(math.atan2(cy - y, cx - x)), ref=lab == "N0")
    ly = y - 50 if y < cy else y + 66
    text(x, ly, lab + (" ref." if lab == "N0" else ""), 21, NAVY, "bold")
    wifi(x + (-44 if x < cx else 44 + 30 * (lab == "N0")), ly - 6, NAVY if lab == "N0" else BLUE)   # clear of "N0 ref."
S.append(f'<circle cx="{cx}" cy="{cy}" r="32" fill="{IR}" opacity=".16"/><circle cx="{cx}" cy="{cy}" r="18" fill="{IR}" opacity=".35"/>'
         f'<circle cx="{cx}" cy="{cy}" r="8.5" fill="{IR}"/>')
S.append(f'<rect x="{cx - 44}" y="{cy + 40}" width="88" height="28" rx="6" fill="white" opacity=".9"/>')
text(cx, cy + 61, "IR LED", 20, "#9a3412", "bold")
text(cx, 534, "802.11 FTM clock alignment to N0", 21, BLUE, "bold")
text(cx, 562, f"offset SD {SD} µs", 21, BLUE)

# ---------------- (2) timing lanes -----------------------------------------------------------------------------
X0, X1 = 520, 890
head((X0 + X1) / 2, "Trigger at a shared slot", "frame start of each node")
XS = X0 + 165                                                               # slot instant


def lanes(y0, offs, col, name, num):
    text(X0, y0 - 12, name, 20, col, "bold", "start")
    for i, o in enumerate(offs):
        yy = y0 + 30 * i
        e = XS + o
        S.append(f'<path d="M{X0 + 4},{yy + 20} L{e},{yy + 20} L{e},{yy + 2} L{X0 + 225},{yy + 2}" fill="none" '
                 f'stroke="{col}" stroke-width="4" stroke-linejoin="round" stroke-linecap="round"/>')
    lo, hi = XS + min(offs), XS + max(offs)
    yb = y0 + 134
    if hi - lo > 20:                                                          # a tiny bracket reads as a glyph
        S.append(f'<path d="M{lo},{yb - 8} L{lo},{yb} L{hi},{yb} L{hi},{yb - 8}" fill="none" stroke="{col}" stroke-width="3"/>')
    text(X1, y0 + 62, f"{num} µs", 36, col, "bold", "end")
    text(X1, y0 + 90, "delay spread", 20, "#374151", "normal", "end")


S.append(f'<line x1="{XS}" y1="150" x2="{XS}" y2="512" stroke="#9ca3af" stroke-width="2.5" stroke-dasharray="6 6"/>')
lanes(166, [-34, 22, -10, 38], RED, "software", SW)
lanes(358, [o * float(HW) / float(SW) for o in (-34, 22, -10, 38)], TEAL, "hardware", HW)   # widths in true ratio
text(XS, 534, "slot", 20, INK)
text((X0 + X1) / 2, 562, "median, per-camera delay removed", 20, "#374151", "normal", "middle", 'font-style="italic"')

# ---------------- (3) length accuracy vs the robot arm (real data: every pair of reached poses) ----------------
head(1140, "Robot arm as ruler", f"{N['LenPoses']} poses, {N['LenPairs']} lengths")
AL = json.load((DATA / "campaign-2026-10-17/arm-length.json").open())["rows"]    # grouping = arm_length_stats.py
G = {}
for r in AL:
    if "pose_xyz" in r: G.setdefault(tuple(round(x) for x in r["pose_xyz"]), []).append(r)
med = lambda v: sorted(v)[len(v) // 2] if len(v) % 2 else sum(sorted(v)[len(v) // 2 - 1:len(v) // 2 + 1]) / 2
R = [([med([r["pose_xyz"][k] for r in v]) for k in range(3)], [med([r["ir_world"][k] for r in v]) for k in range(3)]) for v in G.values()]
PR = [(math.dist(a[0], b[0]), math.dist(a[1], b[1]) - math.dist(a[0], b[0])) for i, a in enumerate(R) for b in R[i + 1:]]
assert len(PR) == int(N["LenPairs"]) and f"{math.sqrt(sum(e * e for _, e in PR) / len(PR)):.1f}" == LEN   # same data as the paper
bx0, bx1, by0, by1, LMAX, EMAX = 1010, 1240, 175, 435, 500, 8
px = lambda l: bx0 + (bx1 - bx0) * l / LMAX
py = lambda e: (by0 + by1) / 2 - (by1 - by0) / 2 * e / EMAX
S.append(f'<rect x="{bx0}" y="{py(float(LEN)):.1f}" width="{bx1 - bx0}" height="{py(-float(LEN)) - py(float(LEN)):.1f}" fill="{TEAL}" opacity=".13"/>')
S.append(f'<line x1="{bx0}" y1="{py(0)}" x2="{bx1}" y2="{py(0)}" stroke="{GREY}" stroke-width="2"/>')
S.append(f'<path d="M{bx0},{by0} L{bx0},{by1} L{bx1},{by1}" fill="none" stroke="{INK}" stroke-width="2.5"/>')
S += [f'<circle cx="{px(l):.1f}" cy="{py(e):.1f}" r="2.6" fill="{TEAL}" opacity=".55"/>' for l, e in PR]
for e in (-EMAX, 0, EMAX):
    text(bx0 - 12, py(e) + 7, f"{e:+d}".replace("-", "−") if e else "0", 20, "#374151", "normal", "end")
    S.append(f'<line x1="{bx0 - 7}" y1="{py(e)}" x2="{bx0}" y2="{py(e)}" stroke="{INK}" stroke-width="2.5"/>')
for l in (0, 250, 500):
    text(px(l), by1 + 28, str(l), 20, "#374151")
    S.append(f'<line x1="{px(l)}" y1="{by1}" x2="{px(l)}" y2="{by1 + 7}" stroke="{INK}" stroke-width="2.5"/>')
text(bx0 + 8, by0 - 8, "length error, mm", 20, "#374151", "normal", "start")
text(bx1, by1 + 52, "arm distance, mm", 20, "#374151", "normal", "end")
text(bx1 + 6, py(float(LEN)) + 6, f"±{LEN}", 20, TEAL, "bold", "start")                          # the band IS the RMS
text(1140, 534, f"{LEN} mm RMS", 28, TEAL, "bold")
text(1140, 562, "tracked vs. arm encoders", 20, "#374151")

S.append("</g>")
from PIL import ImageFont                                                  # self-check: real-font text extents
_spans = []
for m in re.finditer(r'<text x="([\d.]+)" y="[\d.]+"[^>]*font-size="([\d.]+)"[^>]*font-weight="(\w+)" text-anchor="(\w+)"[^>]*>([^<]*)</text>', "".join(S)):
    assert float(m[2]) >= 20, f"text below 20 px (10 px at IEEE 660-px size): {m[5]!r}"
    w = ImageFont.truetype(f"DejaVuSans{'-Bold' if m[3] == 'bold' else ''}.ttf", int(float(m[2]))).getlength(m[5])
    x0 = float(m[1]) - {"middle": w / 2, "start": 0, "end": w}[m[4]]
    assert 6 <= x0 and x0 + w <= W - 6, f"text off canvas: {m[5]!r} {x0:.0f}..{x0 + w:.0f}"
    for y1, a1, b1, t1 in _spans:                                          # same baseline: no two labels may touch
        assert abs(y1 - float(m.group(0).split('y="')[1].split('"')[0])) > 1 or b1 + 8 <= x0 or x0 + w + 8 <= a1, f"labels collide: {t1!r} / {m[5]!r}"
    _spans.append((float(m.group(0).split('y="')[1].split('"')[0]), x0, x0 + w, m[5]))
svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">' + "".join(S) + "</svg>"
out = HERE / "figures"
(out / "graphical_abstract.svg").write_text(svg)
cairosvg.svg2png(bytestring=svg.encode(), write_to=str(out / "graphical_abstract.png"), output_width=2 * W, output_height=2 * H)
cairosvg.svg2pdf(bytestring=svg.encode(), write_to=str(out / "graphical_abstract.pdf"))
print("graphical abstract ->", out / "graphical_abstract.png")
