#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

// HTTP+JSON client for the DevelopmentRAGEnvironment Cloud RAG GAS WebApp,
// per DevelopmentRAGEnvironment/docs/cloud-rag.md §6.2.6 (same request/
// response contract already used by the Unity and Houdini clients).
//
// Per docs/architecture/video-factory-design.md §2, this follows the same
// "don't embed a vector store, be an HTTP client of the existing RAG" policy
// as VectorStoreClient — Cloud RAG is just a second RAG backend reached the
// same way. Credentials (GAS WebApp URL + API key) are never stored in this
// repo; see CloudRagClient::fromEnvironment().

struct CloudRagSource {
    QString title;
    QString db;
    double score = 0.0;
};

struct CloudRagResponse {
    QString answer;
    std::vector<CloudRagSource> sources;
    QStringList allowedNamespaces;
    QString memoryId;
    // How much of `answer` the backend could actually ground in cited
    // sources (0-100) and a human-readable "cited/total claims" detail
    // string (e.g. "3/5") -- both computed server-side
    // (gas_cloud_rag.js's citation-accuracy pipeline) but previously
    // discarded by this client. 0/empty if the backend didn't return them
    // (e.g. under --mock, which never calls this struct's producer).
    double extractionRate = 0.0;
    QString extractionDetail;
};

class CloudRagClient {
public:
    // Reads CLOUD_RAG_URL and CLOUD_RAG_API_KEY from the process environment.
    // Returns nullopt if either is unset.
    static std::optional<CloudRagClient> fromEnvironment();

    CloudRagClient(QString gasWebAppUrl, QString apiKey);

    // Blocking POST per docs/cloud-rag.md §6.2.6:
    // { "query": ..., "apiKey": ..., "dbKey": ..., "history": [] }
    // Throws std::runtime_error on network failure or a non-"ok" status
    // (auth_error / forbidden per the documented error contract).
    CloudRagResponse query(const QString& queryText, const QString& dbKey);

private:
    QString gasWebAppUrl_;
    QString apiKey_;
};
