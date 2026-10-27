#include "AudioStream.h"
#include "WallpaperEngine/Audio/AudioResampleSizing.h"
#include "WallpaperEngine/Logging/Log.h"
#include <cassert>
#include <cmath>
#include <iostream>

// maximum size of the queue to prevent reading too much data

using namespace WallpaperEngine::Audio;

namespace {
/**
 * RAII lock over the shared codec context.
 *
 * The decoder context is used by two threads: the reader thread flushes it
 * when the stream loops back to its start, and the audio thread decodes
 * frames out of it. The mutex is null only when the stream failed to
 * initialize, in which case there is nothing to serialize.
 */
class CodecLock {
public:
    explicit CodecLock (SDL_mutex* mutex) : m_mutex (mutex) {
	if (m_mutex != nullptr) {
	    SDL_LockMutex (m_mutex);
	}
    }

    ~CodecLock () {
	if (m_mutex != nullptr) {
	    SDL_UnlockMutex (m_mutex);
	}
    }

private:
    SDL_mutex* m_mutex;
};

/**
 * Returns the channel count of a decoded frame.
 *
 * The frame is the one holding the samples the resampler is about to convert,
 * so its channel count is the decoder's one: the layout of the source track,
 * which is NOT the layout the resampler writes.
 */
int get_frame_nb_channels (const AVFrame* frame) {
#if FF_API_OLD_CHANNEL_LAYOUT
	return frame->channels;
#else
	return frame->ch_layout.nb_channels;
#endif
}
} // namespace

int audio_read_thread (void* arg) {
    SDL_mutex* waitMutex = SDL_CreateMutex ();
    auto* stream = static_cast<AudioStream*> (arg);
    AVPacket* packet = av_packet_alloc ();
    int ret = 0;

    if (waitMutex == nullptr) {
	sLog.exception ("Cannot create mutex for audio playback waiting");
    }

    while (ret >= 0 && stream->getAudioContext ().getApplicationContext ().state.general.keepRunning
	   && stream->isInitialized ()) {
	// give the cpu some time to play the queued frames if there's enough info there
	if (stream->getQueueSize () >= MAX_QUEUE_SIZE
	    || (stream->getQueuePacketCount () > MIN_FRAMES
		&& (av_q2d (stream->getTimeBase ()) * stream->getQueueDuration () > 1.0))) {
	    SDL_LockMutex (waitMutex);
	    SDL_CondWaitTimeout (stream->getWaitCondition (), waitMutex, 10);
	    SDL_UnlockMutex (waitMutex);
	    continue;
	}

	ret = av_read_frame (stream->getFormatContext (), packet);

	if (ret == AVERROR_EOF) {
	    // seek to the beginning of the file again
	    avformat_seek_file (stream->getFormatContext (), stream->getAudioStream (), 0, 0, 0, ~AVSEEK_FLAG_FRAME);
	    // The decoder context is shared with the audio thread, which decodes
	    // frames out of it: flushing it from this thread without holding the
	    // codec mutex races with avcodec_receive_frame() and corrupts the
	    // decoder state.
	    stream->flushCodec ();

	    // ensure the thread is not killed if audio has to be looped
	    if (stream->isRepeat ()) {
		ret = 0;
	    }

	    continue;
	}

	// TODO: PROPERLY IMPLEMENT THIS
	if (packet->stream_index == stream->getAudioStream ()) {
	    stream->queuePacket (packet);
	} else {
	    av_packet_unref (packet);
	}
    }

    // stop the audio too just in case
    SDL_DestroyMutex (waitMutex);

    return 0;
}

static int audio_read_data_callback (void* streamarg, uint8_t* buffer, int buffer_size) {
    const auto stream = static_cast<AudioStream*> (streamarg);

    // check if we're at eof and return the right value
    if (stream->getBuffer ()->eof ()) {
	return AVERROR_EOF;
    }

    stream->getBuffer ()->read (reinterpret_cast<std::istream::char_type*> (buffer), buffer_size);

    if (stream->getBuffer ()->fail () && !stream->getBuffer ()->eof ()) {
	return AVERROR_INVALIDDATA;
    }

    // return read bytes only
    return stream->getBuffer ()->gcount ();
}

