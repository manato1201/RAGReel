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
    detail.frameWidth = 1920;
    detail.frameHeight = 1080;
    detail.fps = 30;
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
    EXPECT_EQ(render.value(QStringLiteral("width")).toInt(), 1920);
    EXPECT_EQ(render.value(QStringLiteral("height")).toInt(), 1080);
    EXPECT_EQ(render.value(QStringLiteral("fps")).toInt(), 30);
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

TEST_F(ManifestWriterTest, WritesChapterPointsSourcesAndEntryChapterCount) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));

    ManifestVideoDetail detail;
    ChapterInfo intro{QStringLiteral("概要"), QStringLiteral("other"), 0.0, 4.0, {}, {}};
    intro.points = QStringList{QStringLiteral("要点1"), QStringLiteral("要点2")};
    ChapterInfo ref{QStringLiteral("参考"), QStringLiteral("other"), 4.0, 3.0, {}, {}};
    ref.sources = QStringList{QStringLiteral("pyro_solver.md")};
    ChapterInfo bare{QStringLiteral("手順"), QStringLiteral("node"), 7.0, 5.0, {}, {}};
    detail.chapters = {intro, ref, bare};

    ManifestWriter::publish(out, makeEntry(QStringLiteral("t6")), detail, makeDummyVideo(dir), QImage());

    const QJsonArray chapters =
        readJsonObject(out + QStringLiteral("/videos/t6/metadata.json")).value(QStringLiteral("chapters")).toArray();
    ASSERT_EQ(chapters.size(), 3);
    EXPECT_EQ(chapters[0].toObject().value(QStringLiteral("points")).toArray().size(), 2);
    EXPECT_EQ(chapters[1].toObject().value(QStringLiteral("sources")).toArray()[0].toString(),
              QStringLiteral("pyro_solver.md"));
    // Optional fields are omitted, not written empty.
    EXPECT_FALSE(chapters[2].toObject().contains(QStringLiteral("points")));
    EXPECT_FALSE(chapters[2].toObject().contains(QStringLiteral("sources")));

    const QJsonArray manifest = QJsonDocument::fromJson(readAll(out + QStringLiteral("/manifest.json"))).array();
    EXPECT_EQ(manifest[0].toObject().value(QStringLiteral("chapter_count")).toInt(), 3);
}

TEST_F(ManifestWriterTest, OmitsChapterCountWhenChaptersUnknown) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));

    ManifestWriter::publish(out, makeEntry(QStringLiteral("t7")), ManifestVideoDetail{}, makeDummyVideo(dir),
                            QImage());

    const QJsonArray manifest = QJsonDocument::fromJson(readAll(out + QStringLiteral("/manifest.json"))).array();
    EXPECT_FALSE(manifest[0].toObject().contains(QStringLiteral("chapter_count")));
}

// ---- 出典(ギャラリーの「RAG 出典」)。2026-11-17: Houdini の動画は rag_sources が空で、「出典なし」になっていた ----

namespace {

QVariantMap sourceItem(const QString& title, const QString& db, bool cited) {
    QVariantMap m;
    m[QStringLiteral("title")] = title;
    m[QStringLiteral("db")] = db;
    m[QStringLiteral("cited")] = cited;
    return m;
}

QJsonArray publishAndReadSources(const ManifestVideoDetail& detail, const QString& id) {
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("out"));
    ManifestWriter::publish(out, makeEntry(id), detail, makeDummyVideo(dir), QImage());
    return readJsonObject(out + QStringLiteral("/videos/") + id + QStringLiteral("/metadata.json"))
        .value(QStringLiteral("rag_sources"))
        .toArray();
}

}  // namespace

