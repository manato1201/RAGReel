#include "narration/voicevox_narration_engine.h"

#include "narration/pronunciation.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// narration_text
// ---------------------------------------------------------------------------
namespace narration_text {

QString normalizeForSpeech(const QString& text) {
    QString s = text;
    static const QRegularExpression url(QStringLiteral("https?://[^\\s)）」』】]+"));
    s.replace(url, QStringLiteral("リンク"));
    s.remove(QRegularExpression(QStringLiteral("[`*_#>]+")));
    // Markdown links "[text](url)" leave "text(リンク)" behind; drop the bracket noise.
    s.replace(QStringLiteral("(リンク)"), QString());
    s.replace(QStringLiteral("（リンク）"), QString());
    s.replace(QRegularExpression(QStringLiteral("([。、！？!?])\\1{1,}")), QStringLiteral("\\1"));
    s.replace(QRegularExpression(QStringLiteral("[ \\t\\x{3000}]+")), QStringLiteral(" "));
    s.replace(QRegularExpression(QStringLiteral("\\n{2,}")), QStringLiteral("\n"));
    return s.trimmed();
}

namespace {

bool isSentenceEnd(QChar c) {
    return c == QChar(0x3002) /*。*/ || c == QChar(0xFF01) /*！*/ || c == QChar(0xFF1F) /*？*/ ||
           c == QLatin1Char('!') || c == QLatin1Char('?') || c == QLatin1Char('\n');
}

void hardCut(const QString& piece, int maxChars, QStringList& out) {
    const QChar comma(0x3001);  // 、
    QString rest = piece;
    while (rest.size() > maxChars) {
        int cut = rest.lastIndexOf(comma, maxChars - 1);
        if (cut < maxChars / 3) cut = maxChars - 1;  // no usable 、: hard cut
        out << rest.left(cut + 1).trimmed();
        rest = rest.mid(cut + 1);
    }
    if (!rest.trimmed().isEmpty()) out << rest.trimmed();
}

}  // namespace

QStringList splitIntoChunks(const QString& text, int maxChars) {
    maxChars = std::max(maxChars, 10);
    QStringList sentences;
    QString current;
    for (const QChar c : text) {
        current += c;
        if (isSentenceEnd(c)) {
            if (!current.trimmed().isEmpty()) sentences << current.trimmed();
            current.clear();
        }
    }
    if (!current.trimmed().isEmpty()) sentences << current.trimmed();

    QStringList out;
    for (const QString& sentence : sentences) {
        if (sentence.size() <= maxChars) {
            out << sentence;
        } else {
            hardCut(sentence, maxChars, out);
        }
    }
    return out;
}

std::optional<int> resolveSpeakerId(const QByteArray& speakersJson, const QString& speakerName,
                                    const QString& styleName) {
    const QJsonDocument doc = QJsonDocument::fromJson(speakersJson);
    if (!doc.isArray()) return std::nullopt;
    const QJsonArray speakers = doc.array();
    for (int pass = 0; pass < 2; ++pass) {  // pass 0: exact name, pass 1: substring
        for (const QJsonValue& sv : speakers) {
            const QJsonObject speaker = sv.toObject();
            const QString name = speaker.value(QStringLiteral("name")).toString();
            const bool nameOk = speakerName.isEmpty() ||
                                (pass == 0 ? name == speakerName : name.contains(speakerName));
            if (!nameOk) continue;
            const QJsonArray styles = speaker.value(QStringLiteral("styles")).toArray();
            for (const QJsonValue& stv : styles) {
                const QJsonObject style = stv.toObject();
                if (styleName.isEmpty() ||
                    style.value(QStringLiteral("name")).toString() == styleName) {
                    return style.value(QStringLiteral("id")).toInt();
                }
            }
        }
    }
    return std::nullopt;
}

QJsonObject applyQueryTuning(QJsonObject query, const QueryTuning& t) {
    query[QStringLiteral("speedScale")] = t.speed;
    query[QStringLiteral("pitchScale")] = t.pitch;
    query[QStringLiteral("intonationScale")] = t.intonation;
    query[QStringLiteral("volumeScale")] = t.volume;
    query[QStringLiteral("prePhonemeLength")] = t.preSeconds;
    query[QStringLiteral("postPhonemeLength")] = t.postSeconds;
    query[QStringLiteral("outputSamplingRate")] = t.sampleRate;
    query[QStringLiteral("outputStereo")] = false;
    return query;
}

}  // namespace narration_text

