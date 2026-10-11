#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <vector>

#include "narration/narration_engine.h"  // NarrationResult only (no SAPI dependency at link time)

// VOICEVOX ENGINE backend for narration (quality upgrade over SAPI5's
// "Microsoft Haruka Desktop", the only ja-JP voice installed on the dev
// machine). Talks to a locally running VOICEVOX engine over HTTP
// (default http://127.0.0.1:50021): POST /audio_query -> tune -> POST
// /synthesis. No cloud, no API key, no per-call cost.
//
// Everything that does not need a network is exposed as pure functions in
// namespaces `narration_text` / `wav_util` so tests/narration_test.cpp can
// cover it with Qt6::Core only (no engine, no GPU, no display).
//
// Licensing note (see docs/narration-quality.md): the VOICEVOX engine is
// LGPL, but each *voice* has its own terms; the character name used must be
// credited ("VOICEVOX:<キャラクター名>") wherever the video is published.

namespace narration_text {

// Removes things a TTS engine reads badly: URLs -> "リンク", markdown
// emphasis/backticks, 3+ repeated punctuation, runs of whitespace.
QString normalizeForSpeech(const QString& text);

// Splits into sentence-sized chunks (<= maxChars each where possible).
// Splits after 。！？!? and newlines; over-long sentences are cut at 、 and
// finally hard-cut. Empty chunks are dropped. Order is preserved.
QStringList splitIntoChunks(const QString& text, int maxChars = 80);

// Picks a style id from GET /speakers JSON. Matches speaker name exactly or
// by substring; style name exactly (empty styleName = first style).
std::optional<int> resolveSpeakerId(const QByteArray& speakersJson, const QString& speakerName,
                                    const QString& styleName);

struct QueryTuning {
    double speed = 1.0;        // speedScale
    double pitch = 0.0;        // pitchScale
    double intonation = 1.0;   // intonationScale
    double volume = 1.0;       // volumeScale
    double preSeconds = 0.1;   // prePhonemeLength
    double postSeconds = 0.2;  // postPhonemeLength
    int sampleRate = 44100;    // outputSamplingRate
};

// Returns `query` with the tuning fields overwritten (mono output forced).
QJsonObject applyQueryTuning(QJsonObject query, const QueryTuning& tuning);

}  // namespace narration_text

namespace wav_util {

struct Pcm16Mono {
    int sampleRate = 0;
    std::vector<int16_t> samples;
};

// Parses a RIFF/WAVE PCM 16-bit file (mono, or stereo which is down-mixed).
// Scans chunks, so extra chunks (LIST etc.) are tolerated.
std::optional<Pcm16Mono> parsePcm16(const QByteArray& wav);

// Canonical 44-byte-header WAV (the same layout SAPI path produces).
QByteArray buildWavPcm16Mono(int sampleRate, const std::vector<int16_t>& samples);

// Linear-interpolation resampler (VOICEVOX versions differ in whether they
// honour outputSamplingRate; this guarantees the target rate regardless).
std::vector<int16_t> resample(const std::vector<int16_t>& in, int fromRate, int toRate);

std::vector<int16_t> silence(int sampleRate, double seconds);

}  // namespace wav_util

struct VoicevoxConfig {
    QString baseUrl = QStringLiteral("http://127.0.0.1:50021");
    int speakerId = -1;                 // explicit style id wins when >= 0
    QString speakerName = QStringLiteral("春日部つむぎ");  // used when speakerId < 0 (project default)
    QString styleName = QStringLiteral("ノーマル");
    narration_text::QueryTuning tuning;
    double chunkGapSeconds = 0.25;      // silence inserted between chunks
    int maxChunkChars = 80;
    int timeoutMs = 30000;
    QString dictionaryPath;             // JSON array, see docs/narration-quality.md

    // RAGREEL_VOICEVOX_URL, RAGREEL_VOICEVOX_SPEAKER_ID, RAGREEL_VOICEVOX_SPEAKER,
    // RAGREEL_VOICEVOX_STYLE, RAGREEL_VOICEVOX_SPEED, RAGREEL_VOICEVOX_INTONATION,
    // RAGREEL_VOICEVOX_PITCH, RAGREEL_VOICEVOX_GAP, RAGREEL_TTS_DICTIONARY
    static VoicevoxConfig fromEnvironment();
};

class VoicevoxNarrationEngine {
public:
    explicit VoicevoxNarrationEngine(VoicevoxConfig config);

    // Short GET /version probe. Never throws.
    bool isAvailable(int timeoutMs = 1500) const;

    // Throws std::runtime_error if the engine is unreachable, the speaker
    // cannot be resolved, or synthesis fails. Output: 44.1kHz mono 16-bit WAV.
    NarrationResult synthesize(const QString& text, const QString& wavPath) const;

    // Registers entries of `dictionaryPath` that the engine does not already
    // know (matched by surface). Returns how many were added. Best effort:
    // failures are swallowed (a missing dictionary entry only mispronounces).
    int registerUserDictionary() const;

private:
    int resolveSpeaker() const;
    VoicevoxConfig config_;
};
