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

        auto body = req.getBody();
        if (!body.empty()) {
            auto j = json::parse(body);
            if (j.contains("question")) userQuestion = j["question"];


            modelType = j.contains("modelType") ? j["modelType"].get<std::string>() : StrategyFactory::instance().getDefaultModel();
        }

        // 生成会话ID（轻量操作，可在I/O线程完成）
        AISessionIdGenerator generator;
        std::string sessionId = generator.generate();
        std::cout<<"生成的sessionId为 "<<sessionId<<std::endl;

        // 标记异步处理
        resp->setDeferred();
        auto conn = resp->getConnection();

        server_->getBusinessPool()->enqueue([this, conn, userId, username, sessionId, userQuestion, modelType]() {
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

                std::string aiInformation = AIHelperPtr->chat(userId, username, sessionId, userQuestion, modelType);
                json successResp;
                successResp["success"] = true;
                successResp["Information"] = aiInformation;
                successResp["sessionId"] = sessionId;

                std::string successBody = successResp.dump(4);

                // 回到I/O线程发送响应
                conn->getLoop()->runInLoop([conn, successBody]() {
                    http::HttpResponse::sendJsonResponse(conn, successBody, "HTTP/1.1", false);
                });
            } catch (const std::exception& e) {
                LOG_ERROR << "ChatCreateAndSendHandler async error: " << e.what();
                std::string failureBody = json{{"status", "error"}, {"message", e.what()}}.dump(4);
                conn->getLoop()->runInLoop([conn, failureBody]() {
                    http::HttpResponse::sendJsonResponse(conn, failureBody, "HTTP/1.1", true);
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