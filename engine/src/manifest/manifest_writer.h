#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <vector>

#include "ragclient/cloud_rag_client.h"

// Publishes one rendered video into the web dashboard's data contract
// (docs/architecture/video-factory-design.md §5: manifest.json + per-video
// metadata.json). This is the ManifestWriter component named in §2/§7 --
// previously deferred, now implemented because generated videos need to
// actually show up in web/public without a manual copy step.

struct PipelineStageTiming {
    QString stage;  // machine key, e.g. "ingest"
    QString label;  // human label shown in the dashboard, e.g. "取り込み"
    double durationSec = 0.0;
};

struct ManifestEntryInfo {
    QString id;   // unique, also used as the videos/<id>/ directory name
    QString slug;
    QString title;
    QString createdAtIso;
    double durationSec = 0.0;
    QStringList tags;
    QString sourceTutorial;
};

struct ManifestVideoDetail {
    QString narrationSummary;
    std::vector<CloudRagSource> ragSources;
    std::vector<PipelineStageTiming> pipeline;
};

class ManifestWriter {
public:
    // Copies videoFilePath into web/public/videos/<id>/video.mp4, saves
    // thumbnail as thumb.jpg (skipped if null), writes videos/<id>/
    // metadata.json, and prepends/replaces this id's entry in
    // web/public/manifest.json (created fresh if it doesn't exist yet;
    // existing entries for other videos are preserved).
    // webPublicDir must be an absolute path to web/public.
    // Throws std::runtime_error on I/O or malformed-existing-manifest failure.
    static void publish(const QString& webPublicDir, const ManifestEntryInfo& entry,
                         const ManifestVideoDetail& detail, const QString& videoFilePath,
                         const QImage& thumbnail);
};
