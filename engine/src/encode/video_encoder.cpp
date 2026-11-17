#include "video_encoder.h"

#include "video_profile.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

void VideoEncoder::AVFormatContextDeleter::operator()(AVFormatContext* ctx) const {
    if (!ctx) return;
    if (!(ctx->oformat->flags & AVFMT_NOFILE) && ctx->pb) {
        avio_closep(&ctx->pb);
    }
    avformat_free_context(ctx);
}

void VideoEncoder::AVCodecContextDeleter::operator()(AVCodecContext* ctx) const {
    avcodec_free_context(&ctx);
}

void VideoEncoder::AVFrameDeleter::operator()(AVFrame* frame) const {
    av_frame_free(&frame);
}

void VideoEncoder::AVPacketDeleter::operator()(AVPacket* pkt) const {
    av_packet_free(&pkt);
}

void VideoEncoder::SwsContextDeleter::operator()(SwsContext* ctx) const {
    sws_freeContext(ctx);
}

void VideoEncoder::SwrContextDeleter::operator()(SwrContext* ctx) const {
    swr_free(&ctx);
}

namespace {

void throwOnError(int ret, const char* what) {
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, buf, sizeof(buf));
        throw std::runtime_error(std::string(what) + ": " + buf);
    }
}

// 環境変数の整数(未設定・読めないときは既定値)
int envInt(const char* name, int defaultValue) {
    const char* v = std::getenv(name);
    if (!v || !*v) return defaultValue;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return (end && *end == '\0') ? static_cast<int>(n) : defaultValue;
}

struct AudioFifoDeleter {
    void operator()(AVAudioFifo* fifo) const {
        if (fifo) av_audio_fifo_free(fifo);
    }
};
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

struct WavInfo {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    std::vector<uint8_t> pcmData;
};

// Minimal chunk-scanning WAV reader (RIFF/WAVE, PCM). Good enough for the
// files NarrationEngine produces via SAPI, without assuming a fixed 44-byte
// header layout.
WavInfo readWavFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Cannot open WAV file: " + path);
    }

    char riffId[4];
    file.read(riffId, 4);
    file.seekg(4, std::ios::cur); // RIFF chunk size, unused
    char waveId[4];
    file.read(waveId, 4);
    if (std::strncmp(riffId, "RIFF", 4) != 0 || std::strncmp(waveId, "WAVE", 4) != 0) {
        throw std::runtime_error("Not a RIFF/WAVE file: " + path);
    }

    WavInfo info;
    while (file) {
        char chunkId[4];
        file.read(chunkId, 4);
        if (!file) break;
        uint32_t chunkSize = 0;
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!file) break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat = 0, numChannels = 0, blockAlign = 0, bitsPerSample = 0;
            uint32_t sampleRate = 0, byteRate = 0;
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&numChannels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            file.read(reinterpret_cast<char*>(&byteRate), 4);
            file.read(reinterpret_cast<char*>(&blockAlign), 2);
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            info.channels = numChannels;
            info.sampleRate = static_cast<int>(sampleRate);
            info.bitsPerSample = bitsPerSample;
            const uint32_t consumed = 16;
            if (chunkSize > consumed) {
                file.seekg(chunkSize - consumed, std::ios::cur);
            }
        } else if (std::strncmp(chunkId, "data", 4) == 0) {
            info.pcmData.resize(chunkSize);
            file.read(reinterpret_cast<char*>(info.pcmData.data()), chunkSize);
        } else {
            file.seekg(chunkSize, std::ios::cur);
        }
        if (chunkSize % 2 == 1) {
            file.seekg(1, std::ios::cur); // chunks are word-aligned
        }
    }

    if (info.pcmData.empty() || info.sampleRate == 0 || info.channels == 0) {
        throw std::runtime_error("WAV file has no usable fmt/data chunks: " + path);
    }
    return info;
}

} // namespace

