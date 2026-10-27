#!/usr/bin/env python3
"""
X-Plane 12 localizer alignment fixer.

Reads ILS/LOC emitters (earth_nav.dat row types 4 and 5) and the runways that
the scenery actually draws (apt.dat row type 100), then writes a
Custom Data/user_nav.dat in which each runway-paired localizer is moved
sideways onto the scenery runway's extended centerline and its course is set
to the scenery runway's true heading. The LOC keeps its along-track distance
from the runway, so only the lateral error is removed. The glideslope and DME
of each corrected ILS are written too, because X-Plane drops stock entries
that share a corrected localizer's ident and airport.

Localizers are left alone (and listed in the report) when:
  * they are not straight-in (LDA, SDF, IGS),
  * they are intentionally offset from the real runway: the navdata's CIFP
    file for the airport (CIFP/<ICAO>.dat next to earth_nav.dat) gives the
    real thresholds, and a LOC that is off that real centerline was put there
    on purpose (RJBT 01, 30 m east), so it is not "fixed",
  * they are listed in --skip-ids,
  * the runway or airport is not in the scenery,
  * the scenery and the navdata disagree by more than the thresholds, which
    usually means the scenery runway itself is misplaced (the LIML case), and
    "fixing" the LOC would send you somewhere the real one never would.

Usage (auto-discover everything from an X-Plane install):
    python loc_fixer.py --xplane "C:/X-Plane 12"

Or explicit files (first --apt wins for an airport, like scenery_packs.ini):
    python loc_fixer.py --nav earth_nav.dat --apt custom/apt.dat --apt global/apt.dat -o user_nav.dat

Pure Python 3.8+, no dependencies.
"""
import argparse
import csv
import math
import os
import shutil
import sys

# WGS84 local radii are plenty for distances of a few km.
A = 6378137.0
E2 = 6.69437999014e-3

SKIP_NAMES = ("LDA", "SDF", "IGS")
SKIP_IDS = set()        # {"RJBT ITH", ...}: airport + LOC ident, never touched


def radii(lat):
    s = math.sin(math.radians(lat))
    w = math.sqrt(1 - E2 * s * s)
    n = A / w                      # prime vertical
    m = A * (1 - E2) / w ** 3      # meridional
    return m, n * math.cos(math.radians(lat))


def to_en(lat0, lon0, lat, lon):
    """Local east/north metres of (lat, lon) relative to (lat0, lon0)."""
    rm, rp = radii(lat0)
    dlon = (lon - lon0 + 540) % 360 - 180
    return math.radians(dlon) * rp, math.radians(lat - lat0) * rm


def from_en(lat0, lon0, e, n):
    rm, rp = radii(lat0)
    return lat0 + math.degrees(n / rm), lon0 + math.degrees(e / rp)


def norm_rwy(r):
    """'01L' and '1L' are the same runway; apt.dat and earth_nav.dat differ."""
    r = r.upper()
    return r.lstrip("0") or r


def angdiff(a, b):
    return (a - b + 180) % 360 - 180


# --------------------------------------------------------------------------
# apt.dat
# --------------------------------------------------------------------------

def read_apt_runways(path, runways, seen_this_file_cb=None):
    """Add land runways from one apt.dat to `runways` without overriding
    airports already loaded from a higher-priority file.

    runways: {airport_key: {rwy_id: (lat, lon, opp_lat, opp_lon, opp_id)}}
    Airports are keyed by both their apt.dat ident and 1302 icao_code.
    """
    added = set()
    cur = None          # dict of runways for the current airport, or None if skipped
    keys = []

    def close():
        if cur is not None and cur:
            for k in keys:
                if k not in runways or k in added:
                    runways.setdefault(k, {}).update(cur)
                    added.add(k)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line[0] not in "1 ":
                continue
            p = line.split()
            if not p:
                continue
            t = p[0]
            if t in ("1", "16", "17"):
                close()
                cur, keys = None, []
                if t == "1" and len(p) >= 5:
                    ident = p[4].upper()
                    if ident not in runways or ident in added:
                        cur, keys = {}, [ident]
            elif cur is None:
                continue
            elif t == "1302" and len(p) >= 3 and p[1] == "icao_code":
                icao = p[2].upper()
                if icao not in keys and (icao not in runways or icao in added):
                    keys.append(icao)
            elif t == "100" and len(p) >= 20:
                try:
                    r1, la1, lo1 = norm_rwy(p[8]), float(p[9]), float(p[10])
                    r2, la2, lo2 = norm_rwy(p[17]), float(p[18]), float(p[19])
                except ValueError:
                    continue
                cur[r1] = (la1, lo1, la2, lo2, r2)
                cur[r2] = (la2, lo2, la1, lo1, r1)
    close()
    return len(added)


