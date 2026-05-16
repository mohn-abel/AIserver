# Kama-HTTPServer

基于 C++17 的自研 HTTP 服务框架与 AI 智能聊天服务，使用 Muduo 网络库实现高并发 Reactor 模型。

> ⭐️ 本项目为[【代码随想录知识星球】](https://programmercarl.com/other/kstar.html)教学项目  
> ⭐️ 在 [HTTP 服务框架文档](https://www.programmercarl.com/other/project_http.html) 里详细讲解：项目前置知识 + 项目细节 + 代码解读 + 项目难点 + 面试题与回答 + 简历写法 + 项目拓展。

---

## 目录

- [项目架构](#项目架构)
- [HttpServer 框架](#一httpserver-框架)
- [AI Chat 应用](#二ai-chat-应用)
- [环境与依赖](#三环境与依赖)
- [编译与运行](#四编译与运行)
- [API 接口](#五api-接口)
- [压测](#六压测)
- [运行截图](#七运行截图)
- [项目总结](#八项目总结)

---

## 项目架构

```
Kama-HTTPServer/
├── HttpServer/                        # 自研 HTTP 服务框架
│   ├── include/
│   │   ├── http/                      # HTTP 核心：请求解析、响应构建、服务主类
│   │   │   ├── HttpServer.h           # 服务入口，管理连接/路由/中间件/Session/SSL
│   │   │   ├── HttpContext.h          # HTTP 协议解析状态机
│   │   │   ├── HttpRequest.h          # HTTP 请求对象
│   │   │   └── HttpResponse.h         # HTTP 响应对象
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
│   │   │   ├── SslConnection.h        # SSL 连接封装
│   │   │   └── SslTypes.h             # SSL 类型定义
│   │   └── utils/                     # 工具模块
│   │       ├── JsonUtil.h             # JSON 解析（nlohmann/json）
│   │       ├── MysqlUtil.h            # MySQL 操作封装
│   │       ├── FileUtil.h             # 文件读写工具
│   │       └── db/                    # 数据库连接池
│   └── src/                           # 框架实现源码（与 include 对应）
│
├── AIApps/ChatServer/                 # AI 聊天服务应用
│   ├── include/
│   │   ├── ChatServer.h               # 服务主类：路由注册、会话管理、全局状态
│   │   ├── AIUtil/                    # AI 工具模块
│   │   │   ├── AIHelper.h             # AI 对话核心（多模型统一接口）
│   │   │   ├── AIStrategy.h           # 策略模式抽象基类（多模型支持）
│   │   │   ├── AIFactory.h            # 工厂模式：根据类型创建策略
│   │   │   ├── AIConfig.h             # API Key、端点等配置管理
│   │   │   ├── AIToolRegistry.h       # MCP 工具注册与调用
│   │   │   ├── AISessionIdGenerator.h # 会话 ID 生成器
│   │   │   ├── AISpeechProcessor.h    # 语音识别（ASR）+ 语音合成（TTS）
│   │   │   ├── ImageRecognizer.h      # ONNX Runtime 图像分类识别
│   │   │   ├── base64.h               # Base64 编解码
│   │   │   └── MQManager.h            # RabbitMQ 消息队列管理
│   │   └── handlers/                  # HTTP 请求处理器（每个路由一个 Handler）
│   │       ├── ChatLoginHandler.h     # POST /login — 用户登录
│   │       ├── ChatRegisterHandler.h  # POST /register — 用户注册
│   │       ├── ChatLogoutHandler.h    # POST /user/logout — 用户登出
│   │       ├── ChatEntryHandler.h     # GET /entry — 聊天入口页
│   │       ├── ChatHandler.h          # GET /chat — 聊天主页面
│   │       ├── ChatSendHandler.h      # POST /chat/send — 发送消息
│   │       ├── ChatCreateAndSendHandler.h  # POST /chat/send-new-session — 新会话+发送
│   │       ├── ChatHistoryHandler.h   # POST /chat/history — 查询历史消息
│   │       ├── ChatSessionsHandler.h  # GET /chat/sessions — 获取会话列表
│   │       ├── ChatSpeechHandler.h    # POST /chat/tts — 语音合成
│   │       ├── AIMenuHandler.h        # GET /menu — 功能菜单页
│   │       ├── AIUploadHandler.h      # GET /upload — 上传页面
│   │       └── AIUploadSendHandler.h  # POST /upload/send — 上传并分析
│   ├── src/                           # 应用实现源码（与 include 对应）
│   └── resource/                      # 静态页面资源
│       ├── entry.html                 # 登录/注册页面
│       ├── menu.html                  # 功能选择菜单
│       ├── AI.html                    # AI 聊天界面
│       ├── upload.html                # 文件上传界面
│       ├── NotFound.html              # 404 页面
│       └── config.json                # MCP 工具配置文件
│
├── CMakeLists.txt                     # 项目构建配置
├── login.lua                          # wrk 登录压测脚本
└── images/                            # 运行截图
```

---

## 一、HttpServer 框架

基于 Muduo 事件驱动网络库，从零实现完整的 HTTP/HTTPS 服务框架。

### 核心模块

| 模块 | 功能描述 |
|------|----------|
| **HTTP 模块** | HTTP 协议解析（状态机）、请求对象封装、响应构建与发送，支持 GET/POST |
| **路由模块** | 静态路由（精确匹配）+ 动态路由（正则匹配），支持函数回调与 Handler 对象两种注册方式 |
| **中间件模块** | 中间件链模式（Chain of Responsibility），请求前置处理 + 响应后置处理，内置 CORS 跨域中间件 |
| **会话管理** | 基于 Cookie + Session 的用户状态管理，`MemorySessionStorage` 支持多线程安全访问 |
| **SSL 模块** | 基于 OpenSSL 的 HTTPS 加密传输，支持证书配置 |
| **数据库模块** | MySQL 连接池（`DbConnectionPool`），连接复用，封装常用查询与更新操作 |
| **工具模块** | JSON 解析（nlohmann/json）、文件读写、MySQL 操作封装 |

### 框架特性

- 🔄 **Reactor 模型**：基于 Muduo `EventLoop` + 多线程，高并发处理
- 🔐 **HTTP + HTTPS**：双协议支持，通过 `useSSL` 参数切换
- 🧭 **灵活路由**：支持精确匹配和正则动态路由，`Get()` / `Post()` / `addRoute()` API
- ⛓️ **中间件链**：请求/响应流水线处理，可自定义中间件
- 🔑 **Session 管理**：自动从 Cookie 获取或创建 Session，支持多种存储后端
- 📦 **连接池**：MySQL 连接池和 RabbitMQ Channel 池，减少连接开销

---

## 二、AI Chat 应用

在 HttpServer 框架之上构建的 AI 智能聊天服务。

### 功能清单

| 功能 | 描述 |
|------|------|
| **多模型对话** | 支持阿里云通义千问、豆包（DouBao）等大模型，可在运行时切换 |
| **RAG 检索增强** | 结合外部知识库进行增强生成回答 |
| **MCP 工具调用** | Model Context Protocol，AI 可自主调用天气查询、时间查询等工具 |
| **多轮对话** | 维护每用户每会话的对话历史，支持跨轮次上下文记忆 |
| **语音识别** | 集成百度语音 API，PCM 音频 → 文本（ASR） |
| **语音合成** | 文本 → MP3 音频，Base64 编码返回（TTS） |
| **图像识别** | ONNX Runtime + OpenCV，支持图像分类 |
| **用户系统** | 注册 / 登录 / 登出，基于 Session 的身份认证 |
| **消息持久化** | RabbitMQ 消息队列异步写入 MySQL，业务与存储解耦 |
| **聊天历史** | 服务启动加载历史，运行时可查询 |

### 技术亮点

#### 1. 策略模式 + 工厂模式（AI 模型切换）

```
AIStrategy（抽象基类）
├── AliyunStrategy         # 阿里云通义千问
├── DouBaoStrategy         # 豆包大模型
├── AliyunRAGStrategy     # 阿里云 RAG（检索增强）
└── AliyunMcpStrategy     # 阿里云 MCP（工具调用）
```

`AIFactory` 根据配置类型创建对应策略，`AIHelper` 依赖抽象基类。新增模型只需添加 Strategy 子类和工厂分支，符合开闭原则。

#### 2. MCP 工具调用流程

```
用户消息 → AIHelper → AI 模型判断 → 需要工具？
    ├── 否 → 直接返回文本回复
    └── 是 → 返回 JSON 调用指令 → AIToolRegistry 执行工具
              → 结果回传 AI 模型 → 生成最终回复
```

在 `resource/config.json` 中配置可用工具，`AIToolRegistry` 管理注册与调用。

#### 3. RabbitMQ 异步消息队列

```
ChatSendHandler
    │
    ├── 1. 调用 AI 获取回复
    ├── 2. 立即返回 HTTP 响应
    └── 3. 将 SQL INSERT 消息投递到 RabbitMQ
              │
              ▼
    RabbitMQThreadPool（多 Worker 消费）
              │
              └── 异步执行 SQL，写入 chat_message 表
```

- **MQManager**：单例模式，管理 RabbitMQ Channel 连接池
- **RabbitMQThreadPool**：多线程消费消息队列，异步写入 MySQL
- 实现业务响应与数据持久化解耦，提升用户体验

#### 4. 语音处理

`AISpeechProcessor` 集成百度语音 API：
- **语音识别（ASR）**：接收 PCM 音频数据，返回识别文本
- **语音合成（TTS）**：接收文本，返回 MP3 音频（Base64 编码）

#### 5. 图像识别

`ImageRecognizer` 基于 ONNX Runtime 加载预训练模型：
- 使用 OpenCV 进行图像预处理（resize、归一化等）
- 支持从文件路径、内存 Buffer、cv::Mat 三种输入方式
- 返回 Top-1 分类结果

#### 6. 连接池模式

- **MySQL 连接池**：`DbConnectionPool` 管理连接复用，避免频繁创建/销毁
- **RabbitMQ Channel 池**：`MQManager` 池化 Channel，轮询分发

---

## 三、环境与依赖

### 操作系统

- Ubuntu 22.04 LTS

### 依赖列表

| 依赖 | 版本 | 用途 | 安装方式 |
|------|------|------|----------|
| g++ / cmake / make | - | C++17 编译工具链 | `sudo apt install g++ cmake make` |
| Muduo | - | Reactor 网络库 | [源码编译](https://blog.csdn.net/QIANGWEIYUAN/article/details/89023980) |
| Boost | 1.69+ | C++ 基础库 | [源码编译](https://blog.csdn.net/QIANGWEIYUAN/article/details/88792874) |
| OpenSSL | - | HTTPS/SSL 支持 | `sudo apt install libssl-dev` |
| MySQL | 5.7+ | 数据库 | `sudo apt install mysql-server` |
| mysqlcppconn | - | MySQL C++ 连接器 | `sudo apt install libmysqlcppconn-dev` |
| nlohmann/json | 3.x | JSON 解析 | `sudo apt install nlohmann-json3-dev` |
| OpenCV | 4.x | 图像处理 | `sudo apt install libopencv-dev` |
| ONNX Runtime | 1.20 | 模型推理引擎 | [下载预编译包](https://github.com/microsoft/onnxruntime) |
| RabbitMQ | - | 消息队列服务端 | `sudo apt install rabbitmq-server` |
| librabbitmq | - | RabbitMQ C 客户端库 | `sudo apt install librabbitmq-dev` |
| SimpleAmqpClient | - | RabbitMQ C++ 客户端 | 源码编译 |
| libcurl | - | HTTP 客户端（调用 AI API） | `sudo apt install libcurl4-openssl-dev` |

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
# 在项目根目录下
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 启动依赖服务

```bash
# 启动 MySQL
service mysql start

# 启动 RabbitMQ
rabbitmq-server -detached
rabbitmqctl start_app
rabbitmqctl status
```

### 启动服务

```bash
# 默认 80 端口（需 root）
cd build && ./http_server

# 指定端口（如 8080 不需 root）
cd build && ./http_server -p 8080
```

### 启动前配置

1. 确保 MySQL 服务已启动，创建对应数据库和表结构
2. 确保 RabbitMQ 服务已启动
3. 设置 AI API 环境变量：
   ```bash
   export DASHSCOPE_API_KEY="your_aliyun_api_key"
   export DOUBAO_API_KEY="your_doubao_api_key"
   ```
4. 配置百度语音 API 参数（`AISpeechProcessor` 构造时传入 clientId / clientSecret）
5. 配置 ONNX Runtime 模型路径（`ImageRecognizer` 构造时指定）
6. 配置 MCP 工具（编辑 `AIApps/ChatServer/resource/config.json`）

---

## 五、API 接口

### 用户接口

| 路径 | 方法 | Content-Type | 说明 |
|------|------|-------------|------|
| `/register` | POST | `application/json` | 用户注册 |
| `/login` | POST | `application/json` | 用户登录（返回 Session Cookie） |
| `/user/logout` | POST | `application/json` | 用户登出 |

### 页面接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/` | GET | 重定向到聊天入口页 |
| `/entry` | GET | 登录/注册页面 |
| `/chat` | GET | 聊天主页面 |
| `/menu` | GET | 功能选择菜单 |
| `/upload` | GET | 文件上传页面 |

### 聊天接口

| 路径 | 方法 | Content-Type | 说明 |
|------|------|-------------|------|
| `/chat/send` | POST | `application/json` | 发送消息，AI 回复 |
| `/chat/send-new-session` | POST | `application/json` | 创建新会话并发送消息 |
| `/chat/history` | POST | `application/json` | 查询历史消息 |
| `/chat/sessions` | GET | - | 获取用户会话列表 |
| `/chat/tts` | POST | `application/json` | 语音合成（文字转语音） |

### 上传接口

| 路径 | 方法 | 说明 |
|------|------|------|
| `/upload/send` | POST | 上传图片/文件并 AI 分析 |

### 请求示例

**登录：**
```bash
curl -X POST http://127.0.0.1:8080/login \
  -H "Content-Type: application/json" \
  -d '{"username":"user1","password":"123"}'
```

**发送消息（需携带 Session Cookie）：**
```bash
curl -X POST http://127.0.0.1:8080/chat/send \
  -H "Content-Type: application/json" \
  -H "Cookie: sessionId=xxx" \
  -d '{"sessionId":"xxx","content":"你好"}'
```

---

## 六、压测

项目提供 wrk Lua 脚本用于压力测试。

### 依赖

```bash
sudo apt install wrk
```

### 场景一：GET 页面压测（静态路由）

```bash
wrk -t4 -c200 -d30s http://127.0.0.1:8080/entry
```
root@059a558e872a:~/httpserver_vsersion2# wrk -t4 -c200 -d30s http://127.0.0.1:8080/entry
Running 30s test @ http://127.0.0.1:8080/entry
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.83ms    0.94ms  41.96ms   87.48%
    Req/Sec    17.88k     1.43k   20.91k    69.08%
  2134607 requests in 30.01s, 15.11GB read
Requests/sec:  71133.74
Transfer/sec:    515.78MB

**参考结果：**

| 指标 | 数值 |
|------|------|
| 线程/连接 | 4 threads / 200 connections |
| QPS | ~71133 req/s |
| 平均延迟 | ~2.83 ms |
| 吞吐 | ~515 MB/s |

### 场景二：GET 动态路由压测（Session 鉴权）

> **注意**：Session 存储在内存中，服务器重启后旧 sessionId 失效。需先登录获取新的 sessionId 再压测。

**获取 sessionId：**
```bash
curl -s -D - http://127.0.0.1:8080/login \
  -H "Content-Type: application/json" \
  -d '{"username":"test","password":"123"}' \
  | grep -o 'sessionId=[^;]*'
```

**使用获取到的 sessionId 进行压测：**
```bash
wrk -t4 -c200 -d30s -H "Cookie: sessionId=<你的sessionId>" http://127.0.0.1:8080/menu
```

**示例（sessionId 为示例值，请替换为实际值）：**
```bash
wrk -t4 -c200 -d30s -H "Cookie: sessionId=def6946a2be85e146fec64a9facc54fa" http://127.0.0.1:8080/menu
```
root@059a558e872a:~/httpserver_vsersion2# wrk -t4 -c200 -d30s -H "Cookie: sessionId=def6946a2be85e146fec64a9facc54fa" http://127.0.0.1:8080/menu
Running 30s test @ http://127.0.0.1:8080/menu
  4 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     3.72ms  771.59us  20.54ms   85.55%
    Req/Sec    13.51k   746.56    16.02k    70.75%
  1612893 requests in 30.00s, 12.44GB read
Requests/sec:  53754.69
Transfer/sec:    424.57MB

**参考结果：**

| 指标 | 数值 |
|------|------|
| 线程/连接 | 4 threads / 200 connections |
| QPS | ~53754 req/s |
| 平均延迟 | ~3.72 ms |
| 吞吐 | ~424.57 MB/s |

### 场景三：登录压测（POST + JSON）

使用 `login.lua` 脚本，模拟 POST 请求登录接口。脚本每次连接随机选取 `testuser1` ~ `testuser1000` 中的一个用户（密码统一为 `123456`）。

> 测试用户需提前用 `create_users.sh` 脚本注册。如未执行，可手动注册：
> ```bash
> curl -X POST http://127.0.0.1:8080/register \
>   -H "Content-Type: application/json" \
>   -d '{"username":"testuser1","password":"123456"}'
> ```

```bash
wrk -t4 -c100 -d30s -s login.lua http://127.0.0.1:8080/login
```
root@059a558e872a:~/httpserver_vsersion2# wrk -t4 -c100 -d30s -s login.lua http://127.0.0.1:8080/login
Running 30s test @ http://127.0.0.1:8080/login
  4 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    41.28ms    8.75ms 137.86ms   93.40%
    Req/Sec   608.45     60.29   757.00     72.83%
  72793 requests in 30.06s, 27.42MB read
Requests/sec:   2421.55
Transfer/sec:      0.91MB


**参考结果：**

| 指标 | 数值 |
|------|------|
| 线程/连接 | 4 threads / 100 connections |
| QPS | ~2421 req/s |
| 平均延迟 | ~41,2 ms |
| 吞吐 | ~0.91 MB/s |

---

## 七、项目总结

- **HttpServer** 是基于 C++17 从零构建的轻量级 HTTP/HTTPS 服务框架，实现了路由分发、中间件链、Session 管理、SSL 加密等核心机制。
- 框架基于 Muduo 的 Reactor 多线程模型，单机 GET 请求 QPS 可达 **6 万+**。
- **AI Chat 应用** 在框架之上集成了多模型 AI 对话、语音交互、图像识别等完整功能栈。
- 综合运用 **策略模式、工厂模式、单例模式、连接池模式** 等设计模式，代码结构清晰，扩展性强。
- 通过 **RabbitMQ 消息队列** 实现消息异步持久化，解耦业务逻辑与数据库操作。
- 本项目涵盖网络编程、多线程、设计模式、数据库、消息队列、AI 集成等多方面知识点，适合作为 C++ 后端开发的学习与面试项目。