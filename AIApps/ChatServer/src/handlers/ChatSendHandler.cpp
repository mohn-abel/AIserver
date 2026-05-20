#include "../include/handlers/ChatSendHandler.h"


void ChatSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {
        // 登录验证
        auto session = server_->getSessionManager()->getSession(req, resp);
        LOG_INFO << "session->getValue(\"isLoggedIn\") = " << session->getValue("isLoggedIn");
        if (session->getValue("isLoggedIn") != "true")
        {

            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = "Unauthorized";
            std::string errorBody = errorResp.dump(4);

            server_->packageResp(req.getVersion(), http::HttpResponse::k401Unauthorized,
                "Unauthorized", true, "application/json", errorBody.size(),
                errorBody, resp);
            return;
        }

        // 获取基础信息
        int userId = std::stoi(session->getValue("userId"));
        std::string username = session->getValue("username");

        std::string userQuestion;
        std::string modelType;
        std::string sessionId;
        bool stream;
        // 获取用户问题和模型类型
        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];
            if (j.contains("sessionId")) sessionId = j["sessionId"];
            modelType = j.contains("modelType") ? j["modelType"].get<std::string>() : StrategyFactory::instance().getDefaultModel();
            stream = j.contains("stream") ? j["stream"].get<bool>() : false;
        }

        // 标记异步处理，不在 muduo I/O 线程中阻塞
        resp->setDeferred();
        auto conn = resp->getConnection();

        // 捕获必要数据（值拷贝，确保生命周期安全）
        server_->getBusinessPool()->enqueue([this, conn, userId, username, sessionId, userQuestion, modelType, stream]() {
            try {
                // 获取当前会话的实例
                std::shared_ptr<AIHelper> AIHelperPtr; // 创建一个实例
                {
                    std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

                    auto& userSessions = server_->chatInformation[userId];

                    if (userSessions.find(sessionId) == userSessions.end()) {

                        userSessions.emplace( 
                            sessionId,
                            std::make_shared<AIHelper>()
                        );
                    }
                    AIHelperPtr = userSessions[sessionId]; // 将实例绑定至这个会话id上
                }
                if(stream){
                    http::HttpResponse::sendSSEHeaders(conn);
                    AIHelperPtr->chatStreaming(userId, username, sessionId, userQuestion, modelType, [conn](const std::string& chunk){
                        try{
                            if(!chunk.empty()){
                                json chunkResp;
                                chunkResp["id"] = "chatcmpl-stream";
                                chunkResp["object"] = "chat.completion.chunk";
                                chunkResp["choices"] = json::array({{
                                    {"index", 0},
                                    {"delta", {{"content", chunk}}},
                                }}); // 模拟 OpenAI 的流式响应格式
                                std::string chunkBody = chunkResp.dump();

                                conn->getLoop()->runInLoop([conn, chunkBody]() {
                                    http::HttpResponse::sendSSEChunk(conn, chunkBody);
                                });
                            }
                        }catch(...){
                            json errorChunkResp;
                            errorChunkResp["error"] = {{"message", "Error in streaming response"}};
                            std::string errorChunkBody = errorChunkResp.dump();

                            conn->getLoop()->runInLoop([conn, errorChunkBody]() {
                                http::HttpResponse::sendSSEError(conn, errorChunkBody);
                            });
                        }
                    });
                    conn->getLoop()->runInLoop([conn]() {
                        http::HttpResponse::sendSSEEnd(conn);
                    });

                }else{
                    std::string aiInformation=AIHelperPtr->chat(userId, username,sessionId, userQuestion, modelType);
                    json successResp;
                    successResp["success"] = true;
                    successResp["Information"] = aiInformation;
                    std::string successBody = successResp.dump(4);

                    // 在 muduo I/O 线程中发送响应
                    conn->getLoop()->runInLoop([conn, successBody]() {
                        http::HttpResponse::sendJsonResponse(conn, successBody, "HTTP/1.1", false);
                    });
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "ChatSendHandler async error: " << e.what();
                if(stream){
                    json errorChunkResp;
                    errorChunkResp["error"] = {{"message", e.what()}};
                    std::string errorChunkBody = errorChunkResp.dump();

                    conn->getLoop()->runInLoop([conn, errorChunkBody]() {
                        http::HttpResponse::sendSSEError(conn, errorChunkBody);
                    });
                }else{
                    std::string failureBody = json{{"status", "error"}, {"message", e.what()}}.dump(4);
                    conn->getLoop()->runInLoop([conn, failureBody]() {
                        http::HttpResponse::sendJsonResponse(conn, failureBody, "HTTP/1.1", true);
                    });
                }
            }
        });

        return;
    }
    catch (const std::exception& e)
    {

        json failureResp;
        failureResp["status"] = "error";
        failureResp["message"] = e.what();
        std::string failureBody = failureResp.dump(4);
        resp->setStatusLine(req.getVersion(), http::HttpResponse::k400BadRequest, "Bad Request");
        resp->setCloseConnection(true);
        resp->setContentType("application/json");
        resp->setContentLength(failureBody.size());
        resp->setBody(failureBody);
    }
}