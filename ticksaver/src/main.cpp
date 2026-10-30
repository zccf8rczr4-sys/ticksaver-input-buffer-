// TickSaver - buffer de inputs predictivo para Geometry Dash (Geode)
//
// El motor de prediccion esta basado en Afterflow por prsevend (licencia MIT,
// https://github.com/afterflow-gd/afterflow). Ver LICENSE.
//
// Idea general: GD corre la fisica a 240 ticks por segundo. Cada vez que
// apretas o soltas, un "jugador fantasma" invisible copia el estado real y
// simula el futuro con tu input en el tick actual y con retrasos de 1..N
// ticks. Si el input ahora te mata pero uno un poco mas tarde sobrevive, el
// input se retiene y se entrega en el primer tick que sobrevive.

#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/utils/Keyboard.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {

    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK_DT = 0.25f; // dt de un tick de 240 TPS en unidades de GD

    // ------------------------------------------------------------------
    // Tipos de objeto
    // ------------------------------------------------------------------

    constexpr uint64_t typeBit(GameObjectType objectType) {
        return 1ull << static_cast<int>(objectType);
    }

    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) |
        typeBit(GameObjectType::Slope) | typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t HAZARD_TYPES = typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t INTERACTABLE_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    constexpr uint64_t MODE_PORTAL_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    constexpr uint64_t SIZE_PORTAL_TYPES = typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    float speedPortalValue(int objectId) {
        switch (objectId) {
            case 200: return 0.7f;
            case 201: return 0.9f;
            case 202: return 1.1f;
            case 203: return 1.3f;
            case 1334: return 1.6f;
            default: return 0.f;
        }
    }

    // ------------------------------------------------------------------
    // Ajustes del motor (fijos)
    // ------------------------------------------------------------------

    constexpr int HOLD_SAMPLE_COUNT = 8;
    constexpr int SPIDER_LAND_TICKS = 12;
    constexpr int SPAM_STREAK_COUNT = 3;
    constexpr int SPAM_GAP_TICKS = 16;
    constexpr int SHIP_SPAM_GAP_TICKS = 6;
    constexpr int GROUND_HORIZON_TICKS = 130;
    constexpr int WAVE_HORIZON_TICKS = 65;
    constexpr int LATE_CRASH_TICKS = 90;
    constexpr int ORB_WINDOW_TICKS = 6;
    constexpr int WAVE_TURN_MARGIN_TICKS[] = { 4, 8, 12 };
    constexpr int WAVE_MIN_TURN_TICKS = 6;
    constexpr int WAVE_ESCAPE_TICKS = 16;
    constexpr int ROBOT_HOLD_TICKS[] = { 6, 14, 26, 45 };
    constexpr int SPIDER_RETAP_TICKS[] = { 4, 8, 12 };
    constexpr int SPIDER_MIN_RETAP_TICKS = 4;
    constexpr int SAME_CRASH_TRY_COUNT = 4;
    constexpr int SHIP_RECOVERY_TICKS[] = { 8, 16, 24 };
    constexpr int UFO_REFLAP_TICKS[] = { 6, 12, 18 };

    // Niveles de intensidad para la tecla "cambiar intensidad": {todo, wave}
    constexpr int PRESETS[][2] = { { 2, 2 }, { 4, 4 }, { 8, 8 }, { 16, 8 }, { 24, 12 }, { 32, 16 }, { 40, 20 } };
    constexpr int PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

    struct QueuedInput {
        int ticksRemaining;
        bool isDown;
    };

    struct ObjSnapshot {
        GameObject* gameObject;
        bool isActivated;
        EffectGameObject* effectObject;
        bool effectActivated;
        bool byPlayer1;
        bool byPlayer2;
    };

    struct ProbeOrb {
        RingObject* ringObject = nullptr;
        PlayerObject* playerObject = nullptr;
        float touchDistSq = 0.f;
        float closeDistSq = 0.f;
        int touchTick = -1;
        int closeTick = -1;
    };

    struct HoldStats {
        int samples[HOLD_SAMPLE_COUNT] = {};
        int count = 0;
        int next = 0;

        void add(int ticks) {
            samples[next] = std::clamp(ticks, 1, 240);
            next = (next + 1) % HOLD_SAMPLE_COUNT;
            count = std::min(count + 1, HOLD_SAMPLE_COUNT);
        }

        int pick(int fallback, int percentile) const {
            if (count == 0) {
                return fallback;
            }
            int scratch[HOLD_SAMPLE_COUNT];
            std::copy(samples, samples + count, scratch);
            int index = std::min(count - 1, count * percentile / 100);
            std::nth_element(scratch, scratch + index, scratch + count);
            return scratch[index];
        }
    };

    enum class InputKind { Tap, Wave, Robot, Other };
    enum class SimEnd { Ok, StopSearch };

    // ------------------------------------------------------------------
    // Configuracion del usuario (desde los ajustes de Geode)
    // ------------------------------------------------------------------

    struct Config {
        bool enabled = true;
        int maxDelay = 16;
        int maxDelayWave = 8;
        int maxDelayShip = 4;
        bool fixReleases = true;
        bool orbAssist = true;
        bool practiceOnly = false;
        double budgetMs = 8.0;
        bool showHud = true;
        bool adaptive = true;
    } cfg;

    // ------------------------------------------------------------------
    // Estado
    // ------------------------------------------------------------------

    struct State {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        unsigned ghostLayerBaseCount = 0;
        std::unordered_set<GameObject*> simDisabledObjects;

        bool isSimulating = false;
        bool isInjecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool realDown = false;     // lo que el jugador tiene apretado de verdad
        bool appliedDown = false;  // lo que el juego cree que esta apretado
        bool isFirstPlayer = true;

        std::deque<QueuedInput> queue;
        int inputShiftTicks = 0;
        int releaseShiftTicks = 0;

        float tickLength = TICK_DT;
        int ticksSinceInput = 999;
        int rapidInputTicks = 0;

        bool isHeld1 = false, isHeld2 = false;
        bool isAuto1 = false, isAuto2 = false;
        int autoReleaseTick1 = -1, autoReleaseTick2 = -1;
        bool orbClickEnabled = true;
        bool hasTouchedOrb = false;
        bool skipPressHandling = false;
        bool wasOrbTiming = false;

        bool hasRealRect = false;
        CCRect realRect1, realRect2;
        std::unordered_set<GameObject*> harmlessObjects;
        bool baseTouchedDown = false;

        float firstStepScale = 1.f;
        bool isInRealUpdate = false;
        float tickStartX = 0.f;
        float tickDeltaX = 0.f;

        std::unordered_set<GameObject*> usedOrbs;
        int retapTick = -1;

        int simTick = 0;
        int firstOrbTouchTick = -1;
        int orbUseTick = -1;
        RingObject* firstOrbRing = nullptr;
        PlayerObject* firstOrbPlayer = nullptr;
        int orbCloseTick = -1;
        float orbTouchDistSq = 0.f;
        float orbCloseDistSq = 0.f;
        RingObject* intendedOrb = nullptr;
        RingObject* pressOrb = nullptr;
        int inputTickIndex = 0;
        bool isInputDown = false;
        std::vector<ProbeOrb> probes;
        bool isProbing = false;

        std::unordered_set<GameObject*> simUsedObjects;
        std::vector<ObjSnapshot> snapshots;

        // HUD
        CCLabelBMFont* hud = nullptr;
        int savesThisAttempt = 0;
        int savesTotal = 0;
    } state;

    HoldStats tapHolds, robotHolds;
    int currentTick = 0;
    int lastPressTick = -1;
    InputKind lastPressKind = InputKind::Other;
    std::chrono::steady_clock::time_point clickStart;
    double clickBudgetMs = 8.0;
    bool isQuickScore = false;
    int robotUsedHold = -1;
    bool isMarginUsed = false;

    // ------------------------------------------------------------------
    // Utilidades
    // ------------------------------------------------------------------

    float stepScale() {
        float dt = state.tickLength;
        if (!(dt > 0.001f && dt < 2.f)) {
            return 1.f;
        }
        return std::clamp(TICK_DT / dt, 0.25f, 8.f);
    }

    // convierte ticks de 240 TPS a ticks reales (por si se usa physics bypass)
    int st(int ticks240) {
        if (ticks240 <= 0) {
            return ticks240;
        }
        return std::max(1, static_cast<int>(std::lround(ticks240 * stepScale())));
    }

    double clickMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStart).count();
    }

    bool isGhost(PlayerObject* player) {
        return player && (player == state.ghost1 || player == state.ghost2);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing || player->m_isShip;
    }

    void resetPending() {
        state.queue.clear();
        state.inputShiftTicks = 0;
        state.releaseShiftTicks = 0;
    }

    bool activeInThisLevel(PlayLayer* layer) {
        if (!cfg.enabled || !layer) {
            return false;
        }
        if (cfg.practiceOnly && !layer->m_isPracticeMode) {
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Modo adaptativo
    //
    // Cada tipo de modo (suelo, wave, nave) tiene su propia ventana de
    // correccion que se ajusta sola:
    //  - siempre se busca hasta el maximo de los ajustes, pero solo se
    //    aplican correcciones dentro de la ventana actual
    //  - si un click necesitaba mas ticks que la ventana (te moriste y se
    //    podia salvar), la ventana crece para cubrirlo la proxima vez
    //  - si un save cae al borde de la ventana, crece un poco
    //  - si varios saves seguidos necesitan mucho menos, se achica de a 1
    //  - nunca baja de ~2 frames (segun tus FPS), porque los clicks llegan
    //    en frames y no en ticks
    // Todo en ticks de 240 TPS. Lo aprendido se guarda entre sesiones.
    // ------------------------------------------------------------------

    enum class ModeClass { Ground = 0, Wave = 1, Ship = 2 };
    constexpr int MODE_CLASS_COUNT = 3;
    constexpr char const* ADAPT_KEYS[MODE_CLASS_COUNT] = { "adapt-ground", "adapt-wave", "adapt-ship" };
    constexpr int ADAPT_GROW_MARGIN = 2;
    constexpr int ADAPT_CALM_SAVES = 4;

    struct Adaptive {
        int window = 8;
        HoldStats needed;
        int calmSaves = 0;
    } adapt[MODE_CLASS_COUNT];

    int capFor(ModeClass c) {
        switch (c) {
            case ModeClass::Wave: return cfg.maxDelayWave;
            case ModeClass::Ship: return cfg.maxDelayShip;
            default: return cfg.maxDelay;
        }
    }

    int adaptiveFloor(ModeClass c) {
        double interval = CCDirector::get()->getAnimationInterval();
        int ticksPerFrame = interval > 0.0 ? static_cast<int>(std::lround(240.0 * interval)) : 4;
        int floor = std::clamp(ticksPerFrame * 2, 2, 16);
        if (c == ModeClass::Wave) {
            floor = std::max(2, floor / 2); // la wave es muy sensible
        }
        return std::min(floor, capFor(c));
    }

    int effectiveWindow(ModeClass c) {
        int cap = capFor(c);
        if (!cfg.adaptive) {
            return cap;
        }
        return std::clamp(adapt[static_cast<int>(c)].window, std::min(adaptiveFloor(c), cap), std::max(cap, 0));
    }

    void storeAdaptive(ModeClass c) {
        int i = static_cast<int>(c);
        Mod::get()->setSavedValue<int>(ADAPT_KEYS[i], adapt[i].window);
    }

    void loadAdaptive() {
        constexpr int defaults[MODE_CLASS_COUNT] = { 8, 4, 4 };
        for (int i = 0; i < MODE_CLASS_COUNT; ++i) {
            adapt[i].window = std::clamp(Mod::get()->getSavedValue<int>(ADAPT_KEYS[i], defaults[i]), 1, 60);
        }
    }

    void resetAdaptive() {
        for (int i = 0; i < MODE_CLASS_COUNT; ++i) {
            auto c = static_cast<ModeClass>(i);
            adapt[i] = Adaptive {};
            adapt[i].window = adaptiveFloor(c);
            storeAdaptive(c);
        }
    }

    // ------------------------------------------------------------------
    // HUD
    // ------------------------------------------------------------------

    std::string hudText() {
        if (!cfg.enabled) {
            return "TickSaver: OFF";
        }
        auto* layer = PlayLayer::get();
        if (layer && cfg.practiceOnly && !layer->m_isPracticeMode) {
            return "TickSaver: solo practica";
        }
        if (cfg.adaptive) {
            return fmt::format("TickSaver auto {}t / wave {}t  salvados: {}", effectiveWindow(ModeClass::Ground),
                effectiveWindow(ModeClass::Wave), state.savesThisAttempt);
        }
        return fmt::format("TickSaver {}t/{}t  salvados: {}", cfg.maxDelay, cfg.maxDelayWave, state.savesThisAttempt);
    }

    constexpr ccColor3B HUD_GREEN = { 90, 255, 120 };
    constexpr ccColor3B HUD_ORANGE = { 255, 170, 60 };

    void refreshHud(bool flash = false, ccColor3B color = HUD_GREEN) {
        if (!state.hud) {
            return;
        }
        state.hud->setVisible(cfg.showHud);
        state.hud->setString(hudText().c_str());
        if (flash) {
            state.hud->stopAllActions();
            state.hud->setColor(color);
            state.hud->runAction(CCTintTo::create(0.6f, 255, 255, 255));
        }
    }

    // un save que entro en la ventana: ajustar segun lo que hizo falta
    void learnSave(ModeClass c, int needed240) {
        if (!cfg.adaptive) {
            return;
        }
        auto& a = adapt[static_cast<int>(c)];
        int cap = capFor(c);
        int floor = adaptiveFloor(c);
        int w = effectiveWindow(c);
        a.needed.add(needed240);
        if (needed240 >= w - 1) {
            // casi no alcanzo: agrandar un poco
            a.window = std::min(cap, w + ADAPT_GROW_MARGIN);
            a.calmSaves = 0;
        }
        else if (++a.calmSaves >= ADAPT_CALM_SAVES) {
            // varios saves comodos: achicar hacia lo que realmente hace falta
            int target = std::max(floor, a.needed.pick(floor, 90) + ADAPT_GROW_MARGIN);
            if (target < w) {
                a.window = w - 1;
            }
            a.calmSaves = 0;
        }
        storeAdaptive(c);
    }

    // te moriste por un click que se podia salvar con mas ticks: crecer
    void learnMiss(ModeClass c, int needed240) {
        auto& a = adapt[static_cast<int>(c)];
        int cap = capFor(c);
        int w = effectiveWindow(c);
        a.needed.add(needed240);
        a.window = std::min(cap, std::max(w, needed240 + ADAPT_GROW_MARGIN));
        a.calmSaves = 0;
        storeAdaptive(c);
        log::debug("adaptativo: hacian falta {}t, ventana {} -> {}", needed240, w, a.window);
        refreshHud(true, HUD_ORANGE);
    }

    void createHud(PlayLayer* layer) {
        auto winSize = CCDirector::get()->getWinSize();
        auto* label = CCLabelBMFont::create(hudText().c_str(), "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 1.f });
        label->setScale(0.28f);
        label->setOpacity(150);
        label->setPosition({ 4.f, winSize.height - 4.f });
        label->setZOrder(1000);
        label->setID("ticksaver-hud"_spr);
        layer->addChild(label);
        state.hud = label;
        refreshHud();
    }

    // ------------------------------------------------------------------
    // Fantasmas
    // ------------------------------------------------------------------

    void dropGhosts() {
        for (auto** slot : { &state.ghost1, &state.ghost2 }) {
            if (*slot) {
                (*slot)->removeFromParentAndCleanup(true);
                (*slot)->release();
                *slot = nullptr;
            }
        }
        if (state.ghostLayer) {
            state.ghostLayer->removeAllChildrenWithCleanup(true);
            state.ghostLayer->release();
            state.ghostLayer = nullptr;
        }
        state.isSimulating = false;
        state.isInjecting = false;
        state.rapidInputTicks = 0;
        resetPending();
    }

    PlayerObject* makeGhost(PlayLayer* layer) {
        if (!state.ghostLayer) {
            state.ghostLayer = CCLayer::create();
            state.ghostLayer->retain();
        }
        auto* ghost = PlayerObject::create(1, 1, layer, state.ghostLayer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        ghost->m_playEffects = false;
        ghost->unscheduleUpdate();
        ghost->stopAllActions();
        state.ghostLayer->addChild(ghost);
        return ghost;
    }

    // borra efectos que el fantasma haya podido crear durante la simulacion
    void sweepGhostLayer() {
        if (!state.ghostLayer) {
            return;
        }
        auto* children = state.ghostLayer->getChildren();
        while (children && children->count() > state.ghostLayerBaseCount) {
            auto* child = static_cast<CCNode*>(children->lastObject());
            state.ghostLayer->removeChild(child, true);
        }
    }

    void sendButton(PlayLayer* layer, bool isDown) {
        state.isInjecting = true;
        state.appliedDown = isDown;
        layer->handleButton(isDown, 1, state.isFirstPlayer);
        state.isInjecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool isDown) {
        (ghost == state.ghost1 ? state.isHeld1 : state.isHeld2) = isDown;
        if (isDown) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* g, PlayerObject* r) {
        g->setVisible(false);
        g->resetTouchedRings(true);
        g->copyAttributes(r);
        g->m_playEffects = false;
        g->setScale(r->getScale());
        g->setContentSize(r->getContentSize());
        g->m_width = r->m_width;
        g->m_height = r->m_height;
        g->m_gravityMod = r->m_gravityMod;
        g->m_isOnGround = r->m_isOnGround;
        g->m_isUpsideDown = r->m_isUpsideDown;
        g->m_vehicleSize = r->m_vehicleSize;
        g->m_playerSpeed = r->m_playerSpeed;
        g->m_isDashing = r->m_isDashing;
        g->m_lastActivatedPortal = r->m_lastActivatedPortal;

        g->m_holdingButtons = r->m_holdingButtons;
        g->m_jumpBuffered = r->m_jumpBuffered;
        g->m_wasJumpBuffered = r->m_wasJumpBuffered;
        g->m_stateJumpBuffered = r->m_stateJumpBuffered;
        g->m_stateRingJump = r->m_stateRingJump;
        g->m_stateRingJump2 = r->m_stateRingJump2;
        g->m_wasRobotJump = r->m_wasRobotJump;
        g->m_touchedRing = r->m_touchedRing;
        g->m_touchedPad = r->m_touchedPad;
        g->m_touchedGravityPortal = r->m_touchedGravityPortal;

        g->m_isShip = r->m_isShip;
        g->m_isBird = r->m_isBird;
        g->m_isBall = r->m_isBall;
        g->m_isDart = r->m_isDart;
        g->m_isRobot = r->m_isRobot;
        g->m_isSpider = r->m_isSpider;
        g->m_isSwing = r->m_isSwing;
        g->m_isGoingLeft = r->m_isGoingLeft;
        g->m_isSideways = r->m_isSideways;
        g->m_reverseSync = r->m_reverseSync;
        g->m_reverseRelated = r->m_reverseRelated;
        g->m_maybeReverseSpeed = r->m_maybeReverseSpeed;
        g->m_maybeReverseAcceleration = r->m_maybeReverseAcceleration;
        g->m_speedMultiplier = r->m_speedMultiplier;
        g->m_gravity = r->m_gravity;
        g->m_yStart = r->m_yStart;
        g->m_fixGravityBug = r->m_fixGravityBug;
        g->m_fixRobotJump = r->m_fixRobotJump;
        g->m_enable22Changes = r->m_enable22Changes;
        g->m_enableImpulseFix = r->m_enableImpulseFix;
        g->m_totalTime = r->m_totalTime;

        g->m_lastPosition = r->m_lastPosition;
        g->m_yVelocity = r->m_yVelocity;
        g->m_fallSpeed = r->m_fallSpeed;
        g->m_yVelocityBeforeSlope = r->m_yVelocityBeforeSlope;
        g->m_yVelocityRelated = r->m_yVelocityRelated;
        g->m_yVelocityRelated3 = r->m_yVelocityRelated3;
        g->m_groundYVelocity = r->m_groundYVelocity;
        g->m_accelerationOrSpeed = r->m_accelerationOrSpeed;
        g->m_isAccelerating = r->m_isAccelerating;
        g->m_maybeIsFalling = r->m_maybeIsFalling;
        g->m_isOnGround2 = r->m_isOnGround2;
        g->m_isOnGround3 = r->m_isOnGround3;
        g->m_isOnGround4 = r->m_isOnGround4;
        g->m_stateOnGround = r->m_stateOnGround;
        g->m_lastLandTime = r->m_lastLandTime;
        g->m_lastJumpTime = r->m_lastJumpTime;
        g->m_lastFlipTime = r->m_lastFlipTime;
        g->m_lastSpiderFlipTime = r->m_lastSpiderFlipTime;
        g->m_lastGroundedPos = r->m_lastGroundedPos;
        g->m_blackOrbRelated = r->m_blackOrbRelated;
        g->m_padRingRelated = r->m_padRingRelated;
        g->m_ringJumpRelated = r->m_ringJumpRelated;

        g->m_isOnSlope = r->m_isOnSlope;
        g->m_wasOnSlope = r->m_wasOnSlope;
        g->m_slopeVelocity = r->m_slopeVelocity;
        g->m_maybeUpsideDownSlope = r->m_maybeUpsideDownSlope;
        g->m_currentSlope = r->m_currentSlope;
        g->m_currentSlope2 = r->m_currentSlope2;
        g->m_currentPotentialSlope = r->m_currentPotentialSlope;
        g->m_isCollidingWithSlope = r->m_isCollidingWithSlope;
        g->m_collidingWithSlopeId = r->m_collidingWithSlopeId;
        g->m_slopeFlipGravityRelated = r->m_slopeFlipGravityRelated;
        g->m_slopeAngle = r->m_slopeAngle;
        g->m_slopeAngleRadians = r->m_slopeAngleRadians;
        g->m_slopeRotation = r->m_slopeRotation;
        g->m_currentSlopeYVelocity = r->m_currentSlopeYVelocity;
        g->m_isCurrentSlopeTop = r->m_isCurrentSlopeTop;
        g->m_slopeStartTime = r->m_slopeStartTime;
        g->m_slopeEndTime = r->m_slopeEndTime;
        g->m_slopeSlidingMaybeRotated = r->m_slopeSlidingMaybeRotated;
        g->m_stateDartSlide = r->m_stateDartSlide;

        g->m_lastGroundObject = r->m_lastGroundObject;
        g->m_preLastGroundObject = r->m_preLastGroundObject;
        g->m_maybeLastGroundObject = r->m_maybeLastGroundObject;
        g->m_collidedObject = r->m_collidedObject;
        g->m_objectSnappedTo = r->m_objectSnappedTo;
        g->m_snapDistance = r->m_snapDistance;
        g->m_lastCollisionBottom = r->m_lastCollisionBottom;
        g->m_lastCollisionTop = r->m_lastCollisionTop;
        g->m_lastCollisionLeft = r->m_lastCollisionLeft;
        g->m_lastCollisionRight = r->m_lastCollisionRight;
        g->m_collidedTopMinY = r->m_collidedTopMinY;
        g->m_collidedBottomMaxY = r->m_collidedBottomMaxY;
        g->m_collidedLeftMaxX = r->m_collidedLeftMaxX;
        g->m_collidedRightMinX = r->m_collidedRightMinX;

        g->m_dashX = r->m_dashX;
        g->m_dashY = r->m_dashY;
        g->m_dashAngle = r->m_dashAngle;
        g->m_dashStartTime = r->m_dashStartTime;
        g->m_dashRing = r->m_dashRing;
        g->m_dashFireFrame = r->m_dashFireFrame;

        g->m_stateForce = r->m_stateForce;
        g->m_stateForceVector = r->m_stateForceVector;
        g->m_affectedByForces = r->m_affectedByForces;
        g->m_stateBoostX = r->m_stateBoostX;
        g->m_stateBoostY = r->m_stateBoostY;
        g->m_stateNoStickX = r->m_stateNoStickX;
        g->m_stateNoStickY = r->m_stateNoStickY;
        g->m_stateHitHead = r->m_stateHitHead;
        g->m_stateFlipGravity = r->m_stateFlipGravity;
        g->m_stateNoAutoJump = r->m_stateNoAutoJump;
        g->m_stateScale = r->m_stateScale;
        g->m_maybeStateForce2 = r->m_maybeStateForce2;
    }

    InputKind kindOf(PlayerObject* player) {
        if (!player || player->m_isShip) {
            return InputKind::Other;
        }
        if (player->m_isDart) {
            return InputKind::Wave;
        }
        return player->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    // aprende cuanto tiempo solés mantener apretado (para simular el suelte)
    void recordRealInput(bool isDown, PlayerObject* player1) {
        if (isDown) {
            lastPressTick = currentTick;
            lastPressKind = kindOf(player1);
            return;
        }
        if (lastPressTick < 0) {
            return;
        }
        int held = currentTick - lastPressTick;
        lastPressTick = -1;
        switch (lastPressKind) {
            case InputKind::Tap: tapHolds.add(held); break;
            case InputKind::Robot: robotHolds.add(held); break;
            default: break;
        }
    }

    int holdEstimate() {
        return tapHolds.pick(st(12), 50);
    }

    void snapshotObject(GameObject* object) {
        if (!object || state.simUsedObjects.contains(object)) {
            return;
        }
        state.simUsedObjects.insert(object);
        ObjSnapshot snap { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            snap.effectObject = effect;
            snap.effectActivated = effect->m_activated;
            snap.byPlayer1 = effect->m_activatedByPlayer1;
            snap.byPlayer2 = effect->m_activatedByPlayer2;
        }
        state.snapshots.push_back(snap);
    }

    // deja el nivel exactamente como estaba antes de simular
    void restoreObjects() {
        for (auto const& snap : state.snapshots) {
            snap.gameObject->m_isActivated = snap.isActivated;
            if (snap.effectObject) {
                snap.effectObject->m_activated = snap.effectActivated;
                snap.effectObject->m_activatedByPlayer1 = snap.byPlayer1;
                snap.effectObject->m_activatedByPlayer2 = snap.byPlayer2;
            }
        }
        state.snapshots.clear();
        state.simUsedObjects.clear();
        state.usedOrbs.clear();
        state.simDisabledObjects.clear();
    }

    float orbDistSq(PlayerObject* player, RingObject* ring) {
        if (!player || !ring) {
            return 0.f;
        }
        CCPoint a = player->getPosition();
        CCPoint b = ring->getPosition();
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return dx * dx + dy * dy;
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

    void releaseAutoClicks() {
        if (state.isAuto1) {
            if (!state.isHeld1 && state.ghost1) {
                state.ghost1->releaseButton(PlayerButton::Jump);
            }
            state.isAuto1 = false;
        }
        if (state.isAuto2) {
            if (!state.isHeld2 && state.ghost2) {
                state.ghost2->releaseButton(PlayerButton::Jump);
            }
            state.isAuto2 = false;
        }
    }

    // ------------------------------------------------------------------
    // Simulacion
    // Devuelve cuantos ticks sobrevive el fantasma despues del input
    // (horizon = "a salvo").
    // ------------------------------------------------------------------

    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, bool isDown, int horizon,
        int releaseAfter, int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!state.ghost1) {
            return horizon;
        }
        state.isSimulating = true;
        state.ghostDied = false;
        state.ghostHitPortal = false;
        state.firstOrbTouchTick = -1;
        state.orbUseTick = -1;
        state.firstOrbRing = nullptr;
        state.firstOrbPlayer = nullptr;
        state.orbCloseTick = -1;
        state.orbTouchDistSq = 0.f;
        state.orbCloseDistSq = 0.f;
        state.pressOrb = nullptr;
        state.inputTickIndex = delay;
        state.isInputDown = isDown;

        bool second = isDual && state.ghost2 && p2;
        state.hasRealRect = true;
        state.harmlessObjects.clear();
        state.realRect1 = p1->getObjectRect();
        state.realRect2 = second ? p2->getObjectRect() : state.realRect1;

        copyState(state.ghost1, p1);
        if (second) {
            copyState(state.ghost2, p2);
        }
        state.isHeld1 = state.isHeld2 = state.appliedDown;
        state.isAuto1 = state.isAuto2 = false;
        state.autoReleaseTick1 = state.autoReleaseTick2 = -1;

        float dt = state.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK_DT;
        }

        bool flyingRun = isFlying(p1) || (isDual && p2 && isFlying(p2));
        bool spiderRun = p1->m_isSpider || (second && p2->m_isSpider);
        bool ufoRun = p1->m_isBird || (second && p2->m_isBird);
        bool ballRun = p1->m_isBall || (second && p2->m_isBall);

        int total = delay + horizon;
        int survived = 0;
        int airborne = 0;
        int grounded = 0;
        int tick = 0;
        bool died = false;
        bool safe = false;

        for (; tick < total; ++tick) {
            state.simTick = tick;

            if (state.autoReleaseTick1 >= 0 && tick >= state.autoReleaseTick1) {
                if (!state.isHeld1 && state.ghost1->m_touchingRings->count() > 0) {
                    state.ghost1->pushButton(PlayerButton::Jump);
                    state.isAuto1 = true;
                }
                state.autoReleaseTick1 = -2;
            }
            if (second && state.autoReleaseTick2 >= 0 && tick >= state.autoReleaseTick2) {
                if (!state.isHeld2 && state.ghost2->m_touchingRings->count() > 0) {
                    state.ghost2->pushButton(PlayerButton::Jump);
                    state.isAuto2 = true;
                }
                state.autoReleaseTick2 = -2;
            }
            if (tick == delay && !state.skipPressHandling) {
                pressGhost(state.ghost1, isDown);
                if (second) {
                    pressGhost(state.ghost2, isDown);
                }
            }
            if (state.retapTick >= 0) {
                int t = tick - delay - state.retapTick;
                if (t == 0 || t == 1 || t == 5) {
                    pressGhost(state.ghost1, t == 1);
                    if (second) {
                        pressGhost(state.ghost2, t == 1);
                    }
                }
            }
            if (isDown && releaseAfter >= 0 && tick == delay + releaseAfter && state.ghost1->m_isDashing) {
                ++releaseAfter;
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                pressGhost(state.ghost1, !isDown);
                if (second) {
                    pressGhost(state.ghost2, !isDown);
                }
            }

            float stepDt = tick == 0 ? dt * state.firstStepScale : dt;
            advanceGhost(state.ghost1, stepDt);
            if (second) {
                advanceGhost(state.ghost2, stepDt);
            }
            releaseAutoClicks();
            if (state.ghostDied) {
                died = true;
                break;
            }

            checkGhost(layer, state.ghost1, dt);
            if (!state.ghostDied && second) {
                checkGhost(layer, state.ghost2, dt);
            }

            if (state.firstOrbRing && state.firstOrbPlayer && state.orbCloseTick >= 0) {
                float d = orbDistSq(state.firstOrbPlayer, state.firstOrbRing);
                if (d < state.orbCloseDistSq) {
                    state.orbCloseDistSq = d;
                    state.orbCloseTick = state.simTick;
                }
            }
            if (state.isProbing) {
                for (auto& probe : state.probes) {
                    float d = orbDistSq(probe.playerObject, probe.ringObject);
                    if (d < probe.closeDistSq) {
                        probe.closeDistSq = d;
                        probe.closeTick = state.simTick;
                    }
                }
            }
            if (state.ghostDied) {
                died = true;
                break;
            }
            if (state.ghostHitPortal) {
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
            if (flyingRun && !ufoRun) {
                continue;
            }

            bool onGround = state.ghost1->m_isOnGround && (!second || state.ghost2->m_isOnGround);
            if (onGround && airborne > 0) {
                state.baseTouchedDown = true;
            }
            if (!onGround) {
                ++airborne;
                grounded = 0;
                continue;
            }
            if (spiderRun) {
                if (++grounded >= st(SPIDER_LAND_TICKS)) {
                    safe = true;
                    break;
                }
                continue;
            }
            if (airborne < st(ballRun ? 2 : 6)) {
                continue;
            }
            if (grounded > st(ballRun ? 8 : 2)) {
                safe = true;
                break;
            }
            ++grounded;
        }

        if ((died || state.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        state.ghost1->stopAllActions();
        if (state.ghost2) {
            state.ghost2->stopAllActions();
        }
        state.isSimulating = false;
        return safe ? horizon : survived;
    }

    // prueba asumiendo que el jugador usaria un orbe que toca, y si no alcanza,
    // asumiendo lo contrario
    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, bool isDown, int horizon,
        int releaseAfter, int stopAt, SimEnd* end) {
        state.orbClickEnabled = true;
        state.hasTouchedOrb = false;
        int first = simulate(layer, p1, p2, isDual, delay, isDown, horizon, releaseAfter, stopAt, end);
        if (!state.hasTouchedOrb || first >= stopAt) {
            return first;
        }
        SimEnd firstEnd = *end;
        state.orbClickEnabled = false;
        int alt = simulate(layer, p1, p2, isDual, delay, isDown, horizon, releaseAfter, stopAt, end);
        state.orbClickEnabled = true;
        if (alt > first) {
            return alt;
        }
        *end = firstEnd;
        return first;
    }

    int fullWaveHorizon(bool isMini) {
        return st(isMini ? std::max(20, WAVE_HORIZON_TICKS - 15) : WAVE_HORIZON_TICKS);
    }

    float orbSpriteRadius(RingObject* ring) {
        if (!ring) {
            return 16.f;
        }
        CCSize size = ring->getScaledContentSize();
        return std::max(std::max(size.width, size.height) * 0.5f, 12.f);
    }

    // si clickeaste un poco antes de un orbe, busca el tick en que el click lo usa
    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int horizon, int releaseAfter, int* landing) {
        int window = st(ORB_WINDOW_TICKS);
        int found = 0;
        SimEnd end;
        state.orbClickEnabled = false;
        state.wasOrbTiming = false;
        state.intendedOrb = nullptr;

        state.probes.clear();
        state.isProbing = true;
        state.skipPressHandling = true;
        simulate(layer, p1, p2, isDual, 0, true, window + 2, -1, window + 2, &end);
        state.skipPressHandling = false;
        state.isProbing = false;

        int current = simulate(layer, p1, p2, isDual, 0, true, horizon, releaseAfter, horizon, &end);
        RingObject* currentRing = state.pressOrb;
        bool usesOrb = currentRing != nullptr;
        int maxDelay = window + st(4);

        constexpr float CLOSE_FRACTION = 0.60f;
        ProbeOrb* target = nullptr;
        float targetDist = 0.f;
        int targetClose = -1;
        if (!usesOrb) {
            for (auto& probe : state.probes) {
                if (!probe.ringObject || probe.touchDistSq <= 1e-6f) {
                    continue;
                }
                bool alreadyTouching = probe.touchTick == 0;
                if (!alreadyTouching && probe.closeTick > window) {
                    continue;
                }
                if (probe.closeDistSq > probe.touchDistSq * (CLOSE_FRACTION * CLOSE_FRACTION)) {
                    continue;
                }
                if (typeinfo_cast<DashRingObject*>(probe.ringObject)) {
                    float radius = orbSpriteRadius(probe.ringObject);
                    if (probe.closeDistSq > radius * radius * 2.25f) {
                        continue;
                    }
                }
                if (!target || probe.closeDistSq < targetDist) {
                    target = &probe;
                    targetDist = probe.closeDistSq;
                    targetClose = probe.closeTick;
                }
            }
        }

        if (usesOrb && current < horizon) {
            int closeTick = 0;
            for (auto const& probe : state.probes) {
                if (probe.ringObject == currentRing) {
                    closeTick = probe.closeTick;
                    break;
                }
            }
            int low = std::max(1, closeTick - 1);
            int high = std::min(closeTick + st(4), maxDelay);
            int best = -1;
            for (int d = low; d <= high; ++d) {
                int result = simulate(layer, p1, p2, isDual, d, true, horizon, releaseAfter, horizon, &end);
                bool used = state.pressOrb == currentRing;
                if (end == SimEnd::StopSearch) {
                    break;
                }
                if (!used || result < std::min(horizon, current + st(12)) || result <= best) {
                    continue;
                }
                best = result;
                found = d;
                *landing = result;
                state.wasOrbTiming = true;
                if (result >= horizon) {
                    break;
                }
            }
        }
        else if (!usesOrb && target && targetClose >= 0 && targetClose <= window) {
            int best = -1;
            int low = std::max(1, targetClose);
            int high = std::min(targetClose + st(4), maxDelay);
            for (int d = low; d <= high; ++d) {
                int result = simulate(layer, p1, p2, isDual, d, true, horizon, releaseAfter, horizon, &end);
                bool used = state.pressOrb == target->ringObject;
                if (end == SimEnd::StopSearch) {
                    break;
                }
                if (!used || result < std::min(current, horizon)) {
                    continue;
                }
                if (result > best) {
                    best = result;
                    found = d;
                    *landing = result;
                }
                if (result >= horizon) {
                    break;
                }
            }
        }

        if (usesOrb) {
            state.intendedOrb = currentRing;
        }
        else if (target) {
            state.intendedOrb = target->ringObject;
        }
        state.orbClickEnabled = true;
        return found;
    }

    // ------------------------------------------------------------------
    // Puntajes por modo de juego
    // ------------------------------------------------------------------

    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, bool isDown, int horizon, int target, SimEnd* end) {
        int result = simulateOrbs(layer, p1, p2, isDual, delay, isDown, horizon, -1, target, end);
        if (result >= target || *end == SimEnd::StopSearch) {
            return result;
        }
        // si se choca igual, mira si un giro posterior todavia salva
        for (int margin : WAVE_TURN_MARGIN_TICKS) {
            int turn = result - st(margin);
            if (turn < st(WAVE_MIN_TURN_TICKS)) {
                break;
            }
            int run = std::min(horizon, turn + st(WAVE_ESCAPE_TICKS));
            SimEnd turnEnd;
            if (simulateOrbs(layer, p1, p2, isDual, delay, isDown, run, turn, run, &turnEnd) >= run) {
                return horizon;
            }
        }
        return result;
    }

    int robotScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, int horizon, int target, SimEnd* end) {
        int usual = robotHolds.pick(st(20), 50);
        int best = simulateOrbs(layer, p1, p2, isDual, delay, true, horizon, usual, target, end);
        robotUsedHold = usual;
        if (best >= target || *end == SimEnd::StopSearch || isQuickScore) {
            return best;
        }
        for (int hold : ROBOT_HOLD_TICKS) {
            if (std::abs(st(hold) - usual) < st(3)) {
                continue;
            }
            SimEnd trialEnd;
            int trial = simulateOrbs(layer, p1, p2, isDual, delay, true, horizon, st(hold), target, &trialEnd);
            if (trial > best) {
                best = trial;
                robotUsedHold = st(hold);
            }
            if (best >= target) {
                break;
            }
        }
        return best;
    }

    int retapScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end, int const* margins, int marginCount, int minTick, int runExtra) {
        int base = simulateOrbs(layer, p1, p2, isDual, delay, true, horizon, releaseAfter, target, end);
        if (base >= target || *end == SimEnd::StopSearch || isQuickScore) {
            return base;
        }
        for (int i = 0; i < marginCount; ++i) {
            int retap = base - st(margins[i]);
            if (retap < st(minTick)) {
                break;
            }
            int run = std::min(horizon, retap + st(runExtra));
            state.retapTick = retap;
            SimEnd retapEnd;
            int retapped = simulateOrbs(layer, p1, p2, isDual, delay, true, run, releaseAfter, run, &retapEnd);
            state.retapTick = -1;
            if (retapped >= run) {
                return horizon;
            }
        }
        return base;
    }

    int spiderScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, int horizon, int releaseAfter, int target, SimEnd* end) {
        return retapScore(layer, p1, p2, isDual, delay, horizon, releaseAfter, target, end, SPIDER_RETAP_TICKS,
            static_cast<int>(std::size(SPIDER_RETAP_TICKS)), SPIDER_MIN_RETAP_TICKS, 16);
    }

    int ufoScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, int horizon, int releaseAfter, int target, SimEnd* end) {
        return retapScore(layer, p1, p2, isDual, delay, horizon, releaseAfter, target, end, UFO_REFLAP_TICKS,
            static_cast<int>(std::size(UFO_REFLAP_TICKS)), 4, 20);
    }

    int shipScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, int delay, bool isDown, int horizon, int target, SimEnd* end) {
        int result = simulateOrbs(layer, p1, p2, isDual, delay, isDown, horizon, -1, target, end);
        if (result >= target || *end == SimEnd::StopSearch) {
            return result;
        }
        for (int margin : SHIP_RECOVERY_TICKS) {
            int turn = result - st(margin);
            if (turn < st(WAVE_MIN_TURN_TICKS)) {
                break;
            }
            int run = std::min(horizon, turn + st(WAVE_ESCAPE_TICKS));
            SimEnd turnEnd;
            if (simulateOrbs(layer, p1, p2, isDual, delay, isDown, run, turn, run, &turnEnd) >= run) {
                return horizon;
            }
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Busqueda del retraso minimo que sobrevive
    // ------------------------------------------------------------------

    bool sameCrash(int delay, int result, int baseDelay, int& streak) {
        if (streak >= 0 && std::abs(delay + result - baseDelay) <= 1) {
            ++streak;
        }
        else {
            streak = -1;
        }
        return streak >= SAME_CRASH_TRY_COUNT;
    }

    using ScoreFn = std::function<int(int, int, SimEnd*)>;

    int searchSmallest(int horizon, int maxDelay, int baseDelay, int* landing, ScoreFn const& score, double budget = -1.0, bool fullSearch = false) {
        if (budget < 0.0) {
            budget = clickBudgetMs;
        }
        auto start = std::chrono::steady_clock::now();
        int target = fullSearch ? horizon : std::min(horizon, baseDelay + st(12));
        int streak = 0;
        for (int d = 1; d <= maxDelay; ++d) {
            SimEnd end;
            int result = score(d, target, &end);
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (result >= target) {
                *landing = result;
                return d;
            }
            if (sameCrash(d, result, baseDelay, streak)) {
                return 0;
            }
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budget) {
                return 0;
            }
        }
        return 0;
    }

    int searchTwoPass(int horizon, int maxDelay, int baseDelay, int* landing, ScoreFn const& score, bool fullSearch = false) {
        isQuickScore = true;
        int d = searchSmallest(horizon, maxDelay, baseDelay, landing, score, clickBudgetMs * 0.5, fullSearch);
        isQuickScore = false;
        if (d > 0) {
            return d;
        }
        double remaining = clickBudgetMs - clickMs();
        if (remaining < 0.5) {
            return 0;
        }
        return searchSmallest(horizon, maxDelay, baseDelay, landing, score, remaining, fullSearch);
    }

    int searchWave(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, bool isDown, int horizon, int maxDelay, int baseDelay, int* landing) {
        isMarginUsed = false;
        auto start = std::chrono::steady_clock::now();
        int target = std::min(horizon, baseDelay + st(12));
        for (int d = 1; d <= maxDelay; ++d) {
            SimEnd end;
            int result = waveScore(layer, p1, p2, isDual, d, isDown, horizon, target, &end);
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (result >= target) {
                *landing = result;
                // si 1 y 2 ticks mas tarde tambien salvan, usar +1 para tener margen
                if (d + 2 <= maxDelay) {
                    SimEnd e1, e2;
                    if (waveScore(layer, p1, p2, isDual, d + 1, isDown, horizon, target, &e1) >= target &&
                        waveScore(layer, p1, p2, isDual, d + 2, isDown, horizon, target, &e2) >= target) {
                        isMarginUsed = true;
                        return d + 1;
                    }
                }
                return d;
            }
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > clickBudgetMs) {
                return 0;
            }
        }
        return 0;
    }

    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool isDual, bool isDown, bool hasWave, int horizon, int maxDelay,
        int releaseAfter, int baseDelay, int* landing, bool fullSearch = false, int innerMax = -1) {
        // innerMax: hasta donde probar "un poco mas tarde" cuando ya hay un save parcial
        if (innerMax < 0) {
            innerMax = maxDelay;
        }
        auto start = std::chrono::steady_clock::now();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > clickBudgetMs;
        };

        int target = fullSearch ? horizon : std::min(horizon, baseDelay + st(12));
        int streak = 0;
        int waveBestDelay = 0;
        int waveBestResult = baseDelay;
        for (int d = 1; d <= maxDelay; ++d) {
            SimEnd end;
            int result = simulateOrbs(layer, p1, p2, isDual, d, isDown, horizon, releaseAfter, hasWave ? horizon : target, &end);
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (hasWave) {
                if (result >= horizon && result - baseDelay >= d * 2 + 6) {
                    waveBestDelay = d;
                    waveBestResult = result;
                    break;
                }
                if (result >= baseDelay + d * 2 + 10 && result > waveBestResult + 2) {
                    waveBestDelay = d;
                    waveBestResult = result;
                    if (result >= horizon) {
                        break;
                    }
                }
            }
            else if (result < target && sameCrash(d, result, baseDelay, streak)) {
                break;
            }
            else if (result >= target) {
                SimEnd fullEnd;
                if (result < horizon && simulateOrbs(layer, p1, p2, isDual, d, isDown, horizon, releaseAfter, horizon, &fullEnd) < horizon) {
                    for (int inner = d + 1; inner <= std::min(maxDelay, innerMax) && !overBudget(); ++inner) {
                        SimEnd innerEnd;
                        int innerResult = simulateOrbs(layer, p1, p2, isDual, inner, isDown, horizon, releaseAfter, horizon, &innerEnd);
                        if (innerEnd == SimEnd::StopSearch) {
                            break;
                        }
                        if (innerResult >= horizon) {
                            *landing = innerResult;
                            return inner;
                        }
                    }
                }
                *landing = result;
                return d;
            }
            if (overBudget()) {
                break;
            }
        }
        if (hasWave && waveBestDelay > 0) {
            *landing = waveBestResult;
            return waveBestDelay;
        }
        return 0;
    }

    void queueInput(int ticks, bool isDown) {
        state.queue.push_back({ ticks, isDown });
    }

    void noteSave(bool isDown, int delay, int base, int landing) {
        ++state.savesThisAttempt;
        ++state.savesTotal;
        log::debug("{} salvado +{}t ({} -> {})", isDown ? "click" : "suelte", delay, base, landing);
        refreshHud(true);
    }

    // ------------------------------------------------------------------
    // Interceptar un input real. Devuelve true si se retiene.
    // ------------------------------------------------------------------

    bool intercept(GJBaseGameLayer* baseLayer, bool isDown, bool isFirstPlayer) {
        clickStart = std::chrono::steady_clock::now();
        clickBudgetMs = cfg.budgetMs;
        {
            // si el juego ya va lento, no gastar tanto tiempo
            auto* director = CCDirector::get();
            double frameInterval = director->getAnimationInterval() * 1000.0;
            double frameTime = director->getDeltaTime() * 1000.0;
            if (frameInterval > 0.0 && frameTime > std::max(frameInterval * 2.0, 20.0)) {
                clickBudgetMs = std::min(clickBudgetMs, 4.0);
            }
        }

        auto* layer = PlayLayer::get();
        if (!layer) {
            return false;
        }
        if (layer->m_isPaused) {
            state.appliedDown = isDown;
            return false;
        }

        auto* level = layer->m_level;
        bool twoPlayer = level && level->m_twoPlayerMode;
        if (twoPlayer && !isFirstPlayer && layer->m_gameState.m_isDualMode) {
            return false;
        }
        bool isDual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;

        state.isFirstPlayer = isFirstPlayer;
        state.realDown = isDown;
        state.intendedOrb = nullptr;
        if (!activeInThisLevel(layer) || layer->m_isPlatformer) {
            state.appliedDown = isDown;
            return false;
        }

        auto* p1 = baseLayer->m_player1;
        auto* p2 = isDual ? layer->m_player2 : nullptr;

        // compatibilidad con Click Between Frames: el click puede caer a mitad de tick
        float midTick = 0.f;
        if (state.isInRealUpdate && p1 && std::abs(state.tickDeltaX) > 0.01f) {
            midTick = std::clamp((p1->getPosition().x - state.tickStartX) / state.tickDeltaX, 0.f, 0.999f);
        }
        state.firstStepScale = 1.f - midTick;
        struct ResetStep {
            ~ResetStep() { state.firstStepScale = 1.f; }
        } resetStep;
        int earlyTicks = midTick > 0.f ? 1 : 0;

        bool hasShip = p1 && (p1->m_isShip || (p2 && p2->m_isShip));
        if (!p1 || p1->m_isDead || (hasShip && cfg.maxDelayShip <= 0)) {
            resetPending();
            state.appliedDown = isDown;
            return false;
        }

        bool orbUnderClick = p1->m_touchingRings && p1->m_touchingRings->count() > 0;
        if (isDual && p2 && p2->m_touchingRings && p2->m_touchingRings->count() > 0) {
            orbUnderClick = true;
        }
        // si ya estas tocando un orbe, el click es intencional: no tocar
        if (isDown && orbUnderClick && state.queue.empty()) {
            state.ticksSinceInput = 0;
            state.appliedDown = isDown;
            return false;
        }

        if (isDown && state.queue.empty()) {
            state.releaseShiftTicks = 0;
        }

        bool p1Wave = p1->m_isDart;
        bool hasWave = p1Wave || (p2 && p2->m_isDart);
        bool isMini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        // spam rapido (wave/ship/ufo...): no predecir cada click, solo mantener el orden
        int spamGap = st(hasShip && !hasWave ? SHIP_SPAM_GAP_TICKS : SPAM_GAP_TICKS);
        if (state.ticksSinceInput < spamGap) {
            ++state.rapidInputTicks;
        }
        else {
            state.rapidInputTicks = 0;
        }
        state.ticksSinceInput = 0;
        bool hasUfo = p1->m_isBird || (p2 && p2->m_isBird);
        bool hasBall = p1->m_isBall || (p2 && p2->m_isBall);
        bool hasSwing = p1->m_isSwing || (p2 && p2->m_isSwing);
        if ((hasWave || hasShip || hasUfo || hasBall || hasSwing) && state.rapidInputTicks >= SPAM_STREAK_COUNT) {
            if (!state.queue.empty()) {
                for (size_t i = 0; i < state.queue.size(); ++i) {
                    state.queue[i].ticksRemaining = static_cast<int>(i);
                }
                if (state.queue.back().isDown != isDown) {
                    queueInput(static_cast<int>(state.queue.size()), isDown);
                }
                state.inputShiftTicks = 0;
                return true;
            }
            state.appliedDown = isDown;
            return false;
        }

        // ya hay inputs en cola: este va detras, con el mismo corrimiento
        if (!state.queue.empty()) {
            int after = state.queue.back().ticksRemaining;
            queueInput(std::max(state.inputShiftTicks - earlyTicks, after), isDown);
            return true;
        }

        if (!p1Wave) {
            bool hasRobot = p1->m_isRobot;
            bool secondWave = false;
            if (p2 && !hasRobot) {
                secondWave = p2->m_isDart;
                if (!secondWave) {
                    hasRobot = p2->m_isRobot;
                }
            }
            bool dashing = p1->m_isDashing || (p2 && p2->m_isDashing);
            // sueltes en cubo/bola/etc: si el click se habia retrasado, retrasar el suelte igual
            if (!secondWave && !isDown && !hasRobot && !dashing && !hasShip) {
                if (state.releaseShiftTicks > 0) {
                    state.inputShiftTicks = state.releaseShiftTicks;
                    state.releaseShiftTicks = 0;
                    queueInput(state.inputShiftTicks, false);
                    return true;
                }
                state.appliedDown = false;
                return false;
            }
        }

        if (!isDown && !cfg.fixReleases) {
            state.appliedDown = false;
            return false;
        }

        bool hasFlyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(hasFlyer ? 48 : GROUND_HORIZON_TICKS);
        if (hasWave) {
            horizon = fullWaveHorizon(isMini);
        }
        // ventana actual (adaptativa o fija) y hasta donde buscar
        ModeClass modeClass = hasWave ? ModeClass::Wave : (hasShip ? ModeClass::Ship : ModeClass::Ground);
        int windowTicks = st(effectiveWindow(modeClass));
        int maxDelay = cfg.adaptive ? st(capFor(modeClass)) : windowTicks;
        if (windowTicks <= 0 || maxDelay <= 0) {
            state.appliedDown = isDown;
            return false;
        }

        bool hasAnyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = -1;
        if (isDown && !hasWave && !hasShip) {
            releaseAfter = hasAnyRobot ? robotHolds.pick(-1, 50) : holdEstimate();
        }

        bool onGround = p1->m_isOnGround && (!p2 || p2->m_isOnGround);
        if (isDown && !hasWave && !hasShip && cfg.orbAssist && !onGround) {
            int orbLanding = 0;
            int d = alignOrb(layer, p1, p2, isDual, horizon, releaseAfter, &orbLanding);
            if (d > 0) {
                state.inputShiftTicks = d;
                queueInput(d - earlyTicks, isDown);
                noteSave(isDown, d, 0, orbLanding);
                return true;
            }
        }

        SimEnd end;
        bool robotPress = isDown && hasAnyRobot && !hasWave;
        bool hasSpider = p1->m_isSpider || (p2 && p2->m_isSpider);
        bool spiderPress = isDown && hasSpider && !hasWave && !robotPress;
        bool ufoPress = isDown && (hasUfo || hasSwing) && !hasWave && !hasShip;
        bool shipInput = hasShip && !hasWave;

        state.baseTouchedDown = false;
        int baseDelay = hasWave ? waveScore(layer, p1, p2, isDual, 0, isDown, horizon, horizon, &end)
            : robotPress  ? robotScore(layer, p1, p2, isDual, 0, horizon, horizon, &end)
            : spiderPress ? spiderScore(layer, p1, p2, isDual, 0, horizon, releaseAfter, horizon, &end)
            : ufoPress    ? ufoScore(layer, p1, p2, isDual, 0, horizon, releaseAfter, horizon, &end)
            : shipInput   ? shipScore(layer, p1, p2, isDual, 0, isDown, horizon, horizon, &end)
                          : simulateOrbs(layer, p1, p2, isDual, 0, isDown, horizon, releaseAfter, horizon, &end);

        // el input sin tocar ya es seguro
        if (baseDelay >= horizon) {
            state.appliedDown = isDown;
            return false;
        }

        // el choque esta muy lejos y despues de aterrizar: no es culpa de este click
        bool groundPress = isDown && !hasFlyer && !hasAnyRobot && !hasSpider && !hasWave;
        if (groundPress && baseDelay >= st(LATE_CRASH_TICKS) && state.baseTouchedDown) {
            state.appliedDown = isDown;
            return false;
        }

        int landing = 0;
        int saved = hasWave ? searchWave(layer, p1, p2, isDual, isDown, horizon, maxDelay, baseDelay, &landing)
            : robotPress ? searchTwoPass(horizon, maxDelay, baseDelay, &landing, [&](int d, int target, SimEnd* e) {
                  return robotScore(layer, p1, p2, isDual, d, horizon, target, e);
              }, true)
            : spiderPress ? searchTwoPass(horizon, maxDelay, baseDelay, &landing, [&](int d, int target, SimEnd* e) {
                  return spiderScore(layer, p1, p2, isDual, d, horizon, releaseAfter, target, e);
              })
            : shipInput ? searchSmallest(horizon, maxDelay, baseDelay, &landing, [&](int d, int target, SimEnd* e) {
                  return shipScore(layer, p1, p2, isDual, d, isDown, horizon, target, e);
              })
            : ufoPress ? searchTwoPass(horizon, maxDelay, baseDelay, &landing, [&](int d, int target, SimEnd* e) {
                  return ufoScore(layer, p1, p2, isDual, d, horizon, releaseAfter, target, e);
              }, true)
            : searchDelay(layer, p1, p2, isDual, isDown, hasWave, horizon, maxDelay, releaseAfter, baseDelay, &landing, groundPress,
                  windowTicks);

        if (saved <= 0) {
            state.appliedDown = isDown;
            return false;
        }

        // en la wave el margen de +1 puede pasarse justo del borde: usar el minimo
        if (hasWave && isMarginUsed && saved == windowTicks + 1) {
            saved = windowTicks;
        }
        int needed240 = std::max(1, static_cast<int>(std::lround(saved / stepScale())));

        // se podia salvar, pero con mas ticks que la ventana actual: aprender y no tocar
        if (saved > windowTicks) {
            learnMiss(modeClass, needed240);
            state.appliedDown = isDown;
            return false;
        }

        // seguridad: no cambiar que orbe usa el click
        if (isDown) {
            SimEnd verifyEnd;
            int verifyHorizon = st(8);
            state.retapTick = -1;
            state.orbClickEnabled = false;
            simulate(layer, p1, p2, isDual, saved, isDown, verifyHorizon, -1, verifyHorizon, &verifyEnd);
            state.orbClickEnabled = true;
            if (state.pressOrb != state.intendedOrb) {
                state.appliedDown = isDown;
                return false;
            }
        }

        state.inputShiftTicks = saved;
        queueInput(saved - earlyTicks, isDown);
        learnSave(modeClass, needed240);
        noteSave(isDown, saved, baseDelay, landing);
        return true;
    }

    // entrega los inputs retenidos en su tick
    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (layer->m_isPaused) {
            return;
        }
        if (state.ticksSinceInput <= 998) {
            ++state.ticksSinceInput;
        }
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool isDual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        if (cfg.maxDelayShip <= 0 && (player->m_isShip || (isDual && layer->m_player2->m_isShip))) {
            resetPending();
            if (state.appliedDown != state.realDown) {
                sendButton(layer, state.realDown);
            }
            return;
        }

        if (state.queue.empty()) {
            if (state.appliedDown != state.realDown) {
                sendButton(layer, state.realDown);
            }
            sweepGhostLayer();
            return;
        }

        while (!state.queue.empty() && state.queue.front().ticksRemaining <= 0) {
            bool isDown = state.queue.front().isDown;
            state.queue.pop_front();
            state.releaseShiftTicks = isDown ? state.inputShiftTicks : 0;
            if (isDown != state.appliedDown) {
                sendButton(layer, isDown);
            }
        }
        for (auto& input : state.queue) {
            --input.ticksRemaining;
        }
        if (state.queue.empty()) {
            state.inputShiftTicks = 0;
        }
    }

    // al apagar el mod o cambiar ajustes, sincronizar lo que el juego cree apretado
    void syncButtonState() {
        resetPending();
        auto* layer = PlayLayer::get();
        if (layer && state.appliedDown != state.realDown) {
            sendButton(layer, state.realDown);
        }
    }

    // ------------------------------------------------------------------
    // Ajustes y teclas
    // ------------------------------------------------------------------

    void readConfig() {
        auto* mod = Mod::get();
        cfg.enabled = mod->getSettingValue<bool>("enabled");
        cfg.maxDelay = static_cast<int>(mod->getSettingValue<int64_t>("max-delay"));
        cfg.maxDelayWave = static_cast<int>(mod->getSettingValue<int64_t>("max-delay-wave"));
        cfg.maxDelayShip = static_cast<int>(mod->getSettingValue<int64_t>("max-delay-ship"));
        cfg.fixReleases = mod->getSettingValue<bool>("fix-releases");
        cfg.orbAssist = mod->getSettingValue<bool>("orb-assist");
        cfg.practiceOnly = mod->getSettingValue<bool>("practice-only");
        cfg.budgetMs = mod->getSettingValue<double>("budget-ms");
        cfg.showHud = mod->getSettingValue<bool>("show-hud");
        cfg.adaptive = mod->getSettingValue<bool>("adaptive");
    }

    void onAdaptiveChanged() {
        bool was = cfg.adaptive;
        readConfig();
        if (cfg.adaptive && !was) {
            resetAdaptive(); // empezar a aprender de cero
        }
        syncButtonState();
        refreshHud();
    }

    void toggleAdaptive() {
        Mod::get()->setSettingValue<bool>("adaptive", !cfg.adaptive);
        onAdaptiveChanged();
        refreshHud(true);
        Notification::create(cfg.adaptive ? "Modo adaptativo: ON (aprendiendo)" : "Modo adaptativo: OFF (ventana fija)",
            NotificationIcon::Info, 1.2f)->show();
    }

    void onConfigChanged() {
        readConfig();
        syncButtonState();
        refreshHud();
    }

    void toggleEnabled() {
        Mod::get()->setSettingValue<bool>("enabled", !cfg.enabled);
        readConfig();
        syncButtonState();
        refreshHud(true);
        Notification::create(cfg.enabled ? "TickSaver activado" : "TickSaver desactivado",
            cfg.enabled ? NotificationIcon::Success : NotificationIcon::Info, 1.0f)->show();
    }

    void cyclePreset() {
        int next = 0;
        for (int i = 0; i < PRESET_COUNT; ++i) {
            if (PRESETS[i][0] == cfg.maxDelay && PRESETS[i][1] == cfg.maxDelayWave) {
                next = (i + 1) % PRESET_COUNT;
                break;
            }
        }
        Mod::get()->setSettingValue<int64_t>("max-delay", PRESETS[next][0]);
        Mod::get()->setSettingValue<int64_t>("max-delay-wave", PRESETS[next][1]);
        readConfig();
        syncButtonState();
        refreshHud(true);
        Notification::create(fmt::format("{} {}/{}: {} ticks ({:g} frames a 60 FPS)", cfg.adaptive ? "Maximo" : "Intensidad",
                                 next + 1, PRESET_COUNT, PRESETS[next][0], PRESETS[next][0] / 4.f),
            NotificationIcon::Info, 1.2f)->show();
    }
}

