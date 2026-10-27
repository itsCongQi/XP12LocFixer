// X-Plane 12 plugin: keeps Custom Data/user_nav.dat in step with the installed
// scenery and navdata, so every localizer lies on its scenery runway's
// centerline. X-Plane reads user_nav.dat at startup, so when the scenery or
// navdata changed since the last run, the new alignment applies after the
// next restart (or immediately, if X-Plane loads plugins before navdata).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMNavigation.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include "locfix_core.h"

namespace fs = std::filesystem;

#define PLUGIN_VERSION "1.1.1"

static std::string g_xp_root, g_plugin_root, g_user_nav, g_report, g_state_path;
static locfix::Params g_params;
static std::map<std::string, std::string> g_state;
static std::vector<std::string> g_status;  // lines shown in the status window
static std::mutex g_status_mx;
static XPLMWindowID g_window = nullptr;
static XPLMMenuID g_menu = nullptr;
static int g_menu_idx = -1;
static std::thread g_worker;
static std::atomic<bool> g_busy{false}, g_worker_done{false};
static locfix::RunSummary g_worker_result;
static bool g_regenerated_this_session = false;

static void log(const std::string &s) { XPLMDebugString(("LOC Fixer: " + s + "\n").c_str()); }

static void set_status(std::vector<std::string> lines) {
    std::lock_guard<std::mutex> l(g_status_mx);
    g_status = std::move(lines);
}

// ---------------------------------------------------------------- state / config

static void load_kv(const std::string &path, std::map<std::string, std::string> &kv) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto t = [](std::string s) {
            while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
            size_t i = 0;
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
            return s.substr(i);
        };
        kv[t(line.substr(0, eq))] = t(line.substr(eq + 1));
    }
}

static void save_state() {
    std::ofstream f(g_state_path);
    for (auto &kv : g_state) f << kv.first << " = " << kv.second << "\n";
}

static bool g_use_cifp = true;

static void load_config() {
    std::map<std::string, std::string> c;
    load_kv((fs::path(g_plugin_root) / "config.ini").string(), c);
    auto num = [&](const char *k, double &v) {
        if (c.count(k)) v = std::atof(c[k].c_str());
    };
    num("max_angle", g_params.max_angle);
    num("max_offset", g_params.max_offset);
    num("review_offset", g_params.review_offset);
    num("max_beyond", g_params.max_beyond);
    num("min_fix", g_params.min_fix);
    num("real_offset", g_params.real_offset);
    num("real_angle", g_params.real_angle);
    g_use_cifp = !c.count("use_cifp") || std::atoi(c["use_cifp"].c_str()) != 0;
    if (c.count("skip_ids")) {
        g_params.skip_ids.clear();
        std::stringstream ss(c["skip_ids"]);
        std::string t;
        while (std::getline(ss, t, ',')) {
            std::string id;
            for (auto &w : locfix::split_ws(t)) id += (id.empty() ? "" : " ") + w;
            for (auto &ch : id) ch = (char)std::toupper((unsigned char)ch);
            if (!id.empty()) g_params.skip_ids.insert(id);
        }
    }
    if (c.count("skip_types")) {
        g_params.skip_names.clear();
        std::stringstream ss(c["skip_types"]);
        std::string t;
        while (std::getline(ss, t, ',')) {
            auto w = locfix::split_ws(t);
            if (!w.empty()) {
                for (auto &ch : w[0]) ch = (char)std::toupper((unsigned char)ch);
                g_params.skip_names.insert(w[0]);
            }
        }
    }
}

static std::string file_sig(const std::string &p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    auto mt = fs::last_write_time(p, ec).time_since_epoch().count();
    return p + "|" + std::to_string(sz) + "|" + std::to_string((long long)mt);
}

static std::string fingerprint(const std::string &nav, const std::vector<std::string> &apts) {
    std::ostringstream s;
    s << PLUGIN_VERSION << ";" << g_params.max_angle << ";" << g_params.max_offset << ";"
      << g_params.review_offset << ";" << g_params.max_beyond << ";" << g_params.min_fix << ";";
    for (auto &n : g_params.skip_names) s << n << ",";
    s << ";" << g_use_cifp << ";" << g_params.real_offset << ";" << g_params.real_angle << ";";
    for (auto &n : g_params.skip_ids) s << n << ",";
    // CIFP files ship with the navdata, so a navdata update already changes
    // the earth_nav.dat signature; only the folder's presence is tracked here.
    s << ";" << (g_use_cifp ? locfix::cifp_dir_for(nav) : "");
    s << ";" << file_sig(nav);
    for (auto &a : apts) s << ";" << file_sig(a);
    std::string orig = locfix::original_path(g_user_nav);
    if (fs::exists(orig)) s << ";" << file_sig(orig);
    return std::to_string(std::hash<std::string>{}(s.str()));
}

