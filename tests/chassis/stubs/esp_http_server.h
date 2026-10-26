// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
using esp_err_t = int;
using httpd_handle_t = void*;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
constexpr int HTTP_GET = 0, HTTP_POST = 1;
constexpr int HTTPD_400_BAD_REQUEST = 400, HTTPD_500_INTERNAL_SERVER_ERROR = 500;
constexpr int HTTPD_403_FORBIDDEN = 403;
constexpr int HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
    size_t content_len = 0;
    void* user_ctx = nullptr;
    std::string body, response, content_type;
    int status = 200;
    size_t offset = 0, chunk = 3;
    bool receive_error = false;
    std::map<std::string, std::string> headers;
};
struct httpd_uri_t {
    const char* uri;
    int method;
    esp_err_t (*handler)(httpd_req_t*);
    void* user_ctx;
};
struct httpd_config_t { int server_port, max_uri_handlers, recv_wait_timeout, send_wait_timeout, max_resp_headers; };
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
inline std::map<std::pair<std::string, int>, httpd_uri_t> routes;
inline bool http_fail_start = false;
inline int http_starts = 0, http_stops = 0, http_registrations = 0, http_fail_route = -1;
inline esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t*) {
    ++http_starts; http_registrations = 0;
    if (http_fail_start) return ESP_FAIL;
    *handle = &routes; return ESP_OK;
}
inline esp_err_t httpd_stop(httpd_handle_t) { ++http_stops; routes.clear(); return ESP_OK; }
inline esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* uri) {
    if (http_registrations++ == http_fail_route) return ESP_FAIL;
    routes[{uri->uri, uri->method}] = *uri; return ESP_OK;
}
inline size_t httpd_req_get_hdr_value_len(httpd_req_t* req, const char* key) { return req->headers[key].size(); }
inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t* req, const char* key, char* buffer, size_t capacity) {
    const auto& value = req->headers[key];
    if (value.size() >= capacity) return ESP_FAIL;
    memcpy(buffer, value.c_str(), value.size() + 1); return ESP_OK;
}
inline esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* value) { req->content_type = value; return ESP_OK; }
inline esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
inline esp_err_t httpd_resp_set_status(httpd_req_t* req, const char* value) { req->status = std::stoi(value); return ESP_OK; }
inline esp_err_t httpd_resp_send(httpd_req_t* req, const char* data, int size) {
    req->response.assign(data, size == HTTPD_RESP_USE_STRLEN ? strlen(data) : size); return ESP_OK;
}
inline esp_err_t httpd_resp_send_err(httpd_req_t* req, int status, const char* text) { req->status = status; req->response = text; return ESP_OK; }
inline int httpd_req_recv(httpd_req_t* req, char* buffer, size_t size) {
    if (req->receive_error) return ESP_FAIL;
    size_t count = std::min({size, req->chunk, req->body.size() - req->offset});
    memcpy(buffer, req->body.data() + req->offset, count); req->offset += count; return static_cast<int>(count);
}