$on_mod(Loaded) {
    readConfig();
    loadAdaptive();
    listenForSettingChanges<bool>("adaptive", [](bool) { onAdaptiveChanged(); });
    listenForKeybindSettingPresses("keybind-adaptive", [](Keybind const&, bool isDown, bool isRepeat, double) {
        if (isDown && !isRepeat) {
            toggleAdaptive();
        }
        return true;
    });
    for (auto key : { "enabled", "fix-releases", "orb-assist", "practice-only", "show-hud" }) {
        listenForSettingChanges<bool>(key, [](bool) { onConfigChanged(); });
    }
    for (auto key : { "max-delay", "max-delay-wave", "max-delay-ship" }) {
        listenForSettingChanges<int64_t>(key, [](int64_t) { onConfigChanged(); });
    }
    listenForSettingChanges<double>("budget-ms", [](double) { onConfigChanged(); });

    listenForKeybindSettingPresses("keybind-toggle", [](Keybind const&, bool isDown, bool isRepeat, double) {
        if (isDown && !isRepeat) {
            toggleEnabled();
        }
        return true;
    });
    listenForKeybindSettingPresses("keybind-preset", [](Keybind const&, bool isDown, bool isRepeat, double) {
        if (isDown && !isRepeat) {
            cyclePreset();
        }
        return true;
    });
}

