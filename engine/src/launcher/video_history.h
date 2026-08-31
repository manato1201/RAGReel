#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

// Reads output/manifest.json (ManifestWriter's own schema, see
// manifest_writer.h) so the launcher can show a "history" list of
// already-generated videos without the user having to open output/index.html
// in a browser separately -- and so a generated video is one click away
// instead of a manual folder dig.
//
// Read-only: this class never writes manifest.json (that stays
// ManifestWriter's job, from inside video_factory_cloudrag_poc.exe). It just
// re-reads the same file after each run finishes.
class VideoHistory : public QObject {
    Q_OBJECT
    // Each entry: {id, title, createdAt, durationSec, status, videoPath (absolute),
    // hasVideo (bool -- video.webm actually exists on disk)}.
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)

public:
    explicit VideoHistory(QObject* parent = nullptr);

    QVariantList entries() const { return entries_; }

    // Re-reads output/manifest.json from disk. Safe to call anytime
    // (missing/empty/malformed manifest.json just yields an empty list, same
    // as a fresh install that has never published a video yet).
    Q_INVOKABLE void refresh();

    // Opens output/ in the OS file browser (Explorer).
    Q_INVOKABLE void openOutputFolder();

    // Opens output/videos/<id>/video.webm in the OS-default video player.
    Q_INVOKABLE void openVideo(const QString& id);

    // Opens output/index.html (the full web dashboard) in the default browser.
    Q_INVOKABLE void openDashboard();

signals:
    void entriesChanged();

private:
    static QString outputDir();

    QVariantList entries_;
};