# --------------------------------------------------------------------------
# X-Plane install discovery
# --------------------------------------------------------------------------

def discover(xp):
    nav = None
    for c in (os.path.join(xp, "Custom Data", "earth_nav.dat"),
              os.path.join(xp, "Resources", "default data", "earth_nav.dat")):
        if os.path.isfile(c):
            nav = c
            break
    apts = []
    ini = os.path.join(xp, "Custom Scenery", "scenery_packs.ini")
    if os.path.isfile(ini):
        with open(ini, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line.startswith("SCENERY_PACK ") or line.startswith("SCENERY_PACK_DISABLED"):
                    continue
                rel = line[len("SCENERY_PACK "):].strip().rstrip("/\\")
                if rel == "*GLOBAL_AIRPORTS*":
                    rel = os.path.join("Global Scenery", "Global Airports")
                base = rel if os.path.isabs(rel) else os.path.join(xp, rel)
                c = os.path.join(base, "Earth nav data", "apt.dat")
                if os.path.isfile(c) and c not in apts:
                    apts.append(c)
    g = os.path.join(xp, "Global Scenery", "Global Airports", "Earth nav data", "apt.dat")
    if os.path.isfile(g) and g not in apts:
        apts.append(g)
    return nav, apts


# --------------------------------------------------------------------------
# CIFP: real runway thresholds, to recognise intentionally offset localizers
# --------------------------------------------------------------------------

def cifp_coord(s):
    """N35302683 / E134471194 (hemisphere, deg, min, sec in 1/100) -> degrees."""
    s = s.strip()
    if len(s) not in (9, 10) or s[0] not in "NSEW" or not s[1:].isdigit():
        return None
    dw = 2 if s[0] in "NS" else 3
    v = int(s[1:1 + dw]) + int(s[1 + dw:3 + dw]) / 60.0 + int(s[3 + dw:]) / 100.0 / 3600.0
    return -v if s[0] in "SW" else v


def opposite_rwy(r):
    """'1' -> '19', '9L' -> '27R'; None if not a numbered runway."""
    n = r.rstrip("LRC")
    if not n.isdigit():
        return None
    o = (int(n) + 17) % 36 + 1
    side = r[len(n):]
    return str(o) + {"L": "R", "R": "L"}.get(side, side)


def read_cifp_runways(path):
    """{runway id (no leading zero): (lat, lon)} of the landing thresholds."""
    out = {}
    try:
        f = open(path, "r", encoding="utf-8", errors="replace")
    except OSError:
        return out
    with f:
        for line in f:
            if not line.startswith("RWY:"):
                continue
            parts = line.split(";")
            if len(parts) < 2:
                continue
            rid = parts[0][4:].split(",")[0].strip().upper()
            pos = parts[1].split(",")
            if not rid.startswith("RW") or len(pos) < 2:
                continue
            lat, lon = cifp_coord(pos[0]), cifp_coord(pos[1])
            if lat is not None and lon is not None:
                out[norm_rwy(rid[2:])] = (lat, lon)
    return out


_cifp_cache = {}


def real_runway(cifp_dir, airport, rwy):
    """(thr_lat, thr_lon, far_lat, far_lon) of the real runway, or None."""
    if not cifp_dir:
        return None
    if airport not in _cifp_cache:
        _cifp_cache[airport] = read_cifp_runways(os.path.join(cifp_dir, airport + ".dat"))
    r = _cifp_cache[airport]
    o = opposite_rwy(rwy)
    if rwy not in r or o not in r:
        return None
    return r[rwy] + r[o]


def line_geom(tla, tlo, ola, olo, lat, lon, true_crs):
    """LOC vs the centerline tla/tlo -> ola/olo: (rwy_crs, along, cross,
    course difference, lateral miss where the beam crosses the threshold),
    or None if the runway is degenerate."""
    oe, on = to_en(tla, tlo, ola, olo)
    rlen = math.hypot(oe, on)
    if rlen < 50:
        return None
    ue, un = oe / rlen, on / rlen
    rwy_crs = math.degrees(math.atan2(ue, un)) % 360
    le, ln = to_en(tla, tlo, lat, lon)
    along = le * ue + ln * un               # m past the approach end
    cross = le * un - ln * ue               # + = LOC is right of centerline (looking along course)
    dang = angdiff(rwy_crs, true_crs)
    beam_ue = math.sin(math.radians(true_crs))
    beam_un = math.cos(math.radians(true_crs))
    # beam point at threshold-along-track 0: LOC - t*beam, solve along=0
    denom = beam_ue * ue + beam_un * un
    thr_miss = None
    if abs(denom) > 1e-6:
        t = along / denom
        be, bn = le - t * beam_ue, ln - t * beam_un
        thr_miss = be * un - bn * ue
    return rwy_crs, rlen, ue, un, along, cross, dang, thr_miss


# --------------------------------------------------------------------------
# main logic
# --------------------------------------------------------------------------

def fix_loc(p, rwys, a):
    """Return (status, detail dict, new_fields or None) for one LOC row."""
    lat, lon = float(p[1]), float(p[2])
    brg_field = float(p[6])
    airport, rwy, name = p[8].upper(), norm_rwy(p[10]), " ".join(p[11:])
    mag_part = math.floor(brg_field / 360.0) * 360.0
    true_crs = brg_field - mag_part
    d = {"lat": lat, "lon": lon, "true_crs": round(true_crs, 3), "true_crs_raw": true_crs}

    if any(s in name.upper() for s in SKIP_NAMES):
        return "skip_type", d, None
    if (airport + " " + p[7].upper()) in SKIP_IDS:
        return "skip_listed", d, None

    # Is the stock LOC on the real runway's centerline? If not, the offset is
    # in the real procedure and must stay.
    real = real_runway(a.cifp, airport, rwy)
    if real:
        g = line_geom(*real, lat, lon, true_crs)
        if g:
            _, _, _, _, _, rcross, rdang, rmiss = g
            d.update(real_cross_m=round(rcross, 1), real_d_crs=round(rdang, 3),
                     real_thr_miss_m=None if rmiss is None else round(rmiss, 1))
            if abs(rdang) > a.real_angle or abs(rcross) > a.real_offset or \
                    (rmiss is not None and abs(rmiss) > a.real_offset):
                return "skip_real_offset", d, None

    if airport not in rwys:
        return "no_airport", d, None
    r = rwys[airport].get(rwy)
    if r is None:
        return "no_runway", d, None
    tla, tlo, ola, olo, _ = r

    # Centerline from this runway's end toward the far end (the direction an
    # aircraft on approach flies, i.e. the LOC course).
    g = line_geom(tla, tlo, ola, olo, lat, lon, true_crs)
    if g is None:
        return "bad_runway", d, None
    rwy_crs, rlen, ue, un, along, cross, dang, thr_miss = g
    d.update(rwy_crs=round(rwy_crs, 3), d_crs=round(dang, 3), cross_m=round(cross, 1),
             along_m=round(along, 0), rwy_len=round(rlen, 0),
             thr_miss_m=None if thr_miss is None else round(thr_miss, 1))

    if abs(dang) > a.max_angle:
        return "skip_angle", d, None
    if abs(cross) > a.max_offset:
        return "skip_offset", d, None
    if thr_miss is not None and abs(thr_miss) > a.max_offset:
        return "skip_offset", d, None
    if along < rlen * 0.5 or along > rlen + a.max_beyond:
        return "skip_position", d, None
    if abs(cross) < a.min_fix and (thr_miss is None or abs(thr_miss) < a.min_fix) \
            and abs(dang) < 0.001:
        return "ok", d, None

    ne, nn = along * ue, along * un
    nlat, nlon = from_en(tla, tlo, ne, nn)
    new_brg = mag_part + round(rwy_crs, 3) % 360
    q = list(p)
    q[1] = "%.9f" % nlat
    q[2] = "%.9f" % nlon
    q[6] = "%.3f" % new_brg
    d.update(new_lat=round(nlat, 9), new_lon=round(nlon, 9))
    big = max(abs(cross), abs(thr_miss or 0)) > a.review_offset
    return ("fixed_review" if big else "fixed"), d, q


# X-Plane rejects a user_nav.dat whose header isn't exactly the XPNAV1200
# "data cycle ..., build ..., metadata NavXP1200" line, so the source header is
# copied verbatim and ownership is recorded in a sidecar file (same format as
# the plugin's).
def sidecar_path(path):
    return os.path.join(os.path.dirname(os.path.abspath(path)), "user_nav.locfixer")


def file_hash(path):
    h, size = 1469598103934665603, 0
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 16)
            if not b:
                break
            size += len(b)
            for c in b:
                h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%d %016x" % (size, h)


