#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "narration/subtitle_writer.h"
#include "narration/voicevox_narration_engine.h"

using namespace narration_text;

TEST(NormalizeForSpeech, ReplacesUrlsAndStripsMarkdown) {
    const QString in = QStringLiteral("**重要**: 詳細は https://example.com/a?b=1 を参照。`code`");
    const QString out = normalizeForSpeech(in);
    EXPECT_FALSE(out.contains(QStringLiteral("http")));
    EXPECT_FALSE(out.contains(QLatin1Char('*')));
    EXPECT_FALSE(out.contains(QLatin1Char('`')));
    EXPECT_TRUE(out.contains(QStringLiteral("リンク")));
    EXPECT_TRUE(out.contains(QStringLiteral("重要")));
}

TEST(NormalizeForSpeech, CollapsesRepeatedPunctuationAndSpaces) {
    EXPECT_EQ(normalizeForSpeech(QStringLiteral("すごい！！！  本当？？")), QStringLiteral("すごい！ 本当？"));
}

TEST(SplitIntoChunks, SplitsOnSentenceEnds) {
    const QStringList c = splitIntoChunks(QStringLiteral("一つ目です。二つ目です！三つ目ですか？"), 80);
    ASSERT_EQ(c.size(), 3);
    EXPECT_EQ(c[0], QStringLiteral("一つ目です。"));
    EXPECT_EQ(c[2], QStringLiteral("三つ目ですか？"));
}

TEST(SplitIntoChunks, CutsLongSentenceAtCommaThenHard) {
    QString s;
    for (int i = 0; i < 6; ++i) s += QStringLiteral("あいうえおかきくけこ、");  // 11 chars each
    s += QStringLiteral("終わり。");
    const QStringList c = splitIntoChunks(s, 30);
    EXPECT_GT(c.size(), 1);
    for (const QString& chunk : c) EXPECT_LE(chunk.size(), 30) << chunk.toStdString();
    // No characters lost (apart from trimmed whitespace, none here).
    EXPECT_EQ(c.join(QString()).size(), s.size());

    const QStringList hard = splitIntoChunks(QString(100, QChar(0x3042)), 25);  // no comma at all
    EXPECT_EQ(hard.size(), 4);
}

TEST(SplitIntoChunks, DropsEmptyAndKeepsOrder) {
    const QStringList c = splitIntoChunks(QStringLiteral("\n\nA。\n\nB。\n"), 80);
    ASSERT_EQ(c.size(), 2);
    EXPECT_EQ(c[0], QStringLiteral("A。"));
    EXPECT_EQ(c[1], QStringLiteral("B。"));
}

TEST(ResolveSpeakerId, MatchesNameAndStyle) {
    QJsonArray speakers;
    QJsonObject a;
    a["name"] = QStringLiteral("四国めたん");
    a["styles"] = QJsonArray{QJsonObject{{"name", QStringLiteral("ノーマル")}, {"id", 2}},
                             QJsonObject{{"name", QStringLiteral("あまあま")}, {"id", 0}}};
    QJsonObject b;
    b["name"] = QStringLiteral("春日部つむぎ");
    b["styles"] = QJsonArray{QJsonObject{{"name", QStringLiteral("ノーマル")}, {"id", 8}}};
    speakers << a << b;
    const QByteArray json = QJsonDocument(speakers).toJson();

    EXPECT_EQ(resolveSpeakerId(json, QStringLiteral("春日部つむぎ"), QStringLiteral("ノーマル")), 8);
    EXPECT_EQ(resolveSpeakerId(json, QStringLiteral("四国めたん"), QStringLiteral("あまあま")), 0);
    EXPECT_EQ(resolveSpeakerId(json, QStringLiteral("つむぎ"), QStringLiteral("ノーマル")), 8);  // substring
    EXPECT_EQ(resolveSpeakerId(json, QStringLiteral("四国めたん"), QString()), 2);               // first style
    EXPECT_FALSE(resolveSpeakerId(json, QStringLiteral("存在しない"), QStringLiteral("ノーマル")).has_value());
    EXPECT_FALSE(resolveSpeakerId(QByteArray("not json"), QString(), QString()).has_value());
}

