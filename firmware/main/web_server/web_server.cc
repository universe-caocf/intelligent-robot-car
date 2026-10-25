#include "web_server.h"
#include <esp_log.h>
#include <cstring>
#include <cJSON.h>
#include <esp_system.h>
#include <stdlib.h>
#include <cmath>
#include <cerrno>
#include <climits>
#include <stdexcept>
#include "application.h"

static const char *TAG = "WebServer";

namespace {
// HTTP 请求可能分段到达；必须读完整并补零，不能把未终止的缓冲区交给 JSON/表单解析器。
bool ReadBody(httpd_req_t* req, char* buffer, size_t capacity) {
    // 防止其他网页用跨域表单驱动车轮；无Origin的本地CLI仍兼容，这不是身份鉴权。
    size_t origin_size = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_size != 0) {
        char origin[160], host[128];
        size_t host_size = httpd_req_get_hdr_value_len(req, "Host");
        if (origin_size >= sizeof(origin) || host_size == 0 || host_size >= sizeof(host) ||
            httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
            httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
            std::string(origin) != std::string("http://") + host) {
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-origin control is not allowed");
            return false;
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    if (req->content_len == 0 || req->content_len >= capacity) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty or oversized body");
        return false;
    }
    size_t used = 0;
    while (used < req->content_len) {
        int received = httpd_req_recv(req, buffer + used, req->content_len - used);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body");
            return false;
        }
        used += received;
    }
    buffer[used] = '\0';
    if (memchr(buffer, '\0', used)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return false;
    }
    return true;
}

cJSON* ParseObject(const char* body) {
    cJSON* root = cJSON_ParseWithOpts(body, nullptr, true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return nullptr; }
    return root;
}

bool ReadInteger(const cJSON* root, const char* key, int& value, bool required = true) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) return !required;
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
            item->valuedouble < INT_MIN || item->valuedouble > INT_MAX ||
            std::trunc(item->valuedouble) != item->valuedouble) return false;
    value = item->valueint;
    return true;
}

struct ConfigField { const char* key; int car::MotorActionConfig::*member; };
constexpr ConfigField kConfigFields[] = {
    {"forward_ms", &car::MotorActionConfig::forward_duration_ms},
    {"backward_ms", &car::MotorActionConfig::backward_duration_ms},
    {"left_turn_ms", &car::MotorActionConfig::left_turn_duration_ms},
    {"right_turn_ms", &car::MotorActionConfig::right_turn_duration_ms},
    {"spin_ms", &car::MotorActionConfig::spin_duration_ms},
    {"wiggle_ms", &car::MotorActionConfig::wiggle_duration_ms},
    {"dance_ms", &car::MotorActionConfig::dance_duration_ms},
    {"quick_fwd_ms", &car::MotorActionConfig::quick_forward_duration_ms},
    {"quick_bwd_ms", &car::MotorActionConfig::quick_backward_duration_ms},
    {"def_speed_pct", &car::MotorActionConfig::default_speed_percent}
};

bool ParseIntegerText(const std::string& text, int& value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long number = strtol(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || *end != '\0' || number < INT_MIN || number > INT_MAX) return false;
    value = static_cast<int>(number);
    return true;
}

esp_err_t SendMotorResult(httpd_req_t* req, bool accepted) {
    if (!accepted) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"status\":\"rejected\",\"error\":\"Check network and /api/motor/status\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"status\":\"ok\",\"accepted\":true,\"physical_completion_verified\":false}", HTTPD_RESP_USE_STRLEN);
}
}  // namespace

WebServer::WebServer()
    : server_handle_(nullptr) {
}

WebServer::~WebServer() {
    Stop();
}

bool WebServer::Start(int port) {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (port < 1 || port > 65535) return false;
    if (server_handle_) return server_port_ == port;
    ESP_LOGI(TAG, "Starting web server on port %d", port);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 13;
    // 增加超时设置以更好地处理频繁请求
    config.recv_wait_timeout = 5;  // 接收超时5秒
    config.send_wait_timeout = 5;  // 发送超时5秒
    config.max_resp_headers = 8;   // 减少响应头数量

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return false;
    }

    const httpd_uri_t routes[] = {
        {"/api/turret/status", HTTP_GET, turret_status_handler, this},
        {"/api/turret/prepare", HTTP_POST, turret_prepare_handler, this},
        {"/api/turret/disarm", HTTP_POST, turret_disarm_handler, this},
        {"/", HTTP_GET, index_get_handler, this},
        {"/control", HTTP_POST, control_post_handler, this},
        {"/api/control", HTTP_POST, api_control_handler, this},
        {"/api/motor/action", HTTP_POST, api_motor_action_handler, this},
        {"/api/debug/motor_test", HTTP_POST, debug_motor_test_handler, this},
        {"/api/motor/status", HTTP_GET, api_motor_status_handler, this},
        {"/config", HTTP_GET, config_get_handler, this},
        {"/config", HTTP_POST, config_post_handler, this},
        {"/api/config", HTTP_GET, api_config_get_handler, this},
        {"/api/config", HTTP_POST, api_config_post_handler, this},
    };
    for (const auto& route : routes) {
        if (httpd_register_uri_handler(server_handle_, &route) != ESP_OK) {
            ESP_LOGE(TAG, "Cannot register %s; rolling back HTTP server", route.uri);
            httpd_stop(server_handle_); server_handle_ = nullptr; server_port_ = 0;
            return false;
        }
    }
    server_port_ = port;

    ESP_LOGI(TAG, "Web server started successfully");
    return true;
}

