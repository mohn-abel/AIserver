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
        bool enableTools = true;

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];

            modelType = j.contains("modelType") ? j["modelType"].get<std::string>() : StrategyFactory::instance().getDefaultModel();
            enableTools = j.value("enableTools", true);
        }

        // 生成会话ID（轻量操作，可在I/O线程完成）
        AISessionIdGenerator generator;
        std::string sessionId = generator.generate();
        std::cout<<"生成的sessionId为 "<<sessionId<<std::endl;

        // 标记异步处理
        resp->setDeferred();
        auto conn = resp->getConnection();

        server_->getBusinessPool()->enqueue([this, conn, userId, username, sessionId, userQuestion, modelType, enableTools]() {
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
                // 统一走 SSE 流式响应，先发 sessionId
                http::HttpResponse::sendSSEHeaders(conn);
                json sessionInfo;
                sessionInfo["sessionId"] = sessionId;
                http::HttpResponse::sendSSEChunk(conn, sessionInfo.dump());

                AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType, enableTools,
                    [conn](const std::string& chunk) {
                        try {
                            if (!chunk.empty()) {
                                json chunkResp;
                                chunkResp["id"] = "chatcmpl-stream";
                                chunkResp["object"] = "chat.completion.chunk";
                                chunkResp["choices"] = json::array({{
                                    {"index", 0},
                                    {"delta", {{"content", chunk}}},
                                }});
                                std::string chunkBody = chunkResp.dump();

                                conn->getLoop()->runInLoop([conn, chunkBody]() {
                                    http::HttpResponse::sendSSEChunk(conn, chunkBody);
                                });
                            }
                        } catch (...) {
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
            } catch (const std::exception& e) {
                LOG_ERROR << "ChatCreateAndSendHandler async error: " << e.what();
                json errorChunkResp;
                errorChunkResp["error"] = {{"message", e.what()}};
                std::string errorChunkBody = errorChunkResp.dump();

                conn->getLoop()->runInLoop([conn, errorChunkBody]() {
                    http::HttpResponse::sendSSEError(conn, errorChunkBody);
                });
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