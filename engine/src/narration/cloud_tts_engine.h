#pragma once

#include <QString>

#include <utility>

#include "narration/narration_engine.h"  // NarrationResult only

// Cloud narration backend (2026-10-20): POST {CLOUD_RAG_URL}/tts with the same API key the
// Houdini panel and this engine already use for Cloud RAG. The Worker holds the Google Cloud
// Text-to-Speech key, enforces the per-key / global daily character caps and caches repeated
// text, so a tester needs no VOICEVOX install and no extra key.
//
// Output matches the VOICEVOX engine (44.1kHz mono 16-bit WAV, per-chunk segment timing), so
// everything downstream -- per-slide narration, WAV concat, subtitles -- is unchanged.
struct CloudTtsConfig {
    QString baseUrl;                     // e.g. https://rag-poc.xxxx.workers.dev (no trailing slash)
    QString apiKey;
    QString voice;                       // empty = the Worker's default voice
    double speakingRate = 1.0;
    double chunkGapSeconds = 0.25;       // silence inserted between chunks
    int maxChunkChars = 200;             // the Worker accepts up to 1200 characters per request
    int sampleRate = 44100;
    int timeoutMs = 60000;

    bool isConfigured() const { return !baseUrl.isEmpty() && !apiKey.isEmpty(); }

    // RAGREEL_CLOUD_TTS_URL / RAGREEL_CLOUD_TTS_KEY (set by the Houdini panel), else CLOUD_RAG_URL /
    // CLOUD_RAG_API_KEY (the launcher, developers); RAGREEL_CLOUD_TTS_VOICE (e.g. ja-JP-Chirp3-HD-Aoede),
    // RAGREEL_CLOUD_TTS_RATE (0.8-1.3)
    static CloudTtsConfig fromEnvironment();
};

class CloudTtsEngine {
public:
    explicit CloudTtsEngine(CloudTtsConfig config) : config_(std::move(config)) {}

    // Throws std::runtime_error when the Worker is unreachable, refuses (daily cap, bad key, the
    // Google key not set up) or returns audio that cannot be read. The message always includes the
    // HTTP status and the Worker's error text, so the fallback note can show why.
    NarrationResult synthesize(const QString& text, const QString& wavPath) const;

private:
    CloudTtsConfig config_;
};