void WebServer::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (server_handle_) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        server_port_ = 0;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

void WebServer::SetMotorControlCallback(std::function<bool(int direction, int speed)> callback) {
    motor_control_callback_ = callback;
}

bool WebServer::InvokeMotorControl(int direction, int speed) {
    return motor_control_callback_ && motor_control_callback_(direction, speed);
}

void WebServer::SetMotorActionConfigCallback(std::function<MotorActionConfig()> get_callback,
                                           std::function<void(const MotorActionConfig&)> set_callback) {
    get_motor_config_callback_ = get_callback;
    set_motor_config_callback_ = set_callback;
}

void WebServer::SetEmotionCallback(std::function<void(const char* emotion)> callback) {
    emotion_callback_ = callback;
}

void WebServer::SetEmotion(const char* emotion) {
    if (emotion_callback_) {
        emotion_callback_(emotion);
    }
}

void WebServer::SetTurretCallbacks(std::function<std::string()> status, std::function<bool(int)> prepare,
                                  std::function<bool()> disarm) {
    turret_status_callback_ = std::move(status);
    turret_prepare_callback_ = std::move(prepare);
    turret_disarm_callback_ = std::move(disarm);
}

esp_err_t WebServer::turret_status_handler(httpd_req_t* req) {
    auto* server = static_cast<WebServer*>(req->user_ctx);
    if (!server->turret_status_callback_) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Turret callback unavailable");
    auto status = server->turret_status_callback_();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, status.c_str(), status.size());
}

esp_err_t WebServer::turret_prepare_handler(httpd_req_t* req) {
    auto* server = static_cast<WebServer*>(req->user_ctx);
    char content[100];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;
    cJSON* root = ParseObject(content);
    int count = 1;
    bool valid = root && ReadInteger(root, "count", count, false) && count >= 1 && count <= 3;
    cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "count must be an integer 1..3");
    if (!server->turret_prepare_callback_ || !server->turret_prepare_callback_(count)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Turret rejected; inspect /api/turret/status", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"accepted\":true,\"state\":\"awaiting_confirmation\",\"fired\":false}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::turret_disarm_handler(httpd_req_t* req) {
    auto* server = static_cast<WebServer*>(req->user_ctx);
    char content[32];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;
    cJSON* root = ParseObject(content);
    bool valid = root != nullptr; cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON object required");
    if (!server->turret_disarm_callback_ || !server->turret_disarm_callback_()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Disarm requested but turret fault latched; inspect status", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    // 禁用状态也接受幂等取消，不把disabled错误报告为disarmed。
    return httpd_resp_send(req, "{\"accepted\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t WebServer::index_get_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, server->get_html_page(), HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

esp_err_t WebServer::control_post_handler(httpd_req_t* req) {
    auto* server = static_cast<WebServer*>(req->user_ctx);
    char content[100];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;
    int direction = -1, speed = -1;
    server->parse_simple_control_command(content, direction, speed);
    if (!car::MotorControl::ValidCommand(direction, speed)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid direction or speed");
        return ESP_FAIL;
    }
    return SendMotorResult(req, server->InvokeMotorControl(direction, speed));
}

esp_err_t WebServer::api_control_handler(httpd_req_t* req) {
    auto* server = static_cast<WebServer*>(req->user_ctx);
    char content[200];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;
    cJSON* root = ParseObject(content);
    int direction = -1, speed = -1;
    bool valid = root && ReadInteger(root, "direction", direction) && ReadInteger(root, "speed", speed) &&
        car::MotorControl::ValidCommand(direction, speed);
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Integer direction 0..4 and speed 0..100 required");
        return ESP_FAIL;
    }
    return SendMotorResult(req, server->InvokeMotorControl(direction, speed));
}

esp_err_t WebServer::api_motor_status_handler(httpd_req_t* req) {
    auto status = Application::GetInstance().GetMotorStatus();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, status.c_str(), status.size());
}

esp_err_t WebServer::debug_motor_test_handler(httpd_req_t* req) {
    char content[256];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;
    cJSON* root = ParseObject(content);
    auto& app = Application::GetInstance();
    auto config = app.GetMotorActionConfig();
    int direction = 4, speed = config.default_speed_percent, duration = config.forward_duration_ms;
    bool valid = root && ReadInteger(root, "direction", direction, false) &&
        ReadInteger(root, "speed", speed, false) && ReadInteger(root, "duration", duration, false) &&
        car::MotorControl::ValidCommand(direction, speed) && duration >= car::kMinDurationMs && duration <= car::kMaxDurationMs;
    cJSON_Delete(root);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid motor test parameters");
        return ESP_FAIL;
    }
    // 调试接口也走统一定时执行器，不再创建可能打断后续指令的停止任务。
    return SendMotorResult(req, app.HandleMotorActionWithDuration(direction, speed, duration));
}

