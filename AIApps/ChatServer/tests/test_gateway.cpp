// 网关组件单元测试：RateLimiter + CircuitBreaker
// 编译: g++ -std=c++17 -pthread -I../include -I../../HttpServer/include \
//        -o test_gateway test_gateway.cpp \
//        ../src/LLMGateway/RateLimiter.cpp ../src/LLMGateway/CircuitBreaker.cpp
// 运行: ./test_gateway

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <chrono>
#include "../include/LLMGateway/RateLimiter.h"
#include "../include/LLMGateway/CircuitBreaker.h"

static int passed = 0, failed = 0;

#define TEST(name) std::cout << "  " << (name) << "... "
#define OK()   do { std::cout << "OK" << std::endl; passed++; } while(0)
#define FAIL(msg) do { std::cout << "FAIL: " << (msg) << std::endl; failed++; } while(0)

// ============================================================================
// RateLimiter 测试
// ============================================================================

void test_token_bucket_basic() {
    TEST("TokenBucket basic consume");
    TokenBucket tb(10.0, 5.0);  // 10 tokens/s, burst 5
    // 初始有 burst_size 个 token
    for (int i = 0; i < 5; i++) {
        if (!tb.tryConsume()) { FAIL("should have initial tokens"); return; }
    }
    // 第 6 次应该失败（桶已空）
    if (tb.tryConsume()) { FAIL("should be empty after consuming burst"); return; }
    OK();
}

void test_token_bucket_refill() {
    TEST("TokenBucket refill after delay");
    TokenBucket tb(100.0, 1.0);  // 100 tokens/s, burst 1
    if (!tb.tryConsume()) { FAIL("initial token missing"); return; }
    if (tb.tryConsume()) { FAIL("should be empty"); return; }

    // 等待 ~15ms，应该补充了约 1.5 个 token
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    if (!tb.tryConsume()) { FAIL("should have refilled at least 1 token"); return; }
    // 剩余 ~0.5 token 不够消费
    if (tb.tryConsume()) { FAIL("should not have 2 full tokens yet"); return; }
    OK();
}

void test_rate_limiter_user_limit() {
    TEST("RateLimiter per-user enforcement");
    RateLimiter rl;
    rl.configureUserLimit(10.0, 1.0);  // 10 tokens/s, burst 1

    // 第一次可通过
    try {
        rl.checkUserAllowed(1001);
    } catch (const RateLimitException&) {
        FAIL("first request should pass"); return;
    }

    // 第二次应被限流（burst=1）
    try {
        rl.checkUserAllowed(1001);
        FAIL("second request should be rate limited");
    } catch (const RateLimitException&) {
        // 预期行为
    }
    OK();
}

void test_rate_limiter_backend_limit() {
    TEST("RateLimiter per-backend enforcement");
    RateLimiter rl;
    rl.configureBackend("backend-A", 10.0, 2.0);

    // 前两次可通过（burst=2）
    try {
        rl.checkBackendAllowed("backend-A");
        rl.checkBackendAllowed("backend-A");
    } catch (...) {
        FAIL("first 2 requests should pass"); return;
    }

    // 第三次应被限流
    try { rl.checkBackendAllowed("backend-A"); FAIL("3rd should be rate limited"); }
    catch (const RateLimitException&) {}
    OK();
}

void test_rate_limiter_unknown_backend() {
    TEST("RateLimiter unknown backend passes through");
    RateLimiter rl;
    // 未配置的后端应不限流
    try { rl.checkBackendAllowed("unknown"); }
    catch (...) { FAIL("unknown backend should pass through"); return; }
    OK();
}

void test_rate_limiter_different_users() {
    TEST("RateLimiter independent user buckets");
    RateLimiter rl;
    rl.configureUserLimit(10.0, 1.0);

    rl.checkUserAllowed(1);  // 用户 1 消费唯一 token
    // 用户 2 应有自己的独立桶
    try { rl.checkUserAllowed(2); }
    catch (...) { FAIL("user 2 should have independent bucket"); return; }
    OK();
}

void test_rate_limiter_concurrent() {
    TEST("RateLimiter concurrent access (10 threads)");
    RateLimiter rl;
    rl.configureBackend("concurrent-test", 1000.0, 100.0);  // 高容量

    std::atomic<int> allowed{0};
    std::atomic<int> denied{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 10; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 10; i++) {
                try {
                    rl.checkBackendAllowed("concurrent-test");
                    allowed++;
                } catch (const RateLimitException&) {
                    denied++;
                }
            }
        });
    }
    for (auto& t : threads) t.join();

    std::cout << "(allowed=" << allowed << " denied=" << denied << ") ";
    if (allowed != 100) { FAIL("all 100 requests should be allowed with high burst"); return; }
    OK();
}

// ============================================================================
// CircuitBreaker 测试
// ============================================================================

CircuitBreaker::Permit requirePermit(CircuitBreaker& cb) {
    auto permit = cb.allowRequest();
    assert(permit.has_value());
    return *permit;
}