TEST(ApplyQueryTuning, OverwritesOnlyTuningFields) {
    QJsonObject q;
    q["accent_phrases"] = QJsonArray{1, 2};
    q["speedScale"] = 1.0;
    QueryTuning t;
    t.speed = 1.1;
    t.intonation = 1.2;
    t.sampleRate = 44100;
    const QJsonObject out = applyQueryTuning(q, t);
    EXPECT_DOUBLE_EQ(out["speedScale"].toDouble(), 1.1);
    EXPECT_DOUBLE_EQ(out["intonationScale"].toDouble(), 1.2);
    EXPECT_EQ(out["outputSamplingRate"].toInt(), 44100);
    EXPECT_FALSE(out["outputStereo"].toBool(true));
    EXPECT_EQ(out["accent_phrases"].toArray().size(), 2);  // untouched
}

TEST(Wav, BuildAndParseRoundTrip) {
    std::vector<int16_t> samples{0, 1000, -1000, 32767, -32768};
    const QByteArray wav = wav_util::buildWavPcm16Mono(44100, samples);
    EXPECT_EQ(wav.size(), 44 + static_cast<int>(samples.size()) * 2);
    const auto parsed = wav_util::parsePcm16(wav);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->sampleRate, 44100);
    EXPECT_EQ(parsed->samples, samples);
}

TEST(Wav, RejectsGarbageAndNonPcm) {
    EXPECT_FALSE(wav_util::parsePcm16(QByteArray("hello")).has_value());
    QByteArray wav = wav_util::buildWavPcm16Mono(24000, {1, 2, 3});
    wav[20] = 3;  // format tag 3 (float) -> unsupported
    EXPECT_FALSE(wav_util::parsePcm16(wav).has_value());
}

TEST(Wav, ResampleChangesLengthByRatio) {
    std::vector<int16_t> in(24000, 100);  // 1 second @24k
    const auto out = wav_util::resample(in, 24000, 44100);
    EXPECT_EQ(out.size(), 44100u);
    EXPECT_EQ(out[1000], 100);
    EXPECT_EQ(wav_util::resample(in, 24000, 24000).size(), in.size());
}

TEST(Wav, SilenceLength) {
    EXPECT_EQ(wav_util::silence(44100, 0.25).size(), 11025u);
    EXPECT_TRUE(wav_util::silence(44100, -1.0).empty());
}

TEST(VoicevoxConfig, DefaultSpeakerIsKasukabeTsumugiNormal) {
    qunsetenv("RAGREEL_VOICEVOX_SPEAKER");
    qunsetenv("RAGREEL_VOICEVOX_SPEAKER_ID");
    qunsetenv("RAGREEL_VOICEVOX_STYLE");
    const VoicevoxConfig c = VoicevoxConfig::fromEnvironment();
    EXPECT_EQ(c.speakerName, QStringLiteral("春日部つむぎ"));
    EXPECT_EQ(c.styleName, QStringLiteral("ノーマル"));
    EXPECT_EQ(c.speakerId, -1);
}

TEST(VoicevoxConfig, EnvironmentOverridesSpeaker) {
    qputenv("RAGREEL_VOICEVOX_SPEAKER", "TestSpeaker");  // ASCII: qputenv is ANSI on Windows
    qputenv("RAGREEL_VOICEVOX_SPEAKER_ID", "8");
    const VoicevoxConfig c = VoicevoxConfig::fromEnvironment();
    EXPECT_EQ(c.speakerName, QStringLiteral("TestSpeaker"));
    EXPECT_EQ(c.speakerId, 8);
    qunsetenv("RAGREEL_VOICEVOX_SPEAKER");
    qunsetenv("RAGREEL_VOICEVOX_SPEAKER_ID");
}

// ---------------------------------------------------------------------------
// subtitle_writer (V7)
// ---------------------------------------------------------------------------
TEST(SubtitleWriter, FormatsTimestamps) {
    EXPECT_EQ(subtitle_writer::formatTimestamp(0.0, false), QStringLiteral("00:00:00,000"));
    EXPECT_EQ(subtitle_writer::formatTimestamp(3661.5, false), QStringLiteral("01:01:01,500"));
    EXPECT_EQ(subtitle_writer::formatTimestamp(1.2345, true), QStringLiteral("00:00:01.235"));
    EXPECT_EQ(subtitle_writer::formatTimestamp(-4.0, false), QStringLiteral("00:00:00,000"));
}

TEST(SubtitleWriter, WrapsPreferringPunctuationAndNeverDropsText) {
    const QString text = QStringLiteral("Sphereを楕円に伸ばし、Mountainで肉塊の凹凸を付け、色を塗り分けます。");
    const QStringList lines = subtitle_writer::wrapLines(text, 16, 2);
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines.join(QString()), text);  // the second line absorbs the rest
    EXPECT_LE(lines[0].size(), 16);

    EXPECT_EQ(subtitle_writer::wrapLines(QStringLiteral("短い。"), 22, 2).size(), 1);
    EXPECT_TRUE(subtitle_writer::wrapLines(QString(), 22, 2).isEmpty());
}