esp_err_t WebServer::api_motor_action_handler(httpd_req_t* req) {
    auto* server = static_cast<WebServer*>(req->user_ctx);
    char content[256];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;
    cJSON* root = ParseObject(content);
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(value)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "String action required");
        return ESP_FAIL;
    }
    std::string action = value->valuestring;
    cJSON_Delete(root);
    auto& app = Application::GetInstance();
    auto config = app.GetMotorActionConfig();
    struct Action { const char* name; int direction; int car::MotorActionConfig::*duration; };
    const Action actions[] = {
        {"move_forward", 4, &car::MotorActionConfig::forward_duration_ms},
        {"move_backward", 2, &car::MotorActionConfig::backward_duration_ms},
        {"turn_left", 3, &car::MotorActionConfig::left_turn_duration_ms},
        {"turn_right", 1, &car::MotorActionConfig::right_turn_duration_ms},
        {"spin_around", 3, &car::MotorActionConfig::spin_duration_ms},
        {"quick_forward", 4, &car::MotorActionConfig::quick_forward_duration_ms},
        {"quick_backward", 2, &car::MotorActionConfig::quick_backward_duration_ms}
    };
    for (const auto& entry : actions) {
        if (action == entry.name) return SendMotorResult(req,
            app.HandleMotorActionWithDuration(entry.direction, config.default_speed_percent, config.*(entry.duration)));
    }
    if (action == "stop") return SendMotorResult(req, app.HandleMotorActionWithDuration(0, 0, 0));
    if (action == "wiggle" || action == "dance") return SendMotorResult(req, app.HandleMotorPattern(action, config.default_speed_percent));
    for (const char* emotion : {"wake_up", "happy", "sad", "thinking", "listening", "speaking", "excited", "loving", "angry", "surprised", "confused"}) {
        if (action == emotion) {
            // 情绪动作兼容旧网页，只更新显示，不再隐式行驶。
            std::string display_emotion = action == "wake_up" ? "sleepy" : action;
            server->SetEmotion(display_emotion.c_str());
            return SendMotorResult(req, true);
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    return ESP_FAIL;
}

void WebServer::parse_simple_control_command(const char* data, int& direction, int& speed) {
    direction = speed = -1;
    std::string body(data);
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find(',', start);
        std::string item = body.substr(start, end == std::string::npos ? end : end - start);
        size_t equal = item.find('=');
        int value = 0;
        if (equal == std::string::npos || !ParseIntegerText(item.substr(equal + 1), value)) { direction = speed = -1; return; }
        std::string key = item.substr(0, equal);
        if (key == "direction") direction = value;
        else if (key == "speed") speed = value;
        else { direction = speed = -1; return; }
        if (end == std::string::npos) break;
        start = end + 1;
    }
}

