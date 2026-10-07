#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>

using namespace geode::prelude;

// Manual Frame Assist 1.1 - self-adjusting spam smoothing
//
// Not an auto-clicker: every press/release the game receives is one of YOUR real
// inputs. The mod never creates one, and never applies one earlier than you made it.
//
// While you spam above the activation CPS it:
//   1. learns your current spam rhythm (time between your edges, a rolling average),
//   2. holds each early edge back by a few ms so edges land on an even grid
//      (that is what makes every zigzag the same size),
//   3. keeps every press / release visible to >= 1 physics step so no click drops.
// Late edges are never pushed; the grid just re-syncs to you. Below the activation
// CPS (or when you hold normally) input passes straight through with no delay.

namespace {
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    struct Pending {
        bool down;
        std::uint64_t dueStep;
    };

    struct KeyState {
        std::deque<Pending> pending;
        std::uint64_t lastApplied = 0;
        bool hasApplied = false;
        TimePoint lastArrival{};
        bool hasArrival = false;
        TimePoint lastSlot{};
        bool hasSlot = false;
        double emaInterval = 0.0;   // seconds between your edges
    };

    GJBaseGameLayer* g_layer = nullptr;            // identity only, never dereferenced
    std::unordered_map<std::uint64_t, KeyState> g_keys;
    std::deque<TimePoint> g_recentPresses;
    std::uint64_t g_steps = 0;                     // completed player-1 physics updates
    double g_stepRate = 240.0;                     // physics steps per second
    bool g_applying = false;

    std::uint64_t makeKey(int button, bool player2) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(button)) << 1U) |
               static_cast<std::uint64_t>(player2 ? 1U : 0U);
    }
    int keyButton(std::uint64_t key) { return static_cast<int>(key >> 1U); }
    bool keyPlayer2(std::uint64_t key) { return (key & 1U) != 0U; }

    bool isEnabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
    double activationCPS() { return Mod::get()->getSettingValue<double>("activation-cps"); }

    // Max fraction of your edge interval an early edge may be held back (0 .. 0.6).
    double smoothingFraction() {
        const double pct = std::clamp(Mod::get()->getSettingValue<double>("smoothing"), 0.0, 100.0);
        return 0.6 * pct / 100.0;
    }

    Clock::duration toDuration(double seconds) {
        return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    }
    double toSeconds(Clock::duration d) {
        return std::chrono::duration<double>(d).count();
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

        const double span = toSeconds(g_recentPresses.back() - g_recentPresses[start]);
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

    void applyEvent(GJBaseGameLayer* layer, bool down, int button, bool player2) {
        g_applying = true;
        layer->handleButton(down, button, player2);
        g_applying = false;
    }

    // Right before a player-1 physics update: apply every queued event whose time
    // has come (max one per button per step, so each state is seen by a step).
    void flushQueued(GJBaseGameLayer* layer) {
        for (auto& [key, state] : g_keys) {
            if (state.pending.empty() || state.pending.front().dueStep > g_steps) {
                continue;
            }
            const bool down = state.pending.front().down;
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

        // 1) Learn your rhythm: rolling average of the time between your edges.
        if (state.hasArrival) {
            const double dt = toSeconds(now - state.lastArrival);
            if (dt > 0.35) {
                state.emaInterval = 0.0;
                state.hasSlot = false;
            }
            else if (dt >= 0.002) {
                state.emaInterval = state.emaInterval > 0.0
                    ? 0.6 * state.emaInterval + 0.4 * dt
                    : dt;
            }
        }
        state.lastArrival = now;
        state.hasArrival = true;

        // 2) Even-grid delay: only ever hold an EARLY edge back, never advance one.
        double delay = 0.0;
        if (assist && state.emaInterval > 0.0) {
            const TimePoint slot = state.hasSlot
                ? state.lastSlot + toDuration(state.emaInterval)
                : now;
            const double cap = state.emaInterval * smoothingFraction();
            delay = std::clamp(toSeconds(slot - now), 0.0, cap);
            state.lastSlot = now + toDuration(delay);
            state.hasSlot = true;
        }
        else {
            state.hasSlot = false;
        }

        // 3) Convert to a physics step and keep order + one-step visibility.
        std::uint64_t due = g_steps + static_cast<std::uint64_t>(std::llround(delay * g_stepRate));

        const bool spacing = assist || !state.pending.empty();
        if (spacing) {
            if (!state.pending.empty()) {
                due = std::max(due, state.pending.back().dueStep + 1);
            }
            else if (state.hasApplied) {
                due = std::max(due, state.lastApplied + 1);
            }
        }

        if (state.pending.empty() && due <= g_steps) {
            markApplied(state);
            GJBaseGameLayer::handleButton(down, button, player2);
            return;
        }

        state.pending.push_back({down, due});
    }
};

class $modify(ManualFrameAssistPlayer, PlayerObject) {
    void update(float delta) {
        auto* playLayer = PlayLayer::get();
        const bool isPlayerOne = playLayer && this == playLayer->m_player1;
        const bool active = isPlayerOne && !playLayer->m_playerDied;

        if (active) {
            syncLayer(playLayer);
            if (delta > 0.0f) {
                g_stepRate = std::clamp(1.0 / static_cast<double>(delta), 60.0, 2000.0);
            }
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
