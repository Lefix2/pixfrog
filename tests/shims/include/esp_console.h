// esp_console subset: registered commands land in a table the harness runs
// with shim::console_exec() (no REPL task, no UART).
#pragma once
#include "esp_err.h"
#include <cstddef>
#include <cstdint>
typedef int (*esp_console_cmd_func_t)(int argc, char** argv);
typedef int (*esp_console_cmd_func_with_context_t)(void* ctx, int argc, char** argv);
typedef struct {
    const char* command;
    const char* help;
    const char* hint;
    esp_console_cmd_func_t func;
    void* argtable;
    esp_console_cmd_func_with_context_t func_w_context;
    void* context;
} esp_console_cmd_t;
typedef struct esp_console_repl_s {
    int unused;
} esp_console_repl_t;
typedef struct {
    uint32_t max_history_len;
    const char* history_save_path;
    uint32_t task_stack_size;
    uint32_t task_priority;
    int task_core_id;
    const char* prompt;
    size_t max_cmdline_length;
} esp_console_repl_config_t;
typedef struct {
    int channel;
    int baud_rate;
    int tx_gpio_num;
    int rx_gpio_num;
} esp_console_dev_uart_config_t;
#define ESP_CONSOLE_REPL_CONFIG_DEFAULT()                                                          \
    {}
#define ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT()                                                      \
    {}
esp_err_t esp_console_cmd_register(const esp_console_cmd_t* cmd);
esp_err_t esp_console_register_help_command();
esp_err_t esp_console_new_repl_uart(const esp_console_dev_uart_config_t* hw,
                                    const esp_console_repl_config_t* cfg, esp_console_repl_t** out);
esp_err_t esp_console_start_repl(esp_console_repl_t* repl);