// ---------------------------------------------------------------- generation

static locfix::RunSummary generate() {
    std::string nav;
    std::vector<std::string> apts;
    locfix::discover(g_xp_root, nav, apts);
    locfix::RunSummary s;
    if (nav.empty()) { s.error = "earth_nav.dat not found"; return s; }
    if (apts.empty()) { s.error = "no apt.dat found"; return s; }
    g_params.cifp_dir = g_use_cifp ? locfix::cifp_dir_for(nav) : "";
    s = locfix::run(nav, apts, g_user_nav, g_report, g_params, "(plugin " PLUGIN_VERSION ")");
    if (s.error.empty()) {
        g_state["fingerprint"] = fingerprint(nav, apts);
        g_state["nav"] = nav;
        g_state["apt_files"] = std::to_string(apts.size());
        // Remember the LOC that moved furthest (but not absurdly) so we can
        // check in the sim whether X-Plane is using the corrected position.
        const locfix::LocResult *best = nullptr;
        for (auto &r : s.results)
            if (r.fixed() && std::fabs(r.cross_m) >= 3 && std::fabs(r.cross_m) <= 30 &&
                (!best || std::fabs(r.cross_m) > std::fabs(best->cross_m)))
                best = &r;
        if (best) {
            char b[256];
            std::snprintf(b, sizeof b, "%s %s %s %.9f %.9f %.9f %.9f", best->fields[7].c_str(),
                          best->fields[4].c_str(), best->fields[8].c_str(), best->lat, best->lon,
                          best->new_lat, best->new_lon);
            g_state["probe"] = b;
        } else {
            g_state.erase("probe");
        }
    }
    return s;
}

static void report_summary(const locfix::RunSummary &s) {
    if (!s.error.empty()) {
        log("error: " + s.error);
        return;
    }
    std::ostringstream o;
    o << "wrote " << g_user_nav << ": " << s.fixed << " localizers aligned";
    for (auto &kv : s.counts) o << ", " << kv.first << "=" << kv.second;
    log(o.str());
    if (!s.backup_path.empty()) log("your previous user_nav.dat was moved to " + s.backup_path + " and is merged in");
}

// ---------------------------------------------------------------- verification

static double dist_m(double la1, double lo1, double la2, double lo2) {
    const double R = 6371000.0, d2r = 3.14159265358979323846 / 180.0;
    double x = (lo2 - lo1) * d2r * std::cos((la1 + la2) * 0.5 * d2r), y = (la2 - la1) * d2r;
    return R * std::hypot(x, y);
}

enum class Applied { Unknown, Yes, No, Duplicate };

static Applied check_applied(std::string &detail) {
    auto it = g_state.find("probe");
    if (it == g_state.end()) return Applied::Unknown;
    char id[64], freq[32], apt[16];
    double olat, olon, nlat, nlon;
    if (std::sscanf(it->second.c_str(), "%63s %31s %15s %lf %lf %lf %lf", id, freq, apt, &olat, &olon,
                    &nlat, &nlon) != 7)
        return Applied::Unknown;
    int at_orig = 0, at_new = 0;
    XPLMNavRef first = XPLMFindFirstNavAidOfType(xplm_Nav_Localizer);
    XPLMNavRef last = XPLMFindLastNavAidOfType(xplm_Nav_Localizer);
    if (first == XPLM_NAV_NOT_FOUND) return Applied::Unknown;
    for (XPLMNavRef r = first; r != XPLM_NAV_NOT_FOUND; r = XPLMGetNextNavAid(r)) {
        char rid[32] = {0};
        float lat, lon;
        int f;
        XPLMGetNavAidInfo(r, nullptr, &lat, &lon, nullptr, &f, nullptr, rid, nullptr, nullptr);
        if (std::strcmp(rid, id) == 0 && std::to_string(f) == freq) {
            if (dist_m(lat, lon, nlat, nlon) < 1.5) at_new++;
            else if (dist_m(lat, lon, olat, olon) < 1.5) at_orig++;
        }
        if (r == last) break;
    }
    detail = std::string(id) + " at " + apt;
    if (at_new && at_orig) return Applied::Duplicate;
    if (at_new) return Applied::Yes;
    if (at_orig) return Applied::No;
    return Applied::Unknown;
}

static void show_window(bool show);

