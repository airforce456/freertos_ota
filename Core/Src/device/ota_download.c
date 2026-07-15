/**
 * @file    ota_download.c
 * @brief   OneNET MQTT OTA 固件下载模块实现
 *
 * 流程：
 *   1. 收到 $sys/.../ota/firmware/get → 提取固件信息 → 回复确认
 *   2. 收到 $sys/.../ota/firmware/data → 写 W25Q64 → 回复分片确认
 *   3. 全部收完 → CRC32 校验 → 写 EEPROM → NVIC_SystemReset
 *
 * 参考 OneNET 文档：https://open.iot.10086.cn/doc/mqtt/book/device-manager/ota.html
 */
#include "ota_download.h"
#include "onenet.h"
#include "w25q64.h"
#include "at24c02.h"
#include "esp8266.h"
#include "mqttkit.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* APP 区最大固件大小（与 boot_flash.h 保持一致） */
#ifndef APP_MAX_SIZE
#define APP_MAX_SIZE  (44 * 1024)
#endif

/* ---- OTA 状态 ---- */
static OTA_State_t ota_state = OTA_IDLE;
static uint8_t     ota_progress = 0;

/* ---- 固件信息 ---- */
static uint32_t ota_fw_size   = 0;      /* 固件总大小 */
static uint32_t ota_fw_offset = 0;      /* 当前已写入字节数 */
static uint32_t ota_fw_crc    = 0;      /* 平台下发的 CRC32 */
static char     ota_fw_version[32] = {0};
static uint32_t ota_w25q_addr = 0;      /* W25Q64 写入起始地址 (Slot A) */

/* ---- 内部函数声明 ---- */
static void OTA_ReplyGet(const char *msg_id, int code);
static void OTA_ReplyData(const char *msg_id, int code, uint32_t offset);
static void OTA_StartDownload(const char *msg_id, uint32_t size, uint32_t crc,
                              const char *version);
static void OTA_WriteChunk(uint32_t offset, const uint8_t *data, uint16_t len);
static void OTA_Finish(void);

/* ==================================================================
 *  主入口：由 OneNet_RevPro 调用
 *  检查是否为 OTA 相关 Topic，是则处理，不是则返回 false
 * ================================================================== */
bool OTA_ProcessMessage(const char *topic, const char *payload, uint16_t len)
{
    if (topic == NULL || payload == NULL) return false;

    /* 检查 Topic 类型 */
    if (strstr(topic, "ota/firmware/get")) {
        /* 平台下发升级通知 */
        printf("[OTA] Upgrade notification received\r\n");

        cJSON *root = cJSON_Parse(payload);
        if (root == NULL) {
            printf("[OTA] JSON parse failed\r\n");
            return true;
        }

        /* 提取消息 ID */
        cJSON *id_item = cJSON_GetObjectItem(root, "id");
        const char *msg_id = id_item ? id_item->valuestring : "";

        /* 提取固件信息 */
        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (data) {
            cJSON *size_item = cJSON_GetObjectItem(data, "size");
            cJSON *crc_item  = cJSON_GetObjectItem(data, "sign");
            cJSON *ver_item  = cJSON_GetObjectItem(data, "version");

            uint32_t fw_size = size_item ? (uint32_t)size_item->valueint : 0;
            uint32_t fw_crc  = crc_item  ? (uint32_t)crc_item->valueint  : 0;
            const char *ver  = ver_item  ? ver_item->valuestring         : "unknown";

            if (fw_size == 0 || fw_size > APP_MAX_SIZE) {
                printf("[OTA] Invalid firmware size: %lu\r\n", fw_size);
                OTA_ReplyGet(msg_id, 2);  /* 拒绝：大小不合法 */
            } else {
                OTA_StartDownload(msg_id, fw_size, fw_crc, ver);
            }
        } else {
            OTA_ReplyGet(msg_id, 2);
        }

        cJSON_Delete(root);
        return true;
    }

    if (strstr(topic, "ota/firmware/data")) {
        /* 平台下发固件数据分片 */
        if (ota_state != OTA_DOWNLOADING) {
            printf("[OTA] Data received but not in download state\r\n");
            return true;
        }

        cJSON *root = cJSON_Parse(payload);
        if (root == NULL) return true;

        cJSON *id_item   = cJSON_GetObjectItem(root, "id");
        cJSON *data_item = cJSON_GetObjectItem(root, "data");
        cJSON *off_item  = cJSON_GetObjectItem(root, "offset");
        cJSON *len_item  = cJSON_GetObjectItem(root, "len");

        const char *msg_id = id_item ? id_item->valuestring : "";
        uint32_t offset = off_item ? (uint32_t)off_item->valueint : 0;
        uint32_t chunk_len = len_item ? (uint32_t)len_item->valueint : 0;

        if (data_item && data_item->valuestring && chunk_len > 0) {
            OTA_WriteChunk(offset, (const uint8_t *)data_item->valuestring, chunk_len);
            OTA_ReplyData(msg_id, 0, offset + chunk_len);
        } else {
            OTA_ReplyData(msg_id, 1, offset);
        }

        cJSON_Delete(root);
        return true;
    }

    return false;  /* 非 OTA 消息 */
}

