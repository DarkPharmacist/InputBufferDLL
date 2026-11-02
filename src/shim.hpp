
#include <Geode/Geode.hpp>
#include <Geode/loader/Dirs.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Log.hpp>
#include <matjson.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
using namespace geode::prelude;
namespace cbfshim {
    struct Entry {
        std::string_view type;
        std::string_view def;
    };
    static const std::map<std::string_view, Entry, std::less<>>& settingsTable() {
        static const std::map<std::string_view, Entry, std::less<>> table = {
            {"custom-ticks",              {"int",    "40"}},
            {"custom-wave-ticks",         {"int",    "20"}},
            {"sim-budget-ms",             {"float",  "8.0"}},
            {"smart-prediction",          {"bool",   "true"}},
            {"debug-console",             {"bool",   "false"}},
            {"ground-horizon",            {"int",    "130"}},
            {"practice-only",             {"bool",   "false"}},
            {"wave-horizon",              {"int",    "65"}},
            {"orb-assume-hit",            {"bool",   "true"}},
            {"orb-try-both",              {"bool",   "true"}},
            {"avoid-moving-objects",      {"bool",   "false"}},
            {"wave-auto-release",         {"bool",   "false"}},
            {"auto-release-window",       {"int",    "6"}},
            {"drift-check",               {"bool",   "false"}},
            {"wave-spam-gap",             {"int",    "16"}},
            {"wave-spam-streak",          {"int",    "3"}},
            {"orb-align",                 {"bool",   "true"}},
            {"orb-align-ticks",           {"int",    "16"}},
            {"wave-v46",                  {"bool",   "false"}},
            {"wave-safety-margin",        {"bool",   "true"}},
            {"show-path",                 {"bool",   "false"}},
            {"orb-align-ground",          {"bool",   "false"}},
            {"full-ghost-state",          {"bool",   "true"}},
            {"robot-smart",               {"bool",   "true"}},
            {"robot-auto-release",        {"bool",   "false"}},
            {"auto-click",                {"bool",   "false"}},
            {"level-memory",              {"bool",   "true"}},
            {"auto-release-normal-runs",  {"bool",   "false"}},
            {"spider-smart",              {"bool",   "true"}},
            {"dash-auto-release",         {"bool",   "false"}},
            {"perf-log",                  {"bool",   "false"}},
            {"ship-buffer",               {"bool",   "true"}},
            {"ship-max-delay",            {"int",    "4"}},
            {"orb-timing",                {"bool",   "true"}},
            {"cube-late-crash",           {"int",    "90"}},
            {"predict-moving-objects",    {"bool",   "false"}},
            {"predict-move-triggers",     {"bool",   "true"}},
            {"no-hitch-searches",         {"bool",   "true"}},
            {"plan-next-click",           {"bool",   "true"}},
            {"robust-saves",              {"bool",   "true"}},
            {"ignore-touching-hazards",   {"bool",   "true"}},
            {"death-log",                 {"bool",   "false"}},
            {"explain-inputs",            {"bool",   "false"}},
            {"cbf-sync",                  {"bool",   "true"}},
            {"low-latency",               {"bool",   "true"}},
            {"ufo-smart",                 {"bool",   "true"}},
            {"more-time-death-spots",     {"bool",   "true"}},
            {"ship-spam-gap",             {"int",    "6"}},
            {"ball-smart",                {"bool",   "true"}},
            {"swing-smart",               {"bool",   "true"}},
            {"training-mode",             {"bool",   "false"}},
            {"per-level-settings",        {"bool",   "false"}},
            {"death-heatmap",             {"bool",   "true"}},
            {"save-ding",                 {"bool",   "true"}},
            {"performance-mode",          {"bool",   "false"}},
            {"predict-through-portals",   {"bool",   "true"}},
            {"auto-budget",               {"bool",   "true"}},
            {"check-updates",             {"bool",   "false"}},
            {"menu-theme",                {"string", "Purple"}},
            {"bind-stage",                {"keybind", "Six"}},
            {"bind-menu",                 {"keybind", "Seven"}},
            {"bind-export",               {"keybind", "F8"}},
            {"bind-console",              {"keybind", "F9"}},
            {"bind-path",                 {"keybind", "F10"}},
            {"bind-forget",               {"keybind", "F11"}},
            {"tps",                       {"int",    "240"}},
        };
        return table;
    }
    static std::map<std::string, std::string, std::less<>>& launchArgs() {
        static std::map<std::string, std::string, std::less<>> args;
        static bool parsed = false;
        if (parsed) {
            return args;
        }
        parsed = true;
    #ifdef GEODE_IS_WINDOWS
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) {
            return args;
        }
        for (int i = 0; i < argc; ++i) {
            std::string arg;
            int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                arg.resize(static_cast<size_t>(len - 1));
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), len, nullptr, nullptr);
            }
            constexpr std::string_view prefix = "--ib:";
            if (arg.starts_with(prefix)) {
                std::string_view body(arg);
                body.remove_prefix(prefix.size());
                auto eq = body.find('=');
                if (eq == std::string_view::npos) {
                    args.emplace(std::string(body), "true");
                }
                else {
                    std::string key(body.substr(0, eq));
                    std::string value(body.substr(eq + 1));
                    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                        (value.front() == '\'' && value.back() == '\''))) {
                        value = value.substr(1, value.size() - 2);
                    }
                    args[key] = std::move(value);
                }
            }
        }
        LocalFree(argv);
    #endif
        return args;
    }
    static bool parseBool(std::string_view text, bool fallback) {
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
        std::string v;
        v.reserve(text.size());
        for (char c : text) {
            v.push_back(lower(c));
        }
        if (v == "true" || v == "yes" || v == "on" || v == "1") return true;
        if (v == "false" || v == "no" || v == "off" || v == "0") return false;
        return fallback;
    }
    static std::filesystem::path saveFile() {
        return geode::dirs::getModsSaveDir() / "input-buffer" / "saved.json";
    }
    static matjson::Value& savedData() {
        static matjson::Value data = [] {
            std::ifstream in(saveFile());
            if (in) {
                auto res = matjson::parse(in);
                if (res.isOk() && res.unwrap().isObject()) {
                    return std::move(res).unwrap();
                }
            }
            return matjson::Value::object();
        }();
        return data;
    }
    static std::mutex& savedMutex() {
        static std::mutex m;
        return m;
    }
    static void persistSaved() {
        std::lock_guard<std::mutex> lock(savedMutex());
        std::error_code ec;
        auto dir = saveFile().parent_path();
        std::filesystem::create_directories(dir, ec);
        std::string dump = savedData().dump(4);
        std::ofstream(saveFile(), std::ios::trunc) << dump;
    }
    class ShimMod {
    public:
        bool getBool(std::string_view key) const {
            auto& args = launchArgs();
            auto it = args.find(key);
            if (it != args.end()) {
                return parseBool(it->second, tableBool(key));
            }
            auto o = overridesBool.find(key);
            if (o != overridesBool.end()) {
                return o->second;
            }
            return tableBool(key);
        }
        int64_t getInt(std::string_view key) const {
            auto& args = launchArgs();
            auto it = args.find(key);
            if (it != args.end()) {
                int64_t v = 0;
                auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
                if (ec == std::errc()) {
                    return v;
                }
            }
            return tableInt(key);
        }
        double getFloat(std::string_view key) const {
            auto& args = launchArgs();
            auto it = args.find(key);
            if (it != args.end()) {
                try {
                    return std::stod(it->second);
                }
                catch (...) {}
            }
            return tableFloat(key);
        }
        std::string getString(std::string_view key) const {
            auto& args = launchArgs();
            auto it = args.find(key);
            if (it != args.end()) {
                return it->second;
            }
            return tableString(key);
        }
        void setBool(std::string_view key, bool value) {
            overridesBool[std::string(key)] = value;
        }
        void setInt(std::string_view key, int64_t value) {
            overridesInt[std::string(key)] = value;
        }
        bool hasSaved(std::string_view key) const {
            auto& args = launchArgs();
            if (args.find(key) != args.end()) {
                return true;
            }
            std::lock_guard<std::mutex> lock(savedMutex());
            return savedData().contains(key);
        }
        template <class T>
        T getSaved(std::string_view key, T def) const {
            {
                auto& args = launchArgs();
                auto it = args.find(key);
                if (it != args.end()) {
                    if constexpr (std::is_same_v<T, bool>) {
                        return parseBool(it->second, def);
                    }
                    else if constexpr (std::is_integral_v<T>) {
                        T v = 0;
                        auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
                        if (ec == std::errc()) {
                            return v;
                        }
                        return def;
                    }
                    else if constexpr (std::is_floating_point_v<T>) {
                        try {
                            return static_cast<T>(std::stod(it->second));
                        }
                        catch (...) {
                            return def;
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lock(savedMutex());
            auto& data = savedData();
            if (data.contains(key)) {
                auto v = data[key].template as<T>();
                if (v.isOk()) {
                    return std::move(v).unwrap();
                }
            }
            return def;
        }
        template <class T>
        void setSaved(std::string_view key, T const& value) {
            {
                std::lock_guard<std::mutex> lock(savedMutex());
                savedData()[key] = matjson::Value(value);
            }
            persistSaved();
        }
        matjson::Value getSavedJson(std::string_view key) const {
            std::lock_guard<std::mutex> lock(savedMutex());
            auto& data = savedData();
            if (data.contains(key)) {
                return data[key];
            }
            return matjson::Value(nullptr);
        }
        void setSavedJson(std::string_view key, matjson::Value value) {
            {
                std::lock_guard<std::mutex> lock(savedMutex());
                savedData()[key] = std::move(value);
            }
            persistSaved();
        }
        std::filesystem::path getSaveDir() const {
            return saveFile().parent_path();
        }
    private:
        std::map<std::string, bool, std::less<>> overridesBool;
        std::map<std::string, int64_t, std::less<>> overridesInt;
        static const Entry* entry(std::string_view key) {
            auto& table = settingsTable();
            auto it = table.find(key);
            return it == table.end() ? nullptr : &it->second;
        }
        bool tableBool(std::string_view key) const {
            auto* e = entry(key);
            return e ? parseBool(e->def, false) : false;
        }
        int64_t tableInt(std::string_view key) const {
            auto* e = entry(key);
            if (!e) {
                return 0;
            }
            int64_t v = 0;
            auto [ptr, ec] = std::from_chars(e->def.data(), e->def.data() + e->def.size(), v);
            return ec == std::errc() ? v : 0;
        }
        double tableFloat(std::string_view key) const {
            auto* e = entry(key);
            if (!e) {
                return 0.0;
            }
            try {
                return std::stod(std::string(e->def));
            }
            catch (...) {
                return 0.0;
            }
        }
        std::string tableString(std::string_view key) const {
            auto* e = entry(key);
            return e ? std::string(e->def) : std::string();
        }
    };
    static ShimMod& mod() {
        static ShimMod instance;
        return instance;
    }
}
