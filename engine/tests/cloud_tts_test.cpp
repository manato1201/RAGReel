#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

#include "narration/cloud_tts_engine.h"
#include "narration/voicevox_narration_engine.h"  // wav_util

namespace {

void ensureApp() {
    static int argc = 1;
    static char arg0[] = "narration_tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication* app = QCoreApplication::instance() ? nullptr : new QCoreApplication(argc, argv);
    (void)app;
}

// A stand-in for the Worker's POST /tts: records each request and answers with a fixed reply.
class FakeWorker : public QObject {
public:
    FakeWorker() {
        ensureApp();
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            }
        });
        EXPECT_TRUE(server_.listen(QHostAddress::LocalHost, 0));
    }

    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }

    int status = 200;
    QByteArray reply;
    QByteArray voiceHeader = "ja-JP-Neural2-B";
    std::vector<QByteArray> requestHeaders;
    std::vector<QJsonObject> requestBodies;

private:
    void onReadyRead(QTcpSocket* socket) {
        QByteArray& buffer = buffers_[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray headers = buffer.left(headerEnd);
        int contentLength = 0;
        for (const QByteArray& line : headers.split('\n')) {
            if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
        }
        if (buffer.size() < headerEnd + 4 + contentLength) return;
        requestHeaders.push_back(headers);
        requestBodies.push_back(QJsonDocument::fromJson(buffer.mid(headerEnd + 4, contentLength)).object());
        buffers_.erase(socket);

        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n";
        response += status == 200 ? "Content-Type: audio/wav\r\n" : "Content-Type: application/json\r\n";
        response += "X-TTS-Voice: " + voiceHeader + "\r\n";
        response += "Content-Length: " + QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(response + reply);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    std::map<QTcpSocket*, QByteArray> buffers_;
};

// 0.5 s of a 24kHz tone -- what Google's LINEAR16 reply looks like.
QByteArray halfSecondWav() {
    std::vector<int16_t> samples(12000);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * static_cast<double>(i) / 24000.0));
    }
    return wav_util::buildWavPcm16Mono(24000, samples);
}

CloudTtsConfig configFor(const FakeWorker& worker) {
    CloudTtsConfig c;
    c.baseUrl = worker.baseUrl();
    c.apiKey = QStringLiteral("secret-key");
    c.timeoutMs = 5000;
    return c;
}

}  // namespace

TEST(CloudTts, SynthesizesEachChunkAndJoinsThemAtTheEngineSampleRate) {
    FakeWorker worker;
    worker.reply = halfSecondWav();
    CloudTtsConfig config = configFor(worker);
    config.voice = QStringLiteral("ja-JP-Chirp3-HD-Aoede");
    config.speakingRate = 1.1;
    const CloudTtsEngine engine(config);
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString wav = dir.filePath(QStringLiteral("out.wav"));

    const NarrationResult r = engine.synthesize(QStringLiteral("こんにちは。今日はよろしくお願いします。"), wav);

    ASSERT_EQ(worker.requestBodies.size(), 2u) << "one request per sentence";
    EXPECT_TRUE(worker.requestHeaders[0].startsWith("POST /tts "));
    EXPECT_TRUE(worker.requestHeaders[0].contains("Bearer secret-key"));
    EXPECT_EQ(worker.requestBodies[0].value(QStringLiteral("voice")).toString(), QStringLiteral("ja-JP-Chirp3-HD-Aoede"));
    EXPECT_DOUBLE_EQ(worker.requestBodies[0].value(QStringLiteral("speaking_rate")).toDouble(), 1.1);
    EXPECT_EQ(worker.requestBodies[0].value(QStringLiteral("text")).toString(), QStringLiteral("こんにちは。"));

    EXPECT_NEAR(r.durationSeconds, 0.5 + 0.25 + 0.5, 0.01);
    ASSERT_EQ(r.segments.size(), 2u);
    EXPECT_TRUE(r.segmentsExact);
    EXPECT_NEAR(r.segments[0].startSec, 0.0, 1e-9);
    EXPECT_NEAR(r.segments[0].endSec, 0.5, 0.01);
    EXPECT_NEAR(r.segments[1].startSec, 0.75, 0.01);
    EXPECT_TRUE(r.engineName.startsWith(QStringLiteral("Google Cloud TTS:")));
    EXPECT_TRUE(r.engineName.contains(QStringLiteral("ja-JP-Neural2-B"))) << "the voice the Worker reports wins";

    QFile file(wav);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto pcm = wav_util::parsePcm16(file.readAll());
    ASSERT_TRUE(pcm.has_value());
    EXPECT_EQ(pcm->sampleRate, 44100);
}

