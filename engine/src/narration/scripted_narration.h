#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "narration/narration_engine.h"

// Narration built block by block (2026-10-19). One block = what is said while one slide is
// on screen. Each block is synthesised on its own and the WAVs are joined, so the slide
// boundaries come from the real audio lengths -- the picture and the voice can never drift
// apart (the old flow read the whole text once and guessed slide lengths from characters).
//
// Qt6::Core only: the engine is injected as a function, so tests use a fake.
namespace scripted_narration {

using SynthFn = std::function<NarrationResult(const QString& text, const QString& wavPath)>;

struct Options {
    double gapSeconds = 0.45;   // silence after each block with speech (a breath between slides)
    double holdSeconds = 2.5;   // silence used for a block that has no text (the slide still needs screen time)
    int sampleRate = 44100;     // every part is converted to this
};

struct Block {
    QString text;               // empty = no speech, a silent hold
    double startSec = 0.0;      // block start in the merged WAV
    double endSec = 0.0;        // block end including its trailing gap (= next block's start)
    double speechEndSec = 0.0;  // where the speech itself stops
};

struct Result {
    NarrationResult merged;     // wavPath, durationSeconds, engineName, segments (merged time base)
    std::vector<Block> blocks;  // same order and size as the input
};

// Synthesises every block and writes the joined WAV to `wavPath`. The parts are written next to
// it (`<wavPath>.partN.wav`) and removed afterwards. Throws std::runtime_error when a block
// cannot be synthesised or the engine's WAV cannot be read -- the caller falls back to the
// single-text flow in that case.
Result synthesizeBlocks(const SynthFn& synth, const QStringList& texts, const QString& wavPath,
                        const Options& options = {});

// Slide start frames (size = blocks + 1; the last entry is `totalFrames`) from block times, so a
// slide is on screen exactly while its block is spoken.
std::vector<int> startFramesFromBlocks(const std::vector<Block>& blocks, int fps, int totalFrames);

}  // namespace scripted_narration
