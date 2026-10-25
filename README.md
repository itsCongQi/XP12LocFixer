# XP12 LOC Fixer

X-Plane 12's localizers come from real-world navdata (`earth_nav.dat`), but the
runways you see come from scenery (`apt.dat`). When the scenery runway is drawn
a few metres off, a perfectly centred LOC needle puts you beside the runway.
This tool snaps each runway-paired localizer onto the scenery runway's extended
centerline and writes the result to `Custom Data/user_nav.dat`, which X-Plane
loads on top of the default and Navigraph navdata.

## Usage

Python 3.8+, no dependencies.

```
python loc_fixer.py --xplane "C:/X-Plane 12"
```

This reads `Custom Data/earth_nav.dat` (Navigraph) if present, otherwise
`Resources/default data/earth_nav.dat`, and every enabled scenery pack's
`apt.dat` in `scenery_packs.ini` order (the same pack that wins in the sim wins
here), then the Global Airports `apt.dat`. It writes
`Custom Data/user_nav.dat` and `Custom Data/user_nav_report.csv`.

**Re-run it after you add/remove/reorder scenery or update navdata.** The
output is specific to your scenery set.

Explicit files instead:

```
python loc_fixer.py --nav earth_nav.dat --apt my_airport/apt.dat --apt global/apt.dat -o user_nav.dat
```

If a `user_nav.dat` you made yourself already exists it is backed up to
`user_nav.dat.bak` and its entries are kept unless the tool replaces the same
localizer. To undo, delete `user_nav.dat`.

## What it changes

For each type 4/5 LOC row with a runway:

1. Find that runway in the scenery (`apt.dat` row 100; `01L` and `1L` match).
2. Move the LOC **sideways** onto the extended centerline, keeping its distance
   from the runway, and set its true course to the scenery runway heading. The
   magnetic part of the XPNAV1200 bearing field is kept.

Both are needed: changing only the course pivots the beam around an antenna
that is still off the centerline, so it crosses the runway at an angle and is
only right at one point. Moving it onto the line and matching the heading makes
the beam *be* the centerline.

Glideslopes and DMEs are not moved; a few metres sideways doesn't affect them.

## What it leaves alone

| status | meaning |
|---|---|
| `skip_type` | LDA, SDF, IGS: deliberately not aligned with the runway |
| `skip_angle` | course differs from the runway by > 1° (offset approaches, or a wrong scenery runway) |
| `skip_offset` | LOC or its threshold crossing is > 100 m off the scenery centerline: usually the scenery runway is misplaced (the LIML-style case), so "fixing" would send you somewhere bogus |
| `skip_position` | LOC isn't beyond the far end of the runway where it should be |
| `no_airport` / `no_runway` | scenery doesn't have that airport/runway (often renamed runways) |
| `ok` | already within 0.5 m |
| `fixed_review` | fixed, but the shift was > 30 m; worth a look |

Thresholds: `--max-angle`, `--max-offset`, `--review-offset`, `--max-beyond`,
`--min-fix`. Run `python loc_fixer.py -h`.

## Results on the stock data (cycle 2608, global apt.dat)

Of 4,418 LOCs: 4,000 aligned (29 flagged for review), 41 already fine, 127
skipped for angle, 6 for offset, 45 LDA/SDF/IGS, 195 with no matching scenery
runway. Typical fix: median 0.8 m lateral and 3.6 m at the threshold, 95th
percentile 6 m / 12 m.
