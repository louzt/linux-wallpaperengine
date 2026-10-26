#include "PulseAudioPlayingDetector.h"
#include "WallpaperEngine/Logging/Log.h"

#include <chrono>
#include <unistd.h>

namespace WallpaperEngine::Audio::Drivers::Detectors {
/** How long the constructor waits for the pulse server to accept the connection */
static constexpr int PA_CONNECT_TIMEOUT_MS = 3000;
/** How long to wait between two attempts to rebuild a dead pulse context */
static constexpr int PA_RECONNECT_INTERVAL_SEC = 5;
/** How many non-blocking mainloop iterations are pumped after a reconnect attempt */
static constexpr int PA_RECONNECT_ITERATIONS = 5;

void sinkInputInfoCallback (pa_context* context, const pa_sink_input_info* info, int eol, void* userdata) {
    auto* detector = static_cast<PulseAudioPlayingDetector*> (userdata);

    if (info == nullptr) {
	return;
    }

    if (info->proplist == nullptr) {
	return;
    }

    // get processid
    const char* value = pa_proplist_gets (info->proplist, PA_PROP_APPLICATION_PROCESS_ID);

    if (value && strtol (value, nullptr, 10) != getpid () && pa_cvolume_avg (&info->volume) != PA_VOLUME_MUTED) {
	detector->setIsPlaying (true);
    }
}

void defaultSinkInfoCallback (pa_context* context, const pa_server_info* info, void* userdata) {
    if (info == nullptr) {
	return;
    }

    pa_operation* op = pa_context_get_sink_input_info_list (context, sinkInputInfoCallback, userdata);

    // the context can be gone by the time this callback runs: the operation
    // is then NULL and unreferencing it aborts the process through pa_xassert
    if (op == nullptr) {
	return;
    }

    pa_operation_unref (op);
}

PulseAudioPlayingDetector::PulseAudioPlayingDetector (
    Application::ApplicationContext& appContext,
    const Render::Drivers::Detectors::FullScreenDetector& fullscreenDetector
) : AudioPlayingDetector (appContext, fullscreenDetector) {
    this->m_mainloop = pa_mainloop_new ();
    this->m_mainloopApi = pa_mainloop_get_api (this->m_mainloop);
    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine");

    if (this->m_context == nullptr) {
	sLog.error ("Cannot create the pulseaudio context.");
	return;
    }

    pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    // lock until pulseaudio allows connection, but never forever: when the
    // pulse server is not running (or dies while we wait) the context never
    // reaches PA_CONTEXT_READY and an unbounded loop hangs the whole process
    if (!this->waitForContext (PA_CONNECT_TIMEOUT_MS)) {
	sLog.error ("Pulseaudio is not ready, the automute detection will retry in the background.");
    }
}

PulseAudioPlayingDetector::~PulseAudioPlayingDetector () {
    if (this->m_context) {
	pa_context_disconnect (this->m_context);
	pa_context_unref (this->m_context);
	this->m_context = nullptr;
    }

    if (this->m_mainloop) {
	pa_mainloop_free (this->m_mainloop);
	this->m_mainloop = nullptr;
    }
}

bool PulseAudioPlayingDetector::waitForContext (const int timeoutMs) {
    if (this->m_context == nullptr) {
	return false;
    }

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeoutMs);

    while (true) {
	const pa_context_state state = pa_context_get_state (this->m_context);

	if (state == PA_CONTEXT_READY) {
	    return true;
	}

	// the server is gone or was never reachable: retrying would spin forever
	if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
	    return false;
	}

	if (std::chrono::steady_clock::now () >= deadline) {
	    return false;
	}

	pa_mainloop_iterate (this->m_mainloop, 1, nullptr);
    }
}

bool PulseAudioPlayingDetector::ensureContext () {
    if (this->m_mainloopApi == nullptr) {
	return false;
    }

    if (this->m_context != nullptr && pa_context_get_state (this->m_context) == PA_CONTEXT_READY) {
	return true;
    }

    const auto now = std::chrono::steady_clock::now ();

    // rebuilding the context on every frame would hammer the pulse server
    if (now - this->m_lastReconnectAttempt < std::chrono::seconds (PA_RECONNECT_INTERVAL_SEC)) {
	return false;
    }

    this->m_lastReconnectAttempt = now;

    if (this->m_context != nullptr) {
	pa_context_disconnect (this->m_context);
	pa_context_unref (this->m_context);
	this->m_context = nullptr;
    }

    this->m_context = pa_context_new (this->m_mainloopApi, "wallpaperengine");

    if (this->m_context == nullptr) {
	return false;
    }

    pa_context_connect (this->m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    // pump the mainloop without blocking so the connection can make progress
    // without stalling the render thread
    for (int i = 0; i < PA_RECONNECT_ITERATIONS; i++) {
	pa_mainloop_iterate (this->m_mainloop, 0, nullptr);
    }

    return pa_context_get_state (this->m_context) == PA_CONTEXT_READY;
}

void PulseAudioPlayingDetector::update () {
    if (!this->getApplicationContext ().settings.audio.automute) {
	return this->setIsPlaying (false);
    }
    if (this->getFullscreenDetector ().anythingFullscreen ()) {
	return this->setIsPlaying (true);
    }

    // reset playing state
    this->setIsPlaying (false);

    // The pulse server can go away while the wallpaper is running (the audio
    // device gets reconfigured, the server is restarted, the session is
    // suspended...). The context stops being valid and every
    // pa_context_get_* call returns NULL: dereferencing that operation aborts
    // the whole process through pa_xassert, which is how the wallpaper used to
    // die and take the desktop background with it.
    if (!this->ensureContext ()) {
	return;
    }

    // start discovery of sinks
    pa_operation* op = pa_context_get_server_info (this->m_context, defaultSinkInfoCallback, this);

    // the operation could not be created because the context is gone
    if (op == nullptr) {
	return;
    }

    // wait until all the operations are done
    while (pa_operation_get_state (op) == PA_OPERATION_RUNNING) {
	pa_mainloop_iterate (this->m_mainloop, 1, nullptr);
    }

    pa_operation_unref (op);
}
} // namespace WallpaperEngine::Audio::Drivers::Detectors
