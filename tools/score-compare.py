#!/usr/bin/env python3
"""Compare Groove's notation of a score with the score itself.

    tools/score-compare.py REFERENCE.musicxml GROOVE.musicxml [--bars]

REFERENCE is the score as MuseScore exports it (repeats are played out
here, as Groove plays them); GROOVE is Groove's notation of the same score
after importing it, exported with no repeats (out/render_score FILE
OUT.musicxml). Prints one line of figures (JSON with --json):

  heads   note heads in the reference
  found   at the same place (bar, staff, time in the bar, pitch) in Groove's
  value   of those, written with the same value and dots
  spell   ... with the same letter and accidental
  tie     ... with the same tie
  bars    staff-bars (a bar on one staff) in the reference with notes
  same    staff-bars Groove writes identically (notes, values, rests, ties)

--bars lists the staff-bars that differ, to look at.
"""
import json
import sys
import xml.etree.ElementTree as ET
from fractions import Fraction as F

TYPES = {"maxima": 32, "long": 16, "breve": 8, "whole": 4, "half": 2, "quarter": 1, "eighth": F(1, 2),
         "16th": F(1, 4), "32nd": F(1, 8), "64th": F(1, 16), "128th": F(1, 32), "256th": F(1, 64)}


def text(e, path, d=None):
    x = e.find(path)
    return x.text.strip() if x is not None and x.text else d


def parse(path):
    """parts: [ {name, measures: [ {len, start_repeat, end_repeat, times, endings, ending_stop, items} ] } ]
    items: (staff, onset, kind, pitch, type, dots, tie, tup)"""
    root = ET.parse(path).getroot()
    names = {}
    for sp in root.iter("score-part"):
        names[sp.get("id")] = text(sp, "part-name", "")
    parts = []
    for part in root.findall("part"):
        div = 1
        meas = []
        cur_len = F(4)
        open_ending = None
        for m in part.findall("measure"):
            pos = F(0)
            maxpos = F(0)
            last = F(0)
            items = []
            info = {"start_repeat": False, "end_repeat": False, "times": 2, "endings": None, "items": items,
                    "implicit": m.get("implicit") == "yes", "number": m.get("number") or "",
                    "labels": set(), "jump": None}
            sounds = [x for x in m.iter("sound")]
            for snd in sounds:
                if snd.get("segno"):
                    info["labels"].add("S" + snd.get("segno"))
                if snd.get("coda"):
                    info["labels"].add("C" + snd.get("coda"))
                if snd.get("tocoda"):
                    info["labels"].add("T" + snd.get("tocoda"))
                if snd.get("fine"):
                    info["labels"].add("fine")
                if snd.get("dacapo") == "yes":
                    info["jump"] = "start"
                if snd.get("dalsegno"):
                    info["jump"] = "S" + snd.get("dalsegno")
            for e in m:
                if e.tag == "attributes":
                    if e.find("divisions") is not None:
                        div = int(e.find("divisions").text)
                    t = e.find("time")
                    if t is not None and t.find("beats") is not None:
                        try:
                            beats = sum(int(x) for x in t.find("beats").text.split("+"))
                            cur_len = F(beats * 4, int(t.find("beat-type").text))
                        except ValueError:
                            pass
                elif e.tag == "backup":
                    pos -= F(int(e.find("duration").text), div)
                elif e.tag == "forward":
                    pos += F(int(e.find("duration").text), div)
                    maxpos = max(maxpos, pos)
                elif e.tag == "barline":
                    r = e.find("repeat")
                    if r is not None and r.get("direction") == "forward":
                        info["start_repeat"] = True
                    if r is not None and r.get("direction") == "backward":
                        info["end_repeat"] = True
                        info["times"] = int(r.get("times") or 2)
                    en = e.find("ending")
                    if en is not None and en.get("type") in ("stop", "discontinue"):
                        info["ending_stop"] = True
                    if en is not None and en.get("type") == "start":
                        nums = set()
                        for x in (en.get("number") or "1").replace(" ", "").split(","):
                            try:
                                nums.add(int(x))
                            except ValueError:
                                pass
                        info["endings"] = nums
                elif e.tag == "note":
                    if e.find("grace") is not None or e.find("cue") is not None:
                        continue
                    if e.find("rest") is None and (e.get("print-object") == "no" or text(e, "notehead") == "none"):
                        # (a note not shown: as it looks, it isn't there)
                        if e.find("chord") is None:
                            last = pos
                            pos += F(int(text(e, "duration", "0")), div)
                            maxpos = max(maxpos, pos)
                        continue
                    d = F(int(text(e, "duration", "0")), div)
                    chord = e.find("chord") is not None
                    at = last if chord else pos
                    staff = int(text(e, "staff", "1"))
                    typ = text(e, "type")
                    dots = len(e.findall("dot"))
                    tm = e.find("time-modification")
                    tup = (int(text(tm, "actual-notes")), int(text(tm, "normal-notes"))) if tm is not None else None
                    ties = set(t.get("type") for t in e.findall("tie"))
                    tie = "start" in ties
                    if e.find("rest") is not None and e.get("print-object") == "no":
                        pass                # (a rest not shown)
                    elif e.find("rest") is not None:
                        whole = e.find("rest").get("measure") == "yes" or typ is None
                        items.append((staff, at, "r", None, "bar" if whole else typ, 0 if whole else dots, False,
                                      None if whole else tup))
                    elif e.find("pitch") is not None:
                        p = e.find("pitch")
                        pitch = (text(p, "step"), int(float(text(p, "alter", "0"))), int(text(p, "octave")))
                        items.append((staff, at, "n", pitch, typ, dots, tie, tup))
                    elif e.find("unpitched") is not None:
                        p = e.find("unpitched")
                        pitch = (text(p, "display-step"), 0, int(text(p, "display-octave")))
                        items.append((staff, at, "n", pitch, typ, dots, tie, tup))
                    if not chord:
                        last = pos
                        pos += d
                        maxpos = max(maxpos, pos)
            # a volta covers the measures from its start to its stop
            if info["endings"] is not None:
                open_ending = info["endings"]
            elif open_ending is not None:
                info["endings"] = open_ending
            if info.get("ending_stop"):
                open_ending = None
            info["len"] = maxpos if info["implicit"] and maxpos > 0 else cur_len
            info["actual"] = maxpos
            meas.append(info)
        parts.append({"name": names.get(part.get("id"), ""), "measures": meas})
    return parts