// ----------------------------------------------------------------------
// Hooks
// ----------------------------------------------------------------------

class $modify(TSPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::pauseGame", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::resume", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (state.isSimulating || isGhost(player)) {
            if (state.isSimulating) {
                state.ghostDied = true;
            }
            return;
        }
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (!player || wasDead || !player->m_isDead) {
            return;
        }
        resetPending();
        state.appliedDown = false;
        state.rapidInputTicks = 0;
        lastPressTick = -1;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        state.hud = nullptr;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        readConfig();
        state.ghost1 = makeGhost(this);
        state.ghost2 = makeGhost(this);
        state.ghostLayerBaseCount = state.ghostLayer->getChildrenCount();
        state.realDown = false;
        state.appliedDown = false;
        state.isFirstPlayer = true;
        state.ticksSinceInput = 999;
        state.rapidInputTicks = 0;
        state.savesThisAttempt = 0;
        lastPressTick = -1;
        resetPending();
        createHud(this);
        return true;
    }

    void onQuit() {
        resetPending();
        dropGhosts();
        state.hud = nullptr;
        PlayLayer::onQuit();
    }

    void resetLevel() {
        resetPending();
        lastPressTick = -1;
        state.appliedDown = false;
        state.savesThisAttempt = 0;
        PlayLayer::resetLevel();
        refreshHud();
    }

    void pauseGame(bool unfocused) {
        resetPending();
        state.rapidInputTicks = 0;
        state.ticksSinceInput = 999;
        lastPressTick = -1;
        sweepGhostLayer();
        PlayLayer::pauseGame(unfocused);
    }

    void resume() {
        resetPending();
        state.rapidInputTicks = 0;
        state.ticksSinceInput = 999;
        lastPressTick = -1;
        sweepGhostLayer();
        PlayLayer::resume();
        refreshHud();
    }

    void playEndAnimationToPos(CCPoint pos) {
        if (state.isSimulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(pos);
    }
};

