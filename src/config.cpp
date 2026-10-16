#include "config.hpp"
#include "logger.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>

namespace vksumi
{
    namespace
    {
        std::mutex                              g_configMutex;
        std::shared_ptr<const Config>           g_config;
        std::atomic<bool>                       g_watcherRunning{false};
        int                                     g_watcherStopFd[2] = {-1, -1};

        constexpr const char* TEMPLATE = R"VKSUMI(# vkSumi per-game config, auto-created on first launch.
# Merges on top of ~/.config/vkSumi/vkSumi.conf, only sets what's enabled in this file.

# 0 = no change for every knob.
# + = more / brighter.
# - = less / darker.

# enabled = true

# brightness  = 0.0
# contrast    = 0.0
# exposure    = 0.0
# gamma       = 0.0

# saturation  = 0.0
# vibrance    = 0.0
# hue_deg     = 0.0
# temperature = 0.0
# tint        = 0.0

# red_gain    = 0.0
# green_gain  = 0.0
# blue_gain   = 0.0

# shadows     = 0.0
# midtones    = 0.0
# highlights  = 0.0

# toggle_keys = Shift_R+F9

# privacy censor, up to 2 boxes: censor0 and censor1. see vkSumi.conf.example for every field.
# censor0.enabled = false
# censor0.x        = 0.81
# censor0.y        = 0.027
# censor0.w        = 0.14
# censor0.h        = 0.036
)VKSUMI";

        constexpr std::string_view kCensorPrefix = "censor";

        std::string trim(const std::string& s)
        {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return {};
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        }

        bool parseBool(const std::string& v, bool fallback)
        {
            std::string s;
            s.reserve(v.size());
            for (char c : v) s.push_back(static_cast<char>(::tolower(c)));
            if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
            if (s == "0" || s == "false" || s == "no" || s == "off") return false;
            return fallback;
        }

        bool parseFloat(const std::string& v, float& out)
        {
            try { out = std::stof(v); return true; }
            catch (...) { return false; }
        }

        std::string envOr(const char* name, const std::string& fallback)
        {
            const char* v = std::getenv(name);
            return v ? std::string(v) : fallback;
        }

        std::string expandPath(std::string path)
        {
            if (path.empty()) return path;
            if (path[0] == '~')
            {
                path.replace(0, 1, envOr("HOME", "."));
            }

            auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
                size_t start_pos = 0;
                while((start_pos = str.find(from, start_pos)) != std::string::npos) {
                    str.replace(start_pos, from.length(), to);
                    start_pos += to.length();
                }
            };

            replaceAll(path, "$USER", envOr("USER", ""));
            replaceAll(path, "$HOME", envOr("HOME", "."));

