# XP12 LOC Fixer

X-Plane 12's localizers come from real-world navdata (`earth_nav.dat`), but the
runways you see come from scenery (`apt.dat`). When the scenery runway is drawn
a few metres off, a perfectly centred LOC needle puts you beside the runway.
LOC Fixer snaps each runway-paired localizer onto the scenery runway's extended
centerline and writes the result to `Custom Data/user_nav.dat`, which X-Plane
loads on top of the default and Navigraph navdata.

There are two ways to use it:

- **Plugin** (recommended): drop it in and it keeps `user_nav.dat` up to date
  by itself.
- **Command line**: `loc_fixer.py` (Python) or the `locfix` binary, if you'd
  rather generate the file yourself.

## Plugin

### Install

1. Download `LocFixer.zip`:
   - From a [release](../../releases) if one exists, or
   - From **Actions** > latest **build** run on `main` > **Artifacts** >
     `LocFixer` (you need to be signed in to GitHub).
2. Extract it so you get a `LocFixer` folder, and copy that folder into
   `X-Plane 12/Resources/plugins/`. It contains `win_x64/`, `mac_x64/` (Intel
   and Apple Silicon) and `lin_x64/`, plus `config.ini`.
3. Start X-Plane. The first start builds `Custom Data/user_nav.dat` (a few
   seconds) and shows a window asking you to **restart X-Plane once** so the
   sim loads it.
4. After the restart, **Plugins > LOC Fixer > Status** should say
   *Active in this session*.

The plugin rebuilds automatically, and asks for one more restart, whenever any
of these change: your scenery packs or their order in `scenery_packs.ini`, your
navdata (for example a Navigraph cycle update), `config.ini`, or the plugin
version.

### Menu: Plugins > LOC Fixer

| item | what it does |
|---|---|
| **Status** | How many localizers were aligned, and whether the fix is live. It checks one corrected localizer in X-Plane's own nav database. |
| **Rebuild now** | Regenerates `user_nav.dat` immediately. Restart to apply. |
| **Remove fix (restore stock localizers)** | Deletes the generated file, puts back any `user_nav.dat` you had before, and stops automatic rebuilds until you choose **Rebuild now**. Restart to apply. |

Status messages:

- **Active in this session**: working.
- **Restart X-Plane to apply it**: the file was just (re)built. Restart.
- **X-Plane is not using user_nav.dat**: it was built before this start but the
  sim isn't using it. Look in `Log.txt` for navdata errors.
- **Loaded both old and corrected localizers**: X-Plane added the entries
  instead of replacing them. Please open an issue.

Every message is also written to `Log.txt` on lines starting with `LOC Fixer:`.

### Settings

`LocFixer/config.ini` (delete a line to use the default):

| key | default | meaning |
|---|---|---|
| `max_angle` | 1.0 | Skip if the LOC course and scenery runway differ by more (degrees). |
| `max_offset` | 100 | Skip if the LOC, or where its beam crosses the threshold, is further than this from the scenery centerline (metres). |
| `review_offset` | 30 | Fixes bigger than this (metres) are marked `fixed_review` in the report. |
| `max_beyond` | 2500 | Skip if the LOC is further than this past the far runway end (metres). |
| `min_fix` | 0.5 | Leave alone if already within this many metres. |
| `skip_types` | `LDA, SDF, IGS` | Approach types that are deliberately not aligned with the runway. |
| `use_cifp` | 1 | Check each LOC against the real runway from the navdata's CIFP folder and leave intentionally offset ones alone (see below). 0 turns it off. |
| `real_offset` | 15 | Offset if the LOC antenna is further than this from the real centerline (metres). |
| `real_angle` | 0.5 | Offset if the LOC course differs from the real runway by more (degrees). |
| `skip_ids` | `RJBT ITH` | Localizers never to touch, as `AIRPORT IDENT`, comma separated. |

A per-localizer report is written to `LocFixer/report.csv`.

### Uninstall

