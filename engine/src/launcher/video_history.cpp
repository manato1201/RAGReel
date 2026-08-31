#include "video_history.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QVariantMap>

VideoHistory::VideoHistory(QObject* parent) : QObject(parent) {
    refresh();
}

QString VideoHistory::outputDir() {
    // Same convention as ProcessRunner::exePath()/main_cloudrag.cpp's
    // appRelativePath("output"): one output/ folder per install, next to the
    // exe, never a shared network location (see IMPROVEMENT_PLAN.md Phase 0
    // "設計書との乖離" #3).
    return QCoreApplication::applicationDirPath() + QStringLiteral("/output");
}

void VideoHistory::refresh() {
    entries_.clear();

    QFile file(outputDir() + QStringLiteral("/manifest.json"));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isArray()) {
            for (const QJsonValue& v : doc.array()) {
                const QJsonObject o = v.toObject();
                const QString id = o.value(QStringLiteral("id")).toString();
                const QString videoRelPath = o.value(QStringLiteral("video_path")).toString();
                const QString videoAbsPath = videoRelPath.isEmpty()
                    ? QString()
                    : outputDir() + QStringLiteral("/") + videoRelPath;

                QVariantMap entry;
                entry[QStringLiteral("id")] = id;
                entry[QStringLiteral("title")] = o.value(QStringLiteral("title")).toString();
                entry[QStringLiteral("createdAt")] = o.value(QStringLiteral("created_at")).toString();
                entry[QStringLiteral("durationSec")] = o.value(QStringLiteral("duration_sec")).toDouble();
                entry[QStringLiteral("status")] = o.value(QStringLiteral("status")).toString();
                entry[QStringLiteral("hasVideo")] = !videoAbsPath.isEmpty() && QFileInfo::exists(videoAbsPath);
                entries_.append(entry);
            }
        }
    }
    // A missing/malformed manifest.json (fresh install, or corrupted by a
    // crash mid-write before the atomic-replace fix) just yields an empty
    // list here -- not an error state worth surfacing, since "no videos yet"
    // looks identical to the user either way.

    emit entriesChanged();
}

void VideoHistory::openOutputFolder() {
    QDir().mkpath(outputDir());
    QDesktopServices::openUrl(QUrl::fromLocalFile(outputDir()));
}

void VideoHistory::openVideo(const QString& id) {
    const QString path = outputDir() + QStringLiteral("/videos/") + id + QStringLiteral("/video.webm");
    if (QFileInfo::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}

void VideoHistory::openDashboard() {
    const QString path = outputDir() + QStringLiteral("/index.html");
    if (QFileInfo::exists(path)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    }
}
