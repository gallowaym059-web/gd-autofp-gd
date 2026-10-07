#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>

using namespace geode::prelude;

// Manual Frame Assist
//
// Not an auto-clicker: it never creates an input. It only guarantees that every
// REAL press and every REAL release stays "visible" to at least one physics step.
//
// Why spam drops clicks: if a press and its release (or a release and the next
// press) are both processed between two physics steps, the player object never
// sees that state, so the click is silently lost. That is the inconsistency you
// feel when spamming wave / ship / ufo / etc. at high CPS.
//
// Fix: per button, events are applied in order and never closer than one physics
// step apart. While you are not spamming, input is passed straight through with
// zero added latency.

namespace {
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct KeyState {
        std::deque<bool> pending;       // queued events, true = press, false = release
        std::uint64_t lastApplied = 0;  // step counter value when the last event was applied
        bool hasApplied = false;
    };

    GJBaseGameLayer* g_layer = nullptr;            // identity only, never dereferenced
    std::unordered_map<std::uint64_t, KeyState> g_keys;
    std::deque<TimePoint> g_recentPresses;
    std::uint64_t g_steps = 0;                     // completed player-1 physics updates
    bool g_applying = false;                       // true while we feed an event to the game

    std::uint64_t makeKey(int button, bool player2) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(button)) << 1U) |
               static_cast<std::uint64_t>(player2 ? 1U : 0U);
    }

    int keyButton(std::uint64_t key) { return static_cast<int>(key >> 1U); }
    bool keyPlayer2(std::uint64_t key) { return (key & 1U) != 0U; }

    bool isEnabled() {
        return Mod::get()->getSettingValue<bool>("enabled");
    }

    double activationCPS() {
        return Mod::get()->getSettingValue<double>("activation-cps");
    }

    void clearAssistState() {
        g_keys.clear();
        g_recentPresses.clear();
        g_steps = 0;
        g_layer = nullptr;
    }

    void syncLayer(GJBaseGameLayer* layer) {
        if (g_layer != layer) {
            clearAssistState();
            g_layer = layer;
        }
    }

    bool canApply(KeyState const& state) {
        return !state.hasApplied || g_steps > state.lastApplied;
    }

    void markApplied(KeyState& state) {
        state.lastApplied = g_steps;
        state.hasApplied = true;
    }

    void trimRecentPresses(TimePoint now) {
        constexpr auto maxAge = std::chrono::milliseconds(1000);
        while (!g_recentPresses.empty() && now - g_recentPresses.front() > maxAge) {
            g_recentPresses.pop_front();
        }
    }

    // Current CPS from the last few real presses. Decays to 0 shortly after you stop.
    double calculateCPS(TimePoint now) {
        trimRecentPresses(now);

        if (g_recentPresses.size() < 2) {
            return 0.0;
        }
        if (now - g_recentPresses.back() > std::chrono::milliseconds(500)) {
            return 0.0;
        }

        constexpr std::size_t maxIntervals = 7;
        const std::size_t count = g_recentPresses.size();
        const std::size_t start = count > maxIntervals + 1 ? count - (maxIntervals + 1) : 0;

        const double span = std::chrono::duration<double>(
            g_recentPresses.back() - g_recentPresses[start]
        ).count();
        const std::size_t intervals = count - 1 - start;

        if (span <= 0.0 || intervals == 0) {
            return 0.0;
        }
        return static_cast<double>(intervals) / span;
    }

    bool isGameplayLayer(GJBaseGameLayer* layer) {
        auto* playLayer = PlayLayer::get();
        return playLayer != nullptr &&
               layer == static_cast<GJBaseGameLayer*>(playLayer) &&
               !playLayer->m_playerDied;
    }

    // Feed one real event to the game's original input handler.
    void applyEvent(GJBaseGameLayer* layer, bool down, int button, bool player2) {
        g_applying = true;
        layer->handleButton(down, button, player2);
        g_applying = false;
    }

    // Called right before a player-1 physics update: release every queued event
    // that has been waiting long enough (max one event per button per step).
    void flushQueued(GJBaseGameLayer* layer) {
        for (auto& [key, state] : g_keys) {
            if (state.pending.empty() || !canApply(state)) {
                continue;
            }
            const bool down = state.pending.front();
            state.pending.pop_front();
            markApplied(state);
            applyEvent(layer, down, keyButton(key), keyPlayer2(key));
        }
    }
}

class $modify(ManualFrameAssistGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::Early);
    }

    void handleButton(bool down, int button, bool player2) {
        // Our own replayed events, or anything we shouldn't touch.
        if (g_applying || !isEnabled() || !isGameplayLayer(this)) {
            GJBaseGameLayer::handleButton(down, button, player2);
            return;
        }

        syncLayer(this);

        const auto now = Clock::now();
        if (down) {
            trimRecentPresses(now);
            g_recentPresses.push_back(now);
        }

        const bool assist = calculateCPS(now) >= activationCPS();
        auto& state = g_keys[makeKey(button, player2)];

        // Order must never change: if anything is already waiting, we wait too.
        // Otherwise apply right now (zero latency) unless we're spamming and the
        // previous state hasn't been seen by a physics step yet.
        if (state.pending.empty() && (!assist || canApply(state))) {
            markApplied(state);
            GJBaseGameLayer::handleButton(down, button, player2);
            return;
        }

        state.pending.push_back(down);
    }
};

class $modify(ManualFrameAssistPlayer, PlayerObject) {
    void update(float delta) {
        auto* playLayer = PlayLayer::get();
        const bool isPlayerOne = playLayer && this == playLayer->m_player1;
        const bool active = isPlayerOne && !playLayer->m_playerDied;

        if (active) {
            syncLayer(playLayer);
            if (isEnabled()) {
                flushQueued(playLayer);
            }
        }

        PlayerObject::update(delta);

        if (active) {
            ++g_steps;
        }
    }
};

class $modify(ManualFrameAssistPlayLayer, PlayLayer) {
    void resetLevel() {
        clearAssistState();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        clearAssistState();
        PlayLayer::onQuit();
    }
};