Use **Remove fix**, then quit X-Plane and delete the `LocFixer` folder. To do
it by hand instead, delete `Custom Data/user_nav.dat` and
`Custom Data/user_nav.locfixer`, and rename `user_nav_original.dat` back to
`user_nav.dat` if you have one.

### Why a file and not on the fly?

[libradio](https://github.com/skiselkov/libradio) (used by the Challenger 650)
applies the same correction in memory, but it can only do that because it
replaces X-Plane's radios *and* autopilot for that one aircraft. The SDK gives
plugins no way to edit X-Plane's own nav database. So a plugin that works for
every aircraft (default autopilot, G1000, Zibo, and so on) has to feed X-Plane
corrected data through `user_nav.dat`, which X-Plane reads at startup.

## Command line

`loc_fixer.py` needs Python 3.8+ and nothing else. The `locfix` binary
(`locfix-win_x64.exe`, `locfix-mac_x64`, `locfix-lin_x64` in the separate
`locfix-cli` download) takes the same options and gives byte-identical output. Don't run
it while the plugin is installed, because the plugin manages the same file.

```
# Use everything from an X-Plane install (writes Custom Data/user_nav.dat)
python loc_fixer.py --xplane "C:/X-Plane 12"

# Explicit files; the first --apt wins for an airport, like scenery_packs.ini
python loc_fixer.py --nav earth_nav.dat --apt my_airport/apt.dat --apt global/apt.dat -o user_nav.dat
```

| option | meaning |
|---|---|
| `--xplane DIR` | Uses `Custom Data/earth_nav.dat` (Navigraph) if present, otherwise `Resources/default data/earth_nav.dat`. Reads every enabled scenery pack's `apt.dat` in `scenery_packs.ini` order, then the Global Airports `apt.dat`. |
| `--nav FILE` | `earth_nav.dat` to read (overrides discovery). |
| `--apt FILE` | `apt.dat`, highest priority first; repeatable (overrides discovery). |
| `-o, --out FILE` | Output. Default `<xplane>/Custom Data/user_nav.dat`, or `./user_nav.dat` without `--xplane`. |
| `--report FILE` | CSV report. Default `user_nav_report.csv` next to the output. |
| `--max-angle`, `--max-offset`, `--review-offset`, `--max-beyond`, `--min-fix` | Same as the plugin settings above. |
| `--include-types LIST` | Comma list of normally skipped types to align anyway, e.g. `IGS`. |
| `--cifp DIR` | CIFP folder with the real runway thresholds. Default: the `CIFP` folder next to the `earth_nav.dat` in use. `none` turns the check off. |
| `--real-offset`, `--real-angle` | Same as the plugin settings above. |
| `--skip-ids LIST` | Comma list of `AIRPORT IDENT` localizers never to touch, e.g. `"RJBT ITH"`. |

Re-run it after adding, removing or reordering scenery, or updating navdata,
then restart X-Plane.

## Files it writes

| file | purpose |
|---|---|
| `Custom Data/user_nav.dat` | The corrected localizers, plus the glideslope and DME of each corrected ILS. Line 2 is copied from your `earth_nav.dat`, because X-Plane rejects the file ("Unrecognized file format") otherwise. |
| `Custom Data/user_nav.locfixer` | Size and hash of the file above, so the tools know it is theirs and may overwrite it. |
| `Custom Data/user_nav_original.dat` | Only created if you already had your own `user_nav.dat`. It is moved here once and merged into every rebuild, unless a corrected ILS replaces the same entry. Keep hand-made navaids here. |
| report CSV | One row per localizer with its status and measurements (see below). |

## What it changes

For each type 4/5 localizer with a runway:

1. Find that runway in the scenery (`apt.dat` row 100; `01L` and `1L` match).
   The two runway end coordinates define the centerline.
2. Move the LOC **sideways** onto the extended centerline, keeping its distance
   from the runway, and set its true course to the scenery runway heading. The
   magnetic part of the XPNAV1200 bearing field is kept.

Both are needed. Changing only the course pivots the beam around an antenna
that is still off the centerline, so it crosses the runway at an angle and is
only right at one point. Moving it onto the line and matching the heading
makes the beam *be* the centerline.

A `user_nav.dat` localizer replaces every stock entry with the same ident at
that airport, so the glideslope and DME of each corrected ILS are written too.
Otherwise they would disappear. They keep their real positions. Only the
glideslope's course is updated to match the new localizer course.

## What it leaves alone

| status | meaning |
|---|---|
| `skip_type` | LDA, SDF, IGS: deliberately not aligned with the runway. |
| `skip_real_offset` | The stock LOC is off the *real* runway centerline (from CIFP) by more than 15 m or 0.5°, so the offset is part of the real procedure. |
| `skip_listed` | Listed in `skip_ids`. |
| `skip_angle` | Course differs from the runway by > 1° (offset approaches, or a wrong scenery runway). |
| `skip_offset` | LOC or its threshold crossing is > 100 m off the scenery centerline. Usually the scenery runway is misplaced (the LIML-style case), so "fixing" it would send you somewhere bogus. |
| `skip_position` | LOC isn't beyond the far end of the runway, where it should be. |
| `no_airport` / `no_runway` | Scenery doesn't have that airport or runway (often a renamed runway). |
| `ok` | Already within 0.5 m. |
| `fixed_review` | Fixed, but the shift was > 30 m; worth a look. |

### Intentionally offset localizers

Some localizers are deliberately not on the runway centerline. RJBT (Tajima)
01 is one: the LOC sits about 30 m east of the runway and its beam stays
parallel, which the chart notes as "Loc Crs offset". `earth_nav.dat` and
`apt.dat` alone can't tell that apart from a scenery error, because the
scenery runway is where the real one is and the LOC is simply off it.

The navdata's CIFP files can. Every `earth_nav.dat` comes with a `CIFP` folder
(`Custom Data/CIFP` for Navigraph, `Resources/default data/CIFP` for the
stock data) with one file per airport, and its `RWY:` records give the real
landing thresholds. Before aligning a LOC, the tools measure it against the
real centerline between the two thresholds:

* antenna on the real centerline: aligned as usual. Many stock courses are
  0.1 to 0.3 degrees off the real runway, which alone misses the threshold
  by 15 to 20 m; that is the error being fixed, not an offset, so the
  threshold miss is not used for this check;
* antenna more than `real_offset` m off, or course more than `real_angle`
  degrees off: the offset is real, so it is left alone as `skip_real_offset`;
* no CIFP file or runway: the scenery checks above still apply.

For RJBT 01 the LOC is 30.1 m off the real centerline (30.4 m at the
threshold) while the scenery runway is within 3.4 m of the real one, so it is
skipped. The report has `real_d_crs`, `real_cross_m` and `real_thr_miss_m`
columns for every localizer that has CIFP data. Anything the CIFP check
misses can be added to `skip_ids`.

## Results on the stock data (cycle 2608, global apt.dat)

Of 4,418 localizers, 4,000 were aligned (29 flagged for review) and 41 were
already fine. 127 were skipped for angle, 6 for offset, 45 as LDA/SDF/IGS, and
195 had no matching scenery runway. The output also carries 3,791 glideslopes
and 3,127 DMEs. The median fix is 0.8 m sideways and 3.6 m at the threshold;
the 95th percentile is 6 m and 12 m.

## Building

```
cmake -S . -B build -DXPSDK=/path/to/XPSDK
cmake --build build --config Release
```

Without `-DXPSDK` only the `locfix` CLI is built. CI builds Windows, Mac
(universal) and Linux on every push and pull request. Push a `v*` tag to
publish a release zip.

## License

[MIT](LICENSE). Free to use, modify and redistribute, including commercially,
as long as the copyright notice is kept. No code is taken from libradio
(CDDL); only the general idea of snapping localizers to the runway is shared.
The X-Plane SDK used to build the plugin is not included in this repository.
