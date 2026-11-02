
#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <map>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif
#include <ctime>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <climits>
#include <cmath>
#include <complex>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace geode::prelude;
#include "shim.hpp"
namespace {
    struct ModShim {
        template <class T>
        T getSettingValue(std::string_view key) const {
            if constexpr (std::is_same_v<T, bool>) {
                return cbfshim::mod().getBool(key);
            }
            else if constexpr (std::is_same_v<T, int64_t>) {
                return cbfshim::mod().getInt(key);
            }
            else if constexpr (std::is_same_v<T, double>) {
                return cbfshim::mod().getFloat(key);
            }
            else if constexpr (std::is_same_v<T, float>) {
                return static_cast<float>(cbfshim::mod().getFloat(key));
            }
            else if constexpr (std::is_same_v<T, std::string>) {
                return cbfshim::mod().getString(key);
            }
            else {
                static_assert(!sizeof(T), "unsupported setting type in DLL build");
            }
        }
        template <class T>
        T setSettingValue(std::string_view key, T const& value) {
            T old = getSettingValue<T>(key);
            if constexpr (std::is_same_v<T, bool>) {
                cbfshim::mod().setBool(key, value);
            }
            else if constexpr (std::is_same_v<T, int64_t>) {
                cbfshim::mod().setInt(key, value);
            }
            return old;
        }
        template <class T>
        T getSavedValue(std::string_view key) const {
            return cbfshim::mod().getSaved<T>(key, T());
        }
        template <class T>
        T getSavedValue(std::string_view key, T const& def) const {
            return cbfshim::mod().getSaved<T>(key, def);
        }
        template <class T>
        void setSavedValue(std::string_view key, T const& value) {
            if constexpr (std::is_same_v<T, matjson::Value>) {
                cbfshim::mod().setSavedJson(key, value);
            }
            else {
                cbfshim::mod().setSaved<T>(key, value);
            }
        }
        bool hasSavedValue(std::string_view key) const {
            return cbfshim::mod().hasSaved(key);
        }
        std::filesystem::path getSaveDir() const {
            return cbfshim::mod().getSaveDir();
        }
    };
    ModShim& modShim() {
        static ModShim shim;
        return shim;
    }
}
#define Mod_CBF_SHIM
namespace {
    inline ModShim* cbfModGet() {
        return &modShim();
    }
}
namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr size_t PANEL_LINES = 8;
    constexpr int SPIDER_LAND = 12;
    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }
    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);
    constexpr uint64_t HAZARD_TYPES = typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);
    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);
    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);
    constexpr uint64_t SIZE_TYPES = typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);
    bool hasType(uint64_t mask, GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 && ((mask >> type) & 1);
    }
    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;
            case 201: return 0.9f;
            case 202: return 1.1f;
            case 203: return 1.3f;
            case 1334: return 1.6f;
            default: return 0.f;
        }
    }
    struct Stage {
        bool enabled;
        int ticksAll;
        int ticksWave;
        bool custom;
    };
    constexpr Stage STAGES[] = {
        { true, 2, 2, false },
        { true, 4, 4, false },
        { true, 8, 8, false },
        { true, 16, 8, false },
        { true, 24, 12, false },
        { true, 32, 16, false },
        { true, 0, 0, true },
        { false, 0, 0, false },
    };
    constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);
    struct QueuedInput {
        int ticksLeft;
        bool down;
        bool resume = false;
        int from = 0;
        int elapsed = 0;
        int resumes = 0;
    };
    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
        bool animated = false;
    };
    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        unsigned ghostLayerBase = 0;
        CCNode* panel = nullptr;
        CCLabelBMFont* panelTitle = nullptr;
        std::vector<CCLabelBMFont*> panelLines;
        bool panelOpen = false;
        bool panelDirty = false;
        bool perfMode = false;
        double userBudget = 6.0;
        bool autoBudget = true;
        std::unordered_set<GameObject*> simDisabled;
        std::deque<std::string> logLines;
        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool portalPredict = true;
        bool ghostSwitched = false;
        bool lastRunSwitched = false;
        bool ghostSolo = false;
        bool simSecond = false;
        bool noHitch = true;
        std::vector<GameObject*> simEnabled;
        bool nextClick = true;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;
        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;
        std::deque<QueuedInput> queue;
        int shift = 0;
        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;
        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;
        bool practiceOnly = true;
        int groundHorizon = 130;
        int releaseShift = 0;
        int waveHorizon = 65;
        bool assumeOrbs = true;
        bool held1 = false, held2 = false;
        bool auto1 = false, auto2 = false;
        bool orbTiming = true;
        int orbClickDelay = 0;
        int autoAt1 = -1, autoAt2 = -1;
        bool orbWasTiming = false;
        bool tryBothOrbs = true;
        bool orbClick = true;
        bool touchedOrb = false;
        bool skipPress = false;
        bool avoidMovers = true;
        bool sawMover = false;
        bool trackMovers = false;
        bool autoRelease = false;
        int autoWindow = 6;
        bool autoReleased = false;
        bool autoClick = false;
        bool autoClicked = false;
        int autoClickTick = 0;
        PlayLayer* owner = nullptr;
        int spamGap = 16;
        int spamStreak = 3;
        bool waveMargin = true;
        bool waveV46 = false;
        bool orbAlign = true;
        bool orbAlignGround = false;
        bool ignoreTouching = true;
        bool haveRealRect = false;
        bool haveStartRects = false;
        CCRect realRect1, realRect2;
        std::unordered_set<GameObject*> harmless;
        int baseEscape = -1;
        int baseCrash = -1;
        bool lowLatency = false;
        bool explain = false;
        bool deathLog = false;
        bool captureKiller = false;
        bool baseTouchedDown = false;
        bool killerSeen = false;
        int killerTick = -1;
        int killerId = 0, killerType = -1;
        bool killerNoTouch = false, killerDisabled = false;
        CCRect killerRect, ghostDeathRect;
        bool robustSaves = true;
        float ghostShiftX = 0.f;
        int lateCrash = 90;
        bool predictMovers = false;
        bool predictTriggers = true;
        int triggersAhead = 0;
        bool shipBuffer = false;
        int shipSpamGap = 6;
        int shipMaxDelay = 2;
        bool perMode = false;
        std::array<int, 8> modeTicks { 40, 4, 40, 40, 20, 40, 40, 40 };
        bool perfLog = false;
        int simRuns = 0;
        long simTicks = 0;
        bool cbfSync = true;
        float firstStep = 1.f;
        bool inRealUpdate = false;
        float tickStartX = 0.f;
        float tickDx = 0.f;
        bool dashAutoRelease = false;
        bool autoDash = false;
        std::unordered_set<GameObject*> orbsUsed;
        bool moreTime = true;
        bool ballSmart = true;
        bool swingSmart = true;
        bool train = false;
        bool perLevel = true;
        bool heatmap = true;
        bool saveDing = true;
        CCLabelBMFont* hintLabel = nullptr;
        CCDrawNode* heatNode = nullptr;
        bool ufoSmart = true;
        bool spiderSmart = true;
        int retapAt = -1;
        bool robotSmart = true;
        bool robotAutoRelease = false;
        bool autoNormalRuns = false;
        CCLabelBMFont* autoTag = nullptr;
        CCLabelBMFont* watermark = nullptr;
        bool usedThisAttempt = false;
        bool learn = true;
        bool fullState = true;
        int orbWindow = 16;
        int simTick = 0;
        int firstOrbTick = -1;
        int lastOrbTick = -1;
        bool orbAfterFirst = false;
        bool laterOrbTouched = false;
        int orbUsedTick = -1;
        bool driftCheck = false;
        std::vector<CCPoint>* recordPath = nullptr;
        bool showPath = false;
        CCDrawNode* pathNode = nullptr;
        int pathHoldUntil = 0;
        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;
    std::string stageText();
    void forgetMoveTriggers();
    float stepScale() {
        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.f)) {
            return 1.f;
        }
        return std::clamp(TICK / dt, 0.25f, 8.f);
    }
    int st(int ticks240) {
        if (ticks240 <= 0) {
            return ticks240;
        }
        return std::max(1, static_cast<int>(std::lround(ticks240 * stepScale())));
    }
    struct Watched {
        double x;
        double y;
        int movedTick;
        double vx = 0.0;
        double vy = 0.0;
        double pvx = 0.0;
        double pvy = 0.0;
        double rot = 0.0;
        double vrot = 0.0;
    };
    constexpr int MOVER_TICKS = 120;
    constexpr size_t WATCH_LIMIT = 4096;
    std::unordered_map<GameObject*, Watched> watched;
    struct Drift {
        std::vector<CCPoint> path;
        bool active = false;
        int index = -1;
        int compared = 0;
        float maxErr = 0.f;
        int maxAt = 0;
    } drift;
    constexpr float PANEL_W = 320.f;
    constexpr float PANEL_H = 178.f;
    ccColor3B lineColor(std::string const& line) {
        auto starts = [&](char const* prefix) { return line.rfind(prefix, 0) == 0; };
        if (starts("SAVED") || starts("MORE TIME")) return { 120, 255, 140 };
        if (starts("ORB") || starts("orb")) return { 90, 220, 255 };
        if (starts("AUTO")) return { 200, 150, 255 };
        if (starts("no save")) return { 175, 175, 190 };
        if (starts("skip") || starts("prediction budget")) return { 255, 180, 80 };
        if (starts("drift")) return { 255, 240, 150 };
        if (starts("ghost killed") || starts("  hitbox")) return { 255, 120, 120 };
        if (starts("perf")) return { 150, 200, 255 };
        if (starts("Stage") || starts("v0") || starts("path") || starts("Click") || starts("CBF")) return { 255, 215, 90 };
        if (starts("pass")) return { 140, 150, 170 };
        if (starts("memory")) return { 255, 150, 220 };
        if (starts("console")) return { 150, 255, 200 };
        return { 255, 255, 255 };
    }
    void refreshPanel() {
        if (!buf.panel) {
            return;
        }
        buf.panel->setVisible(buf.panelOpen);
        if (!buf.panelOpen) {
            return;
        }
        buf.panelTitle->setString(stageText().c_str());
        buf.panelTitle->limitLabelWidth(PANEL_W - 20.f, 0.4f, 0.2f);
        size_t first = buf.logLines.size() > buf.panelLines.size() ? buf.logLines.size() - buf.panelLines.size() : 0;
        for (size_t i = 0; i < buf.panelLines.size(); ++i) {
            auto* label = buf.panelLines[i];
            size_t at = first + i;
            if (at < buf.logLines.size()) {
                auto const& line = buf.logLines[at];
                label->setString(line.c_str());
                label->setColor(lineColor(line));
                label->limitLabelWidth(PANEL_W - 20.f, 0.55f, 0.3f);
            }
            else {
                label->setString("");
            }
        }
    }
    constexpr size_t EXPORT_LINES = 400;
    std::deque<std::string> exportLines;
    int logClock = 0;
    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        auto msg = fmt::format(f, std::forward<Args>(args)...);
        exportLines.push_back(fmt::format("[{:>6}] {}", logClock, msg));
        while (exportLines.size() > EXPORT_LINES) {
            exportLines.pop_front();
        }
        buf.logLines.push_back(msg);
        while (buf.logLines.size() > PANEL_LINES) {
            buf.logLines.pop_front();
        }
        if (buf.console) {
            std::printf("[InputBuffer] %s\n", msg.c_str());
        }
        buf.panelDirty = buf.panelOpen;
    }
    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }
    bool ghostTakePortal(PlayerObject* g, GameObjectType type) {
        switch (type) {
            case GameObjectType::CubePortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::SpiderPortal:
            case GameObjectType::SwingPortal:
                break;
            default:
                return false;
        }
        bool already = (type == GameObjectType::ShipPortal && g->m_isShip) || (type == GameObjectType::UfoPortal && g->m_isBird) ||
            (type == GameObjectType::BallPortal && g->m_isBall) || (type == GameObjectType::WavePortal && g->m_isDart) ||
            (type == GameObjectType::RobotPortal && g->m_isRobot) || (type == GameObjectType::SpiderPortal && g->m_isSpider) ||
            (type == GameObjectType::SwingPortal && g->m_isSwing) ||
            (type == GameObjectType::CubePortal && !g->m_isShip && !g->m_isBird && !g->m_isBall && !g->m_isDart && !g->m_isRobot &&
                !g->m_isSpider && !g->m_isSwing);
        if (already) {
            return true;
        }
        if (g->m_isShip) g->toggleFlyMode(false, true);
        if (g->m_isBird) g->toggleBirdMode(false, true);
        if (g->m_isBall) g->toggleRollMode(false, true);
        if (g->m_isDart) g->toggleDartMode(false, true);
        if (g->m_isRobot) g->toggleRobotMode(false, true);
        if (g->m_isSpider) g->toggleSpiderMode(false, true);
        if (g->m_isSwing) g->toggleSwingMode(false, true);
        switch (type) {
            case GameObjectType::ShipPortal: g->toggleFlyMode(true, true); break;
            case GameObjectType::UfoPortal: g->toggleBirdMode(true, true); break;
            case GameObjectType::BallPortal: g->toggleRollMode(true, true); break;
            case GameObjectType::WavePortal: g->toggleDartMode(true, true); break;
            case GameObjectType::RobotPortal: g->toggleRobotMode(true, true); break;
            case GameObjectType::SpiderPortal: g->toggleSpiderMode(true, true); break;
            case GameObjectType::SwingPortal: g->toggleSwingMode(true, true); break;
            default: break;
        }
        buf.ghostSwitched = true;
        return true;
    }
    bool ghostTeleport(PlayerObject* g, GameObject* object) {
        if (object->m_objectType != GameObjectType::TeleportPortal) {
            return false;
        }
        if (object->m_objectID != 747) {
            return object->m_objectID == 749;
        }
        auto* t = static_cast<TeleportPortalObject*>(object);
        if (t->m_staticForceEnabled || t->m_redirectForceEnabled || t->m_gravityMode != 0 || t->m_redirectDash) {
            return false;
        }
        double dx = 0.0, dy = t->m_teleportYOffset;
        if (t->m_orangePortal && t->m_orangePortal != t) {
            dx = t->m_orangePortal->m_positionX - t->m_positionX;
            dy = t->m_orangePortal->m_positionY - t->m_positionY;
        }
        if (t->m_ignoreX) {
            dx = 0.0;
        }
        if (t->m_ignoreY) {
            dy = 0.0;
        }
        if (dx == 0.0 && dy == 0.0) {
            return true;
        }
        CCPoint shift { static_cast<float>(dx), static_cast<float>(dy) };
        CCPoint pos = g->getPosition() + shift;
        g->setPosition(pos);
        g->m_position = pos;
        g->m_lastPosition = g->m_lastPosition + shift;
        g->m_isOnGround = false;
        return true;
    }
    bool ghostSoloPortal(GameObject* object) {
        if (object->m_objectType != GameObjectType::SoloPortal) {
            return false;
        }
        buf.ghostSolo = true;
        buf.simSecond = false;
        return true;
    }
    void copyState(PlayerObject* ghost, PlayerObject* real);
    void pressGhost(PlayerObject* ghost, bool down);
    bool ghostDualPortal(PlayerObject* g, GameObject* object) {
        if (object->m_objectType != GameObjectType::DualPortal) {
            return false;
        }
        if (buf.simSecond) {
            return true;
        }
        if (g != buf.ghost1 || !buf.ghost2) {
            return false;
        }
        auto* g2 = buf.ghost2;
        float shift = buf.ghostShiftX;
        buf.ghostShiftX = 0.f;
        copyState(g2, g);
        buf.ghostShiftX = shift;
        g2->setPosition(g->getPosition());
        g2->m_position = g->m_position;
        g2->m_lastPosition = g->m_lastPosition;
        g2->flipGravity(!g->m_isUpsideDown, true);
        pressGhost(g2, buf.held1);
        buf.simSecond = true;
        buf.ghostSolo = false;
        return true;
    }
    void resetPending() {
        buf.queue.clear();
        buf.shift = 0;
        buf.releaseShift = 0;
    }
    void readModeTicks();
    void readSettings() {
        auto* mod = cbfModGet();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.portalPredict = mod->getSettingValue<bool>("predict-through-portals");
        buf.budgetMs = mod->getSettingValue<double>("sim-budget-ms");
        buf.autoBudget = mod->getSettingValue<bool>("auto-budget");
        buf.practiceOnly = mod->getSettingValue<bool>("practice-only");
        buf.groundHorizon = static_cast<int>(mod->getSettingValue<int64_t>("ground-horizon"));
        buf.waveHorizon = static_cast<int>(mod->getSettingValue<int64_t>("wave-horizon"));
        buf.assumeOrbs = mod->getSettingValue<bool>("orb-assume-hit");
        buf.orbClick = buf.assumeOrbs;
        buf.tryBothOrbs = mod->getSettingValue<bool>("orb-try-both");
        buf.avoidMovers = mod->getSettingValue<bool>("avoid-moving-objects");
        buf.autoRelease = mod->getSettingValue<bool>("wave-auto-release");
        buf.autoClick = mod->getSettingValue<bool>("auto-click");
        buf.autoWindow = static_cast<int>(mod->getSettingValue<int64_t>("auto-release-window"));
        buf.driftCheck = mod->getSettingValue<bool>("drift-check");
        buf.spamGap = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-gap"));
        buf.spamStreak = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-streak"));
        buf.orbAlign = mod->getSettingValue<bool>("orb-align");
        buf.orbTiming = mod->getSettingValue<bool>("orb-timing");
        buf.orbAlignGround = mod->getSettingValue<bool>("orb-align-ground");
        buf.fullState = mod->getSettingValue<bool>("full-ghost-state");
        buf.learn = mod->getSettingValue<bool>("level-memory");
        buf.robotSmart = mod->getSettingValue<bool>("robot-smart");
        buf.spiderSmart = mod->getSettingValue<bool>("spider-smart");
        buf.ufoSmart = mod->getSettingValue<bool>("ufo-smart");
        buf.ballSmart = mod->getSettingValue<bool>("ball-smart");
        buf.swingSmart = mod->getSettingValue<bool>("swing-smart");
        buf.train = mod->getSettingValue<bool>("training-mode");
        buf.perLevel = mod->getSettingValue<bool>("per-level-settings");
        buf.heatmap = mod->getSettingValue<bool>("death-heatmap");
        buf.saveDing = mod->getSettingValue<bool>("save-ding");
        buf.moreTime = mod->getSettingValue<bool>("more-time-death-spots");
        buf.dashAutoRelease = mod->getSettingValue<bool>("dash-auto-release");
        buf.perfLog = mod->getSettingValue<bool>("perf-log");
        buf.shipBuffer = mod->getSettingValue<bool>("ship-buffer");
        buf.lateCrash = static_cast<int>(mod->getSettingValue<int64_t>("cube-late-crash"));
        buf.robustSaves = mod->getSettingValue<bool>("robust-saves");
        buf.ignoreTouching = mod->getSettingValue<bool>("ignore-touching-hazards");
        buf.deathLog = mod->getSettingValue<bool>("death-log");
        buf.explain = mod->getSettingValue<bool>("explain-inputs");
        buf.predictMovers = mod->getSettingValue<bool>("predict-moving-objects");
        buf.predictTriggers = mod->getSettingValue<bool>("predict-move-triggers");
        buf.noHitch = mod->getSettingValue<bool>("no-hitch-searches");
        buf.nextClick = mod->getSettingValue<bool>("plan-next-click");
        buf.shipMaxDelay = static_cast<int>(mod->getSettingValue<int64_t>("ship-max-delay"));
        readModeTicks();
        buf.shipSpamGap = static_cast<int>(mod->getSettingValue<int64_t>("ship-spam-gap"));
        buf.cbfSync = mod->getSettingValue<bool>("cbf-sync");
        buf.robotAutoRelease = mod->getSettingValue<bool>("robot-auto-release");
        buf.autoNormalRuns = mod->getSettingValue<bool>("auto-release-normal-runs");
        buf.waveV46 = mod->getSettingValue<bool>("wave-v46");
        buf.waveMargin = mod->getSettingValue<bool>("wave-safety-margin");
        buf.orbWindow = static_cast<int>(mod->getSettingValue<int64_t>("orb-align-ticks"));
        buf.lowLatency = mod->getSettingValue<bool>("low-latency");
        if (buf.lowLatency) {
            buf.budgetMs = std::min(buf.budgetMs, 3.0);
            buf.robustSaves = false;
            buf.waveMargin = false;
        }
        buf.perfMode = mod->getSettingValue<bool>("performance-mode");
        if (buf.perfMode) {
            buf.budgetMs = std::min(buf.budgetMs, 4.0);
            buf.robustSaves = false;
            buf.orbTiming = false;
            buf.driftCheck = false;
        }
        buf.userBudget = buf.budgetMs;
    }
    void clickBudget() {
        double budget = buf.userBudget;
        if (buf.autoBudget) {
            auto* director = CCDirector::get();
            double interval = director->getAnimationInterval() * 1000.0;
            double frame = director->getDeltaTime() * 1000.0;
            if (interval > 0.0) {
                budget = std::min(budget, std::max(4.0, interval));
            }
            if (interval > 0.0 && frame > std::max(interval * 2.0, 20.0)) {
                budget = std::min(budget, 4.0);
            }
        }
        buf.budgetMs = budget;
    }
    constexpr char const* MODE_NAMES[] = { "Cube", "Ship", "Ball", "UFO", "Wave", "Robot", "Spider", "Swing" };
    constexpr int MODE_TICKS_MAX = 40;
    void readModeTicks() {
        auto* mod = cbfModGet();
        buf.perMode = mod->getSavedValue<bool>("per-mode-frames", false);
        int all = static_cast<int>(mod->getSettingValue<int64_t>("custom-ticks"));
        int wave = static_cast<int>(mod->getSettingValue<int64_t>("custom-wave-ticks"));
        int ship = static_cast<int>(mod->getSettingValue<int64_t>("ship-max-delay"));
        for (int i = 0; i < 8; ++i) {
            int fallback = i == 4 ? wave : i == 1 ? ship : all;
            buf.modeTicks[i] = std::clamp(mod->getSavedValue<int>(fmt::format("mode-ticks-{}", i), fallback), 0, MODE_TICKS_MAX);
        }
    }
    void saveModeTicks() {
        auto* mod = cbfModGet();
        mod->setSavedValue("per-mode-frames", buf.perMode);
        for (int i = 0; i < 8; ++i) {
            mod->setSavedValue(fmt::format("mode-ticks-{}", i), buf.modeTicks[i]);
        }
    }
    int modeIndex(PlayerObject* p1, PlayerObject* p2) {
        auto of = [](PlayerObject* p) {
            return p->m_isDart ? 4 : p->m_isShip ? 1 : p->m_isBall ? 2 : p->m_isBird ? 3 : p->m_isRobot ? 5 : p->m_isSpider ? 6
                : p->m_isSwing ? 7 : 0;
        };
        int m = of(p1);
        if (p2 && of(p2) == 4) {
            m = 4;
        }
        return m;
    }
    void applyStage() {
        auto const& s = STAGES[buf.stage];
        buf.enabled = s.enabled;
        if (s.custom) {
            buf.ticksAll = static_cast<int>(cbfModGet()->getSettingValue<int64_t>("custom-ticks"));
            buf.ticksWave = static_cast<int>(cbfModGet()->getSettingValue<int64_t>("custom-wave-ticks"));
        }
        else {
            buf.ticksAll = s.ticksAll;
            buf.ticksWave = s.ticksWave;
        }
    }
    void refreshWatermark() {
        if (!buf.watermark) {
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        buf.watermark->setVisible(buf.enabled && !(layer && layer->m_isPracticeMode));
        buf.watermark->setString(fmt::format("Input Buffer  |  {}", stageText()).c_str());
    }
    void refreshAutoTag() {
        if (!buf.autoTag) {
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        bool on = buf.enabled && buf.autoNormalRuns && (buf.autoRelease || buf.robotAutoRelease || buf.dashAutoRelease || buf.autoClick) &&
            layer && !layer->m_isPracticeMode;
        buf.autoTag->setVisible(on);
    }
    size_t memorySpots();
    void exportConsole() {
        auto* layer = GameManager::get()->m_playLayer;
        std::string text;
        text += fmt::format("Input Buffer {} console export\n", std::string("v0.2"));
        if (layer && layer->m_level) {
            text += fmt::format("Level: {} (id {}){}\n", std::string(layer->m_level->m_levelName), layer->m_level->m_levelID.value(),
                layer->m_isPracticeMode ? ", practice" : "");
        }
        float dt = buf.tickLength > 0.001f ? buf.tickLength : TICK;
        text += fmt::format("{} | physics ~{:.0f} steps/s\n", stageText(), 60.f / dt);
        text += fmt::format("budget {:g} ms | smart {} | full state {} | robust {} | late crash {} | practice only {}\n", buf.budgetMs,
            buf.smart, buf.fullState, buf.robustSaves, buf.lateCrash, buf.practiceOnly);
        text += fmt::format("orbs: align {} (ground {}, {}t) timing {} assume {} both {} | memory {} ({} spots)\n", buf.orbAlign,
            buf.orbAlignGround, buf.orbWindow, buf.orbTiming, buf.assumeOrbs, buf.tryBothOrbs, buf.learn, memorySpots());
        text += fmt::format("wave: v4.6 {} margin {} horizon {} spam gap {} | robot smart {} | spider smart {} | ship {} (max {}t)\n",
            buf.waveV46, buf.waveMargin, buf.waveHorizon, buf.spamGap, buf.robotSmart, buf.spiderSmart, buf.shipBuffer, buf.shipMaxDelay);
        text += fmt::format("auto-release: wave {} robot {} dash {} | auto-click {} | normal runs {} window {} | movers: avoid {} predict {}\n",
            buf.autoRelease, buf.robotAutoRelease, buf.dashAutoRelease, buf.autoClick, buf.autoNormalRuns, buf.autoWindow, buf.avoidMovers,
            buf.predictMovers);
        text += fmt::format("debug: drift check {} | path view {} | perf log {} | death log {} | ignore touching {}\n", buf.driftCheck,
            buf.showPath, buf.perfLog, buf.deathLog, buf.ignoreTouching);
        text += fmt::format("explain inputs {} | low latency {} | cbf sync {}{}\n", buf.explain, buf.lowLatency, buf.cbfSync,
            Loader::get()->isModLoaded("syzzi.click_between_frames") ? " (CBF loaded)" : "");
        if (Loader::get()->isModLoaded("square3ang.cbfextrapolate")) {
            text += "CBF Extrapolate loaded (visual only)\n";
        }
        text += "----\n";
        for (auto const& line : exportLines) {
            text += line;
            text += '\n';
        }
        bool copied = geode::utils::clipboard::write(text);
        std::error_code ec;
        std::filesystem::path saveDir = geode::dirs::getModsSaveDir() / "input-buffer";
        std::filesystem::create_directories(saveDir, ec);
        std::ofstream(saveDir / "console-export.txt", std::ios::trunc) << text;
        say("console {} ({} lines) + saved to console-export.txt", copied ? "copied to clipboard" : "NOT copied", exportLines.size());
    }
    std::string stageText() {
        if (!buf.enabled) {
            return "Buffer: OFF";
        }
        if (buf.perMode) {
            return fmt::format("Buffer: per mode{}", buf.train ? " [training]" : "");
        }
        return fmt::format("Buffer{}: {:g}f / wave {:g}f{}", STAGES[buf.stage].custom ? " (custom)" : "", buf.ticksAll * TICK,
            buf.ticksWave * TICK, buf.train ? " [training]" : "");
    }
    void dropGhosts() {
#ifdef GEODE_IS_ANDROID
        buf.ghost1 = nullptr;
        buf.ghost2 = nullptr;
        buf.ghostLayer = nullptr;
#else
        for (auto** g : { &buf.ghost1, &buf.ghost2 }) {
            if (*g) {
                (*g)->removeFromParentAndCleanup(true);
                (*g)->release();
                *g = nullptr;
            }
        }
        if (buf.ghostLayer) {
            buf.ghostLayer->removeAllChildrenWithCleanup(true);
            buf.ghostLayer->release();
            buf.ghostLayer = nullptr;
        }
#endif
        buf.panel = nullptr;
        buf.panelTitle = nullptr;
        buf.panelLines.clear();
        buf.pathNode = nullptr;
        buf.autoTag = nullptr;
        buf.watermark = nullptr;
        buf.simulating = false;
        buf.injecting = false;
        buf.rapid = 0;
        buf.autoReleased = false;
        buf.autoClicked = false;
        buf.recordPath = nullptr;
        watched.clear();
        forgetMoveTriggers();
        drift.active = false;
        resetPending();
        buf.realInput = false;
        buf.appliedInput = false;
        buf.owner = nullptr;
    }
    PlayerObject* makeGhost(PlayLayer* layer) {
        if (!buf.ghostLayer) {
            buf.ghostLayer = CCLayer::create();
            buf.ghostLayer->retain();
        }
        auto* ghost = PlayerObject::create(1, 1, layer, buf.ghostLayer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        buf.ghostLayer->addChild(ghost);
        return ghost;
    }
    void sweepGhostLayer() {
        if (!buf.ghostLayer) {
            return;
        }
        auto* children = buf.ghostLayer->getChildren();
        while (children && children->count() > buf.ghostLayerBase) {
            auto* node = static_cast<CCNode*>(children->lastObject());
            buf.ghostLayer->removeChild(node, true);
        }
    }
    bool gameHolds(PlayerObject* player) {
        if (!player) {
            return false;
        }
        auto it = player->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        return it != player->m_holdingButtons.end() && it->second;
    }
    void syncHeld(PlayLayer* layer) {
        resetPending();
        buf.realInput = buf.appliedInput = gameHolds(layer->m_player1);
    }
    void sendButton(PlayLayer* layer, bool down) {
        buf.injecting = true;
        buf.appliedInput = down;
        layer->handleButton(down, 1, buf.isPlayer1);
        buf.injecting = false;
    }
    void pressGhost(PlayerObject* ghost, bool down) {
        (ghost == buf.ghost1 ? buf.held1 : buf.held2) = down;
        if (down) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }
    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;
        if (buf.ghostShiftX != 0.f) {
            CCPoint pos = real->getPosition();
            pos.x += buf.ghostShiftX;
            ghost->setPosition(pos);
            ghost->m_position = pos;
        }
        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;
        if (!buf.fullState) {
            return;
        }
        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_isSideways = real->m_isSideways;
        ghost->m_reverseSync = real->m_reverseSync;
        ghost->m_reverseRelated = real->m_reverseRelated;
        ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed;
        ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration;
        ghost->m_speedMultiplier = real->m_speedMultiplier;
        ghost->m_gravity = real->m_gravity;
        ghost->m_yStart = real->m_yStart;
        ghost->m_fixGravityBug = real->m_fixGravityBug;
        ghost->m_fixRobotJump = real->m_fixRobotJump;
        ghost->m_enable22Changes = real->m_enable22Changes;
        ghost->m_enableImpulseFix = real->m_enableImpulseFix;
        ghost->m_totalTime = real->m_totalTime;
        ghost->m_lastPosition = real->m_lastPosition;
        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        ghost->m_yVelocityRelated = real->m_yVelocityRelated;
        ghost->m_yVelocityRelated3 = real->m_yVelocityRelated3;
        ghost->m_groundYVelocity = real->m_groundYVelocity;
        ghost->m_accelerationOrSpeed = real->m_accelerationOrSpeed;
        ghost->m_isAccelerating = real->m_isAccelerating;
        ghost->m_maybeIsFalling = real->m_maybeIsFalling;
        ghost->m_isOnGround2 = real->m_isOnGround2;
        ghost->m_isOnGround3 = real->m_isOnGround3;
        ghost->m_isOnGround4 = real->m_isOnGround4;
        ghost->m_stateOnGround = real->m_stateOnGround;
        ghost->m_lastLandTime = real->m_lastLandTime;
        ghost->m_lastJumpTime = real->m_lastJumpTime;
        ghost->m_lastFlipTime = real->m_lastFlipTime;
        ghost->m_lastSpiderFlipTime = real->m_lastSpiderFlipTime;
        ghost->m_lastGroundedPos = real->m_lastGroundedPos;
        ghost->m_blackOrbRelated = real->m_blackOrbRelated;
        ghost->m_padRingRelated = real->m_padRingRelated;
        ghost->m_ringJumpRelated = real->m_ringJumpRelated;
        ghost->m_isOnSlope = real->m_isOnSlope;
        ghost->m_wasOnSlope = real->m_wasOnSlope;
        ghost->m_slopeVelocity = real->m_slopeVelocity;
        ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope;
        ghost->m_currentSlope = real->m_currentSlope;
        ghost->m_currentSlope2 = real->m_currentSlope2;
        ghost->m_currentPotentialSlope = real->m_currentPotentialSlope;
        ghost->m_isCollidingWithSlope = real->m_isCollidingWithSlope;
        ghost->m_collidingWithSlopeId = real->m_collidingWithSlopeId;
        ghost->m_slopeFlipGravityRelated = real->m_slopeFlipGravityRelated;
        ghost->m_slopeAngle = real->m_slopeAngle;
        ghost->m_slopeAngleRadians = real->m_slopeAngleRadians;
        ghost->m_slopeRotation = real->m_slopeRotation;
        ghost->m_currentSlopeYVelocity = real->m_currentSlopeYVelocity;
        ghost->m_isCurrentSlopeTop = real->m_isCurrentSlopeTop;
        ghost->m_slopeStartTime = real->m_slopeStartTime;
        ghost->m_slopeEndTime = real->m_slopeEndTime;
        ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated;
        ghost->m_stateDartSlide = real->m_stateDartSlide;
        ghost->m_lastGroundObject = real->m_lastGroundObject;
        ghost->m_preLastGroundObject = real->m_preLastGroundObject;
        ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
        ghost->m_collidedObject = real->m_collidedObject;
        ghost->m_objectSnappedTo = real->m_objectSnappedTo;
        ghost->m_snapDistance = real->m_snapDistance;
        ghost->m_lastCollisionBottom = real->m_lastCollisionBottom;
        ghost->m_lastCollisionTop = real->m_lastCollisionTop;
        ghost->m_lastCollisionLeft = real->m_lastCollisionLeft;
        ghost->m_lastCollisionRight = real->m_lastCollisionRight;
        ghost->m_collidedTopMinY = real->m_collidedTopMinY;
        ghost->m_collidedBottomMaxY = real->m_collidedBottomMaxY;
        ghost->m_collidedLeftMaxX = real->m_collidedLeftMaxX;
        ghost->m_collidedRightMinX = real->m_collidedRightMinX;
        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashStartTime = real->m_dashStartTime;
        ghost->m_dashRing = real->m_dashRing;
        ghost->m_dashFireFrame = real->m_dashFireFrame;
        ghost->m_stateForce = real->m_stateForce;
        ghost->m_stateForceVector = real->m_stateForceVector;
        ghost->m_affectedByForces = real->m_affectedByForces;
        ghost->m_stateBoostX = real->m_stateBoostX;
        ghost->m_stateBoostY = real->m_stateBoostY;
        ghost->m_stateNoStickX = real->m_stateNoStickX;
        ghost->m_stateNoStickY = real->m_stateNoStickY;
        ghost->m_stateHitHead = real->m_stateHitHead;
        ghost->m_stateFlipGravity = real->m_stateFlipGravity;
        ghost->m_stateNoAutoJump = real->m_stateNoAutoJump;
        ghost->m_stateScale = real->m_stateScale;
        ghost->m_maybeStateForce2 = real->m_maybeStateForce2;
    }
    constexpr int HOLD_SAMPLES = 8;
    struct HoldStats {
        int samples[HOLD_SAMPLES] = {};
        int count = 0;
        int index = 0;
        void add(int ticks) {
            samples[index] = std::clamp(ticks, 1, 240);
            index = (index + 1) % HOLD_SAMPLES;
            count = std::min(count + 1, HOLD_SAMPLES);
        }
        int pick(int fallback, int percent) const {
            if (count == 0) {
                return fallback;
            }
            int tmp[HOLD_SAMPLES];
            std::copy(samples, samples + count, tmp);
            int at = std::min(count - 1, count * percent / 100);
            std::nth_element(tmp, tmp + at, tmp + count);
            return tmp[at];
        }
    };
    HoldStats tapHold, robotHold;
    int tickCount = 0;
    enum class InputKind { Tap, Wave, Robot, Other };
    int pressTick = -1;
    InputKind pressKind = InputKind::Other;
    InputKind kindOf(PlayerObject* p) {
        if (!p || p->m_isShip) {
            return InputKind::Other;
        }
        if (p->m_isDart) {
            return InputKind::Wave;
        }
        return p->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }
    void recordRealInput(bool down, PlayerObject* p1) {
        if (down) {
            pressTick = tickCount;
            pressKind = kindOf(p1);
            return;
        }
        if (pressTick < 0) {
            return;
        }
        int held = tickCount - pressTick;
        pressTick = -1;
        if (buf.autoReleased) {
            return;
        }
        switch (pressKind) {
            case InputKind::Tap: tapHold.add(held); break;
            case InputKind::Robot: robotHold.add(held); break;
            default: break;
        }
    }
    void forgetPress() {
        pressTick = -1;
    }
    int holdEstimate() {
        return tapHold.pick(st(12), 50);
    }
    void watchObject(GameObject* object) {
        auto [it, added] = watched.try_emplace(object, Watched { object->m_positionX, object->m_positionY, -MOVER_TICKS });
        if (added) {
            it->second.rot = object->getRotation();
        }
        if (!added && tickCount - it->second.movedTick < st(MOVER_TICKS)) {
            buf.sawMover = true;
        }
    }
    std::vector<GameObject*> movingNow;
    void updateWatched() {
        if (watched.size() > WATCH_LIMIT) {
            watched.clear();
        }
        movingNow.clear();
        double px = 0.0;
        bool prune = false;
        if (auto* layer = GameManager::get()->m_playLayer; layer && layer->m_player1) {
            px = layer->m_player1->getPosition().x;
            prune = true;
        }
        for (auto it = watched.begin(); it != watched.end();) {
            auto* object = it->first;
            auto& w = it->second;
            if (prune && std::abs(object->m_positionX - px) > 800.0) {
                it = watched.erase(it);
                continue;
            }
            double rot = object->getRotation();
            if (object->m_positionX != w.x || object->m_positionY != w.y || rot != w.rot) {
                bool consecutive = tickCount - w.movedTick == 1;
                w.pvx = consecutive ? w.vx : 0.0;
                w.pvy = consecutive ? w.vy : 0.0;
                w.vx = object->m_positionX - w.x;
                w.vy = object->m_positionY - w.y;
                double dr = rot - w.rot;
                w.vrot = std::remainder(dr, 360.0);
                w.x = object->m_positionX;
                w.y = object->m_positionY;
                w.rot = rot;
                w.movedTick = tickCount;
                movingNow.push_back(object);
            }
            else if (tickCount - w.movedTick > 1) {
                w.vx = w.vy = w.pvx = w.pvy = w.vrot = 0.0;
            }
            ++it;
        }
    }
    struct Mover {
        GameObject* object;
        double x, y, vx, vy;
        double turn = 0.0;
        double rotX = 0.0, rotY = 0.0, vrot = 0.0;
        double ax = 0.0, ay = 0.0;
        int accelSteps = 0;
    };
    std::vector<Mover> movers;
    void collectMovers() {
        movers.clear();
        if (!buf.predictMovers) {
            return;
        }
        for (auto* object : movingNow) {
            auto it = watched.find(object);
            if (it == watched.end()) {
                continue;
            }
            auto const& w = it->second;
            if ((w.vx != 0.0 || w.vy != 0.0 || w.vrot != 0.0) && tickCount - w.movedTick <= 1) {
                Mover m { object, object->m_positionX, object->m_positionY, w.vx, w.vy };
                double speed = std::hypot(w.vx, w.vy);
                double prev = std::hypot(w.pvx, w.pvy);
                if (speed > 1e-6 && prev > 1e-6) {
                    double turn = std::remainder(std::atan2(w.vy, w.vx) - std::atan2(w.pvy, w.pvx), 6.283185307179586);
                    double ratio = speed / prev;
                    if (std::abs(turn) > 1e-4 && std::abs(turn) < 0.5 && ratio > 0.8 && ratio < 1.25) {
                        m.turn = turn;
                    }
                    else if (std::abs(turn) <= 1e-4 && std::abs(ratio - 1.0) > 1e-3 && ratio > 0.5 && ratio < 2.0) {
                        m.ax = w.vx - w.pvx;
                        m.ay = w.vy - w.pvy;
                        double along = (m.ax * w.vx + m.ay * w.vy) / speed;
                        double steps = along < 0.0 ? speed / -along : 2.0 * speed / along;
                        m.accelSteps = static_cast<int>(std::min(steps, 1000.0));
                    }
                }
                m.rotX = object->getRotationX();
                m.rotY = object->getRotationY();
                m.vrot = w.vrot;
                movers.push_back(m);
            }
        }
    }
    void placeMovers(int steps) {
        for (auto const& m : movers) {
            double x = m.x + m.vx * steps;
            double y = m.y + m.vy * steps;
            if (m.accelSteps > 0 && steps > 0) {
                int n = std::min(steps, m.accelSteps);
                double grow = 0.5 * n * (n + 1);
                x = m.x + m.vx * steps + m.ax * (grow + static_cast<double>(n) * (steps - n));
                y = m.y + m.vy * steps + m.ay * (grow + static_cast<double>(n) * (steps - n));
            }
            if (m.turn != 0.0 && steps > 0) {
                std::complex<double> v { m.vx, m.vy };
                auto rho = std::polar(1.0, m.turn);
                auto moved = v * rho * (1.0 - std::pow(rho, steps)) / (1.0 - rho);
                x = m.x + moved.real();
                y = m.y + moved.imag();
            }
            if (m.vrot != 0.0) {
                double spun = steps == 0 ? 0.0 : m.vrot * steps;
                m.object->setRotationX(static_cast<float>(m.rotX + spun));
                m.object->setRotationY(static_cast<float>(m.rotY + spun));
            }
            m.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
            m.object->m_positionX = x;
            m.object->m_positionY = y;
            m.object->setObjectRectDirty(true);
            m.object->m_isOrientedBoxDirty = true;
        }
    }
    enum class TriggerKind { Move, Rotate, Toggle, Spawn };
    struct LevelTrigger {
        EffectGameObject* trigger;
        double x;
        TriggerKind kind;
    };
    std::vector<LevelTrigger> levelTriggers;
    GJBaseGameLayer* levelTriggersLayer = nullptr;
    unsigned levelTriggersCount = 0;
    std::unordered_map<EffectGameObject*, int> firedMoves;
    struct ScriptedMove {
        TriggerKind kind = TriggerKind::Move;
        double x = 0.0;
        int fireStep = -1;
        double dx = 0.0, dy = 0.0;
        double angle = 0.0;
        bool hasCenter = false;
        double cx = 0.0, cy = 0.0;
        bool lockRot = false;
        double steps = 0.0;
        int easing = 0;
        double rate = 0.0;
        double from = 0.0;
        CCArray* group = nullptr;
        bool on = false;
        bool armed = false;
        int delay = 0;
        bool toggled = false;
    };
    struct ScriptedObject {
        GameObject* object;
        double x, y;
        CCPoint node;
        float rotX, rotY;
        bool mover;
        bool placed = false;
        bool rotated = false;
        std::vector<int> moves;
        double lastX = 0.0, lastY = 0.0, lastSpin = 0.0;
    };
    std::vector<ScriptedMove> scriptedMoves;
    std::vector<ScriptedObject> scriptedObjects;
    void forgetTriggerChecks();
    constexpr int SCRIPTED_TRIGGER_LIMIT = 48;
    struct PendingSpawn {
        int group;
        int at;
    };
    std::vector<PendingSpawn> pendingSpawns;
    constexpr int SCRIPTED_OBJECT_LIMIT = 160;
    struct ScriptedCache {
        GJBaseGameLayer* layer = nullptr;
        int tick = -1;
        double px = 0.0, reach = 0.0;
        std::vector<ScriptedMove> moves;
        std::vector<ScriptedObject> objects;
    } scriptedCache;
    void forgetMoveTriggers() {
        scriptedCache = {};
        pendingSpawns.clear();
        forgetTriggerChecks();
        firedMoves.clear();
        levelTriggers.clear();
        levelTriggersLayer = nullptr;
        levelTriggersCount = 0;
    }
    double easeMove(int type, double rate, double t) {
        constexpr double PI = 3.141592653589793;
        t = std::clamp(t, 0.0, 1.0);
        if (t <= 0.0 || t >= 1.0) {
            return t;
        }
        rate = rate > 0.0 ? rate : 2.0;
        auto bounce = [](double u) {
            if (u < 1.0 / 2.75) {
                return 7.5625 * u * u;
            }
            if (u < 2.0 / 2.75) {
                u -= 1.5 / 2.75;
                return 7.5625 * u * u + 0.75;
            }
            if (u < 2.5 / 2.75) {
                u -= 2.25 / 2.75;
                return 7.5625 * u * u + 0.9375;
            }
            u -= 2.625 / 2.75;
            return 7.5625 * u * u + 0.984375;
        };
        double period = rate > 0.0 && rate < 2.0 ? rate : 0.3;
        double s = period / 4.0;
        constexpr double BACK = 1.70158;
        switch (type) {
            case 1: {
                double u = t * 2.0;
                return u < 1.0 ? 0.5 * std::pow(u, rate) : 1.0 - 0.5 * std::pow(2.0 - u, rate);
            }
            case 2: return std::pow(t, rate);
            case 3: return std::pow(t, 1.0 / rate);
            case 4: {
                double u = t * 2.0 - 1.0;
                return u < 0.0 ? -0.5 * std::pow(2.0, 10.0 * u) * std::sin((u - s) * 2.0 * PI / period)
                               : std::pow(2.0, -10.0 * u) * std::sin((u - s) * 2.0 * PI / period) * 0.5 + 1.0;
            }
            case 5: {
                double u = t - 1.0;
                return -std::pow(2.0, 10.0 * u) * std::sin((u - s) * 2.0 * PI / period);
            }
            case 6: return std::pow(2.0, -10.0 * t) * std::sin((t - s) * 2.0 * PI / period) + 1.0;
            case 7: return t < 0.5 ? (1.0 - bounce(1.0 - t * 2.0)) * 0.5 : bounce(t * 2.0 - 1.0) * 0.5 + 0.5;
            case 8: return 1.0 - bounce(1.0 - t);
            case 9: return bounce(t);
            case 10: {
                double u = t * 2.0;
                return u < 1.0 ? 0.5 * std::pow(2.0, 10.0 * (u - 1.0)) : 0.5 * (2.0 - std::pow(2.0, -10.0 * (u - 1.0)));
            }
            case 11: return std::pow(2.0, 10.0 * (t - 1.0));
            case 12: return 1.0 - std::pow(2.0, -10.0 * t);
            case 13: return -0.5 * (std::cos(PI * t) - 1.0);
            case 14: return 1.0 - std::cos(t * PI / 2.0);
            case 15: return std::sin(t * PI / 2.0);
            case 16: {
                double o = BACK * 1.525;
                double u = t * 2.0;
                if (u < 1.0) {
                    return u * u * ((o + 1.0) * u - o) / 2.0;
                }
                u -= 2.0;
                return u * u * ((o + 1.0) * u + o) / 2.0 + 1.0;
            }
            case 17: return t * t * ((BACK + 1.0) * t - BACK);
            case 18: {
                double u = t - 1.0;
                return u * u * ((BACK + 1.0) * u + BACK) + 1.0;
            }
            default: return t;
        }
    }
    bool plainTrigger(EffectGameObject* t, bool rotate) {
        if (t->m_targetGroupID <= 0 || t->m_isDynamicMode) {
            return false;
        }
        if (rotate) {
            return t->m_rotationTargetID <= 0 && (t->m_rotationDegrees != 0.f || t->m_times360 != 0);
        }
        return !t->m_useMoveTarget && !t->m_lockToPlayerX && !t->m_lockToPlayerY && !t->m_lockToCameraX && !t->m_lockToCameraY &&
            (t->m_moveOffset.x != 0.f || t->m_moveOffset.y != 0.f);
    }
    bool plainSpawn(EffectGameObject* t) {
        auto* s = static_cast<SpawnTriggerGameObject*>(t);
        return t->m_targetGroupID > 0 && s->m_delayRange <= 0.f && s->m_remapObjects.empty() && !t->m_spawnOrdered;
    }
    int spawnSteps(EffectGameObject* t, float tickLength) {
        return static_cast<int>(std::lround(std::max(0.f, static_cast<SpawnTriggerGameObject*>(t)->m_spawnDelay) * 60.0 / tickLength));
    }
    void scanMoveTriggers(GJBaseGameLayer* layer) {
        unsigned count = layer->m_objects ? layer->m_objects->count() : 0;
        if (layer == levelTriggersLayer && count == levelTriggersCount) {
            return;
        }
        levelTriggersLayer = layer;
        levelTriggersCount = count;
        levelTriggers.clear();
        if (!layer->m_objects) {
            return;
        }
        for (auto* object : CCArrayExt<GameObject*>(layer->m_objects)) {
            if (!object) {
                continue;
            }
            int id = object->m_objectID;
            if (id != 901 && id != 1346 && id != 1049 && id != 1268) {
                continue;
            }
            auto* t = static_cast<EffectGameObject*>(object);
            if (t->m_isSpawnTriggered || t->m_isTouchTriggered) {
                continue;
            }
            if (id == 1049) {
                if (t->m_targetGroupID > 0) {
                    levelTriggers.push_back({ t, t->m_positionX, TriggerKind::Toggle });
                }
                continue;
            }
            if (id == 1268) {
                if (plainSpawn(t)) {
                    levelTriggers.push_back({ t, t->m_positionX, TriggerKind::Spawn });
                }
                continue;
            }
            bool rotate = id == 1346;
            if (plainTrigger(t, rotate)) {
                levelTriggers.push_back({ t, t->m_positionX, rotate ? TriggerKind::Rotate : TriggerKind::Move });
            }
        }
        std::sort(levelTriggers.begin(), levelTriggers.end(), [](auto const& a, auto const& b) { return a.x < b.x; });
    }
    GameObject* groupParentOf(GJBaseGameLayer* layer, int group) {
        if (group <= 0 || !layer->m_parentGroupsDict) {
            return nullptr;
        }
        return typeinfo_cast<GameObject*>(layer->m_parentGroupsDict->objectForKey(group));
    }
    void fillMove(GJBaseGameLayer* layer, EffectGameObject* t, TriggerKind kind, float tickLength, ScriptedMove& m) {
        m.kind = kind;
        m.steps = t->m_duration * 60.0 / tickLength;
        m.easing = static_cast<int>(t->m_easingType);
        m.rate = t->m_easingRate;
        if (kind == TriggerKind::Move) {
            m.dx = t->m_moveOffset.x;
            m.dy = t->m_moveOffset.y;
            return;
        }
        m.angle = t->m_rotationDegrees + 360.0 * t->m_times360;
        m.lockRot = t->m_lockObjectRotation;
        GameObject* pivot = nullptr;
        if (t->m_centerGroupID > 0) {
            pivot = groupParentOf(layer, t->m_centerGroupID);
            if (!pivot) {
                auto* center = layer->getGroup(t->m_centerGroupID);
                if (center && center->count() > 0) {
                    pivot = static_cast<GameObject*>(center->objectAtIndex(0));
                }
            }
        }
        else {
            pivot = groupParentOf(layer, t->m_targetGroupID);
        }
        if (pivot) {
            m.hasCenter = true;
            m.cx = pivot->m_positionX;
            m.cy = pivot->m_positionY;
        }
    }
    void applyMove(ScriptedMove const& m, double f, double& x, double& y, double& spin) {
        if (m.kind == TriggerKind::Move) {
            x += m.dx * f;
            y += m.dy * f;
            return;
        }
        if (m.kind != TriggerKind::Rotate) {
            return;
        }
        double a = m.angle * f;
        if (m.hasCenter) {
            double th = -a * 3.141592653589793 / 180.0;
            double rx = x - m.cx, ry = y - m.cy;
            x = m.cx + rx * std::cos(th) - ry * std::sin(th);
            y = m.cy + rx * std::sin(th) + ry * std::cos(th);
        }
        if (!m.lockRot) {
            spin += a;
        }
    }
    struct TriggerCheck {
        GameObject* object;
        int fired;
        int at;
        double x, y, rot;
    };
    std::vector<TriggerCheck> triggerChecks;
    int checksGood = 0, checksBad = 0, checksUnclear = 0;
    bool triggersDistrusted = false;
    int attemptStartTick = 0;
    struct TriggerFire {
        EffectGameObject* trigger;
        int tick;
        ScriptedMove move;
    };
    std::vector<TriggerFire> triggerFires;
    void noteTriggerFire(GJBaseGameLayer* layer, EffectGameObject* t) {
        if (t->m_targetGroupID <= 0) {
            return;
        }
        if (triggerFires.size() > 1024) {
            triggerFires.erase(triggerFires.begin(), triggerFires.begin() + 512);
        }
        ScriptedMove m;
        int id = t->m_objectID;
        if (layer && (id == 901 || id == 1346)) {
            float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
            fillMove(layer, t, id == 1346 ? TriggerKind::Rotate : TriggerKind::Move, tickLength, m);
        }
        triggerFires.push_back({ t, tickCount, m });
    }
    bool unmodeledMover(int id) {
        switch (id) {
            case 1347:
            case 1814:
            case 2067:
            case 3016:
            case 3033:
            case 3006: case 3007: case 3008: case 3009: case 3010:
                return true;
            default:
                return false;
        }
    }
    bool inGroups(GameObject* object, int group) {
        auto* groups = object->m_groups;
        int count = std::clamp<int>(object->m_groupCount, 0, 10);
        if (!groups || group <= 0) {
            return false;
        }
        for (int i = 0; i < count; ++i) {
            if ((*groups)[i] == group) {
                return true;
            }
        }
        return false;
    }
    void startTriggerCheck(GJBaseGameLayer* layer, EffectGameObject* t) {
        if (!buf.predictTriggers || triggerChecks.size() >= 16 || !layer || !layer->m_player1 || tickCount - attemptStartTick < st(60)) {
            return;
        }
        bool rotate = t->m_objectID == 1346;
        if (!plainTrigger(t, rotate) || t->m_duration * 60.f < TICK) {
            return;
        }
        auto* group = layer->getGroup(t->m_targetGroupID);
        if (!group) {
            return;
        }
        float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
        double px = layer->m_player1->getPositionX();
        for (auto* object : CCArrayExt<GameObject*>(group)) {
            if (!object || object == t || object->m_isDecoration || object->m_objectType == GameObjectType::Decoration ||
                object->m_positionX < px - 150.0 || object->m_positionX > px + 700.0) {
                continue;
            }
            for (auto const& c : triggerChecks) {
                if (c.object == object) {
                    return;
                }
            }
            int span = std::min(static_cast<int>(std::lround(t->m_duration * 60.0 / tickLength)), st(120));
            triggerChecks.push_back({ object, tickCount, tickCount + std::max(span, 1), object->m_positionX, object->m_positionY,
                static_cast<double>(object->getRotationX()) });
            return;
        }
    }
    void forgetTriggerChecks() {
        triggerChecks.clear();
        triggerFires.clear();
        checksGood = checksBad = checksUnclear = 0;
        triggersDistrusted = false;
    }
    char const* predictCheck(GJBaseGameLayer* layer, TriggerCheck const& c, double& x, double& y, double& spin, int& used,
        std::string* detail = nullptr) {
        float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
        x = c.x;
        y = c.y;
        spin = 0.0;
        used = 0;
        for (auto const& f : triggerFires) {
            if (f.tick > c.at || !inGroups(c.object, f.trigger->m_targetGroupID)) {
                continue;
            }
            int id = f.trigger->m_objectID;
            if (unmodeledMover(id)) {
                if (f.tick >= c.fired - st(240)) {
                    return "a follow / keyframe / area trigger moves it";
                }
                continue;
            }
            if (id != 901 && id != 1346) {
                continue;
            }
            ScriptedMove m = f.move;
            bool rotate = id == 1346;
            int start = std::max(c.fired, f.tick);
            if (m.steps >= 0.5 && f.tick + m.steps <= c.fired) {
                continue;
            }
            if (!plainTrigger(f.trigger, rotate)) {
                return "a target / lock-to-player move moves it";
            }
            double done = m.steps < 0.5 ? 1.0 : easeMove(m.easing, m.rate, (c.at - f.tick) / m.steps);
            double before = m.steps < 0.5 ? (f.tick < c.fired ? 1.0 : 0.0) : easeMove(m.easing, m.rate, (start - f.tick) / m.steps);
            applyMove(m, done - before, x, y, spin);
            ++used;
            if (detail) {
                auto* t = f.trigger;
                *detail += rotate ? fmt::format(" | rotate {:g}deg{}{} {:.2f}s e{}/{:g}", m.angle, m.hasCenter ? " around center" : " in place",
                                        m.lockRot ? " locked" : "", t->m_duration, m.easing, m.rate)
                                  : fmt::format(" | move {:g},{:g}{}{} {:.2f}s e{}/{:g}", m.dx, m.dy, t->m_smallStep ? " smallstep" : "",
                                        t->m_isSilent ? " silent" : "", t->m_duration, m.easing, m.rate);
                *detail += fmt::format(" from t{}", f.tick - c.fired);
            }
        }
        return nullptr;
    }
    void runTriggerChecks(GJBaseGameLayer* layer) {
        for (auto it = triggerChecks.begin(); it != triggerChecks.end();) {
            auto const& c = *it;
            if (tickCount < c.fired) {
                it = triggerChecks.erase(it);
                continue;
            }
            if (tickCount < c.at) {
                ++it;
                continue;
            }
            double x, y, spin;
            int used;
            std::string detail;
            char const* unclear = predictCheck(layer, c, x, y, spin, used, buf.explain ? &detail : nullptr);
            double moved = std::hypot(c.object->m_positionX - c.x, c.object->m_positionY - c.y);
            double turned = std::abs(std::remainder(c.object->getRotationX() - c.rot, 360.0));
            if (!unclear && moved < 0.5 && turned < 0.5) {
                unclear = "it didn't move";
            }
            if (unclear) {
                ++checksUnclear;
                if (buf.explain && checksUnclear % 10 == 1) {
                    say("trigger check: unclear ({}), {} unclear so far", unclear, checksUnclear);
                }
                it = triggerChecks.erase(it);
                continue;
            }
            double err = std::hypot(c.object->m_positionX - x, c.object->m_positionY - y);
            double tol = std::max(3.0, std::hypot(x - c.x, y - c.y) * 0.1);
            double turnErr = std::abs(std::remainder(c.object->getRotationX() - (c.rot + spin), 360.0));
            bool ok = err <= tol && turnErr <= std::max(5.0, std::abs(spin) * 0.1);
            ok ? ++checksGood : ++checksBad;
            if (buf.explain) {
                say("trigger check: {} the prediction ({} trigger{}, off by {:.1f}u {:.0f} deg | {} hit / {} missed / {} unclear)",
                    ok ? "matches" : "DOESN'T match", used, used == 1 ? "" : "s", err, turnErr, checksGood, checksBad, checksUnclear);
                if (!ok) {
                    say("  obj {} expected moved {:.1f},{:.1f} turned {:.0f} | really moved {:.1f},{:.1f} turned {:.0f} over {}t{}",
                        c.object->m_objectID, x - c.x, y - c.y, spin, c.object->m_positionX - c.x, c.object->m_positionY - c.y,
                        std::remainder(c.object->getRotationX() - c.rot, 360.0), c.at - c.fired, detail);
                }
            }
            bool wrong = checksGood + checksBad >= 10 && checksBad > 2 * checksGood;
            if (wrong != triggersDistrusted) {
                triggersDistrusted = wrong;
                if (wrong) {
                    say("trigger check: {} of {} trigger predictions were wrong on this level, trigger prediction is off here", checksBad,
                        checksBad + checksGood);
                }
                else {
                    say("trigger check: {} of {} wrong now, trigger prediction is back on", checksBad, checksBad + checksGood);
                }
            }
            it = triggerChecks.erase(it);
        }
    }
    void collectScripted(GJBaseGameLayer* layer, int total) {
        scriptedMoves.clear();
        scriptedObjects.clear();
        buf.triggersAhead = 0;
        if (!buf.predictTriggers || triggersDistrusted || !buf.ghost1 || !layer->m_player1 || layer->m_player1->m_isGoingLeft) {
            return;
        }
        double px = buf.ghost1->getPositionX();
        double py = buf.ghost1->getPositionY();
        double reach = (total + 2) * std::max(static_cast<double>(std::abs(buf.tickDx)), 2.0) + 30.0;
        auto& cache = scriptedCache;
        if (cache.layer == layer && cache.tick == tickCount && reach <= cache.reach && std::abs(px - cache.px) < 3.0) {
            scriptedMoves = cache.moves;
            scriptedObjects = cache.objects;
            buf.triggersAhead = static_cast<int>(scriptedMoves.size());
            return;
        }
        scanMoveTriggers(layer);
        float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
        std::unordered_map<GameObject*, size_t> index;
        double pathMinX = px - 90.0, pathMaxX = px + reach + 90.0;
        double pathMinY = py - 450.0, pathMaxY = py + 450.0;
        auto nearPath = [&](GameObject* object, ScriptedMove const& m) {
            double x = object->m_positionX, y = object->m_positionY;
            double x0 = x, x1 = x, y0 = y, y1 = y;
            if (m.kind == TriggerKind::Move) {
                x0 = std::min(x, x + m.dx);
                x1 = std::max(x, x + m.dx);
                y0 = std::min(y, y + m.dy);
                y1 = std::max(y, y + m.dy);
            }
            else if (m.hasCenter) {
                double r = std::hypot(x - m.cx, y - m.cy);
                x0 = m.cx - r;
                x1 = m.cx + r;
                y0 = m.cy - r;
                y1 = m.cy + r;
            }
            constexpr double SIZE = 60.0;
            return x1 + SIZE >= pathMinX && x0 - SIZE <= pathMaxX && y1 + SIZE >= pathMinY && y0 - SIZE <= pathMaxY;
        };
        auto addMove = [&](EffectGameObject* t, double x, TriggerKind kind, bool armed, int when) {
            int elapsed = armed && when < 0 ? -when : -1;
            if (static_cast<int>(scriptedMoves.size()) >= SCRIPTED_TRIGGER_LIMIT) {
                return;
            }
            auto* group = layer->getGroup(t->m_targetGroupID);
            if (!group || group->count() == 0) {
                return;
            }
            ScriptedMove m;
            m.kind = kind;
            m.x = x;
            m.armed = armed;
            m.fireStep = armed ? when : -1;
            m.delay = armed ? 0 : when;
            if (kind == TriggerKind::Toggle) {
                m.group = group;
                m.on = t->m_activateGroup;
                scriptedMoves.push_back(m);
                return;
            }
            fillMove(layer, t, kind, tickLength, m);
            if (elapsed >= 0) {
                if (m.steps < 0.5 || elapsed >= m.steps) {
                    return;
                }
                m.from = easeMove(m.easing, m.rate, elapsed / m.steps);
            }
            int move = static_cast<int>(scriptedMoves.size());
            bool any = false;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || object == t || object->m_isDecoration || object->m_objectType == GameObjectType::Decoration ||
                    !nearPath(object, m)) {
                    continue;
                }
                auto [found, added] = index.try_emplace(object, scriptedObjects.size());
                if (added) {
                    if (scriptedObjects.size() >= SCRIPTED_OBJECT_LIMIT) {
                        index.erase(found);
                        continue;
                    }
                    bool mover = std::any_of(movers.begin(), movers.end(), [&](Mover const& mv) { return mv.object == object; });
                    scriptedObjects.push_back({ object, object->m_positionX, object->m_positionY, object->getPosition(),
                        object->getRotationX(), object->getRotationY(), mover });
                }
                scriptedObjects[found->second].moves.push_back(move);
                any = true;
            }
            if (any) {
                scriptedMoves.push_back(m);
            }
        };
        if (!buf.predictMovers) {
            for (auto fired = firedMoves.begin(); fired != firedMoves.end();) {
                auto* t = fired->first;
                int elapsed = tickCount - fired->second;
                if (elapsed < 0 || elapsed * tickLength > t->m_duration * 60.f + 1.f) {
                    fired = firedMoves.erase(fired);
                    continue;
                }
                bool rotate = t->m_objectID == 1346;
                if (plainTrigger(t, rotate)) {
                    addMove(t, 0.0, rotate ? TriggerKind::Rotate : TriggerKind::Move, true, -elapsed);
                }
                ++fired;
            }
        }
        std::function<void(int, double, bool, int, int)> spawnChain = [&](int groupID, double x, bool armed, int when, int depth) {
            auto* group = layer->getGroup(groupID);
            if (!group || depth > 4) {
                return;
            }
            int seen = 0;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || ++seen > 600) {
                    break;
                }
                int id = object->m_objectID;
                if (id != 901 && id != 1346 && id != 1049 && id != 1268) {
                    continue;
                }
                auto* c = static_cast<EffectGameObject*>(object);
                if (!c->m_isSpawnTriggered) {
                    continue;
                }
                if (id == 1268) {
                    if (plainSpawn(c)) {
                        spawnChain(c->m_targetGroupID, x, armed, when + spawnSteps(c, tickLength), depth + 1);
                    }
                }
                else if (id == 1049) {
                    if (c->m_targetGroupID > 0) {
                        addMove(c, x, TriggerKind::Toggle, armed, when);
                    }
                }
                else if (plainTrigger(c, id == 1346)) {
                    addMove(c, x, id == 1346 ? TriggerKind::Rotate : TriggerKind::Move, armed, when);
                }
            }
        };
        if (!pendingSpawns.empty()) {
            std::erase_if(pendingSpawns, [](PendingSpawn const& p) { return p.at < tickCount; });
            for (auto const& p : pendingSpawns) {
                spawnChain(p.group, 0.0, true, p.at - tickCount, 0);
            }
        }
        auto it = std::lower_bound(levelTriggers.begin(), levelTriggers.end(), px - 2.0, [](auto const& m, double x) { return m.x < x; });
        for (; it != levelTriggers.end() && it->x <= px + reach; ++it) {
            if (it->trigger->m_activated) {
                continue;
            }
            if (it->kind == TriggerKind::Spawn) {
                spawnChain(it->trigger->m_targetGroupID, it->x, false, spawnSteps(it->trigger, tickLength), 0);
            }
            else {
                addMove(it->trigger, it->x, it->kind, false, 0);
            }
        }
        buf.triggersAhead = static_cast<int>(scriptedMoves.size());
        if (scriptedMoves.empty()) {
            scriptedObjects.clear();
        }
        cache.layer = layer;
        cache.tick = tickCount;
        cache.px = px;
        cache.reach = reach;
        cache.moves = scriptedMoves;
        cache.objects = scriptedObjects;
    }
    void simToggle(CCArray* group, bool on) {
        int seen = 0;
        for (auto* object : CCArrayExt<GameObject*>(group)) {
            if (!object || ++seen > 2000) {
                break;
            }
            if (!on) {
                buf.simDisabled.insert(object);
                continue;
            }
            buf.simDisabled.erase(object);
            if (object->m_isGroupDisabled && buf.simEnabled.size() < 256 &&
                std::find(buf.simEnabled.begin(), buf.simEnabled.end(), object) == buf.simEnabled.end()) {
                buf.simEnabled.push_back(object);
            }
        }
    }
    void fireScripted(double gx, int tick) {
        for (auto& m : scriptedMoves) {
            if (!m.armed && gx >= m.x) {
                m.armed = true;
                m.fireStep = tick + m.delay;
            }
            if (m.kind == TriggerKind::Toggle && m.armed && !m.toggled && tick >= m.fireStep && m.group) {
                m.toggled = true;
                simToggle(m.group, m.on);
            }
        }
    }
    void placeScripted(int step) {
        for (auto& o : scriptedObjects) {
            double x = o.mover ? o.object->m_positionX : o.x;
            double y = o.mover ? o.object->m_positionY : o.y;
            double spin = 0.0;
            bool any = false;
            for (int i : o.moves) {
                auto const& m = scriptedMoves[i];
                if (!m.armed || step <= m.fireStep) {
                    continue;
                }
                double f = (m.steps < 0.5 ? 1.0 : easeMove(m.easing, m.rate, (step - m.fireStep) / m.steps)) - m.from;
                if (f == 0.0) {
                    continue;
                }
                any = true;
                applyMove(m, f, x, y, spin);
            }
            if (!any && !o.placed) {
                continue;
            }
            if (o.placed && !o.mover && x == o.lastX && y == o.lastY && spin == o.lastSpin) {
                continue;
            }
            o.lastX = x;
            o.lastY = y;
            o.lastSpin = spin;
            o.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
            o.object->m_positionX = x;
            o.object->m_positionY = y;
            if (!o.mover && (spin != 0.0 || o.rotated)) {
                o.object->setRotationX(static_cast<float>(o.rotX + spin));
                o.object->setRotationY(static_cast<float>(o.rotY + spin));
                o.rotated = true;
            }
            o.object->setObjectRectDirty(true);
            o.object->m_isOrientedBoxDirty = true;
            o.placed = true;
        }
    }
    void restoreScripted() {
        for (auto& o : scriptedObjects) {
            if (o.placed && !o.mover) {
                o.object->setPosition(o.node);
                o.object->m_positionX = o.x;
                o.object->m_positionY = o.y;
                if (o.rotated) {
                    o.object->setRotationX(o.rotX);
                    o.object->setRotationY(o.rotY);
                }
                o.object->setObjectRectDirty(true);
                o.object->m_isOrientedBoxDirty = true;
            }
        }
        scriptedObjects.clear();
        scriptedMoves.clear();
    }
    void finishDrift(char const* why) {
        if (!drift.active) {
            return;
        }
        drift.active = false;
        if (drift.compared >= 8) {
            say("drift {:.1f}u max at t{} over {}t{}", drift.maxErr, drift.maxAt, drift.compared, why);
        }
    }
    void driftStep(PlayerObject* real) {
        if (!drift.active) {
            return;
        }
        if (drift.index >= 0) {
            if (drift.index >= static_cast<int>(drift.path.size())) {
                finishDrift("");
                return;
            }
            float err = ccpDistance(real->getPosition(), drift.path[drift.index]);
            if (err > drift.maxErr) {
                drift.maxErr = err;
                drift.maxAt = drift.index;
            }
            ++drift.compared;
        }
        ++drift.index;
    }
    constexpr float SLICE = 15.f;
    constexpr int OUTCOME_TICKS = 90;
    struct Spot {
        uint32_t ok = 0;
        uint32_t bad = 0;
        uint32_t deaths = 0;
        uint32_t delaySum = 0;
        uint32_t last = 0;
    };
    std::unordered_map<int, Spot> memory;
    std::string memoryKey;
    bool memoryDirty = false;
    int memoryUpdates = 0;
    struct PendingOutcome {
        bool active = false;
        int slice = 0;
        int tick = 0;
        int delay = 0;
        bool skipped = false;
    } pending;
    int sliceOf(PlayerObject* p) {
        return static_cast<int>(std::floor(p->getPosition().x / SLICE));
    }
    Spot spotAround(int slice) {
        Spot sum;
        for (int s = slice - 1; s <= slice + 1; ++s) {
            auto it = memory.find(s);
            if (it == memory.end()) {
                continue;
            }
            sum.ok += it->second.ok;
            sum.bad += it->second.bad;
            sum.deaths += it->second.deaths;
            sum.delaySum += it->second.delaySum;
        }
        return sum;
    }
    std::filesystem::path memoryFile() {
        return cbfModGet()->getSaveDir() / "memory" / (memoryKey + "_v3.txt");
    }
    size_t memorySpots() {
        return memory.size();
    }
    void saveMemory() {
        if (!memoryDirty || memoryKey.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(memoryFile().parent_path(), ec);
        std::ofstream out(memoryFile(), std::ios::trunc);
        for (auto const& [slice, spot] : memory) {
            out << slice << ' ' << spot.ok << ' ' << spot.bad << ' ' << spot.deaths << ' ' << spot.delaySum << ' ' << spot.last << '\n';
        }
        memoryDirty = false;
    }
    void loadMemory(GJGameLevel* level) {
        memory.clear();
        memoryDirty = false;
        memoryUpdates = 0;
        pending.active = false;
        memoryKey.clear();
        if (!level) {
            return;
        }
        int id = level->m_levelID.value();
        if (id > 0) {
            memoryKey = fmt::format("level_{}", id);
        }
        else {
            std::string name = level->m_levelName;
            for (auto& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c))) {
                    c = '_';
                }
            }
            memoryKey = "local_" + name;
        }
        std::ifstream in(memoryFile());
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            int slice;
            Spot spot;
            if (!(fields >> slice >> spot.ok >> spot.bad >> spot.deaths >> spot.delaySum)) {
                continue;
            }
            fields >> spot.last;
            memory[slice] = spot;
        }
        std::error_code ec;
        auto written = std::filesystem::last_write_time(memoryFile(), ec);
        if (!ec && std::filesystem::file_time_type::clock::now() - written > std::chrono::hours(12)) {
            for (auto it = memory.begin(); it != memory.end();) {
                auto& s = it->second;
                uint32_t ok = s.ok * 3 / 4;
                s.delaySum = s.ok > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(s.delaySum) * ok / s.ok) : 0;
                s.ok = ok;
                if (ok == 0) {
                    s.last = 0;
                }
                s.bad = s.bad * 3 / 4;
                s.deaths = s.deaths * 3 / 4;
                it = (s.ok | s.bad | s.deaths) == 0 ? memory.erase(it) : std::next(it);
            }
            memoryDirty = true;
            saveMemory();
        }
        if (!memory.empty()) {
            say("memory: {} spots learned on this level", memory.size());
        }
    }
    void memoryChanged() {
        memoryDirty = true;
        if (++memoryUpdates % 10 == 0) {
            saveMemory();
        }
    }
    void creditPending() {
        if (!pending.active) {
            return;
        }
        pending.active = false;
        if (pending.delay > 0) {
            auto& spot = memory[pending.slice];
            auto delay = static_cast<uint32_t>(std::lround(pending.delay / stepScale()));
            ++spot.ok;
            spot.delaySum += delay;
            spot.last = delay;
            if (spot.bad > 0) {
                --spot.bad;
            }
            memoryChanged();
        }
    }
    void memoryNote(PlayerObject* p1, int delay) {
        if (!buf.learn || memoryKey.empty()) {
            return;
        }
        if (pending.active && tickCount - pending.tick >= st(30)) {
            creditPending();
        }
        pending = { true, sliceOf(p1), tickCount, delay, false };
    }
    void memoryTick() {
        if (pending.active && tickCount - pending.tick >= st(OUTCOME_TICKS)) {
            creditPending();
        }
    }
    bool memorySaysSkip(int slice);
    void memoryDeath() {
        if (!pending.active) {
            return;
        }
        pending.active = false;
        auto& spot = memory[pending.slice];
        ++spot.deaths;
        if (pending.delay > 0) {
            ++spot.bad;
        }
        if (pending.skipped) {
            int worst = pending.slice;
            for (int s = pending.slice - 1; s <= pending.slice + 1; ++s) {
                auto it = memory.find(s);
                if (it != memory.end() && it->second.bad > memory[worst].bad) {
                    worst = s;
                }
            }
            if (memory[worst].bad > 0) {
                --memory[worst].bad;
            }
            if (!memorySaysSkip(pending.slice)) {
                say("memory: died with the input left alone, saves will be tried here again");
            }
        }
        memoryChanged();
    }
    bool memorySaysSkip(int slice) {
        if (!buf.learn) {
            return false;
        }
        auto spot = spotAround(slice);
        return spot.bad >= 2 && spot.bad > spot.ok;
    }
    int memoryDelay(int slice) {
        if (!buf.learn) {
            return 0;
        }
        for (int s : { slice, slice - 1, slice + 1 }) {
            auto it = memory.find(s);
            if (it != memory.end() && it->second.last > 0) {
                return st(static_cast<int>(it->second.last));
            }
        }
        auto spot = spotAround(slice);
        if (spot.ok < 2) {
            return 0;
        }
        return st(static_cast<int>(std::lround(static_cast<double>(spot.delaySum) / spot.ok)));
    }
    bool memoryRisky(int slice) {
        auto spot = spotAround(slice);
        return spot.deaths >= 3;
    }
    void snapshotObject(GameObject* object) {
        if (!object || buf.simUsed.contains(object)) {
            return;
        }
        buf.simUsed.insert(object);
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
            s.animated = effect->m_animationTriggered;
        }
        buf.snapshots.push_back(s);
    }
    struct NodeMark {
        CCNode* node;
        unsigned count;
    };
    std::vector<NodeMark> levelMarks;
    void markLevelNodes(PlayLayer* layer) {
        levelMarks.clear();
        auto add = [](CCNode* n) {
            if (n) {
                levelMarks.push_back({ n, n->getChildrenCount() });
            }
        };
        add(layer);
        add(layer->m_objectLayer);
        if (layer->m_aboveShaderObjectLayer != layer->m_objectLayer) {
            add(layer->m_aboveShaderObjectLayer);
        }
        if (layer->m_objectLayer && layer->m_objectLayer->getChildren()) {
            for (auto* child : CCArrayExt<CCNode*>(layer->m_objectLayer->getChildren())) {
                add(child);
            }
        }
    }
    void sweepLevelNodes() {
        for (auto const& m : levelMarks) {
            auto* kids = m.node->getChildren();
            while (kids && kids->count() > m.count) {
                m.node->removeChild(static_cast<CCNode*>(kids->lastObject()), true);
            }
        }
        levelMarks.clear();
    }
    void restoreObjects() {
        sweepLevelNodes();
        for (auto const& s : buf.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
                s.effect->m_animationTriggered = s.animated;
            }
        }
        buf.snapshots.clear();
        buf.simUsed.clear();
        buf.orbsUsed.clear();
        buf.simDisabled.clear();
        buf.simEnabled.clear();
    }
    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }
    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }
    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing || player->m_isShip;
    }
    void releaseAutoClicks() {
        if (buf.auto1) {
            if (!buf.held1 && buf.ghost1) {
                buf.ghost1->releaseButton(PlayerButton::Jump);
            }
            buf.auto1 = false;
        }
        if (buf.auto2) {
            if (!buf.held2 && buf.ghost2) {
                buf.ghost2->releaseButton(PlayerButton::Jump);
            }
            buf.auto2 = false;
        }
    }
    enum class SimEnd { Ok, StopSearch };
    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        markLevelNodes(layer);
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.ghostSwitched = false;
        buf.ghostSolo = false;
        buf.firstOrbTick = -1;
        buf.lastOrbTick = -1;
        buf.laterOrbTouched = false;
        buf.orbUsedTick = -1;
        bool second = dual && buf.ghost2 && p2;
        buf.simSecond = second;
        buf.haveRealRect = buf.ignoreTouching;
        buf.harmless.clear();
        buf.haveStartRects = true;
        buf.realRect1 = p1->getObjectRect();
        buf.realRect2 = second ? p2->getObjectRect() : buf.realRect1;
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }
        buf.held1 = buf.held2 = buf.appliedInput;
        buf.auto1 = buf.auto2 = false;
        buf.autoAt1 = buf.autoAt2 = -1;
        bool waveRun = p1->m_isDart || (second && p2->m_isDart);
        if (buf.waveV46 && waveRun && delay > 0 && !buf.skipPress) {
            pressGhost(buf.ghost1, !down);
            if (second) {
                pressGhost(buf.ghost2, !down);
            }
        }
        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }
        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        bool spiderRun = buf.spiderSmart && (p1->m_isSpider || (second && p2->m_isSpider));
        bool ufoRun = buf.ufoSmart && (p1->m_isBird || (second && p2->m_isBird));
        bool ballRun = buf.ballSmart && (p1->m_isBall || (second && p2->m_isBall));
        int total = delay + horizon;
        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        bool safe = false;
        collectMovers();
        collectScripted(layer, total);
        for (; tick < total; ++tick) {
            second = buf.simSecond;
            if (!movers.empty()) {
                placeMovers(tick + 1);
            }
            if (!scriptedMoves.empty()) {
                fireScripted(buf.ghost1->getPositionX(), tick);
                placeScripted(tick + 1);
            }
            buf.simTick = tick;
            if (buf.autoAt1 >= 0 && tick >= buf.autoAt1) {
                if (!buf.held1 && buf.ghost1->m_touchingRings->count() > 0) {
                    buf.ghost1->pushButton(PlayerButton::Jump);
                    buf.auto1 = true;
                }
                buf.autoAt1 = -2;
            }
            if (second && buf.autoAt2 >= 0 && tick >= buf.autoAt2) {
                if (!buf.held2 && buf.ghost2->m_touchingRings->count() > 0) {
                    buf.ghost2->pushButton(PlayerButton::Jump);
                    buf.auto2 = true;
                }
                buf.autoAt2 = -2;
            }
            if (tick == delay && !buf.skipPress) {
                pressGhost(buf.ghost1, down);
                if (second) {
                    pressGhost(buf.ghost2, down);
                }
            }
            if (buf.retapAt >= 0) {
                int t = tick - delay - buf.retapAt;
                if (t == 0 || t == 1 || t == 5) {
                    pressGhost(buf.ghost1, t == 1);
                    if (second) {
                        pressGhost(buf.ghost2, t == 1);
                    }
                }
            }
            if (down && releaseAfter >= 0 && tick == delay + releaseAfter && buf.ghost1->m_isDashing) {
                ++releaseAfter;
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                pressGhost(buf.ghost1, !down);
                if (second) {
                    pressGhost(buf.ghost2, !down);
                }
            }
            float stepDt = tick == 0 ? dt * buf.firstStep : dt;
            advanceGhost(buf.ghost1, stepDt);
            if (second) {
                advanceGhost(buf.ghost2, stepDt);
            }
            releaseAutoClicks();
            if (buf.ghostDied) {
                died = true;
                break;
            }
            checkGhost(layer, buf.ghost1, dt);
            if (!buf.ghostDied && second) {
                checkGhost(layer, buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (buf.recordPath) {
                buf.recordPath->push_back(buf.ghost1->getPosition());
            }
            if (buf.ghostHitPortal) {
                safe = tick >= delay;
                break;
            }
            if (tick < delay) {
                continue;
            }
            ++survived;
            if (survived >= stopAt) {
                break;
            }
            if ((flying && !ufoRun) || buf.ghostSwitched) {
                continue;
            }
            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (grounded && airTicks > 0 && buf.captureKiller) {
                buf.baseTouchedDown = true;
            }
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (spiderRun) {
                if (++groundTicks >= st(SPIDER_LAND)) {
                    safe = true;
                    break;
                }
                continue;
            }
            if (airTicks < st(ballRun ? 2 : 6)) {
                continue;
            }
            if (groundTicks > st(ballRun ? 8 : 2)) {
                safe = true;
                break;
            }
            ++groundTicks;
        }
        restoreScripted();
        if (!movers.empty()) {
            placeMovers(0);
            movers.clear();
        }
        ++buf.simRuns;
        buf.simTicks += tick + 1;
        if ((died || buf.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        buf.ghost1->stopAllActions();
        if (buf.ghost2) {
            buf.ghost2->stopAllActions();
        }
        if (buf.firstOrbTick >= 0) {
            buf.ghost1->stopParticles();
            if (buf.ghost2) {
                buf.ghost2->stopParticles();
            }
        }
        buf.lastRunSwitched = buf.ghostSwitched;
        buf.simulating = false;
        return safe ? horizon : survived;
    }
    std::chrono::steady_clock::time_point clickStart;
    bool clickTimed = false;
    double clickMs() {
        return clickTimed ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStart).count() : 0.0;
    }
    bool clickOverTime() {
        return clickMs() > buf.budgetMs * 1.25;
    }
    double searchBudget() {
        return clickTimed ? std::max(buf.budgetMs - clickMs(), buf.budgetMs * 0.5) : buf.budgetMs;
    }
    struct ClickTimer {
        ClickTimer() {
            clickStart = std::chrono::steady_clock::now();
            clickTimed = true;
        }
        ~ClickTimer() {
            clickTimed = false;
        }
    };
    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        buf.orbClick = buf.assumeOrbs;
        buf.touchedOrb = false;
        int first = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        if (!buf.tryBothOrbs || !buf.smart || !buf.touchedOrb || first >= stopAt || clickOverTime()) {
            return first;
        }
        SimEnd firstEnd = *end;
        buf.orbClick = !buf.assumeOrbs;
        int second = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        buf.orbClick = buf.assumeOrbs;
        if (firstEnd != SimEnd::StopSearch) {
            *end = SimEnd::Ok;
        }
        int best = std::max(first, second);
        if (buf.orbTiming && best < stopAt && !clickOverTime()) {
            for (int late : { 3, 6 }) {
                buf.orbClick = true;
                buf.orbClickDelay = st(late);
                SimEnd lateEnd;
                int ret = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, &lateEnd);
                buf.orbClickDelay = 0;
                buf.orbClick = buf.assumeOrbs;
                best = std::max(best, ret);
                if (best >= stopAt || clickOverTime()) {
                    break;
                }
            }
        }
        return best;
    }
    std::vector<CCPoint> recordRun(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon,
        int releaseAfter, bool* died = nullptr) {
        std::vector<CCPoint> path;
        buf.recordPath = &path;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, delay + horizon, &end);
        buf.recordPath = nullptr;
        if (died) {
            *died = buf.ghostDied;
        }
        return path;
    }
    void drawPath(std::vector<CCPoint> const& path, int delay, ccColor4F color, bool died) {
        for (size_t i = 1; i < path.size(); ++i) {
            buf.pathNode->drawSegment(path[i - 1], path[i], 1.2f, color);
        }
        if (delay > 0 && delay <= static_cast<int>(path.size())) {
            buf.pathNode->drawDot(path[delay - 1], 3.f, { 1.f, 1.f, 1.f, 0.9f });
        }
        if (died && !path.empty()) {
            buf.pathNode->drawDot(path.back(), 4.f, { 1.f, 0.2f, 0.2f, 1.f });
        }
    }
    void showPaths(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int chosen, bool down, int horizon, int releaseAfter) {
        if (!buf.showPath || !buf.pathNode) {
            return;
        }
        buf.pathNode->clear();
        buf.pathHoldUntil = tickCount + st(45);
        bool nowDied = false;
        auto now = recordRun(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &nowDied);
        ccColor4F red { 1.f, 0.3f, 0.3f, 0.85f };
        ccColor4F green { 0.3f, 1.f, 0.5f, 0.9f };
        drawPath(now, 0, chosen > 0 ? red : (nowDied ? red : green), nowDied);
        if (chosen > 0) {
            bool died = false;
            auto delayed = recordRun(layer, p1, p2, dual, chosen, down, horizon, releaseAfter, &died);
            drawPath(delayed, chosen, green, died);
        }
        if (buf.learn && !memory.empty()) {
            int last = INT_MIN;
            for (auto const& point : now) {
                int slice = static_cast<int>(std::floor(point.x / SLICE));
                if (slice != last && memoryRisky(slice)) {
                    buf.pathNode->drawDot(point, 3.5f, { 1.f, 0.6f, 0.1f, 0.95f });
                }
                last = slice;
            }
        }
    }
    void startDrift(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        finishDrift("");
        if (!buf.driftCheck) {
            return;
        }
        drift.path = recordRun(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
        drift.active = !drift.path.empty();
        drift.index = buf.firstStep < 1.f ? 0 : -1;
        drift.compared = 0;
        drift.maxErr = 0.f;
        drift.maxAt = 0;
    }
    void decided(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        memoryNote(p1, delay);
        if (clickMs() > buf.budgetMs * 1.5) {
            finishDrift("");
            return;
        }
        startDrift(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
        showPaths(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
    }
    int fullWaveHorizon(bool mini) {
        int wh = std::clamp(buf.waveHorizon, 20, 160);
        return st(mini ? std::max(20, wh - 15) : wh);
    }
    constexpr int NEXT_CLICK_MARGINS[] = { 8, 16, 26 };
    constexpr int NEXT_CLICK_MIN = 24;
    int nextClickAt(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int ret) {
        if (!buf.nextClick || ret >= horizon || ret < st(NEXT_CLICK_MIN + 8)) {
            return -1;
        }
        int length = std::min(horizon, ret + st(40));
        for (int margin : NEXT_CLICK_MARGINS) {
            int at = ret - st(margin);
            if (at < st(NEXT_CLICK_MIN) || clickOverTime()) {
                break;
            }
            buf.retapAt = at;
            SimEnd e;
            int r = simulate(layer, p1, p2, dual, delay, down, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (r >= length) {
                return at;
            }
        }
        return -1;
    }
    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int horizon, int releaseAfter, int learned, int maxDelay,
        int* landed) {
        int window = st(std::clamp(buf.orbWindow, 1, 64));
        int found = 0;
        SimEnd end;
        buf.orbClick = false;
        buf.orbWasTiming = false;
        buf.orbAfterFirst = true;
        struct Reset {
            ~Reset() {
                buf.orbClick = buf.assumeOrbs;
                buf.orbAfterFirst = false;
                buf.orbClickDelay = 0;
                buf.ghostShiftX = 0.f;
            }
        } reset;
        auto tryAt = [&](int k, bool* used) {
            int best = -1;
            *used = false;
            for (int late : { 0, 3, 6, 10 }) {
                buf.orbClickDelay = st(late);
                int ret = simulate(layer, p1, p2, dual, k, true, horizon, releaseAfter, horizon, &end);
                buf.orbClickDelay = 0;
                if (late == 0) {
                    *used = buf.orbUsedTick >= 0;
                }
                best = std::max(best, ret);
                if (ret >= horizon || buf.orbUsedTick < 0 || !buf.laterOrbTouched || !buf.orbTiming || end == SimEnd::StopSearch) {
                    break;
                }
            }
            return best;
        };
        auto robustAt = [&](int k) {
            if (!buf.robustSaves) {
                return true;
            }
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            for (float off : { shift, -shift }) {
                buf.ghostShiftX = off;
                bool used;
                int ret = tryAt(k, &used);
                buf.ghostShiftX = 0.f;
                if (ret < horizon || !used) {
                    return false;
                }
            }
            return true;
        };
        if (learned > 0 && learned <= maxDelay) {
            bool used;
            int ret = tryAt(learned, &used);
            if (used && ret >= horizon) {
                *landed = ret;
                return learned;
            }
        }
        buf.skipPress = true;
        buf.orbAfterFirst = false;
        int probeLen = std::min(maxDelay, std::max(window, st(40))) + 2;
        simulate(layer, p1, p2, dual, 0, true, probeLen, -1, probeLen, &end);
        buf.skipPress = false;
        buf.orbAfterFirst = true;
        int touch = buf.firstOrbTick;
        int leave = buf.lastOrbTick;
        std::string trace;
        if (touch < 0 || touch > window) {
            return 0;
        }
        bool nowUsed;
        int now = tryAt(0, &nowUsed);
        if (nowUsed && (now >= horizon || !buf.orbTiming)) {
            return 0;
        }
        int from = nowUsed ? 1 : std::max(1, touch - 1);
        int to = std::min(maxDelay, std::max(leave, touch + st(6)));
        int safeStart = -1, safeEnd = -1;
        int bestRet = -1, bestK = 0;
        int bestAt = -1;
        std::string nextNote;
        double orbBudget = buf.budgetMs * 1.75;
        for (int k = from; k <= to; ++k) {
            if (clickMs() > orbBudget) {
                trace += " (budget)";
                break;
            }
            bool used;
            int ret = tryAt(k, &used);
            if (buf.explain) {
                trace += fmt::format(" +{}:{}{}", k, ret, used ? "o" : "-");
            }
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (!used) {
                if (safeStart >= 0) {
                    break;
                }
                continue;
            }
            if (ret >= horizon) {
                if (safeStart < 0) {
                    safeStart = k;
                }
                safeEnd = k;
            }
            else if (safeStart >= 0) {
                break;
            }
            if (k + ret > bestAt) {
                bestAt = k + ret;
                bestRet = ret;
                bestK = k;
            }
        }
        if (safeStart >= 0) {
            int mid = (safeStart + safeEnd) / 2;
            found = mid;
            for (int off = 0; off <= safeEnd - safeStart; ++off) {
                int k = off % 2 == 0 ? mid + off / 2 : mid - (off + 1) / 2;
                if (k < safeStart || k > safeEnd) {
                    continue;
                }
                if (clickMs() > buf.budgetMs * 2.0 || robustAt(k)) {
                    found = k;
                    break;
                }
            }
            *landed = horizon;
        }
        else {
            int nowNext = nowUsed && now >= 0 ? nextClickAt(layer, p1, p2, dual, 0, true, horizon, releaseAfter, now) : -1;
            if (nowNext >= 0) {
                nextNote = fmt::format(" (now is fine if you click again by t{})", nowNext);
            }
            else if (bestK > 0 && bestAt >= std::max(now, 0) + st(nowUsed ? 30 : 12)) {
                found = bestK;
                *landed = bestRet;
            }
            else if (bestK > 0 && bestAt > std::max(now, 0)) {
                int at = nextClickAt(layer, p1, p2, dual, bestK, true, horizon, releaseAfter, bestRet);
                if (at >= 0) {
                    found = bestK;
                    *landed = bestRet;
                    nextNote = fmt::format(" (safe if you click again by t{})", at);
                }
            }
        }
        buf.orbWasTiming = found > 0 && nowUsed;
        if (buf.explain) {
            say("orb: inside t{}-t{}, press now {} -> {}t |{} => {}{}{}", touch, leave, nowUsed ? "uses it" : "misses it", now,
                trace.empty() ? " (nothing to try)" : trace, found > 0 ? fmt::format("+{}t", found) : std::string("no pick"),
                safeStart >= 0 ? fmt::format(" (safe +{}..+{}t)", safeStart, safeEnd) : std::string(), nextNote);
        }
        return found;
    }
    int dbgBestK = 0;
    int dbgBestRet = -1;
    int searchFrom = 1;
    int budgetCutK = 0;
    bool resumeNow = false;
    QueuedInput resumeEntry {};
    constexpr int MAX_RESUMES = 4;
    constexpr int WAVE_TURN_MARGINS[] = { 4, 8, 12 };
    constexpr int WAVE_MIN_TURN = 6;
    constexpr int WAVE_ESCAPE = 16;
    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : WAVE_TURN_MARGINS) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) {
                break;
            }
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = turnAt;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }
    constexpr int ROBOT_HOLDS[] = { 4, 9, 14, 20, 27, 36, 48, 64 };
    int robotUsedHold = -1;
    bool quickScore = false;
    int robotScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int target, SimEnd* end) {
        int usual = robotHold.pick(st(20), 50);
        int best = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, usual, target, end);
        robotUsedHold = usual;
        if (best >= target || *end == SimEnd::StopSearch || quickScore) {
            return best;
        }
        int holds[std::size(ROBOT_HOLDS)];
        std::copy(std::begin(ROBOT_HOLDS), std::end(ROBOT_HOLDS), holds);
        std::stable_sort(std::begin(holds), std::end(holds), [&](int a, int b) { return std::abs(st(a) - usual) < std::abs(st(b) - usual); });
        for (int hold : holds) {
            if (std::abs(st(hold) - usual) < st(3)) {
                continue;
            }
            if (clickOverTime()) {
                break;
            }
            SimEnd e;
            int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, st(hold), target, &e);
            if (ret > best) {
                best = ret;
                robotUsedHold = st(hold);
            }
            if (best >= target) {
                break;
            }
        }
        return best;
    }
    constexpr int SPIDER_RETAP[] = { 4, 8, 12 };
    constexpr int SPIDER_MIN_RETAP = 4;
    int spiderScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : SPIDER_RETAP) {
            int at = ret - st(margin);
            if (at < st(SPIDER_MIN_RETAP)) {
                break;
            }
            int length = std::min(horizon, at + st(16));
            buf.retapAt = at;
            SimEnd e;
            int retapped = simulateOrbs(layer, p1, p2, dual, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (retapped >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = at;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }
    constexpr int SAME_CRASH_TRIES = 4;
    bool crashFixed = false;
    bool sameCrash(int k, int ret, int base, int& streak) {
        if (streak >= 0 && std::abs(k + ret - base) <= 1) {
            ++streak;
        }
        else {
            streak = -1;
        }
        if (streak >= SAME_CRASH_TRIES) {
            crashFixed = true;
            return true;
        }
        return false;
    }
    int searchSmallest(int horizon, int maxDelay, int base, int* landed, std::function<int(int, int, SimEnd*)> const& score,
        double budget = -1.0, bool quiet = false, bool needFull = false) {
        if (budget < 0.0) {
            budget = buf.budgetMs;
        }
        budget = std::min(budget, searchBudget());
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = needFull ? horizon : std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        budgetCutK = 0;
        int streak = 0;
        for (int k = searchFrom; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = score(k, target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                return k;
            }
            if (sameCrash(k, ret, base, streak)) {
                return 0;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget) {
                if (!quiet) {
                    say("prediction budget hit at +{} ticks", k);
                }
                budgetCutK = k;
                return 0;
            }
        }
        return 0;
    }
    int searchTwoPass(int horizon, int maxDelay, int base, int* landed, std::function<int(int, int, SimEnd*)> const& score,
        bool needFull = false) {
        quickScore = true;
        int k = searchSmallest(horizon, maxDelay, base, landed, score, searchBudget() * 0.5, true, needFull);
        quickScore = false;
        if (k > 0) {
            return k;
        }
        crashFixed = false;
        double left = buf.budgetMs - clickMs();
        if (left < 0.5) {
            say("prediction budget hit (quick pass only)");
            budgetCutK = 0;
            return 0;
        }
        return searchSmallest(horizon, maxDelay, base, landed, score, left, false, needFull);
    }
    constexpr int SHIP_RECOVER[] = { 8, 16, 24 };
    int shipScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : SHIP_RECOVER) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) {
                break;
            }
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = turnAt;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }
    constexpr int UFO_REFLAP[] = { 6, 12, 18 };
    int ufoScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : UFO_REFLAP) {
            int at = ret - st(margin);
            if (at < st(4)) {
                break;
            }
            int length = std::min(horizon, at + st(20));
            buf.retapAt = at;
            SimEnd e;
            int reflapped = simulateOrbs(layer, p1, p2, dual, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (reflapped >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = at;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }
    bool marginUsed = false;
    int searchWave(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, int horizon, int maxDelay, int base,
        int* landed) {
        marginUsed = false;
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        double budget = searchBudget();
        int target = std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        budgetCutK = 0;
        for (int k = searchFrom; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = waveScore(layer, p1, p2, dual, k, down, horizon, target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                if (buf.waveMargin && k + 2 <= maxDelay) {
                    SimEnd e1, e2;
                    if (waveScore(layer, p1, p2, dual, k + 1, down, horizon, target, &e1) >= target &&
                        waveScore(layer, p1, p2, dual, k + 2, down, horizon, target, &e2) >= target) {
                        marginUsed = true;
                        return k + 1;
                    }
                }
                return k;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget) {
                say("prediction budget hit at +{} ticks", k);
                budgetCutK = k;
                return 0;
            }
        }
        return 0;
    }
    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, bool anyWave, int horizon, int maxDelay,
        int releaseAfter, int base, int* landed, bool needFull = false) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        double budget = searchBudget();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget;
        };
        int target = needFull ? horizon : std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        int sameStreak = 0;
        int waveBestK = 0;
        int waveBestRet = base;
        budgetCutK = 0;
        for (int k = searchFrom; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, anyWave ? horizon : target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (anyWave) {
                if (ret >= horizon && ret - base >= k * 2 + 6) {
                    waveBestK = k;
                    waveBestRet = ret;
                    break;
                }
                if (ret >= base + k * 2 + 10 && ret > waveBestRet + 2) {
                    waveBestK = k;
                    waveBestRet = ret;
                    if (ret >= horizon) {
                        break;
                    }
                }
            }
            else if (ret < target && sameCrash(k, ret, base, sameStreak)) {
                break;
            }
            else if (ret >= target) {
                SimEnd full;
                if (ret < horizon && simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, horizon, &full) < horizon) {
                    for (int j = k + 1; j <= maxDelay && !overBudget(); ++j) {
                        SimEnd e;
                        int r = simulateOrbs(layer, p1, p2, dual, j, down, horizon, releaseAfter, horizon, &e);
                        if (e == SimEnd::StopSearch) {
                            break;
                        }
                        if (r >= horizon) {
                            *landed = r;
                            return j;
                        }
                    }
                }
                *landed = ret;
                return k;
            }
            if (overBudget()) {
                say("prediction budget hit at +{} ticks", k);
                budgetCutK = anyWave ? 0 : k;
                break;
            }
        }
        if (anyWave && waveBestK > 0) {
            *landed = waveBestRet;
            return waveBestK;
        }
        return 0;
    }
    void livePath(PlayLayer* layer, PlayerObject* p1) {
        if (!buf.showPath || buf.perfMode || !buf.pathNode || !buf.ghost1 || buf.simulating || p1->m_isDead || layer->m_isPlatformer) {
            return;
        }
        if (tickCount < buf.pathHoldUntil || !buf.queue.empty() || tickCount % st(12) != 0) {
            return;
        }
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        bool wave = p1->m_isDart || (p2 && p2->m_isDart);
        bool ship = p1->m_isShip || (p2 && p2->m_isShip);
        bool flyer = isFlying(p1) || (p2 && isFlying(p2));
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);
        int horizon = wave ? fullWaveHorizon(mini) : st(flyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        bool down = !buf.appliedInput;
        int releaseAfter = down && !wave && !ship ? holdEstimate() : -1;
        buf.pathNode->clear();
        bool idleDied = false;
        buf.skipPress = true;
        auto idle = recordRun(layer, p1, p2, dual, 0, down, horizon, -1, &idleDied);
        buf.skipPress = false;
        drawPath(idle, 0, { 0.45f, 0.7f, 1.f, 0.6f }, idleDied);
        bool clickDied = false;
        auto click = recordRun(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &clickDied);
        drawPath(click, 0, { 1.f, 0.85f, 0.3f, 0.85f }, clickDied);
    }
    void queueInput(int ticks, bool down) {
        buf.queue.push_back({ ticks, down });
    }
    struct Stats {
        int saves = 0;
        int earlySum = 0;
        int attempts = 0;
        std::map<int, int> savesAt;
    };
    Stats attemptStats;
    Stats sessionStats;
    std::string statsKey;
    void noteSave(PlayLayer* layer, int ticks) {
        int t = static_cast<int>(std::lround(ticks / stepScale()));
        int pct = std::clamp(static_cast<int>(layer->getCurrentPercent()), 0, 100);
        for (auto* s : { &attemptStats, &sessionStats }) {
            ++s->saves;
            s->earlySum += t;
            ++s->savesAt[pct];
        }
        if (buf.saveDing && !buf.train) {
            FMODAudioEngine::sharedEngine()->playEffect("counter003.ogg");
        }
    }
    std::string attemptSummary() {
        auto const& a = attemptStats;
        return fmt::format("{} {} | avg {:.1f}t ({:.2f}f) early", a.saves, buf.train ? (a.saves == 1 ? "early click" : "early clicks")
            : (a.saves == 1 ? "save" : "saves"), static_cast<double>(a.earlySum) / a.saves, a.earlySum * TICK / a.saves);
    }
    void endAttempt() {
        if (attemptStats.saves > 0) {
            ++sessionStats.attempts;
            say("attempt: {}", attemptSummary());
        }
        attemptStats = {};
    }
    bool trainInstead(PlayLayer* layer, int ticks, bool down, char const* what) {
        if (!buf.train) {
            return false;
        }
        noteSave(layer, ticks);
        say("TRAIN {} {}: {}t ({:g}f) early", what, down ? "press" : "release", ticks, ticks * TICK);
        if (buf.hintLabel) {
            buf.hintLabel->setString(fmt::format("{} {:g}f early", down ? "click" : "release", ticks * TICK).c_str());
            buf.hintLabel->stopAllActions();
            buf.hintLabel->setOpacity(230);
            buf.hintLabel->setScale(0.5f);
            buf.hintLabel->runAction(CCSequence::create(CCEaseBackOut::create(CCScaleTo::create(0.12f, 0.6f)), CCDelayTime::create(0.5f),
                CCFadeOut::create(0.4f), nullptr));
        }
        return true;
    }
    constexpr char const* PROFILE_KEYS[] = { "robust-saves", "orb-align", "orb-timing", "level-memory",
        "more-time-death-spots", "ship-buffer", "ufo-smart", "ball-smart", "swing-smart", "cbf-sync", "predict-moving-objects",
        "training-mode" };
    struct Globals {
        bool have = false;
        std::vector<bool> values;
    } globals;
    std::string profileKey() {
        return memoryKey.empty() ? std::string() : "profile-" + memoryKey;
    }
    void saveProfile() {
        if (!buf.perLevel || profileKey().empty() || !globals.have) {
            return;
        }
        auto v = matjson::Value::object();
        for (auto* key : PROFILE_KEYS) {
            v[key] = cbfModGet()->getSettingValue<bool>(key);
        }
        cbfModGet()->setSavedValue(profileKey(), v);
    }
    void restoreGlobals() {
        if (!globals.have) {
            return;
        }
        auto* mod = cbfModGet();
        for (size_t i = 0; i < std::size(PROFILE_KEYS); ++i) {
            mod->setSettingValue<bool>(PROFILE_KEYS[i], globals.values[i]);
        }
        globals = {};
        mod->setSavedValue("profile-globals", matjson::Value(nullptr));
        readSettings();
        applyStage();
    }
    void loadProfile() {
        if (!buf.perLevel || profileKey().empty()) {
            return;
        }
        auto* mod = cbfModGet();
        globals.have = true;
        globals.values.clear();
        auto backup = matjson::Value::object();
        for (auto* key : PROFILE_KEYS) {
            bool value = mod->getSettingValue<bool>(key);
            globals.values.push_back(value);
            backup[key] = value;
        }
        mod->setSavedValue("profile-globals", backup);
        if (!mod->hasSavedValue(profileKey())) {
            return;
        }
        auto v = mod->getSavedValue<matjson::Value>(profileKey());
        if (!v.isObject()) {
            return;
        }
        for (auto* key : PROFILE_KEYS) {
            if (v.contains(key)) {
                mod->setSettingValue<bool>(key, v[key].asBool().unwrapOr(mod->getSettingValue<bool>(key)));
            }
        }
        readSettings();
        applyStage();
        say("level settings loaded: {}", stageText());
    }
    void recoverGlobals() {
        auto* mod = cbfModGet();
        if (!mod->hasSavedValue("profile-globals")) {
            return;
        }
        auto v = mod->getSavedValue<matjson::Value>("profile-globals");
        if (!v.isObject()) {
            return;
        }
        for (auto* key : PROFILE_KEYS) {
            if (v.contains(key)) {
                mod->setSettingValue<bool>(key, v[key].asBool().unwrapOr(mod->getSettingValue<bool>(key)));
            }
        }
        mod->setSavedValue("profile-globals", matjson::Value(nullptr));
    }
    void refreshHeatmap() {
        auto* layer = GameManager::get()->m_playLayer;
        if (!buf.heatNode || !layer || !layer->m_progressBar) {
            return;
        }
        buf.heatNode->clear();
        float length = layer->m_levelLength;
        if (!buf.heatmap || length <= 0.f) {
            return;
        }
        auto size = layer->m_progressBar->getContentSize();
        uint32_t most = 0;
        for (auto const& [slice, spot] : memory) {
            most = std::max(most, spot.deaths);
        }
        if (most == 0) {
            return;
        }
        for (auto const& [slice, spot] : memory) {
            if (spot.deaths == 0) {
                continue;
            }
            float pct = std::clamp((slice + 0.5f) * SLICE / length, 0.f, 1.f);
            float x = 2.f + pct * (size.width - 4.f);
            float heat = static_cast<float>(spot.deaths) / most;
            buf.heatNode->drawSegment({ x, 2.f }, { x, size.height - 2.f }, 0.7f, { 1.f, 0.35f + 0.3f * (1.f - heat), 0.55f, 0.35f + 0.6f * heat });
        }
    }
    struct BenchRun {
        float startX = 0.f, startY = 0.f, dt = TICK;
        std::vector<std::pair<int, bool>> inputs;
        std::vector<CCPoint> track;
    };
    BenchRun benchLive;
    bool benchRecording = false;
    bool benchNeedStart = false;
    bool benchPending = false;
    std::string lastBench;
    std::filesystem::path benchFile() {
        return cbfModGet()->getSaveDir() / "bench" / (memoryKey + ".txt");
    }
    size_t benchSavedTicks() {
        std::ifstream in(benchFile());
        float x, y, dt;
        size_t ticks = 0;
        if (in >> x >> y >> dt >> ticks) {
            return ticks;
        }
        return 0;
    }
    bool benchLoad(BenchRun& r) {
        std::ifstream in(benchFile());
        size_t ticks = 0, inputs = 0;
        if (!(in >> r.startX >> r.startY >> r.dt >> ticks >> inputs)) {
            return false;
        }
        for (size_t i = 0; i < inputs; ++i) {
            int t, d;
            if (!(in >> t >> d)) {
                return false;
            }
            r.inputs.push_back({ t, d != 0 });
        }
        for (size_t i = 0; i < ticks; ++i) {
            float x, y;
            if (!(in >> x >> y)) {
                return false;
            }
            r.track.push_back({ x, y });
        }
        return !r.track.empty();
    }
    void benchStart(PlayLayer* layer, PlayerObject* p1) {
        benchLive = {};
        benchLive.startX = p1->getPosition().x;
        benchLive.startY = p1->getPosition().y;
        benchLive.dt = buf.tickLength;
        bool fromCheckpoint = layer->m_isPracticeMode && layer->m_checkpointArray && layer->m_checkpointArray->count() > 0;
        benchRecording = !memoryKey.empty() && !fromCheckpoint;
    }
    void benchInput(bool down) {
        if (benchRecording) {
            benchLive.inputs.push_back({ static_cast<int>(benchLive.track.size()), down });
        }
    }
    void benchStep(PlayerObject* p1) {
        if (benchRecording) {
            benchLive.track.push_back(p1->getPosition());
            if (benchLive.track.size() > 200000) {
                benchRecording = false;
            }
        }
    }
    BenchRun benchBest;
    size_t benchBestTicks = 0;
    bool benchBestDirty = false;
    void benchFinish() {
        if (!benchRecording) {
            return;
        }
        benchRecording = false;
        if (benchLive.track.size() < 240 || benchLive.track.size() <= benchBestTicks) {
            return;
        }
        benchBestTicks = benchLive.track.size();
        benchBest = std::move(benchLive);
        benchLive = {};
        benchBestDirty = true;
    }
    void benchFlush() {
        if (!benchBestDirty || memoryKey.empty()) {
            return;
        }
        benchBestDirty = false;
        std::error_code ec;
        std::filesystem::create_directories(benchFile().parent_path(), ec);
        std::ofstream out(benchFile(), std::ios::trunc);
        out << benchBest.startX << ' ' << benchBest.startY << ' ' << benchBest.dt << ' ' << benchBest.track.size() << ' '
            << benchBest.inputs.size() << '\n';
        for (auto const& [t, d] : benchBest.inputs) {
            out << t << ' ' << (d ? 1 : 0) << '\n';
        }
        for (auto const& p : benchBest.track) {
            out << p.x << ' ' << p.y << '\n';
        }
    }
    void benchCheck(PlayLayer* layer, PlayerObject* p1) {
        benchFlush();
        BenchRun r;
        if (!benchLoad(r)) {
            say("run check: no recorded run for this level yet (play one attempt from the start first)");
            return;
        }
        if (std::abs(r.startX - p1->getPosition().x) > 0.5f || std::abs(r.startY - p1->getPosition().y) > 0.5f) {
            say("run check: this attempt doesn't start where the recording did (start from the beginning, no checkpoint)");
            benchPending = true;
            return;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.haveRealRect = false;
        buf.haveStartRects = false;
        buf.harmless.clear();
        buf.orbClick = false;
        buf.retapAt = -1;
        copyState(buf.ghost1, p1);
        buf.held1 = false;
        buf.auto1 = false;
        buf.autoAt1 = -1;
        size_t next = 0;
        int firstOff = -1;
        float maxErr = 0.f;
        int t = 0;
        char const* end = "end of the recording";
        auto clockStart = std::chrono::steady_clock::now();
        for (; t < static_cast<int>(r.track.size()); ++t) {
            while (next < r.inputs.size() && r.inputs[next].first <= t) {
                pressGhost(buf.ghost1, r.inputs[next].second);
                ++next;
            }
            buf.simTick = t;
            advanceGhost(buf.ghost1, r.dt);
            releaseAutoClicks();
            if (!buf.ghostDied) {
                checkGhost(layer, buf.ghost1, r.dt);
            }
            if (buf.ghostDied) {
                end = "the ghost died";
                break;
            }
            if (buf.ghostHitPortal) {
                end = "a mode portal (predictions stop there)";
                break;
            }
            auto g = buf.ghost1->getPosition();
            float err = std::hypot(g.x - r.track[t].x, g.y - r.track[t].y);
            maxErr = std::max(maxErr, err);
            if (err > 3.f && firstOff < 0) {
                firstOff = t;
            }
        }
        restoreObjects();
        buf.ghost1->stopAllActions();
        buf.simulating = false;
        buf.orbClick = buf.assumeOrbs;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clockStart).count();
        int matched = firstOff < 0 ? t : firstOff;
        lastBench = fmt::format("{}/{} steps ({:.0f}%) match, stopped at {}", matched, r.track.size(),
            100.0 * matched / r.track.size(), end);
        say("run check: {} | max error {:.1f}u{} | {:.0f} ms", lastBench, maxErr,
            std::abs(r.dt - buf.tickLength) > 1e-4f ? " | recorded at another frame rate" : "", ms);
        std::error_code ec;
        std::filesystem::create_directories(cbfModGet()->getSaveDir(), ec);
        std::ofstream(cbfModGet()->getSaveDir() / "run-check-results.txt", std::ios::app)
            << "v0.2" << ' ' << memoryKey << ": " << lastBench << '\n';
    }
    std::string statsText() {
        auto const& s = sessionStats;
        std::string text;
        if (s.saves == 0) {
            text += "No early clicks yet this session.\n";
        }
        else {
            text += fmt::format("<cp>{}</c> {} in <cp>{}</c> attempts, on average <cp>{:.1f}t</c> ({:.2f}f) early.\n",
                s.saves, buf.train ? "early clicks" : "saves", s.attempts, static_cast<double>(s.earlySum) / s.saves, s.earlySum * TICK / s.saves);
            std::vector<std::pair<int, int>> spots(s.savesAt.begin(), s.savesAt.end());
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            text += "Most early clicks at:";
            for (size_t i = 0; i < std::min<size_t>(3, spots.size()); ++i) {
                text += fmt::format(" <cy>{}%</c> ({}x)", spots[i].first, spots[i].second);
            }
            text += "\n";
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && layer->m_levelLength > 0.f) {
            std::map<int, uint32_t> deathsAt;
            for (auto const& [slice, spot] : memory) {
                if (spot.deaths > 0) {
                    deathsAt[std::clamp(static_cast<int>((slice + 0.5f) * SLICE / layer->m_levelLength * 100.f), 0, 100)] += spot.deaths;
                }
            }
            std::vector<std::pair<int, uint32_t>> spots(deathsAt.begin(), deathsAt.end());
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            if (!spots.empty()) {
                text += "Most deaths at:";
                for (size_t i = 0; i < std::min<size_t>(3, spots.size()); ++i) {
                    text += fmt::format(" <cr>{}%</c> ({}x)", spots[i].first, spots[i].second);
                }
                text += "\n";
            }
        }
        if (!lastBench.empty()) {
            text += "Run check: " + lastBench + "\n";
        }
        return text;
    }
    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        ClickTimer timer;
        clickBudget();
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return false;
        }
        bool resuming = resumeNow;
        searchFrom = resuming ? std::max(1, resumeEntry.from) : 1;
        budgetCutK = 0;
        struct SearchFromReset {
            ~SearchFromReset() {
                searchFrom = 1;
            }
        } searchFromReset;
        if (!resuming) {
            for (auto& q : buf.queue) {
                q.resume = false;
            }
        }
        if (buf.autoClicked && isPlayer1 && !resuming) {
            if (!down) {
                buf.autoClicked = false;
            }
            say("AUTO click: your own {} ignored (already clicked for you)", down ? "press" : "release");
            return true;
        }
        auto* level = layer->m_level;
        bool twoPlayer = level && level->m_twoPlayerMode;
        if (twoPlayer && !isPlayer1 && layer->m_gameState.m_isDualMode) {
            return false;
        }
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        if (!resuming) {
            buf.isPlayer1 = isPlayer1;
            buf.realInput = down;
        }
        if (buf.autoReleased && !resuming) {
            buf.autoReleased = false;
            buf.autoDash = false;
            if (!down) {
                return true;
            }
        }
        if (!buf.enabled) {
            buf.appliedInput = down;
            return false;
        }
        if (buf.practiceOnly && !layer->m_isPracticeMode) {
            buf.appliedInput = down;
            return false;
        }
        if (layer->m_isPlatformer) {
            buf.appliedInput = down;
            return false;
        }
        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        float midTick = 0.f;
        if (buf.cbfSync && buf.inRealUpdate && p1 && std::abs(buf.tickDx) > 0.01f && !resuming) {
            midTick = std::clamp((p1->getPosition().x - buf.tickStartX) / buf.tickDx, 0.f, 0.999f);
        }
        buf.firstStep = 1.f - midTick;
        struct FirstStepReset {
            ~FirstStepReset() {
                buf.firstStep = 1.f;
            }
        } firstStepReset;
        int early = midTick > 0.f ? 1 : 0;
        bool anyShip = p1 && (p1->m_isShip || (p2 && p2->m_isShip));
        if (!p1 || p1->m_isDead || (anyShip && !buf.shipBuffer)) {
            resetPending();
            buf.appliedInput = down;
            return false;
        }
        if (down && buf.queue.empty()) {
            buf.releaseShift = 0;
        }
        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);
        int spamGap = st(anyShip && !anyWave ? std::clamp(buf.shipSpamGap, 2, 40) : std::clamp(buf.spamGap, 4, 40));
        if (!resuming) {
            if (buf.sinceInput < spamGap) {
                ++buf.rapid;
            }
            else {
                buf.rapid = 0;
            }
            buf.sinceInput = 0;
        }
        bool anyUfo = p1->m_isBird || (p2 && p2->m_isBird);
        bool anyBall = p1->m_isBall || (p2 && p2->m_isBall);
        bool anySwing = p1->m_isSwing || (p2 && p2->m_isSwing);
        if ((anyWave || anyShip || (anyUfo && buf.ufoSmart) || (anyBall && buf.ballSmart) || (anySwing && buf.swingSmart)) &&
            buf.rapid >= buf.spamStreak) {
            if (buf.explain) {
                say("pass {}: spam ({} fast inputs in a row, gap < {}t)", down ? "press" : "release", buf.rapid, spamGap);
            }
            if (buf.showPath && buf.pathNode) {
                buf.pathNode->clear();
                buf.pathNode->drawDot(p1->getPosition(), 4.f, { 0.6f, 0.6f, 0.65f, 0.9f });
            }
            if (!buf.queue.empty()) {
                int tick = 0;
                for (auto& q : buf.queue) {
                    q.ticksLeft = tick++;
                }
                queueInput(tick, down);
                return true;
            }
            buf.appliedInput = down;
            return false;
        }
        if (!buf.queue.empty() && anyWave && buf.waveV46) {
            auto first = buf.queue.front();
            buf.queue.erase(buf.queue.begin() + 1, buf.queue.end());
            if (down != first.down) {
                queueInput(first.ticksLeft + 1, down);
            }
            return true;
        }
        if (!buf.queue.empty()) {
            int after = buf.queue.back().ticksLeft;
            queueInput(buf.lowLatency && down ? after : std::max(buf.shift - early, after), down);
            return true;
        }
        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) {
                    robot = p2->m_isRobot;
                }
            }
            bool dashing = p1->m_isDashing || (p2 && p2->m_isDashing);
            if (!wave2 && !down && !robot && !dashing && !anyShip) {
                if (buf.releaseShift > 0) {
                    buf.shift = buf.releaseShift;
                    buf.releaseShift = 0;
                    queueInput(buf.shift, false);
                    return true;
                }
                buf.appliedInput = false;
                return false;
            }
        }
        bool anyFlyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(anyFlyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        if (anyWave) {
            horizon = fullWaveHorizon(mini);
        }
        int maxDelay = st(anyWave ? buf.ticksWave : (anyShip ? std::clamp(buf.shipMaxDelay, 0, 16) : buf.ticksAll));
        if (buf.lowLatency) {
            maxDelay = std::min(maxDelay, st(4));
        }
        if (buf.perMode) {
            maxDelay = st(buf.modeTicks[modeIndex(p1, p2)]);
        }
        if (resuming) {
            maxDelay -= resumeEntry.elapsed;
            if (maxDelay < searchFrom) {
                maxDelay = 0;
            }
        }
        if (maxDelay <= 0) {
            if (buf.explain) {
                say("pass {}: max delay is 0 for this mode", down ? "press" : "release");
            }
            buf.appliedInput = down;
            return false;
        }
        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = -1;
        if (down && !anyWave && !anyShip) {
            releaseAfter = anyRobot ? robotHold.pick(-1, 50) : holdEstimate();
        }
        bool grounded = p1->m_isOnGround && (!p2 || p2->m_isOnGround);
        if (buf.explain && down && grounded && !buf.orbAlignGround && !anyWave && !anyShip && buf.orbAlign && buf.smart && !resuming) {
            SimEnd probeEnd;
            buf.skipPress = true;
            buf.orbClick = false;
            int window = st(std::clamp(buf.orbWindow, 1, 64));
            simulate(layer, p1, p2, dual, 0, true, window + 2, -1, window + 2, &probeEnd);
            buf.skipPress = false;
            buf.orbClick = buf.assumeOrbs;
            if (buf.firstOrbTick >= 0 && buf.firstOrbTick <= window) {
                say("orb: you'd touch an orb at t{}, but this press is from the ground ('also from the ground' is off)",
                    buf.firstOrbTick);
            }
        }
        if (down && !anyWave && !anyShip && buf.orbAlign && buf.smart && (!grounded || buf.orbAlignGround) && !resuming) {
            int orbLanded = 0;
            int k = memorySaysSkip(sliceOf(p1)) ? 0 : alignOrb(layer, p1, p2, dual, horizon, releaseAfter, memoryDelay(sliceOf(p1)), maxDelay, &orbLanded);
            if (k > 0 && trainInstead(layer, k, down, "orb")) {
                decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
                buf.appliedInput = down;
                return false;
            }
            if (k > 0) {
                say("ORB {} +{}t ({:g}f) -> {}t{}", buf.orbWasTiming ? "timing" : "press", k, k * TICK, orbLanded,
                    orbLanded < horizon ? fmt::format(" (not a full save: still crashes in {}t)", orbLanded) : std::string());
                noteSave(layer, k);
                decided(layer, p1, p2, dual, k, down, horizon, releaseAfter);
                buf.shift = k;
                queueInput(k - early, down);
                return true;
            }
        }
        buf.sawMover = false;
        buf.trackMovers = buf.avoidMovers || buf.predictMovers;
        SimEnd end;
        bool cubeStyleWave = anyWave && !buf.waveV46;
        bool robotPress = down && anyRobot && !anyWave && buf.robotSmart;
        bool robotRelease = !down && anyRobot && !anyWave && !anyShip && buf.robotSmart;
        bool anySpider = p1->m_isSpider || (p2 && p2->m_isSpider);
        bool spiderPress = down && anySpider && !anyWave && !robotPress && buf.spiderSmart;
        bool ufoPress = down && anyUfo && !anyWave && !anyShip && buf.ufoSmart;
        bool swingPress = down && anySwing && !anyWave && !anyShip && !ufoPress && buf.swingSmart;
        ufoPress = ufoPress || swingPress;
        bool shipInput = anyShip && !anyWave;
        buf.captureKiller = true;
        buf.baseTouchedDown = false;
        buf.killerSeen = false;
        buf.baseEscape = -1;
        int base = cubeStyleWave ? waveScore(layer, p1, p2, dual, 0, down, horizon, horizon, &end)
                 : robotPress    ? robotScore(layer, p1, p2, dual, 0, horizon, horizon, &end)
                 : spiderPress   ? spiderScore(layer, p1, p2, dual, 0, horizon, releaseAfter, horizon, &end)
                 : ufoPress      ? ufoScore(layer, p1, p2, dual, 0, horizon, releaseAfter, horizon, &end)
                 : shipInput     ? shipScore(layer, p1, p2, dual, 0, down, horizon, horizon, &end)
                                 : simulateOrbs(layer, p1, p2, dual, 0, down, horizon, releaseAfter, horizon, &end);
        int baseHold = robotUsedHold;
        bool baseSwitched = buf.lastRunSwitched;
        buf.captureKiller = false;
        buf.trackMovers = false;
        if (baseSwitched && base < horizon && !cubeStyleWave && !shipInput && end != SimEnd::StopSearch && buf.baseEscape < 0) {
            int at = nextClickAt(layer, p1, p2, dual, 0, down, horizon, releaseAfter, base);
            if (at >= 0) {
                buf.baseEscape = at;
                buf.baseCrash = base;
                base = horizon;
            }
        }
        if (buf.killerSeen && (base <= st(3) || (buf.deathLog && base < horizon))) {
            auto real = p1->getObjectRect();
            auto const& g = buf.ghostDeathRect;
            if (buf.killerType >= 0) {
                auto const& k = buf.killerRect;
                say("ghost killed at t{} by obj {} (type {}{}{}) at dx {:.1f} dy {:.1f} size {:.1f}x{:.1f}", buf.killerTick, buf.killerId,
                    buf.killerType, buf.killerNoTouch ? ", no-touch" : "", buf.killerDisabled ? ", disabled" : "",
                    k.getMidX() - real.getMidX(), k.getMidY() - real.getMidY(), k.size.width, k.size.height);
            }
            else {
                say("ghost killed at t{} by no object (crushed / out of bounds / slope)", buf.killerTick);
            }
            say("  hitbox ghost {:.1f}x{:.1f} vs you {:.1f}x{:.1f}, ghost moved dx {:.1f} dy {:.1f}{}{}", g.size.width, g.size.height,
                real.size.width, real.size.height, g.getMidX() - real.getMidX(), g.getMidY() - real.getMidY(),
                buf.predictMovers ? " (moving objects predicted)" : "",
                buf.triggersAhead > 0 ? fmt::format(" ({} move triggers ahead predicted)", buf.triggersAhead) : std::string());
            auto desc = [](PlayerObject* p) {
                if (!p) {
                    return std::string("none");
                }
                char const* mode = p->m_isDart ? "wave" : p->m_isShip ? "ship" : p->m_isBird ? "ufo" : p->m_isBall ? "ball"
                    : p->m_isRobot ? "robot" : p->m_isSpider ? "spider" : p->m_isSwing ? "swing" : "cube";
                return fmt::format("{}{}{}", mode, p->m_controlsDisabled ? " controls-off" : "", p->isVisible() ? "" : " hidden");
            };
            say("  players: P1 {} | P2 {} | dual {} | predicted {}", desc(layer->m_player1), desc(layer->m_player2),
                layer->m_gameState.m_isDualMode, p1 == layer->m_player1 ? "P1" : "P2");
        }
        std::string tickTag = midTick > 0.f ? fmt::format(" @{:.2f}", midTick) : std::string();
        if (base >= horizon) {
            bool reactMode = cubeStyleWave || shipInput || ufoPress || spiderPress;
            if (buf.moreTime && buf.learn && reactMode && buf.baseEscape >= 0 && spotAround(sliceOf(p1)).deaths >= 2) {
                int crash0 = buf.baseCrash;
                int bestK = 0;
                int bestCrash = crash0;
                int hold = (cubeStyleWave || shipInput) ? -1 : releaseAfter;
                int moreMax = (cubeStyleWave || shipInput) ? std::min(maxDelay, st(4)) : maxDelay;
                for (int k = 1; k <= moreMax; ++k) {
                    SimEnd e;
                    if (clickOverTime()) {
                        break;
                    }
                    int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, hold, horizon, &e);
                    if (e == SimEnd::StopSearch) {
                        break;
                    }
                    if (ret >= horizon) {
                        bestK = k;
                        bestCrash = -1;
                        break;
                    }
                    int crashAt = k + ret;
                    if (crashAt >= crash0 + st(8) && crashAt > bestCrash) {
                        bestK = k;
                        bestCrash = crashAt;
                    }
                }
                if (bestK > 0 && !buf.train) {
                    noteSave(layer, bestK);
                    say("MORE TIME {}{} +{}t: crash t{} -> {} (died here {}x)", down ? "press" : "release", tickTag, bestK, crash0,
                        bestCrash < 0 ? std::string("none") : fmt::format("t{}", bestCrash), spotAround(sliceOf(p1)).deaths);
                    decided(layer, p1, p2, dual, bestK, down, horizon, releaseAfter);
                    buf.shift = bestK;
                    queueInput(bestK - early, down);
                    return true;
                }
            }
            if (buf.explain) {
                if (buf.baseEscape >= 0) {
                    say("pass {}{}: safe only if you react by t{} (else crash at t{})", down ? "press" : "release", tickTag,
                        buf.baseEscape, buf.baseCrash);
                }
                else {
                    say("pass {}{}: safe now (survives the {}t look-ahead)", down ? "press" : "release", tickTag, horizon);
                }
            }
            if (robotPress && baseHold > 0) {
                int usual = robotHold.pick(st(20), 50);
                if (std::abs(baseHold - usual) >= st(3)) {
                    if (buf.explain) {
                        say("robot: safe with a {} hold ({}t, you usually hold {}t)", baseHold > usual ? "longer" : "shorter", baseHold,
                            usual);
                    }
                    releaseAfter = baseHold;
                }
            }
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        bool groundPress = down && !anyFlyer && !anyRobot && !anySpider && !anyWave;
        if (groundPress && buf.lateCrash > 0 && base >= st(buf.lateCrash) && buf.baseTouchedDown) {
            if (buf.explain) {
                say("pass press: crash only at {}t, after the jump would land (late crash rule)", base);
            }
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        int slice = sliceOf(p1);
        if (memorySaysSkip(slice)) {
            say("memory: saves here failed before, input left alone");
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            pending.skipped = true;
            buf.appliedInput = down;
            return false;
        }
        int landed = 0;
        int saved = 0;
        bool fromMemory = false;
        crashFixed = false;
        auto scoreAt = [&](int k, int target, SimEnd* e) {
            return cubeStyleWave ? waveScore(layer, p1, p2, dual, k, down, horizon, target, e)
                 : robotPress    ? robotScore(layer, p1, p2, dual, k, horizon, target, e)
                 : spiderPress   ? spiderScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e)
                 : ufoPress      ? ufoScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e)
                 : shipInput     ? shipScore(layer, p1, p2, dual, k, down, horizon, target, e)
                                 : simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, target, e);
        };
        int needed = (groundPress || robotPress || robotRelease || ufoPress) ? horizon : std::min(horizon, base + st(12));
        int learned = memoryDelay(slice);
        if (learned >= searchFrom && learned <= maxDelay && !(anyWave && buf.waveV46)) {
            int target = needed;
            SimEnd e;
            int ret = scoreAt(learned, target, &e);
            if (ret >= target) {
                saved = learned;
                landed = ret;
                fromMemory = true;
            }
        }
        if (saved <= 0) {
            saved = cubeStyleWave ? searchWave(layer, p1, p2, dual, down, horizon, maxDelay, base, &landed)
                  : robotPress    ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return robotScore(layer, p1, p2, dual, k, horizon, target, e);
                    }, true)
                  : robotRelease  ? searchSmallest(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return simulateOrbs(layer, p1, p2, dual, k, false, horizon, -1, target, e);
                    }, searchBudget(), false, true)
                  : spiderPress   ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return spiderScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e);
                    })
                  : shipInput     ? searchSmallest(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return shipScore(layer, p1, p2, dual, k, down, horizon, target, e);
                    })
                  : ufoPress      ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return ufoScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e);
                    }, true)
                                  : searchDelay(layer, p1, p2, dual, down, anyWave, horizon, maxDelay, releaseAfter, base, &landed,
                                        groundPress);
        }
        int resumes = resuming ? resumeEntry.resumes : 0;
        if (saved <= 0 && buf.noHitch && budgetCutK > 0 && budgetCutK < maxDelay && resumes < MAX_RESUMES &&
            !(anyWave && buf.waveV46)) {
            QueuedInput next { std::max(0, 1 - early), down, true, budgetCutK, (resuming ? resumeEntry.elapsed : 0) + 1, resumes + 1 };
            if (buf.explain) {
                say("search goes on next tick (tried up to +{}t of {}t)", budgetCutK + (resuming ? resumeEntry.elapsed : 0),
                    maxDelay + (resuming ? resumeEntry.elapsed : 0));
            }
            buf.shift = 1;
            buf.queue.push_back(next);
            return true;
        }
        if (saved <= 0) {
            say("no save: {}{} dies in {}t (max {}t, best +{}t -> {}t of {}){}", down ? "press" : "release", tickTag, base, maxDelay,
                dbgBestK, dbgBestRet, horizon, !crashFixed ? "" : base < st(30) ? " - too late: waiting only makes it crash sooner" : " - same crash at any timing (next obstacle)");
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        bool robustNote = false;
        if (buf.robustSaves && !(anyWave && buf.waveV46)) {
            int target = needed;
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            auto robust = [&](int k) {
                for (float off : { shift, -shift }) {
                    buf.ghostShiftX = off;
                    SimEnd e;
                    int ret = scoreAt(k, target, &e);
                    buf.ghostShiftX = 0.f;
                    if (ret < target) {
                        return false;
                    }
                }
                return true;
            };
            if (clickMs() < buf.budgetMs && !robust(saved)) {
                for (int k = saved + 1; k <= std::min(maxDelay, saved + st(3)) && !clickOverTime(); ++k) {
                    SimEnd e;
                    int ret = scoreAt(k, target, &e);
                    if (ret >= target && robust(k)) {
                        saved = k;
                        landed = ret;
                        robustNote = true;
                        break;
                    }
                }
            }
        }
        if (buf.avoidMovers) {
            buf.trackMovers = true;
            simulateOrbs(layer, p1, p2, dual, saved, down, horizon, releaseAfter, horizon, &end);
            buf.trackMovers = false;
        }
        if (buf.avoidMovers && buf.sawMover) {
            say("skip +{}t save: moving objects ahead", saved);
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        if (trainInstead(layer, saved, down, swingPress ? "swing" : "")) {
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            buf.appliedInput = down;
            return false;
        }
        if (robotPress) {
            releaseAfter = robotUsedHold;
        }
        noteSave(layer, saved);
        say("SAVED {}{} +{}t ({:g}f)  {}->{}  [max {}]{}{}", down ? "press" : "release", tickTag, saved, saved * TICK, base, landed, maxDelay,
            resuming ? fmt::format("  (search went on {}t)", resumeEntry.elapsed) : std::string(),
            robustNote ? std::string("  (robust)")
            : fromMemory ? std::string("  (memory)")
            : cubeStyleWave && marginUsed ? std::string("  +margin")
            : robotPress ? fmt::format("  hold {}t", robotUsedHold) : std::string());
        decided(layer, p1, p2, dual, saved, down, horizon, releaseAfter);
        buf.shift = saved;
        queueInput(saved - early, down);
        return true;
    }
    void cycleStage(int dir = 1) {
        buf.stage = (buf.stage + dir + STAGE_COUNT) % STAGE_COUNT;
        resetPending();
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && !buf.autoReleased && buf.appliedInput != buf.realInput) {
            sendButton(layer, buf.realInput);
        }
        applyStage();
        readSettings();
        saveProfile();
        cbfModGet()->setSavedValue("stage", buf.stage);
        say("Stage (6) -> {}", stageText());
        refreshAutoTag();
        refreshWatermark();
        refreshPanel();
    }
    void tryAutoRelease(PlayLayer* layer, PlayerObject* p1) {
        if ((!buf.autoRelease && !buf.robotAutoRelease && !buf.dashAutoRelease) || buf.autoReleased || layer->m_isPlatformer) {
            return;
        }
        if (!layer->m_isPracticeMode && !buf.autoNormalRuns) {
            return;
        }
        if (!buf.queue.empty() || !buf.realInput || !buf.appliedInput || !buf.isPlayer1 || !buf.ghost1) {
            return;
        }
        if (layer->m_level && layer->m_level->m_twoPlayerMode) {
            return;
        }
        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        bool dash = p1->m_isDashing || (p2 && p2->m_isDashing);
        bool wave = !dash && (p1->m_isDart || (p2 && p2->m_isDart));
        bool robot = !dash && !wave && (p1->m_isRobot || (p2 && p2->m_isRobot));
        if (p1->m_isDead || !((dash && buf.dashAutoRelease) || (wave && buf.autoRelease) || (robot && buf.robotAutoRelease))) {
            return;
        }
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);
        int window = st(std::clamp(buf.autoWindow, 2, 24));
        buf.sawMover = false;
        buf.skipPress = true;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        int hold = simulate(layer, p1, p2, dual, 0, true, window, -1, window, &end);
        buf.skipPress = false;
        if (hold >= window || buf.ghostHitPortal) {
            return;
        }
        int horizon = wave ? fullWaveHorizon(mini) : st(std::clamp(buf.groundHorizon, 48, 240));
        if (dash && (p1->m_isDart || p1->m_isBird || p1->m_isSwing)) {
            horizon = st(48);
        }
        buf.trackMovers = buf.avoidMovers;
        int release = simulateOrbs(layer, p1, p2, dual, 0, false, horizon, -1, horizon, &end);
        buf.trackMovers = false;
        if (release < horizon && release < hold + st(10)) {
            return;
        }
        if (buf.avoidMovers && buf.sawMover) {
            return;
        }
        buf.autoReleased = true;
        buf.autoDash = dash;
        sendButton(layer, false);
        say("AUTO release{}: holding dies in {}t, letting go lives {}t", dash ? " (dash)" : (robot ? " (robot)" : ""), hold, release);
    }
    void tryAutoClick(PlayLayer* layer, PlayerObject* p1) {
        if (!buf.autoClick || buf.autoReleased || buf.autoClicked || layer->m_isPlatformer) {
            return;
        }
        if (!layer->m_isPracticeMode && !buf.autoNormalRuns) {
            return;
        }
        if (!buf.queue.empty() || buf.realInput || buf.appliedInput || !buf.isPlayer1 || !buf.ghost1) {
            return;
        }
        if (layer->m_level && layer->m_level->m_twoPlayerMode) {
            return;
        }
        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        auto skipMode = [](PlayerObject* p) { return p && (p->m_isDart || p->m_isShip || p->m_isDashing); };
        if (p1->m_isDead || skipMode(p1) || skipMode(p2)) {
            return;
        }
        int window = st(std::clamp(buf.autoWindow, 2, 24));
        buf.sawMover = false;
        buf.skipPress = true;
        buf.orbClick = false;
        SimEnd end;
        int idle = simulate(layer, p1, p2, dual, 0, true, window, -1, window, &end);
        buf.skipPress = false;
        buf.orbClick = buf.assumeOrbs;
        if (idle >= window || buf.ghostHitPortal) {
            return;
        }
        bool flyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(flyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        int hold = std::max(1, holdEstimate());
        buf.trackMovers = buf.avoidMovers;
        int press = simulateOrbs(layer, p1, p2, dual, 0, true, horizon, hold, horizon, &end);
        buf.trackMovers = false;
        if (press < horizon || (buf.avoidMovers && buf.sawMover)) {
            return;
        }
        buf.autoClicked = true;
        buf.autoClickTick = tickCount;
        sendButton(layer, true);
        queueInput(hold, false);
        say("AUTO click: doing nothing dies in {}t, clicking now is safe (hold {}t)", idle, hold);
    }
    void resumeClick(PlayLayer* layer, QueuedInput const& q) {
        resumeNow = true;
        resumeEntry = q;
        bool held = intercept(layer, q.down, buf.isPlayer1);
        resumeNow = false;
        sweepGhostLayer();
        if (!held) {
            sendButton(layer, q.down);
        }
    }
    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (buf.sinceInput <= 998) {
            ++buf.sinceInput;
        }
        if (player->m_isDead) {
            return;
        }
        if ((buf.avoidMovers || buf.predictMovers) && !watched.empty()) {
            updateWatched();
        }
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        if (!buf.shipBuffer && (player->m_isShip || (dual && layer->m_player2->m_isShip))) {
            resetPending();
            buf.autoReleased = false;
            if (buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput);
            }
            return;
        }
        bool autoMode = player->m_isDart || player->m_isRobot || (dual && (layer->m_player2->m_isDart || layer->m_player2->m_isRobot));
        if (buf.autoReleased && !buf.autoDash && !autoMode) {
            buf.autoReleased = false;
        }
        if (buf.autoClicked && !buf.realInput && tickCount - buf.autoClickTick > st(30)) {
            buf.autoClicked = false;
        }
        if (buf.queue.empty()) {
            if (buf.appliedInput != buf.realInput && !buf.autoReleased && !buf.autoClicked) {
                sendButton(layer, buf.realInput);
            }
            tryAutoRelease(layer, player);
            tryAutoClick(layer, player);
            sweepGhostLayer();
            return;
        }
        while (!buf.queue.empty() && buf.queue.front().ticksLeft <= 0) {
            if (buf.queue.front().resume) {
                auto q = buf.queue.front();
                buf.queue.pop_front();
                resumeClick(layer, q);
                continue;
            }
            bool down = buf.queue.front().down;
            buf.queue.pop_front();
            buf.releaseShift = down ? buf.shift : 0;
            if (down != buf.appliedInput) {
                sendButton(layer, down);
            }
        }
        for (auto& q : buf.queue) {
            --q.ticksLeft;
        }
        if (buf.queue.empty()) {
            buf.shift = 0;
        }
    }
}
class $modify(BufEndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();
        benchFinish();
        benchFlush();
        endAttempt();
    }
};
static void addPauseButton(CCNode* pause) {
    (void)pause;
}
class $modify(BufPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        addPauseButton(this);
    }
};
#define CBF_STEP(name)                                  \
    do {                                                \
        static bool cbfStepDone = false;                \
        if (!cbfStepDone) {                             \
            cbfStepDone = true;                         \
            geode::log::info("startup step: {}", name); \
        }                                               \
    } while (false)