int64_t audio_seek_data_callback (void* streamarg, int64_t offset, int whence) {
    const auto stream = static_cast<AudioStream*> (streamarg);

    // reset error state
    stream->getBuffer ()->clear ();

    if (whence & AVSEEK_SIZE) {
	const auto current = stream->getBuffer ()->tellg ();
	stream->getBuffer ()->seekg (0, std::ios_base::end);
	const auto end = stream->getBuffer ()->tellg ();
	stream->getBuffer ()->seekg (current, std::ios_base::beg);
	return end;
    }

    switch (whence) {
	case SEEK_CUR:
	    stream->getBuffer ()->seekg (offset, std::ios_base::cur);
	    break;
	case SEEK_SET:
	    stream->getBuffer ()->seekg (offset, std::ios_base::beg);
	    break;
	case SEEK_END:
	    stream->getBuffer ()->seekg (offset, std::ios_base::end);
	    break;
    }

    return 0;
}

AudioStream::AudioStream (AudioContext& context, const std::string& filename) : m_audioContext (context) {
    this->loadCustomContent (filename.c_str ());
}

AudioStream::AudioStream (AudioContext& context, const ReadStreamSharedPtr& buffer) : m_audioContext (context) {
    // setup a custom context first
    this->m_formatContext = avformat_alloc_context ();

    if (this->m_formatContext == nullptr) {
	sLog.exception ("Cannot allocate ffmpeg format context");
    }

    this->m_buffer = buffer;

    // setup custom io for it
    this->m_formatContext->pb = avio_alloc_context (
	static_cast<uint8_t*> (av_malloc (4096)), 4096, 0, this, &audio_read_data_callback, nullptr,
	&audio_seek_data_callback
    );

    if (this->m_formatContext->pb == nullptr) {
	sLog.exception ("Cannot create avio context");
    }

    // continue the normal load procedure
    this->loadCustomContent ();
}

AudioStream::AudioStream (AudioContext& audioContext, AVCodecContext* context) :
    m_audioContext (audioContext), m_context (context), m_queue (new PacketQueue) {
    this->initialize ();
}

AudioStream::~AudioStream () {
    // stop the audio
    this->stop ();

    if (this->m_audioThread != nullptr) {
	// wait for the thread to finish
	SDL_WaitThread (this->m_audioThread, nullptr);
    }

    this->m_audioThread = nullptr;

    // the reader thread is gone by now, the codec mutex is no longer in use
    if (this->m_codecMutex != nullptr) {
	SDL_DestroyMutex (this->m_codecMutex);
	this->m_codecMutex = nullptr;
    }

    if (this->m_queue != nullptr) {
	// wait for the audio buffers to be done
	SDL_CondWait (this->m_queue->wait, this->m_queue->mutex);
    }

    if (this->m_swrctx != nullptr && swr_is_initialized (this->m_swrctx) == true) {
	swr_close (this->m_swrctx);
    }
    if (this->m_swrctx != nullptr) {
	swr_free (&this->m_swrctx);
    }
    if (this->m_decodePacket != nullptr) {
	av_packet_free (&this->m_decodePacket);
    }
    if (this->m_decodeFrame != nullptr) {
	av_frame_free (&this->m_decodeFrame);
    }
    if (this->m_queue != nullptr && this->m_queue->packetList != nullptr) {
#if FF_API_FIFO_OLD_API
	av_fifo_free (this->m_queue->packetList);
	this->m_queue->packetList = nullptr;
#else
	av_fifo_freep2 (&this->m_queue->packetList);
#endif /* FF_API_FIFO_OLD_API */
    }

    delete this->m_queue;

    if (this->m_formatContext != nullptr) {
	avformat_free_context (this->m_formatContext);
    }

    if (this->m_context != nullptr) {
	avcodec_free_context (&this->m_context);
    }
}

