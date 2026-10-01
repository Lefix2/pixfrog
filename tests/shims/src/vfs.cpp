// SD card + FAT volume on the host: a directory stands for the card, and the
// device's mount point prefix is rewritten onto it (vfs_redirect.h).
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>

#include "esp_vfs_fat.h"
#include "shim_control.h"

namespace {
std::string g_root;           // host directory holding the card's files
bool g_inserted     = false;  // a card is in the slot
std::string g_mount = "/sdcard";

std::string map(const char* path) {
    if (!path) return {};
    const size_t n = g_mount.size();
    if (!g_root.empty() && std::strncmp(path, g_mount.c_str(), n) == 0 &&
        (path[n] == '/' || path[n] == '\0'))
        return g_root + (path + n);
    return path;
}
}  // namespace

namespace shim {
void sd_root(const std::string& host_dir) {
    g_root = host_dir;
}
void sd_insert(bool inserted) {
    g_inserted = inserted;
}
}  // namespace shim

extern "C" {
FILE* shim_fopen(const char* path, const char* mode) {
    return std::fopen(map(path).c_str(), mode);
}
DIR* shim_opendir(const char* path) {
    return ::opendir(map(path).c_str());
}
int shim_remove(const char* path) {
    return std::remove(map(path).c_str());
}
int shim_rename(const char* from, const char* to) {
    return std::rename(map(from).c_str(), map(to).c_str());
}
}

esp_err_t esp_vfs_fat_sdmmc_mount(const char* base_path, const sdmmc_host_t*, const void*,
                                  const esp_vfs_fat_mount_config_t*, sdmmc_card_t** out_card) {
    static sdmmc_card_t card;
    if (!g_inserted || g_root.empty()) return ESP_FAIL;
    g_mount = base_path;
    if (out_card) *out_card = &card;
    return ESP_OK;
}
esp_err_t esp_vfs_fat_sdcard_unmount(const char*, sdmmc_card_t*) {
    return ESP_OK;
}
esp_err_t sdmmc_get_status(sdmmc_card_t*) {
    return g_inserted ? ESP_OK : ESP_FAIL;
}
