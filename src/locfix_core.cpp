// Port of loc_fixer.py. Keep the two in step.
#include "locfix_core.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace locfix {

const char *kMarker = "loc_fixer";

static const double A = 6378137.0;
static const double E2 = 6.69437999014e-3;
static const double PI = 3.14159265358979323846;
static double rad(double d) { return d * PI / 180.0; }
static double deg(double r) { return r * 180.0 / PI; }

static void radii(double lat, double &rm, double &rp) {
    double s = std::sin(rad(lat));
    double w = std::sqrt(1 - E2 * s * s);
    double n = A / w;
    rm = A * (1 - E2) / (w * w * w);
    rp = n * std::cos(rad(lat));
}

static double wrap180(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

static void to_en(double lat0, double lon0, double lat, double lon, double &e, double &n) {
    double rm, rp;
    radii(lat0, rm, rp);
    e = rad(wrap180(lon - lon0)) * rp;
    n = rad(lat - lat0) * rm;
}

static void from_en(double lat0, double lon0, double e, double n, double &lat, double &lon) {
    double rm, rp;
    radii(lat0, rm, rp);
    lat = lat0 + deg(n / rm);
    lon = lon0 + deg(e / rp);
}

static std::string upper(std::string s) {
    for (auto &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// '01L' and '1L' are the same runway; apt.dat and earth_nav.dat differ.
static std::string norm_rwy(const std::string &r) {
    std::string u = upper(r);
    size_t i = u.find_first_not_of('0');
    return i == std::string::npos ? u : u.substr(i);
}

std::vector<std::string> split_ws(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        while (i < n && std::isspace((unsigned char)s[i])) i++;
        size_t j = i;
        while (j < n && !std::isspace((unsigned char)s[j])) j++;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

static bool to_d(const std::string &s, double &v) {
    char *end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == 0 && end != s.c_str();
}

int read_apt_runways(const std::string &path, RunwayDb &db) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return -1;
    std::set<std::string> added;
    std::map<std::string, RunwayEnd> cur;
    bool active = false;
    std::vector<std::string> keys;

    auto avail = [&](const std::string &k) { return !db.count(k) || added.count(k); };
    auto close = [&]() {
        if (active && !cur.empty())
            for (auto &k : keys)
                if (avail(k)) {
                    auto &m = db[k];
                    for (auto &kv : cur) m[kv.first] = kv.second;
                    added.insert(k);
                }
        cur.clear();
        keys.clear();
        active = false;
    };

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || (line[0] != '1' && line[0] != ' ')) continue;
        auto p = split_ws(line);
        if (p.empty()) continue;
        const std::string &t = p[0];
        if (t == "1" || t == "16" || t == "17") {
            close();
            if (t == "1" && p.size() >= 5) {
                std::string ident = upper(p[4]);
                if (avail(ident)) {
                    active = true;
                    keys.push_back(ident);
                }
            }
        } else if (!active) {
            continue;
        } else if (t == "1302" && p.size() >= 3 && p[1] == "icao_code") {
            std::string icao = upper(p[2]);
            bool have = false;
            for (auto &k : keys) have |= (k == icao);
            if (!have && avail(icao)) keys.push_back(icao);
        } else if (t == "100" && p.size() >= 20) {
            double la1, lo1, la2, lo2;
            if (!to_d(p[9], la1) || !to_d(p[10], lo1) || !to_d(p[18], la2) || !to_d(p[19], lo2))
                continue;
            cur[norm_rwy(p[8])] = {la1, lo1, la2, lo2};
            cur[norm_rwy(p[17])] = {la2, lo2, la1, lo1};
        }
    }
    close();
    return (int)added.size();
}

LocResult fix_loc(const std::vector<std::string> &p, const RunwayDb &db, const Params &a) {
    LocResult r;
    r.fields = p;
    double brg_field;
    if (p.size() < 12 || !to_d(p[1], r.lat) || !to_d(p[2], r.lon) || !to_d(p[6], brg_field)) {
        r.status = "bad_row";
        return r;
    }
    std::string airport = upper(p[8]), rwy = norm_rwy(p[10]);
    std::string name;
    for (size_t i = 11; i < p.size(); i++) name += (i > 11 ? " " : "") + p[i];
    // XPNAV1200 LOC bearing = magnetic course * 360 + true course.
    double mag_part = std::floor(brg_field / 360.0) * 360.0;
    r.true_crs = brg_field - mag_part;

    std::string uname = upper(name);
    for (auto &s : a.skip_names)
        if (uname.find(s) != std::string::npos) { r.status = "skip_type"; return r; }
    auto ai = db.find(airport);
    if (ai == db.end()) { r.status = "no_airport"; return r; }
    auto ri = ai->second.find(rwy);
    if (ri == ai->second.end()) { r.status = "no_runway"; return r; }
    const RunwayEnd &e = ri->second;

    // Centerline from this end toward the far end (the LOC course).
    double oe, on;
    to_en(e.lat, e.lon, e.opp_lat, e.opp_lon, oe, on);
    double rlen = std::hypot(oe, on);
    if (rlen < 50) { r.status = "bad_runway"; return r; }
    double ue = oe / rlen, un = on / rlen;
    double rwy_crs = std::fmod(deg(std::atan2(ue, un)) + 360.0, 360.0);

    double le, ln;
    to_en(e.lat, e.lon, r.lat, r.lon, le, ln);
    double along = le * ue + ln * un;
    double cross = le * un - ln * ue;
    double dang = wrap180(rwy_crs - r.true_crs);
    // Where the published beam crosses the threshold, relative to the centerline.
    double bue = std::sin(rad(r.true_crs)), bun = std::cos(rad(r.true_crs));
    double denom = bue * ue + bun * un;
    if (std::fabs(denom) > 1e-6) {
        double t = along / denom;
        double be = le - t * bue, bn = ln - t * bun;
        r.has_thr_miss = true;
        r.thr_miss_m = be * un - bn * ue;
    }
    r.has_geom = true;
    r.rwy_crs = rwy_crs; r.d_crs = dang; r.cross_m = cross; r.along_m = along; r.rwy_len = rlen;

    double tm = r.has_thr_miss ? std::fabs(r.thr_miss_m) : 0;
    if (std::fabs(dang) > a.max_angle) { r.status = "skip_angle"; return r; }
    if (std::fabs(cross) > a.max_offset || tm > a.max_offset) { r.status = "skip_offset"; return r; }
    if (along < rlen * 0.5 || along > rlen + a.max_beyond) { r.status = "skip_position"; return r; }
    if (std::fabs(cross) < a.min_fix && (!r.has_thr_miss || tm < a.min_fix) && std::fabs(dang) < 0.001) {
        r.status = "ok";
        return r;
    }
    from_en(e.lat, e.lon, along * ue, along * un, r.new_lat, r.new_lon);
    r.new_brg_field = mag_part + std::fmod(std::round(rwy_crs * 1000.0) / 1000.0, 360.0);
    r.status = std::max(std::fabs(cross), tm) > a.review_offset ? "fixed_review" : "fixed";
    return r;
}

static std::string fmt_nav(const LocResult &r) {
    const auto &q = r.fields;
    char lat[32], lon[32], brg[32];
    std::snprintf(lat, sizeof lat, "%.9f", r.new_lat);
    std::snprintf(lon, sizeof lon, "%.9f", r.new_lon);
    std::snprintf(brg, sizeof brg, "%.3f", r.new_brg_field);
    std::string rest;
    for (size_t i = 11; i < q.size(); i++) rest += (i > 11 ? " " : "") + q[i];
    char buf[512];
    std::snprintf(buf, sizeof buf, "%2s %13s %14s %8s %8s %5s %11s %s %s %s %s %s", q[0].c_str(), lat,
                  lon, q[3].c_str(), q[4].c_str(), q[5].c_str(), brg, q[7].c_str(), q[8].c_str(),
                  q[9].c_str(), q[10].c_str(), rest.c_str());
    return buf;
}

static std::string csv_field(const std::string &s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) o += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

static std::string num(double v, int prec) {
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

std::string original_path(const std::string &out_path) {
    return (fs::path(out_path).parent_path() / "user_nav_original.dat").string();
}

// X-Plane rejects a user_nav.dat whose header isn't exactly the XPNAV1200
// "data cycle ..., build ..., metadata NavXP1200" line, so we copy the source
// header verbatim and record ownership in a sidecar file instead.
static std::string sidecar_path(const std::string &path) {
    return (fs::path(path).parent_path() / "user_nav.locfixer").string();
}

static std::string file_hash(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64
    char buf[65536];
    unsigned long long size = 0;
    while (f.read(buf, sizeof buf) || f.gcount() > 0) {
        std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; i++) {
            h ^= (unsigned char)buf[i];
            h *= 1099511628211ULL;
        }
        size += (unsigned long long)n;
    }
    char out[64];
    std::snprintf(out, sizeof out, "%llu %016llx", size, (unsigned long long)h);
    return out;
}

bool is_ours(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    std::ifstream side(sidecar_path(path));
    std::string recorded;
    if (std::getline(side, recorded) && !recorded.empty() && recorded == file_hash(path)) return true;
    // Files from 1.0.0 carried the marker in the header (which X-Plane rejected).
    std::ifstream f(path, std::ios::binary);
    std::string line;
    for (int i = 0; i < 3 && std::getline(f, line); i++)
        if (line.find(kMarker) != std::string::npos) return true;
    return false;
}

void mark_ours(const std::string &path) {
    std::ofstream side(sidecar_path(path));
    side << file_hash(path) << "\n";
}

void unmark(const std::string &path) {
    std::error_code ec;
    fs::remove(sidecar_path(path), ec);
}

RunSummary run(const std::string &nav_path, const std::vector<std::string> &apt_paths,
               const std::string &out_path, const std::string &report_path, const Params &p,
               const std::string &tag) {
    RunSummary s;
    RunwayDb db;
    for (auto &ap : apt_paths) read_apt_runways(ap, db);

    std::ifstream nf(nav_path, std::ios::binary);
    if (!nf) { s.error = "cannot open " + nav_path; return s; }
    std::string line, header;
    std::getline(nf, line);
    std::getline(nf, header);
    while (!header.empty() && (header.back() == '\r' || header.back() == ' ')) header.pop_back();
    while (std::getline(nf, line)) {
        if (line.size() < 2) continue;
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || (line[i] != '4' && line[i] != '5')) continue;
        auto f = split_ws(line);
        if (f.size() < 12 || (f[0] != "4" && f[0] != "5")) continue;
        s.results.push_back(fix_loc(f, db, p));
        s.counts[s.results.back().status]++;
    }
    nf.close();

    // A user_nav.dat we didn't write is moved aside to user_nav_original.dat
    // once; its entries are merged into every run unless we replace the same
    // localizer. Put hand-made navaids there.
    std::vector<std::string> existing;
    std::error_code ec;
    std::string orig = original_path(out_path);
    if (fs::exists(out_path, ec) && !is_ours(out_path) && !fs::exists(orig, ec)) {
        fs::copy_file(out_path, orig, ec);
        if (!ec) s.backup_path = orig;
    }
    if (fs::exists(orig, ec)) {
        std::ifstream ef(orig, std::ios::binary);
        int n = 0;
        while (std::getline(ef, line)) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            if (n++ < 2) continue;
            auto f = split_ws(line);
            if (!f.empty() && f[0] != "99") existing.push_back(line);
        }
    }

    std::set<std::string> keys;
    for (auto &r : s.results)
        if (r.fixed()) {
            keys.insert(r.fields[0] + "|" + r.fields[7] + "|" + r.fields[8] + "|" + r.fields[9]);
            s.fixed++;
        }
    std::vector<std::string> kept;
    for (auto &l : existing) {
        auto f = split_ws(l);
        if (f.size() >= 10 && keys.count(f[0] + "|" + f[7] + "|" + f[8] + "|" + f[9])) continue;
        kept.push_back(l);
    }
    s.kept_existing = (int)kept.size();

    fs::create_directories(fs::path(out_path).parent_path(), ec);
    std::string tmp = out_path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary);
        if (!o) { s.error = "cannot write " + tmp; return s; }
        (void)tag;
        o << "I\n" << header << "\n\n";
        for (auto &l : kept) o << l << "\n";
        for (auto &r : s.results)
            if (r.fixed()) o << fmt_nav(r) << "\n";
        o << "99\n";
        if (!o) { s.error = "write failed " + tmp; return s; }
    }
    fs::rename(tmp, out_path, ec);
    if (ec) {
        fs::remove(out_path, ec);
        fs::rename(tmp, out_path, ec);
        if (ec) { s.error = "cannot replace " + out_path; return s; }
    }
    mark_ours(out_path);

    if (!report_path.empty()) {
        std::ofstream c(report_path, std::ios::binary);
        c << "status,type,ident,airport,region,rwy,name,lat,lon,true_crs,rwy_crs,d_crs,cross_m,"
             "thr_miss_m,along_m,rwy_len,new_lat,new_lon\n";
        for (auto &r : s.results) {
            const auto &f = r.fields;
            std::string name;
            for (size_t i = 11; i < f.size(); i++) name += (i > 11 ? " " : "") + f[i];
            c << r.status << "," << f[0] << "," << csv_field(f[7]) << "," << f[8] << "," << f[9] << ","
              << f[10] << "," << csv_field(name) << "," << num(r.lat, 9) << "," << num(r.lon, 9) << ","
              << num(r.true_crs, 3);
            if (r.has_geom)
                c << "," << num(r.rwy_crs, 3) << "," << num(r.d_crs, 3) << "," << num(r.cross_m, 1) << ","
                  << (r.has_thr_miss ? num(r.thr_miss_m, 1) : "") << "," << num(r.along_m, 0) << ","
                  << num(r.rwy_len, 0);
            else
                c << ",,,,,,";
            if (r.fixed()) c << "," << num(r.new_lat, 9) << "," << num(r.new_lon, 9);
            else c << ",,";
            c << "\n";
        }
    }
    return s;
}

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}