static void verify_and_report(const locfix::RunSummary *s) {
    std::vector<std::string> lines;
    lines.push_back("LOC Fixer " PLUGIN_VERSION);
    if (g_state["disabled"] == "1") {
        lines.push_back("Disabled. Use Plugins > LOC Fixer > Rebuild to turn it back on.");
        set_status(lines);
        return;
    }
    if (s && !s->error.empty()) {
        lines.push_back("Error: " + s->error);
        set_status(lines);
        show_window(true);
        return;
    }
    if (s) {
        char b[160];
        std::snprintf(b, sizeof b, "%d localizers aligned to scenery runways.", s->fixed);
        lines.push_back(b);
        int skipped = 0;
        for (auto &kv : s->counts)
            if (kv.first.rfind("skip_", 0) == 0) skipped += kv.second;
        std::snprintf(b, sizeof b, "%d left alone (offset approaches or scenery too far off).", skipped);
        lines.push_back(b);
        auto cnt = [&](const char *k) { auto i = s->counts.find(k); return i == s->counts.end() ? 0 : i->second; };
        if (cnt("skip_real_offset") || cnt("skip_listed")) {
            std::snprintf(b, sizeof b, "  of which intentionally offset: %d from CIFP, %d from skip_ids.",
                          cnt("skip_real_offset"), cnt("skip_listed"));
            lines.push_back(b);
        }
    }
    std::string detail;
    Applied a = check_applied(detail);
    bool attention = false;
    switch (a) {
    case Applied::Yes: lines.push_back("Active in this session (checked " + detail + ")."); break;
    case Applied::No:
        if (g_regenerated_this_session) {
            lines.push_back("Alignment updated for your current scenery/navdata.");
            lines.push_back("Restart X-Plane to apply it.");
        } else {
            lines.push_back("X-Plane is not using user_nav.dat (checked " + detail + ").");
            lines.push_back("Check Log.txt for navdata errors.");
        }
        attention = true;
        break;
    case Applied::Duplicate:
        lines.push_back("X-Plane loaded both old and corrected localizers (" + detail + ").");
        lines.push_back("Overrides are not replacing; please report this.");
        attention = true;
        break;
    case Applied::Unknown: lines.push_back("Could not verify in the sim."); break;
    }
    lines.push_back("Report: " + g_report);
    set_status(lines);
    {
        std::lock_guard<std::mutex> l(g_status_mx);
        for (size_t i = 1; i < g_status.size(); i++) log(g_status[i]);
    }
    if (attention) show_window(true);
}

// ---------------------------------------------------------------- window

static void draw_window(XPLMWindowID id, void *) {
    int l, t, r, b;
    XPLMGetWindowGeometry(id, &l, &t, &r, &b);
    float col[] = {1.0f, 1.0f, 1.0f};
    std::lock_guard<std::mutex> lk(g_status_mx);
    int y = t - 20;
    for (auto &s : g_status) {
        XPLMDrawString(col, l + 12, y, (char *)s.c_str(), nullptr, xplmFont_Proportional);
        y -= 16;
    }
}
static int mouse_window(XPLMWindowID, int, int, XPLMMouseStatus, void *) { return 1; }
static XPLMCursorStatus cursor_window(XPLMWindowID, int, int, void *) { return xplm_CursorDefault; }
static int wheel_window(XPLMWindowID, int, int, int, int, void *) { return 1; }
static void key_window(XPLMWindowID, char, XPLMKeyFlags, char, void *, int) {}

static void show_window(bool show) {
    if (!g_window) {
        int l, t, r, b;
        XPLMGetScreenBoundsGlobal(&l, &t, &r, &b);
        XPLMCreateWindow_t p;
        std::memset(&p, 0, sizeof p);
        p.structSize = sizeof p;
        p.left = l + 80;
        p.top = t - 120;
        p.right = l + 80 + 520;
        p.bottom = t - 120 - 150;
        p.visible = 0;
        p.drawWindowFunc = draw_window;
        p.handleMouseClickFunc = mouse_window;
        p.handleRightClickFunc = mouse_window;
        p.handleMouseWheelFunc = wheel_window;
        p.handleKeyFunc = key_window;
        p.handleCursorFunc = cursor_window;
        p.layer = xplm_WindowLayerFloatingWindows;
        p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
        g_window = XPLMCreateWindowEx(&p);
        XPLMSetWindowPositioningMode(g_window, xplm_WindowPositionFree, -1);
        XPLMSetWindowTitle(g_window, "LOC Fixer");
    }
    XPLMSetWindowIsVisible(g_window, show ? 1 : 0);
}

// ---------------------------------------------------------------- menu / loop

enum { MENU_STATUS = 1, MENU_REBUILD, MENU_REMOVE };

static void start_rebuild() {
    if (g_busy.exchange(true)) return;
    set_status({"LOC Fixer " PLUGIN_VERSION, "Rebuilding..."});
    show_window(true);
    if (g_worker.joinable()) g_worker.join();
    g_worker_done = false;
    g_worker = std::thread([] {
        g_worker_result = generate();
        g_worker_done = true;
    });
}

