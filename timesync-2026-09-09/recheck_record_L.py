"""Record L (camera-loaded clock record): re-analysis with an a-priori glitch filter.

The 16 MS/s capture of 20260908-175800-tracking has single-sample (62.5 ns)
HIGH glitches on two channels (extra-edges.json: 97 on D2, 25 on D3). Rule fixed
before looking at offsets: delete any HIGH or LOW run shorter than 1 us (16
samples), then apply the paper's unchanged pulse rule (HIGH 40-65 ms, follower
rising edge matched to the nearest reference (node 2) edge within +-0.5 s).

Result is asserted equal to the unfiltered offsets in analysis/offsets.csv:
the glitches never touch a qualified rising edge, so record L needs no caveat
beyond being a single 60-s record. Run: python3 recheck_record_L.py
"""
import csv, os, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
CAP = os.path.join(HERE, "historical-evidence/test-output/pipeline/20260908-175800-tracking/digital.csv")
OFF = os.path.join(HERE, "analysis/offsets.csv")
FILTER_US = 1.0


def edges(t, lv, filt_us):
    tr = [(t[i], lv[i]) for i in range(1, len(t)) if lv[i] != lv[i - 1]]
    if filt_us:
        i = 0
        while i < len(tr) - 1:
            if (tr[i + 1][0] - tr[i][0]) * 1e6 < filt_us:
                del tr[i:i + 2]; i = max(i - 1, 0)
            else:
                i += 1
    return [a for (a, s), (b, _) in zip(tr, tr[1:]) if s == 1 and 0.040 <= b - a <= 0.065]


def offsets(filt_us):
    rows = list(csv.reader(open(CAP)))[1:]
    t = [float(r[0]) for r in rows]
    R = [edges(t, [int(r[c + 1]) for r in rows], filt_us) for c in range(4)]
    out = []
    for f in (0, 1, 3):
        for x in R[f]:
            m = min(R[2], key=lambda y: abs(y - x))
            if abs(m - x) <= 0.5:
                out.append((x - m) * 1e6)
    return R, out


if __name__ == "__main__":
    R0, raw = offsets(None)
    R1, filt = offsets(FILTER_US)
    ref = [float(r[4]) for r in csv.reader(open(OFF)) if len(r) > 4 and r[1] == "mcpwm:ftm-camera-load"]
    assert [len(r) for r in R1] == [60, 60, 60, 60], [len(r) for r in R1]
    assert len(filt) == len(raw) == len(ref) == 180
    assert max(abs(a - b) for a, b in zip(sorted(filt), sorted(ref))) < 1e-3, "filtered offsets differ from offsets.csv"
    assert max(abs(a - b) for a, b in zip(sorted(raw), sorted(ref))) < 1e-3
    print(f"record L, glitch filter {FILTER_US} us: n={len(filt)} SD={st.pstdev(filt):.3f} us "
          f"max|.|={max(map(abs, filt)):.3f} us; identical to unfiltered offsets.csv -> RECORD_L_FILTER_INVARIANT")
