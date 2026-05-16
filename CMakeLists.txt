cmake_minimum_required(VERSION 3.10)
project(http_server)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 OpenSSL 和 CURL
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# 设置导出编译命令（供 VS Code IntelliSense 使用）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 添加头文件路径
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/HttpServer/include
    ${PROJECT_SOURCE_DIR}/AIApps/ChatServer/include
    /usr/include/mysql-cppconn-8
    /usr/include/mysql
    /usr/include/x86_64-linux-gnu
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}

    # OpenCV 头文件路径（本地系统安装的）
    /usr/include/opencv4

    # ONNX Runtime 本地路径
    ${PROJECT_SOURCE_DIR}/../onnxruntime-linux-x64-1.20.0/include

    # SimpleAmqpClient 头文件
    /usr/local/include
)

# 查找必要的库
find_library(MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn mysqlcppconn8
    PATHS /usr/lib /usr/lib64 /usr/local/lib
)

# 添加源文件
file(GLOB_RECURSE HTTP_SERVER_SRC
    "${PROJECT_SOURCE_DIR}/HttpServer/src/*.cpp"
)

file(GLOB_RECURSE GOMOKU_SERVER_SRC
    "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/*.cpp"
)

# 主程序文件
set(MAIN_SRC "${PROJECT_SOURCE_DIR}/AIApps/ChatServer/src/main.cpp")

# 添加可执行文件
add_executable(http_server
    ${MAIN_SRC}
    ${HTTP_SERVER_SRC}
    ${GOMOKU_SERVER_SRC}
)

# 链接库
target_link_libraries(http_server
    pthread
    muduo_net
    muduo_base
    mysqlcppconn
    mysqlclient
    ssl
    crypto
    ${CURL_LIBRARIES}
    ${OPENCV4_LIBRARIES} 
    # OpenCV 本地库
    /usr/lib/x86_64-linux-gnu/libopencv_core.so
    /usr/lib/x86_64-linux-gnu/libopencv_imgproc.so
    /usr/lib/x86_64-linux-gnu/libopencv_highgui.so
    /usr/lib/x86_64-linux-gnu/libopencv_imgcodecs.so
    /usr/lib/x86_64-linux-gnu/libopencv_videoio.so
    /usr/lib/x86_64-linux-gnu/libopencv_dnn.so

    # ONNX Runtime 本地库
    ${PROJECT_SOURCE_DIR}/../onnxruntime-linux-x64-1.20.0/lib/libonnxruntime.so

    # SimpleAmqpClient / RabbitMQ
    SimpleAmqpClient
    rabbitmq
)

# 如果库不在默认路径，添加链接目录
link_directories(/usr/local/lib)

# 打印调试信息
message(STATUS "Include directories:")
get_property(dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
foreach(dir ${dirs})
    message(STATUS "  ${dir}")
endforeach()

message(STATUS "Main source file: ${MAIN_SRC}")
message(STATUS "HTTP Server sources: ${HTTP_SERVER_SRC}")
message(STATUS "Gomoku Server sources: ${GOMOKU_SERVER_SRC}")

