#pragma once

#include <atomic>
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
// Video: VP9. Audio: Opus. Container: WebM. Chosen over H.264/AAC/MP4
// because those are patent-encumbered codecs that most Qt WebEngine builds
// (including Houdini's embedded browser) ship without decode support for --
// VP9/Opus are royalty-free and decode everywhere Chromium/WebEngine runs,
// so the same file plays in the web dashboard and inside Houdini
// (2026-08-28; see docs/architecture/video-factory-design.md).
class VideoEncoder {
public:
    // If audioWavPath is non-empty, a second (Opus) stream is added and the
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

    // pushFrame を 2 段に分けたもの。別々のスレッドで呼んでよい(変換のスレッド 1 本、エンコードのスレッド 1 本。
    // どちらも、同じスレッドからだけ呼ぶこと)。
    //   convertFrame: RGBA -> YUV420 の変換(sws_scale)。pts は付けない。
    //   encodeConverted: 変換済みの画面を VP9 にエンコードして出力する。pts は呼んだ順に振る。
    //                    同じ画面を何度渡してもよい(静止したスライドの繰り返し。中身は変えない)。
    // pushFrame(rgba) は convertFrame -> encodeConverted を続けて呼ぶのと同じ。
    std::shared_ptr<AVFrame> convertFrame(const uint8_t* rgba);
    void encodeConverted(const std::shared_ptr<AVFrame>& yuv);

    // 時間の内訳(ナノ秒)。エンコード用のスレッドが書き、別のスレッドからいつ読んでもよい(アトミック)。
    long long swsNs() const { return swsNs_.load(std::memory_order_relaxed); }      // RGBA -> YUV420 の変換(sws_scale)
    long long codecNs() const { return codecNs_.load(std::memory_order_relaxed); }  // VP9 のエンコード + 出力
    long long swsFrames() const { return swsFrames_.load(std::memory_order_relaxed); }
    long long codecFrames() const { return codecFrames_.load(std::memory_order_relaxed); }

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
    std::atomic<long long> swsNs_{0};
    std::atomic<long long> codecNs_{0};
    std::atomic<long long> swsFrames_{0};
    std::atomic<long long> codecFrames_{0};

    AVFormatContextPtr formatCtx_;
    AVCodecContextPtr codecCtx_;
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
