#include "narration/cloud_tts_engine.h"

#include "narration/pronunciation.h"
#include "narration/voicevox_narration_engine.h"  // narration_text / wav_util

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <stdexcept>

namespace {

struct HttpReply {
    int status = 0;
    QByteArray body;
    QString voice;  // X-TTS-Voice response header
};

// Same shape as VoicevoxNarrationEngine's httpCall (the reply is deleted before the manager --
// see the comment there for the crash this ordering avoids), plus headers and the HTTP status.
HttpReply postJson(const QUrl& url, const QByteArray& apiKey, const QByteArray& body, int timeoutMs) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + apiKey);
    // An explicit, ordinary-client User-Agent (Cloudflare's bot rules reject some library defaults).
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("RAGReel/1.0 (cloud-tts)"));
    QNetworkReply* reply = manager.post(request, body);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        loop.quit();
    });
    timer.start(timeoutMs);
    loop.exec();

    struct Cleanup {
        QNetworkReply* r;
        ~Cleanup() {
            QObject::disconnect(r, nullptr, nullptr, nullptr);
            r->abort();
            delete r;
        }
    } cleanup{reply};

    if (timedOut) throw std::runtime_error("Cloud TTS: request timed out");
    HttpReply out;
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = reply->readAll();
    out.voice = QString::fromUtf8(reply->rawHeader("X-TTS-Voice"));
    if (out.status == 0) {
        // No HTTP response at all (DNS, connection refused, TLS ...)
        throw std::runtime_error("Cloud TTS: " + reply->errorString().toStdString());
    }
    return out;
}

QString workerErrorText(const QByteArray& body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QString message = doc.object().value(QStringLiteral("error")).toString();
        if (!message.isEmpty()) return message;
    }
    return QString::fromUtf8(body.left(160));
}

double envDouble(const char* name, double fallback) {
    const QByteArray v = qgetenv(name);
    if (v.isEmpty()) return fallback;
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : fallback;
}

}  // namespace

CloudTtsConfig CloudTtsConfig::fromEnvironment() {
    CloudTtsConfig c;
    // The dedicated variables win (the Houdini panel sets them from its Settings tab without touching the
    // Cloud RAG ones); the launcher / developers already have CLOUD_RAG_*.
    c.baseUrl = qEnvironmentVariable("RAGREEL_CLOUD_TTS_URL", qEnvironmentVariable("CLOUD_RAG_URL")).trimmed();
    while (c.baseUrl.endsWith(QLatin1Char('/'))) c.baseUrl.chop(1);
    c.apiKey = qEnvironmentVariable("RAGREEL_CLOUD_TTS_KEY", qEnvironmentVariable("CLOUD_RAG_API_KEY")).trimmed();
    c.voice = qEnvironmentVariable("RAGREEL_CLOUD_TTS_VOICE").trimmed();
    c.speakingRate = std::clamp(envDouble("RAGREEL_CLOUD_TTS_RATE", 1.0), 0.8, 1.3);
    return c;
}

NarrationResult CloudTtsEngine::synthesize(const QString& text, const QString& wavPath) const {
    if (!config_.isConfigured()) throw std::runtime_error("Cloud TTS: CLOUD_RAG_URL / CLOUD_RAG_API_KEY are not set");

    // Same text preparation as the SAPI path: dictionary terms -> katakana, ALL-CAPS acronyms spelled out.
    const QString prepared = pronunciation::prepareForSapi(narration_text::normalizeForSpeech(text));
    const QStringList chunks = narration_text::splitIntoChunks(prepared, config_.maxChunkChars);
    if (chunks.isEmpty()) throw std::runtime_error("Cloud TTS: nothing to synthesize (empty text)");

    const int rate = config_.sampleRate;
    const QUrl url(config_.baseUrl + QStringLiteral("/tts"));
    std::vector<int16_t> all;
    std::vector<NarrationSegment> segments;
    QString voice = config_.voice;

    for (int i = 0; i < chunks.size(); ++i) {
        QJsonObject req;
        req.insert(QStringLiteral("text"), chunks[i]);
        if (!config_.voice.isEmpty()) req.insert(QStringLiteral("voice"), config_.voice);
        if (config_.speakingRate != 1.0) req.insert(QStringLiteral("speaking_rate"), config_.speakingRate);
        const HttpReply reply = postJson(url, config_.apiKey.toUtf8(),
                                         QJsonDocument(req).toJson(QJsonDocument::Compact), config_.timeoutMs);
        if (reply.status != 200) {
            throw std::runtime_error(QStringLiteral("Cloud TTS: HTTP %1: %2")
                                         .arg(reply.status)
                                         .arg(workerErrorText(reply.body))
                                         .toStdString());
        }
        if (!reply.voice.isEmpty()) voice = reply.voice;
        const auto pcm = wav_util::parsePcm16(reply.body);
        if (!pcm || pcm->samples.empty()) throw std::runtime_error("Cloud TTS: the Worker returned an unreadable or empty WAV");

        const std::vector<int16_t> fit = wav_util::resample(pcm->samples, pcm->sampleRate, rate);
        const double chunkStart = static_cast<double>(all.size()) / rate;
        const double chunkEnd = chunkStart + static_cast<double>(fit.size()) / rate;
        NarrationSegment seg;
        seg.text = chunks[i];
        seg.startSec = chunkStart;
        seg.endSec = chunkEnd;
        segments.push_back(seg);
        all.insert(all.end(), fit.begin(), fit.end());
        if (i + 1 < chunks.size()) {
            const auto gap = wav_util::silence(rate, config_.chunkGapSeconds);
            all.insert(all.end(), gap.begin(), gap.end());
        }
    }

    const QByteArray out = wav_util::buildWavPcm16Mono(rate, all);
    QFile file(wavPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("Cloud TTS: cannot write " + wavPath.toStdString());
    }
    file.write(out);
    file.close();

    NarrationResult result;
    result.wavPath = wavPath;
    result.engineName = QStringLiteral("Google Cloud TTS:%1").arg(voice.isEmpty() ? QStringLiteral("(既定の声)") : voice);
    result.durationSeconds = static_cast<double>(all.size()) / rate;
    result.segments = std::move(segments);
    result.segmentsExact = true;
    return result;
}
