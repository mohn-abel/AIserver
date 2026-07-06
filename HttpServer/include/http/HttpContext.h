#pragma once

#include <iostream>

#include <muduo/net/TcpServer.h> // 使用TCP

#include "HttpRequest.h"
// 当前文件定义了解析请求并封装为request结构体
namespace http
{

class HttpContext 
{
public:
    // 四种状态机
    enum HttpRequestParseState
    {
        kExpectRequestLine, // 解析请求行
        kExpectHeaders, // 解析请求头
        kExpectBody, // 解析请求体
        kGotAll, // 解析完成
    };
    
    HttpContext()
    : state_(kExpectRequestLine) // 初始化状态为解析请求行
    {}
    // 从缓冲区读取字节流并解析请求
    bool parseRequest(muduo::net::Buffer* buf, muduo::Timestamp receiveTime);
    // 状态切换为解析完成
    bool gotAll() const 
    { return state_ == kGotAll;  }
    // 重置解析状态
    void reset()
    {
        state_ = kExpectRequestLine;
        HttpRequest dummyData;
        request_.swap(dummyData); // 清空请求
    }
    // 
    const HttpRequest& request() const
    { return request_;}

    HttpRequest& request()
    { return request_;}

private:
    // 解析请求行
    bool processRequestLine(const char* begin, const char* end);
private:
    HttpRequestParseState state_; // 解析状态机
    HttpRequest           request_; // 请求结构体
};

} // namespace http