/* ==================================================================
 *  回复升级通知
 *  code: 0=接受, 1=设备忙, 2=拒绝
 * ================================================================== */
static void OTA_ReplyGet(const char *msg_id, int code)
{
    MQTT_PACKET_STRUCTURE pkt = {NULL, 0, 0, 0};
    char topic[64];
    char payload[128];

    snprintf(topic, sizeof(topic), "$sys/%s/%s/ota/firmware/get_reply",
             ONENET_PROID, ONENET_DEV_NAME);

    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"code\":%d}", msg_id, code);

    if (MQTT_PacketSaveData(ONENET_PROID, ONENET_DEV_NAME,
                            strlen(payload), NULL, &pkt) == 0) {
        memcpy(pkt._data + pkt._len, payload, strlen(payload));
        pkt._len += strlen(payload);
        ESP8266_SendData(pkt._data, pkt._len);
        MQTT_DeleteBuffer(&pkt);
    }

    printf("[OTA] Reply get: code=%d\r\n", code);
}

/* ==================================================================
 *  回复数据分片
 *  code: 0=成功, 1=失败
 * ================================================================== */
static void OTA_ReplyData(const char *msg_id, int code, uint32_t offset)
{
    MQTT_PACKET_STRUCTURE pkt = {NULL, 0, 0, 0};
    char topic[64];
    char payload[128];

    snprintf(topic, sizeof(topic), "$sys/%s/%s/ota/firmware/data_reply",
             ONENET_PROID, ONENET_DEV_NAME);

    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"code\":%d,\"offset\":%lu}",
             msg_id, code, offset);

    if (MQTT_PacketSaveData(ONENET_PROID, ONENET_DEV_NAME,
                            strlen(payload), NULL, &pkt) == 0) {
        memcpy(pkt._data + pkt._len, payload, strlen(payload));
        pkt._len += strlen(payload);
        ESP8266_SendData(pkt._data, pkt._len);
        MQTT_DeleteBuffer(&pkt);
    }
}

/* ==================================================================
 *  开始下载：擦除 W25Q64 OTA Slot，初始化状态
 * ================================================================== */