namespace {
    void bufLoad() {
        CBF_STEP("loaded: reading settings");
        buf.console = cbfModGet()->getSettingValue<bool>("debug-console");
        buf.showPath = cbfModGet()->getSettingValue<bool>("show-path");
        recoverGlobals();
        if (!cbfModGet()->getSavedValue<bool>("low-latency-v258", false)) {
            cbfModGet()->setSettingValue<bool>("low-latency", true);
            cbfModGet()->setSavedValue("low-latency-v258", true);
        }
        buf.stage = std::clamp(cbfModGet()->getSavedValue<int>("stage", buf.stage), 0, STAGE_COUNT - 1);
        if (!cbfModGet()->getSavedValue<bool>("per-level-v262", false)) {
            cbfModGet()->setSettingValue<bool>("per-level-settings", false);
            cbfModGet()->setSavedValue("per-level-v262", true);
        }
        CBF_STEP("loaded: settings read, applying stage");
        readSettings();
        applyStage();
        CBF_STEP("loaded: stage applied");
#ifdef GEODE_IS_WINDOWS
        if (buf.console) {
            AllocConsole();
            FILE* out = nullptr;
            freopen_s(&out, "CONOUT$", "w", stdout);
            SetConsoleTitleA("Input Buffer - Live Console");
        }
#endif
        if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
            say("Click Between Frames detected: mid-tick sync {}", buf.cbfSync ? "on" : "off");
        }
        if (Loader::get()->isModLoaded("square3ang.cbfextrapolate")) {
            say("CBF Extrapolate detected: visual only, predictions unaffected");
        }
        say("v0.2 (dll) loaded. Stage key cycles: 0.5f -> 1f -> 2f -> 4f -> 6f -> 8f -> custom -> off");
    }
}
class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) {
            buf.ghostDied = true;
            if (buf.captureKiller && !buf.killerSeen) {
                buf.killerSeen = true;
                buf.killerTick = buf.simTick;
                buf.ghostDeathRect = player->getObjectRect();
                if (object) {
                    buf.killerId = object->m_objectID;
                    buf.killerType = static_cast<int>(object->m_objectType);
                    buf.killerNoTouch = object->m_isNoTouch;
                    buf.killerDisabled = object->m_isDisabled;
                    buf.killerRect = object->getObjectRect();
                }
                else {
                    buf.killerId = 0;
                    buf.killerType = -1;
                }
            }
            return;
        }
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (!player || wasDead || !player->m_isDead) {
            return;
        }
        if (buf.appliedInput && buf.owner == this) {
            sendButton(this, false);
        }
        resetPending();
        buf.appliedInput = false;
        buf.autoReleased = false;
        buf.autoClicked = false;
        buf.rapid = 0;
        forgetPress();
        memoryDeath();
        benchFinish();
        refreshHeatmap();
        endAttempt();
        finishDrift(" (died)");
    }
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        readSettings();
        applyStage();
        exportLines.clear();
        logClock = 0;
        loadMemory(level);
        if (statsKey != memoryKey) {
            statsKey = memoryKey;
            sessionStats = {};
        }
        attemptStats = {};
        benchNeedStart = true;
        benchRecording = false;
        benchBest = {};
        benchBestDirty = false;
        benchBestTicks = benchSavedTicks();
        loadProfile();
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);
        buf.ghostLayerBase = buf.ghostLayer->getChildrenCount();
        buf.panel = nullptr;
        buf.panelTitle = nullptr;
        buf.panelLines.clear();
        buf.watermark = nullptr;
        buf.autoTag = nullptr;
        buf.hintLabel = nullptr;
        buf.heatNode = nullptr;
        buf.pathNode = nullptr;
        buf.usedThisAttempt = false;
        buf.realInput = false;
        buf.appliedInput = false;
        buf.isPlayer1 = true;
        buf.sinceInput = 999;
        buf.rapid = 0;
        forgetPress();
        buf.owner = this;
        return true;
    }
    void onQuit() {
        benchFinish();
        benchFlush();
        benchRecording = false;
        saveMemory();
        restoreGlobals();
        buf.hintLabel = nullptr;
        buf.heatNode = nullptr;
        memoryKey.clear();
        dropGhosts();
        PlayLayer::onQuit();
    }
    void resetLevel() {
        triggerChecks.clear();
        attemptStartTick = tickCount;
        benchFinish();
        benchNeedStart = true;
        endAttempt();
        buf.usedThisAttempt = false;
        if (buf.owner != this) {
            PlayLayer::resetLevel();
            return;
        }
        if (buf.appliedInput && !buf.realInput) {
            sendButton(this, false);
        }
        resetPending();
        forgetPress();
        refreshAutoTag();
        if (buf.pathNode) {
            buf.pathNode->clear();
        }
        buf.autoReleased = false;
        buf.autoClicked = false;
        drift.active = false;
        PlayLayer::resetLevel();
        syncHeld(this);
    }
    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
    }
    void togglePracticeMode(bool practiceMode) {
        PlayLayer::togglePracticeMode(practiceMode);
        refreshWatermark();
        refreshAutoTag();
    }
    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }
    void spawnCircle() {
        if (buf.simulating) {
            return;
        }
        PlayLayer::spawnCircle();
    }
};
class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};
class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", HOOK_PRIORITY);
    }
    void addPoint(CCPoint point) {
        if (buf.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};
class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", HOOK_PRIORITY);
    }
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) {
            return;
        }
        if (buf.predictTriggers) {
            noteTriggerFire(layer, this);
        }
        if ((m_objectID == 901 || m_objectID == 1346) && buf.predictTriggers && m_duration > 0.f && firedMoves.size() < 256) {
            firedMoves[this] = tickCount;
            startTriggerCheck(layer, this);
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
    void playTriggerEffect() {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::playTriggerEffect();
    }
    void animationTriggered() {
        if (buf.simulating) {
            return;
        }
        EffectGameObject::animationTriggered();
    }
};
class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", HOOK_PRIORITY);
    }
    void playShineEffect() {
        if (buf.simulating) {
            return;
        }
        GameObject::playShineEffect();
    }
    void playDestroyObjectAnim(GJBaseGameLayer* layer) {
        if (buf.simulating) {
            return;
        }
        GameObject::playDestroyObjectAnim(layer);
    }
};
class $modify(BufRingObject, RingObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("RingObject::powerOnObject", HOOK_PRIORITY);
        (void)self.setHookPriority("RingObject::spawnCircle", HOOK_PRIORITY);
    }
    void powerOnObject(int state) {
        if (buf.simulating) {
            return;
        }
        RingObject::powerOnObject(state);
    }
    void spawnCircle() {
        if (buf.simulating) {
            return;
        }
        RingObject::spawnCircle();
    }
