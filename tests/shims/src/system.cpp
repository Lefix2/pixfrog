// esp_console / app description / OTA / restart shims.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "shim_control.h"

namespace {
std::map<std::string, esp_console_cmd_func_t> g_cmds;
int g_restarts  = 0;
int g_log_level = ESP_LOG_INFO;
}  // namespace

esp_err_t esp_console_cmd_register(const esp_console_cmd_t* cmd) {
    g_cmds[cmd->command] = cmd->func;
    return ESP_OK;
}
esp_err_t esp_console_register_help_command() {
    return ESP_OK;
}
esp_err_t esp_console_new_repl_uart(const esp_console_dev_uart_config_t*,
                                    const esp_console_repl_config_t*, esp_console_repl_t** out) {
    static esp_console_repl_t repl;
    *out = &repl;
    return ESP_OK;
}
esp_err_t esp_console_start_repl(esp_console_repl_t*) {
    return ESP_OK;
}

const esp_app_desc_t* esp_app_get_description() {
    static esp_app_desc_t d = [] {
        esp_app_desc_t a{};
        std::strcpy(a.version, "v0.0.0-host");
        std::strcpy(a.project_name, "pixfrog");
        std::strcpy(a.date, "Jan  1 2026");
        std::strcpy(a.time, "00:00:00");
        return a;
    }();
    return &d;
}
const char* esp_get_idf_version() {
    return "v5.5-host";
}
const esp_partition_t* esp_ota_get_running_partition() {
    static const esp_partition_t p = { "ota_0", 0x20000, 0x700000 };
    return &p;
}
void esp_restart() {
    ++g_restarts;
}
esp_reset_reason_t esp_reset_reason() {
    return ESP_RST_POWERON;
}
void esp_log_level_set(const char* tag, esp_log_level_t level) {
    if (tag && std::strcmp(tag, "*") == 0) g_log_level = level;
}

namespace shim {
int restarts() {
    return g_restarts;
}
int log_level() {
    return g_log_level;
}
int console_exec(const char* line, std::string* out) {
    std::vector<std::string> words;
    std::string cur;
    for (const char* p = line;; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!cur.empty()) words.push_back(cur);
            cur.clear();
            if (!*p) break;
        } else {
            cur += *p;
        }
    }
    if (words.empty()) return -1;
    auto it = g_cmds.find(words[0]);
    if (it == g_cmds.end()) return -2;
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(&w[0]);
    argv.push_back(nullptr);

    // Capture stdout through a temp file: the commands printf directly.
    std::fflush(stdout);
    FILE* tmp       = std::tmpfile();
    const int saved = dup(fileno(stdout));
    dup2(fileno(tmp), fileno(stdout));
    const int rc = it->second(static_cast<int>(words.size()), argv.data());
    std::fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);
    if (out) {
        out->clear();
        std::rewind(tmp);
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
            out->append(buf, n);
    }
    std::fclose(tmp);
    return rc;
}
}  // namespace shim