TEST(CloudTts, OmitsVoiceAndRateWhenNotConfigured) {
    FakeWorker worker;
    worker.reply = halfSecondWav();
    const CloudTtsEngine engine(configFor(worker));
    QTemporaryDir dir;
    engine.synthesize(QStringLiteral("テストです。"), dir.filePath(QStringLiteral("o.wav")));
    ASSERT_EQ(worker.requestBodies.size(), 1u);
    EXPECT_FALSE(worker.requestBodies[0].contains(QStringLiteral("voice")));
    EXPECT_FALSE(worker.requestBodies[0].contains(QStringLiteral("speaking_rate")));
}

TEST(CloudTts, WorkerRefusalBecomesAnErrorWithTheReason) {
    FakeWorker worker;
    worker.status = 429;
    worker.reply = QJsonDocument(QJsonObject{{QStringLiteral("error"), QStringLiteral("1日の上限に達しています")}}).toJson();
    const CloudTtsEngine engine(configFor(worker));
    QTemporaryDir dir;
    try {
        engine.synthesize(QStringLiteral("テストです。"), dir.filePath(QStringLiteral("o.wav")));
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        const QString what = QString::fromUtf8(e.what());
        EXPECT_TRUE(what.contains(QStringLiteral("HTTP 429"))) << what.toStdString();
        EXPECT_TRUE(what.contains(QStringLiteral("1日の上限"))) << what.toStdString();
    }
    EXPECT_FALSE(QFile::exists(dir.filePath(QStringLiteral("o.wav")))) << "no half-written file";
}

TEST(CloudTts, GarbageAudioIsAnError) {
    FakeWorker worker;
    worker.reply = "this is not a wav file";
    const CloudTtsEngine engine(configFor(worker));
    QTemporaryDir dir;
    EXPECT_THROW(engine.synthesize(QStringLiteral("テストです。"), dir.filePath(QStringLiteral("o.wav"))), std::runtime_error);
}

TEST(CloudTts, UnreachableWorkerIsAnErrorNotAHang) {
    ensureApp();
    CloudTtsConfig config;
    config.baseUrl = QStringLiteral("http://127.0.0.1:1");  // nothing listens here
    config.apiKey = QStringLiteral("k");
    config.timeoutMs = 3000;
    const CloudTtsEngine engine(config);
    QTemporaryDir dir;
    EXPECT_THROW(engine.synthesize(QStringLiteral("テストです。"), dir.filePath(QStringLiteral("o.wav"))), std::runtime_error);
}

TEST(CloudTts, RefusesToRunWithoutWorkerSettings) {
    ensureApp();
    const CloudTtsEngine engine{CloudTtsConfig{}};
    QTemporaryDir dir;
    EXPECT_THROW(engine.synthesize(QStringLiteral("テストです。"), dir.filePath(QStringLiteral("o.wav"))), std::runtime_error);
}

TEST(CloudTtsConfig, ReadsEnvironmentAndClampsRate) {
    qputenv("CLOUD_RAG_URL", "https://example.workers.dev//");
    qputenv("CLOUD_RAG_API_KEY", " key ");
    qputenv("RAGREEL_CLOUD_TTS_VOICE", "ja-JP-Neural2-C");
    qputenv("RAGREEL_CLOUD_TTS_RATE", "9");
    const CloudTtsConfig c = CloudTtsConfig::fromEnvironment();
    EXPECT_EQ(c.baseUrl, QStringLiteral("https://example.workers.dev"));
    EXPECT_EQ(c.apiKey, QStringLiteral("key"));
    EXPECT_EQ(c.voice, QStringLiteral("ja-JP-Neural2-C"));
    EXPECT_DOUBLE_EQ(c.speakingRate, 1.3);
    EXPECT_TRUE(c.isConfigured());
    // The dedicated variables (set by the Houdini panel) take precedence.
    qputenv("RAGREEL_CLOUD_TTS_URL", "https://panel.workers.dev");
    qputenv("RAGREEL_CLOUD_TTS_KEY", "panel-key");
    const CloudTtsConfig d = CloudTtsConfig::fromEnvironment();
    EXPECT_EQ(d.baseUrl, QStringLiteral("https://panel.workers.dev"));
    EXPECT_EQ(d.apiKey, QStringLiteral("panel-key"));
    qunsetenv("RAGREEL_CLOUD_TTS_URL");
    qunsetenv("RAGREEL_CLOUD_TTS_KEY");
    qunsetenv("CLOUD_RAG_URL");
    EXPECT_FALSE(CloudTtsConfig::fromEnvironment().isConfigured());
    qunsetenv("CLOUD_RAG_API_KEY");
    qunsetenv("RAGREEL_CLOUD_TTS_VOICE");
    qunsetenv("RAGREEL_CLOUD_TTS_RATE");
}
