#pragma once

#include <memory>

#include "services/interfaces.h"

// Lightweight service registry for IMPROVEMENT_PLAN.md Phase 4. No
// framework -- just constructor-time registration and getter access.
// Scope note (IMPROVEMENT_PLAN.md Phase 0 "設計書との乖離" #8): this
// container is used by main_cloudrag.cpp (the video-generation engine)
// only. RAGReel.exe's launcher (main_launcher.cpp, engine/src/launcher/)
// is a separate executable with its own ProcessRunner/NamespaceLister/
// LauncherSettings wiring and is untouched by this class.
//
// vectorStoreClient() can legitimately be null: CloudRagClient::
// fromEnvironment() (see concrete_services.h's CloudRagVectorStoreClient)
// fails when CLOUD_RAG_URL/CLOUD_RAG_API_KEY aren't set, which is a normal,
// expected state under --mock or a misconfigured environment -- callers
// must null-check before use, matching how main_cloudrag.cpp already
// treated CloudRagClient::fromEnvironment()'s std::optional before this
// phase.
class ServiceContainer {
public:
    void registerNarrationEngine(std::unique_ptr<INarrationEngine> impl) {
        narrationEngine_ = std::move(impl);
    }
    void registerVectorStoreClient(std::unique_ptr<IVectorStoreClient> impl) {
        vectorStoreClient_ = std::move(impl);
    }
    void registerVideoEncoderFactory(std::unique_ptr<IVideoEncoderFactory> impl) {
        videoEncoderFactory_ = std::move(impl);
    }
    void registerManifestWriter(std::unique_ptr<IManifestWriter> impl) {
        manifestWriter_ = std::move(impl);
    }

    INarrationEngine& narrationEngine() { return *narrationEngine_; }
    IVectorStoreClient* vectorStoreClient() { return vectorStoreClient_.get(); }  // nullable, see above
    IVideoEncoderFactory& videoEncoderFactory() { return *videoEncoderFactory_; }
    IManifestWriter& manifestWriter() { return *manifestWriter_; }

private:
    std::unique_ptr<INarrationEngine> narrationEngine_;
    std::unique_ptr<IVectorStoreClient> vectorStoreClient_;
    std::unique_ptr<IVideoEncoderFactory> videoEncoderFactory_;
    std::unique_ptr<IManifestWriter> manifestWriter_;
};
