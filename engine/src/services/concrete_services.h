#pragma once

#include <cstdio>
#include <memory>
#include <utility>

#include <QString>

#include "encode/video_encoder.h"
#include "narration/voicevox_narration_engine.h"
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

// VOICEVOX backend (docs/narration-quality.md). Only constructed when the
// local engine answers /version, so a missing engine never changes behaviour.
class VoicevoxNarrationAdapter : public INarrationEngine {
public:
    explicit VoicevoxNarrationAdapter(VoicevoxConfig config) : engine_(std::move(config)) {
        engine_.registerUserDictionary();  // best effort, never throws
    }
    NarrationResult synthesize(const QString& text, const QString& wavPath) override {
        return engine_.synthesize(text, wavPath);
    }

private:
    VoicevoxNarrationEngine engine_;
};

// "Try the better voice, never lose the video": if the primary backend
// throws (engine stopped mid-job, speaker not found, ...), the same text is
// synthesised with SAPI so a job still gets narration. The failure is
// reported on stderr -- main_cloudrag.cpp's own logLine() only sees the
// final result.
class FallbackNarrationEngine : public INarrationEngine {
public:
    FallbackNarrationEngine(std::unique_ptr<INarrationEngine> primary,
                            std::unique_ptr<INarrationEngine> fallback)
        : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

    NarrationResult synthesize(const QString& text, const QString& wavPath) override {
        try {
            return primary_->synthesize(text, wavPath);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[narration] primary backend failed (%s); falling back to SAPI\n", e.what());
            return fallback_->synthesize(text, wavPath);
        }
    }

private:
    std::unique_ptr<INarrationEngine> primary_;
    std::unique_ptr<INarrationEngine> fallback_;
};

// RAGREEL_TTS_BACKEND = auto (default) | voicevox | sapi
//   auto     : VOICEVOX if the engine answers, otherwise SAPI (per-call fallback).
//   voicevox : VOICEVOX with SAPI as per-call fallback (explicit intent; warns if unreachable).
//   sapi     : SAPI only (the previous behaviour).
inline std::unique_ptr<INarrationEngine> makeNarrationEngineFromEnvironment() {
    const QString mode = qEnvironmentVariable("RAGREEL_TTS_BACKEND", QStringLiteral("auto")).toLower();
    if (mode == QStringLiteral("sapi")) {
        return std::make_unique<SapiNarrationEngine>();
    }
    VoicevoxConfig config = VoicevoxConfig::fromEnvironment();
    VoicevoxNarrationEngine probe(config);
    if (!probe.isAvailable()) {
        if (mode == QStringLiteral("voicevox")) {
            std::fprintf(stderr,
                         "[narration] RAGREEL_TTS_BACKEND=voicevox but %s is unreachable; using SAPI\n",
                         config.baseUrl.toUtf8().constData());
        }
        return std::make_unique<SapiNarrationEngine>();
    }
    return std::make_unique<FallbackNarrationEngine>(
        std::make_unique<VoicevoxNarrationAdapter>(std::move(config)),
        std::make_unique<SapiNarrationEngine>());
}

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
