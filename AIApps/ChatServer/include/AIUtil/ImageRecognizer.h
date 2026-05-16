#pragma once
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <iostream>

// 该文件定义了图像识别类，使用 ONNX Runtime 推理引擎和 OpenCV 进行图像分类。

class ImageRecognizer {
public:
    // 构造函数：加载 ONNX 模型和标签文件
    // 参数 model_path: ONNX 模型文件路径
    // 参数 label_path: 类别标签文件路径（默认 ImageNet 标签）
    explicit ImageRecognizer(const std::string& model_path,
        const std::string& label_path = "/root/imagenet_classes.txt");

    // 从图像文件进行预测
    // 参数 image_path: 图像文件路径
    // 返回: 预测的类别名称字符串
    std::string PredictFromFile(const std::string& image_path);

    // 从内存缓冲区进行预测（用于网络传输的图像数据）
    // 参数 image_data: 图像数据的字节数组
    // 返回: 预测的类别名称字符串
    std::string PredictFromBuffer(const std::vector<unsigned char>& image_data);

    // 从 OpenCV Mat 对象进行预测
    // 参数 img: OpenCV 图像矩阵
    // 返回: 预测的类别名称字符串
    std::string PredictFromMat(const cv::Mat& img);

private:
    Ort::Env env;  // ONNX Runtime 环境对象
    std::unique_ptr<Ort::Session> session;  // ONNX 会话，用于模型推理
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;  // 内存分配器

    std::string input_name;   // 模型输入节点名称
    std::string output_name;  // 模型输出节点名称
    std::vector<int64_t> input_shape;  // 输入张量形状
    int input_height{}, input_width{};  // 模型期望的输入图像尺寸

    std::vector<std::string> labels;  // 类别标签列表

    // 私有方法：加载类别标签文件
    // 参数 label_path: 标签文件路径
    void LoadLabels(const std::string& label_path);
};
