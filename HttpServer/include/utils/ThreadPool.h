#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <type_traits>
#include <stdexcept>

namespace http {
namespace utils {

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads);
    ~ThreadPool();

    // 提交任务，返回 future 用于获取结果
    template<class F>
    auto enqueue(F&& f) -> std::future<typename std::invoke_result_t<F>>;

    size_t threadCount() const { return workers_.size(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};

// 模板实现必须在头文件中
template<class F>
auto ThreadPool::enqueue(F&& f) -> std::future<typename std::invoke_result_t<F>> {
    using return_type = typename std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::forward<F>(f)
    );
    std::future<return_type> res = task->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) throw std::runtime_error("ThreadPool is stopped");
        tasks_.emplace([task]() { (*task)(); });
    }
    cv_.notify_one();
    return res;
}

} // namespace utils
} // namespace http