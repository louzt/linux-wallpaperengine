#pragma once

#include "AudioPlayingDetector.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <pulse/pulseaudio.h>

namespace WallpaperEngine::Audio::Drivers::Detectors {
class PulseAudioPlayingDetector final : public AudioPlayingDetector {
public:
    explicit PulseAudioPlayingDetector (
	Application::ApplicationContext& appContext, const Render::Drivers::Detectors::FullScreenDetector&
    );
    ~PulseAudioPlayingDetector () override;

    void update () override;

private:
    /**
     * Waits for the pulse context to become usable
     *
     * @param timeoutMs how long to wait for the connection at most
     *
     * @return true if the context reached PA_CONTEXT_READY
     */
    [[nodiscard]] bool waitForContext (int timeoutMs);

    /**
     * Makes sure the pulse context is usable, rebuilding it when the pulse
     * server went away
     *
     * @return true if the context is ready to be used
     */
    [[nodiscard]] bool ensureContext ();

    pa_mainloop* m_mainloop = nullptr;
    pa_mainloop_api* m_mainloopApi = nullptr;
    pa_context* m_context = nullptr;
    /** The last time a dead context was rebuilt */
    std::chrono::steady_clock::time_point m_lastReconnectAttempt {};
};
} // namespace WallpaperEngine::Audio::Drivers::Detectors
