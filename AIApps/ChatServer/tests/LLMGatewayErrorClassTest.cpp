#include "../include/LLMGateway/GatewayTypes.h"

#include <cassert>

int main() {
    BackendHttpException authError(
        401,
        BackendErrorClass::kAuth,
        "Backend HTTP 401: unauthorized");
    assert(authError.statusCode() == 401);
    assert(authError.errorClass() == BackendErrorClass::kAuth);
    assert(!authError.countsForCircuitBreaker());
    assert(!authError.shouldTryFallback());

    BackendHttpException rateLimitError(
        429,
        BackendErrorClass::kRateLimited,
        "Backend HTTP 429: rate limited");
    assert(rateLimitError.countsForCircuitBreaker());
    assert(rateLimitError.shouldTryFallback());

    BackendHttpException unavailableError(
        503,
        BackendErrorClass::kBackendUnavailable,
        "Backend HTTP 503: unavailable");
    assert(unavailableError.countsForCircuitBreaker());
    assert(unavailableError.shouldTryFallback());

    return 0;
}
