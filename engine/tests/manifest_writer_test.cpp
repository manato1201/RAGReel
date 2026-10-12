#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "manifest/manifest_writer.h"

namespace {

QByteArray readAll(const QString& path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

QJsonObject readJsonObject(const QString& path) {
    return QJsonDocument::fromJson(readAll(path)).object();
}

ManifestEntryInfo makeEntry(const QString& id) {
    ManifestEntryInfo entry;
    entry.id = id;
    entry.slug = id;
    entry.title = QStringLiteral("テスト動画");
    entry.createdAtIso = QStringLiteral("2026-10-11T00:00:00Z");
    entry.durationSec = 12.0;
    entry.tags = QStringList{QStringLiteral("houdini21")};
    return entry;
}

// A real file for publish() to copy; its content is irrelevant here.
QString makeDummyVideo(const QTemporaryDir& dir) {
    const QString path = dir.filePath(QStringLiteral("src.webm"));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("webm-bytes");
    return path;
}

class ManifestWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Cloud upload is opt-in via these; a stray value in the developer's
        // shell must never turn a unit test into a network call.
        qunsetenv("GALLERY_UPLOAD_URL");
        qunsetenv("GALLERY_UPLOAD_TOKEN");
    }
};

}  // namespace

TEST_F(ManifestWriterTest, WritesChaptersAndEncodeSummaryIntoMetadata) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));

    ManifestVideoDetail detail;
    detail.chapters.push_back({QStringLiteral("概要"), QStringLiteral("other"), 0.0, 4.0});
    detail.chapters.push_back({QStringLiteral("ノード"), QStringLiteral("node"), 4.0, 8.0});
    detail.frameWidth = 1280;
    detail.frameHeight = 720;
    detail.fps = 15;
    detail.fileSizeBytes = 1234567;

    ManifestWriter::publish(out, makeEntry(QStringLiteral("t1")), detail, makeDummyVideo(dir), QImage());

    const QJsonObject meta = readJsonObject(out + QStringLiteral("/videos/t1/metadata.json"));
    const QJsonArray chapters = meta.value(QStringLiteral("chapters")).toArray();
    ASSERT_EQ(chapters.size(), 2);
    EXPECT_EQ(chapters[0].toObject().value(QStringLiteral("title")).toString(), QStringLiteral("概要"));
    EXPECT_EQ(chapters[1].toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("node"));
    EXPECT_DOUBLE_EQ(chapters[1].toObject().value(QStringLiteral("start_sec")).toDouble(), 4.0);
    EXPECT_DOUBLE_EQ(chapters[1].toObject().value(QStringLiteral("duration_sec")).toDouble(), 8.0);

    const QJsonObject render = meta.value(QStringLiteral("render")).toObject();
    EXPECT_EQ(render.value(QStringLiteral("width")).toInt(), 1280);
    EXPECT_EQ(render.value(QStringLiteral("height")).toInt(), 720);
    EXPECT_EQ(render.value(QStringLiteral("fps")).toInt(), 15);
    EXPECT_EQ(static_cast<qint64>(render.value(QStringLiteral("file_size_bytes")).toDouble()), 1234567);
}

TEST_F(ManifestWriterTest, OmitsUnknownEncodeSummaryInsteadOfWritingZeros) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));

    ManifestWriter::publish(out, makeEntry(QStringLiteral("t2")), ManifestVideoDetail{}, makeDummyVideo(dir),
                            QImage());

    const QJsonObject meta = readJsonObject(out + QStringLiteral("/videos/t2/metadata.json"));
    EXPECT_TRUE(meta.value(QStringLiteral("chapters")).toArray().isEmpty());
    const QJsonObject render = meta.value(QStringLiteral("render")).toObject();
    EXPECT_FALSE(render.contains(QStringLiteral("width")));
    EXPECT_FALSE(render.contains(QStringLiteral("fps")));
    EXPECT_FALSE(render.contains(QStringLiteral("file_size_bytes")));
}

TEST_F(ManifestWriterTest, MirrorsManifestAndMetadataAsScriptsForFileProtocol) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));

    ManifestWriter::publish(out, makeEntry(QStringLiteral("t3")), ManifestVideoDetail{}, makeDummyVideo(dir),
                            QImage());

    // The dashboard is opened via file://, where fetch() of local files is
    // blocked, so app.js loads these as classic scripts instead.
    EXPECT_TRUE(readAll(out + QStringLiteral("/manifest.js")).startsWith("window.__VF_MANIFEST__ = ["));
    EXPECT_TRUE(readAll(out + QStringLiteral("/videos/t3/metadata.js")).startsWith("window.__VF_METADATA__ = {"));
}

TEST_F(ManifestWriterTest, NewestEntryComesFirstAndSameIdReplaces) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));
    const QString video = makeDummyVideo(dir);

    ManifestWriter::publish(out, makeEntry(QStringLiteral("a")), ManifestVideoDetail{}, video, QImage());
    ManifestWriter::publish(out, makeEntry(QStringLiteral("b")), ManifestVideoDetail{}, video, QImage());
    ManifestWriter::publish(out, makeEntry(QStringLiteral("a")), ManifestVideoDetail{}, video, QImage());

    const QJsonArray manifest = QJsonDocument::fromJson(readAll(out + QStringLiteral("/manifest.json"))).array();
    ASSERT_EQ(manifest.size(), 2);
    EXPECT_EQ(manifest[0].toObject().value(QStringLiteral("id")).toString(), QStringLiteral("a"));
    EXPECT_EQ(manifest[1].toObject().value(QStringLiteral("id")).toString(), QStringLiteral("b"));
}

TEST_F(ManifestWriterTest, WritesSubtitlesVttAndRecordsPathWhenSegmentsGiven) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));

    ManifestVideoDetail detail;
    detail.subtitles.push_back({QStringLiteral("最初の文です。"), 0.2, 2.1});
    detail.subtitles.push_back({QStringLiteral("次の文です。"), 2.6, 4.0});
    detail.subtitlesExact = true;

    ManifestWriter::publish(out, makeEntry(QStringLiteral("sub1")), detail, makeDummyVideo(dir), QImage());

    const QJsonObject meta = readJsonObject(out + QStringLiteral("/videos/sub1/metadata.json"));
    const QJsonObject render = meta.value(QStringLiteral("render")).toObject();
    EXPECT_EQ(render.value(QStringLiteral("subtitles")).toString(), QStringLiteral("videos/sub1/subtitles.vtt"));
    EXPECT_TRUE(render.value(QStringLiteral("subtitles_exact")).toBool());

    const QString vtt = QString::fromUtf8(readAll(out + QStringLiteral("/videos/sub1/subtitles.vtt")));
    EXPECT_TRUE(vtt.startsWith(QStringLiteral("WEBVTT")));
    EXPECT_TRUE(vtt.contains(QStringLiteral("00:00:02.600 --> 00:00:04.000")));
}

TEST_F(ManifestWriterTest, OmitsSubtitlesWhenNoSegments) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));
    ManifestWriter::publish(out, makeEntry(QStringLiteral("nosub")), ManifestVideoDetail{}, makeDummyVideo(dir), QImage());
    const QJsonObject render =
        readJsonObject(out + QStringLiteral("/videos/nosub/metadata.json")).value(QStringLiteral("render")).toObject();
    EXPECT_FALSE(render.contains(QStringLiteral("subtitles")));
    EXPECT_FALSE(QFile::exists(out + QStringLiteral("/videos/nosub/subtitles.vtt")));
}
