#pragma once

#include <utility>

#include "encode/video_encoder.h"
#include "services/interfaces.h"

// Thin adapters wrapping each existing module's real implementation behind
// the Phase 4 interfaces (interfaces.h), so main_cloudrag.cpp's own
// production wiring (see ServiceContainer usage in main()) goes through
// the same seam a test's mocks would. None of these change the wrapped
// module's behavior -- they exist purely to satisfy the interface.

class SapiNarrationEngine : public INarrationEngine {
public:
    NarrationResult synthesize(const QString& text, const QString& wavPath) override {
        return NarrationEngine::synthesize(text, wavPath);
    }
};

// Wraps an already-constructed CloudRagClient (see CloudRagClient::
// fromEnvironment(), which can fail if CLOUD_RAG_URL/CLOUD_RAG_API_KEY
// aren't set -- callers only construct this adapter once that succeeds).
class CloudRagVectorStoreClient : public IVectorStoreClient {
public:
    explicit CloudRagVectorStoreClient(CloudRagClient client) : client_(std::move(client)) {}

    CloudRagResponse query(const QString& queryText, const QString& dbKey) override {
        return client_.query(queryText, dbKey);
    }
    QStringList listAllowedNamespaces() override { return client_.listAllowedNamespaces(); }

private:
    CloudRagClient client_;
};

class FfmpegVideoEncoder : public IVideoEncoder {
public:
    FfmpegVideoEncoder(const std::string& outputPath, int width, int height, int fps,
                        const std::string& audioWavPath)
        : encoder_(outputPath, width, height, fps, audioWavPath) {}

    void pushFrame(const uint8_t* rgba) override { encoder_.pushFrame(rgba); }
    void writeAudioTrack() override { encoder_.writeAudioTrack(); }
    void finish() override { encoder_.finish(); }

private:
    VideoEncoder encoder_;
};

class FfmpegVideoEncoderFactory : public IVideoEncoderFactory {
public:
    std::unique_ptr<IVideoEncoder> create(const std::string& outputPath, int width, int height,
                                           int fps, const std::string& audioWavPath) override {
        return std::make_unique<FfmpegVideoEncoder>(outputPath, width, height, fps, audioWavPath);
    }
};

class LocalManifestWriter : public IManifestWriter {
public:
    void publish(const QString& outputDir, const ManifestEntryInfo& entry,
                 const ManifestVideoDetail& detail, const QString& videoFilePath,
                 const QImage& thumbnail) override {
        ManifestWriter::publish(outputDir, entry, detail, videoFilePath, thumbnail);
    }
};