// ---------------------------------------------------------------------------
// wav_util
// ---------------------------------------------------------------------------
namespace wav_util {

namespace {
uint32_t rd32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
void wr32(QByteArray& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.append(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void wr16(QByteArray& b, uint16_t v) {
    b.append(static_cast<char>(v & 0xFF));
    b.append(static_cast<char>((v >> 8) & 0xFF));
}
}  // namespace

std::optional<Pcm16Mono> parsePcm16(const QByteArray& wav) {
    const auto* d = reinterpret_cast<const unsigned char*>(wav.constData());
    const qsizetype n = wav.size();
    if (n < 12 || std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }
    int channels = 0, rate = 0, bits = 0, format = 0;
    qsizetype pos = 12;
    while (pos + 8 <= n) {
        const uint32_t size = rd32(d + pos + 4);
        const qsizetype body = pos + 8;
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && body + 16 <= n) {
            format = rd16(d + body);
            channels = rd16(d + body + 2);
            rate = static_cast<int>(rd32(d + body + 4));
            bits = rd16(d + body + 14);
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate <= 0) {
                return std::nullopt;
            }
            qsizetype avail = std::min<qsizetype>(size, n - body);
            const qsizetype frames = avail / (2 * channels);
            Pcm16Mono out;
            out.sampleRate = rate;
            out.samples.resize(static_cast<size_t>(frames));
            for (qsizetype i = 0; i < frames; ++i) {
                const unsigned char* p = d + body + i * 2 * channels;
                int v = static_cast<int16_t>(rd16(p));
                if (channels == 2) v = (v + static_cast<int16_t>(rd16(p + 2))) / 2;
                out.samples[static_cast<size_t>(i)] = static_cast<int16_t>(v);
            }
            return out;
        }
        pos = body + size + (size & 1);
    }
    return std::nullopt;
}

QByteArray buildWavPcm16Mono(int sampleRate, const std::vector<int16_t>& samples) {
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    QByteArray out;
    out.reserve(static_cast<qsizetype>(44 + dataBytes));
    out.append("RIFF", 4);
    wr32(out, 36 + dataBytes);
    out.append("WAVEfmt ", 8);
    wr32(out, 16);
    wr16(out, 1);   // PCM
    wr16(out, 1);   // mono
    wr32(out, static_cast<uint32_t>(sampleRate));
    wr32(out, static_cast<uint32_t>(sampleRate) * 2);
    wr16(out, 2);   // block align
    wr16(out, 16);  // bits
    out.append("data", 4);
    wr32(out, dataBytes);
    for (int16_t s : samples) wr16(out, static_cast<uint16_t>(s));
    return out;
}

std::vector<int16_t> resample(const std::vector<int16_t>& in, int fromRate, int toRate) {
    if (in.empty() || fromRate <= 0 || toRate <= 0 || fromRate == toRate) return in;
    const double ratio = static_cast<double>(toRate) / fromRate;
    const size_t outLen = static_cast<size_t>(std::llround(in.size() * ratio));
    std::vector<int16_t> out(outLen);
    for (size_t i = 0; i < outLen; ++i) {
        const double src = i / ratio;
        const size_t i0 = static_cast<size_t>(src);
        const size_t i1 = std::min(i0 + 1, in.size() - 1);
        const double frac = src - static_cast<double>(i0);
        out[i] = static_cast<int16_t>(std::lround(in[i0] * (1.0 - frac) + in[i1] * frac));
    }
    return out;
}

std::vector<int16_t> silence(int sampleRate, double seconds) {
    const size_t n = seconds <= 0 ? 0 : static_cast<size_t>(std::llround(seconds * sampleRate));
    return std::vector<int16_t>(n, 0);
}

}  // namespace wav_util