void discover(const std::string &xp_root, std::string &nav, std::vector<std::string> &apts) {
    fs::path xp(xp_root);
    std::error_code ec;
    nav.clear();
    for (auto c : {xp / "Custom Data" / "earth_nav.dat", xp / "Resources" / "default data" / "earth_nav.dat"})
        if (fs::is_regular_file(c, ec)) { nav = c.string(); break; }
    apts.clear();
    std::set<std::string> seen;
    std::ifstream ini(xp / "Custom Scenery" / "scenery_packs.ini");
    std::string line;
    const std::string key = "SCENERY_PACK ";
    while (std::getline(ini, line)) {
        line = trim(line);
        if (line.rfind(key, 0) != 0) continue;
        std::string rel = trim(line.substr(key.size()));
        while (!rel.empty() && (rel.back() == '/' || rel.back() == '\\')) rel.pop_back();
        fs::path base = rel == "*GLOBAL_AIRPORTS*" ? xp / "Global Scenery" / "Global Airports"
                                                   : (fs::path(rel).is_absolute() ? fs::path(rel) : xp / rel);
        fs::path c = base / "Earth nav data" / "apt.dat";
        if (fs::is_regular_file(c, ec) && seen.insert(c.string()).second) apts.push_back(c.string());
    }
    fs::path g = xp / "Global Scenery" / "Global Airports" / "Earth nav data" / "apt.dat";
    if (fs::is_regular_file(g, ec) && seen.insert(g.string()).second) apts.push_back(g.string());
}

}  // namespace locfix