void AudioStream::loadCustomContent (const char* filename) {
    if (avformat_open_input (&this->m_formatContext, filename, nullptr, nullptr) != 0) {
	sLog.exception ("Cannot open audio file: ", filename);
    }
    if (avformat_find_stream_info (this->m_formatContext, nullptr) < 0) {
	sLog.exception ("Cannot determine file format: ", filename);
    }

    // find the audio stream
    for (unsigned int i = 0; i < this->m_formatContext->nb_streams; i++) {
	if (this->m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
	    && this->m_audioStream == NO_AUDIO_STREAM) {
	    this->m_audioStream = i;
	}
    }

    if (this->m_audioStream == NO_AUDIO_STREAM) {
	sLog.exception ("Cannot find an audio stream in file ", filename);
    }

    // get the decoder for it and alloc the required context
    const AVCodec* aCodec
	= avcodec_find_decoder (this->m_formatContext->streams[this->m_audioStream]->codecpar->codec_id);

    if (aCodec == nullptr) {
	sLog.exception ("Cannot initialize audio decoder for file: ", filename);
    }

    // alocate context
    AVCodecContext* avCodecContext = avcodec_alloc_context3 (aCodec);

    if (avcodec_parameters_to_context (avCodecContext, this->m_formatContext->streams[this->m_audioStream]->codecpar)
	!= 0) {
	sLog.exception ("Cannot initialize audio decoder parameters");
    }

    // finally open
    avcodec_open2 (avCodecContext, aCodec, nullptr);

    // initialize default data
    this->m_context = avCodecContext;
    this->m_queue = new PacketQueue;

    this->initialize ();

    // initialize an SDL thread to read the file
    this->m_audioThread = SDL_CreateThread (audio_read_thread, filename, this);
}

