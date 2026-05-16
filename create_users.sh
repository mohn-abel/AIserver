#!/bin/bash

# 循环 1000 次
for i in {1..1000}
do
   # 构造 JSON 数据，账号为 testuser1 到 testuser1000，密码统一为 123456
   PAYLOAD="{\"username\": \"testuser$i\", \"password\": \"123456\"}"
   
   # 调用注册接口 (-s 屏蔽 curl 进度条，仅输出必要信息)
   curl -X POST http://127.0.0.1:8080/register \
        -H "Content-Type: application/json" \
        -d "$PAYLOAD" -s > /dev/null
        
   # 打印进度
   echo "已发送注册请求: testuser$i"
done

echo "1000 个账号注册脚本执行完毕。"