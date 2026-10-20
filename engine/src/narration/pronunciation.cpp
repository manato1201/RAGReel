#include "narration/pronunciation.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace pronunciation {

namespace {

bool isAsciiOnly(const QString& s) {
    for (const QChar c : s) {
        if (c.unicode() > 0x7F) return false;
    }
    return true;
}

// Letter names as a Japanese speaker would say them.
QString letterName(QChar c) {
    static const char16_t* const kNames[26] = {
        u"エー",   u"ビー",   u"シー",   u"ディー", u"イー",   u"エフ",     u"ジー",
        u"エイチ", u"アイ",   u"ジェー", u"ケー",   u"エル",   u"エム",     u"エヌ",
        u"オー",   u"ピー",   u"キュー", u"アール", u"エス",   u"ティー",   u"ユー",
        u"ブイ",   u"ダブリュー", u"エックス", u"ワイ", u"ゼット"};
    const ushort u = c.toUpper().unicode();
    if (u >= 'A' && u <= 'Z') return QString::fromUtf16(kNames[u - 'A']);
    return QString(c);
}

}  // namespace

std::vector<Entry> parseDictionary(const QByteArray& json) {
    std::vector<Entry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return out;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString surface = o.value(QStringLiteral("surface")).toString().trimmed();
        const QString reading = o.value(QStringLiteral("pronunciation")).toString().trimmed();
        if (surface.isEmpty() || reading.isEmpty()) continue;
        out.push_back({surface, reading});
    }
    return out;
}

std::vector<Entry> loadDictionary(const QString& path) {
    if (path.isEmpty()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return parseDictionary(f.readAll());
}

QString defaultDictionaryPath() {
    const QString fromEnv = qEnvironmentVariable("RAGREEL_TTS_DICTIONARY");
    if (!fromEnv.isEmpty()) return fromEnv;
    const QString base = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QDir::currentPath();
    return QDir(base).filePath(QStringLiteral("assets/tts_dictionary.json"));
}

QString replaceTerms(const QString& text, const std::vector<Entry>& dict) {
    std::vector<Entry> sorted = dict;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Entry& a, const Entry& b) { return a.surface.size() > b.surface.size(); });
    QString out = text;
    for (const Entry& e : sorted) {
        if (isAsciiOnly(e.surface)) {
            // Whole word only: not preceded/followed by a letter or digit.
            const QRegularExpression re(
                QStringLiteral("(?<![A-Za-z0-9])%1(?![A-Za-z0-9])").arg(QRegularExpression::escape(e.surface)),
                QRegularExpression::CaseInsensitiveOption);
            out.replace(re, e.reading);
        } else {
            out.replace(e.surface, e.reading);
        }
    }
    return out;
}

QString spellOutAcronyms(const QString& text) {
    static const QRegularExpression re(QStringLiteral("(?<![A-Za-z0-9])[A-Z]{2,6}(?![A-Za-z0-9])"));
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        for (const QChar c : m.captured()) out += letterName(c);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

QString prepareForSapi(const QString& text) {
    static const std::vector<Entry> dict = loadDictionary(defaultDictionaryPath());
    return spellOutAcronyms(replaceTerms(text, dict));
}

}  // namespace pronunciation