const char* WebServer::get_html_page() {
    static const char html_page[] = R"html(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>小智小车遥控器</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
        }

        .container {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 30px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
            text-align: center;
            max-width: 400px;
            width: 100%;
        }

        h1 {
            color: #333;
            margin-bottom: 10px;
            font-size: 2.2em;
        }

        .subtitle {
            color: #666;
            margin-bottom: 30px;
            font-size: 1.1em;
        }

        .joystick-container {
            position: relative;
            width: 400px;
            height: 400px;
            margin: 0 auto 30px;
            border-radius: 50%;
            background: #f0f0f0;
            border: 3px solid #ddd;
            touch-action: none;
        }

        .joystick {
            position: absolute;
            width: 112px;
            height: 112px;
            background: linear-gradient(135deg, #4CAF50, #45a049);
            border-radius: 50%;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            box-shadow: 0 4px 8px rgba(0,0,0,0.2);
            transition: all 0.1s ease;
            cursor: pointer;
        }

        .joystick.active {
            background: linear-gradient(135deg, #2196F3, #1976D2);
            transform: translate(-50%, -50%) scale(0.95);
        }

        .direction-indicator {
            position: absolute;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            font-size: 18px;
            font-weight: bold;
            color: #333;
            pointer-events: none;
            transition: opacity 0.3s ease;
        }

        .direction-indicator.active {
            opacity: 1;
        }

        .status {
            margin-top: 20px;
            padding: 10px;
            border-radius: 10px;
            background: #f8f9fa;
            border: 1px solid #e9ecef;
        }

        .status.connected {
            background: #d4edda;
            border-color: #c3e6cb;
            color: #155724;
        }

        .status.disconnected {
            background: #f8d7da;
            border-color: #f5c6cb;
            color: #721c24;
        }

        .controls {
            margin-top: 20px;
        }

        .control-btn {
            background: linear-gradient(135deg, #FF6B6B, #EE5A24);
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 25px;
            font-size: 16px;
            cursor: pointer;
            margin: 5px;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(255, 107, 107, 0.3);
        }

        .control-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(255, 107, 107, 0.4);
        }

        .control-btn:active {
            transform: translateY(0);
        }

        .stop-btn {
            background: linear-gradient(135deg, #DC3545, #C82333);
        }

        .stop-btn:hover {
            box-shadow: 0 6px 20px rgba(220, 53, 69, 0.4);
        }

        /* 动作控制区域样式 */
        .actions-section {
            margin-top: 30px;
            padding: 20px;
            background: rgba(255, 255, 255, 0.9);
            border-radius: 15px;
            border: 1px solid #e9ecef;
        }

        .action-buttons {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
            gap: 20px;
        }

        .action-group {
            background: #f8f9fa;
            padding: 15px;
            border-radius: 10px;
            border: 1px solid #dee2e6;
        }

        .action-group h4 {
            margin: 0 0 15px 0;
            color: #495057;
            font-size: 1.1em;
            text-align: center;
            border-bottom: 2px solid #e9ecef;
            padding-bottom: 8px;
        }

        .action-btn {
            background: linear-gradient(135deg, #28a745, #20c997);
            color: white;
            border: none;
            padding: 10px 15px;
            border-radius: 8px;
            font-size: 14px;
            cursor: pointer;
            margin: 5px;
            transition: all 0.3s ease;
            box-shadow: 0 2px 8px rgba(40, 167, 69, 0.2);
            min-width: 100px;
        }

        .action-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(40, 167, 69, 0.3);
        }

        .action-btn:active {
            transform: translateY(0);
        }

        @media (max-width: 480px) {
            .container {
                padding: 20px;
                margin: 10px;
            }

            .joystick-container {
                width: 320px;
                height: 320px;
            }

            .joystick {
                width: 96px;
                height: 96px;
            }

            h1 {
                font-size: 1.8em;
            }

            .action-buttons {
                grid-template-columns: 1fr;
                gap: 15px;
            }

            .action-btn {
                font-size: 13px;
                padding: 8px 12px;
                min-width: 80px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🚗 灵矽互动小车</h1>
        <div class="subtitle">触摸或拖拽摇杆控制小车移动</div>

        <div class="joystick-container" id="joystick-container">
            <div class="joystick" id="joystick"></div>
            <div class="direction-indicator" id="direction-indicator">⏹️</div>
        </div>

        <div class="status connected" id="status">
            <strong>状态:</strong> <span id="status-text">已连接</span>
        </div>

        <div class="controls">
            <button class="control-btn stop-btn" onclick="stopCar()">🛑 停止</button>
            <a href="/config" class="control-btn" style="background: linear-gradient(135deg, #FF9800, #F57C00);">⚙️ 配置</a>
        </div>

        <div class="actions-section">
            <h3 style="color: #333; margin: 20px 0 15px 0; text-align: center;">🎭 动作控制</h3>

            <div class="action-buttons">
                <!-- 基本移动动作 -->
                <div class="action-group">
                    <h4>🚗 基本移动</h4>
                    <button class="action-btn" onclick="executeAction('move_forward')">⬆️ 前进</button>
                    <button class="action-btn" onclick="executeAction('move_backward')">⬇️ 后退</button>
                    <button class="action-btn" onclick="executeAction('turn_left')">⬅️ 左转</button>
                    <button class="action-btn" onclick="executeAction('turn_right')">➡️ 右转</button>
                    <button class="action-btn" onclick="executeAction('spin_around')">🔄 转圈</button>
                </div>

                <!-- 情感动作 -->
                <div class="action-group">
                    <h4>😊 情感表达</h4>
                    <button class="action-btn" onclick="executeAction('wake_up')">🌅 唤醒</button>
                    <button class="action-btn" onclick="executeAction('happy')">😄 开心</button>
                    <button class="action-btn" onclick="executeAction('sad')">😢 悲伤</button>
                    <button class="action-btn" onclick="executeAction('thinking')">🤔 思考</button>
                    <button class="action-btn" onclick="executeAction('listening')">👂 倾听</button>
                    <button class="action-btn" onclick="executeAction('speaking')">💬 说话</button>
                    <button class="action-btn" onclick="executeAction('wiggle')">🌊 摆动</button>
                    <button class="action-btn" onclick="executeAction('dance')">💃 跳舞</button>
                </div>

                <!-- 高级情感 -->
                <div class="action-group">
                    <h4>🎭 高级情感</h4>
                    <button class="action-btn" onclick="executeAction('excited')">🤩 兴奋</button>
                    <button class="action-btn" onclick="executeAction('loving')">😍 爱慕</button>
                    <button class="action-btn" onclick="executeAction('angry')">😠 生气</button>
                    <button class="action-btn" onclick="executeAction('surprised')">😲 惊讶</button>
                    <button class="action-btn" onclick="executeAction('confused')">😕 困惑</button>
                </div>
            </div>
        </div>
    </div>

    <script>
        let joystick = document.getElementById('joystick');
        let joystickContainer = document.getElementById('joystick-container');
        let directionIndicator = document.getElementById('direction-indicator');
        let statusText = document.getElementById('status-text');

        let isDragging = false;
        let centerX = 0;
        let centerY = 0;
        let currentDirection = 0;
        let currentSpeed = 0;
        let isRequestPending = false;
        let pendingControl = null;
        let controlDrain = Promise.resolve(true);
        let inputGeneration = 0;


        // 初始化摇杆中心位置
        function initJoystick() {
            const rect = joystickContainer.getBoundingClientRect();
            centerX = rect.left + rect.width / 2;
            centerY = rect.top + rect.height / 2;
        }

        // 更新摇杆位置
        function updateJoystickPosition(x, y) {
            const rect = joystickContainer.getBoundingClientRect();
            const containerCenterX = rect.left + rect.width / 2;
            const containerCenterY = rect.top + rect.height / 2;

            // 计算相对于容器的位置
            let relativeX = x - containerCenterX;
            let relativeY = y - containerCenterY;

            // 限制在圆形范围内
            const maxRadius = rect.width / 2 - 56;
            const distance = Math.sqrt(relativeX * relativeX + relativeY * relativeY);

            if (distance > maxRadius) {
                relativeX = (relativeX / distance) * maxRadius;
                relativeY = (relativeY / distance) * maxRadius;
            }

            // 更新摇杆位置
            joystick.style.left = `calc(50% + ${relativeX}px)`;
            joystick.style.top = `calc(50% + ${relativeY}px)`;

            // 计算方向和速度
            const normalizedX = relativeX / maxRadius;
            const normalizedY = relativeY / maxRadius;

            // 计算方向角度 (0-360度)
            let angle = Math.atan2(normalizedY, normalizedX) * (180 / Math.PI);
            if (angle < 0) angle += 360;

            // 计算速度 (0-100)
            const speed = Math.min(distance / maxRadius, 1) * 100;

            // 转换方向为整数值
            let direction = 0; // 停止
            if (speed > 5) { // 最小阈值 (降低阈值以响应点击)
                if (angle >= 315 || angle < 45) {
                    direction = 1; // 右
                } else if (angle >= 45 && angle < 135) {
                    direction = 2; // 下
                } else if (angle >= 135 && angle < 225) {
                    direction = 3; // 左
                } else if (angle >= 225 && angle < 315) {
                    direction = 4; // 上
                }
            }

            return { direction, speed: Math.round(speed) };
        }

        // 更新方向指示器
        function updateDirectionIndicator(direction, speed) {
            let icon = '⏹️';
            let text = '停止';

            if (speed > 5) {
                switch(direction) {
                    case 1: icon = '➡️'; text = '右转'; break;
                    case 2: icon = '⬇️'; text = '后退'; break;
                    case 3: icon = '⬅️'; text = '左转'; break;
                    case 4: icon = '⬆️'; text = '前进'; break;
                }
            }

            directionIndicator.textContent = icon;
            directionIndicator.classList.toggle('active', speed > 5);
        }

        // 保留最新待发指令；请求超时后停止交互，由固件租约兜底。
        function sendControl(direction, speed) {
            currentDirection = direction;
            currentSpeed = speed;
            pendingControl = { direction, speed };
            if (!isRequestPending) controlDrain = drainControls();
            return controlDrain;
        }

        async function drainControls() {
            isRequestPending = true;
            let accepted = true;
            try {
                while (pendingControl) {
                    const command = pendingControl;
                    pendingControl = null;
                    const controller = new AbortController();
                    const timeout = setTimeout(() => controller.abort(), 500);
                    try {
                        const response = await fetch('/api/control', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify(command), signal: controller.signal
                        });
                        if (!response.ok) throw new Error('Control rejected');
                        statusText.textContent = '指令已接受';
                        document.getElementById('status').className = 'status connected';
                    } catch (error) {
                        accepted = false;
                        isDragging = false;
                        currentDirection = currentSpeed = 0;
                        pendingControl = null;
                        statusText.textContent = '连接异常，停止遥控';
                        document.getElementById('status').className = 'status error';
                        // 尽力补发停止，失败时最后一条有效心跳将在 750ms 后失效。
                        void fetch('/api/control', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ direction: 0, speed: 0 }), keepalive: true
                        }).catch(() => {});
                    } finally {
                        clearTimeout(timeout);
                    }
                }
            } finally {
                isRequestPending = false;
            }
            return accepted;
        }

        // 停止小车
        function stopCar() {
            ++inputGeneration;
            // 重置状态
            isDragging = false;
            currentDirection = 0;
            currentSpeed = 0;

            // 重置UI
            joystick.style.left = '50%';
            joystick.style.top = '50%';
            joystick.classList.remove('active');
            updateDirectionIndicator(0, 0);

            // 发送停止命令
            return sendControl(0, 0);
        }


        // 执行电机动作
        async function executeAction(action) {
            // 先结束摇杆租约，防止心跳立刻覆盖按钮动作。
            const stopping = stopCar();
            const generation = inputGeneration;
            const accepted = await stopping;
            // 等待停车期间若用户再次停止或开始新操作，不能补发过时的按钮动作。
            if (!accepted || generation !== inputGeneration) return;
            try {
                const response = await fetch('/api/motor/action', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({
                        action: action
                    })
                });

                if (!response.ok) {
                    throw new Error('Network response was not ok');
                }

                const result = await response.json();
                console.log('Action executed:', action, result);

                // 更新状态显示
                statusText.textContent = '指令已接受（实车效果待确认）';
                document.getElementById('status').className = 'status connected';

            } catch (error) {
                console.error('Failed to execute action:', action, error);
                statusText.textContent = '动作执行失败';
                document.getElementById('status').className = 'status disconnected';
            }
        }

        // 鼠标事件
        joystickContainer.addEventListener('mousedown', (e) => {
            isDragging = true;
            joystick.classList.add('active');
            initJoystick();
            const { direction, speed } = updateJoystickPosition(e.clientX, e.clientY);
            updateDirectionIndicator(direction, speed);
            sendControl(direction, speed);
        });

        document.addEventListener('mousemove', (e) => {
            if (isDragging) {
                const { direction, speed } = updateJoystickPosition(e.clientX, e.clientY);
                updateDirectionIndicator(direction, speed);
                sendControl(direction, speed);
            }
        });

        document.addEventListener('mouseup', () => {
            if (isDragging) {
                stopCar();
            }
        });

        // 触摸事件
        joystickContainer.addEventListener('touchstart', (e) => {
            ++inputGeneration;
            e.preventDefault();
            isDragging = true;
            joystick.classList.add('active');
            initJoystick();
            const touch = e.touches[0];
            const { direction, speed } = updateJoystickPosition(touch.clientX, touch.clientY);
            updateDirectionIndicator(direction, speed);
            sendControl(direction, speed);
        });

        joystickContainer.addEventListener('touchmove', (e) => {
            e.preventDefault();
            if (isDragging) {
                const touch = e.touches[0];
                const { direction, speed } = updateJoystickPosition(touch.clientX, touch.clientY);
                updateDirectionIndicator(direction, speed);
                sendControl(direction, speed);
            }
        });

        joystickContainer.addEventListener('touchend', (e) => {
            e.preventDefault();
            if (isDragging) {
                stopCar();
            }
        });

        // 全局触摸结束事件，确保在任何地方松手都能停止
        document.addEventListener('touchend', (e) => {
            if (isDragging && e.target !== joystick && e.target !== joystickContainer) {
                stopCar();
            }
        });

        // 定期发送控制命令（当摇杆被拖拽时）
        setInterval(() => {
            if (isDragging) {
                sendControl(currentDirection, currentSpeed);
            }
        }, 200); // 每200ms发送一次，减少服务器压力

        joystickContainer.addEventListener('touchcancel', stopCar);
        window.addEventListener('blur', stopCar);
        document.addEventListener('visibilitychange', () => { if (document.hidden) stopCar(); });
        window.addEventListener('pagehide', () => {
            ++inputGeneration;
            isDragging = false;
            void fetch('/api/control', {
                method: 'POST', headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ direction: 0, speed: 0 }), keepalive: true
            }).catch(() => {});
        });

        // 初始化
        initJoystick();
        window.addEventListener('resize', initJoystick);
    </script>
