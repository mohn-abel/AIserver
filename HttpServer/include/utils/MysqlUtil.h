#pragma once
#include "db/DbConnectionPool.h"

#include <string>
#include <memory>

namespace http
{

    // 查询结果包装类：同时持有数据库连接和查询结果
    // 确保连接在 ResultSet 使用期间不被归还到连接池
    class QueryResult
    {
    public:
        QueryResult(std::shared_ptr<db::DbConnection> conn, db::DbQueryResult result)
            : conn_(std::move(conn)), result_(std::move(result)) {}

        ~QueryResult() = default;

        // 禁止拷贝
        QueryResult(const QueryResult&) = delete;
        QueryResult& operator=(const QueryResult&) = delete;

        // 允许移动
        QueryResult(QueryResult&& other) noexcept
            : conn_(std::move(other.conn_)), result_(std::move(other.result_)) {}

        QueryResult& operator=(QueryResult&& other) noexcept
        {
            if (this != &other)
            {
                conn_ = std::move(other.conn_);
                result_ = std::move(other.result_);
            }
            return *this;
        }

        // 提供 ResultSet 的访问接口
        sql::ResultSet* get() const { return result_.get(); }
        sql::ResultSet* operator->() const { return result_.get(); }
        explicit operator bool() const { return static_cast<bool>(result_); }

    private:
        std::shared_ptr<db::DbConnection> conn_;  // 持有连接，防止提前归还
        db::DbQueryResult result_;                 // 持有 stmt + ResultSet
    };

    class MysqlUtil
    {
    public:
        static void init(const std::string& host, const std::string& user,
            const std::string& password, const std::string& database,
            size_t poolSize = 10)
        {
            http::db::DbConnectionPool::getInstance().init(
                host, user, password, database, poolSize);
        }

        template<typename... Args>
        QueryResult executeQuery(const std::string& sql, Args&&... args)
        {
            auto conn = http::db::DbConnectionPool::getInstance().getConnection();
            auto result = conn->executeQuery(sql, std::forward<Args>(args)...);
            return QueryResult(std::move(conn), std::move(result));
        }

        template<typename... Args>
        int executeUpdate(const std::string& sql, Args&&... args)
        {
            auto conn = http::db::DbConnectionPool::getInstance().getConnection();
            return conn->executeUpdate(sql, std::forward<Args>(args)...);
        }
    };
    

} // namespace http