TEST_F(ManifestWriterTest, TutorialSourcesAreWrittenAsRagSourcesWithTheGalleryKeys) {
    ManifestVideoDetail detail;  // ragSources は空(Houdini の動画は Cloud RAG に直接聞かない)
    detail.tutorialSources << sourceItem(QStringLiteral("VEX ループ"), QStringLiteral("houdini22"), true)
                           << sourceItem(QStringLiteral("Shelf tools"), QStringLiteral("houdini22"), false);

    const QJsonArray sources = publishAndReadSources(detail, QStringLiteral("s1"));
    ASSERT_EQ(sources.size(), 2);
    const QJsonObject first = sources[0].toObject();
    // ギャラリー(web/public/app.js sourcesHTML)が読むキー名: file / namespace / (similarity) / cited
    EXPECT_EQ(first.value(QStringLiteral("file")).toString(), QStringLiteral("VEX ループ"));
    EXPECT_EQ(first.value(QStringLiteral("namespace")).toString(), QStringLiteral("houdini22"));
    EXPECT_TRUE(first.value(QStringLiteral("cited")).toBool());
    EXPECT_FALSE(first.contains(QStringLiteral("similarity")));  // 無いものを 0 と書かない
    EXPECT_FALSE(sources[1].toObject().value(QStringLiteral("cited")).toBool());
}

TEST_F(ManifestWriterTest, CloudRagSourcesKeepTheirScoreAndAreNotDuplicatedByTutorialSources) {
    ManifestVideoDetail detail;
    detail.ragSources = {{QStringLiteral("VEX ループ"), QStringLiteral("houdini22"), 0.86}};
    detail.tutorialSources << sourceItem(QStringLiteral("VEX ループ"), QStringLiteral("houdini22"), true)
                           << sourceItem(QStringLiteral("別の資料"), QStringLiteral("houdini22"), false);

    const QJsonArray sources = publishAndReadSources(detail, QStringLiteral("s2"));
    ASSERT_EQ(sources.size(), 2);
    EXPECT_DOUBLE_EQ(sources[0].toObject().value(QStringLiteral("similarity")).toDouble(), 0.86);
    EXPECT_EQ(sources[1].toObject().value(QStringLiteral("file")).toString(), QStringLiteral("別の資料"));
}

TEST_F(ManifestWriterTest, NoSourcesAtAllStaysAnEmptyArray) {
    EXPECT_TRUE(publishAndReadSources(ManifestVideoDetail{}, QStringLiteral("s3")).isEmpty());
}

TEST(TutorialSourcesParseTest, ReadsBadgedLinesOfACompletedTutorial) {
    const QString md = QStringLiteral(
        "## 概要\n\nはじめに\n\n"
        "## 参考\n\n"
        "- [1] ✅ 引用済み VEX ループ・条件文と関数定義（houdini21）\n"
        "- [2] ⬜ 未引用 KineFXプロシージャルアニメーション（houdini21）\n"
        "\n利用率: 50%（引用 1/2 件）\n");
    const QVariantList items = parseTutorialSources(md);
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("VEX ループ・条件文と関数定義"));
    EXPECT_EQ(items[0].toMap().value(QStringLiteral("db")).toString(), QStringLiteral("houdini21"));
    EXPECT_TRUE(items[0].toMap().value(QStringLiteral("cited")).toBool());
    EXPECT_FALSE(items[1].toMap().value(QStringLiteral("cited")).toBool());
    EXPECT_EQ(items[1].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("KineFXプロシージャルアニメーション"));
}

TEST(TutorialSourcesParseTest, ReadsUnbadgedLinesOfACutOffTutorialWithoutInventingCitedFlags) {
    // 打ち切りの動画は「引用済み/未引用」を評価していない。cited のキーを付けない(未評価を「未使用」と言わない)
    const QString md = QStringLiteral(
        "## 参考\n\n- [1] Labs Fire Presets（houdini22）\n- [2] Pyro 基礎（houdini22）\n\n## 次\n本文\n");
    const QVariantList items = parseTutorialSources(md);
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Labs Fire Presets"));
    EXPECT_FALSE(items[0].toMap().contains(QStringLiteral("cited")));
}

TEST(TutorialSourcesParseTest, MergesDuplicatesAndKeepsTheCitedOne) {
    const QString md = QStringLiteral(
        "## 参考\n"
        "- [1] ⬜ 未引用 Shelf tools（houdini22shelfindex）\n"
        "- [2] ✅ 引用済み Shelf tools（houdini22shelfindex）\n"
        "- [3] ⬜ 未引用 Shelf tools（houdini22shelfindex）\n");
    const QVariantList items = parseTutorialSources(md);
    ASSERT_EQ(items.size(), 1);
    EXPECT_TRUE(items[0].toMap().value(QStringLiteral("cited")).toBool());
}

