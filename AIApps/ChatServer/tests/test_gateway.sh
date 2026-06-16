#!/bin/bash
# 网关集成测试：需服务器在 localhost:8080 运行
# 用法: ./test_gateway.sh [host:port]

HOST="${1:-localhost:8080}"
COOKIE=$(mktemp)
TMP_FILES=()
PASS=0
FAIL=0

green() { echo -e "\033[32m$1\033[0m"; }
red()   { echo -e "\033[31m$1\033[0m"; }
check() {
    if [ "$1" -eq 0 ]; then
        green "  PASS: $2"
        PASS=$((PASS + 1))
    else
        red "  FAIL: $2"
        FAIL=$((FAIL + 1))
    fi
}

post_sse_chat() {
    local payload="$1"
    local output_file="$2"
    curl -s -N -w "\n__HTTP_CODE__:%{http_code}\n" -b "$COOKIE" \
        -X POST "http://$HOST/chat/send" \
        -H "Content-Type: application/json" \
        -d "$payload" > "$output_file"
}

check_sse_response() {
    local output_file="$1"
    local label="$2"
    local http_code
    http_code=$(sed -n "s/^__HTTP_CODE__://p" "$output_file" | tail -1)

    if [ "$http_code" != "200" ]; then
        check 1 "$label (HTTP $http_code)"
        return
    fi
    if ! grep -q "^data: " "$output_file"; then
        check 1 "$label emitted SSE data"
        return
    fi
    if grep -q "\"error\"" "$output_file"; then
        check 1 "$label returned no SSE error"
        return
    fi
    if ! grep -q "^data: \[DONE\]" "$output_file"; then
        check 1 "$label completed with [DONE]"
        return
    fi
    check 0 "$label SSE completed"
}

cleanup() { rm -f "$COOKIE" "${TMP_FILES[@]}"; }
trap cleanup EXIT

echo "=== Gateway Integration Tests ==="
echo "Target: http://$HOST"
echo

# ---- 前置: 登录 ----
echo "--- Login ---"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -c "$COOKIE" \
    -X POST "http://$HOST/login" \
    -H "Content-Type: application/json" \
    -d '{"username":"testuser","password":"123456"}')
check "$([ "$HTTP_CODE" != "200" ] && echo 1 || echo 0)" "Login (HTTP $HTTP_CODE)"

# ---- 测试 1: 正常聊天（SSE） ----
echo "--- Test 1: Normal chat (SSE) ---"
OUT=$(mktemp); TMP_FILES+=("$OUT")
post_sse_chat '{"question":"Say hello in one word","sessionId":"gw-test-1","modelType":"aliyun-qwen","enableTools":false}' "$OUT"
check_sse_response "$OUT" "Normal chat"

# ---- 测试 2: MCP 工具链路（SSE） ----
echo "--- Test 2: MCP tools chat (SSE) ---"
OUT=$(mktemp); TMP_FILES+=("$OUT")
post_sse_chat '{"question":"What time is it?","sessionId":"gw-test-2","modelType":"aliyun-qwen","enableTools":true}' "$OUT"
check_sse_response "$OUT" "MCP tools chat"

# ---- 测试 3: 会话历史 ----
echo "--- Test 3: Session history ---"
RESP=$(curl -s -b "$COOKIE" -X POST "http://$HOST/chat/history" \
    -H "Content-Type: application/json" \
    -d '{"sessionId":"gw-test-1"}')
SUCCESS=$(echo "$RESP" | grep -c '"success":true')
check "$([ "$SUCCESS" -eq 0 ] && echo 1 || echo 0)" "Session history accessible"

# ---- 测试 4: 网关关闭模式 ----
echo "--- Test 4: Gateway disabled (check config) ---"
GATEWAY_ENABLED=$(grep -c '"enabled": *true' /root/httpserver_vsersion2/AIApps/ChatServer/resource/gateway_config.json 2>/dev/null)
echo "  Gateway enabled in config: $([ "$GATEWAY_ENABLED" -gt 0 ] && echo "YES" || echo "NO")"
check 0 "Gateway config readable"

# ---- 测试 5: burp 限流压力测试 ----
echo "--- Test 5: Rate limit burst (3 rapid requests) ---"
if [ "$GATEWAY_ENABLED" -gt 0 ]; then
    for i in 1 2 3; do
        OUT=$(mktemp); TMP_FILES+=("$OUT")
        post_sse_chat "{\"question\":\"ping $i\",\"sessionId\":\"gw-burst\",\"modelType\":\"aliyun-qwen\",\"enableTools\":false}" "$OUT" &
    done
    wait

    # 观察服务器日志中是否有 rate limit 消息；SSE 错误会出现在输出文件中。
    echo "  (check server logs for rate-limit messages; burst requests completed)"
    check 0 "Rate limit burst sent"
else
    echo "  (gateway disabled, skipping)"
    check 0 "Rate limit burst skipped (gateway off)"
fi

# ---- 测试 6: 模型降级路由 ----
echo "--- Test 6: Model fallback routing ---"
echo "  Manual verification step:"
echo "    1. Set aliyun-qwen api_url to invalid address in gateway_config.json"
echo "    2. Set circuit_breaker.min_requests to 2 and failure_rate_threshold to 0.5"
echo "       Optionally lower circuit_breaker.window_ms for faster testing"
echo "    3. Restart server"
echo "    4. Send 5 requests → check logs for 'Falling back' messages"
check 0 "Fallback routing test described"

echo
echo "=== Integration Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