def is_ours(path):
    if not os.path.isfile(path):
        return False
    try:
        with open(sidecar_path(path)) as f:
            if f.readline().strip() == file_hash(path):
                return True
    except OSError:
        pass
    # Files from 1.0.0 carried the marker in the header (which X-Plane rejected).
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return any("loc_fixer" in f.readline() for _ in range(3))


def fmt_nav(q):
    # Keep the XPNAV1200 column look; X-Plane only needs whitespace separation.
    return "%2s %13s %14s %8s %8s %5s %11s %s" % (
        q[0], q[1], q[2], q[3], q[4], q[5], q[6], " ".join(q[7:]))


def companions(q, old_crs, comp):
    """Rows that must travel with a corrected LOC.

    An ILS's localizer, glideslope and DME share an ident and airport (some
    airports reuse one ident for both runway ends). When user_nav.dat
    redefines the localizer, X-Plane drops the stock rows with that ident and
    airport, so we re-emit them. The glideslope's course (its
    bearing field is angle * 100000 + true course) follows the new LOC course.
    """
    new_crs = float(q[6]) % 360
    out = []
    for c in comp.get((q[7], q[8]), ()):
        if c[0] in ("4", "5"):
            continue  # localizers are emitted on their own, fixed or not
        c = list(c)
        if c[0] == "6":
            field = float(c[6])
            angle_part = math.floor(field / 1000.0) * 1000.0
            if abs(angdiff(field - angle_part, old_crs)) <= 1.0:
                c[6] = "%.3f" % (angle_part + new_crs)
        out.append(c)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xplane", help="X-Plane 12 root; discovers earth_nav.dat and every apt.dat in scenery_packs.ini order")
    ap.add_argument("--nav", help="earth_nav.dat to read (overrides discovery)")
    ap.add_argument("--apt", action="append", default=[], help="apt.dat, highest priority first; repeatable")
    ap.add_argument("-o", "--out", help="output user_nav.dat (default: <xplane>/Custom Data/user_nav.dat or ./user_nav.dat)")
    ap.add_argument("--report", help="CSV report path (default: next to output)")
    ap.add_argument("--max-angle", type=float, default=1.0, help="skip if LOC course and scenery runway differ by more (deg, default 1.0)")
    ap.add_argument("--max-offset", type=float, default=100.0, help="skip if LOC is further than this from the scenery centerline (m, default 100)")
    ap.add_argument("--review-offset", type=float, default=30.0, help="fixes bigger than this (m) are marked fixed_review in the report (default 30)")
    ap.add_argument("--max-beyond", type=float, default=2500.0, help="skip if LOC is further than this past the far runway end (m, default 2500)")
    ap.add_argument("--min-fix", type=float, default=0.5, help="leave alone if already within this many metres (default 0.5)")
    ap.add_argument("--include-types", default="", help="comma list of names normally skipped to include anyway, e.g. IGS")
    ap.add_argument("--skip-ids", default="", help='comma list of "AIRPORT IDENT" localizers to never touch, e.g. "RJBT ITH"')
    ap.add_argument("--cifp", help="CIFP folder with real runway thresholds (default: CIFP next to earth_nav.dat); 'none' to disable")
    ap.add_argument("--real-offset", type=float, default=15.0, help="skip as intentionally offset if the LOC or its threshold crossing is further than this from the real (CIFP) centerline (m, default 15)")
    ap.add_argument("--real-angle", type=float, default=0.5, help="skip as intentionally offset if the LOC course differs from the real runway by more (deg, default 0.5)")
    a = ap.parse_args()

    global SKIP_NAMES
    inc = {s.strip().upper() for s in a.include_types.split(",") if s.strip()}
    SKIP_NAMES = tuple(s for s in SKIP_NAMES if s not in inc)
    SKIP_IDS.update(" ".join(s.upper().split()) for s in a.skip_ids.split(",") if s.strip())

    nav, apts = a.nav, list(a.apt)
    if a.xplane:
        dnav, dapts = discover(a.xplane)
        nav = nav or dnav
        apts = apts or dapts
    if not nav or not apts:
        ap.error("need an earth_nav.dat and at least one apt.dat (use --xplane or --nav/--apt)")
    if a.cifp is None:
        a.cifp = os.path.join(os.path.dirname(os.path.abspath(nav)), "CIFP")
    if a.cifp.lower() == "none" or not os.path.isdir(a.cifp):
        a.cifp = None
    out = a.out or (os.path.join(a.xplane, "Custom Data", "user_nav.dat") if a.xplane else "user_nav.dat")
    report = a.report or os.path.splitext(out)[0] + "_report.csv"

    print("navdata: %s" % nav)
    print("CIFP:    %s" % (a.cifp or "(none, offset localizers only caught by the scenery checks)"))
    rwys = {}
    for p in apts:
        n = read_apt_runways(p, rwys)
        print("apt.dat: %s (%d airport keys)" % (p, n))

    # A user_nav.dat we didn't write is moved aside to user_nav_original.dat
    # once; its entries are merged into every run unless we replace the same
    # localizer. Put hand-made navaids there.
    existing = []
    orig = os.path.join(os.path.dirname(os.path.abspath(out)), "user_nav_original.dat")
    if os.path.isfile(out) and not os.path.exists(orig) and not is_ours(out):
            shutil.copyfile(out, orig)
            print("moved your existing user_nav.dat entries to %s (they are merged in)" % orig)
    if os.path.isfile(orig):
        with open(orig, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        existing = [l for l in lines[2:] if l.strip() and l.split()[0] != "99"]

    fixed, rows, counts, comp = [], [], {}, {}
    header = ""
    with open(nav, "r", encoding="utf-8", errors="replace") as f:
        f.readline()
        header = f.readline().strip()
        for line in f:
            p = line.split()
            if len(p) >= 11 and p[0] in ("4", "5", "6", "12", "13"):
                comp.setdefault((p[7], p[8]), []).append(p)
            if len(p) < 12 or p[0] not in ("4", "5"):
                continue
            status, d, q = fix_loc(p, rwys, a)
            counts[status] = counts.get(status, 0) + 1
            if q:
                fixed.append((q, d["true_crs_raw"]))
            rows.append(dict(status=status, type=p[0], ident=p[7], airport=p[8], region=p[9],
                             rwy=p[10], name=" ".join(p[11:]), **d))

    out_rows, seen = [], set()
    fixed_ids = {(q[7], q[8]) for q, _ in fixed}

    def emit(q):
        k = " ".join(q)
        if k not in seen:
            seen.add(k)
            out_rows.append(q)

    for q, old_crs in fixed:
        emit(q)
        for c in companions(q, old_crs, comp):
            emit(c)
    # Unfixed localizers that share an ident with a fixed one would be dropped too.
    fixed_keys = {(q[0], q[7], q[8], q[10]) for q, _ in fixed}
    for k in sorted(fixed_ids):
        for c in comp.get(k, ()):
            if c[0] in ("4", "5") and (c[0], c[7], c[8], c[10]) not in fixed_keys:
                emit(c)
    keys = {(q[0], q[7], q[8], q[9]) for q in out_rows}
    kept = []
    for l in existing:
        p = l.split()
        if len(p) >= 10 and (p[0], p[7], p[8], p[9]) in keys:
            continue
        kept.append(l)

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("I\n%s\n\n" % header)
        for l in kept:
            f.write(l + "\n")
        for q in out_rows:
            f.write(fmt_nav(q) + "\n")
        f.write("99\n")
    with open(sidecar_path(out), "w") as f:
        f.write(file_hash(out) + "\n")

    cols = ["status", "type", "ident", "airport", "region", "rwy", "name", "lat", "lon", "true_crs",
            "rwy_crs", "d_crs", "cross_m", "thr_miss_m", "along_m", "rwy_len", "new_lat", "new_lon",
            "real_d_crs", "real_cross_m", "real_thr_miss_m"]
    with open(report, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)

    print("\nresults:")
    for k in sorted(counts):
        print("  %-14s %d" % (k, counts[k]))
    print("\nwrote %s (%d fixed, %d hand-made entries kept)" % (out, len(fixed), len(kept)))
    print("wrote %s" % report)


if __name__ == "__main__":
    main()
