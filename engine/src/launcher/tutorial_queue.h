#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <vector>

class ProcessRunner;

// Queues several Houdini-tutorial .md files and runs them through
// ProcessRunner one at a time, auto-advancing to the next pending item when
// the current one finishes -- so a batch of tutorials doesn't need an
// external script (see IMPROVEMENT_PLAN.md-adjacent code review: this is
// also what made the runId second-granularity collision in
// main_cloudrag.cpp reachable in practice).
//
// Deliberately does not touch ProcessRunner's cancel()/GPU-lease/job-object
// machinery: ProcessRunner still only ever runs one job at a time (its own
// isRunning() guard is unchanged), this class just decides *which* job to
// start next. stop() only stops the queue from advancing after the current
// job -- it does not kill the in-flight job (use the existing "[キャンセル]"
// log-panel control for that, same as any other run).
class TutorialQueue : public QObject {
    Q_OBJECT
    // Each entry: {path, name (basename), status: "pending"/"running"/"done"/"error"}.
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)

public:
    explicit TutorialQueue(ProcessRunner* runner, QObject* parent = nullptr);

    QVariantList items() const;
    bool active() const { return active_; }

    // Ignores paths already in the queue (by exact path match).
    Q_INVOKABLE void addFiles(const QStringList& paths);
    // No-op if index is out of range or is the currently-running item.
    Q_INVOKABLE void removeAt(int index);
    // Drops every "done"/"error" entry, keeping pending/running ones.
    Q_INVOKABLE void clearFinished();

    // Starts processing from the first pending item. No-op if already
    // active or if ProcessRunner is busy with an unrelated job.
    Q_INVOKABLE void start();
    // Stops auto-advancing after the current job finishes; does not cancel
    // the in-flight job itself.
    Q_INVOKABLE void stop();

signals:
    void itemsChanged();
    void activeChanged();

private:
    struct Item {
        QString path;
        QString status = QStringLiteral("pending");
    };

    void runCurrent();
    void advance();
    int firstPendingIndexFrom(int start) const;

    ProcessRunner* runner_;
    std::vector<Item> items_;
    int currentIndex_ = -1;
    bool active_ = false;
    // True only while ProcessRunner is running a job THIS queue started --
    // ProcessRunner::finished() fires for every job (Cloud RAG tab/single
    // Houdini tab runs included), so this distinguishes "my job just ended"
    // from "some unrelated job just ended" without adding a job-tag to
    // ProcessRunner itself.
    bool waitingForResult_ = false;
};
