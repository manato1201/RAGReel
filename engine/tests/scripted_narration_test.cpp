#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <stdexcept>

#include "narration/scripted_narration.h"
#include "narration/voicevox_narration_engine.h"  // wav_util

using namespace scripted_narration;

namespace {

// A fake engine: every character is "spoken" for 0.1 s (a tone, so the WAV is real PCM).
NarrationResult fakeSynth(const QString& text, const QString& path, int rate = 44100,
                          const QString& engine = QStringLiteral("FAKE")) {
    const double seconds = text.size() * 0.1;
    std::vector<int16_t> samples(static_cast<size_t>(seconds * rate), 1000);
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(wav_util::buildWavPcm16Mono(rate, samples));
    NarrationResult r;
    r.wavPath = path;
    r.durationSeconds = seconds;
    r.engineName = engine;
    r.segmentsExact = true;
    r.segments.push_back({text, 0.0, seconds});
    return r;
}

double wavSeconds(const QString& path) {
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    const auto parsed = wav_util::parsePcm16(f.readAll());
    EXPECT_TRUE(parsed.has_value());
    return static_cast<double>(parsed->samples.size()) / parsed->sampleRate;
}

}  // namespace

TEST(ScriptedNarration, BlocksAreContiguousAndMatchTheAudioLength) {
    QTemporaryDir dir;
    const QString wav = dir.filePath("out.wav");
    Options opt;  // gap 0.45, hold 2.5
    const Result r = synthesizeBlocks([](const QString& t, const QString& p) { return fakeSynth(t, p); },
                                      {QStringLiteral("あいうえお"), QStringLiteral("かきくけこさしすせそ")}, wav, opt);
    ASSERT_EQ(r.blocks.size(), 2u);
    EXPECT_NEAR(r.blocks[0].startSec, 0.0, 1e-6);
    EXPECT_NEAR(r.blocks[0].speechEndSec, 0.5, 1e-3);
    EXPECT_NEAR(r.blocks[0].endSec, 0.95, 1e-3);           // + 0.45 s gap
    EXPECT_NEAR(r.blocks[1].startSec, r.blocks[0].endSec, 1e-6);
    EXPECT_NEAR(r.blocks[1].speechEndSec, 0.95 + 1.0, 1e-3);
    EXPECT_NEAR(r.merged.durationSeconds, r.blocks[1].endSec, 1e-6);
    EXPECT_NEAR(wavSeconds(wav), r.merged.durationSeconds, 1e-3);  // the file really has that length
}

TEST(ScriptedNarration, SegmentsAreShiftedIntoTheMergedTimeBase) {
    QTemporaryDir dir;
    const Result r = synthesizeBlocks([](const QString& t, const QString& p) { return fakeSynth(t, p); },
                                      {QStringLiteral("一二三四五"), QStringLiteral("六七八九十")}, dir.filePath("o.wav"));
    ASSERT_EQ(r.merged.segments.size(), 2u);
    EXPECT_NEAR(r.merged.segments[0].startSec, 0.0, 1e-6);
    EXPECT_NEAR(r.merged.segments[1].startSec, r.blocks[1].startSec, 1e-6);
    EXPECT_NEAR(r.merged.segments[1].endSec, r.blocks[1].speechEndSec, 1e-3);
    EXPECT_TRUE(r.merged.segmentsExact);
}

TEST(ScriptedNarration, EmptyBlockBecomesASilentHold) {
    QTemporaryDir dir;
    Options opt;
    opt.holdSeconds = 2.0;
    const Result r = synthesizeBlocks([](const QString& t, const QString& p) { return fakeSynth(t, p); },
                                      {QStringLiteral("あ"), QString(), QStringLiteral("い")}, dir.filePath("o.wav"), opt);
    ASSERT_EQ(r.blocks.size(), 3u);
    EXPECT_NEAR(r.blocks[1].endSec - r.blocks[1].startSec, 2.0, 1e-3);
    EXPECT_NEAR(r.blocks[1].speechEndSec, r.blocks[1].startSec, 1e-6);
    EXPECT_NEAR(r.blocks[2].startSec, r.blocks[1].endSec, 1e-6);
    EXPECT_EQ(r.merged.segments.size(), 2u);  // the hold has no subtitle
}

TEST(ScriptedNarration, PartsAreConvertedToTheTargetRateAndCleanedUp) {
    QTemporaryDir dir;
    const QString wav = dir.filePath("o.wav");
    const Result r = synthesizeBlocks([](const QString& t, const QString& p) { return fakeSynth(t, p, 22050); },
                                      {QStringLiteral("あいうえおかきくけこ")}, wav);
    EXPECT_NEAR(r.blocks[0].speechEndSec, 1.0, 0.01);  // 10 chars * 0.1 s, resampled
    EXPECT_NEAR(wavSeconds(wav), r.merged.durationSeconds, 1e-3);
    EXPECT_FALSE(QFile::exists(wav + ".part0.wav"));
}

TEST(ScriptedNarration, EngineNamesAreKeptAndMixedOnesCombined) {
    QTemporaryDir dir;
    int call = 0;
    const Result r = synthesizeBlocks(
        [&](const QString& t, const QString& p) {
            return fakeSynth(t, p, 44100, call++ == 0 ? QStringLiteral("VOICEVOX") : QStringLiteral("SAPI"));
        },
        {QStringLiteral("あ"), QStringLiteral("い")}, dir.filePath("o.wav"));
    EXPECT_TRUE(r.merged.engineName.contains(QStringLiteral("VOICEVOX")));
    EXPECT_TRUE(r.merged.engineName.contains(QStringLiteral("SAPI")));
}

TEST(ScriptedNarration, EstimatedSegmentsMakeTheWholeResultInexact) {
    QTemporaryDir dir;
    const Result r = synthesizeBlocks(
        [](const QString& t, const QString& p) {
            NarrationResult n = fakeSynth(t, p);
            n.segmentsExact = false;
            return n;
        },
        {QStringLiteral("あ")}, dir.filePath("o.wav"));
    EXPECT_FALSE(r.merged.segmentsExact);
}

TEST(ScriptedNarration, EngineFailureIsReportedToTheCaller) {
    QTemporaryDir dir;
    EXPECT_THROW(synthesizeBlocks([](const QString&, const QString&) -> NarrationResult { throw std::runtime_error("boom"); },
                                  {QStringLiteral("あ")}, dir.filePath("o.wav")),
                 std::runtime_error);
    EXPECT_THROW(synthesizeBlocks([](const QString& t, const QString& p) { return fakeSynth(t, p); }, {}, dir.filePath("o.wav")),
                 std::runtime_error);
}

TEST(ScriptedNarration, StartFramesFollowTheBlocks) {
    std::vector<Block> blocks(3);
    blocks[0].startSec = 0.0;
    blocks[1].startSec = 4.0;
    blocks[2].startSec = 9.5;
    const std::vector<int> f = startFramesFromBlocks(blocks, 30, 400);
    ASSERT_EQ(f.size(), 4u);
    EXPECT_EQ(f[0], 0);
    EXPECT_EQ(f[1], 120);
    EXPECT_EQ(f[2], 285);
    EXPECT_EQ(f[3], 400);  // the last slide runs to the end (includes the tail padding)
    // Never goes backwards or past the end, even for odd input.
    blocks[2].startSec = 99.0;
    const std::vector<int> g = startFramesFromBlocks(blocks, 30, 400);
    EXPECT_EQ(g[2], 400);
}