void AudioStream::initialize () {
// allocate the FIFO buffer
#if FF_API_FIFO_OLD_API
    this->m_queue->packetList = av_fifo_alloc (sizeof (MyAVPacketList));
#else
    this->m_queue->packetList = av_fifo_alloc2 (1, sizeof (MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
#endif

#if FF_API_OLD_CHANNEL_LAYOUT
    int64_t out_channel_layout;

    // set output audio channels based on the input audio channels
    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_layout = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_layout = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_layout = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    // the resampler always writes the DEVICE layout: remember how many
    // channels it will produce, the destination buffer has to be sized with it
    this->m_out_nb_channels = av_get_channel_layout_nb_channels (out_channel_layout);

    // initialize swrctx
    this->m_swrctx = swr_alloc_set_opts (
	nullptr, out_channel_layout, this->m_audioContext.getFormat (), this->m_audioContext.getSampleRate (),
	this->getContext ()->channel_layout, this->getContext ()->sample_fmt, this->getContext ()->sample_rate, 0,
	nullptr
    );
#else
    AVChannelLayout out_channel_layout;
    int64_t out_channel_mask;

    // set output audio channels based on the input audio channels
    switch (this->m_audioContext.getChannels ()) {
	case 1:
	    out_channel_mask = AV_CH_LAYOUT_MONO;
	    break;
	case 2:
	    out_channel_mask = AV_CH_LAYOUT_STEREO;
	    break;
	default:
	    out_channel_mask = AV_CH_LAYOUT_SURROUND;
	    break;
    }

    if (av_channel_layout_from_mask (&out_channel_layout, out_channel_mask) != 0) {
	sLog.exception ("Cannot get channel layout from mask");
    }

    // the resampler always writes the DEVICE layout: remember how many
    // channels it will produce, the destination buffer has to be sized with it
    this->m_out_nb_channels = out_channel_layout.nb_channels;

    swr_alloc_set_opts2 (
	&this->m_swrctx, &out_channel_layout, this->m_audioContext.getFormat (), this->m_audioContext.getSampleRate (),
	&this->m_context->ch_layout, this->m_context->sample_fmt, this->m_context->sample_rate, 0, nullptr
    );
#endif

    if (this->m_swrctx == nullptr) {
	sLog.exception ("Cannot initialize swrctx for audio resampling");
    }

    // initialize the context
    if (swr_init (this->m_swrctx) < 0) {
	sLog.exception ("Failed to initialize the resampling context.");
    }

    // setup the queue information
    this->m_queue->mutex = SDL_CreateMutex ();
    this->m_queue->cond = SDL_CreateCond ();
    this->m_queue->wait = SDL_CreateCond ();

    // The codec context is shared between the reader thread (which flushes it
    // when the stream loops back to its start) and the audio thread (which
    // decodes frames out of it): a dedicated mutex serializes both accesses.
    if (this->m_codecMutex == nullptr) {
	this->m_codecMutex = SDL_CreateMutex ();
    }

    this->m_decodeFrame = av_frame_alloc ();
    this->m_decodePacket = av_packet_alloc ();

    if (!this->m_decodeFrame) {
	sLog.exception ("Could not allocate AVFrame.\n");
    }
    if (!this->m_decodePacket) {
	sLog.exception ("Could not allocate AVPacket.\n");
    }

    this->m_initialized = true;
}

void AudioStream::queuePacket (AVPacket* pkt) {
    // clone the packet
    AVPacket* clone = av_packet_alloc ();

    if (clone == nullptr) {
	av_packet_unref (clone);
	return;
    }

    av_packet_move_ref (clone, pkt);

    SDL_LockMutex (this->m_queue->mutex);
    const bool gotQueued = this->doQueue (clone);
    SDL_UnlockMutex (this->m_queue->mutex);

    if (!gotQueued) {
	av_packet_free (&pkt);
    }
}

bool AudioStream::doQueue (AVPacket* pkt) {
    MyAVPacketList entry { pkt };

#if FF_API_FIFO_OLD_API
    if (av_fifo_space (this->m_queue->packetList) < static_cast<int> (sizeof (entry))) {
	if (av_fifo_grow (this->m_queue->packetList, sizeof (entry)) < 0) {
	    return false;
	}
    }

    av_fifo_generic_write (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
#else
    // write the entry if possible
    if (av_fifo_write (this->m_queue->packetList, &entry, 1) < 0) {
	return false;
    }
#endif

    this->m_queue->nb_packets++;
    this->m_queue->size += entry.packet->size + sizeof (entry);
    this->m_queue->duration += entry.packet->duration;

    SDL_CondSignal (this->m_queue->cond);

    return true;
}

void AudioStream::dequeuePacket () {
    MyAVPacketList entry {};

    SDL_LockMutex (this->m_queue->mutex);

    while (this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
#if FF_API_FIFO_OLD_API
	int ret = -1;

	if (av_fifo_size (this->m_queue->packetList) >= static_cast<int> (sizeof (entry))) {
	    ret = av_fifo_generic_read (this->m_queue->packetList, &entry, sizeof (entry), nullptr);
	}
#else
	const int ret = av_fifo_read (this->m_queue->packetList, &entry, 1);
#endif

	// enough data available, read it
	if (ret >= 0) {
	    this->m_queue->nb_packets--;
	    this->m_queue->size -= entry.packet->size + sizeof (entry);
	    this->m_queue->duration -= entry.packet->duration;

	    // move the reference and free the old one
	    av_packet_move_ref (this->m_decodePacket, entry.packet);
	    av_packet_free (&entry.packet);
	    break;
	}

	// make the thread wait if nothing was available
	SDL_CondWait (this->m_queue->cond, this->m_queue->mutex);
    }

    SDL_UnlockMutex (this->m_queue->mutex);
}

AVCodecContext* AudioStream::getContext () const { return this->m_context; }

AVFormatContext* AudioStream::getFormatContext () const { return this->m_formatContext; }

int AudioStream::getAudioStream () const { return this->m_audioStream; }

bool AudioStream::isInitialized () const { return this->m_initialized; }

void AudioStream::setRepeat (const bool newRepeat) { this->m_repeat = newRepeat; }

bool AudioStream::isRepeat () const { return this->m_repeat; }

ReadStreamSharedPtr& AudioStream::getBuffer () { return this->m_buffer; }

SDL_cond* AudioStream::getWaitCondition () const { return this->m_queue->wait; }

size_t AudioStream::getQueueSize () const { return this->m_queue->size; }

int AudioStream::getQueuePacketCount () const { return this->m_queue->nb_packets; }

AVRational AudioStream::getTimeBase () const {
    if (this->m_audioStream == NO_AUDIO_STREAM) {
	return { 0, 0 };
    }

    return this->m_formatContext->streams[this->m_audioStream]->time_base;
}

int64_t AudioStream::getQueueDuration () const { return this->m_queue->duration; }

bool AudioStream::isQueueEmpty () const { return this->m_queue->nb_packets == 0; }

SDL_mutex* AudioStream::getMutex () const { return this->m_queue->mutex; }

void AudioStream::stop () {
    if (!this->isInitialized ()) {
	return;
    }

    // stop the threads running
    this->m_initialized = false;
}

/**
 * Frees a sample buffer allocated with av_samples_alloc_array_and_samples().
 *
 * A planar format gives every channel its own plane, a packed one keeps the
 * whole buffer in the first slot. av_samples_free() cannot be used here: it
 * treats audio_data[0] as a plane pointer, which is wrong for packed buffers.
 */
static void free_resampled_data (uint8_t**& resampled_data, const int nb_channels, const AVSampleFormat format) {
    if (resampled_data == nullptr) {
	return;
    }

    if (av_sample_fmt_is_planar (format)) {
	for (int i = 0; i < nb_channels; i++) {
	    av_freep (&resampled_data[i]);
	}
    } else {
	av_freep (&resampled_data[0]);
    }

    av_freep (&resampled_data);
}

int AudioStream::resampleAudio (uint8_t* out_buf, const int out_size) {
    int out_linesize = 0;
    int ret;
    uint8_t** resampled_data = nullptr;
    int resampled_data_size;

    // retrieve number of audio samples (per channel)
    const int in_nb_samples = this->m_decodeFrame->nb_samples;
    if (in_nb_samples <= 0) {
	sLog.error ("in_nb_samples error.");
	return -1;
    }

    // swr_get_out_samples() is the documented upper bound of the samples the
    // next swr_convert() call produces for in_nb_samples input samples: it
    // accounts for the resampler's internal delay.
    const int max_out_nb_samples = swr_get_out_samples (this->m_swrctx, in_nb_samples);

    // check the sample count was correctly retrieved
    if (max_out_nb_samples <= 0) {
	sLog.error ("swr_get_out_samples error.");
	return -1;
    }

    // The resampler always writes the DEVICE layout, no matter what the
    // decoded track holds: a mono track on a stereo device is upmixed to two
    // channels. The destination buffer MUST be sized with that same count.
    // Sizing it with the DECODER's channel count (what this function used to
    // do) under-allocates mono sources on a stereo device and the resampler
    // then writes past the end of the buffer, corrupting the next heap chunk
    // and aborting the process with "free(): invalid next size (normal)".
    //
    // The decision lives in computeResampleBufferSizing() so it can be pinned
    // down by the test suite: the decoder's channel count is handed over to it
    // on purpose, and it is the one that must not influence the result.
    const auto sizing = computeResampleBufferSizing (
	this->m_out_nb_channels, get_frame_nb_channels (this->m_decodeFrame), max_out_nb_samples,
	av_get_bytes_per_sample (this->m_audioContext.getFormat ()), RESAMPLE_BUFFER_ALIGNMENT
    );

    if (!sizing.valid) {
	sLog.error ("Cannot compute the size of the resampler destination buffer.");
	return -1;
    }

    ret = av_samples_alloc_array_and_samples (
	&resampled_data, &out_linesize, sizing.channels, sizing.max_out_nb_samples, this->m_audioContext.getFormat (), 0
    );

    if (ret < 0) {
	sLog.error ("av_samples_alloc_array_and_samples() error: Could not allocate destination samples.");
	return -1;
    }

    // the allocation has to be able to hold the whole resampled chunk
    const int allocated_bytes = av_samples_get_buffer_size (
	nullptr, sizing.channels, sizing.max_out_nb_samples, this->m_audioContext.getFormat (), 0
    );

    if (allocated_bytes < sizing.buffer_bytes) {
	sLog.error ("The resampler destination buffer is smaller than the data it has to hold.");
	free_resampled_data (resampled_data, sizing.channels, this->m_audioContext.getFormat ());
	return -1;
    }

    // do the actual audio data resampling
    ret = swr_convert (
	this->m_swrctx, resampled_data, sizing.max_out_nb_samples, const_cast<const uint8_t**> (this->m_decodeFrame->data),
	this->m_decodeFrame->nb_samples
    );

    // check audio conversion was successful
    if (ret < 0) {
	sLog.error ("swr_convert_error.");
	free_resampled_data (resampled_data, sizing.channels, this->m_audioContext.getFormat ());
	return -1;
    }

    // Get the required buffer size for the given audio parameters
    resampled_data_size
	= av_samples_get_buffer_size (&out_linesize, sizing.channels, ret, this->m_audioContext.getFormat (), 1);

    // check audio buffer size
    if (resampled_data_size < 0) {
	sLog.error ("av_samples_get_buffer_size error.");
	free_resampled_data (resampled_data, sizing.channels, this->m_audioContext.getFormat ());
	return -1;
    }

    // the resampler must never write more than what the destination buffer holds
    if (resampled_data_size > sizing.buffer_bytes) {
	sLog.error ("The resampler wrote more data than the destination buffer holds.");
	free_resampled_data (resampled_data, sizing.channels, this->m_audioContext.getFormat ());
	return -1;
    }

    // the driver mixes interleaved samples, a planar buffer cannot be copied
    // over as-is
    if (av_sample_fmt_is_planar (this->m_audioContext.getFormat ())) {
	sLog.error ("Planar output format is not supported by the audio driver.");
	free_resampled_data (resampled_data, sizing.channels, this->m_audioContext.getFormat ());
	return -1;
    }

    // copy the resampled data to the output buffer up to out_size bytes
    const int copy_size = std::min (resampled_data_size, out_size);
    memcpy (out_buf, resampled_data[0], copy_size);

    // memory cleanup
    free_resampled_data (resampled_data, sizing.channels, this->m_audioContext.getFormat ());

    // the driver mixes exactly this many bytes out of the buffer
    return copy_size;
}

void AudioStream::flushCodec () {
    // The decoder context is shared between the reader thread (which flushes
    // it when the stream loops back to its start) and the audio thread (which
    // decodes frames out of it): the flush is serialized against the decode
    // calls through the codec mutex.
    CodecLock lock (this->m_codecMutex);

    avcodec_flush_buffers (this->getContext ());
}

int AudioStream::decodeFrame (uint8_t* audioBuffer, const int bufferSize) {
    static int audio_pkt_size = 0;

    // block until there's any data in the buffers
    while (this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
	while (audio_pkt_size > 0 && this->m_audioContext.getApplicationContext ().state.general.keepRunning) {
	    int got_frame = 0;
	    int ret = 0;

	    {
		// the decoder context is shared with the reader thread, which
		// flushes it when the stream loops back to the start: don't
		// decode while it is being flushed
		CodecLock lock (this->m_codecMutex);

		ret = avcodec_receive_frame (this->getContext (), this->m_decodeFrame);

		if (ret == 0) {
		    got_frame = 1;
		}
		if (ret == AVERROR (EAGAIN)) {
		    ret = 0;
		}
		if (ret == 0) {
		    ret = avcodec_send_packet (this->getContext (), this->m_decodePacket);
		}
	    }

	    if (ret < 0 && ret != AVERROR (EAGAIN)) {
		return -1;
	    }

	    if (this->m_decodePacket->size < 0) {
		// if error, skip frame
		audio_pkt_size = 0;
		break;
	    }

	    audio_pkt_size -= this->m_decodePacket->size;
	    int data_size = 0;

	    if (got_frame) {
		// audio resampling
		data_size = this->resampleAudio (audioBuffer, bufferSize);
	    }
	    if (data_size <= 0) {
		// no data found, keep waiting
		continue;
	    }
	    // some data was found
	    return data_size;
	}

	if (this->m_decodePacket->data) {
	    av_packet_unref (this->m_decodePacket);
	}

	this->dequeuePacket ();

	audio_pkt_size = this->m_decodePacket->size;
    }

    return 0;
}

AudioContext& AudioStream::getAudioContext () const { return this->m_audioContext; }