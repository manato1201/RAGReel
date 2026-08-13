#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <string>

#include "manifest/manifest_writer.h"
#include "narration/narration_engine.h"
#include "ragclient/cloud_rag_client.h"

// Service interfaces for IMPROVEMENT_PLAN.md Phase 4 ("サービスコンテナ
//化"). Each interface's sole purpose is letting Orchestrator/main() depend
// on an abstraction instead of a concrete class, so a test can substitute a
// mock/fake implementation without a network connection, SAPI voice, or
// filesystem write. None of the underlying modules (NarrationEngine,
// CloudRagClient, VideoEncoder, ManifestWriter) were changed to support
// this -- their public API signatures are untouched (IMPROVEMENT_PLAN.md
// Phase 0's rule); concrete_services.h instead adapts each one to its
// matching interface here.
//
// IVideoEncoder is deliberately NOT constructed and registered once like
// the other three: VideoEncoder's constructor takes per-job parameters
// (output path, dimensions, fps, audio path) that aren't known until deep
// into a job (the audio path depends on narration synthesis having already
// run) -- so ServiceContainer holds an IVideoEncoderFactory instead of an
// IVideoEncoder, and callers create one encoder per job from it. This is a
// deliberate departure from the design doc's single-registered-instance
// sketch for this one service, driven by VideoEncoder's actual
// constructor shape.

class INarrationEngine {
public:
    virtual ~INarrationEngine() = default;
    virtual NarrationResult synthesize(const QString& text, const QString& wavPath) = 0;
};

class IVectorStoreClient {
public:
    virtual ~IVectorStoreClient() = default;
    virtual CloudRagResponse query(const QString& queryText, const QString& dbKey) = 0;
    virtual QStringList listAllowedNamespaces() = 0;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;
    virtual void pushFrame(const uint8_t* rgba) = 0;
    virtual void writeAudioTrack() = 0;
    virtual void finish() = 0;
};

class IVideoEncoderFactory {
public:
    virtual ~IVideoEncoderFactory() = default;
    virtual std::unique_ptr<IVideoEncoder> create(const std::string& outputPath, int width,
                                                   int height, int fps,
                                                   const std::string& audioWavPath) = 0;
};

class IManifestWriter {
public:
    virtual ~IManifestWriter() = default;
    virtual void publish(const QString& outputDir, const ManifestEntryInfo& entry,
                          const ManifestVideoDetail& detail, const QString& videoFilePath,
                          const QImage& thumbnail) = 0;
};
