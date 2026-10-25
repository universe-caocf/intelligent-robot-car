#include "compact_wifi_board.h"
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include <wifi_manager.h>

// Forward declaration for Application function
extern "C" void HandleMotorActionForApplication(int direction, int speed, int duration_ms, int priority);
extern "C" void HandleMotorActionForEmotion(const char* emotion);



#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <utility>
#include <string>
#include <stdexcept>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactWifiBoard"

// Global pointer to motor controller for Application callbacks
class CompactWifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                // Get the current board instance and call EnterWifiConfigMode
                auto board = static_cast<WifiBoard*>(&Board::GetInstance());
                board->EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([]() {
            Application::GetInstance().StopListening();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);
        // No longer using MotorController - all motor control goes through Application

        // Add motor control tools to MCP server
        auto& mcp_server = McpServer::GetInstance();

        // -1 是“使用当前保存配置”的缺省标记；每次调用时读取，网页修改后立即生效。
        struct MovementTool {
            const char* name;
            int direction;
            int car::MotorActionConfig::*duration;
        };
        const MovementTool movements[] = {
            {"move_forward", 4, &car::MotorActionConfig::forward_duration_ms},
            {"move_backward", 2, &car::MotorActionConfig::backward_duration_ms},
            {"turn_left", 3, &car::MotorActionConfig::left_turn_duration_ms},
            {"turn_right", 1, &car::MotorActionConfig::right_turn_duration_ms},
            {"spin_around", 3, &car::MotorActionConfig::spin_duration_ms},
            {"quick_forward", 4, &car::MotorActionConfig::quick_forward_duration_ms},
            {"quick_backward", 2, &car::MotorActionConfig::quick_backward_duration_ms},
        };
        for (const auto& movement : movements) {
            mcp_server.AddTool(std::string("self.motor.") + movement.name,
                "Start a timed movement, replacing any previous movement. "
                "speed_percent: -1 uses saved speed, 0 stops, otherwise 1..100. "
                "duration_ms: -1 uses saved duration, otherwise 100..10000ms. "
                "Returns accepted only; no measured distance, angle or completion feedback.",
                PropertyList({
                    Property("speed_percent", kPropertyTypeInteger, -1, -1, 100),
                    Property("duration_ms", kPropertyTypeInteger, -1, -1, car::kMaxDurationMs)
                }),
                [movement](const PropertyList& properties) -> ReturnValue {
                    auto& app = Application::GetInstance();
                    auto config = app.GetMotorActionConfig();
                    int speed = properties["speed_percent"].value<int>();
                    int duration = properties["duration_ms"].value<int>();
                    if (speed == -1) speed = config.default_speed_percent;
                    if (duration == -1) duration = config.*(movement.duration);
                    if (!app.HandleMotorActionWithDuration(movement.direction, speed, duration)) {
                        throw std::runtime_error("Movement rejected: check parameters, network and self.motor.get_status");
                    }
                    return std::string("Accepted ") + movement.name + " speed=" + std::to_string(speed) +
                        " duration_ms=" + std::to_string(duration) + "; physical completion unverified";
                });
        }
        for (const std::string pattern : {"wiggle", "dance"}) {
            mcp_server.AddTool("self.motor." + pattern,
                "Start a cancellable movement sequence. speed_percent: -1 uses saved speed; 0 stops; 1..100. "
                "New movement or stop cancels all remaining steps. Returns accepted, not physical completion.",
                PropertyList({Property("speed_percent", kPropertyTypeInteger, -1, -1, 100)}),
                [pattern](const PropertyList& properties) -> ReturnValue {
                    auto& app = Application::GetInstance();
                    int speed = properties["speed_percent"].value<int>();
                    if (speed == -1) speed = app.GetMotorActionConfig().default_speed_percent;
                    if (!app.HandleMotorPattern(pattern, speed)) throw std::runtime_error("Motor sequence rejected");
                    return "Accepted " + pattern + "; physical completion unverified";
                });
        }
        mcp_server.AddTool("self.motor.stop", "Stop chassis output and cancel all remaining movement steps",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                if (!Application::GetInstance().HandleMotorActionWithDuration(0, 0, 0))
                    throw std::runtime_error("Stop requested but motor hardware fault is latched; check status");
                return std::string("Chassis output stopped and pending steps canceled; no physical feedback sensor");
            });
        mcp_server.AddTool("self.motor.get_status", "Get commanded chassis state; this is not measured physical motion",
            PropertyList(), [](const PropertyList&) -> ReturnValue {
                return Application::GetInstance().GetMotorStatus();
            });

        // 保留已有工具名称兼容客户端，但情绪工具只更新屏幕，不驱动车轮。
        for (const std::string emotion : {"wake_up", "happy", "sad", "thinking", "listening", "speaking",
                "excited", "loving", "angry", "surprised", "confused"}) {
            mcp_server.AddTool("self.motor." + emotion, "Update display emotion only. Does not move the chassis.",
                PropertyList(), [emotion](const PropertyList&) -> ReturnValue {
                    HandleMotorActionForEmotion(emotion.c_str());
                    return std::string("Display update scheduled; chassis unchanged");
                });
        }

        mcp_server.AddTool("self.network.get_ip",
            "获取当前WiFi IP地址信息，用于语音播报或状态查询",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                auto& wifi = WifiManager::GetInstance();
                std::string ip = wifi.GetIpAddress();
                if (ip.empty()) {
                    return std::string("当前未连接到WiFi网络，无法获取IP地址");
                }
                return std::string("当前IP地址是") + ip;
            });
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

public:
    CompactWifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }
    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }


};

// 兼容旧工具名称，动作语义已改为显示；不会在回调里延迟或生成后续行驶。
extern "C" void HandleMotorActionForEmotion(const char* emotion) {
    if (!emotion) return;
    std::string value = emotion;
    if (value == "wake" || value == "wake_up") value = "sleepy";
    else if (value == "happy") value = "laughing";
    else if (value == "sad") value = "crying";
    else if (value == "excited") value = "shocked";
    else if (value == "loving") value = "kissy";
    Application::GetInstance().Schedule([value]() { Board::GetInstance().GetDisplay()->SetEmotion(value.c_str()); });
}

extern "C" void HandleMotorActionForDance(uint8_t speed_percent) {
    if (!Application::GetInstance().HandleMotorPattern("dance", speed_percent)) {
        throw std::runtime_error("Motor dance rejected");
    }
}

DECLARE_BOARD(CompactWifiBoard);
