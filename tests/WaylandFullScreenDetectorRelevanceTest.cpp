#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

// The production header is empty unless ENABLE_WAYLAND is defined, and that
// macro is only set for the library target, so the declaration of the helper
// this suite exercises is repeated here. The definition lives in
// tests/WaylandFullScreenDetectorRelevanceLogic.cpp, the mirror of the
// production logic.
namespace WallpaperEngine::Render::Drivers::Detectors {
bool isFullscreenRelevantParams (
    const bool pauseOnFullscreen, const bool pauseOnFullscreenOnlyWhenActive,
    const std::vector<std::string>& ignoreAppIds, const bool fullscreen, const bool activated,
    const std::string& appId
);
} // namespace WallpaperEngine::Render::Drivers::Detectors

using WallpaperEngine::Render::Drivers::Detectors::isFullscreenRelevantParams;

namespace {

// Mirror of the truth table documented in
// WaylandFullScreenDetector.h — keep the two in sync.
struct RelevanceCase {
    bool pauseOnFullscreen;
    bool pauseOnFullscreenOnlyWhenActive;
    std::vector<std::string> ignoreAppIds;
    bool fullscreen;
    bool activated;
    std::string appId;
    bool expected;
};

const std::vector<RelevanceCase> kCases = {
    // 1. pauseOnFullscreen=false → always false
    {false, false, {}, true, true, "anything", false},
    {false, true, {}, true, true, "anything", false},

    // 2. not fullscreen → false regardless of flags
    {true, false, {}, false, true, "anything", false},
    {true, true, {}, false, true, "anything", false},

    // 3. pauseOnFullscreenOnlyWhenActive=true and !activated → false
    {true, true, {}, true, false, "anything", false},

    // 4. pauseOnFullscreenOnlyWhenActive=false (default) → true if fullscreen,
    //    no ignore list filtering
    {true, false, {}, true, true, "anything", true},
    {true, false, {}, true, false, "anything", true},

    // 5. ignore list: empty ignore entries are skipped (treated as wildcard
    //    that matches anything, but since they're empty, we skip them)
    {true, true, {""}, true, true, "steam", true},

    // 6. ignore list: substring match (case-insensitive) skips the toplevel
    {true, true, {"steam"}, true, true, "steam_app_12345", false},
    {true, true, {"STEAM"}, true, true, "SteamLauncher", false},
    {true, true, {"minecraft"}, true, true, "net.minecraft", false},

    // 7. ignore list: no substring match → still relevant
    {true, true, {"steam"}, true, true, "firefox", true},
    {true, true, {"steam", "lutris"}, true, true, "firefox", true},
};

} // namespace

TEST_CASE ("isFullscreenRelevantParams truth table", "[wayland][detector]") {
    int idx = 0;
    for (const auto& c : kCases) {
        INFO ("case #" << idx
             << " pauseOnFullscreen=" << c.pauseOnFullscreen
             << " pauseOnFullscreenOnlyWhenActive=" << c.pauseOnFullscreenOnlyWhenActive
             << " fullscreen=" << c.fullscreen
             << " activated=" << c.activated
             << " appId='" << c.appId << "'");
        REQUIRE (isFullscreenRelevantParams (
                     c.pauseOnFullscreen, c.pauseOnFullscreenOnlyWhenActive,
                     c.ignoreAppIds, c.fullscreen, c.activated, c.appId)
                 == c.expected);
        ++idx;
    }
}

TEST_CASE ("isFullscreenRelevantParams: pauseOnFullscreen=false short-circuits", "[wayland][detector]") {
    REQUIRE_FALSE (isFullscreenRelevantParams (false, false, {}, true, true, ""));
    REQUIRE_FALSE (isFullscreenRelevantParams (false, true, {}, true, true, ""));
}

TEST_CASE ("isFullscreenRelevantParams: not-fullscreen short-circuits", "[wayland][detector]") {
    REQUIRE_FALSE (isFullscreenRelevantParams (true, false, {}, false, true, ""));
    REQUIRE_FALSE (isFullscreenRelevantParams (true, true, {}, false, false, ""));
}

TEST_CASE ("isFullscreenRelevantParams: onlyWhenActive gate", "[wayland][detector]") {
    REQUIRE (isFullscreenRelevantParams (true, false, {}, true, false, "firefox"));
    REQUIRE_FALSE (isFullscreenRelevantParams (true, true, {}, true, false, "firefox"));
    REQUIRE (isFullscreenRelevantParams (true, true, {}, true, true, "firefox"));
}

TEST_CASE ("isFullscreenRelevantParams: ignore list is substring + case-insensitive", "[wayland][detector]") {
    REQUIRE_FALSE (isFullscreenRelevantParams (true, true, {"Steam"}, true, true, "STEAM_LAUNCHER"));
    REQUIRE_FALSE (isFullscreenRelevantParams (true, true, {"minecraft"}, true, true, "net.Minecraft"));
    REQUIRE (isFullscreenRelevantParams (true, true, {"steam"}, true, true, "firefox"));
}