// ---------------------------------------------------------------------------
// HTTP + engine
// ---------------------------------------------------------------------------
namespace {

QByteArray httpCall(bool post, const QUrl& url, const QByteArray& body, int timeoutMs) {
    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    if (post) request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* reply = post ? manager.post(request, body) : manager.get(request);

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

    // The reply must be gone BEFORE `manager` is destroyed (locals are destroyed in reverse order, and
    // `manager` was declared first). The old deleteLater() left it pending past the manager's lifetime;
    // after a timed-out request (an engine that is still starting up) the next call's event loop then ran
    // that deferred delete on a reply whose manager no longer existed, and crashed (found 2026-10-19 when
    // the engine began auto-starting VOICEVOX and probing it while it booted). Deleting directly is safe
    // here: we are outside any of the reply's own signal handlers.
    struct Cleanup {
        QNetworkReply* r;
        ~Cleanup() {
            QObject::disconnect(r, nullptr, nullptr, nullptr);
            r->abort();
            delete r;
        }
    } cleanup{reply};

    if (timedOut) throw std::runtime_error("VOICEVOX: request timed out: " + url.toString().toStdString());
    if (reply->error() != QNetworkReply::NoError) {
        throw std::runtime_error("VOICEVOX: " + reply->errorString().toStdString() + " (" +
                                 url.toString().toStdString() + ")");
    }
    return reply->readAll();
}

QUrl endpoint(const QString& base, const QString& path, const QUrlQuery& query = QUrlQuery()) {
    QUrl url(base + path);
    if (!query.isEmpty()) url.setQuery(query);
    return url;
}

double envDouble(const char* name, double fallback) {
    const QByteArray v = qgetenv(name);
    if (v.isEmpty()) return fallback;
    bool ok = false;
    const double d = v.toDouble(&ok);
    return ok ? d : fallback;
}

}  // namespace

VoicevoxConfig VoicevoxConfig::fromEnvironment() {
    VoicevoxConfig c;
    if (const QByteArray v = qgetenv("RAGREEL_VOICEVOX_URL"); !v.isEmpty()) {
        c.baseUrl = QString::fromUtf8(v);
        while (c.baseUrl.endsWith(QLatin1Char('/'))) c.baseUrl.chop(1);
    }
    c.speakerId = qEnvironmentVariableIsSet("RAGREEL_VOICEVOX_SPEAKER_ID")
                      ? qEnvironmentVariableIntValue("RAGREEL_VOICEVOX_SPEAKER_ID")
                      : -1;
    if (const QString name = qEnvironmentVariable("RAGREEL_VOICEVOX_SPEAKER"); !name.isEmpty()) {
        c.speakerName = name;  // otherwise keep the struct default (春日部つむぎ)
    }
    if (const QString style = qEnvironmentVariable("RAGREEL_VOICEVOX_STYLE"); !style.isEmpty()) {
        c.styleName = style;
    }
    c.tuning.speed = envDouble("RAGREEL_VOICEVOX_SPEED", c.tuning.speed);
    c.tuning.intonation = envDouble("RAGREEL_VOICEVOX_INTONATION", c.tuning.intonation);
    c.tuning.pitch = envDouble("RAGREEL_VOICEVOX_PITCH", c.tuning.pitch);
    c.chunkGapSeconds = envDouble("RAGREEL_VOICEVOX_GAP", c.chunkGapSeconds);
    c.dictionaryPath = qEnvironmentVariable("RAGREEL_TTS_DICTIONARY");
    if (c.dictionaryPath.isEmpty()) {
        // Default: assets/tts_dictionary.json next to the exe (copied by CMake), when it is there.
        const QString fallback = pronunciation::defaultDictionaryPath();
        if (QFileInfo::exists(fallback)) c.dictionaryPath = fallback;
    }
    return c;
}

VoicevoxNarrationEngine::VoicevoxNarrationEngine(VoicevoxConfig config) : config_(std::move(config)) {}

bool VoicevoxNarrationEngine::isAvailable(int timeoutMs) const {
    try {
        const QByteArray v = httpCall(false, endpoint(config_.baseUrl, QStringLiteral("/version")), {}, timeoutMs);
        return !v.isEmpty();
    } catch (const std::exception&) {
        return false;
    }
}

int VoicevoxNarrationEngine::resolveSpeaker() const {
    if (config_.speakerId >= 0) return config_.speakerId;
    const QByteArray json =
        httpCall(false, endpoint(config_.baseUrl, QStringLiteral("/speakers")), {}, config_.timeoutMs);
    const auto id = narration_text::resolveSpeakerId(json, config_.speakerName, config_.styleName);
    if (!id) {
        throw std::runtime_error("VOICEVOX: speaker not found (name='" +
                                 config_.speakerName.toStdString() + "', style='" +
                                 config_.styleName.toStdString() + "')");
    }
    return *id;
}

