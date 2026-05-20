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

void test_cb_initial_state() {
    TEST("CircuitBreaker initial CLOSED");
    CircuitBreaker cb;
    cb.configure(3, 10000, 2);
    cb.setBackendId("test");
    if (cb.allowRequest() != true) { FAIL("should allow in CLOSED state"); return; }
    OK();
}

void test_cb_trip_to_open() {
    TEST("CircuitBreaker CLOSED -> OPEN after threshold");
    CircuitBreaker cb;
    cb.configure(3, 10000, 2);
    cb.setBackendId("test");

    // 3 次失败应该触发熔断
    cb.reportFailure();
    cb.reportFailure();
    if (!cb.allowRequest()) { FAIL("still CLOSED after 2 failures"); return; }
    cb.reportFailure();  // 第 3 次 → OPEN

    if (cb.allowRequest()) { FAIL("should be OPEN after 3 failures"); return; }
    OK();
}

void test_cb_half_open_and_recover() {
    TEST("CircuitBreaker OPEN -> HALF_OPEN -> CLOSED recovery");
    CircuitBreaker cb;
    cb.configure(2, 50, 2);  // threshold=2, recovery=50ms
    cb.setBackendId("test");

    // 触发熔断
    cb.reportFailure();
    cb.reportFailure();

    // 等待恢复超时
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // 应该进入 HALF_OPEN 并放行
    if (!cb.allowRequest()) { FAIL("should allow in HALF_OPEN after timeout"); return; }

    // 2 次成功应该恢复
    cb.reportSuccess();
    cb.reportSuccess();
    // 现在应该是 CLOSED，再试应该放行
    if (!cb.allowRequest()) { FAIL("should be CLOSED after recovery"); return; }
    OK();
}

void test_cb_half_open_fail_reopens() {
    TEST("CircuitBreaker HALF_OPEN failure -> OPEN");
    CircuitBreaker cb;
    cb.configure(2, 50, 2);
    cb.setBackendId("test");

    // 触发熔断
    cb.reportFailure();
    cb.reportFailure();

    // 等待恢复
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    // HALF_OPEN 探测
    if (!cb.allowRequest()) { FAIL("should enter HALF_OPEN"); return; }
    // 探测失败 → 回到 OPEN
    cb.reportFailure();

    if (cb.allowRequest()) { FAIL("should be back to OPEN after probe failure"); return; }
    OK();
}

void test_cb_success_resets_counter() {
    TEST("CircuitBreaker success resets failure counter");
    CircuitBreaker cb;
    cb.configure(5, 10000, 2);
    cb.setBackendId("test");

    // 2 次失败，1 次成功，再 2 次失败（应仍在 CLOSED，因为成功重置了计数）
    cb.reportFailure();
    cb.reportFailure();
    cb.reportSuccess();  // 重置
    cb.reportFailure();
    cb.reportFailure();

    if (!cb.allowRequest()) { FAIL("still CLOSED after 2+success+2 failures"); return; }
    cb.reportFailure();  // 总共 3 次连续失败
    cb.reportFailure();
    cb.reportFailure();
    if (cb.allowRequest()) { FAIL("should be OPEN after 5 continuous"); return; }
    OK();
}

void test_cb_independent_instances() {
    TEST("CircuitBreaker independent instances");
    CircuitBreaker cb1, cb2;
    cb1.configure(2, 10000, 2);
    cb2.configure(2, 10000, 2);
    cb1.setBackendId("A");
    cb2.setBackendId("B");

    // 触发 cb1 熔断
    cb1.reportFailure();
    cb1.reportFailure();

    // cb2 应不受影响
    if (!cb2.allowRequest()) { FAIL("cb2 should be independent of cb1"); return; }
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
    test_cb_trip_to_open();
    test_cb_half_open_and_recover();
    test_cb_half_open_fail_reopens();
    test_cb_success_resets_counter();
    test_cb_independent_instances();

    std::cout << std::endl
              << "=== Results: " << passed << " passed, "
              << failed << " failed ===" << std::endl;

    return failed ? 1 : 0;
}
