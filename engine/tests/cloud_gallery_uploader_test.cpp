// CloudGalleryUploader の「分けて送る」経路(90MB を超える動画)のテスト(2026-11-18)。
// 1080p30 の長い動画は、1 回の POST では Cloudflare の本文の上限(100MB)を超え、「Payload Too Large」になった。
// 別のスレッドで、小さな偽のギャラリー(QTcpServer)を動かし、開始 → パート → 完了、失敗したときの取り消しを確かめる。
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <memory>

#include "manifest/cloud_gallery_uploader.h"

namespace {

struct Seen {
    QString method;
    QString path;
    qint64 bodySize = 0;
    QByteArray body;  // 先頭の 64KB まで(JSON の確認用)
};

class FakeGallery : public QObject {
public:
    // 設定(テストから、サーバーを動かす前に決める)
    int partSizeToReturn = 40 * 1024 * 1024;
    int failPartNumber = 0;       // その番号のパートを、必ず 500 にする(0 = 失敗させない)
    int failPartStatus = 500;
    int completeStatus = 200;
    QString expectedToken = QStringLiteral("TOKEN");

    QMutex mutex;
    QList<Seen> seen;

    quint16 start() {
        server_ = new QTcpServer(this);
        connect(server_, &QTcpServer::newConnection, this, &FakeGallery::onConnection);
        server_->listen(QHostAddress::LocalHost, 0);
        return server_->serverPort();
    }

    QList<Seen> snapshot() {
        QMutexLocker lock(&mutex);
        return seen;
    }

private:
    struct Conn {
        QByteArray buffer;
    };

    void onConnection() {
        while (QTcpSocket* socket = server_->nextPendingConnection()) {
            auto* conn = new Conn;
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, conn]() { onData(socket, conn); });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QObject::destroyed, this, [conn]() { delete conn; });
        }
    }

    void onData(QTcpSocket* socket, Conn* conn) {
        conn->buffer += socket->readAll();
        const int headerEnd = conn->buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QList<QByteArray> lines = conn->buffer.left(headerEnd).split('\n');
        const QList<QByteArray> request = lines[0].trimmed().split(' ');
        if (request.size() < 2) return;
        qint64 contentLength = 0;
        QByteArray auth;
        for (const QByteArray& line : lines) {
            const QByteArray lower = line.toLower();
            if (lower.startsWith("content-length:")) contentLength = line.mid(15).trimmed().toLongLong();
            if (lower.startsWith("authorization:")) auth = line.mid(14).trimmed();
        }
        const qint64 have = conn->buffer.size() - (headerEnd + 4);
        if (have < contentLength) return;  // 本文が、まだ届ききっていない

        Seen s;
        s.method = QString::fromLatin1(request[0]);
        s.path = QString::fromLatin1(request[1]);
        s.bodySize = contentLength;
        s.body = conn->buffer.mid(headerEnd + 4, std::min<qint64>(contentLength, 65536));
        conn->buffer.clear();
        {
            QMutexLocker lock(&mutex);
            seen.append(s);
        }
        int status = 200;
        QJsonObject out{{"ok", true}};
        if (auth != "Bearer " + expectedToken.toUtf8()) {
            status = 401;
            out = QJsonObject{{"error", "unauthorized"}};
        } else if (s.method == "POST" && s.path == "/api/videos/uploads") {
            out = QJsonObject{{"ok", true}, {"uploadId", "up1"}, {"partSize", partSizeToReturn}};
        } else if (s.method == "PUT" && s.path.startsWith("/api/videos/uploads/up1/parts/")) {
            const int number = s.path.mid(QStringLiteral("/api/videos/uploads/up1/parts/").size()).split('?')[0].toInt();
            if (number == failPartNumber) {
                status = failPartStatus;
                out = QJsonObject{{"error", "boom"}};
            } else {
                out = QJsonObject{{"ok", true}, {"partNumber", number}, {"etag", QStringLiteral("e%1").arg(number)}};
            }
        } else if (s.method == "POST" && s.path == "/api/videos/uploads/up1/complete") {
            status = completeStatus;
            if (status != 200) out = QJsonObject{{"error", "complete failed"}};
        } else if (s.method == "DELETE" && s.path.startsWith("/api/videos/uploads/up1")) {
            // 取り消し
        } else if (s.method == "POST" && s.path == "/api/videos") {
            // 1 回の POST(小さい動画)
        } else {
            status = 404;
            out = QJsonObject{{"error", "not found"}};
        }
        const QByteArray payload = QJsonDocument(out).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
                      QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
        socket->disconnectFromHost();
    }

    QTcpServer* server_ = nullptr;
};