</body>
</html>
)html";

    return html_page;
}


// 配置页面处理器
esp_err_t WebServer::config_get_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, server->get_config_html_page(), HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// 配置页面POST处理器
esp_err_t WebServer::config_post_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    char content[512];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;

    // 解析表单数据
    MotorActionConfig config = server->get_motor_config_callback_ ? server->get_motor_config_callback_() : MotorActionConfig{};
    if (!server->parse_config_form_data(content, config) || !config.IsValid()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid motor configuration");
        return ESP_FAIL;
    }

    // 保存配置
    if (server->set_motor_config_callback_) {
        server->set_motor_config_callback_(config);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>配置已保存</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .container {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 40px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
            text-align: center;
            max-width: 500px;
            width: 100%;
        }

        .success-icon {
            font-size: 4em;
            margin-bottom: 20px;
        }

        h1 {
            color: #333;
            margin-bottom: 10px;
            font-size: 2.2em;
        }

        .message {
            color: #666;
            margin-bottom: 30px;
            font-size: 1.1em;
        }

        .buttons {
            display: flex;
            gap: 15px;
            justify-content: center;
            flex-wrap: wrap;
        }

        .btn {
            background: linear-gradient(135deg, #4CAF50, #45a049);
            color: white;
            border: none;
            padding: 12px 24px;
            border-radius: 25px;
            font-size: 16px;
            cursor: pointer;
            text-decoration: none;
            display: inline-block;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(76, 175, 80, 0.3);
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(76, 175, 80, 0.4);
        }

        .btn.secondary {
            background: linear-gradient(135deg, #2196F3, #1976D2);
        }

        .btn.secondary:hover {
            box-shadow: 0 6px 20px rgba(33, 150, 243, 0.4);
        }

        @media (max-width: 480px) {
            .container {
                padding: 30px 20px;
            }

            .buttons {
                flex-direction: column;
                align-items: center;
            }

            .btn {
                width: 200px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="success-icon">✅</div>
        <h1>配置已保存！</h1>
        <div class="message">您的电机动作配置已成功保存到设备中。</div>

        <div class="buttons">
            <a href="/config" class="btn secondary">⚙️ 返回配置页面</a>
            <a href="/" class="btn">🏠 返回遥控器</a>
        </div>
    </div>
</body>
</html>)html", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration callback not set");
    }

    return ESP_OK;
}

// API配置GET处理器
esp_err_t WebServer::api_config_get_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    if (server->get_motor_config_callback_) {
        MotorActionConfig config = server->get_motor_config_callback_();

        cJSON *root = cJSON_CreateObject();
        for (const auto& field : kConfigFields) cJSON_AddNumberToObject(root, field.key, config.*(field.member));

        char *json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (json_str) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
            cJSON_free(json_str);
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to generate JSON");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration callback not set");
    }

    return ESP_OK;
}