void test_cb_initial_state() {
    TEST("CircuitBreaker initial CLOSED");
    CircuitBreaker cb;
    cb.configure(3, 10000, 10000, 2, 2);
    cb.setBackendId("test");
    if (!cb.allowRequest()) { FAIL("should allow in CLOSED state"); return; }
    OK();
}

void test_cb_consecutive_failures_open() {
    TEST("CircuitBreaker consecutive failures open");
    CircuitBreaker cb;
    cb.configure(3, 10000, 10000, 2, 2);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));
    if (!cb.allowRequest()) { FAIL("should stay CLOSED before failure threshold"); return; }

    cb.reportFailure(requirePermit(cb));
    if (cb.allowRequest()) { FAIL("should be OPEN after reaching failure threshold"); return; }
    OK();
}

void test_cb_success_does_not_clear_before_reset_window() {
    TEST("CircuitBreaker success does not clear failures before reset window");
    CircuitBreaker cb;
    cb.configure(3, 10000, 10000, 2, 2);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));
    cb.reportSuccess(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));

    if (cb.allowRequest()) { FAIL("should open because success before reset window does not clear failures"); return; }
    OK();
}

void test_cb_failure_counter_expires_after_reset_window() {
    TEST("CircuitBreaker failure counter expires after reset window");
    CircuitBreaker cb;
    cb.configure(3, 30, 10000, 2, 2);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    cb.reportSuccess(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));

    if (!cb.allowRequest()) { FAIL("should remain CLOSED because old failures expired"); return; }
    OK();
}

void test_cb_half_open_and_recover() {
    TEST("CircuitBreaker OPEN -> HALF_OPEN -> CLOSED recovery");
    CircuitBreaker cb;
    cb.configure(2, 10000, 50, 2, 2);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));
    if (cb.allowRequest()) { FAIL("should be OPEN after failure threshold"); return; }

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    auto firstProbe = cb.allowRequest();
    if (!firstProbe) { FAIL("should allow first HALF_OPEN probe after timeout"); return; }
    cb.reportSuccess(*firstProbe);
    auto secondProbe = cb.allowRequest();
    if (!secondProbe) { FAIL("should allow second HALF_OPEN probe"); return; }
    cb.reportSuccess(*secondProbe);

    if (cb.state() != CircuitState::CLOSED) { FAIL("should be CLOSED after successful probes"); return; }
    if (!cb.allowRequest()) { FAIL("should allow normal request after recovery"); return; }
    OK();
}

void test_cb_half_open_call_limit() {
    TEST("CircuitBreaker HALF_OPEN call limit");
    CircuitBreaker cb;
    cb.configure(2, 10000, 50, 2, 3);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));
    if (cb.allowRequest()) { FAIL("should be OPEN after failure threshold"); return; }

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    if (!cb.allowRequest()) { FAIL("first probe should be allowed after timeout"); return; }
    if (!cb.allowRequest()) { FAIL("second probe should be allowed"); return; }
    if (cb.allowRequest()) { FAIL("third probe should be denied by half_open_max_calls"); return; }
    OK();
}

void test_cb_half_open_fail_reopens() {
    TEST("CircuitBreaker HALF_OPEN failure -> OPEN");
    CircuitBreaker cb;
    cb.configure(2, 10000, 50, 2, 2);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    cb.reportFailure(requirePermit(cb));
    if (cb.allowRequest()) { FAIL("should be OPEN before recovery timeout"); return; }

    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    auto probe = cb.allowRequest();
    if (!probe) { FAIL("should enter HALF_OPEN"); return; }
    cb.reportFailure(*probe);

    if (cb.allowRequest()) { FAIL("should be back to OPEN after probe failure"); return; }
    OK();
}

void test_cb_independent_instances() {
    TEST("CircuitBreaker independent instances");
    CircuitBreaker cb1, cb2;
    cb1.configure(2, 10000, 10000, 2, 2);
    cb2.configure(2, 10000, 10000, 2, 2);
    cb1.setBackendId("A");
    cb2.setBackendId("B");

    cb1.reportFailure(requirePermit(cb1));
    cb1.reportFailure(requirePermit(cb1));

    if (!cb2.allowRequest()) { FAIL("cb2 should be independent of cb1"); return; }
    OK();
}

void test_cb_stale_closed_result_does_not_recover_half_open() {
    TEST("CircuitBreaker stale CLOSED result does not recover HALF_OPEN");
    CircuitBreaker cb;
    cb.configure(1, 10000, 0, 1, 1);
    cb.setBackendId("test");

    auto slowRequest = cb.allowRequest();
    if (!slowRequest) { FAIL("slow CLOSED request should be allowed"); return; }
    auto failingRequest = cb.allowRequest();
    if (!failingRequest) { FAIL("failing CLOSED request should be allowed"); return; }
    cb.reportFailure(*failingRequest);

    auto probe = cb.allowRequest();
    if (!probe) { FAIL("HALF_OPEN probe should be allowed"); return; }
    cb.reportSuccess(*slowRequest);

    if (cb.state() != CircuitState::HALF_OPEN) {
        FAIL("stale CLOSED success must not close the HALF_OPEN generation");
        return;
    }
    OK();
}