// 偽のギャラリーを、専用のスレッド(イベントループつき)で動かす
class GalleryThread {
public:
    GalleryThread() {
        gallery = new FakeGallery;
        gallery->moveToThread(&thread);
        thread.start();
    }
    ~GalleryThread() {
        QMetaObject::invokeMethod(gallery, [this]() { delete gallery; gallery = nullptr; }, Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    }
    quint16 listen() {
        quint16 port = 0;
        QMetaObject::invokeMethod(gallery, [this, &port]() { port = gallery->start(); }, Qt::BlockingQueuedConnection);
        return port;
    }
    QThread thread;
    FakeGallery* gallery = nullptr;
};

QCoreApplication& app() {
    static int argc = 1;
    static char name[] = "cloud_gallery_uploader_tests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication a(argc, argv);
    return a;
}

class UploaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        app();
        ASSERT_TRUE(dir_.isValid());
        thumb_ = dir_.filePath("thumb.png");
        QFile t(thumb_);
        ASSERT_TRUE(t.open(QIODevice::WriteOnly));
        t.write("PNGDATA");
        t.close();
    }

    QString makeVideo(qint64 bytes) {
        const QString path = dir_.filePath("video.webm");
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        EXPECT_TRUE(f.resize(bytes));  // 中身は 0(確かめるのは、パートの数と大きさ)
        f.close();
        return path;
    }

    static qint64 countOf(const QList<Seen>& seen, const QString& method, const QString& prefix) {
        qint64 n = 0;
        for (const Seen& s : seen) {
            if (s.method == method && s.path.startsWith(prefix)) ++n;
        }
        return n;
    }

    CloudGalleryUploader uploader(quint16 port, const QString& token = QStringLiteral("TOKEN")) {
        qputenv("GALLERY_UPLOAD_URL", QByteArray("http://127.0.0.1:") + QByteArray::number(port));
        qputenv("GALLERY_UPLOAD_TOKEN", token.toUtf8());
        auto u = CloudGalleryUploader::fromEnvironment();
        EXPECT_TRUE(u.has_value());
        return *u;
    }

    QTemporaryDir dir_;
    QString thumb_;
    const QByteArray entry_ = R"({"id":"cloudrag_1","title":"長い動画"})";
    const QByteArray metadata_ = R"({"id":"cloudrag_1","rag_sources":[]})";
};

TEST_F(UploaderTest, AVideoOverTheSingleLimitIsSentInPartsAndCompleted) {
    GalleryThread g;
    const quint16 port = g.listen();
    const qint64 size = 91LL * 1024 * 1024;  // 90MB を超える
    const QString video = makeVideo(size);
    auto u = uploader(port);
    ASSERT_NO_THROW(u.upload("cloudrag_1", entry_, metadata_, video, thumb_));

    const QList<Seen> seen = g.gallery->snapshot();
    EXPECT_EQ(countOf(seen, "POST", "/api/videos/uploads"), 2);  // 開始 + 完了
    EXPECT_EQ(countOf(seen, "PUT", "/api/videos/uploads/up1/parts/"), 3);  // 40MB + 40MB + 11MB
    EXPECT_EQ(countOf(seen, "DELETE", "/api/videos/uploads"), 0);
    EXPECT_EQ(countOf(seen, "POST", "/api/videos"), 2);  // 1 回の POST の経路は使っていない(/api/videos/uploads だけ)
    qint64 sum = 0;
    for (const Seen& s : seen) {
        if (s.method == "PUT") sum += s.bodySize;
    }
    EXPECT_EQ(sum, size);

    const Seen& complete = seen.last();
    ASSERT_EQ(complete.path, "/api/videos/uploads/up1/complete");
    const QJsonObject body = QJsonDocument::fromJson(complete.body).object();
    EXPECT_EQ(body["id"].toString(), "cloudrag_1");
    EXPECT_EQ(body["parts"].toArray().size(), 3);
    EXPECT_EQ(body["parts"].toArray()[2].toObject()["partNumber"].toInt(), 3);
    EXPECT_EQ(body["parts"].toArray()[2].toObject()["etag"].toString(), "e3");
    EXPECT_EQ(QByteArray::fromBase64(body["thumbnail_base64"].toString().toLatin1()), "PNGDATA");
    EXPECT_EQ(body["entry"].toObject()["title"].toString(), "長い動画");
    EXPECT_TRUE(body["metadata"].toObject().contains("rag_sources"));
}

