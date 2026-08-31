#include "tutorial_queue.h"
#include "process_runner.h"

#include <QFileInfo>
#include <QVariantMap>

TutorialQueue::TutorialQueue(ProcessRunner* runner, QObject* parent)
    : QObject(parent), runner_(runner) {
    connect(runner_, &ProcessRunner::finished, this, [this](int exitCode, const QString&) {
        if (!waitingForResult_) {
            // Some other job (Cloud RAG tab, or a single Houdini-tab run)
            // finished -- not ours, ignore it.
            return;
        }
        waitingForResult_ = false;

        if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(items_.size())) {
            items_[static_cast<size_t>(currentIndex_)].status =
                exitCode == 0 ? QStringLiteral("done") : QStringLiteral("error");
            emit itemsChanged();
        }

        if (active_) {
            advance();
        }
    });
}

QVariantList TutorialQueue::items() const {
    QVariantList result;
    for (const Item& item : items_) {
        QVariantMap m;
        m[QStringLiteral("path")] = item.path;
        m[QStringLiteral("name")] = QFileInfo(item.path).fileName();
        m[QStringLiteral("status")] = item.status;
        result.append(m);
    }
    return result;
}

void TutorialQueue::addFiles(const QStringList& paths) {
    bool changed = false;
    for (const QString& path : paths) {
        bool alreadyQueued = false;
        for (const Item& item : items_) {
            if (item.path == path) {
                alreadyQueued = true;
                break;
            }
        }
        if (!alreadyQueued) {
            items_.push_back({path, QStringLiteral("pending")});
            changed = true;
        }
    }
    if (changed) {
        emit itemsChanged();
    }
}

void TutorialQueue::removeAt(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    if (index == currentIndex_ && items_[static_cast<size_t>(index)].status == QStringLiteral("running")) {
        return; // let the running job finish; use the log panel's cancel for that
    }
    items_.erase(items_.begin() + index);
    if (currentIndex_ > index) {
        --currentIndex_;
    }
    emit itemsChanged();
}

void TutorialQueue::clearFinished() {
    std::vector<Item> kept;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const Item& item = items_[static_cast<size_t>(i)];
        if (item.status == QStringLiteral("done") || item.status == QStringLiteral("error")) {
            if (i == currentIndex_) currentIndex_ = -1;
            continue;
        }
        kept.push_back(item);
    }
    items_ = std::move(kept);
    emit itemsChanged();
}

int TutorialQueue::firstPendingIndexFrom(int start) const {
    for (int i = start; i < static_cast<int>(items_.size()); ++i) {
        if (items_[static_cast<size_t>(i)].status == QStringLiteral("pending")) {
            return i;
        }
    }
    return -1;
}

void TutorialQueue::start() {
    if (active_ || runner_->isRunning()) return;

    const int next = firstPendingIndexFrom(0);
    if (next < 0) return;

    active_ = true;
    emit activeChanged();
    currentIndex_ = next;
    runCurrent();
}

void TutorialQueue::stop() {
    if (!active_) return;
    active_ = false;
    emit activeChanged();
}

void TutorialQueue::runCurrent() {
    items_[static_cast<size_t>(currentIndex_)].status = QStringLiteral("running");
    emit itemsChanged();
    waitingForResult_ = true;
    runner_->runHoudiniTutorial(items_[static_cast<size_t>(currentIndex_)].path);
}

void TutorialQueue::advance() {
    const int next = firstPendingIndexFrom(0);
    if (next < 0) {
        active_ = false;
        currentIndex_ = -1;
        emit activeChanged();
        return;
    }
    currentIndex_ = next;
    runCurrent();
}
