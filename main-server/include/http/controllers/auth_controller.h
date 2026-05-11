#pragma once
// ============================================================================
// auth_controller.h — POST /auth/register, POST /auth/login, POST /auth/logout
// ============================================================================

#include "http/http_server.h"
#include "service/user_service.h"

namespace factory::http {

class AuthController {
public:
    AuthController(HttpServer& server, UserService& service);
    void register_routes();

private:
    HttpServer&  server_;
    UserService& service_;
};

} // namespace factory::http