TEST(TutorialSourcesParseTest, KeepsTheFirstWordOfATitleWhenTheBadgeHasNoLabel) {
    // 「✅」の後ろに「引用済み」が無い行でも、題名の最初の語を消さない(以前は \S* で読み飛ばしていた)
    const QVariantList items = parseTutorialSources(QStringLiteral("## 参考\n- [1] ✅ Pyro 基礎（houdini22）\n"));
    ASSERT_EQ(items.size(), 1);
    EXPECT_EQ(items[0].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("Pyro 基礎"));
    EXPECT_TRUE(items[0].toMap().value(QStringLiteral("cited")).toBool());
}

TEST(TutorialSourcesParseTest, ReadsCrlfTextAndDoesNotJoinAnEmptyItemWithTheNextLine) {
    const QVariantList crlf = parseTutorialSources(
        QStringLiteral("## 参考\r\n\r\n- [1] ✅ 引用済み A（d）\r\n- [2] ⬜ 未引用 B（d）\r\n"));
    ASSERT_EQ(crlf.size(), 2);
    EXPECT_EQ(crlf[1].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("B"));
    EXPECT_EQ(crlf[1].toMap().value(QStringLiteral("db")).toString(), QStringLiteral("d"));

    // 題名が空の行は、次の行の項目を、題名として読まない
    const QVariantList empty = parseTutorialSources(QStringLiteral("## 参考\n- [1]\n- [2] C（d）\n"));
    for (const QVariant& v : empty) {
        EXPECT_FALSE(v.toMap().value(QStringLiteral("title")).toString().contains(QLatin1Char('\n')));
    }
}

TEST(TutorialSourcesParseTest, HugeOrMalformedInputIsBoundedAndDoesNotCrash) {
    // 件数の上限(200)。重複しない題名を 1000 件並べても、上限で止まる
    QString many = QStringLiteral("## 参考\n");
    for (int i = 0; i < 1000; ++i) {
        many += QStringLiteral("- [%1] 資料%1（d）\n").arg(i);
    }
    EXPECT_EQ(parseTutorialSources(many).size(), 200);

    // 閉じない「（」が延々と続く 1 行、不正なサロゲート、制御文字でも、固まらず・落ちない
    QString weird = QStringLiteral("## 参考\n- [1] ") + QString(50000, QChar(0xFF08)) + QStringLiteral("\n");
    weird += QStringLiteral("- [2] ") + QString(QChar(0xD800)) + QStringLiteral("x\n- [3] \x01\x02（）\n");
    const QVariantList items = parseTutorialSources(weird);
    EXPECT_LE(items.size(), 3);
    EXPECT_TRUE(parseTutorialSources(QString(500000, QChar('#'))).isEmpty());
}

TEST(TutorialSourcesParseTest, ReturnsNothingWhenThereAreNoSources) {
    EXPECT_TRUE(parseTutorialSources(QStringLiteral("## 概要\n本文\n")).isEmpty());
    EXPECT_TRUE(parseTutorialSources(QStringLiteral("## 参考\n\n- （参考ドキュメントなし）\n")).isEmpty());
    EXPECT_TRUE(parseTutorialSources(QString()).isEmpty());
}

TEST(DedupeReferenceItemsTest, DropsEmptyTitlesAndKeepsFirstPosition) {
    QVariantList in;
    in << sourceItem(QStringLiteral("A"), QStringLiteral("d"), false) << sourceItem(QString(), QStringLiteral("d"), true)
       << sourceItem(QStringLiteral("B"), QStringLiteral("d"), false) << sourceItem(QStringLiteral("A"), QStringLiteral("d"), true);
    const QVariantList out = dedupeReferenceItems(in);
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out[0].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("A"));
    EXPECT_TRUE(out[0].toMap().value(QStringLiteral("cited")).toBool());
    EXPECT_EQ(out[1].toMap().value(QStringLiteral("title")).toString(), QStringLiteral("B"));
}
