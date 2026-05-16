#pragma once
#include <iostream>
#include <string>
#include <curl/curl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <chrono>


#include "../../../../HttpServer/include/utils/JsonUtil.h"
#include"base64.h"

// 该文件定义了语音处理类，用于封装百度语音 API 的语音识别和合成功能。

// 语音处理类：集成语音识别和合成功能
class AISpeechProcessor {
public:
    // 构造函数：初始化百度语音 API 的认证信息
    // 参数 clientId: 百度 API 的 Client ID
    // 参数 clientSecret: 百度 API 的 Client Secret
    // 参数 cuid: 用户唯一标识符（默认值可替换为实际标识）
    AISpeechProcessor(const std::string& clientId,
                      const std::string& clientSecret,
                      const std::string& cuid = "RZjSQGzNaA8EFWf6rvuHEKDh9i4XJIV9") //用户唯一标识，需要更改成自身标识
        : client_id_(clientId), client_secret_(clientSecret), cuid_(cuid)
    {
        // 构造函数中获取访问令牌
        token_ = getAccessToken();
    }

    // 语音识别：将音频数据转换为文本
    // 参数 speechData: 音频数据的二进制内容（Base64 编码前）
    // 参数 format: 音频格式，默认 "pcm"
    // 参数 rate: 采样率，默认 16000
    // 参数 channel: 声道数，默认 1（单声道）
    // 返回: 识别出的文本字符串
    std::string recognize(const std::string& speechData,
                          const std::string& format = "pcm",
                          int rate = 16000,
                          int channel = 1);

    // 语音合成：将文本转换为音频数据
    // 参数 text: 要合成的文本内容
    // 参数 format: 输出音频格式，默认 "mp3-16k"
    // 参数 lang: 语言，默认 "zh"（中文）
    // 参数 speed: 语速（1-9），默认 5
    // 参数 pitch: 音调（1-9），默认 5
    // 参数 volume: 音量（1-9），默认 5
    // 返回: 合成的音频数据（Base64 编码）
    std::string synthesize(const std::string& text,
                           const std::string& format = "mp3-16k",
                           const std::string& lang = "zh",
                           int speed = 5,
                           int pitch = 5,
                           int volume = 5);


private:
    std::string client_id_;      // 百度 API Client ID
    std::string client_secret_;  // 百度 API Client Secret
    std::string cuid_;           // 用户唯一标识符
    std::string token_;          // 访问令牌（通过 API 获取）

    // 获取百度 API 的访问令牌（私有方法）
    // 返回: 有效的访问令牌字符串
    std::string getAccessToken();

};