VideoEncoder::VideoEncoder(const std::string& outputPath, int width, int height, int fps,
                            const std::string& audioWavPath)
    : width_(width), height_(height), fps_(fps) {
    AVFormatContext* rawFormatCtx = nullptr;
    // "webm" is passed explicitly rather than left to extension-guessing:
    // the plain Matroska muxer would also match a ".webm"/".mkv" name, and
    // player expectations (in particular Qt WebEngine builds without
    // proprietary-codec support -- see below) assume the strict WebM
    // profile, not arbitrary Matroska.
    throwOnError(
        avformat_alloc_output_context2(&rawFormatCtx, nullptr, "webm", outputPath.c_str()),
        "avformat_alloc_output_context2");
    formatCtx_.reset(rawFormatCtx);

    // VP9/Opus (WebM), not H.264/AAC (MP4): Houdini's embedded browser is
    // Qt WebEngine, and most Qt WebEngine builds (including Houdini's) are
    // compiled without the proprietary-codec patent license that H.264/AAC
    // decode requires, so generated tutorial videos played silently/blank
    // there while working fine in an external browser or YouTube (which
    // itself falls back to VP9/Opus). VP9+Opus are royalty-free and decode
    // in every Chromium/WebEngine build regardless of that flag, so this
    // removes the web/app playback gap entirely (2026-08-28).
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
    if (!codec) {
        throw std::runtime_error("VP9 encoder not available in this FFmpeg build (vcpkg ffmpeg needs the \"vpx\" feature)");
    }

    stream_ = avformat_new_stream(formatCtx_.get(), nullptr);
    if (!stream_) {
        throw std::runtime_error("avformat_new_stream failed");
    }

    codecCtx_.reset(avcodec_alloc_context3(codec));
    if (!codecCtx_) {
        throw std::runtime_error("avcodec_alloc_context3 failed");
    }

    codecCtx_->width = width_;
    codecCtx_->height = height_;
    codecCtx_->time_base = AVRational{1, fps_};
    codecCtx_->framerate = AVRational{fps_, 1};
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->gop_size = fps_; // one keyframe per second
    if (formatCtx_->oformat->flags & AVFMT_GLOBALHEADER) {
        codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    // bit_rate stays 0 (the AVCodecContext default) so libvpx uses
    // constant-quality mode driven by crf instead of target-bitrate mode.
    // Speed over compression efficiency (2026-09-26): a real-machine run took ~17
    // minutes to encode a 519s video, with ~3/4 of the render stage spent inside
    // libvpx ("deadline=good, cpu-used=4" is its slow, best-quality preset). These
    // are mostly-static slide/screenshot videos at crf 32, where the realtime
    // preset costs a little file size but no visible quality, and row-mt lets
    // libvpx spread one 720p frame over several cores (it barely parallelises
    // without it). Timing lines in the log ("Timing after N frames") show the split.
    av_opt_set(codecCtx_->priv_data, "deadline", "realtime", 0);
    // crf は画素数で決める(1080p は 30、720p は 32。video_profile.h)。文字が多い画面の縁を保つため。
    av_opt_set(codecCtx_->priv_data, "crf",
               std::to_string(video_profile::crfFor(width_, height_)).c_str(), 0);
    // 速さの設定(2026-11-17 に測定。docs/technical-reference.md §28)。
    //  - thread_count: 以前は未指定(= 0)で、FFmpeg の libvpx は実質 1 スレッドで動いていた(1 フレーム 7.5 ms)。
    //    指定すると 4 スレッドで 4.0 ms、6〜8 スレッドで 3.3〜3.5 ms(それ以上は変わらない)。描画・変換のスレッドも
    //    同時に動くので、コア数 - 2 を上限 8 にする。
    //  - cpu-used: 5〜7 は速さが同じ(7.6 / 7.5 / 7.3 ms)。8 は速いがファイルが 17% 大きく、画素も目に見えて変わるので使わない。
    //  - tile-columns: 2 と 3 は同じ。1 はファイルが 0.7% 小さいが 10% 遅い。
    // crf・解像度は変えていない(画質の設定はそのまま)。測定のため、環境変数 RAGREEL_VP9_CPU_USED /
    // RAGREEL_VP9_TILE_COLUMNS / RAGREEL_VP9_THREADS で上書きできる。
    av_opt_set_int(codecCtx_->priv_data, "cpu-used", envInt("RAGREEL_VP9_CPU_USED", 6), 0);
    av_opt_set(codecCtx_->priv_data, "row-mt", "1", 0);
    av_opt_set_int(codecCtx_->priv_data, "tile-columns", envInt("RAGREEL_VP9_TILE_COLUMNS", 2), 0);
    {
        const int cores = static_cast<int>(std::thread::hardware_concurrency());
        codecCtx_->thread_count = std::max(1, envInt("RAGREEL_VP9_THREADS", std::clamp(cores - 2, 1, 8)));
    }

    throwOnError(avcodec_open2(codecCtx_.get(), codec, nullptr), "avcodec_open2");
    throwOnError(avcodec_parameters_from_context(stream_->codecpar, codecCtx_.get()),
                 "avcodec_parameters_from_context");
    stream_->time_base = codecCtx_->time_base;

    // All streams (including audio) must exist before avformat_write_header.
    if (!audioWavPath.empty()) {
        setupAudioStream(audioWavPath);
    }

    if (!(formatCtx_->oformat->flags & AVFMT_NOFILE)) {
        throwOnError(avio_open(&formatCtx_->pb, outputPath.c_str(), AVIO_FLAG_WRITE), "avio_open");
    }
    throwOnError(avformat_write_header(formatCtx_.get(), nullptr), "avformat_write_header");

    packet_.reset(av_packet_alloc());

    swsCtx_.reset(sws_getContext(width_, height_, AV_PIX_FMT_RGBA, width_, height_,
                                  AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!swsCtx_) {
        throw std::runtime_error("sws_getContext failed");
    }
}

void VideoEncoder::setupAudioStream(const std::string& audioWavPath) {
    WavInfo wav = readWavFile(audioWavPath);
    // swr_alloc_set_opts2 below is hardcoded to AV_SAMPLE_FMT_S16 for the
    // input side; readWavFile() has no resampling/format-conversion of its
    // own, so a WAV with any other bit depth would be silently
    // reinterpreted as 16-bit and produce garbled audio instead of erroring.
    if (wav.bitsPerSample != 16) {
        throw std::runtime_error("Unsupported WAV bit depth (expected 16-bit PCM): " +
                                  audioWavPath + " is " + std::to_string(wav.bitsPerSample) +
                                  "-bit");
    }

    // Looked up by name, not avcodec_find_encoder(AV_CODEC_ID_OPUS): FFmpeg
    // ships its own native experimental Opus encoder alongside libopus, and
    // which one avcodec_find_encoder() returns for the same codec ID
    // depends on internal registration order -- not something to rely on.
    // "libopus" pins it explicitly.
    const AVCodec* audioCodec = avcodec_find_encoder_by_name("libopus");
    if (!audioCodec) {
        throw std::runtime_error("libopus encoder not available in this FFmpeg build (vcpkg ffmpeg needs the \"opus\" feature)");
    }

    audioStream_ = avformat_new_stream(formatCtx_.get(), nullptr);
    if (!audioStream_) {
        throw std::runtime_error("avformat_new_stream (audio) failed");
    }

    audioCodecCtx_.reset(avcodec_alloc_context3(audioCodec));
    if (!audioCodecCtx_) {
        throw std::runtime_error("avcodec_alloc_context3 (audio) failed");
    }

    // Opus only accepts 8/12/16/24/48kHz regardless of the source WAV's
    // native rate (typically 22050/44100Hz from Windows SAPI); 48000 is its
    // highest/most compatible rate. swr_alloc_set_opts2 below already takes
    // separate input/output rates, so this just makes the resample real
    // instead of a no-op.
    audioCodecCtx_->sample_rate = 48000;
    av_channel_layout_default(&audioCodecCtx_->ch_layout, 1); // narration is synthesized mono
    audioCodecCtx_->sample_fmt =
        audioCodec->sample_fmts ? audioCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    audioCodecCtx_->bit_rate = 96000;
    // Must match audioCodecCtx_->sample_rate (forced to 48000 above), not
    // the source WAV's rate -- pts values assigned in writeAudioTrack() are
    // counted in resampled (48kHz) samples, so a time_base derived from the
    // source rate would misreport every packet's timestamp/duration in the
    // muxed WebM.
    audioCodecCtx_->time_base = AVRational{1, audioCodecCtx_->sample_rate};
    if (formatCtx_->oformat->flags & AVFMT_GLOBALHEADER) {
        audioCodecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    throwOnError(avcodec_open2(audioCodecCtx_.get(), audioCodec, nullptr), "avcodec_open2 (audio)");
    throwOnError(avcodec_parameters_from_context(audioStream_->codecpar, audioCodecCtx_.get()),
                 "avcodec_parameters_from_context (audio)");
    audioStream_->time_base = audioCodecCtx_->time_base;

    AVChannelLayout inLayout;
    av_channel_layout_default(&inLayout, wav.channels);
    SwrContext* rawSwr = nullptr;
    throwOnError(swr_alloc_set_opts2(&rawSwr, &audioCodecCtx_->ch_layout, audioCodecCtx_->sample_fmt,
                                      audioCodecCtx_->sample_rate, &inLayout, AV_SAMPLE_FMT_S16,
                                      wav.sampleRate, 0, nullptr),
                  "swr_alloc_set_opts2");
    swrCtx_.reset(rawSwr);
    av_channel_layout_uninit(&inLayout);
    throwOnError(swr_init(swrCtx_.get()), "swr_init");

    audioPcm_ = std::move(wav.pcmData);
    audioBytesPerSample_ = wav.channels * (wav.bitsPerSample / 8);
}

VideoEncoder::~VideoEncoder() {
    if (!finished_) {
        // Best-effort cleanup on an error path; a valid file needs finish()
        // called explicitly so the caller can react to encode failures.
        av_write_trailer(formatCtx_.get());
    }
}

std::shared_ptr<AVFrame> VideoEncoder::convertFrame(const uint8_t* rgba) {
    const auto start = std::chrono::steady_clock::now();
    // 画面ごとに新しいバッファに変換する(エンコードのスレッドが前の画面を使っている間に、次の画面を作れる)
    AVFrame* raw = av_frame_alloc();
    if (!raw) {
        throw std::runtime_error("av_frame_alloc failed");
    }
    std::shared_ptr<AVFrame> yuv(raw, [](AVFrame* f) { av_frame_free(&f); });
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = width_;
    yuv->height = height_;
    throwOnError(av_frame_get_buffer(yuv.get(), 0), "av_frame_get_buffer");

    const uint8_t* srcSlices[1] = {rgba};
    const int srcStride[1] = {width_ * 4};
    sws_scale(swsCtx_.get(), srcSlices, srcStride, 0, height_, yuv->data, yuv->linesize);
    swsNs_ += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    ++swsFrames_;
    return yuv;
}

void VideoEncoder::encodeConverted(const std::shared_ptr<AVFrame>& yuv) {
    const auto start = std::chrono::steady_clock::now();
    // 渡された画面は触らない(同じ画面を繰り返し渡されることがある)。pts を付けるため、参照を増やした別の AVFrame を使う
    // (YUV のバッファは共有で、複製しない)。
    AVFramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::runtime_error("av_frame_alloc failed");
    }
    throwOnError(av_frame_ref(frame.get(), yuv.get()), "av_frame_ref");
    frame->pts = nextPts_++;
    encodeAndWrite(frame.get());
    codecNs_ += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    ++codecFrames_;
}

void VideoEncoder::pushFrame(const uint8_t* rgba) {
    encodeConverted(convertFrame(rgba));
}

void VideoEncoder::encodeAndWrite(AVFrame* frame) {
    throwOnError(avcodec_send_frame(codecCtx_.get(), frame), "avcodec_send_frame");

    while (true) {
        int ret = avcodec_receive_packet(codecCtx_.get(), packet_.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        throwOnError(ret, "avcodec_receive_packet");

        av_packet_rescale_ts(packet_.get(), codecCtx_->time_base, stream_->time_base);
        packet_->stream_index = stream_->index;
        throwOnError(av_interleaved_write_frame(formatCtx_.get(), packet_.get()),
                     "av_interleaved_write_frame");
        av_packet_unref(packet_.get());
    }
}

void VideoEncoder::drainAudioPackets(AVPacket* packet) {
    while (true) {
        int ret = avcodec_receive_packet(audioCodecCtx_.get(), packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        throwOnError(ret, "avcodec_receive_packet (audio)");

        av_packet_rescale_ts(packet, audioCodecCtx_->time_base, audioStream_->time_base);
        packet->stream_index = audioStream_->index;
        throwOnError(av_interleaved_write_frame(formatCtx_.get(), packet),
                     "av_interleaved_write_frame (audio)");
        av_packet_unref(packet);
    }
}

void VideoEncoder::writeAudioTrack() {
    if (!audioCodecCtx_ || audioWritten_) {
        return;
    }
    audioWritten_ = true;

    audioFrame_.reset(av_frame_alloc());

    const int frameSize = audioCodecCtx_->frame_size > 0 ? audioCodecCtx_->frame_size : 1024;
    const int totalInputSamples = audioBytesPerSample_ > 0
        ? static_cast<int>(audioPcm_.size() / audioBytesPerSample_)
        : 0;
    const int channels = audioCodecCtx_->ch_layout.nb_channels;
    const int outBytesPerSample = av_get_bytes_per_sample(audioCodecCtx_->sample_fmt);

    // The source WAV and the 48kHz Opus output rarely share a sample rate
    // (e.g. 22050Hz SAPI audio is a ~2.18x upsample), so a fixed count of
    // *input* samples per iteration does not correspond to a fixed count of
    // *output* samples, and libopus requires exact frame_size chunks.
    // Resampled audio is pushed through an AVAudioFifo and drained in
    // frame_size chunks, decoupling "how many source samples were consumed
    // this iteration" from "how many samples the encoder needs next".
    AudioFifoPtr fifo(av_audio_fifo_alloc(audioCodecCtx_->sample_fmt, channels, frameSize * 4));
    if (!fifo) {
        throw std::runtime_error("av_audio_fifo_alloc failed");
    }

    AVPacketPtr audioPacket(av_packet_alloc());
    int64_t pts = 0;

    auto pushResampledSamples = [&](uint8_t** outData, int produced) {
        if (produced <= 0) return;
        throwOnError(av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(outData), produced),
                     "av_audio_fifo_write");
    };

    auto encodeFramesFromFifo = [&](bool flushRemainder) {
        while (av_audio_fifo_size(fifo.get()) >= frameSize ||
               (flushRemainder && av_audio_fifo_size(fifo.get()) > 0)) {
            const int samplesThisFrame = std::min(frameSize, av_audio_fifo_size(fifo.get()));

            audioFrame_->format = audioCodecCtx_->sample_fmt;
            av_channel_layout_copy(&audioFrame_->ch_layout, &audioCodecCtx_->ch_layout);
            audioFrame_->sample_rate = audioCodecCtx_->sample_rate;
            audioFrame_->nb_samples = frameSize;
            throwOnError(av_frame_get_buffer(audioFrame_.get(), 0), "av_frame_get_buffer (audio)");
            throwOnError(av_frame_make_writable(audioFrame_.get()), "av_frame_make_writable (audio)");
            if (samplesThisFrame < frameSize) {
                // Final, short frame: pad the encoder-required frame_size
                // buffer with silence rather than sending a partially
                // uninitialized tail.
                av_samples_set_silence(audioFrame_->data, 0, frameSize, channels,
                                        audioCodecCtx_->sample_fmt);
            }
            throwOnError(av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(audioFrame_->data),
                                             samplesThisFrame),
                         "av_audio_fifo_read");

            audioFrame_->pts = pts;
            pts += frameSize;

            throwOnError(avcodec_send_frame(audioCodecCtx_.get(), audioFrame_.get()),
                         "avcodec_send_frame (audio)");
            drainAudioPackets(audioPacket.get());

            av_frame_unref(audioFrame_.get());
        }
    };

    int samplesConsumed = 0;
    while (samplesConsumed < totalInputSamples) {
        const int chunk = std::min(frameSize, totalInputSamples - samplesConsumed);
        const uint8_t* inData[1] = {
            audioPcm_.data() + static_cast<size_t>(samplesConsumed) * audioBytesPerSample_};

        const int maxOutSamples = swr_get_out_samples(swrCtx_.get(), chunk);
        throwOnError(maxOutSamples, "swr_get_out_samples");
        // channels is always 1 here (narration is synthesized mono, see
        // setupAudioStream), so a single packed buffer is equivalent to a
        // single-plane planar buffer -- this would need one buffer per
        // channel if mono ever stopped being an invariant.
        std::vector<uint8_t> outBuf(static_cast<size_t>(maxOutSamples) * outBytesPerSample);
        uint8_t* outData[1] = {outBuf.data()};
        const int produced = swr_convert(swrCtx_.get(), outData, maxOutSamples, inData, chunk);
        throwOnError(produced, "swr_convert");
        pushResampledSamples(outData, produced);

        samplesConsumed += chunk;
        encodeFramesFromFifo(/*flushRemainder=*/false);
    }

    // Drain samples swresample buffered internally but hasn't emitted yet
    // (resampler lookahead/filter delay) by calling it once more with a
    // null input, then push everything remaining -- including a final
    // shorter-than-frame_size chunk -- through the encoder.
    const int delaySamples =
        static_cast<int>(swr_get_delay(swrCtx_.get(), audioCodecCtx_->sample_rate));
    if (delaySamples > 0) {
        std::vector<uint8_t> outBuf(static_cast<size_t>(delaySamples) * outBytesPerSample);
        uint8_t* outData[1] = {outBuf.data()};
        const int produced = swr_convert(swrCtx_.get(), outData, delaySamples, nullptr, 0);
        throwOnError(produced, "swr_convert (flush)");
        pushResampledSamples(outData, produced);
    }
    encodeFramesFromFifo(/*flushRemainder=*/true);

    throwOnError(avcodec_send_frame(audioCodecCtx_.get(), nullptr), "avcodec_send_frame (audio flush)");
    drainAudioPackets(audioPacket.get());
}

void VideoEncoder::finish() {
    encodeAndWrite(nullptr); // flush the video encoder
    throwOnError(av_write_trailer(formatCtx_.get()), "av_write_trailer");
    finished_ = true;
}
