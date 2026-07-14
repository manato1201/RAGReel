#include "video_encoder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

extern "C" {
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
    throwOnError(
        avformat_alloc_output_context2(&rawFormatCtx, nullptr, nullptr, outputPath.c_str()),
        "avformat_alloc_output_context2");
    formatCtx_.reset(rawFormatCtx);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        throw std::runtime_error("H.264 encoder not available in this FFmpeg build");
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
    av_opt_set(codecCtx_->priv_data, "preset", "medium", 0);
    av_opt_set(codecCtx_->priv_data, "crf", "20", 0);

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

    frame_.reset(av_frame_alloc());
    frame_->format = codecCtx_->pix_fmt;
    frame_->width = width_;
    frame_->height = height_;
    throwOnError(av_frame_get_buffer(frame_.get(), 0), "av_frame_get_buffer");

    packet_.reset(av_packet_alloc());

    swsCtx_.reset(sws_getContext(width_, height_, AV_PIX_FMT_RGBA, width_, height_,
                                  AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!swsCtx_) {
        throw std::runtime_error("sws_getContext failed");
    }
}

void VideoEncoder::setupAudioStream(const std::string& audioWavPath) {
    const WavInfo wav = readWavFile(audioWavPath);

    const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audioCodec) {
        throw std::runtime_error("AAC encoder not available in this FFmpeg build");
    }

    audioStream_ = avformat_new_stream(formatCtx_.get(), nullptr);
    if (!audioStream_) {
        throw std::runtime_error("avformat_new_stream (audio) failed");
    }

    audioCodecCtx_.reset(avcodec_alloc_context3(audioCodec));
    if (!audioCodecCtx_) {
        throw std::runtime_error("avcodec_alloc_context3 (audio) failed");
    }

    audioCodecCtx_->sample_rate = wav.sampleRate;
    av_channel_layout_default(&audioCodecCtx_->ch_layout, 1); // narration is synthesized mono
    audioCodecCtx_->sample_fmt =
        audioCodec->sample_fmts ? audioCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    audioCodecCtx_->bit_rate = 96000;
    audioCodecCtx_->time_base = AVRational{1, wav.sampleRate};
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

    audioPcm_ = wav.pcmData;
    audioBytesPerSample_ = wav.channels * (wav.bitsPerSample / 8);
}

VideoEncoder::~VideoEncoder() {
    if (!finished_) {
        // Best-effort cleanup on an error path; a valid file needs finish()
        // called explicitly so the caller can react to encode failures.
        av_write_trailer(formatCtx_.get());
    }
}

void VideoEncoder::pushFrame(const uint8_t* rgba) {
    throwOnError(av_frame_make_writable(frame_.get()), "av_frame_make_writable");

    const uint8_t* srcSlices[1] = {rgba};
    const int srcStride[1] = {width_ * 4};
    sws_scale(swsCtx_.get(), srcSlices, srcStride, 0, height_, frame_->data, frame_->linesize);

    frame_->pts = nextPts_++;
    encodeAndWrite(frame_.get());
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
    const int totalSamples = audioBytesPerSample_ > 0
        ? static_cast<int>(audioPcm_.size() / audioBytesPerSample_)
        : 0;

    AVPacketPtr audioPacket(av_packet_alloc());
    int64_t pts = 0;
    int samplesConsumed = 0;
    while (samplesConsumed < totalSamples) {
        const int chunk = std::min(frameSize, totalSamples - samplesConsumed);

        audioFrame_->format = audioCodecCtx_->sample_fmt;
        av_channel_layout_copy(&audioFrame_->ch_layout, &audioCodecCtx_->ch_layout);
        audioFrame_->sample_rate = audioCodecCtx_->sample_rate;
        audioFrame_->nb_samples = chunk;
        throwOnError(av_frame_get_buffer(audioFrame_.get(), 0), "av_frame_get_buffer (audio)");
        throwOnError(av_frame_make_writable(audioFrame_.get()), "av_frame_make_writable (audio)");

        const uint8_t* inData[1] = {
            audioPcm_.data() + static_cast<size_t>(samplesConsumed) * audioBytesPerSample_};
        throwOnError(swr_convert(swrCtx_.get(), audioFrame_->data, chunk, inData, chunk),
                     "swr_convert");

        audioFrame_->pts = pts;
        pts += chunk;
        samplesConsumed += chunk;

        throwOnError(avcodec_send_frame(audioCodecCtx_.get(), audioFrame_.get()),
                     "avcodec_send_frame (audio)");
        drainAudioPackets(audioPacket.get());

        av_frame_unref(audioFrame_.get());
    }

    throwOnError(avcodec_send_frame(audioCodecCtx_.get(), nullptr), "avcodec_send_frame (audio flush)");
    drainAudioPackets(audioPacket.get());
}

void VideoEncoder::finish() {
    encodeAndWrite(nullptr); // flush the video encoder
    throwOnError(av_write_trailer(formatCtx_.get()), "av_write_trailer");
    finished_ = true;
}
