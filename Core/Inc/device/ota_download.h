/**
 * @file    ota_download.h
 * @brief   OneNET HTTP OTA 固件升级模块
 * @note    开机时在 main() 中调用 OTA_CheckAndDownload()，在 MQTT 连接之前。
 *          有升级任务则下载→写 EEPROM→复位，无任务则继续正常启动。
 */
#ifndef __OTA_DOWNLOAD_H__
#define __OTA_DOWNLOAD_H__

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    OTA_IDLE = 0,
    OTA_DOWNLOADING,
    OTA_VERIFY,
    OTA_DONE,
    OTA_FAILED
} OTA_State_t;

bool OTA_CheckAndDownload(void);
OTA_State_t OTA_GetState(void);
uint8_t    OTA_GetProgress(void);

#endif
