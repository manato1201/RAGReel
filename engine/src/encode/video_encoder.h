#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// RAII wrapper around libavformat/libavcodec per
// docs/architecture/video-factory-design.md §4: every FFmpeg C handle is
// owned by exactly one smart pointer with a custom deleter, never a raw
// pointer crossing an ownership boundary. Reused AVFrame/AVPacket instead of
// per-frame allocation in the encode loop.
class VideoEncoder {
public:
    // If audioWavPath is non-empty, a second (AAC) stream is added and the
    // whole WAV file is muxed in alongside the video frames pushed via
    // pushFrame(); interleaving/ordering is handled by
    // av_interleaved_write_frame regardless of the order the two streams'
    // packets are produced in.
    VideoEncoder(const std::string& outputPath, int width, int height, int fps,
                 const std::string& audioWavPath = {});
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;

    // Encodes one RGBA frame (tightly packed, width*4 bytes per row).
    void pushFrame(const uint8_t* rgba);

    // Encodes and writes the whole audio track set up in the constructor.
    // No-op if the encoder was constructed without an audioWavPath. Must be
    // called before finish(); order relative to pushFrame() calls does not
    // matter for correctness (see interleaving note above).
    void writeAudioTrack();

    // Flushes the encoder(s) and writes the trailer. Must be called exactly
    // once before destruction for a valid output file.
    void finish();

private:
    struct AVFormatContextDeleter { void operator()(AVFormatContext* ctx) const; };
    struct AVCodecContextDeleter { void operator()(AVCodecContext* ctx) const; };
    struct AVFrameDeleter { void operator()(AVFrame* frame) const; };
    struct AVPacketDeleter { void operator()(AVPacket* pkt) const; };
    struct SwsContextDeleter { void operator()(SwsContext* ctx) const; };
    struct SwrContextDeleter { void operator()(SwrContext* ctx) const; };

    using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
    using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;
    using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
    using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
    using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
    using SwrContextPtr = std::unique_ptr<SwrContext, SwrContextDeleter>;

    void encodeAndWrite(AVFrame* frame);
    void setupAudioStream(const std::string& audioWavPath);
    void drainAudioPackets(AVPacket* packet);

    int width_;
    int height_;
    int fps_;
    int64_t nextPts_ = 0;
    bool finished_ = false;
    bool audioWritten_ = false;

    AVFormatContextPtr formatCtx_;
    AVCodecContextPtr codecCtx_;
    AVFramePtr frame_;
    AVPacketPtr packet_;
    SwsContextPtr swsCtx_;
    AVStream* stream_ = nullptr; // non-owning, lifetime tied to formatCtx_

    // Audio (optional; only set up when constructed with an audioWavPath).
    AVCodecContextPtr audioCodecCtx_;
    AVFramePtr audioFrame_;
    SwrContextPtr swrCtx_;
    AVStream* audioStream_ = nullptr; // non-owning, lifetime tied to formatCtx_
    std::vector<uint8_t> audioPcm_;
    int audioBytesPerSample_ = 0; // bytes per multi-channel sample frame in audioPcm_
};
