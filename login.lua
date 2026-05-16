-- login.lua — 登录接口压测脚本
-- 每次连接随机选取 testuser1..1000 中的一个用户进行 POST /login
-- 密码统一为 123456（与 create_users.sh 注册的密码一致）

local login_req = ""

init = function(args)
    math.randomseed(os.time() + math.floor(os.clock() * 1000000))
    local user_id = math.random(1, 1000)
    local login_body = '{"username": "testuser' .. user_id .. '", "password": "123456"}'
    login_req = wrk.format("POST", "/login",
        {["Content-Type"] = "application/json", ["Connection"] = "Keep-Alive"},
        login_body)
end

request = function()
    return login_req
end