            return path;
        }

        CensorShape censorShapeFromStr(const std::string& s)
        {
            return s == "ellipse" ? CensorShape::Ellipse : CensorShape::Rect;
        }

        CensorMode censorModeFromStr(const std::string& s)
        {
            if (s == "blur") return CensorMode::Blur;
            if (s == "fill") return CensorMode::Fill;
            return CensorMode::Pixelate;
        }

        bool applyCensorKey(Config& cfg, const std::string& key, const std::string& val)
        {
            const size_t dot = key.find('.');
            if (key.rfind(kCensorPrefix, 0) != 0 || dot != kCensorPrefix.size() + 1) return false;

            const size_t idx = static_cast<size_t>(key[kCensorPrefix.size()] - '0');
            if (idx >= kMaxCensor) return false;

            CensorRegion&     r     = cfg.censors[idx];
            const std::string field = key.substr(dot + 1);

            #define F(name) if (field == #name) { parseFloat(val, r.name); return true; }
            F(x)
            F(y)
            F(w)
            F(h)
            F(strength)
            F(feather)
            #undef F
            if (field == "enabled") { r.enabled = parseBool(val, r.enabled); return true; }
            if (field == "shape")   { r.shape   = censorShapeFromStr(val);    return true; }
            if (field == "mode")    { r.mode    = censorModeFromStr(val);     return true; }
            return false;
        }

        void applyKey(Config& cfg, const std::string& key, const std::string& val)
        {
            #define F(name) if (key == #name) { float f; if (parseFloat(val, f)) cfg.knobs.name = f; return; }
            F(brightness)
            F(contrast)
            F(exposure)
            F(saturation)
            F(vibrance)
            F(hue_deg)
            F(gamma)
            F(temperature)
            F(tint)
            F(red_gain)
            F(green_gain)
            F(blue_gain)
            F(shadows)
            F(midtones)
            F(highlights)
            #undef F
            if (key == "enabled")     { cfg.enabled = parseBool(val, true); return; }
            if (key == "toggle_keys") { cfg.toggle_keys = val; return; }
            if (key == "VKSUMI_CONFIG_FILE") { cfg.vksumi_config_file = expandPath(val); return; }
            if (key == "PER_GAME_CONFIG_CREATION") { cfg.per_game_config_creation = parseBool(val, true); return; }
            if (applyCensorKey(cfg, key, val)) return;
            VKSUMI_TRACE("conf: unknown key '%s'", key.c_str());
        }

        bool parseFile(const std::string& path, Config& cfg)
        {
            std::ifstream in(path);
            if (!in) return false;
            std::string line;
            while (std::getline(in, line))
            {
                size_t hash = line.find('#');
                if (hash != std::string::npos) line.resize(hash);
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = trim(line.substr(0, eq));
                std::string val = trim(line.substr(eq + 1));
                if (key.empty() || val.empty()) continue;
                applyKey(cfg, key, val);
            }
            cfg.sources.push_back(path);
            return true;
        }
        std::string basename(const std::string& path)
        {
            size_t fwd = path.find_last_of('/');
            size_t bwd = path.find_last_of('\\');
            size_t cut = std::string::npos;
            if (fwd != std::string::npos) cut = fwd;
            if (bwd != std::string::npos && (cut == std::string::npos || bwd > cut)) cut = bwd;
            return cut == std::string::npos ? path : path.substr(cut + 1);
        }

        std::string dirname(const std::string& path)
        {
            size_t cut = path.find_last_of('/');
            return cut == std::string::npos ? "." : path.substr(0, cut);
        }

        std::string absolutePath(const std::string& path)
        {
            if (path.empty() || path[0] == '/') return path;
            char cwd[4096];
            return getcwd(cwd, sizeof(cwd)) ? std::string(cwd) + "/" + path : path;
        }

        bool endsWithIcase(const std::string& s, const std::string& suffix)
        {
            if (s.size() < suffix.size()) return false;
            for (size_t i = 0; i < suffix.size(); ++i)
            {
                char a = static_cast<char>(::tolower(s[s.size() - suffix.size() + i]));
                char b = static_cast<char>(::tolower(suffix[i]));
                if (a != b) return false;
            }
            return true;
        }

        std::string exeBasename()
        {
            std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
            if (cmdline)
            {
                std::string buf((std::istreambuf_iterator<char>(cmdline)),
                                 std::istreambuf_iterator<char>());
                std::string lastExe;
                size_t start = 0;
                for (size_t i = 0; i <= buf.size(); ++i)
                {
                    if (i == buf.size() || buf[i] == '\0')
                    {
                        if (i > start)
                        {
                            std::string arg = buf.substr(start, i - start);
                            if (endsWithIcase(arg, ".exe"))
                                lastExe = basename(arg);
                        }
                        start = i + 1;
                    }
                }
                if (!lastExe.empty()) return lastExe;
            }

            char buf[4096];
            ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
            if (n <= 0) return {};
            buf[n] = '\0';
            return basename(buf);
        }

        bool fileExists(const std::string& p)
        {
            struct stat st{};
            return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        }

        void mkdir_p(const std::string& path)
        {
            std::string cur;
            for (size_t i = 0; i <= path.size(); ++i)
            {
                if (i == path.size() || path[i] == '/')
                {
                    if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
                    if (i < path.size()) cur.push_back('/');
                }
                else cur.push_back(path[i]);
            }
        }

        std::string configHomeDir()
        {
            std::string xdg = envOr("XDG_CONFIG_HOME", envOr("HOME", ".") + "/.config");
            return xdg + "/vkSumi";
        }

        std::string perGameConfName()
        {
            std::string b = exeBasename();
            if (b.empty()) return {};
            if (endsWithIcase(b, ".exe")) b.resize(b.size() - 4);
            if (b.empty()) return {};
            return b + ".conf";
        }

        std::string perGameConfPath()
        {
            std::string n = perGameConfName();
            return n.empty() ? std::string{} : configHomeDir() + "/games/" + n;
        }

        std::vector<std::string> buildLadder()
        {
            std::vector<std::string> ladder;

            ladder.push_back(configHomeDir() + "/vkSumi.conf");

            std::string per = perGameConfPath();
            if (!per.empty()) ladder.push_back(per);

            ladder.push_back(absolutePath("vkSumi.conf"));

            if (const char* p = std::getenv("VKSUMI_CONFIG_FILE"))
                ladder.push_back(absolutePath(p));

            return ladder;
        }
    }

    void ensurePerGameConfig()
    {
        if (!currentConfig()->per_game_config_creation) return;

        std::string p = perGameConfPath();
        if (p.empty() || fileExists(p)) return;

        mkdir_p(configHomeDir() + "/games");
        std::ofstream out(p);
        if (!out) return;
        out << TEMPLATE;
        VKSUMI_LOG("created per-game conf: %s", p.c_str());

        setCurrentConfig(loadConfig());
    }

    std::shared_ptr<const Config> loadConfig()
    {
        auto cfg = std::make_shared<Config>();
        auto ladder = buildLadder();
        auto searchPaths = ladder;

        bool any = false;

        if (!ladder.empty())
        {
            if (parseFile(ladder[0], *cfg))
            {
                any = true;
                if (!cfg->vksumi_config_file.empty())
                {
                    std::string redirect = absolutePath(cfg->vksumi_config_file);
                    *cfg = Config();
                    ladder[0] = redirect;
                    searchPaths.push_back(redirect);
                    any = false;
                }
                else
                {
                    VKSUMI_TRACE("merged conf: %s", ladder[0].c_str());
                }
            }
        }

        for (size_t i = any ? 1 : 0; i < ladder.size(); ++i)
        {
            const auto& p = ladder[i];
            if (parseFile(p, *cfg)) { any = true; VKSUMI_TRACE("merged conf: %s", p.c_str()); }
        }

        if (any)
            VKSUMI_LOG("config loaded (%zu file%s)",
                       cfg->sources.size(), cfg->sources.size() == 1 ? "" : "s");
        else
            VKSUMI_LOG("no config files found, using defaults");

        cfg->search_paths = std::move(searchPaths);
        return cfg;
    }

    std::shared_ptr<const Config> currentConfig()
    {
        {
            std::lock_guard<std::mutex> l(g_configMutex);
            if (g_config) return g_config;
        }
        auto loaded = loadConfig();
        std::lock_guard<std::mutex> l(g_configMutex);
        if (!g_config) g_config = loaded;
        return g_config;
    }

    void setCurrentConfig(std::shared_ptr<const Config> next)
    {
        std::lock_guard<std::mutex> l(g_configMutex);
        g_config = std::move(next);
    }

    GradeParams buildGradeParams(const Config& cfg)
    {
        GradeParams p{};
        p.knobs = cfg.enabled ? cfg.knobs : Knobs{};
        for (size_t i = 0; i < kMaxCensor; ++i)
        {
            const CensorRegion& r = cfg.censors[i];
            if (!r.enabled || r.w <= 0.0f || r.h <= 0.0f) continue;
            const float rx = r.w * 0.5f;
            const float ry = r.h * 0.5f;
            p.region[i] = { r.x + rx, r.y + ry, rx, ry,
                            r.strength, r.feather,
                            static_cast<float>(r.shape), static_cast<float>(r.mode) };
        }
        return p;
    }

    static void watcherLoop()
    {
        int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (ifd < 0) { VKSUMI_LOG("inotify_init1 failed: %s", std::strerror(errno)); return; }

        mkdir_p(configHomeDir() + "/games");
        const uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;

        std::unordered_map<int, std::string> watchedDirs;
        auto syncWatches = [&](const Config& cfg)
        {
            for (const auto& path : cfg.search_paths)
            {
                const std::string dir = dirname(path);
                int wd = inotify_add_watch(ifd, dir.c_str(), mask);
                if (wd < 0) { VKSUMI_TRACE("inotify_add_watch(%s) failed: %s", dir.c_str(), std::strerror(errno)); continue; }
                if (watchedDirs.emplace(wd, dir).second) VKSUMI_LOG("watching %s for conf changes", dir.c_str());
            }
        };

        syncWatches(*currentConfig());
        if (watchedDirs.empty()) { close(ifd); return; }

        struct pollfd pfds[2];
        pfds[0] = { ifd,                  POLLIN, 0 };
        pfds[1] = { g_watcherStopFd[0],   POLLIN, 0 };

        char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

        while (g_watcherRunning.load())
        {
            int n = poll(pfds, 2, -1);
            if (n < 0) { if (errno == EINTR) continue; break; }
            if (pfds[1].revents & POLLIN) break;
            if (!(pfds[0].revents & POLLIN)) continue;

            ssize_t got = read(ifd, buf, sizeof(buf));
            if (got <= 0) continue;

            const auto  cfg   = currentConfig();
            const auto& paths = cfg->search_paths;
            bool need_reload = false;
            for (char* p = buf; p < buf + got; )
            {
                auto* ev = reinterpret_cast<struct inotify_event*>(p);
                auto  it = watchedDirs.find(ev->wd);
                if (ev->len > 0 && it != watchedDirs.end())
                {
                    const std::string full = it->second + "/" + ev->name;
                    if (std::find(paths.begin(), paths.end(), full) != paths.end()) need_reload = true;
                }
                p += sizeof(struct inotify_event) + ev->len;
            }

            if (need_reload)
            {
                auto next = loadConfig();
                setCurrentConfig(next);
                syncWatches(*next);
                VKSUMI_LOG("config reloaded");
            }
        }

        for (const auto& [wd, dir] : watchedDirs) inotify_rm_watch(ifd, wd);
        close(ifd);
    }

    void startConfigWatcher()
    {
        bool expected = false;
        if (!g_watcherRunning.compare_exchange_strong(expected, true)) return;

        if (pipe(g_watcherStopFd) != 0) { g_watcherRunning.store(false); return; }
        std::thread(watcherLoop).detach();
    }

    void stopConfigWatcher()
    {
        if (!g_watcherRunning.exchange(false)) return;
        if (g_watcherStopFd[1] >= 0) { (void)!::write(g_watcherStopFd[1], "x", 1); }
    }
}
