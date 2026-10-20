#pragma once

#include <cstdio>
#include <memory>
#include <utility>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>

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
// final result -- and recorded in NarrationResult::engineName, so the log and
// the Houdini panel can say that the old voice was used and why.
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
            NarrationResult r = fallback_->synthesize(text, wavPath);
            r.engineName += QStringLiteral("（VOICEVOXが失敗したため切替: %1）").arg(QString::fromUtf8(e.what()));
            return r;
        }
    }

private:
    std::unique_ptr<INarrationEngine> primary_;
    std::unique_ptr<INarrationEngine> fallback_;
};

// Starts an already-installed VOICEVOX ENGINE when nothing answers yet, so that installing
// VOICEVOX is the only step a user has to do. Looks at RAGREEL_VOICEVOX_EXE, then the
// per-user install folder. Returns true if the engine answers within `waitMs`.
// Never installs or downloads anything.
inline bool tryStartInstalledVoicevox(const VoicevoxConfig& config, QString* detail, int waitMs = 90000) {
    QStringList candidates;
    const QString fromEnv = qEnvironmentVariable("RAGREEL_VOICEVOX_EXE");
    if (!fromEnv.isEmpty()) candidates << fromEnv;
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty()) {
        candidates << QDir(local).filePath(QStringLiteral("Programs/VOICEVOX/vv-engine/run.exe"));
        candidates << QDir(local).filePath(QStringLiteral("Programs/voicevox_engine/run.exe"));
    }
    QString exe;
    for (const QString& c : candidates) {
        if (QFileInfo::exists(c)) {
            exe = c;
            break;
        }
    }
    if (exe.isEmpty()) {
        if (detail) *detail = QStringLiteral("VOICEVOXが見つかりません（未導入）");
        return false;
    }
    QStringList args;
    const QUrl url(config.baseUrl);
    if (url.port() > 0) args << QStringLiteral("--port") << QString::number(url.port());
    // The engine must not inherit our stdout/stderr: the Houdini panel runs this exe with its output
    // going to a log file, and the engine's start-up chatter (hundreds of progress lines) would end up
    // in that log and keep the file handle open for as long as the engine runs.
    QProcess launcher;
    launcher.setProgram(exe);
    launcher.setArguments(args);
    launcher.setWorkingDirectory(QFileInfo(exe).absolutePath());
    launcher.setStandardOutputFile(QProcess::nullDevice());
    launcher.setStandardErrorFile(QProcess::nullDevice());
    launcher.setStandardInputFile(QProcess::nullDevice());
    if (!launcher.startDetached()) {
        if (detail) *detail = QStringLiteral("VOICEVOXを起動できませんでした: %1").arg(exe);
        return false;
    }
    VoicevoxNarrationEngine probe(config);
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < waitMs) {
        if (probe.isAvailable(1500)) {
            if (detail) *detail = QStringLiteral("VOICEVOXを起動しました（%1）").arg(exe);
            return true;
        }
        QThread::msleep(1000);
    }
    if (detail) *detail = QStringLiteral("VOICEVOXを起動しましたが、%1秒待っても応答しません").arg(waitMs / 1000);
    return false;
}

// RAGREEL_TTS_BACKEND = auto (default) | voicevox | sapi
//   auto     : VOICEVOX if the engine answers (or an installed one can be started),
//              otherwise SAPI (per-call fallback).
//   voicevox : VOICEVOX with SAPI as per-call fallback (explicit intent; warns if unreachable).
//   sapi     : SAPI only (the previous behaviour).
// `note` (optional) receives one line saying which voice will be used and why -- logged by
// main_cloudrag.cpp, so a silent fall-back to the old SAPI voice is always visible.
inline std::unique_ptr<INarrationEngine> makeNarrationEngineFromEnvironment(QString* note = nullptr) {
    const QString mode = qEnvironmentVariable("RAGREEL_TTS_BACKEND", QStringLiteral("auto")).toLower();
    if (mode == QStringLiteral("sapi")) {
        if (note) *note = QStringLiteral("RAGREEL_TTS_BACKEND=sapi のため、Windows標準の音声（SAPI）を使います");
        return std::make_unique<SapiNarrationEngine>();
    }
    VoicevoxConfig config = VoicevoxConfig::fromEnvironment();
    VoicevoxNarrationEngine probe(config);
    QString startDetail;
    if (!probe.isAvailable() && !tryStartInstalledVoicevox(config, &startDetail)) {
        if (mode == QStringLiteral("voicevox")) {
            std::fprintf(stderr,
                         "[narration] RAGREEL_TTS_BACKEND=voicevox but %s is unreachable; using SAPI\n",
                         config.baseUrl.toUtf8().constData());
        }
        if (note) {
            *note = QStringLiteral(
                        "VOICEVOXに接続できないため、Windows標準の音声（SAPI）を使います（%1。%2）。"
                        "自然な声にするには、VOICEVOXを導入して起動してください（docs/narration-quality.md）")
                        .arg(config.baseUrl, startDetail.isEmpty() ? QStringLiteral("起動していません") : startDetail);
        }
        return std::make_unique<SapiNarrationEngine>();
    }
    if (note) {
        *note = startDetail.isEmpty() ? QStringLiteral("VOICEVOXを使います（%1）").arg(config.baseUrl)
                                      : startDetail;
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
