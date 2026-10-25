#!/usr/bin/env python3
"""
X-Plane 12 localizer alignment fixer.

Reads ILS/LOC emitters (earth_nav.dat row types 4 and 5) and the runways that
the scenery actually draws (apt.dat row type 100), then writes a
Custom Data/user_nav.dat in which each runway-paired localizer is moved
sideways onto the scenery runway's extended centerline and its course is set
to the scenery runway's true heading. The LOC keeps its along-track distance
from the runway, so only the lateral error is removed.

Localizers are left alone (and listed in the report) when:
  * they are not straight-in (LDA, SDF, IGS, or an intentionally offset course),
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
# main logic
# --------------------------------------------------------------------------

def fix_loc(p, rwys, a):
    """Return (status, detail dict, new_fields or None) for one LOC row."""
    lat, lon = float(p[1]), float(p[2])
    brg_field = float(p[6])
    airport, rwy, name = p[8].upper(), norm_rwy(p[10]), " ".join(p[11:])
    mag_part = math.floor(brg_field / 360.0) * 360.0
    true_crs = brg_field - mag_part
    d = {"lat": lat, "lon": lon, "true_crs": round(true_crs, 3)}

    if any(s in name.upper() for s in SKIP_NAMES):
        return "skip_type", d, None
    if airport not in rwys:
        return "no_airport", d, None
    r = rwys[airport].get(rwy)
    if r is None:
        return "no_runway", d, None
    tla, tlo, ola, olo, _ = r

    # Centerline unit vector, from this runway's end toward the far end
    # (the direction an aircraft on approach flies, i.e. the LOC course).
    oe, on = to_en(tla, tlo, ola, olo)
    rlen = math.hypot(oe, on)
    if rlen < 50:
        return "bad_runway", d, None
    ue, un = oe / rlen, on / rlen
    rwy_crs = math.degrees(math.atan2(ue, un)) % 360

    le, ln = to_en(tla, tlo, lat, lon)
    along = le * ue + ln * un               # m past the approach end
    cross = le * un - ln * ue               # + = LOC is right of centerline (looking along course)
    dang = angdiff(rwy_crs, true_crs)
    # Lateral miss at the threshold if you fly the LOC as published,
    # relative to the scenery centerline.
    beam_ue = math.sin(math.radians(true_crs))
    beam_un = math.cos(math.radians(true_crs))
    # beam point at threshold-along-track 0: LOC - t*beam, solve along=0
    denom = beam_ue * ue + beam_un * un
    thr_miss = None
    if abs(denom) > 1e-6:
        t = along / denom
        be, bn = le - t * beam_ue, ln - t * beam_un
        thr_miss = be * un - bn * ue
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


def fmt_nav(q):
    # Keep the XPNAV1200 column look; X-Plane only needs whitespace separation.
    return "%2s %13s %14s %8s %8s %5s %11s %s %s %s %s %s" % (
        q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], " ".join(q[11:]))


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
    a = ap.parse_args()

    global SKIP_NAMES
    inc = {s.strip().upper() for s in a.include_types.split(",") if s.strip()}
    SKIP_NAMES = tuple(s for s in SKIP_NAMES if s not in inc)

    nav, apts = a.nav, list(a.apt)
    if a.xplane:
        dnav, dapts = discover(a.xplane)
        nav = nav or dnav
        apts = apts or dapts
    if not nav or not apts:
        ap.error("need an earth_nav.dat and at least one apt.dat (use --xplane or --nav/--apt)")
    out = a.out or (os.path.join(a.xplane, "Custom Data", "user_nav.dat") if a.xplane else "user_nav.dat")
    report = a.report or os.path.splitext(out)[0] + "_report.csv"

    print("navdata: %s" % nav)
    rwys = {}
    for p in apts:
        n = read_apt_runways(p, rwys)
        print("apt.dat: %s (%d airport keys)" % (p, n))

    # A user_nav.dat we didn't write is moved aside to user_nav_original.dat
    # once; its entries are merged into every run unless we replace the same
    # localizer. Put hand-made navaids there.
    existing = []
    orig = os.path.join(os.path.dirname(os.path.abspath(out)), "user_nav_original.dat")
    if os.path.isfile(out) and not os.path.exists(orig):
        with open(out, "r", encoding="utf-8", errors="replace") as f:
            head = [f.readline() for _ in range(3)]
        if not any("loc_fixer" in l for l in head):
            shutil.copyfile(out, orig)
            print("moved your existing user_nav.dat entries to %s (they are merged in)" % orig)
    if os.path.isfile(orig):
        with open(orig, "r", encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        existing = [l for l in lines[2:] if l.strip() and l.split()[0] != "99"]

    fixed, rows, counts = [], [], {}
    header = ""
    with open(nav, "r", encoding="utf-8", errors="replace") as f:
        f.readline()
        header = f.readline().strip()
        for line in f:
            p = line.split()
            if len(p) < 12 or p[0] not in ("4", "5"):
                continue
            status, d, q = fix_loc(p, rwys, a)
            counts[status] = counts.get(status, 0) + 1
            if q:
                fixed.append(q)
            rows.append(dict(status=status, type=p[0], ident=p[7], airport=p[8], region=p[9],
                             rwy=p[10], name=" ".join(p[11:]), **d))

    keys = {(q[0], q[7], q[8], q[9]) for q in fixed}
    kept = []
    for l in existing:
        p = l.split()
        if len(p) >= 10 and (p[0], p[7], p[8], p[9]) in keys:
            continue
        kept.append(l)

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("I\n1200 Version - loc_fixer: %d localizers aligned to scenery. Source: %s\n\n"
                % (len(fixed), header[:120]))
        for l in kept:
            f.write(l + "\n")
        for q in fixed:
            f.write(fmt_nav(q) + "\n")
        f.write("99\n")

    cols = ["status", "type", "ident", "airport", "region", "rwy", "name", "lat", "lon", "true_crs",
            "rwy_crs", "d_crs", "cross_m", "thr_miss_m", "along_m", "rwy_len", "new_lat", "new_lon"]
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
