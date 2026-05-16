#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <cppconn/connection.h>
#include <cppconn/prepared_statement.h>
#include <cppconn/resultset.h>
#include <mysql_driver.h>
#include <mysql/mysql.h>
#include <muduo/base/Logging.h>
#include "DbException.h"

namespace http
{
    namespace db
    {

        // 自定义 deleter：确保 PreparedStatement 在析构时调用 close() 释放 MySQL 服务端资源
        struct StmtDeleter {
            void operator()(sql::PreparedStatement* stmt) const {
                if (stmt) {
                    stmt->close();
                    delete stmt;
                }
            }
        };
        // 使用 unique_ptr 管理 PreparedStatement，确保异常安全和资源正确释放
        using StmtPtr = std::unique_ptr<sql::PreparedStatement, StmtDeleter>;

        // 查询结果持有者：同时持有 PreparedStatement 和 ResultSet
        // 确保 ResultSet 使用期间 PreparedStatement 不被释放
        class DbQueryResult
        {
        public:
            DbQueryResult(StmtPtr stmt, sql::ResultSet* rs)
                : stmt_(std::move(stmt)), rs_(rs) {}

            ~DbQueryResult()
            {
                if (rs_ != nullptr)
                {
                    delete rs_;
                    rs_ = nullptr;
                }
            }

            // 禁止拷贝
            DbQueryResult(const DbQueryResult&) = delete;
            DbQueryResult& operator=(const DbQueryResult&) = delete;

            // 允许移动
            DbQueryResult(DbQueryResult&& other) noexcept
                : stmt_(std::move(other.stmt_)), rs_(other.rs_)
            {
                other.rs_ = nullptr;
            }

            DbQueryResult& operator=(DbQueryResult&& other) noexcept
            {
                if (this != &other)
                {
                    if (rs_ != nullptr)
                    {
                        delete rs_;
                    }
                    stmt_ = std::move(other.stmt_);
                    rs_ = other.rs_;
                    other.rs_ = nullptr;
                }
                return *this;
            }

            sql::ResultSet* get() const { return rs_; }
            sql::ResultSet* operator->() const { return rs_; }
            explicit operator bool() const { return rs_ != nullptr; }

        private:
            StmtPtr stmt_;           // 持有 PreparedStatement，析构时自动 close
            sql::ResultSet* rs_;     // ResultSet 生命周期依赖 stmt_
        };

        class DbConnection
        {
        public:
            DbConnection(const std::string& host,
                const std::string& user,
                const std::string& password,
                const std::string& database);
            ~DbConnection();

            // 禁止拷贝
            DbConnection(const DbConnection&) = delete;
            DbConnection& operator=(const DbConnection&) = delete;

            bool isValid();
            void reconnect();
            void cleanup();

            // 返回 DbQueryResult，调用者持有结果期间 stmt 保持存活
            // 调用者释放 DbQueryResult 时自动释放 PreparedStatement
            template<typename... Args>
            DbQueryResult executeQuery(const std::string& sql, Args&&... args)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // 创建新的 PreparedStatement
                StmtPtr stmt(conn_->prepareStatement(sql));
                bindParams(stmt.get(), 1, std::forward<Args>(args)...);
                sql::ResultSet* result = stmt->executeQuery();
                // 将 stmt 所有权转移给 DbQueryResult
                return DbQueryResult(std::move(stmt), result);
            }

            template<typename... Args>
            int executeUpdate(const std::string& sql, Args&&... args)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // 创建新的 PreparedStatement，RAII 保证函数结束时 close()
                StmtPtr stmt(conn_->prepareStatement(sql));
                bindParams(stmt.get(), 1, std::forward<Args>(args)...);
                int ret = stmt->executeUpdate();
                // stmt 在此处析构，自动 close 释放服务端资源
                return ret;
            }

            bool ping();  // 检测连接是否有效
        private:
            // 辅助函数：递归终止条件
            void bindParams(sql::PreparedStatement*, int) {}

            // 辅助函数：绑定参数
            template<typename T, typename... Args>
            void bindParams(sql::PreparedStatement* stmt, int index,
                T&& value, Args&&... args)
            {
                stmt->setString(index, std::to_string(std::forward<T>(value)));
                bindParams(stmt, index + 1, std::forward<Args>(args)...);
            }

            // 特化 string 类型的参数绑定
            template<typename... Args>
            void bindParams(sql::PreparedStatement* stmt, int index,
                const std::string& value, Args&&... args)
            {
                stmt->setString(index, value);
                bindParams(stmt, index + 1, std::forward<Args>(args)...);
            }

            // 内部不加锁版本，供已持锁的函数调用
            void reconnectInternal();

        private:
            std::shared_ptr<sql::Connection> conn_;
            std::string                      host_;
            std::string                      user_;
            std::string                      password_;
            std::string                      database_;
            std::mutex                       mutex_;
        };

    } // namespace db
} // namespace http
