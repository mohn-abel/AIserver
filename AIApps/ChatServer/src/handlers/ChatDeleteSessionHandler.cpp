#include "../include/handlers/ChatDeleteSessionHandler.h"

void ChatDeleteSessionHandler::handle(const http::HttpRequest& req, http::HttpResponse* resp) {
    // Implementation for handling session deletion
        try {
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
            // 获取sessionId
            int userId = std::stoi(session->getValue("userId"));
            std::string sessionId;
            auto body = req.getBody();
            if(!body.empty()){
                auto j = json::parse(body);
                if(j.contains("sessionId")){sessionId = j["sessionId"];}
            }
            if(sessionId.empty()){
                json errorResp;
                errorResp["status"] = "error";
                errorResp["message"] = "Missing sessionId";
                std::string errorBody = errorResp.dump(4);
                server_->packageResp(req.getVersion(), http::HttpResponse::k400BadRequest,
                    "Bad Request", true, "application/json", errorBody.size(),
                    errorBody, resp);
                return;
            }
            // 删除会话相关消息
            std::string sql = "DELETE FROM chat_message WHERE id = "
                        + std::to_string(userId) + " AND session_id = '" + sessionId + "'";
            server_->mysqlUtil_.executeUpdate(sql);
            {
                // 删除内存中的聊天信息
                std::lock_guard<std::mutex> lock(server_->mutexForChatInformation);
                auto userIt = server_->chatInformation.find(userId);
                if(userIt != server_->chatInformation.end()){
                    auto& chatInformation = userIt->second;
                    chatInformation.erase(sessionId);
                }
            }
            {
                // 删除内存中的会话ID
                std::lock_guard<std::mutex> lock(server_->mutexForSessionsId);
                auto it = server_->sessionsIdsMap.find(userId);
                if(it != server_->sessionsIdsMap.end()){
                    auto& session = it->second;
                    session.erase(std::remove(session.begin(), session.end(), sessionId), session.end());
                }
            }

            json ok;
            ok["success"] = true;
            std::string okBody = ok.dump(4);
            server_->packageResp(req.getVersion(), http::HttpResponse::k200Ok,
                "OK", false, "application/json", okBody.size(),
                okBody, resp);
        }catch(std::exception& e){
            json errorResp;
            errorResp["status"] = "error";
            errorResp["message"] = e.what();
            std::string errorBody = errorResp.dump(4);
            server_->packageResp(req.getVersion(), http::HttpResponse::k400BadRequest,
                "Bad Request", true, "application/json", errorBody.size(),
                errorBody, resp);
        }
}