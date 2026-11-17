#pragma once

// 描画(QML / RHI。1 つのスレッドに固定)とエンコード(sws_scale + VP9)を、別スレッドで流すための
// 有界のキュー。Qt にも FFmpeg にも依存しない(テストしやすいよう、フレームの型はテンプレートにした)。
//
//   描画スレッド: push(frame) -> [最大 capacity 個のキュー] -> エンコードのスレッド: sink(frame)
//
// 約束ごと:
//   - キューが満杯なら push は待つ(メモリが増えない)。
//   - sink(エンコード)が例外を投げたら、そのスレッドは止まり、次の push / finish が同じ例外を描画スレッドに投げ直す
//     (スレッドの中で例外が外へ出ると std::terminate になるので、ここで必ず受け止める)。
//   - finish は、積んだフレームをすべてエンコードし終えてからスレッドを join する(エラーがあれば投げ直す)。
//   - 途中でやめる(描画側の例外・キャンセル)ときは、デストラクタか abort が、残りを捨ててスレッドを join する。
//     スレッドが残ることはなく、デストラクタは例外を投げない。
//   - sink はエンコード用のスレッド 1 本だけが順番に呼ぶ。フレームの順序は push した順のまま。

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

template <class Frame>
class FramePipeline {
public:
    using Sink = std::function<void(const Frame&)>;

    FramePipeline(std::size_t capacity, Sink sink)
        : capacity_(capacity == 0 ? 1 : capacity), sink_(std::move(sink)) {
        worker_ = std::thread([this] { run(); });
    }

    ~FramePipeline() { abort(); }

    FramePipeline(const FramePipeline&) = delete;
    FramePipeline& operator=(const FramePipeline&) = delete;

    // フレームを積む。満杯なら空くまで待つ。エンコード側が失敗していたら、その例外を投げる。
    void push(Frame frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        notFull_.wait(lock, [this] { return queue_.size() < capacity_ || error_ || aborted_; });
        if (error_) {
            std::rethrow_exception(error_);
        }
        if (aborted_ || closing_) {
            return;  // 終わった後・中止した後に積まれたフレームは捨てる
        }
        queue_.push_back(std::move(frame));
        notEmpty_.notify_one();
    }

    // 積んだフレームをすべてエンコードし終えて、スレッドを止める。エンコード側の例外があれば投げる。
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closing_ = true;
        }
        notEmpty_.notify_all();
        join();
        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error = error_;
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    // 残りを捨てて、スレッドを止める(例外は投げない)。何度呼んでもよい。
    void abort() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            aborted_ = true;
            queue_.clear();
        }
        notEmpty_.notify_all();
        notFull_.notify_all();
        join();
    }

    // エンコード側が失敗しているか(push を待たずに知りたいとき)
    bool failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<bool>(error_);
    }

    // 今キューにあるフレームの数(テスト・ログ用)
    std::size_t queued() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    void join() noexcept {
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
    }

    void run() {
        while (true) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                notEmpty_.wait(lock, [this] { return !queue_.empty() || closing_ || aborted_; });
                if (aborted_ || queue_.empty()) {
                    return;  // 中止、または終わりの合図で空になった
                }
                frame = std::move(queue_.front());
                queue_.pop_front();
            }
            notFull_.notify_one();
            try {
                sink_(frame);
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    error_ = std::current_exception();
                    queue_.clear();
                }
                notFull_.notify_all();
                return;
            }
        }
    }

    const std::size_t capacity_;
    Sink sink_;
    mutable std::mutex mutex_;
    std::condition_variable notEmpty_;
    std::condition_variable notFull_;
    std::deque<Frame> queue_;
    bool closing_ = false;
    bool aborted_ = false;
    std::exception_ptr error_;
    std::thread worker_;  // 最後に初期化する(他のメンバーが揃ってから動き出す)
};