TEST_F(UploaderTest, AFailingPartIsRetriedThenAbortsTheUploadAndThrows) {
    GalleryThread g;
    const quint16 port = g.listen();
    QMetaObject::invokeMethod(g.gallery, [&]() { g.gallery->failPartNumber = 2; }, Qt::BlockingQueuedConnection);
    const QString video = makeVideo(91LL * 1024 * 1024);
    auto u = uploader(port);
    try {
        u.upload("cloudrag_1", entry_, metadata_, video, thumb_);
        FAIL() << "should have thrown";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("part 2"), std::string::npos) << e.what();
    }
    const QList<Seen> seen = g.gallery->snapshot();
    EXPECT_EQ(countOf(seen, "PUT", "/api/videos/uploads/up1/parts/2"), 3);  // 5xx は 3 回まで
    EXPECT_EQ(countOf(seen, "DELETE", "/api/videos/uploads/up1"), 1);       // 途中の分を捨てる
    EXPECT_EQ(countOf(seen, "POST", "/api/videos/uploads/up1/complete"), 0);
}

TEST_F(UploaderTest, AClientErrorOnAPartIsNotRetried) {
    GalleryThread g;
    const quint16 port = g.listen();
    QMetaObject::invokeMethod(g.gallery, [&]() { g.gallery->failPartNumber = 1; g.gallery->failPartStatus = 413; },
                              Qt::BlockingQueuedConnection);
    const QString video = makeVideo(91LL * 1024 * 1024);
    auto u = uploader(port);
    EXPECT_THROW(u.upload("cloudrag_1", entry_, metadata_, video, thumb_), std::runtime_error);
    const QList<Seen> seen = g.gallery->snapshot();
    EXPECT_EQ(countOf(seen, "PUT", "/api/videos/uploads/up1/parts/1"), 1);
    EXPECT_EQ(countOf(seen, "DELETE", "/api/videos/uploads/up1"), 1);
}

TEST_F(UploaderTest, AFailingCompleteAbortsAndThrows) {
    GalleryThread g;
    const quint16 port = g.listen();
    QMetaObject::invokeMethod(g.gallery, [&]() { g.gallery->completeStatus = 500; }, Qt::BlockingQueuedConnection);
    const QString video = makeVideo(91LL * 1024 * 1024);
    auto u = uploader(port);
    EXPECT_THROW(u.upload("cloudrag_1", entry_, metadata_, video, thumb_), std::runtime_error);
    EXPECT_EQ(countOf(g.gallery->snapshot(), "DELETE", "/api/videos/uploads/up1"), 1);
}

TEST_F(UploaderTest, AWrongTokenFailsAtTheStartWithNoPartsSent) {
    GalleryThread g;
    const quint16 port = g.listen();
    const QString video = makeVideo(91LL * 1024 * 1024);
    auto u = uploader(port, QStringLiteral("WRONG"));
    try {
        u.upload("cloudrag_1", entry_, metadata_, video, thumb_);
        FAIL() << "should have thrown";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("401"), std::string::npos) << e.what();
    }
    EXPECT_EQ(countOf(g.gallery->snapshot(), "PUT", "/api/videos/uploads"), 0);
}

TEST_F(UploaderTest, ASmallVideoStillUsesTheSinglePost) {
    GalleryThread g;
    const quint16 port = g.listen();
    const QString video = makeVideo(1024);
    auto u = uploader(port);
    ASSERT_NO_THROW(u.upload("cloudrag_1", entry_, metadata_, video, thumb_));
    const QList<Seen> seen = g.gallery->snapshot();
    ASSERT_EQ(seen.size(), 1);
    EXPECT_EQ(seen[0].method, "POST");
    EXPECT_EQ(seen[0].path, "/api/videos");
}

}  // namespace