#if !defined(GEODE_IS_IOS) && !defined(GEODE_IS_MACOS)
    void triggerActivated(float xPosition) {
        if (buf.simulating) {
            return;
        }
        RingObject::triggerActivated(xPosition);
    }
#endif
};
namespace {
    void pollKeys();
}
class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::update", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }
    void update(float dt) {
        if (buf.simulating) {
            PlayerObject::update(dt);
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && this == layer->m_player1) {
            ++tickCount;
            if (!triggerChecks.empty()) {
                runTriggerChecks(layer);
            }
            ++logClock;
            if (buf.enabled) {
                buf.usedThisAttempt = true;
            }
            pollKeys();
            if (buf.panelDirty) {
                buf.panelDirty = false;
                refreshPanel();
            }
            if (benchNeedStart) {
                benchNeedStart = false;
                if (benchPending) {
                    benchPending = false;
                    benchCheck(layer, this);
                }
                benchStart(layer, this);
            }
            driftStep(this);
            memoryTick();
            if (buf.enabled) {
                deliverPending(layer, this);
                livePath(layer, this);
            }
        }
        bool isP1 = layer && this == layer->m_player1;
        if (isP1) {
            buf.tickStartX = getPosition().x;
            buf.inRealUpdate = true;
        }
        PlayerObject::update(dt);
        if (isP1) {
            benchStep(this);
            buf.inRealUpdate = false;
            float dx = getPosition().x - buf.tickStartX;
            if (std::abs(dx) > 0.01f) {
                buf.tickDx = dx;
            }
            if (dt > 0.f) {
                float avg = buf.tickLength;
                buf.tickLength = (avg > 0.f && std::abs(dt - avg) < avg * 0.3f) ? avg * 0.9f + dt * 0.1f : dt;
            }
        }
    }
    void incrementJumps() {
        if (buf.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }
    bool ghostEffect() {
        return buf.simulating && isGhost(this);
    }
    void spawnCircle() {
        if (!ghostEffect()) PlayerObject::spawnCircle();
    }
    void spawnDualCircle() {
        if (!ghostEffect()) PlayerObject::spawnDualCircle();
    }
    void spawnPortalCircle(ccColor3B color, float startRadius) {
        if (!ghostEffect()) PlayerObject::spawnPortalCircle(color, startRadius);
    }
    void spawnScaleCircle() {
        if (!ghostEffect()) PlayerObject::spawnScaleCircle();
    }
    void playBumpEffect(int objectType, GameObject* object) {
        if (!ghostEffect()) PlayerObject::playBumpEffect(objectType, object);
    }
    void playSpawnEffect() {
        if (!ghostEffect()) PlayerObject::playSpawnEffect();
    }
    void createFadeOutDartStreak() {
        if (!ghostEffect()) PlayerObject::createFadeOutDartStreak();
    }
    void fadeOutStreak2(float duration) {
        if (!ghostEffect()) PlayerObject::fadeOutStreak2(duration);
    }
    void playDynamicSpiderRun() {
        if (!ghostEffect()) PlayerObject::playDynamicSpiderRun();
    }
    void animatePlatformerJump(float scale) {
        if (!ghostEffect()) PlayerObject::animatePlatformerJump(scale);
    }
    void runBallRotation(float speed) {
        if (!ghostEffect()) PlayerObject::runBallRotation(speed);
    }
    void runBallRotation2() {
        if (!ghostEffect()) PlayerObject::runBallRotation2();
    }
    void runNormalRotation(bool notNormalMode, float speed) {
        if (!ghostEffect()) PlayerObject::runNormalRotation(notNormalMode, speed);
    }
    void toggleGhostEffect(GhostType type) {
        if (!ghostEffect()) PlayerObject::toggleGhostEffect(type);
    }
    void activateStreak() {
        if (!ghostEffect()) PlayerObject::activateStreak();
    }
    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }
    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(this)) {
                return;
            }
            if (buf.orbsUsed.contains(ring)) {
                return;
            }
            snapshotObject(ring);
            double vy = m_yVelocity;
            bool upsideDown = m_isUpsideDown;
            bool dashing = m_isDashing;
            PlayerObject::ringJump(ring, skipCheck);
            if (m_yVelocity != vy || m_isUpsideDown != upsideDown || m_isDashing != dashing) {
                buf.orbsUsed.insert(ring);
                if (buf.orbUsedTick < 0) {
                    buf.orbUsedTick = buf.simTick;
                }
            }
            return;
        }
        PlayerObject::ringJump(ring, skipCheck);
    }
    void startDashing(DashRingObject* ring) {
        if (buf.simulating && isGhost(this)) {
            buf.orbsUsed.insert(ring);
            if (buf.orbUsedTick < 0) {
                buf.orbUsedTick = buf.simTick;
            }
        }
        PlayerObject::startDashing(ring);
    }
};
namespace {
    void pollKeys() {
#ifdef GEODE_IS_WINDOWS
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return;
        }
        HWND focused = GetForegroundWindow();
        if (focused) {
            char cls[32] = {};
            if (!GetClassNameA(focused, cls, sizeof(cls)) || std::string_view(cls) != "GLFW30") {
                return;
            }
        }
        SHORT stage = GetAsyncKeyState(0x36);
        SHORT forget = GetAsyncKeyState(VK_F11);
        static bool stageDown = false;
        static bool forgetDown = false;
        if ((stage & 0x8000) && !stageDown) {
            cycleStage();
        }
        stageDown = (stage & 0x8000) != 0;
        if ((forget & 0x8000) && !forgetDown) {
            memory.clear();
            pending.active = false;
            memoryDirty = true;
            saveMemory();
            say("memory for this level cleared");
        }
        forgetDown = (forget & 0x8000) != 0;
#endif
    }
}
static bool tpsDriving = false;
static double tpsCarry = 0.0;
class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        int tps = static_cast<int>(cbfModGet()->getSettingValue<int64_t>("tps"));
        if (tps <= 240 || tpsDriving || !typeinfo_cast<PlayLayer*>(this)) {
            GJBaseGameLayer::update(dt);
            return;
        }
        double step = 1.0 / tps;
        tpsCarry += dt;
        int steps = static_cast<int>(tpsCarry / step);
        int cap = std::max(1, tps / 15);
        if (steps > cap) {
            steps = cap;
            tpsCarry = 0.0;
        }
        else {
            tpsCarry -= steps * step;
        }
        tpsDriving = true;
        for (int i = 0; i < steps; ++i) {
            GJBaseGameLayer::update(static_cast<float>(step));
        }
        tpsDriving = false;
    }
    double getModifiedDelta(float dt) {
        if (!tpsDriving) {
            return GJBaseGameLayer::getModifiedDelta(dt);
        }
        m_extraDelta = 0.0;
        if (m_resumeTimer > 0) {
            --m_resumeTimer;
            return 0.0;
        }
        return dt;
    }
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", HOOK_PRIORITY);
    }
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player) || !object) {
                return false;
            }
            if (typeinfo_cast<RingObject*>(object)) {
                if (buf.orbsUsed.contains(object)) {
                    return false;
                }
                snapshotObject(object);
                return true;
            }
            if (buf.simUsed.contains(object)) {
                return false;
            }
            snapshotObject(object);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount) {
            relevant.reserve(objectCount);
        }
        uint64_t keep = buf.smart ? (COLLIDING_TYPES | INTERACT_TYPES) : COLLIDING_TYPES;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i];
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                if (!buf.simDisabled.empty() && buf.simDisabled.contains(object)) {
                    continue;
                }
                if (buf.haveRealRect && (bit & HAZARD_TYPES)) {
                    if (!buf.harmless.empty() && buf.harmless.contains(object)) {
                        continue;
                    }
                    if (buf.simTick <= 1) {
                        auto const& r = object->getObjectRect();
                        if (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2)) {
                            buf.harmless.insert(object);
                            continue;
                        }
                    }
                }
                relevant.push_back(object);
                if (buf.trackMovers && (COLLIDING_TYPES & bit)) {
                    watchObject(object);
                }
                continue;
            }
            if (!buf.smart) {
                continue;
            }
            bool stop = (STOP_TYPES & bit) != 0;
            bool size = (SIZE_TYPES & bit) != 0;
            float speed = (stop || size) ? 0.f : speedPortalValue(object->m_objectID);
            if (!stop && !size && speed <= 0.f) {
                continue;
            }
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    auto const& r = object->getObjectRect();
                    if (buf.harmless.contains(object)) {
                        continue;
                    }
                    if (buf.haveStartRects && buf.simTick <= 1 && object->m_objectType != GameObjectType::TeleportOrb &&
                        (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2))) {
                        buf.harmless.insert(object);
                        continue;
                    }
                    if (buf.portalPredict && isGhost(player) &&
                        (ghostTakePortal(player, object->m_objectType) || ghostTeleport(player, object) || ghostSoloPortal(object) ||
                            ghostDualPortal(player, object))) {
                        buf.harmless.insert(object);
                        continue;
                    }
                    buf.ghostHitPortal = true;
                }
                else if (size) {
                    bool mini = object->m_objectType == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                    }
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        static std::vector<GameObject*> woke;
        woke.clear();
        if (!buf.simEnabled.empty() && player) {
            auto pos = player->getPosition();
            for (auto* object : buf.simEnabled) {
                auto type = static_cast<unsigned>(object->m_objectType);
                uint64_t bit = type < 64 ? (1ull << type) : 0;
                if (!(keep & bit) || buf.simDisabled.contains(object) || std::abs(object->m_positionX - pos.x) > 200.f ||
                    std::abs(object->m_positionY - pos.y) > 200.f) {
                    continue;
                }
                if (object->m_isGroupDisabled) {
                    object->m_isGroupDisabled = false;
                    woke.push_back(object);
                }
                relevant.push_back(object);
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
        for (auto* object : woke) {
            object->m_isGroupDisabled = true;
        }
    }
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            auto* p1 = this->m_player1;
            if (down || (p1 && (p1->m_isDart || p1->m_isRobot))) {
                finishDrift("");
            }
            recordRealInput(down, p1);
        }
        if (!buf.simulating && button == 1 && !buf.injecting) {
            buf.simRuns = 0;
            buf.simTicks = 0;
            auto start = std::chrono::steady_clock::now();
            bool held = intercept(this, down, isPlayer1);
            sweepGhostLayer();
            if (buf.perfLog && buf.simRuns > 0) {
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                say("perf: {:.2f} ms, {} runs, {} ghost ticks ({:.1f} us/tick)", ms, buf.simRuns, buf.simTicks,
                    ms * 1000.0 / static_cast<double>(std::max(1L, buf.simTicks)));
            }
            if (held) {
                return;
            }
        }
        if (!buf.simulating && button == 1) {
            benchInput(down);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player)) {
                return;
            }
            snapshotObject(ring);
            buf.touchedOrb = true;
            if (buf.firstOrbTick < 0) {
                buf.firstOrbTick = buf.simTick;
            }
            buf.lastOrbTick = buf.simTick;
            if (buf.orbUsedTick >= 0 && !buf.orbsUsed.contains(ring)) {
                buf.laterOrbTouched = true;
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
        bool autoOrb = buf.orbClick || (buf.orbAfterFirst && buf.orbUsedTick >= 0 && !buf.orbsUsed.contains(ring));
        if (buf.simulating && autoOrb) {
            bool first = player == buf.ghost1;
            bool& held = first ? buf.held1 : buf.held2;
            bool& autoClick = first ? buf.auto1 : buf.auto2;
            int& autoAt = first ? buf.autoAt1 : buf.autoAt2;
            if (buf.orbClickDelay > 0) {
                if (!held && autoAt == -1) {
                    autoAt = buf.simTick + buf.orbClickDelay;
                }
            }
            else if (!held && !autoClick) {
                player->pushButton(PlayerButton::Jump);
                autoClick = true;
            }
        }
    }
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (buf.smart && isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }
    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }
    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            if (auto* objects = this->getGroup(group)) {
                simToggle(objects, activate);
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }
    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            return;
        }
        if (buf.predictTriggers && delay > 0.0 && !ordered && remapKeys.empty() && group > 0 && pendingSpawns.size() < 64) {
            float tickLength = buf.tickLength > 0.001f && buf.tickLength < 2.f ? buf.tickLength : TICK;
            pendingSpawns.push_back({ group, tickCount + static_cast<int>(std::lround(delay * 60.0 / tickLength)) });
        }
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }
    void toggleGroup(int id, bool activate) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::toggleGroup(id, activate);
    }
    void pickupItem(EffectGameObject* object) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::pickupItem(object);
    }
    void updateCounters(int itemId, int value) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::updateCounters(itemId, value);
    }
    void activateItemEditTrigger(ItemTriggerGameObject* object) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::activateItemEditTrigger(object);
    }
    void activateItemCompareTrigger(ItemTriggerGameObject* object, gd::vector<int> const& remapKeys) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::activateItemCompareTrigger(object, remapKeys);
    }
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (buf.simulating) {
            buf.ghostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};
namespace {
    void startupThread() {
        for (int i = 0; i < 600; ++i) {
            if (geode::Loader::get() != nullptr) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        geode::log::info("[input-buffer] injected, starting up");
        bufLoad();
        geode::log::info("[input-buffer] ready");
    }
}
#ifdef GEODE_IS_WINDOWS
extern "C" IMAGE_DOS_HEADER __ImageBase;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        std::thread(startupThread).detach();
    }
    return TRUE;
}
#endif
