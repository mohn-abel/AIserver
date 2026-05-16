#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <muduo/net/TcpServer.h>
#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>

#include"../include/ChatServer.h"
#include"../include/AIUtil/AIFactory.h"

const std::string RABBITMQ_HOST = "localhost"; // 本地rabbitmq服务器
const std::string QUEUE_NAME = "sql_queue"; // 监听的消息队列
const int THREAD_NUM = 2; // 线程数

// 自定义消息异步入库函数
void executeMysql(const std::string sql) {
    http::MysqlUtil mysqlUtil_;
    mysqlUtil_.executeUpdate(sql);
}


int main(int argc, char* argv[]) {
	LOG_INFO << "pid = " << getpid();
	std::string serverName = "ChatServer";
	int port = 80;
    // 
    int opt;
    const char* str = "p:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            port = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }
    muduo::Logger::setLogLevel(muduo::Logger::WARN);
    ChatServer server(port, serverName);
    server.setThreadNum(4);
    // 加载 LLM 模型配置（在服务器启动前注册所有可用模型）
    StrategyFactory::instance().loadFromConfig("../AIApps/ChatServer/resource/model_config.json");

    //һҪ˯߲ChatServer캯гʼֿ
    std::this_thread::sleep_for(std::chrono::seconds(2));
    //ʼchat_messagechatInformation
    server.initChatMessage();    

    // ʼѶе̳߳أ봦̶߳ͳһĴ߼
    //ҪЭ̿ÿ߳ͬôҲٷһ࣬߳ȡеĺִв
    RabbitMQThreadPool pool(RABBITMQ_HOST, QUEUE_NAME, THREAD_NUM, executeMysql);
    pool.start();

    server.start();
}
