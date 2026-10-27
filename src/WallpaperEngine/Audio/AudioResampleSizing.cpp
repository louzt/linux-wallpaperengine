#include "AudioResampleSizing.h"

namespace WallpaperEngine::Audio {

ResampleBufferSizing computeResampleBufferSizing (
    const int deviceChannels, const int decoderChannels, const int maxOutNbSamples, const int bytesPerSample,
    const int align
) {
    ResampleBufferSizing sizing {};

    // The decoder channel count never sizes the destination buffer: the resampler
    // writes the device layout. It is part of the signature on purpose, so the
    // invariant is pinned by the test suite instead of regressing silently.
    (void) decoderChannels;

    if (deviceChannels <= 0 || maxOutNbSamples <= 0 || bytesPerSample <= 0) {
	return sizing;
    }

    sizing.channels = deviceChannels;
    sizing.max_out_nb_samples = maxOutNbSamples;

    long long bytes = static_cast<long long> (deviceChannels) * static_cast<long long> (maxOutNbSamples)
		      * static_cast<long long> (bytesPerSample);

    if (align > 1) {
	bytes = ((bytes + align - 1) / align) * align;
    }

    sizing.buffer_bytes = bytes;
    sizing.valid = true;

    return sizing;
}

} // namespace WallpaperEngine::Audio
