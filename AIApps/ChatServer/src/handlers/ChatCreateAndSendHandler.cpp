#include "../include/handlers/ChatCreateAndSendHandler.h"


void ChatCreateAndSendHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp)
{
    try
    {

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


        int userId = std::stoi(session->getValue("userId"));
        std::string username = session->getValue("username");

        std::string userQuestion;
        std::string modelType;
        bool stream;

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];
            stream = j.contains("stream") ? j["stream"].get<bool>() : false;

            modelType = j.contains("modelType") ? j["modelType"].get<std::string>() : StrategyFactory::instance().getDefaultModel();
        }

        // 生成会话ID（轻量操作，可在I/O线程完成）
        AISessionIdGenerator generator;
        std::string sessionId = generator.generate();
        std::cout<<"生成的sessionId为 "<<sessionId<<std::endl;

        // 标记异步处理
        resp->setDeferred();
        auto conn = resp->getConnection();

        server_->getBusinessPool()->enqueue([this, conn, userId, username, sessionId, userQuestion, modelType, stream]() {
            try {
                std::shared_ptr<AIHelper> AIHelperPtr;
                {
                    std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);

                    auto& userSessions = server_->chatInformation[userId];

                    if (userSessions.find(sessionId) == userSessions.end()) {
                        userSessions.emplace( 
                            sessionId,
                            std::make_shared<AIHelper>()
                        );
                        server_->sessionsIdsMap[userId].push_back(sessionId);
                    }
                    AIHelperPtr = userSessions[sessionId];
                }
                if(stream){
                    http::HttpResponse::sendSSEHeaders(conn);
                    // 先发 sessionId，客户端用它在本地建立会话
                    json sessionInfo;
                    sessionInfo["sessionId"] = sessionId;
                    http::HttpResponse::sendSSEChunk(conn, sessionInfo.dump());
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
                LOG_ERROR << "ChatCreateAndSendHandler async error: " << e.what();
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