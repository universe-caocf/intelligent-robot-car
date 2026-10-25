// SPDX-License-Identifier: GPL-3.0-or-later
#include "web_server/web_server.h"
#include "application.h"
#include "cJSON.h"
#include <cassert>
#include <iostream>
httpd_req_t Request(const std::string& path, const std::string& body, int method = HTTP_POST, bool receive_error = false) {
    auto entry = routes.at({path, method});
    httpd_req_t req;
    req.user_ctx = entry.user_ctx;
    req.body = body;
    req.content_len = body.size();
    req.receive_error = receive_error;
    entry.handler(&req);
    return req;
}
int main() {
    WebServer server;
    auto& app = Application::GetInstance();
    int web_calls = 0, config_writes = 0;
    server.SetMotorControlCallback([&](int direction, int speed) {
        ++web_calls;
        return app.available && app.motor.WebCommand(direction, speed, app.now);
    });
    server.SetMotorActionConfigCallback([&]() { return app.config; }, [&](const car::MotorActionConfig& config) {
        ++config_writes; app.config = config;
    });
    assert(server.Start() && routes.size() == 10);
    assert(Request("/api/control", R"({"direction":4,"speed":80})").status == 200);
    assert(app.motor.direction() == 4 && app.motor.deadline_ms() == 750);
    app.now = 600;
    assert(Request("/api/control", R"({"direction":4,"speed":80})").status == 200);
    assert(app.motor.deadline_ms() == 1350);
    int before = web_calls;
    for (const std::string& body : {"", "{", "[]", "null", "{}", "{\"direction\":4}",
            "{\"direction\":4,\"speed\":\"80\"}", "{\"direction\":4,\"speed\":80.5}",
            "{\"direction\":5,\"speed\":80}", "{\"direction\":4,\"speed\":101}",
            "{\"direction\":4,\"speed\":80} garbage", "{\"direction\":4,\"speed\":1e1000}"}) {
        assert(Request("/api/control", body).status == 400);
    }
    assert(Request("/api/control", std::string(200, 'x')).status == 400);
    assert(Request("/api/control", std::string("{}\0junk", 7)).status == 400);
    assert(Request("/api/control", "{}", HTTP_POST, true).status == 400);
    assert(web_calls == before && app.motor.direction() == 4);
    assert(Request("/control", "direction=3,speed=40").status == 200);
    assert(Request("/control", "direction=bad,speed=40").status == 400);
    assert(Request("/control", "direction=3").status == 400);
    assert(Request("/control", "direction=3,speed=99999999999999999999").status == 400);
    app.available = false;
    assert(Request("/api/control", R"({"direction":4,"speed":80})").status == 503);
    app.available = true;
    assert(Request("/api/debug/motor_test", R"({"direction":4,"speed":80,"duration":1000})").status == 200);
    int64_t first_deadline = app.motor.deadline_ms();
    app.now += 400;
    assert(Request("/api/motor/action", R"({"action":"move_backward"})").status == 200);
    assert(!app.motor.Poll(first_deadline) && app.motor.direction() == 2);
    assert(Request("/api/motor/action", R"({"action":"stop"})").status == 200);
    assert(app.motor.direction() == 0 && !app.motor.Poll(app.now + 10000));
    before = app.calls;
    assert(Request("/api/debug/motor_test", "oops").status == 400);
    assert(Request("/api/debug/motor_test", R"({"duration":0})").status == 400);
    assert(Request("/api/motor/action", "{}").status == 400);
    assert(Request("/api/motor/action", R"({"action":"unknown"})").status == 400);
    assert(app.calls == before);
    assert(Request("/api/motor/action", R"({"action":"happy"})").status == 200 && app.calls == before);
    assert(Request("/api/motor/status", "", HTTP_GET).response.find("no_motion_sensor") != std::string::npos);
    app.config.wiggle_duration_ms = 800;
    assert(Request("/api/config", R"({"forward_ms":1200,"def_speed_pct":60})").status == 200);
    assert(app.config.forward_duration_ms == 1200 && app.config.default_speed_percent == 60);
    assert(app.config.wiggle_duration_ms == 800 && app.config.quick_forward_duration_ms == 500);
    before = config_writes;
    for (const auto& body : {R"({"forward_ms":0})", R"({"def_speed_pct":101})", R"({"forward_ms":"1200"})", R"({"forward_ms":100.5})", "[]"}) {
        assert(Request("/api/config", body).status == 400);
    }
    assert(config_writes == before);
    assert(Request("/config", "forward_ms=1600&def_speed_pct=50").status == 200);
    assert(app.config.forward_duration_ms == 1600 && app.config.wiggle_duration_ms == 800);
    assert(Request("/config", "forward_ms=bad").status == 400);
    assert(Request("/config", "def_speed_pct=bad").status == 400);
    assert(app.config.default_speed_percent == 50);
    auto response = Request("/api/config", "", HTTP_GET);
    cJSON* json = cJSON_Parse(response.response.c_str());
    assert(json && cJSON_GetObjectItem(json, "wiggle_ms")->valueint == 800);
    assert(cJSON_GetObjectItem(json, "quick_fwd_ms")->valueint == 500);
    cJSON_Delete(json);
    std::cout << "PASS: production HTTP handlers, fragmented bodies, validation, heartbeat, debug replacement, status, partial config\n";
}
