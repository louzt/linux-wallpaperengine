#include <catch2/catch_test_macros.hpp>

#include <vector>

// The sizing decision of the resampler lives in a pure function
// (src/WallpaperEngine/Audio/AudioResampleSizing.cpp) that is part of
// COMMON_SOURCES, so the tests target links the very same function the
// production path calls: no mirror of the logic is needed here.
#include "WallpaperEngine/Audio/AudioResampleSizing.h"

using WallpaperEngine::Audio::computeResampleBufferSizing;
using WallpaperEngine::Audio::RESAMPLE_BUFFER_ALIGNMENT;

namespace {

// The regression this suite pins down: the destination buffer of the resampler
// has to be sized with the DEVICE channel layout, never with the decoder's.
// A mono track on a stereo device used to reserve half of the bytes the
// resampler wrote (16400 instead of 32800 on the measured coredump), the
// resampler then wrote past the end of the buffer, corrupted the next heap
// chunk and aborted the process with "free(): invalid next size (normal)".
struct SizingCase {
    int deviceChannels;
    int decoderChannels;
    int samples;
    int bytesPerSample;
    int expectedChannels;
    long long expectedBufferBytes;
};

const std::vector<SizingCase> kCases = {
    // 1. stereo device + mono decoder: the track is upmixed to two channels,
    //    the buffer has to hold both of them
    {2, 1, 4100, 4, 2, 32800},
    // 2. mono device + mono decoder: one channel
    {1, 1, 4100, 4, 1, 16400},
    // 3. stereo device + stereo decoder: two channels
    {2, 2, 4100, 4, 2, 32800},
    // 4. surround device + mono decoder: six channels
    {6, 1, 1024, 4, 6, 24576},
};

} // namespace

TEST_CASE ("the resampler destination buffer is sized by the device layout", "[audio][resample]") {
    int idx = 0;
    for (const auto& c : kCases) {
        INFO ("case #" << idx
             << " deviceChannels=" << c.deviceChannels
             << " decoderChannels=" << c.decoderChannels
             << " samples=" << c.samples
             << " bytesPerSample=" << c.bytesPerSample);
        const auto sizing = computeResampleBufferSizing (
            c.deviceChannels, c.decoderChannels, c.samples, c.bytesPerSample, RESAMPLE_BUFFER_ALIGNMENT
        );
        REQUIRE (sizing.valid);
        REQUIRE (sizing.channels == c.expectedChannels);
        REQUIRE (sizing.max_out_nb_samples == c.samples);
        REQUIRE (sizing.buffer_bytes == c.expectedBufferBytes);
        ++idx;
    }
}

TEST_CASE ("a mono track on a stereo device reserves the stereo size", "[audio][resample]") {
    // the exact numbers measured on the coredump: 4100 samples of 4 bytes
    const auto sizing = computeResampleBufferSizing (2, 1, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT);

    REQUIRE (sizing.valid);
    REQUIRE (sizing.channels == 2);
    REQUIRE (sizing.max_out_nb_samples == 4100);
    REQUIRE (sizing.buffer_bytes == 32800);
    REQUIRE (sizing.buffer_bytes != 16400);
}

TEST_CASE ("the decoder channel count never sizes the destination buffer", "[audio][resample]") {
    // Same stereo device, three different decoder layouts: the size of the
    // destination buffer must be identical, because the resampler always
    // writes the device layout. This is the test that catches the original
    // bug: with the decoder's channel count winning, the mono case would
    // reserve 16400 bytes instead of 32800.
    const auto mono = computeResampleBufferSizing (2, 1, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT);
    const auto stereo = computeResampleBufferSizing (2, 2, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT);
    const auto surround = computeResampleBufferSizing (2, 6, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT);

    REQUIRE (mono.valid);
    REQUIRE (stereo.valid);
    REQUIRE (surround.valid);

    REQUIRE (mono.buffer_bytes == stereo.buffer_bytes);
    REQUIRE (mono.buffer_bytes == surround.buffer_bytes);
    REQUIRE (mono.buffer_bytes == 32800);

    REQUIRE (mono.channels == stereo.channels);
    REQUIRE (mono.channels == surround.channels);
    REQUIRE (mono.channels == 2);
}

TEST_CASE ("bogus sizing parameters are rejected", "[audio][resample]") {
    // no device channel count
    REQUIRE_FALSE (computeResampleBufferSizing (0, 1, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT).valid);
    REQUIRE_FALSE (computeResampleBufferSizing (-1, 1, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT).valid);
    // no samples to resample
    REQUIRE_FALSE (computeResampleBufferSizing (2, 1, 0, 4, RESAMPLE_BUFFER_ALIGNMENT).valid);
    REQUIRE_FALSE (computeResampleBufferSizing (2, 1, -1, 4, RESAMPLE_BUFFER_ALIGNMENT).valid);
    // no sample size
    REQUIRE_FALSE (computeResampleBufferSizing (2, 1, 4100, 0, RESAMPLE_BUFFER_ALIGNMENT).valid);
    REQUIRE_FALSE (computeResampleBufferSizing (2, 1, 4100, -4, RESAMPLE_BUFFER_ALIGNMENT).valid);
}

TEST_CASE ("an invalid sizing does not report a size", "[audio][resample]") {
    const auto sizing = computeResampleBufferSizing (0, 1, 4100, 4, RESAMPLE_BUFFER_ALIGNMENT);

    REQUIRE_FALSE (sizing.valid);
    REQUIRE (sizing.channels == 0);
    REQUIRE (sizing.max_out_nb_samples == 0);
    REQUIRE (sizing.buffer_bytes == 0);
}
