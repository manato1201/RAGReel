#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

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

// ---- Subtitles keep the real spelling (2026-11-08) ----

namespace {
// A fake engine that returns one segment per sentence, like VOICEVOX / the cloud voice.
NarrationResult sentenceSynth(const QString& text, const QString& path) {
    NarrationResult r = fakeSynth(text, path);
    r.segments.clear();
    double t = 0.0;
    QString current;
    for (const QChar c : text) {
        current += c;
        if (c == QChar(0x3002)) {
            r.segments.push_back({current, t, t + current.size() * 0.1});
            t += current.size() * 0.1;
            current.clear();
        }
    }
    return r;
}
}  // namespace

TEST(ScriptedNarrationSubtitles, DisplayTextReplacesTheSegmentTextAndKeepsTheTimes) {
    const std::vector<NarrationSegment> segs = {{QStringLiteral("ブラストを置きます。"), 0.0, 1.0},
                                                {QStringLiteral("次に進みます。"), 1.2, 2.0}};
    const auto out = withDisplayText(segs, QStringLiteral("Blastを置きます。次に進みます。"));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].text, QStringLiteral("Blastを置きます。"));
    EXPECT_EQ(out[1].text, QStringLiteral("次に進みます。"));
    EXPECT_DOUBLE_EQ(out[0].endSec, 1.0);
    EXPECT_DOUBLE_EQ(out[1].startSec, 1.2);
}

TEST(ScriptedNarrationSubtitles, AMismatchedSentenceCountKeepsTheSpokenText) {
    const std::vector<NarrationSegment> segs = {{QStringLiteral("ブラストを置きます。"), 0.0, 1.0},
                                                {QStringLiteral("次に進みます。"), 1.2, 2.0}};
    const auto out = withDisplayText(segs, QStringLiteral("Blastを置いて、次に進みます。"));  // one sentence, not two
    EXPECT_EQ(out[0].text, QStringLiteral("ブラストを置きます。"));
    EXPECT_EQ(out[1].text, QStringLiteral("次に進みます。"));
    EXPECT_EQ(withDisplayText(segs, QString())[0].text, QStringLiteral("ブラストを置きます。"));
}

TEST(ScriptedNarrationSubtitles, SynthesizeBlocksUsesTheDisplayTextPerBlock) {
    QTemporaryDir dir;
    const QStringList spoken = {QStringLiteral("ブラストを置きます。"), QStringLiteral("ここは、そのままです。")};
    const QStringList shown = {QStringLiteral("Blastを置きます。"), QString()};  // block 2: show what is spoken
    const Result r = synthesizeBlocks(sentenceSynth, spoken, dir.filePath("o.wav"), Options{}, shown);
    ASSERT_EQ(r.merged.segments.size(), 2u);
    EXPECT_EQ(r.merged.segments[0].text, QStringLiteral("Blastを置きます。"));
    EXPECT_EQ(r.merged.segments[1].text, QStringLiteral("ここは、そのままです。"));
    // The audio and the times do not change.
    const Result plain = synthesizeBlocks(sentenceSynth, spoken, dir.filePath("p.wav"));
    EXPECT_NEAR(r.merged.durationSeconds, plain.merged.durationSeconds, 1e-9);
    EXPECT_NEAR(r.merged.segments[1].startSec, plain.merged.segments[1].startSec, 1e-9);
}

// ---- 同時に合成する(Options::maxParallel)。出力は 1 つずつ合成した場合と同じでなければならない ----

TEST(ScriptedNarrationParallel, ProducesTheSameResultAsSerial) {
    QTemporaryDir dir;
    QStringList texts;
    for (int i = 0; i < 12; ++i) {
        // 空の block(無音の間)も混ぜる。長さもばらばらにして、終わる順が入れ替わるようにする
        texts << (i % 5 == 3 ? QString() : QString(QStringLiteral("あ")).repeated(3 + (i * 7) % 11));
    }
    const SynthFn slowSynth = [](const QString& t, const QString& p) {
        // 長い文ほど先に終わる(順序が入れ替わる)ようにして、組み立てが block の順であることを確かめる
        std::this_thread::sleep_for(std::chrono::milliseconds(40 - std::min<int>(t.size(), 12) * 3));
        return fakeSynth(t, p);
    };
    const Result serial = synthesizeBlocks(slowSynth, texts, dir.filePath("serial.wav"));
    Options parallel;
    parallel.maxParallel = 4;
    const Result par = synthesizeBlocks(slowSynth, texts, dir.filePath("par.wav"), parallel);

    ASSERT_EQ(par.blocks.size(), serial.blocks.size());
    for (size_t i = 0; i < serial.blocks.size(); ++i) {
        EXPECT_NEAR(par.blocks[i].startSec, serial.blocks[i].startSec, 1e-9) << i;
        EXPECT_NEAR(par.blocks[i].endSec, serial.blocks[i].endSec, 1e-9) << i;
        EXPECT_NEAR(par.blocks[i].speechEndSec, serial.blocks[i].speechEndSec, 1e-9) << i;
    }
    ASSERT_EQ(par.merged.segments.size(), serial.merged.segments.size());
    for (size_t i = 0; i < serial.merged.segments.size(); ++i) {
        EXPECT_EQ(par.merged.segments[i].text, serial.merged.segments[i].text);
        EXPECT_NEAR(par.merged.segments[i].startSec, serial.merged.segments[i].startSec, 1e-9);
    }
    EXPECT_NEAR(par.merged.durationSeconds, serial.merged.durationSeconds, 1e-9);
    // WAV のバイト列も同じ
    QFile a(dir.filePath("serial.wav")), b(dir.filePath("par.wav"));
    ASSERT_TRUE(a.open(QIODevice::ReadOnly));
    ASSERT_TRUE(b.open(QIODevice::ReadOnly));
    EXPECT_EQ(a.readAll(), b.readAll());
}

TEST(ScriptedNarrationParallel, ReallyRunsSeveralBlocksAtOnce) {
    QTemporaryDir dir;
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    Options parallel;
    parallel.maxParallel = 3;
    const QStringList texts = {QStringLiteral("あ"), QStringLiteral("い"), QStringLiteral("う"),
                               QStringLiteral("え"), QStringLiteral("お"), QStringLiteral("か")};
    synthesizeBlocks(
        [&](const QString& t, const QString& p) {
            const int now = ++running;
            int seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            --running;
            return fakeSynth(t, p);
        },
        texts, dir.filePath("o.wav"), parallel);
    EXPECT_GE(peak.load(), 2);
    EXPECT_LE(peak.load(), 3);  // 指定した数を超えない
}

TEST(ScriptedNarrationParallel, TheEarliestFailingBlockIsReportedAndPartsAreCleanedUp) {
    QTemporaryDir dir;
    Options parallel;
    parallel.maxParallel = 4;
    const QStringList texts = {QStringLiteral("あ"), QStringLiteral("い"), QStringLiteral("う"), QStringLiteral("え")};
    try {
        synthesizeBlocks(
            [&](const QString& t, const QString& p) -> NarrationResult {
                if (t == QStringLiteral("い")) throw std::runtime_error("block 1 failed");
                return fakeSynth(t, p);
            },
            texts, dir.filePath("o.wav"), parallel);
        FAIL() << "should throw";
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "block 1 failed");
    }
    EXPECT_FALSE(QFile::exists(dir.filePath("o.wav.part0.wav")));
    EXPECT_FALSE(QFile::exists(dir.filePath("o.wav.part2.wav")));
}

TEST(ScriptedNarrationParallel, SerialByDefault) {
    Options defaults;
    EXPECT_EQ(defaults.maxParallel, 1);
}
