// Localizer-to-scenery alignment, shared by the X-Plane plugin and the CLI.
// No X-Plane SDK dependency.
#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace locfix {

struct Params {
    double max_angle = 1.0;      // deg, skip if LOC course vs runway differ more
    double max_offset = 100.0;   // m, skip if LOC/threshold crossing further off centerline
    double review_offset = 30.0; // m, fixes bigger than this are "fixed_review"
    double max_beyond = 2500.0;  // m, skip if LOC further past the far runway end
    double min_fix = 0.5;        // m, leave alone if already this close
    std::set<std::string> skip_names = {"LDA", "SDF", "IGS"};
    std::set<std::string> skip_ids;  // "AIRPORT IDENT", e.g. "RJBT ITH": never touched
    // Real runway thresholds from the navdata's CIFP/<ICAO>.dat. A stock LOC
    // off the real centerline is intentionally offset and left alone.
    std::string cifp_dir;        // empty: no check
    double real_offset = 15.0;   // m
    double real_angle = 0.5;     // deg
};


struct RunwayEnd {
    double lat, lon, opp_lat, opp_lon;
};

// airport key (ident and 1302 icao_code) -> runway id (no leading zero) -> end
using RunwayDb = std::map<std::string, std::map<std::string, RunwayEnd>>;

// Real runway thresholds per airport, read from CIFP files on first use.
class CifpDb {
public:
    explicit CifpDb(std::string dir) : dir_(std::move(dir)) {}
    // Threshold of rwy and of its opposite end; false if unknown.
    bool runway(const std::string &airport, const std::string &rwy, RunwayEnd &out);
private:
    std::string dir_;
    std::map<std::string, std::map<std::string, std::pair<double, double>>> cache_;
};

// Add land runways from one apt.dat; airports already present (from a higher
// priority file) are not overridden. Returns number of airport keys added,
// or -1 if the file can't be opened.
int read_apt_runways(const std::string &path, RunwayDb &db);

struct LocResult {
    std::string status;  // fixed, fixed_review, ok, skip_*, no_airport, no_runway
    std::vector<std::string> fields;  // original row split on whitespace
    double lat = 0, lon = 0, true_crs = 0;
    bool has_geom = false;
    double rwy_crs = 0, d_crs = 0, cross_m = 0, along_m = 0, rwy_len = 0;
    bool has_thr_miss = false;
    double thr_miss_m = 0;
    double new_lat = 0, new_lon = 0, new_brg_field = 0;
    bool has_real = false, has_real_thr_miss = false;  // vs the CIFP runway
    double real_d_crs = 0, real_cross_m = 0, real_thr_miss_m = 0;
    bool fixed() const { return status == "fixed" || status == "fixed_review"; }
};

LocResult fix_loc(const std::vector<std::string> &fields, const RunwayDb &db, const Params &p,
                  CifpDb *cifp = nullptr);

struct RunSummary {
    std::string error;            // non-empty on failure
    std::map<std::string, int> counts;
    int fixed = 0, kept_existing = 0;
    std::vector<LocResult> results;
    std::string backup_path;      // set if a hand-made user_nav.dat was moved aside this run
};

// Read nav, apply fixes, write user_nav.dat (merging any hand-made entries
// already there) and a CSV report. report_path may be empty.
RunSummary run(const std::string &nav_path, const std::vector<std::string> &apt_paths,
               const std::string &out_path, const std::string &report_path,
               const Params &p, const std::string &tag);

// X-Plane install discovery. nav: Custom Data/earth_nav.dat if present else
// Resources/default data/earth_nav.dat. apts: enabled packs from
// scenery_packs.ini in order, then Global Airports.
void discover(const std::string &xp_root, std::string &nav, std::vector<std::string> &apts);

// CIFP folder that belongs to this earth_nav.dat (its sibling), or empty.
std::string cifp_dir_for(const std::string &nav_path);

// Where a pre-existing hand-made user_nav.dat is kept.
std::string original_path(const std::string &out_path);
// True if the file was written by us (checked against the user_nav.locfixer
// sidecar next to it).
bool is_ours(const std::string &path);
void mark_ours(const std::string &path);
void unmark(const std::string &path);

// Marker 1.0.0 wrote into the header; still recognised.
extern const char *kMarker;

std::vector<std::string> split_ws(const std::string &s);

}  // namespace locfix
