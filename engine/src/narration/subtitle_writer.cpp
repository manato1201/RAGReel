#include "narration/subtitle_writer.h"

#include <algorithm>
#include <cmath>

namespace subtitle_writer {

QString formatTimestamp(double seconds, bool vtt) {
    if (!(seconds > 0.0)) seconds = 0.0;  // also catches NaN
    qint64 ms = static_cast<qint64>(std::llround(seconds * 1000.0));
    const qint64 h = ms / 3600000;
    ms %= 3600000;
    const qint64 m = ms / 60000;
    ms %= 60000;
    const qint64 s = ms / 1000;
    ms %= 1000;
    return QStringLiteral("%1:%2:%3%4%5")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(vtt ? QLatin1Char('.') : QLatin1Char(','))
        .arg(ms, 3, 10, QLatin1Char('0'));
}

QStringList wrapLines(const QString& text, int maxChars, int maxLines) {
    QStringList lines;
    QString rest = text.trimmed();
    if (maxChars < 1) maxChars = 1;
    if (maxLines < 1) maxLines = 1;
    while (!rest.isEmpty()) {
        if (lines.size() == maxLines - 1 || rest.size() <= maxChars) {
            lines << rest;  // last allowed line takes everything that remains
            break;
        }
        int cut = maxChars;
        for (int i = maxChars; i >= maxChars / 2; --i) {  // prefer a break after 、 or 。
            const QChar c = rest.at(i - 1);
            if (c == QChar(0x3001) || c == QChar(0x3002)) {
                cut = i;
                break;
            }
        }
        lines << rest.left(cut);
        rest = rest.mid(cut).trimmed();
    }
    return lines;
}

namespace {

QString build(const std::vector<NarrationSegment>& segments, bool vtt) {
    QString out;
    if (vtt) out += QStringLiteral("WEBVTT\n\n");
    int n = 0;
    for (const NarrationSegment& s : segments) {
        if (!(s.endSec > s.startSec) || s.text.trimmed().isEmpty()) continue;
        ++n;
        if (!vtt) out += QString::number(n) + QLatin1Char('\n');
        out += formatTimestamp(s.startSec, vtt) + QStringLiteral(" --> ") + formatTimestamp(s.endSec, vtt) +
               QLatin1Char('\n');
        out += wrapLines(s.text).join(QLatin1Char('\n')) + QStringLiteral("\n\n");
    }
    return out;
}

}  // namespace

QString toSrt(const std::vector<NarrationSegment>& segments) { return build(segments, false); }
QString toVtt(const std::vector<NarrationSegment>& segments) { return build(segments, true); }

}  // namespace subtitle_writer