// API配置POST处理器
esp_err_t WebServer::api_config_post_handler(httpd_req_t *req) {
    WebServer* server = (WebServer*)req->user_ctx;

    // 设置CORS头
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    char content[512];
    if (!ReadBody(req, content, sizeof(content))) return ESP_FAIL;

    // 解析JSON数据
    cJSON *root = ParseObject(content);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    MotorActionConfig config = server->get_motor_config_callback_ ? server->get_motor_config_callback_() : MotorActionConfig{};
    bool valid = cJSON_IsObject(root);
    for (const auto& field : kConfigFields) {
        valid = valid && ReadInteger(root, field.key, config.*(field.member), false);
    }
    cJSON_Delete(root);
    if (!valid || !config.IsValid()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Motor durations must be 100..10000ms and speed 0..100");
        return ESP_FAIL;
    }

    // 保存配置
    if (server->set_motor_config_callback_) {
        server->set_motor_config_callback_(config);

        // 在串口中输出保存的配置信息
        ESP_LOGI(TAG, "网页配置已保存:");
        ESP_LOGI(TAG, "  前进时间: %d ms", config.forward_duration_ms);
        ESP_LOGI(TAG, "  后退时间: %d ms", config.backward_duration_ms);
        ESP_LOGI(TAG, "  左转时间: %d ms", config.left_turn_duration_ms);
        ESP_LOGI(TAG, "  右转时间: %d ms", config.right_turn_duration_ms);
        ESP_LOGI(TAG, "  转圈时间: %d ms", config.spin_duration_ms);
        ESP_LOGI(TAG, "  快速前进时间: %d ms", config.quick_forward_duration_ms);
        ESP_LOGI(TAG, "  快速后退时间: %d ms", config.quick_backward_duration_ms);
        ESP_LOGI(TAG, "  默认速度: %d%%", config.default_speed_percent);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"success\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Configuration callback not set");
    }

    return ESP_OK;
}