TEST(SubtitleWriter, SrtSkipsInvalidCuesAndRenumbers) {
    std::vector<NarrationSegment> segs = {
        {QStringLiteral("一つ目です。"), 0.1, 2.0},
        {QStringLiteral("   "), 2.0, 3.0},              // blank text -> skipped
        {QStringLiteral("終了が開始より前"), 5.0, 4.0},  // end <= start -> skipped
        {QStringLiteral("二つ目です。"), 3.0, 4.5},
    };
    const QString srt = subtitle_writer::toSrt(segs);
    EXPECT_TRUE(srt.startsWith(QStringLiteral("1\n00:00:00,100 --> 00:00:02,000\n一つ目です。\n")));
    EXPECT_TRUE(srt.contains(QStringLiteral("2\n00:00:03,000 --> 00:00:04,500\n二つ目です。")));
    EXPECT_FALSE(srt.contains(QStringLiteral("3\n")));
}

TEST(SubtitleWriter, VttHasHeaderAndDotTimestamps) {
    const std::vector<NarrationSegment> segs = {{QStringLiteral("こんにちは。"), 0.0, 1.0}};
    const QString vtt = subtitle_writer::toVtt(segs);
    EXPECT_TRUE(vtt.startsWith(QStringLiteral("WEBVTT\n\n")));
    EXPECT_TRUE(vtt.contains(QStringLiteral("00:00:00.000 --> 00:00:01.000")));
}

// ---- HTTP probe against an engine that is still starting up (2026-10-19) ----------------------
// A VOICEVOX engine that has just been launched accepts TCP connections late, so the first probes
// time out. Probing repeatedly must keep working (it used to crash on the call after a timeout).
#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>

TEST(VoicevoxProbe, RepeatedTimedOutProbesDoNotCrash) {
    int argc = 1;
    char arg0[] = "narration_tests";
    char* argv[] = {arg0, nullptr};
    // One per process (cloud_tts_test.cpp may have created it already).
    static QCoreApplication* app = QCoreApplication::instance() ? nullptr : new QCoreApplication(argc, argv);
    (void)app;

    QTcpServer silent;  // accepts connections (backlog) but never answers
    ASSERT_TRUE(silent.listen(QHostAddress::LocalHost, 0));
    VoicevoxConfig config;
    config.baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(silent.serverPort());
    const VoicevoxNarrationEngine engine(config);
    for (int i = 0; i < 4; ++i) {
        EXPECT_FALSE(engine.isAvailable(250)) << "probe " << i;
    }
}

// ---- pcmDurationSeconds: only the "data" chunk is audio (2026-11-11) ----
// SAPI (SPFEI_ALL_EVENTS) writes an "EVNT" chunk AFTER "data". The engine used to compute the length as
// (file size - 44) / bytes-per-second, which counted that chunk as audio: a 114.12 s narration was
// reported as 114.91 s, the video came out 0.8-4 s too long and the (estimated) subtitles were stretched.
namespace {
QByteArray le32(uint32_t v) {
    QByteArray b;
    for (int i = 0; i < 4; ++i) b.append(static_cast<char>((v >> (8 * i)) & 0xFF));
    return b;
}
QByteArray le16(uint16_t v) {
    QByteArray b;
    b.append(static_cast<char>(v & 0xFF));
    b.append(static_cast<char>((v >> 8) & 0xFF));
    return b;
}
QByteArray chunk(const char* id, const QByteArray& body) {
    QByteArray out(id, 4);
    out += le32(static_cast<uint32_t>(body.size())) + body;
    if (body.size() & 1) out.append('\0');  // RIFF pads odd-sized chunks
    return out;
}
QByteArray fmtChunk(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits) {
    const uint16_t align = static_cast<uint16_t>(channels * (bits / 8));
    return chunk("fmt ", le16(format) + le16(channels) + le32(rate) + le32(rate * align) + le16(align) + le16(bits));
}
QByteArray riff(const QByteArray& chunks) {
    return QByteArray("RIFF") + le32(static_cast<uint32_t>(4 + chunks.size())) + QByteArray("WAVE") + chunks;
}
}  // namespace