void test_cb_success_threshold_is_reachable() {
    TEST("CircuitBreaker success threshold is reachable");
    CircuitBreaker cb;
    cb.configure(1, 10000, 0, 2, 3);
    cb.setBackendId("test");

    auto failingRequest = cb.allowRequest();
    if (!failingRequest) { FAIL("CLOSED request should be allowed"); return; }
    cb.reportFailure(*failingRequest);
    auto firstProbe = cb.allowRequest();
    if (!firstProbe) { FAIL("first HALF_OPEN probe should be allowed"); return; }
    cb.reportSuccess(*firstProbe);
    auto secondProbe = cb.allowRequest();
    if (!secondProbe) { FAIL("second HALF_OPEN probe should be allowed"); return; }
    cb.reportSuccess(*secondProbe);

    if (cb.state() != CircuitState::CLOSED) {
        FAIL("success threshold must not exceed the HALF_OPEN call budget");
        return;
    }
    OK();
}

void test_cb_concurrent_half_open_failure_wins() {
    TEST("CircuitBreaker concurrent HALF_OPEN failure wins");
    CircuitBreaker cb;
    cb.configure(1, 10000, 0, 2, 1);
    cb.setBackendId("test");

    cb.reportFailure(requirePermit(cb));
    auto successfulProbe = cb.allowRequest();
    auto failingProbe = cb.allowRequest();
    if (!successfulProbe || !failingProbe) {
        FAIL("both HALF_OPEN probes should be allowed");
        return;
    }

    std::atomic<bool> start{false};
    std::thread successThread([&]() {
        while (!start.load()) std::this_thread::yield();
        cb.reportSuccess(*successfulProbe);
    });
    std::thread failureThread([&]() {
        while (!start.load()) std::this_thread::yield();
        cb.reportFailure(*failingProbe);
    });

    start.store(true);
    successThread.join();
    failureThread.join();

    if (cb.state() != CircuitState::OPEN) {
        FAIL("any admitted HALF_OPEN failure must reopen the circuit");
        return;
    }
    OK();
}

void test_cb_concurrent_half_open_call_limit() {
    TEST("CircuitBreaker concurrent HALF_OPEN call limit");
    CircuitBreaker cb;
    cb.configure(1, 10000, 0, 2, 2);
    cb.setBackendId("test");
    cb.reportFailure(requirePermit(cb));

    std::atomic<bool> start{false};
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 32; ++i) {
        threads.emplace_back([&]() {
            while (!start.load()) std::this_thread::yield();
            if (cb.allowRequest()) ++allowed;
        });
    }

    start.store(true);
    for (auto& thread : threads) thread.join();

    if (allowed.load() != 2) {
        FAIL("concurrent probes must not exceed halfOpenMaxCalls");
        return;
    }
    OK();
}

void test_cb_concurrent_closed_failures_open() {
    TEST("CircuitBreaker concurrent CLOSED failures open");
    CircuitBreaker cb;
    constexpr int failureCount = 32;
    cb.configure(failureCount, 10000, 10000, 2, 2);
    cb.setBackendId("test");

    std::vector<CircuitBreaker::Permit> permits;
    permits.reserve(failureCount);
    for (int i = 0; i < failureCount; ++i) {
        permits.push_back(requirePermit(cb));
    }

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (auto permit : permits) {
        threads.emplace_back([&, permit]() {
            while (!start.load()) std::this_thread::yield();
            cb.reportFailure(permit);
        });
    }

    start.store(true);
    for (auto& thread : threads) thread.join();

    if (cb.state() != CircuitState::OPEN) {
        FAIL("concurrent failures reaching the threshold must open the circuit");
        return;
    }
    OK();
}

// ============================================================================
int main() {
    std::cout << "=== Gateway Unit Tests ===" << std::endl << std::endl;

    std::cout << "--- RateLimiter ---" << std::endl;
    test_token_bucket_basic();
    test_token_bucket_refill();
    test_rate_limiter_user_limit();
    test_rate_limiter_backend_limit();
    test_rate_limiter_unknown_backend();
    test_rate_limiter_different_users();
    test_rate_limiter_concurrent();

    std::cout << std::endl << "--- CircuitBreaker ---" << std::endl;
    test_cb_initial_state();
    test_cb_consecutive_failures_open();
    test_cb_success_does_not_clear_before_reset_window();
    test_cb_failure_counter_expires_after_reset_window();
    test_cb_half_open_and_recover();
    test_cb_half_open_call_limit();
    test_cb_half_open_fail_reopens();
    test_cb_independent_instances();
    test_cb_stale_closed_result_does_not_recover_half_open();
    test_cb_success_threshold_is_reachable();
    test_cb_concurrent_half_open_failure_wins();
    test_cb_concurrent_half_open_call_limit();
    test_cb_concurrent_closed_failures_open();

    std::cout << std::endl
              << "=== Results: " << passed << " passed, "
              << failed << " failed ===" << std::endl;

    return failed ? 1 : 0;
}