static void OTA_StartDownload(const char *msg_id, uint32_t size, uint32_t crc,
                              const char *version)
{
    printf("[OTA] Starting download: size=%lu, version=%s\r\n", size, version);

    ota_fw_size   = size;
    ota_fw_offset = 0;
    ota_fw_crc    = crc;
    ota_progress  = 0;
    ota_state     = OTA_DOWNLOADING;
    ota_w25q_addr = W25Q64_OTA_SLOT_A;
    strncpy(ota_fw_version, version, sizeof(ota_fw_version) - 1);

    /* 擦除 OTA Slot A（60KB，15个 4KB 扇区） */
    printf("[OTA] Erasing W25Q64 slot A...\r\n");
    for (uint32_t addr = ota_w25q_addr; addr < ota_w25q_addr + W25Q64_OTA_SLOT_SIZE; addr += W25Q64_SECTOR_SIZE) {
        W25Q64_SectorErase(addr);
    }
    printf("[OTA] Erase done\r\n");

    /* 回复接受升级 */
    OTA_ReplyGet(msg_id, 0);
}

/* ==================================================================
 *  写入一个数据分片到 W25Q64
 * ================================================================== */
static void OTA_WriteChunk(uint32_t offset, const uint8_t *data, uint16_t len)
{
    if (offset + len > ota_fw_size) {
        printf("[OTA] Chunk overflow: off=%lu len=%u fw_size=%lu\r\n",
               offset, len, ota_fw_size);
        return;
    }

    W25Q64_Write(ota_w25q_addr + offset, data, len);
    ota_fw_offset = offset + len;

    /* 更新进度 */
    ota_progress = (uint8_t)((ota_fw_offset * 100) / ota_fw_size);

    printf("[OTA] Progress: %lu/%lu (%d%%)\r\n",
           ota_fw_offset, ota_fw_size, ota_progress);

    /* 检查是否下载完成 */
    if (ota_fw_offset >= ota_fw_size) {
        OTA_Finish();
    }
}

/* ==================================================================
 *  下载完成：CRC32 校验 → 写 EEPROM → 复位
 * ================================================================== */
static void OTA_Finish(void)
{
    printf("[OTA] Download complete. Verifying CRC32...\r\n");
    ota_state = OTA_VERIFY;

    uint32_t calc_crc = W25Q64_CRC32(ota_w25q_addr, ota_fw_size);

    if (calc_crc != ota_fw_crc) {
        printf("[OTA] CRC mismatch! calc=0x%08lX expected=0x%08lX\r\n",
               calc_crc, ota_fw_crc);
        ota_state = OTA_FAILED;
        return;
    }

    printf("[OTA] CRC OK. Writing EEPROM flags...\r\n");

    /* 写 EEPROM 升级标志 */
    uint8_t slot = 0;  /* Slot A */
    AT24C02_WriteByte(EE_FW_SLOT, slot);

    /* FW_Size (4 bytes, big-endian) */
    uint8_t buf4[4];
    buf4[0] = (ota_fw_size >> 24) & 0xFF;
    buf4[1] = (ota_fw_size >> 16) & 0xFF;
    buf4[2] = (ota_fw_size >> 8)  & 0xFF;
    buf4[3] = (ota_fw_size)       & 0xFF;
    AT24C02_WriteBuf(EE_FW_SIZE, buf4, 4);

    /* FW_CRC32 */
    buf4[0] = (ota_fw_crc >> 24) & 0xFF;
    buf4[1] = (ota_fw_crc >> 16) & 0xFF;
    buf4[2] = (ota_fw_crc >> 8)  & 0xFF;
    buf4[3] = (ota_fw_crc)       & 0xFF;
    AT24C02_WriteBuf(EE_FW_CRC32, buf4, 4);

    /* Download_Status = 2 (完成) */
    AT24C02_WriteByte(EE_DOWNLOAD_STATUS, 2);

    /* BootCmd = 1 (OTA 升级) */
    AT24C02_WriteByte(EE_BOOT_CMD, 1);

    ota_state = OTA_DONE;

    printf("[OTA] Flags written. Rebooting in 2 seconds...\r\n");
    HAL_Delay(2000);
    NVIC_SystemReset();
}

/* ==================================================================
 *  状态查询
 * ================================================================== */
OTA_State_t OTA_GetState(void)
{
    return ota_state;
}

uint8_t OTA_GetProgress(void)
{
    return ota_progress;
}
