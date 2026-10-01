// Host shim: "mounting" the card maps the mount point onto a host directory
// (see vfs_redirect.h); it fails while no card is inserted.
#pragma once
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    int allocation_unit_size;
} esp_vfs_fat_mount_config_t;

esp_err_t esp_vfs_fat_sdmmc_mount(const char* base_path, const sdmmc_host_t* host,
                                  const void* slot_config, const esp_vfs_fat_mount_config_t* cfg,
                                  sdmmc_card_t** out_card);
esp_err_t esp_vfs_fat_sdcard_unmount(const char* base_path, sdmmc_card_t* card);
