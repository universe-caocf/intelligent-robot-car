// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define SETTINGS_H
#include "runtime.h"
class Settings {
public:
    Settings(const std::string&, bool writable = false) : writable_(writable) {}
    int GetInt(const std::string& key, int fallback) { auto item = nvs.find(key); return item == nvs.end() ? fallback : item->second; }
    void SetInt(const std::string& key, int value) { if (!writable_) throw "read-only NVS write"; nvs[key] = value; ++nvs_writes; }
private:
    bool writable_;
};