def unfold(meas):
    """The order measures are played in: repeats, first/second endings and
    jumps (D.C., D.S., al Fine, al Coda), as MuseScore plays them. A new
    section starts where the measure numbers start again at 1."""
    nm = len(meas)
    sec_start = [False] * nm
    last = None                     # (measures numbered "X1" aren't counted)
    for i in range(nm):
        try:
            b = int(meas[i]["number"])
        except ValueError:
            continue
        if i and last is not None and b <= 1 and last > 1:
            sec_start[i] = True
            # (a measure not counted just before it is in the new section
            # if it's a pickup, as MuseScore numbers them)
        last = b
    fine = any("fine" in m["labels"] for m in meas)
    toc = next((l for m in meas for l in sorted(m["labels"]) if l[0] == "T"), None)
    until = "fine" if fine else toc
    cont = ("C" + toc[1:]) if (not fine and toc) else None

    def find(label, frm):
        if label == "start":
            i = frm
            while i > 0 and not sec_start[i]:
                i -= 1
            return i
        return next((i for i, m in enumerate(meas) if label in m["labels"]), -1)

    def last_volta(i):
        e = meas[i]["endings"]
        while i + 1 < nm and meas[i + 1]["endings"] == e and not meas[i]["end_repeat"]:
            i += 1
        return i + 1 >= nm or not meas[i + 1]["endings"]

    def endings_ahead(k):
        while k < nm and not meas[k]["start_repeat"]:
            if meas[k]["endings"]:
                return True
            if k + 1 < nm and sec_start[k + 1]:
                break
            k += 1
        return False

    out = []
    i, start, pas = 0, 0, 1
    count = [0] * nm
    jumped = [False] * nm
    in_jump = norep = False
    jfrom = jlabel = None
    guard = 0
    while i < nm and guard < 100000:
        guard += 1
        m = meas[i]
        if sec_start[i] and not in_jump:
            pas, start = 1, i
        if norep:
            if m["endings"] and not last_volta(i):
                i += 1
                continue
        else:
            if m["start_repeat"] and pas == 1:
                start = i
            if m["endings"] is not None and pas not in m["endings"]:
                i += 1
                continue
        out.append(i)
        if norep:
            pass
        elif m["end_repeat"]:
            count[i] += 1
            if count[i] < m["times"]:
                pas += 1
                i = start
                continue
            count[i] = 0
            if not m["endings"]:
                pas, start = 1, i + 1
        elif m["endings"] and pas > 1 and (i + 1 >= nm or not meas[i + 1]["endings"]) and not endings_ahead(i + 1):
            pas, start = 1, i + 1
        elif m["endings"] and i + 1 < nm and meas[i + 1]["endings"] and meas[i + 1]["endings"] != m["endings"]:
            # (an ending with no repeat, others straight after: on at the
            # next ending for this pass further on, as MuseScore plays it)
            j = i + 1
            while j < nm and meas[j]["endings"]:
                j += 1
            k = j
            while k < nm and not meas[k]["start_repeat"] and not sec_start[k] and not (meas[k]["endings"] and pas in meas[k]["endings"]):
                k += 1
            if k < nm and meas[k]["endings"] and pas in meas[k]["endings"] and not meas[k]["start_repeat"] and not sec_start[k]:
                if in_jump and until and until in m["labels"]:
                    pass
                else:
                    i = k
                    continue
        # (a jump with no Fine or To Coda of its own, from MuseScore's
        # "play until" a coda sign, "continue at" a sign after the jump:
        # its export writes those as a coda and a segno; MuseScore plays
        # the coda sign's bar, then goes on)
        if in_jump and not until and jfrom is not None and i < jfrom \
                and any(l[0] in "SC" and l != jlabel for l in m["labels"]):
            j = next((k for k in range(jfrom + 1, nm)
                      if any(l[0] in "SC" for l in meas[k]["labels"])), -1)
            if j >= 0:
                in_jump = norep = False
                pas, start, i = 1, j, j
                if meas[j]["endings"]:
                    pas = max(meas[j]["endings"])
                continue
        if in_jump and until and until in m["labels"]:
            j = find(cont, i) if cont else -1
            if j < 0:
                j = i
                while j < nm and not (j + 1 < nm and sec_start[j + 1]):
                    j += 1
                j += 1
                if j >= nm:
                    break
            in_jump = norep = False
            pas, start, i = 1, j, j
            if j < nm and meas[j]["endings"]:
                pas = max(meas[j]["endings"])
            continue
        if m["jump"] and not jumped[i]:
            jumped[i] = True
            j = find(m["jump"], i)
            if j >= 0:
                jfrom, jlabel = i, m["jump"]
                in_jump = norep = True
                pas, start, i = 1, j, j
                continue
        i += 1
    return out