class $modify(TSEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};

class $modify(TSHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", HOOK_PRIORITY);
    }
    void addPoint(CCPoint point) {
        if (state.isSimulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(TSEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", HOOK_PRIORITY);
    }
    void triggerObject(GJBaseGameLayer* layer, int uniqueId, gd::vector<int> const* remapKeys) {
        if (state.isSimulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueId, remapKeys);
    }
};

class $modify(TSGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", HOOK_PRIORITY);
    }
    void playShineEffect() {
        if (state.isSimulating) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(TSRingObject, RingObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("RingObject::spawnCircle", HOOK_PRIORITY);
    }
    void spawnCircle() {
        if (state.isSimulating) {
            return;
        }
        RingObject::spawnCircle();
    }
};

class $modify(TSPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::update", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::playDeathEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::playerDestroyed", HOOK_PRIORITY);
    }

    void update(float dt) {
        if (state.isSimulating) {
            PlayerObject::update(dt);
            return;
        }
        auto* layer = PlayLayer::get();
        bool isP1 = layer && this == layer->m_player1;
        if (isP1 && !layer->m_isPaused) {
            ++currentTick;
            deliverPending(layer, this);
        }
        if (isP1) {
            state.tickStartX = getPosition().x;
            state.isInRealUpdate = true;
        }
        PlayerObject::update(dt);
        if (isP1) {
            state.isInRealUpdate = false;
            float dx = getPosition().x - state.tickStartX;
            if (std::abs(dx) > 0.01f) {
                state.tickDeltaX = dx;
            }
            if (dt > 0.f) {
                float avg = state.tickLength;
                state.tickLength = (avg > 0.f && std::abs(dt - avg) < avg * 0.3f) ? avg * 0.9f + dt * 0.1f : dt;
            }
        }
    }

    void incrementJumps() {
        if (state.isSimulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    bool ghostFx() {
        return state.isSimulating && isGhost(this);
    }

    void spawnCircle() { if (!ghostFx()) PlayerObject::spawnCircle(); }
    void spawnDualCircle() { if (!ghostFx()) PlayerObject::spawnDualCircle(); }
    void spawnPortalCircle(ccColor3B color, float radius) { if (!ghostFx()) PlayerObject::spawnPortalCircle(color, radius); }
    void spawnScaleCircle() { if (!ghostFx()) PlayerObject::spawnScaleCircle(); }
    void playBumpEffect(int type, GameObject* object) { if (!ghostFx()) PlayerObject::playBumpEffect(type, object); }
    void playSpawnEffect() { if (!ghostFx()) PlayerObject::playSpawnEffect(); }
    void createFadeOutDartStreak() { if (!ghostFx()) PlayerObject::createFadeOutDartStreak(); }
    void fadeOutStreak2(float duration) { if (!ghostFx()) PlayerObject::fadeOutStreak2(duration); }
    void playDynamicSpiderRun() { if (!ghostFx()) PlayerObject::playDynamicSpiderRun(); }
    void animatePlatformerJump(float scale) { if (!ghostFx()) PlayerObject::animatePlatformerJump(scale); }
    void runBallRotation(float speed) { if (!ghostFx()) PlayerObject::runBallRotation(speed); }
    void runBallRotation2() { if (!ghostFx()) PlayerObject::runBallRotation2(); }
    void runNormalRotation(bool notNormal, float speed) { if (!ghostFx()) PlayerObject::runNormalRotation(notNormal, speed); }
    void toggleGhostEffect(GhostType type) { if (!ghostFx()) PlayerObject::toggleGhostEffect(type); }
    void activateStreak() { if (!ghostFx()) PlayerObject::activateStreak(); }
    void playCompleteEffect(bool noEffects, bool instant) { if (!ghostFx()) PlayerObject::playCompleteEffect(noEffects, instant); }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (state.isSimulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void playDeathEffect() {
        if (state.isSimulating || isGhost(this)) {
            return;
        }
        PlayerObject::playDeathEffect();
    }

    void playerDestroyed(bool noEffects) {
        if (state.isSimulating || isGhost(this)) {
            if (state.isSimulating) {
                state.ghostDied = true;
            }
            return;
        }
        PlayerObject::playerDestroyed(noEffects);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (state.isSimulating) {
            if (!isGhost(this) || state.usedOrbs.contains(ring)) {
                return;
            }
            snapshotObject(ring);
            double yVel = m_yVelocity;
            bool upsideDown = m_isUpsideDown;
            bool dashing = m_isDashing;
            PlayerObject::ringJump(ring, skipCheck);
            if (m_yVelocity != yVel || m_isUpsideDown != upsideDown || m_isDashing != dashing) {
                state.usedOrbs.insert(ring);
                if (state.orbUseTick < 0) {
                    state.orbUseTick = state.simTick;
                }
                if (state.isInputDown && state.simTick >= state.inputTickIndex && !state.pressOrb) {
                    state.pressOrb = ring;
                }
            }
            return;
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    void startDashing(DashRingObject* dashRing) {
        if (state.isSimulating && isGhost(this)) {
            state.usedOrbs.insert(dashRing);
            if (state.orbUseTick < 0) {
                state.orbUseTick = state.simTick;
            }
            if (state.isInputDown && state.simTick >= state.inputTickIndex && !state.pressOrb) {
                state.pressOrb = dashRing;
            }
        }
        PlayerObject::startDashing(dashRing);
    }
};

class $modify(TSBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", HOOK_PRIORITY);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* effect) {
        if (state.isSimulating) {
            if (!isGhost(player) || !effect) {
                return false;
            }
            if (typeinfo_cast<RingObject*>(effect)) {
                if (state.usedOrbs.contains(effect)) {
                    return false;
                }
                snapshotObject(effect);
                return true;
            }
            if (state.simUsedObjects.contains(effect)) {
                return false;
            }
            snapshotObject(effect);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, effect);
    }

    // durante la simulacion solo se chequean objetos que importan (mas rapido)
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        if (!state.isSimulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < count) {
            relevant.reserve(count);
        }

        constexpr uint64_t keptMask = COLLIDING_TYPES | INTERACTABLE_TYPES;
        bool hasRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < count; ++i) {
            auto* object = list[i];
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keptMask & bit) {
                if (!state.simDisabledObjects.empty() && state.simDisabledObjects.contains(object)) {
                    continue;
                }
                // pinchos que ya estas tocando de verdad (hitbox generosa) no cuentan
                if (state.hasRealRect && (bit & HAZARD_TYPES)) {
                    if (!state.harmlessObjects.empty() && state.harmlessObjects.contains(object)) {
                        continue;
                    }
                    if (state.simTick <= 1) {
                        auto const& rect = object->getObjectRect();
                        if (rect.intersectsRect(state.realRect1) || rect.intersectsRect(state.realRect2)) {
                            state.harmlessObjects.insert(object);
                            continue;
                        }
                    }
                }
                relevant.push_back(object);
                continue;
            }
            bool modePortal = (MODE_PORTAL_TYPES & bit) != 0;
            bool sizePortal = (SIZE_PORTAL_TYPES & bit) != 0;
            float speed = (modePortal || sizePortal) ? 0.f : speedPortalValue(object->m_objectID);
            if (!modePortal && !sizePortal && speed <= 0.f) {
                continue;
            }
            if (!hasRect) {
                ghostRect = player->getObjectRect();
                hasRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (modePortal) {
                    state.ghostHitPortal = true;
                }
                else if (sizePortal) {
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
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool isDown, int button, bool isFirstPlayer) {
        if (!state.isSimulating && button == 1 && !state.isInjecting) {
            recordRealInput(isDown, this->m_player1);
            bool held = intercept(this, isDown, isFirstPlayer);
            sweepGhostLayer();
            if (held) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(isDown, button, isFirstPlayer);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (state.isSimulating) {
            if (!isGhost(player)) {
                return;
            }
            snapshotObject(ring);
            state.hasTouchedOrb = true;
            if (state.firstOrbTouchTick < 0) {
                state.firstOrbTouchTick = state.simTick;
                state.firstOrbRing = ring;
                state.firstOrbPlayer = player;
                state.orbTouchDistSq = orbDistSq(player, ring);
                state.orbCloseTick = state.simTick;
                state.orbCloseDistSq = state.orbTouchDistSq;
            }
            if (state.isProbing) {
                bool known = false;
                for (auto const& probe : state.probes) {
                    if (probe.ringObject == ring) {
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    float d = orbDistSq(player, ring);
                    ProbeOrb probe;
                    probe.ringObject = ring;
                    probe.playerObject = player;
                    probe.touchDistSq = d;
                    probe.closeDistSq = d;
                    probe.touchTick = state.simTick;
                    probe.closeTick = state.simTick;
                    state.probes.push_back(probe);
                }
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);

        // en la simulacion se asume que clickearias el orbe que tocas
        if (state.isSimulating && state.orbClickEnabled) {
            if (typeinfo_cast<DashRingObject*>(ring)) {
                return;
            }
            bool first = player == state.ghost1;
            bool& held = first ? state.isHeld1 : state.isHeld2;
            bool& autoClick = first ? state.isAuto1 : state.isAuto2;
            if (!held && !autoClick) {
                player->pushButton(PlayerButton::Jump);
                autoClick = true;
            }
        }
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (state.isSimulating) {
            if (isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerId) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerId);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerId, int controlId) {
        if (state.isSimulating) {
            if (!activate) {
                if (auto* objects = this->getGroup(group)) {
                    for (auto* object : CCArrayExt<GameObject*>(objects)) {
                        state.simDisabledObjects.insert(object);
                    }
                }
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerId, controlId);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerId, int controlId) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerId, controlId);
    }

    void toggleGroup(int group, bool activate) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::toggleGroup(group, activate);
    }

    void pickupItem(EffectGameObject* effect) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::pickupItem(effect);
    }

    void updateCounters(int itemId, int value) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::updateCounters(itemId, value);
    }

    void activateItemEditTrigger(ItemTriggerGameObject* trigger) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::activateItemEditTrigger(trigger);
    }

    void activateItemCompareTrigger(ItemTriggerGameObject* trigger, gd::vector<int> const& remapKeys) {
        if (state.isSimulating) {
            return;
        }
        GJBaseGameLayer::activateItemCompareTrigger(trigger, remapKeys);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (state.isSimulating) {
            state.ghostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};