TEST(PcmDurationSeconds, IgnoresTheTrailingEvntChunkThatSapiWrites) {
    const int seconds = 10;
    const QByteArray audio(44100 * 2 * seconds, '\x01');  // mono 16-bit
    const QByteArray evnt(6000, '\x7f');                  // ~0.68% of the file, like the real SAPI output
    const QByteArray wav = riff(fmtChunk(1, 1, 44100, 16) + chunk("data", audio) + chunk("EVNT", evnt));
    ASSERT_TRUE(wav_util::pcmDurationSeconds(wav).has_value());
    EXPECT_DOUBLE_EQ(*wav_util::pcmDurationSeconds(wav), 10.0);
    // The old estimate: counts the EVNT chunk (and its 8-byte header) as audio.
    const double oldEstimate = static_cast<double>(wav.size() - 44) / 88200.0;
    EXPECT_GT(oldEstimate, 10.05) << "this is the over-report the fix removes";
}

TEST(PcmDurationSeconds, MatchesTheRealOutputRatio) {
    // The real SAPI file of a 332 s narration: data 29,287,128 bytes, EVNT 199,920 bytes.
    const double real = 29287128.0 / 88200.0;                       // 332.0536 s
    const double oldEstimate = (29287128.0 + 199920.0 + 8 + 12 + 38 - 44) / 88200.0;
    EXPECT_NEAR(real, 332.0536, 1e-3);
    EXPECT_GT(oldEstimate - real, 2.2);  // what the video was too long by
}

TEST(PcmDurationSeconds, ToleratesChunksBeforeDataAndOddSizes) {
    const QByteArray audio(22050 * 2, '\x02');  // 1.0 s mono 16-bit at 22.05 kHz
    const QByteArray wav = riff(fmtChunk(1, 1, 22050, 16) + chunk("LIST", QByteArray(7, 'x')) + chunk("data", audio));
    ASSERT_TRUE(wav_util::pcmDurationSeconds(wav).has_value());
    EXPECT_DOUBLE_EQ(*wav_util::pcmDurationSeconds(wav), 1.0);
}

TEST(PcmDurationSeconds, HandlesOtherChannelCountsAndBitDepths) {
    EXPECT_DOUBLE_EQ(*wav_util::pcmDurationSeconds(riff(fmtChunk(1, 2, 22050, 16) + chunk("data", QByteArray(22050 * 4, '\x01')))), 1.0);
    EXPECT_DOUBLE_EQ(*wav_util::pcmDurationSeconds(riff(fmtChunk(1, 1, 8000, 8) + chunk("data", QByteArray(4000, '\x80')))), 0.5);
    EXPECT_DOUBLE_EQ(*wav_util::pcmDurationSeconds(riff(fmtChunk(1, 1, 48000, 24) + chunk("data", QByteArray(48000 * 3, '\x01')))), 1.0);
}

TEST(PcmDurationSeconds, AgreesWithParsePcm16OnCanonicalFiles) {
    const std::vector<int16_t> samples(12000, 100);
    const QByteArray wav = wav_util::buildWavPcm16Mono(24000, samples);
    const auto parsed = wav_util::parsePcm16(wav);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(*wav_util::pcmDurationSeconds(wav), static_cast<double>(parsed->samples.size()) / parsed->sampleRate);
}

TEST(PcmDurationSeconds, TruncatedDataUsesWhatIsThere) {
    QByteArray wav = riff(fmtChunk(1, 1, 44100, 16) + chunk("data", QByteArray(44100 * 2, '\x01')));
    wav.chop(44100);  // the file is cut in half; the chunk header still claims a full second
    ASSERT_TRUE(wav_util::pcmDurationSeconds(wav).has_value());
    EXPECT_NEAR(*wav_util::pcmDurationSeconds(wav), 0.5, 1e-3);
}

TEST(PcmDurationSeconds, RejectsWhatIsNotPcmWav) {
    EXPECT_FALSE(wav_util::pcmDurationSeconds(QByteArray("hello")).has_value());
    EXPECT_FALSE(wav_util::pcmDurationSeconds(QByteArray()).has_value());
    EXPECT_FALSE(wav_util::pcmDurationSeconds(riff(fmtChunk(3, 1, 44100, 32) + chunk("data", QByteArray(400, 'x')))).has_value())
        << "IEEE float is not PCM";
    EXPECT_FALSE(wav_util::pcmDurationSeconds(riff(fmtChunk(1, 1, 44100, 16))).has_value()) << "no data chunk";
}