int VoicevoxNarrationEngine::registerUserDictionary() const {
    if (config_.dictionaryPath.isEmpty()) return 0;
    QFile file(config_.dictionaryPath);
    if (!file.open(QIODevice::ReadOnly)) return 0;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) return 0;

    int added = 0;
    try {
        QStringList known;
        const QByteArray current =
            httpCall(false, endpoint(config_.baseUrl, QStringLiteral("/user_dict")), {}, config_.timeoutMs);
        const QJsonObject dict = QJsonDocument::fromJson(current).object();
        for (auto it = dict.begin(); it != dict.end(); ++it) {
            known << it.value().toObject().value(QStringLiteral("surface")).toString();
        }
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject e = v.toObject();
            const QString surface = e.value(QStringLiteral("surface")).toString();
            const QString pron = e.value(QStringLiteral("pronunciation")).toString();
            if (surface.isEmpty() || pron.isEmpty() || known.contains(surface)) continue;
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("surface"), surface);
            q.addQueryItem(QStringLiteral("pronunciation"), pron);
            q.addQueryItem(QStringLiteral("accent_type"),
                           QString::number(e.value(QStringLiteral("accent_type")).toInt(0)));
            httpCall(true, endpoint(config_.baseUrl, QStringLiteral("/user_dict_word"), q), {}, config_.timeoutMs);
            ++added;
        }
    } catch (const std::exception&) {
        // best effort, see header
    }
    return added;
}

NarrationResult VoicevoxNarrationEngine::synthesize(const QString& text, const QString& wavPath) const {
    const QString normalized = narration_text::normalizeForSpeech(text);
    const QStringList chunks = narration_text::splitIntoChunks(normalized, config_.maxChunkChars);
    if (chunks.isEmpty()) throw std::runtime_error("VOICEVOX: nothing to synthesize (empty text)");

    const int speaker = resolveSpeaker();
    const int rate = config_.tuning.sampleRate;

    std::vector<int16_t> all;
    std::vector<NarrationSegment> segments;
    for (int i = 0; i < chunks.size(); ++i) {
        QUrlQuery qq;
        qq.addQueryItem(QStringLiteral("text"), chunks[i]);
        qq.addQueryItem(QStringLiteral("speaker"), QString::number(speaker));
        const QByteArray queryJson = httpCall(
            true, endpoint(config_.baseUrl, QStringLiteral("/audio_query"), qq), {}, config_.timeoutMs);
        QJsonDocument queryDoc = QJsonDocument::fromJson(queryJson);
        if (!queryDoc.isObject()) throw std::runtime_error("VOICEVOX: /audio_query returned non-object JSON");
        const QJsonObject tuned = narration_text::applyQueryTuning(queryDoc.object(), config_.tuning);

        QUrlQuery sq;
        sq.addQueryItem(QStringLiteral("speaker"), QString::number(speaker));
        const QByteArray wav = httpCall(true, endpoint(config_.baseUrl, QStringLiteral("/synthesis"), sq),
                                        QJsonDocument(tuned).toJson(QJsonDocument::Compact),
                                        config_.timeoutMs);
        const auto pcm = wav_util::parsePcm16(wav);
        if (!pcm || pcm->samples.empty()) {
            throw std::runtime_error("VOICEVOX: /synthesis returned an unreadable or empty WAV");
        }
        const std::vector<int16_t> fit = wav_util::resample(pcm->samples, pcm->sampleRate, rate);
        // Subtitle timing (V7): this chunk occupies [chunkStart, chunkEnd) in the
        // final WAV. VOICEVOX pads every synthesis with the configured pre/post
        // phoneme silence, so the audible speech is that window minus the
        // padding -- known exactly, no signal analysis needed.
        const double chunkStart = static_cast<double>(all.size()) / rate;
        const double chunkEnd = chunkStart + static_cast<double>(fit.size()) / rate;
        NarrationSegment seg;
        seg.text = chunks[i];
        seg.startSec = std::min(chunkStart + config_.tuning.preSeconds, chunkEnd);
        seg.endSec = std::max(seg.startSec, chunkEnd - config_.tuning.postSeconds);
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
        throw std::runtime_error("VOICEVOX: cannot write " + wavPath.toStdString());
    }
    file.write(out);
    file.close();

    NarrationResult result;
    result.wavPath = wavPath;
    result.engineName = config_.speakerId >= 0
        ? QStringLiteral("VOICEVOX:スタイルID %1").arg(config_.speakerId)
        : QStringLiteral("VOICEVOX:%1(%2)").arg(config_.speakerName, config_.styleName);
    result.durationSeconds = static_cast<double>(all.size()) / rate;
    result.segments = std::move(segments);
    result.segmentsExact = true;
    return result;
}