static void remove_fix() {
    if (g_busy) return;
    std::error_code ec;
    if (locfix::is_ours(g_user_nav)) fs::remove(g_user_nav, ec);
    locfix::unmark(g_user_nav);
    std::string orig = locfix::original_path(g_user_nav);
    if (fs::exists(orig, ec)) fs::rename(orig, g_user_nav, ec);
    g_state["disabled"] = "1";
    g_state.erase("fingerprint");
    save_state();
    log("removed generated user_nav.dat");
    set_status({"LOC Fixer " PLUGIN_VERSION, "Removed. The stock localizers return after a restart.",
                "Rebuild turns it back on."});
    show_window(true);
}

static void menu_handler(void *, void *item) {
    switch ((int)(intptr_t)item) {
    case MENU_STATUS: show_window(true); break;
    case MENU_REBUILD:
        if (g_busy) break;
        g_state.erase("disabled");
        start_rebuild();
        break;
    case MENU_REMOVE: remove_fix(); break;
    }
}

static bool g_verified = false;

static float flight_loop(float, float, int, void *) {
    if (!g_verified) {
        g_verified = true;
        verify_and_report(g_regenerated_this_session ? &g_worker_result : nullptr);
    }
    if (g_busy && g_worker_done) {
        g_worker.join();
        g_busy = false;
        report_summary(g_worker_result);
        if (g_worker_result.error.empty()) {
            g_regenerated_this_session = true;
            save_state();
        }
        verify_and_report(&g_worker_result);
        show_window(true);
    }
    return 1.0f;
}

// ---------------------------------------------------------------- plugin API

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
    std::strcpy(name, "LOC Fixer");
    std::strcpy(sig, "itscongqi.locfixer");
    std::strcpy(desc, "Aligns localizers with scenery runway centerlines via Custom Data/user_nav.dat.");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    char buf[2048];
    XPLMGetSystemPath(buf);
    g_xp_root = buf;
    XPLMGetPluginInfo(XPLMGetMyID(), nullptr, buf, nullptr, nullptr);
    // .../plugins/LocFixer/<platform>_x64/LocFixer.xpl
    g_plugin_root = fs::path(buf).parent_path().parent_path().string();
    g_user_nav = (fs::path(g_xp_root) / "Custom Data" / "user_nav.dat").string();
    g_report = (fs::path(g_plugin_root) / "report.csv").string();
    g_state_path = (fs::path(g_plugin_root) / "state.txt").string();

    load_config();
    load_kv(g_state_path, g_state);

    if (g_state["disabled"] != "1") {
        std::string nav;
        std::vector<std::string> apts;
        locfix::discover(g_xp_root, nav, apts);
        bool stale = !locfix::is_ours(g_user_nav) || nav.empty() ||
                     g_state["fingerprint"] != fingerprint(nav, apts);
        if (stale) {
            bool nav_loaded = XPLMGetFirstNavAid() != XPLM_NAV_NOT_FOUND;
            log(std::string("scenery or navdata changed, regenerating (navdata already loaded: ") +
                (nav_loaded ? "yes" : "no") + ")");
            g_worker_result = generate();
            report_summary(g_worker_result);
            if (g_worker_result.error.empty()) {
                g_regenerated_this_session = nav_loaded;  // if not loaded yet, it applies now
                save_state();
            }
        } else {
            log("user_nav.dat is up to date");
        }
    }
    return 1;
}

PLUGIN_API void XPluginStop(void) {
    if (g_worker.joinable()) g_worker.join();
    if (g_window) XPLMDestroyWindow(g_window);
    g_window = nullptr;
}

PLUGIN_API int XPluginEnable(void) {
    g_menu_idx = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "LOC Fixer", nullptr, 0);
    g_menu = XPLMCreateMenu("LOC Fixer", XPLMFindPluginsMenu(), g_menu_idx, menu_handler, nullptr);
    XPLMAppendMenuItem(g_menu, "Status", (void *)(intptr_t)MENU_STATUS, 0);
    XPLMAppendMenuItem(g_menu, "Rebuild now", (void *)(intptr_t)MENU_REBUILD, 0);
    XPLMAppendMenuItem(g_menu, "Remove fix (restore stock localizers)", (void *)(intptr_t)MENU_REMOVE, 0);
    XPLMRegisterFlightLoopCallback(flight_loop, 2.0f, nullptr);
    return 1;
}

PLUGIN_API void XPluginDisable(void) {
    XPLMUnregisterFlightLoopCallback(flight_loop, nullptr);
    if (g_menu) XPLMDestroyMenu(g_menu);
    if (g_menu_idx >= 0) XPLMRemoveMenuItem(XPLMFindPluginsMenu(), g_menu_idx);
    g_menu = nullptr;
    g_menu_idx = -1;
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void *) {}
