/**
 * @file    ota_download.h
 * @brief   OneNET MQTT OTA 固件下载模块
 * @note    复用 onenet.c 的 MQTT 连接、w25q64.c 的 Flash 存储、at24c02.c 的 EEPROM。
 *          在 OneNET 任务中调用，不创建独立任务。
 */
#ifndef __OTA_DOWNLOAD_H__
#define __OTA_DOWNLOAD_H__

#include <stdint.h>
#include <stdbool.h>

/* OTA 状态 */
typedef enum {
    OTA_IDLE = 0,        /* 空闲 */
    OTA_DOWNLOADING,     /* 下载中 */
    OTA_VERIFY,          /* 校验中 */
    OTA_DONE,            /* 完成，待重启 */
    OTA_FAILED           /* 失败 */
} OTA_State_t;

/**
 * @brief  处理 OneNET 下发的 MQTT 消息
 * @param  topic   消息 Topic
 * @param  payload 消息内容
 * @param  len     内容长度
 * @note   在 OneNet_RevPro() 中调用，检查是否为 OTA 相关消息。
 *         是则内部处理并返回 true，否则返回 false 让调用者继续处理。
 * @return true=已处理（OTA消息），false=非OTA消息
 */
bool OTA_ProcessMessage(const char *topic, const char *payload, uint16_t len);

/**
 * @brief  获取当前 OTA 状态
 */
OTA_State_t OTA_GetState(void);

/**
 * @brief  获取下载进度（0~100）
 */
uint8_t OTA_GetProgress(void);

#endif /* __OTA_DOWNLOAD_H__ */
