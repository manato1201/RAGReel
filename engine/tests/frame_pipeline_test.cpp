#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "encode/frame_pipeline.h"

namespace {

using namespace std::chrono_literals;

struct Item {
    int id = 0;
};

}  // namespace

TEST(FramePipelineTest, DeliversEveryFrameInOrderOnOneOtherThread) {
    std::vector<int> seen;
    std::thread::id sinkThread;
    {
        FramePipeline<Item> pipeline(4, [&](const Item& item) {
            sinkThread = std::this_thread::get_id();
            seen.push_back(item.id);
        });
        for (int i = 0; i < 1000; ++i) {
            pipeline.push(Item{i});
        }
        pipeline.finish();
    }
    ASSERT_EQ(seen.size(), 1000u);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(seen[static_cast<size_t>(i)], i);
    }
    EXPECT_NE(sinkThread, std::this_thread::get_id());
}

TEST(FramePipelineTest, FullQueueMakesTheProducerWait) {
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<int> processed{0};
    std::atomic<bool> started{false};
    FramePipeline<Item> pipeline(2, [&](const Item&) {
        started = true;
        released.wait();  // 解放されるまで 1 枚目で止まる
        ++processed;
    });

    // 1 枚はエンコード中、2 枚はキュー: 3 枚までは待たずに積める
    pipeline.push(Item{0});
    while (!started) std::this_thread::sleep_for(1ms);
    pipeline.push(Item{1});
    pipeline.push(Item{2});
    EXPECT_EQ(pipeline.queued(), 2u);

    // 4 枚目は、キューが満杯なので積めずに待つ
    std::atomic<bool> pushed{false};
    std::thread producer([&] {
        pipeline.push(Item{3});
        pushed = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(pushed.load());
    EXPECT_LE(pipeline.queued(), 2u);  // 待っている間、キューは増えない

    release.set_value();
    producer.join();
    pipeline.finish();
    EXPECT_TRUE(pushed.load());
    EXPECT_EQ(processed.load(), 4);
}

TEST(FramePipelineTest, SinkExceptionIsRethrownToTheProducerWithoutTerminating) {
    FramePipeline<Item> pipeline(2, [&](const Item& item) {
        if (item.id == 3) throw std::runtime_error("encode failed");
    });
    bool thrown = false;
    std::string message;
    try {
        for (int i = 0; i < 100000; ++i) {
            pipeline.push(Item{i});
        }
        pipeline.finish();
    } catch (const std::runtime_error& e) {
        thrown = true;
        message = e.what();
    }
    EXPECT_TRUE(thrown);
    EXPECT_EQ(message, "encode failed");
    EXPECT_TRUE(pipeline.failed());
    // 失敗した後は、何度 push しても、同じ例外が投げられる(待ち続けない)
    EXPECT_THROW(pipeline.push(Item{0}), std::runtime_error);
}

TEST(FramePipelineTest, ErrorOnTheLastFrameSurfacesFromFinish) {
    FramePipeline<Item> pipeline(4, [&](const Item& item) {
        if (item.id == 9) throw std::runtime_error("last frame failed");
    });
    for (int i = 0; i < 10; ++i) {
        pipeline.push(Item{i});
    }
    EXPECT_THROW(pipeline.finish(), std::runtime_error);
}

TEST(FramePipelineTest, DestructorJoinsTheThreadWhenTheProducerBailsOut) {
    std::atomic<int> processed{0};
    std::atomic<bool> sinkAlive{true};
    {
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        auto pipeline = std::make_unique<FramePipeline<Item>>(2, [&](const Item&) {
            released.wait_for(50ms);
            ++processed;
        });
        for (int i = 0; i < 3; ++i) pipeline->push(Item{i});
        // finish を呼ばずに(描画側の例外のつもりで)壊す。残りは捨てて、スレッドは join される
        pipeline.reset();
        sinkAlive = false;
    }
    const int after = processed.load();
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(processed.load(), after);  // 壊した後に、スレッドがフレームを処理し続けていない
}

TEST(FramePipelineTest, DestructorNeverThrowsEvenAfterAnError) {
    EXPECT_NO_THROW({
        FramePipeline<Item> pipeline(2, [&](const Item&) { throw std::runtime_error("boom"); });
        try {
            for (int i = 0; i < 100; ++i) pipeline.push(Item{i});
        } catch (const std::runtime_error&) {
        }
    });
}

TEST(FramePipelineTest, AbortIsIdempotentAndLaterPushesAreDropped) {
    std::atomic<int> processed{0};
    FramePipeline<Item> pipeline(4, [&](const Item&) { ++processed; });
    pipeline.push(Item{0});
    pipeline.abort();
    pipeline.abort();
    EXPECT_NO_THROW(pipeline.push(Item{1}));
    EXPECT_NO_THROW(pipeline.finish());
    EXPECT_LE(processed.load(), 1);
}

TEST(FramePipelineTest, SharedFramesAreNotCopiedWhenQueued) {
    // 同じ画面を何度積んでも、実体は 1 つ(shared_ptr の参照が増えるだけ)
    auto image = std::make_shared<std::vector<char>>(8 * 1024 * 1024);
    struct SharedItem {
        std::shared_ptr<std::vector<char>> data;
        bool repeat = false;
    };
    std::atomic<int> processed{0};
    {
        FramePipeline<SharedItem> pipeline(4, [&](const SharedItem& item) {
            EXPECT_EQ(item.data.get(), image.get());
            ++processed;
        });
        for (int i = 0; i < 50; ++i) pipeline.push(SharedItem{image, i > 0});
        pipeline.finish();
    }
    EXPECT_EQ(processed.load(), 50);
    EXPECT_EQ(image.use_count(), 1);  // キューを抜けた後に参照が残っていない
}
