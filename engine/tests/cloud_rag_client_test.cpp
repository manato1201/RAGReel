// CloudRagClient against a stand-in for the Cloudflare Worker (POST /query, POST /me/namespaces).
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "ragclient/cloud_rag_client.h"

namespace {

void ensureApp() {
    static int argc = 1;
    static char arg0[] = "cloud_rag_client_tests";
    static char* argv[] = {arg0, nullptr};
    static QCoreApplication* app = QCoreApplication::instance() ? nullptr : new QCoreApplication(argc, argv);
    (void)app;
}

class FakeWorker : public QObject {
public:
    FakeWorker() {
        ensureApp();
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
            }
        });
        EXPECT_TRUE(server_.listen(QHostAddress::LocalHost, 0));
    }

    QString baseUrl() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }

    int status = 200;
    QByteArray reply = "{}";
    std::vector<QByteArray> requestHeaders;
    std::vector<QJsonObject> requestBodies;

private:
    void onReadyRead(QTcpSocket* socket) {
        QByteArray& buffer = buffers_[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray headers = buffer.left(headerEnd);
        int contentLength = 0;
        for (const QByteArray& line : headers.split('\n')) {
            if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
        }
        if (buffer.size() < headerEnd + 4 + contentLength) return;
        requestHeaders.push_back(headers);
        requestBodies.push_back(QJsonDocument::fromJson(buffer.mid(headerEnd + 4, contentLength)).object());
        buffers_.erase(socket);

        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + QByteArray::number(reply.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(response + reply);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    std::map<QTcpSocket*, QByteArray> buffers_;
};

}  // namespace

TEST(CloudRagClient, QuerySendsBearerAndSharedNamespaceAndReadsTheWorkerShapedReply) {
    FakeWorker worker;
    worker.reply = R"({"answer":"A","status":"ok","memoryId":42,"extractionRate":60,"extractionDetail":"3/5",
        "namespaces":["shared:houdini22"],
        "sources":[{"file":"sop/box.md","namespace":"shared:houdini22","score":0.8}]})";
    CloudRagClient client(worker.baseUrl() + "/", QStringLiteral("secret-key"));

    const CloudRagResponse r = client.query(QStringLiteral("how"), QStringLiteral("houdini22"));

    ASSERT_EQ(worker.requestBodies.size(), 1u);
    EXPECT_TRUE(worker.requestHeaders[0].startsWith("POST /query "));
    EXPECT_TRUE(worker.requestHeaders[0].contains("Bearer secret-key"));
    EXPECT_TRUE(worker.requestHeaders[0].contains("RAGReelEngine"));
    EXPECT_EQ(worker.requestBodies[0].value("query").toString(), QStringLiteral("how"));
    EXPECT_EQ(worker.requestBodies[0].value("namespaces").toArray().at(0).toString(), QStringLiteral("shared:houdini22"));
    EXPECT_FALSE(worker.requestBodies[0].contains("apiKey")) << "the key goes in the header only";

    EXPECT_EQ(r.answer, QStringLiteral("A"));
    EXPECT_EQ(r.memoryId, QStringLiteral("42"));
    EXPECT_DOUBLE_EQ(r.extractionRate, 60.0);
    EXPECT_EQ(r.extractionDetail, QStringLiteral("3/5"));
    ASSERT_EQ(r.sources.size(), 1u);
    EXPECT_EQ(r.sources[0].title, QStringLiteral("sop/box.md"));
    EXPECT_EQ(r.sources[0].db, QStringLiteral("houdini22")) << "the shared: prefix is removed";
    EXPECT_EQ(r.allowedNamespaces, QStringList{QStringLiteral("houdini22")});
}

TEST(CloudRagClient, ANameWithAColonIsSentAsIs) {
    FakeWorker worker;
    worker.reply = R"({"answer":"","status":"ok"})";
    CloudRagClient client(worker.baseUrl(), QStringLiteral("k"));
    client.query(QStringLiteral("q"), QStringLiteral("personal:me"));
    EXPECT_EQ(worker.requestBodies[0].value("namespaces").toArray().at(0).toString(), QStringLiteral("personal:me"));
}

TEST(CloudRagClient, HttpErrorStatusesBecomeDistinctMessages) {
    FakeWorker worker;
    CloudRagClient client(worker.baseUrl(), QStringLiteral("k"));

    worker.status = 429;
    worker.reply = R"({"error":"budget used up"})";
    try {
        client.query(QStringLiteral("q"), QStringLiteral("houdini22"));
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("429"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("budget used up"), std::string::npos);
    }

    worker.status = 401;
    worker.reply = R"({"error":"invalid key"})";
    try {
        client.query(QStringLiteral("q"), QStringLiteral("houdini22"));
        FAIL() << "expected an exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("401"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("invalid key"), std::string::npos);
    }
}

TEST(CloudRagClient, ListsAllowedNamespacesWithoutTheSharedPrefix) {
    FakeWorker worker;
    worker.reply = R"({"namespaces":["shared:houdini21","shared:houdini22","personal:me"],"role":"member","status":"ok"})";
    CloudRagClient client(worker.baseUrl(), QStringLiteral("k"));

    const QStringList names = client.listAllowedNamespaces();

    EXPECT_TRUE(worker.requestHeaders[0].startsWith("POST /me/namespaces "));
    EXPECT_EQ(names, (QStringList{QStringLiteral("houdini21"), QStringLiteral("houdini22"), QStringLiteral("personal:me")}));
}
