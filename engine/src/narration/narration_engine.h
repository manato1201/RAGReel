#pragma once

#include <QString>

// Windows SAPI5-based narration synthesis (see docs/architecture/
// video-factory-design.md §3: NarrationEngine runs CPU-only, no GPU
// contention with the renderer). SAPI ships with Windows -- no network call,
// no API key, no extra vcpkg dependency. Quality depends on which SAPI
// voices are installed on the machine; this is a pragmatic offline choice,
// not a permanent architecture decision (llama.cpp-driven narration
// refinement per the design doc is still future work).
struct NarrationResult {
    QString wavPath;
    double durationSeconds = 0.0;
};

class NarrationEngine {
public:
    // Synthesizes `text` to a 44.1kHz mono 16-bit PCM WAV file at `wavPath`.
    // Tries to select an installed ja-JP voice; falls back to the system
    // default voice if none is found. Throws std::runtime_error if SAPI is
    // unavailable or produces no audio (e.g. no voice installed at all).
    static NarrationResult synthesize(const QString& text, const QString& wavPath);
};