def bars_of(part, order):
    return [part["measures"][i] for i in order]


def key_items(m, staff, shift=F(0)):
    return sorted(((it[1] + shift, it[2], it[3] or ("", 0, 0), it[4] or "", it[5], it[6], it[7] or (1, 1))
                   for it in m["items"] if it[0] == staff), key=lambda x: (x[0], x[1], x[2]))


def moved_rests(rm, gm, s, shift, ri, gi):
    """The same but for rests on the other staff: a rest the score moves
    there (staffMove), which MuseScore's export lists on its own staff and
    Groove on the one it's shown on."""
    if [x for x in ri if x[1] != "r"] != [x for x in gi if x[1] != "r"]:
        return False
    rr = [x for x in ri if x[1] == "r"]
    gr = [x for x in gi if x[1] == "r"]
    staves = set(it[0] for it in rm["items"]) | set(it[0] for it in gm["items"])
    allr = set(x for t in staves for x in key_items(rm, t, shift) if x[1] == "r")
    allg = set(x for t in staves for x in key_items(gm, t) if x[1] == "r")
    return set(rr) <= allg and set(gr) <= allr


def compare(ref_path, gr_path, show=False):
    ref = parse(ref_path)
    gro = parse(gr_path)
    # (the repeats and endings as the first part has them: MuseScore puts
    # the endings in the top part only)
    order = unfold(ref[0]["measures"]) if ref else []
    # a pickup (as the first part has it): Groove writes it at the end of a whole bar
    pickup = None
    if ref and ref[0]["measures"] and ref[0]["measures"][0]["implicit"]:
        pickup = ref[0]["measures"][0]["actual"]
    ref = [p for p in ref if any(it[2] == "n" for m in p["measures"] for it in m["items"])]
    st = dict(heads=0, found=0, value=0, spell=0, tie=0, bars=0, same=0, parts=len(ref), gparts=len(gro))
    diffs = []
    # parts matched by name (Groove leaves out a part with no notes it
    # plays, such as cue notes), else in order
    used = set()
    match = []
    for pi, rp in enumerate(ref):
        g = next((j for j, gp in enumerate(gro) if j not in used and gp["name"] == rp["name"]), None)
        if g is None and len(ref) == len(gro):
            g = pi
        if g is not None:
            used.add(g)
        match.append(g)
    st["unmatched"] = 0
    for pi, rp in enumerate(ref):
        if match[pi] is None:
            st["unmatched"] += sum(1 for m in rp["measures"] for it in m["items"] if it[2] == "n")
            continue
        gp = gro[match[pi]]
        rbars = bars_of(rp, [i for i in order if i < len(rp["measures"])])
        gbars = gp["measures"]
        for b, rm in enumerate(rbars):
            gm = gbars[b] if b < len(gbars) else {"items": [], "len": rm["len"]}
            # a pickup: Groove writes it at the end of a whole bar
            shift = F(0)
            if b == 0 and pickup is not None and pickup < gm.get("actual", pickup):
                shift = gm["actual"] - pickup     # (older Groove: at the end of a whole bar)
            staves = sorted(set(it[0] for it in rm["items"]))
            for s in staves:
                ri = key_items(rm, s, shift)
                gi = key_items(gm, s)
                rn = [x for x in ri if x[1] == "n"]
                gn = {}
                for x in gi:
                    if x[1] == "n":
                        gn.setdefault((x[0], x[2][0], x[2][1], x[2][2]), x)
                gn_pitch = {}
                for x in gi:
                    if x[1] == "n":
                        p = x[2]
                        gn_pitch.setdefault((x[0], semis(p)), x)
                st["heads"] += len(rn)
                for x in rn:
                    g = gn_pitch.get((x[0], semis(x[2])))
                    if g is None:
                        continue
                    st["found"] += 1
                    st["value"] += (g[3], g[4], g[6]) == (x[3], x[4], x[6])
                    st["spell"] += g[2] == x[2]
                    st["tie"] += g[5] == x[5]
                if rn:
                    st["bars"] += 1
                    if ri == gi or moved_rests(rm, gm, s, shift, ri, gi):
                        st["same"] += 1
                    else:
                        diffs.append((pi, b, s, ri, gi))
    if show:
        for pi, b, s, ri, gi in diffs:
            print("part %d bar %d staff %d" % (pi + 1, b + 1, s))
            print("  ref:   " + " ".join(fmt(x) for x in ri))
            print("  groove:" + " ".join(fmt(x) for x in gi))
    return st


def semis(p):
    return "C D EF G A B".index(p[0]) + p[1] + 12 * p[2] if p[0] else -1


def fmt(x):
    on, kind, p, typ, dots, tie, tup = x
    s = "%s:%s" % (on, "r" if kind == "r" else "%s%s%d" % (p[0], {-2: "bb", -1: "b", 0: "", 1: "#", 2: "x"}[p[1]], p[2]))
    s += "/%s%s" % (typ, "." * dots)
    if tie:
        s += "~"
    if tup != (1, 1):
        s += "(%d:%d)" % tup
    return s


if __name__ == "__main__":
    st = compare(sys.argv[1], sys.argv[2], "--bars" in sys.argv)
    if "--json" in sys.argv:
        print(json.dumps(st))
    else:
        h = max(st["heads"], 1)
        f = max(st["found"], 1)
        print("heads %d  found %.1f%%  value %.1f%%  spell %.1f%%  tie %.1f%%  bars %d  same %.1f%%" % (
            st["heads"], 100 * st["found"] / h, 100 * st["value"] / f, 100 * st["spell"] / f,
            100 * st["tie"] / f, st["bars"], 100 * st["same"] / max(st["bars"], 1)))
