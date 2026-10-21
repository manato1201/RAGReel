#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

// HTTP+JSON client for the Cloudflare Worker RAG (DevelopmentRAGEnvironment/cloudflare-rag-poc):
// POST /query, POST /me/namespaces. Authentication is "Authorization: Bearer <API key>".
//
// Per docs/architecture/video-factory-design.md §2, this follows the same "don't embed a vector
// store, be an HTTP client of the existing RAG" policy as VectorStoreClient. Credentials (Worker
// URL + API key) are never stored in this repo; see CloudRagClient::fromEnvironment().

struct CloudRagSource {
    QString title;  // the source file name
    QString db;     // the knowledge base name ("shared:" prefix removed)
    double score = 0.0;
};

struct CloudRagResponse {
    QString answer;
    std::vector<CloudRagSource> sources;
    QStringList allowedNamespaces;
    QString memoryId;
    // How much of `answer` the backend could actually ground in cited sources (0-100) and a
    // human-readable "cited/total claims" detail string (e.g. "3/5"), both computed server-side.
    // 0/empty if the backend didn't return them (e.g. under --mock, which never calls this
    // struct's producer).
    double extractionRate = 0.0;
    QString extractionDetail;
};

class CloudRagClient {
public:
    // Reads CLOUD_RAG_URL and CLOUD_RAG_API_KEY from the process environment.
    // Returns nullopt if either is unset.
    static std::optional<CloudRagClient> fromEnvironment();

    CloudRagClient(QString workerUrl, QString apiKey);

    // Blocking POST /query with { "query": ..., "history": [], "namespaces": ["shared:<dbKey>"] }.
    // dbKey is the bare knowledge base name (e.g. "houdini22"); a name that already contains ':'
    // (e.g. "personal:me") is sent as is.
    // Throws std::runtime_error on network failure or an HTTP error status (401/403 = bad URL,
    // key or permission; 429 = token budget / rate limit).
    CloudRagResponse query(const QString& queryText, const QString& dbKey);

    // Returns the knowledge bases this API key is allowed to query ("shared:" prefix removed),
    // via POST /me/namespaces. Costs no tokens. Throws on a bad key or a network failure.
    QStringList listAllowedNamespaces();

private:
    // Shared POST + JSON-parse plumbing. Raises on transport failure, an HTTP error status, or a
    // body that is not a JSON object.
    QJsonObject postRaw(const QString& path, const QJsonObject& body);

    QString workerUrl_;
    QString apiKey_;
};
