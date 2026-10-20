#pragma once

#include <QString>

// Windows SAPI5-based narration synthesis (see docs/architecture/
// video-factory-design.md §3: NarrationEngine runs CPU-only, no GPU
// contention with the renderer). SAPI ships with Windows -- no network call,
// no API key, no extra vcpkg dependency. Quality depends on which SAPI
// voices are installed on the machine; this is a pragmatic offline choice,
// not a permanent architecture decision (llama.cpp-driven narration
// refinement per the design doc is still future work).
#include <vector>

// One spoken sentence/chunk and the time window it occupies in the WAV. Used to
// build subtitles (IMPROVEMENT_PLAN_2026-10.md V7): the engine knows exactly
// what it just synthesised, so the on-screen text and the audio can share one
// clock instead of the renderer guessing durations from character counts.
struct NarrationSegment {
    QString text;
    double startSec = 0.0;  // speech onset (leading engine silence excluded when known)
    double endSec = 0.0;
};

struct NarrationResult {
    QString wavPath;
    double durationSeconds = 0.0;
    // Which voice actually produced this audio, e.g. "VOICEVOX:春日部つむぎ(ノーマル)" or
    // "SAPI:Microsoft Haruka Desktop". Logged and shown in the Houdini panel so a silent
    // fallback to the old SAPI voice is never invisible again (2026-10-19).
    QString engineName;
    // Empty if the engine could not provide segments. `segmentsExact` is true
    // when the times were derived from the synthesised samples themselves
    // (VOICEVOX: per-chunk sample counts), false when they are an estimate
    // distributed over the total duration by character count (SAPI5 path).
    std::vector<NarrationSegment> segments;
    bool segmentsExact = false;
};

class NarrationEngine {
public:
    // Synthesizes `text` to a 44.1kHz mono 16-bit PCM WAV file at `wavPath`.
    // Tries to select an installed ja-JP voice; falls back to the system
    // default voice if none is found. Throws std::runtime_error if SAPI is
    // unavailable or produces no audio (e.g. no voice installed at all).
    static NarrationResult synthesize(const QString& text, const QString& wavPath);
};