// 解析配置表单数据
bool WebServer::parse_config_form_data(const char* data, MotorActionConfig& config) {
    std::string body(data);
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('&', start);
        std::string item = body.substr(start, end == std::string::npos ? end : end - start);
        size_t equal = item.find('=');
        if (equal == std::string::npos) return false;
        std::string key = item.substr(0, equal);
        int value = 0;
        if (!ParseIntegerText(item.substr(equal + 1), value)) return false;
        bool known = false;
        for (const auto& field : kConfigFields) {
            if (key == field.key) { config.*(field.member) = value; known = true; break; }
        }
        if (!known) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return config.IsValid();
}

// 获取配置页面HTML
const char* WebServer::get_config_html_page() {
    static const char config_html_page[] = R"html(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>电机动作配置</title>
    <style>
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0;
            padding: 20px;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
        }

        .container {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 20px;
            padding: 30px;
            box-shadow: 0 20px 40px rgba(0,0,0,0.1);
            max-width: 600px;
            width: 100%;
        }

        h1 {
            color: #333;
            text-align: center;
            margin-bottom: 30px;
        }

        .form-group {
            margin-bottom: 20px;
        }

        label {
            display: block;
            margin-bottom: 5px;
            font-weight: bold;
            color: #555;
        }

        input[type="number"] {
            width: 100%;
            padding: 10px;
            border: 2px solid #ddd;
            border-radius: 8px;
            font-size: 16px;
            transition: border-color 0.3s ease;
        }

        input[type="number"]:focus {
            outline: none;
            border-color: #4CAF50;
        }

        .unit {
            color: #666;
            font-size: 14px;
            margin-left: 5px;
        }

        .description {
            color: #777;
            font-size: 14px;
            margin-top: 3px;
            font-weight: normal;
        }

        .buttons {
            text-align: center;
            margin-top: 30px;
        }

        .btn {
            background: linear-gradient(135deg, #4CAF50, #45a049);
            color: white;
            border: none;
            padding: 12px 30px;
            border-radius: 25px;
            font-size: 16px;
            cursor: pointer;
            margin: 0 10px;
            text-decoration: none;
            display: inline-block;
            transition: all 0.3s ease;
        }

        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(76, 175, 80, 0.4);
        }

        .btn.secondary {
            background: linear-gradient(135deg, #2196F3, #1976D2);
        }

        .btn.secondary:hover {
            box-shadow: 0 6px 20px rgba(33, 150, 243, 0.4);
        }

        .grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 20px;
        }

        @media (max-width: 480px) {
            .grid {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>⚙️ 电机动作配置</h1>
        <form method="POST" action="/config">
            <div class="grid">
                <div class="form-group">
                    <label for="forward_ms">前进时间</label>
                    <input type="number" id="forward_ms" name="forward_ms" min="100" max="10000" step="100" required>
                    <span class="unit">毫秒</span>
                    <div class="description">默认前进动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="backward_ms">后退时间</label>
                    <input type="number" id="backward_ms" name="backward_ms" min="100" max="10000" step="100" required>
                    <span class="unit">毫秒</span>
                    <div class="description">默认后退动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="left_turn_ms">左转时间</label>
                    <input type="number" id="left_turn_ms" name="left_turn_ms" min="100" max="10000" step="50" required>
                    <span class="unit">毫秒</span>
                    <div class="description">左转动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="right_turn_ms">右转时间</label>
                    <input type="number" id="right_turn_ms" name="right_turn_ms" min="100" max="10000" step="50" required>
                    <span class="unit">毫秒</span>
                    <div class="description">右转动作的持续时间</div>
                </div>

                <div class="form-group">
                    <label for="spin_ms">转圈时间</label>
                    <input type="number" id="spin_ms" name="spin_ms" min="500" max="10000" step="100" required>
                    <span class="unit">毫秒</span>
                    <div class="description">转圈动作的持续时间</div>
                </div>


                <div class="form-group">
                    <label for="def_speed_pct">默认速度</label>
                    <input type="number" id="def_speed_pct" name="def_speed_pct" min="0" max="100" step="5" required>
                    <span class="unit">%</span>
                    <div class="description">电机动作的默认速度百分比</div>
                </div>
            </div>

            <div class="buttons">
                <button type="submit" class="btn">💾 保存配置</button>
                <a href="/" class="btn secondary">🏠 返回遥控器</a>
            </div>
        </form>
    </div>

    <script>
        // 页面加载时获取当前配置
        window.onload = function() {
            fetch('/api/config')
                .then(response => response.json())
                .then(config => {
                    document.getElementById('forward_ms').value = config.forward_ms;
                    document.getElementById('backward_ms').value = config.backward_ms;
                    document.getElementById('left_turn_ms').value = config.left_turn_ms;
                    document.getElementById('right_turn_ms').value = config.right_turn_ms;
                    document.getElementById('spin_ms').value = config.spin_ms;
                    document.getElementById('def_speed_pct').value = config.def_speed_pct;
                })
                .catch(error => console.error('Failed to load config:', error));
        };

        // 处理表单提交
        document.getElementById('config-form').addEventListener('submit', function(e) {
            e.preventDefault(); // 阻止默认表单提交

            const formData = new FormData(this);
            const data = Object.fromEntries([...formData.entries()].map(([key, value]) => [key, Number(value)]));

            fetch('/api/config', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                },
                body: JSON.stringify(data)
            })
            .then(response => response.json())
            .then(result => {
                if (result.status === 'success') {
                    // 显示成功消息
                    alert('配置保存成功！');
                    // 自动跳转回遥控器界面
                    window.location.href = '/';
                } else {
                    alert('配置保存失败，请重试');
                }
            })
            .catch(error => {
                console.error('Failed to save config:', error);
                alert('配置保存失败，请检查网络连接');
            });
        });
    </script>
</body>
</html>
)html";

    return config_html_page;
}
