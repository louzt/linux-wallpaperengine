#pragma once

namespace WallpaperEngine::Audio {

/**
 * The alignment the resampled data is assumed to satisfy: the exact size of the
 * data, without any padding.
 */
constexpr int RESAMPLE_BUFFER_ALIGNMENT = 1;

/**
 * The sizing of the destination buffer of the audio resampler.
 */
struct ResampleBufferSizing {
    /** The channel count the resampler writes: the DEVICE layout, never the decoder's */
    int channels = 0;
    /** The upper bound of the samples the resampler can emit for the current chunk */
    int max_out_nb_samples = 0;
    /** The amount of bytes the destination buffer has to hold */
    long long buffer_bytes = 0;
    /** Whether the given parameters describe a valid buffer or not */
    bool valid = false;
};

/**
 * Computes the size of the destination buffer of the audio resampler.
 *
 * The resampler always writes the DEVICE channel layout, no matter what the
 * decoded track holds: a mono track on a stereo device is upmixed to two
 * channels. The destination buffer MUST be sized with that same count, so the
 * device channel count is the one that matters here.
 *
 * The decoder channel count is part of the signature on purpose, and it is
 * deliberately ignored: sizing the buffer with it under-allocates mono sources
 * on a stereo device, the resampler then writes past the end of the buffer,
 * corrupts the next heap chunk and aborts the whole process with
 * "free(): invalid next size (normal)". Keeping it in the signature pins that
 * invariant in the test suite instead of letting it regress silently.
 *
 * @param deviceChannels the channel count of the audio device, which is the layout the resampler writes
 * @param decoderChannels the channel count of the decoded track, ignored on purpose (see above)
 * @param maxOutNbSamples the upper bound of the samples the resampler can emit for the current chunk. In
 *        production this is swr_get_out_samples(), the documented upper bound of the next swr_convert() call,
 *        which already accounts for the resampler's internal delay.
 * @param bytesPerSample the size of a single sample in the output format
 * @param align the alignment the produced data has to satisfy, the resulting size is rounded up to a multiple of
 *        it. An alignment of 1 (or less) means "no padding, the exact size of the data".
 *
 * @return the sizing of the destination buffer, with valid=false when the parameters are bogus
 */
ResampleBufferSizing computeResampleBufferSizing (
    int deviceChannels, int decoderChannels, int maxOutNbSamples, int bytesPerSample, int align
);

} // namespace WallpaperEngine::Audio
