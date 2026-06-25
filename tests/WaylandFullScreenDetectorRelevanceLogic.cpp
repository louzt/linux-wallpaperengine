// Pure-data relevance helpers extracted from WaylandFullScreenDetector.cpp
// so they can be unit-tested without a Wayland display or its headers.
//
// Source-of-truth mirror: keep these definitions in sync with the
// outer-namespace definitions in WaylandFullScreenDetector.cpp when
// ENABLE_WAYLAND is built. This file exists so the tests target can
// link the helper without pulling in <wayland-client.h> / wlroots /
// wayland-protocols, which are not available in the test sandbox.

#include <string>
#include <string_view>
#include <vector>

namespace WallpaperEngine::Render::Drivers::Detectors {

bool icontains (std::string_view haystack, std::string_view needle) {
    if (needle.empty ()) {
	return true;
    }
    if (haystack.empty ()) {
	return false;
    }
    if (needle.size () > haystack.size ()) {
	return false;
    }

    auto toLower = [] (unsigned char c) {
	if (c >= 'A' && c <= 'Z') {
	    return static_cast<unsigned char> (c - 'A' + 'a');
	}
	return c;
    };

    for (size_t i = 0; i + needle.size () <= haystack.size (); ++i) {
	bool match = true;
	for (size_t j = 0; j < needle.size (); ++j) {
	    if (toLower (static_cast<unsigned char> (haystack[i + j]))
		!= toLower (static_cast<unsigned char> (needle[j]))) {
		match = false;
		break;
	    }
	}
	if (match) {
	    return true;
	}
    }

    return false;
}

bool isFullscreenRelevantParams (
    const bool pauseOnFullscreen, const bool pauseOnFullscreenOnlyWhenActive,
    const std::vector<std::string>& ignoreAppIds, const bool fullscreen, const bool activated,
    const std::string& appId
) {
    if (!pauseOnFullscreen) {
	return false;
    }
    if (!fullscreen) {
	return false;
    }
    if (pauseOnFullscreenOnlyWhenActive && !activated) {
	return false;
    }
    if (!appId.empty ()) {
	for (const auto& ignore : ignoreAppIds) {
	    if (ignore.empty ()) {
		continue;
	    }
	    if (icontains (appId, ignore)) {
		return false;
	    }
	}
    }
    return true;
}

} // namespace WallpaperEngine::Render::Drivers::Detectors