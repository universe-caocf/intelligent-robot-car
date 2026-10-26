#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>
#include <string>
#include <functional>
#include <vector>
#include <mutex>
#include "motor_control.h"

class WebServer {
public:
    WebServer();
    ~WebServer();

    bool Start(int port = 80);
    void Stop();

    // 注册电机控制回调函数 - 使用 std::function 支持 lambda
    void SetMotorControlCallback(std::function<bool(int direction, int speed)> callback);
    // 由外部调用以触发电机控制（封装私有回调）
    bool InvokeMotorControl(int direction, int speed);

    // 注册表情设置回调函数
    void SetEmotionCallback(std::function<void(const char* emotion)> callback);
    // 设置表情（封装私有回调）
    void SetEmotion(const char* emotion);
    void SetTurretCallbacks(std::function<std::string()> status, std::function<bool(int)> prepare,
                            std::function<bool()> disarm);

    // 电机动作配置回调
    using MotorActionConfig = car::MotorActionConfig;

    void SetMotorActionConfigCallback(std::function<MotorActionConfig()> get_callback,
                                     std::function<void(const MotorActionConfig&)> set_callback);

    // Debug handler 注册（/api/debug/motor_test）
    static esp_err_t debug_motor_test_handler(httpd_req_t *req);

private:
    httpd_handle_t server_handle_;
    std::mutex lifecycle_mutex_;
    int server_port_ = 0;
    std::function<bool(int direction, int speed)> motor_control_callback_;
    std::function<void(const char* emotion)> emotion_callback_;
    std::function<MotorActionConfig()> get_motor_config_callback_;
    std::function<void(const MotorActionConfig&)> set_motor_config_callback_;
    std::function<std::string()> turret_status_callback_;
    std::function<bool(int)> turret_prepare_callback_;
    std::function<bool()> turret_disarm_callback_;
    static esp_err_t turret_status_handler(httpd_req_t* req);
    static esp_err_t turret_prepare_handler(httpd_req_t* req);
    static esp_err_t turret_disarm_handler(httpd_req_t* req);

    // HTTP请求处理函数
    static esp_err_t index_get_handler(httpd_req_t *req);
    static esp_err_t control_post_handler(httpd_req_t *req);
    static esp_err_t api_control_handler(httpd_req_t *req);
    static esp_err_t api_motor_status_handler(httpd_req_t *req);
    static esp_err_t api_motor_action_handler(httpd_req_t *req);
    static esp_err_t config_get_handler(httpd_req_t *req);
    static esp_err_t config_post_handler(httpd_req_t *req);
    static esp_err_t api_config_get_handler(httpd_req_t *req);
    static esp_err_t api_config_post_handler(httpd_req_t *req);

    // CORS处理
    static esp_err_t cors_handler(httpd_req_t *req);

    // 获取HTML页面内容
    static const char* get_html_page();

    // 解析控制命令
    void parse_simple_control_command(const char* data, int& direction, int& speed);

    // 配置相关方法
    bool parse_config_form_data(const char* data, MotorActionConfig& config);
    static const char* get_config_html_page();
};

#endif // WEB_SERVER_H
