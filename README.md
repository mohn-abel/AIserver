# AIserver

基于开源项目 [Kama-HTTPServer](https://github.com/youngyangyang04/Kama-HTTPServer)（[CppAIService](https://github.com/youngyangyang04/CppAIService)）进行代码重构的 AI 智能聊天服务。

使用 C++17 和 Muduo 网络库实现高并发 Reactor 模型，集成多模型 AI 对话、语音处理、图像识别等功能。

---

## 与原项目的差异

> 原项目 Kama-HTTPServer 是[代码随想录知识星球](https://programmercarl.com/other/kstar.html)的教学项目，包含自研 HTTP 框架、五子棋游戏服务（GomokuServer）和 AI 聊天服务（ChatServer）。

本项目在原项目基础上进行了以下重构：

| 类别 | 改动 |
|------|------|
| **结构清理** | 移除 `WebApps/GomokuServer` 五子棋游戏应用，聚焦 AI 聊天场景 |
| **新增线程池** | 新增 `HttpServer/include/utils/ThreadPool` 通用线程池模块，支持异步任务提交 |
| **AI 模型重构** | 原项目硬编码 4 个独立策略类（AliyunStrategy / DouBaoStrategy / AliyunRAGStrategy / AliyunMcpStrategy），重构为 `GenericAIStrategy`（OpenAI 兼容接口）+ 配置驱动模式 |
| **模型配置化** | 新增 `model_config.json` 运行时配置，支持动态注册模型和别名，新增 DeepSeek 模型 |
| **框架修复** | 优化 Session 管理、HttpResponse 异步响应、MysqlUtil 封装、DbConnection 连接池等核心模块 |
| **工程化** | 添加 `.gitignore`、压力测试脚本 `login.lua`、批量注册脚本 `create_users.sh` |

---

## 项目架构

```
AIserver/
├── HttpServer/                        # HTTP/HTTPS 服务框架
│   ├── include/
│   │   ├── http/                      # HTTP 核心：请求解析、响应构建、服务主类
│   │   │   ├── HttpServer.h           # 服务入口，管理连接/路由/中间件/Session/SSL
│   │   │   ├── HttpContext.h          # HTTP 协议解析状态机
│   │   │   ├── HttpRequest.h          # HTTP 请求对象
│   │   │   └── HttpResponse.h         # HTTP 响应对象（支持异步响应）
│   │   ├── router/                    # 路由模块
│   │   │   ├── Router.h               # 静态路由 + 动态路由（正则匹配）
│   │   │   └── RouterHandler.h        # 路由处理器抽象基类
│   │   ├── middleware/                # 中间件模块
│   │   │   ├── Middleware.h           # 中间件抽象接口
│   │   │   ├── MiddlewareChain.h      # 中间件链（请求/响应拦截）
│   │   │   └── cors/                  # CORS 跨域中间件
│   │   ├── session/                   # 会话管理模块
│   │   │   ├── Session.h              # 会话对象
│   │   │   ├── SessionManager.h       # 会话管理器（创建/加载/销毁）
│   │   │   └── SessionStorage.h       # 会话存储接口 + 内存实现（线程安全）
│   │   ├── ssl/                       # SSL/HTTPS 模块
│   │   │   ├── SslConfig.h            # SSL 配置
│   │   │   ├── SslContext.h           # OpenSSL 上下文封装
│   │   │   ├── SslConnection.h        # SSL 连接（内存 BIO）
│   │   │   └── SslTypes.h             # SSL 类型定义
│   │   └── utils/                     # 工具模块
│   │       ├── JsonUtil.h             # JSON 解析（nlohmann/json）
│   │       ├── MysqlUtil.h            # MySQL 操作封装 + QueryResult RAII
│   │       ├── FileUtil.h             # 文件读写工具
│   │       ├── ThreadPool.h           # 通用线程池（新增）
│   │       └── db/                    # 数据库连接池
│   └── src/                           # 框架实现源码（与 include 对应）
│
├── AIApps/ChatServer/                 # AI 聊天服务应用
│   ├── main.cpp                       # 入口
│   ├── ChatServer.h/.cpp              # 服务主类：路由注册、会话管理、全局状态
│   ├── include/AIUtil/                # AI 工具模块
│   │   ├── AIHelper.h                 # AI 对话核心（多模型统一接口）
│   │   ├── AIStrategy.h               # 策略模式：GenericAIStrategy / AliyunRAGStrategy
│   │   ├── AIFactory.h                # 工厂模式：配置驱动注册 + 别名解析
│   │   ├── AIConfig.h                 # MCP 工具提示词配置管理
│   │   ├── AIToolRegistry.h           # MCP 工具注册与调用
│   │   ├── AISpeechProcessor.h        # 百度语音识别（ASR）+ 语音合成（TTS）
│   │   ├── ImageRecognizer.h          # ONNX Runtime 图像分类识别
│   │   ├── MQManager.h                # RabbitMQ 消息队列管理
│   │   └── base64.h                   # Base64 编解码
│   ├── include/handlers/              # HTTP 请求处理器（13 个 Handler）
│   ├── src/                           # 应用实现源码
│   └── resource/                      # 静态页面与配置文件
│       ├── model_config.json          # AI 模型配置（运行时加载）
│       ├── config.json                # MCP 工具配置
│       └── *.html                     # 前端页面
│
├── CMakeLists.txt                     # 项目构建配置
├── login.lua                          # wrk 登录压测脚本（新增）
├── create_users.sh                    # 批量注册测试用户（新增）
└── images/                            # 运行截图
```

---

## 一、HttpServer 框架

基于 Muduo 事件驱动网络库实现的 HTTP/HTTPS 服务框架。

### 核心模块

| 模块 | 功能描述 |
|------|----------|
| **HTTP 模块** | HTTP 协议解析（状态机）、请求对象封装、响应构建与发送 |
| **路由模块** | 静态路由（精确匹配）+ 动态路由（正则匹配），支持函数回调与 Handler 对象两种注册方式 |
| **中间件模块** | 中间件链模式，请求前置处理 + 响应后置处理，内置 CORS 跨域中间件 |
| **会话管理** | 基于 Cookie + Session 的用户状态管理，多线程安全访问 |
| **SSL 模块** | 基于 OpenSSL + 内存 BIO 的 HTTPS 加密传输 |
| **数据库模块** | MySQL 连接池（`DbConnectionPool`），连接复用，参数化查询防注入 |
| **线程池** | 通用线程池（`ThreadPool`），支持异步任务提交（**新增**） |

### 重构优化内容

- **ThreadPool**：新增通用线程池模块，ChatServer 使用线程池异步执行 AI 推理，避免阻塞 I/O 线程
- **HttpResponse**：增强异步响应支持（`setDeferred` / `setConnection`），配合线程池实现业务逻辑与网络 I/O 解耦
- **MysqlUtil**：新增 `QueryResult` RAII 封装，自动管理 Statement 和 ResultSet 生命周期
- **DbConnection**：优化连接池健康检查、StmtDeleter 自动清理、UTF-8 编码支持

---

## 二、AI Chat 应用

在 HttpServer 框架之上构建的 AI 智能聊天服务。

### 功能清单

| 功能 | 描述 |
|------|------|
| **多模型对话** | 支持阿里云通义千问、DeepSeek 等 OpenAI 兼容 API，运行时切换 |
| **RAG 检索增强** | 结合阿里云 DashScope 知识库进行增强生成 |
| **MCP 工具调用** | AI 可自主调用天气查询、时间查询等工具（两阶段推理） |
| **多轮对话** | 维护每用户每会话的对话历史，支持跨轮次上下文记忆 |
| **语音识别** | 集成百度语音 API，PCM 音频转文本（ASR） |
| **语音合成** | 文本转 MP3 音频，Base64 编码返回（TTS） |
| **图像识别** | ONNX Runtime + OpenCV，MobileNetV2 模型推理 |
| **用户系统** | 注册 / 登录 / 登出，基于 Session 的身份认证 |
| **消息持久化** | RabbitMQ 消息队列异步写入 MySQL，业务与存储解耦 |

### 设计模式

```
AIStrategy（抽象基类）
├── GenericAIStrategy      # OpenAI 兼容接口（通义千问、DeepSeek 等）
│   └── model_config.json  # 配置驱动，运行时动态注册
└── AliyunRAGStrategy      # 阿里云 DashScope RAG（检索增强）
```

综合运用策略模式、工厂模式（配置驱动注册 + 别名解析）、单例模式、连接池模式。

### 重构对比：AI 模型系统

**原项目**：每个模型一个独立策略类（AliyunStrategy / DouBaoStrategy / AliyunRAGStrategy / AliyunMcpStrategy），在编译期通过 `StrategyRegister<T>` 模板静态注册，新增模型需要编写新的类。

**重构后**：
- 通过 `GenericAIStrategy` 统一处理所有 OpenAI 兼容 API
- `StrategyFactory::loadFromConfig()` 从 `model_config.json` 运行时加载模型
- 支持别名系统（`"1"` → `"aliyun-qwen"`），兼容前端旧接口
- 新增 DeepSeek 模型支持仅需在 JSON 中添加一行配置

### 异步消息架构

```
用户消息 → ChatSendHandler（I/O 线程）
    │
    ├── 设置 Deferred Response（不阻塞 I/O 线程）
    ├── 投递到业务 ThreadPool → 异步调用 AI API
    ├── AI 响应后通过 runInLoop() 回写 HTTP 响应
    └── 同时投递 SQL INSERT 到 RabbitMQ
              │
              ▼
     RabbitMQThreadPool（2 Worker 消费）
              │
              └── 异步写入 MySQL chat_message 表
```

---

## 三、环境与依赖

### 操作系统

- Ubuntu 22.04 LTS

### 依赖列表

| 依赖 | 用途 | 安装方式 |
|------|------|----------|
| g++ / cmake / make | C++17 编译工具链 | `sudo apt install g++ cmake make` |
| Muduo | Reactor 网络库 | [源码编译](https://blog.csdn.net/QIANGWEIYUAN/article/details/89023980) |
| Boost 1.69+ | C++ 基础库 | [源码编译](https://blog.csdn.net/QIANGWEIYUAN/article/details/88792874) |
| OpenSSL | HTTPS/SSL 支持 | `sudo apt install libssl-dev` |
| MySQL 5.7+ | 数据库 | `sudo apt install mysql-server` |
| mysqlcppconn | MySQL C++ 连接器 | `sudo apt install libmysqlcppconn-dev` |
| nlohmann/json 3.x | JSON 解析 | `sudo apt install nlohmann-json3-dev` |
| OpenCV 4.x | 图像处理 | `sudo apt install libopencv-dev` |
| ONNX Runtime 1.20 | 模型推理引擎 | [下载预编译包](https://github.com/microsoft/onnxruntime) |
| RabbitMQ | 消息队列服务端 | `sudo apt install rabbitmq-server` |
| librabbitmq | RabbitMQ C 客户端库 | `sudo apt install librabbitmq-dev` |
| SimpleAmqpClient | RabbitMQ C++ 客户端 | 源码编译 |
| libcurl | HTTP 客户端（调用 AI API） | `sudo apt install libcurl4-openssl-dev` |

### 快速安装

```bash
# 编译工具链
sudo apt install g++ cmake make

# 系统包管理安装
sudo apt update
sudo apt install nlohmann-json3-dev libmysqlcppconn-dev libssl-dev \
                 libopencv-dev libcurl4-openssl-dev librabbitmq-dev \
                 rabbitmq-server mysql-server

# Muduo 与 Boost 需源码编译（参考上方链接）
# ONNX Runtime 需下载预编译包放置于 ../onnxruntime-linux-x64-1.20.0/
# SimpleAmqpClient 需源码编译安装
```

---

## 四、编译与运行

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 启动依赖服务

```bash
service mysql start
rabbitmq-server -detached
rabbitmqctl start_app
rabbitmqctl status
```

### 启动

```bash
cd build && ./http_server           # 默认 80 端口（需 root）
cd build && ./http_server -p 8080   # 指定端口
```

### 配置

- **AI 模型**：编辑 `AIApps/ChatServer/resource/model_config.json`，配置 API Key 和端点
- **MCP 工具**：编辑 `AIApps/ChatServer/resource/config.json`
- **百度语音**：设置环境变量 `BAIDU_CLIENT_ID` / `BAIDU_CLIENT_SECRET`
- **数据库**：确保 MySQL 中存在 `ChatHttpServer` 数据库及 `users`、`chat_message` 表

---

## 五、API 接口

### 用户接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/register` | POST | 用户注册 |
| `/login` | POST | 用户登录（返回 Session Cookie） |
| `/user/logout` | POST | 用户登出 |

### 页面接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 重定向到聊天入口页 |
| `/entry` | GET | 登录/注册页面 |
| `/chat` | GET | 聊天主页面 |
| `/menu` | GET | 功能选择菜单 |
| `/upload` | GET | 文件上传页面 |

### 聊天接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/chat/send` | POST | 发送消息，AI 回复 |
| `/chat/send-new-session` | POST | 创建新会话并发送消息 |
| `/chat/history` | POST | 查询历史消息 |
| `/chat/sessions` | GET | 获取用户会话列表 |
| `/chat/tts` | POST | 语音合成（文字转语音） |

### 上传接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/upload/send` | POST | 上传图片并 AI 分析 |

### 请求示例

```bash
# 登录
curl -X POST http://127.0.0.1:8080/login \
  -H "Content-Type: application/json" \
  -d '{"username":"user1","password":"123"}'

# 发送消息（需携带 Session Cookie）
curl -X POST http://127.0.0.1:8080/chat/send \
  -H "Content-Type: application/json" \
  -H "Cookie: sessionId=xxx" \
  -d '{"sessionId":"xxx","content":"你好"}'
```

---

## 六、压测

项目提供 wrk Lua 脚本用于压力测试。

### 场景一：GET 页面压测（静态路由）

```bash
wrk -t4 -c200 -d30s http://127.0.0.1:8080/entry
```

| 指标 | 数值 |
|------|------|
| 线程/连接 | 4 threads / 200 connections |
| QPS | ~71133 req/s |
| 平均延迟 | ~2.83 ms |
| 吞吐 | ~515 MB/s |

### 场景二：动态路由 + Session 鉴权

```bash
wrk -t4 -c200 -d30s -H "Cookie: sessionId=<yourSessionId>" http://127.0.0.1:8080/menu
```

| 指标 | 数值 |
|------|------|
| 线程/连接 | 4 threads / 200 connections |
| QPS | ~53754 req/s |
| 平均延迟 | ~3.72 ms |
| 吞吐 | ~424.57 MB/s |

### 场景三：登录压测（POST + JSON）

```bash
wrk -t4 -c100 -d30s -s login.lua http://127.0.0.1:8080/login
```

| 指标 | 数值 |
|------|------|
| 线程/连接 | 4 threads / 100 connections |
| QPS | ~2421 req/s |
| 平均延迟 | ~41.2 ms |
| 吞吐 | ~0.91 MB/s |

---

## 项目总结

- 基于开源项目 [Kama-HTTPServer](https://github.com/youngyangyang04/Kama-HTTPServer) / [CppAIService](https://github.com/youngyangyang04/CppAIService) 重构而来
- HttpServer 框架基于 Muduo Reactor 多线程模型，单机 QPS 可达 **6 万+**
- 重构 AI 模型系统：从硬编码策略 → 配置驱动 `GenericAIStrategy`，新增 DeepSeek 模型支持
- 新增 `ThreadPool` 通用线程池模块，配合异步响应实现业务逻辑与网络 I/O 解耦
- 通过 RabbitMQ 消息队列实现消息异步持久化
- 综合运用策略模式、工厂模式、单例模式、连接池模式等设计模式
- 涵盖网络编程、多线程、设计模式、数据库、消息队列、AI 集成等多方面知识点
