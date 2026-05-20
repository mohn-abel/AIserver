#pragma once

#include <muduo/net/TcpServer.h>

namespace http
{

class HttpResponse 
{
public:
    enum HttpStatusCode
    {
        kUnknown,
        k200Ok = 200,
        k204NoContent = 204,
        k301MovedPermanently = 301,
        k400BadRequest = 400,
        k401Unauthorized = 401,
        k403Forbidden = 403,
        k404NotFound = 404,
        k409Conflict = 409,
        k500InternalServerError = 500,
    };

    HttpResponse(bool close = true)
        : statusCode_(kUnknown)
        , closeConnection_(close)
    {}

    void setVersion(std::string version)
    { httpVersion_ = version; }
    void setStatusCode(HttpStatusCode code)
    { statusCode_ = code; }

    HttpStatusCode getStatusCode() const
    { return statusCode_; }

    void setStatusMessage(const std::string message)
    { statusMessage_ = message; }

    void setCloseConnection(bool on)
    { closeConnection_ = on; }

    bool closeConnection() const
    { return closeConnection_; }
    
    void setContentType(const std::string& contentType)
    { addHeader("Content-Type", contentType); }

    void setContentLength(uint64_t length)
    { addHeader("Content-Length", std::to_string(length)); }

    void addHeader(const std::string& key, const std::string& value)
    { headers_[key] = value; }
    
    void setBody(const std::string& body)
    { 
        body_ = body;
        // body_ += "\0";
    }

    void setStatusLine(const std::string& version,
                         HttpStatusCode statusCode,
                         const std::string& statusMessage);

    void setErrorHeader(){}

    void appendToBuffer(muduo::net::Buffer* outputBuf) const;

    // ---- 异步响应支持 ----
    void setConnection(const muduo::net::TcpConnectionPtr& conn) { conn_ = conn; }
    muduo::net::TcpConnectionPtr getConnection() const { return conn_; }

    void setDeferred() { deferred_ = true; }
    bool isDeferred() const { return deferred_; }

    // 在 I/O 线程中发送 JSON 响应的便捷方法
    static void sendJsonResponse(const muduo::net::TcpConnectionPtr& conn,
                                  const std::string& body,
                                  const std::string& version = "HTTP/1.1",
                                  bool close = false);
    
    // 流式响应的辅助方法
    static void sendSSEHeaders(const muduo::net::TcpConnectionPtr& conn);
    static void sendSSEChunk(const muduo::net::TcpConnectionPtr& conn, const std::string& data);
    static void sendSSEError(const muduo::net::TcpConnectionPtr& conn, const std::string& errorJson);
    static void sendSSEEnd(const muduo::net::TcpConnectionPtr& conn);

    
private:
    std::string                        httpVersion_; 
    HttpStatusCode                     statusCode_;
    std::string                        statusMessage_;
    bool                               closeConnection_;
    std::map<std::string, std::string> headers_;
    std::string                        body_;
    bool                               isFile_;
    // 异步响应支持
    bool                               deferred_ = false;
    muduo::net::TcpConnectionPtr       conn_;
};

} // namespace http