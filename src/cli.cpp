// Command-line front end, same options as loc_fixer.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "locfix_core.h"

static void usage() {
    std::puts(
        "usage: locfix --xplane DIR | --nav earth_nav.dat --apt apt.dat [--apt ...]\n"
        "              [-o user_nav.dat] [--report report.csv] [--max-angle 1.0] [--max-offset 100]\n"
        "              [--review-offset 30] [--max-beyond 2500] [--min-fix 0.5] [--include-types IGS,...]");
}

int main(int argc, char **argv) {
    locfix::Params p;
    std::string xp, nav, out, report;
    std::vector<std::string> apts;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if (a == "--xplane") xp = val();
        else if (a == "--nav") nav = val();
        else if (a == "--apt") apts.push_back(val());
        else if (a == "-o" || a == "--out") out = val();
        else if (a == "--report") report = val();
        else if (a == "--max-angle") p.max_angle = std::atof(val().c_str());
        else if (a == "--max-offset") p.max_offset = std::atof(val().c_str());
        else if (a == "--review-offset") p.review_offset = std::atof(val().c_str());
        else if (a == "--max-beyond") p.max_beyond = std::atof(val().c_str());
        else if (a == "--min-fix") p.min_fix = std::atof(val().c_str());
        else if (a == "--include-types") {
            std::string v = val();
            size_t s = 0;
            while (s <= v.size()) {
                size_t e = v.find(',', s);
                if (e == std::string::npos) e = v.size();
                std::string t = v.substr(s, e - s);
                for (auto &c : t) c = (char)std::toupper((unsigned char)c);
                p.skip_names.erase(t);
                s = e + 1;
            }
        } else { usage(); return a == "-h" || a == "--help" ? 0 : 2; }
    }
    if (!xp.empty()) {
        std::string dn;
        std::vector<std::string> da;
        locfix::discover(xp, dn, da);
        if (nav.empty()) nav = dn;
        if (apts.empty()) apts = da;
    }
    if (nav.empty() || apts.empty()) { usage(); return 2; }
    if (out.empty()) out = xp.empty() ? "user_nav.dat" : (std::filesystem::path(xp) / "Custom Data" / "user_nav.dat").string();
    if (report.empty()) report = (std::filesystem::path(out).parent_path() / "user_nav_report.csv").string();

    std::printf("navdata: %s\n", nav.c_str());
    for (auto &a : apts) std::printf("apt.dat: %s\n", a.c_str());
    auto s = locfix::run(nav, apts, out, report, p, "(cli)");
    if (!s.error.empty()) { std::fprintf(stderr, "error: %s\n", s.error.c_str()); return 1; }
    if (!s.backup_path.empty()) std::printf("moved your existing user_nav.dat entries to %s (they are merged in)\n", s.backup_path.c_str());
    std::printf("\nresults:\n");
    for (auto &kv : s.counts) std::printf("  %-14s %d\n", kv.first.c_str(), kv.second);
    std::printf("\nwrote %s (%d fixed, %d hand-made entries kept)\nwrote %s\n", out.c_str(), s.fixed,
                s.kept_existing, report.c_str());
    return 0;
}
