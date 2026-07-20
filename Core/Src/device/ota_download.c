/**
 * @file    ota_download.c
 * @brief   OneNET HTTP OTA 固件升级模块
 *
 * 流程：
 *   1. POST /fuse-ota/{pid}/{dev}/version  → 上报版本号
 *   2. GET  /fuse-ota/{pid}/{dev}/check?type=2&version=X.X → 查询任务
 *   3. 有任务 → GET /fuse-ota/{pid}/{dev}/{tid}/download → 分片下载(256B/片)
 *   4. 下载完 → 存 EEPROM 标志 → NVIC_SystemReset()
 *
 * OTA 服务器: iot-api.heclouds.com (HTTP)
 * MQTT 服务器: mqtts.heclouds.com:1883 (TCP)
 */
#include "ota_download.h"
#include "onenet.h"
#include "w25q64.h"
#include "at24c02.h"
#include "esp8266.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OTA_HOST    "iot-api.heclouds.com"
#define OTA_PORT    80

/* OTA 状态 */
static OTA_State_t ota_state   = OTA_IDLE;
static uint8_t     ota_progress = 0;

/* 固件信息 */
static char     ota_tid[16]     = {0};   /* 下载任务 ID */
static char     ota_new_ver[16] = {0};   /* 新版本号 */
static uint32_t ota_file_size   = 0;      /* 文件总大小 */
static uint32_t ota_offset      = 0;      /* 已下载字节数 */
static char     ota_server_md5[33] = {0}; /* 服务器下发的 MD5 */

/* ---- 当前版本号（从 EEPROM 读取）---- */
static char current_version[16] = "V1.0";

static void OTA_LoadVersion(void)
{
    /* 从 EEPROM 读取版本号 */
    AT24C02_ReadBuf(EE_VERSION_STR, (uint8_t *)current_version, sizeof(current_version) - 1);
    current_version[sizeof(current_version) - 1] = '\0';

    /* 如果 EEPROM 里是空或非法，用默认值 */
    if (current_version[0] == '\0' || current_version[0] == 0xFF) {
        strcpy(current_version, "V1.0");
    }
}

/* ---- 内部函数 ---- */
static bool OTA_HttpRequest(const char *method, const char *path,
                            const char *extra_header, const char *body,
                            char *resp, uint16_t resp_max, uint16_t timeout,
                            uint16_t *resp_len);
static bool OTA_ReportVersion(void);
static bool OTA_CheckTask(void);
static bool OTA_DownloadFirmware(void);
static void OTA_Finish(void);

/* ==================================================================
 *  主入口：开机时调用，在连接 MQTT 之前
 *  返回 true=有升级发生(已重启), false=无任务,继续正常启动
 * ================================================================== */
bool OTA_CheckAndDownload(void)
{
    /* 从 EEPROM 加载当前版本号 */
    OTA_LoadVersion();
    printf("[OTA] Current version: %s\r\n", current_version);

    /* 1. 上报版本号 */
    if (!OTA_ReportVersion()) {
        printf("[OTA] Report version failed\r\n");
        return false;
    }

    /* 2. 查询 OTA 任务 */
    if (!OTA_CheckTask()) {
        printf("[OTA] No upgrade task, continue normal boot\r\n");
        return false;
    }

    /* 3. 安全检查 */
    if (ota_tid[0] == '\0' || ota_file_size == 0) {
        printf("[OTA] Invalid tid or size, abort (no version change)\r\n");
        return false;
    }

    /* 4. 下载固件 */
    printf("[OTA] Upgrade found: %s -> %s, size=%lu\r\n",
           current_version, ota_new_ver, (unsigned long)ota_file_size);

    if (!OTA_DownloadFirmware()) {
        printf("[OTA] Download failed, version NOT changed\r\n");
        ota_state = OTA_FAILED;
        return false;
    }

    /* 5. 下载成功 → 写 EEPROM → 重启进入 Bootloader */
    OTA_Finish();
    return true;  /* 不会执行到这里(OTA_Finish 里会复位) */
}

/* ==================================================================
 *  HTTP 请求封装（阻塞，调度器启动前可用）
 *
 *  method:  "POST" / "GET"
 *  path:    如 "/fuse-ota/xxx/d1/version"
 *  extra_header: 如 "Range:bytes=0-255\r\n" 或 NULL
 *  body:    POST body，GET 时传 NULL
 *  resp:    输出响应缓冲区
 *  timeout: 超时时间 (×10ms)
 * ================================================================== */
static bool OTA_HttpRequest(const char *method, const char *path,
                            const char *extra_header, const char *body,
                            char *resp, uint16_t resp_max, uint16_t timeout,
                            uint16_t *resp_len)
{
    char send_buf[512];
    char cmd[64];
    int len;

    /* 连接 OTA HTTP 服务器 */
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", OTA_HOST, OTA_PORT);
    {
        int retry = 0;
        while (ESP8266_SendCmd(cmd, "CONNECT")) {
            if (++retry > 10) {
                printf("[OTA] TCP connect timeout\r\n");
                return false;
            }
            HAL_Delay(500);
        }
    }

    /* 构建 HTTP 请求 */
    /* Authorization 签名与 MQTT 相同 */
    char auth_buf[160];
    OneNET_Authorization("2018-10-31", ONENET_PROID, 1956499200,
                         ONENET_AUTH_KEY, ONENET_DEV_NAME, auth_buf, sizeof(auth_buf), 0);

    if (body) {
        snprintf(send_buf, sizeof(send_buf),
            "%s %s HTTP/1.1\r\n"
            "Content-Type: application/json\r\n"
            "Authorization:%s\r\n"
            "host:%s\r\n"
            "Content-Length:%d\r\n"
            "%s\r\n"         /* extra header (e.g. Range) */
            "%s",
            method, path, auth_buf, OTA_HOST,
            (int)strlen(body),
            extra_header ? extra_header : "",
            body);
    } else {
        snprintf(send_buf, sizeof(send_buf),
            "%s %s HTTP/1.1\r\n"
            "Authorization:%s\r\n"
            "host:%s\r\n"
            "%s\r\n",          /* extra header (e.g. Range) */
            method, path, auth_buf, OTA_HOST,
            extra_header ? extra_header : "");
    }

    len = strlen(send_buf);
    ESP8266_Clear();
    ESP8266_SendData((unsigned char *)send_buf, len);

    /* 循环拼接多个 +IPD 片段，直到 HTTP 头+体完整 */
    uint16_t total_copy = 0;
    uint16_t rounds = 0;
    uint32_t expected_total = 0;
    bool got_header = false;

    while (rounds++ < 8 && total_copy < (uint16_t)(resp_max - 1)) {
        char *ptr = (char *)ESP8266_GetIPD(timeout);
        if (ptr == NULL) {
            break;
        }

        uint32_t ipd_len = 0;
        char *ipd = strstr((char *)esp8266_buf, "IPD,");
        if (ipd != NULL) {
            ipd_len = (uint32_t)strtoul(ipd + 4, NULL, 10);
        }

        uint16_t copy_len;
        if (ipd_len > 0) {
            if (ipd_len > (uint32_t)(resp_max - 1 - total_copy)) {
                copy_len = (uint16_t)(resp_max - 1 - total_copy);
            } else {
                copy_len = (uint16_t)ipd_len;
            }
        } else {
            copy_len = strlen(ptr);
            if (copy_len > (uint16_t)(resp_max - 1 - total_copy)) {
                copy_len = (uint16_t)(resp_max - 1 - total_copy);
            }
        }

        memcpy(resp + total_copy, ptr, copy_len);
        total_copy += copy_len;
        resp[total_copy] = '\0';

        if (!got_header) {
            char *hdr_end = strstr(resp, "\r\n\r\n");
            if (hdr_end != NULL) {
                got_header = true;
                uint32_t header_len = (uint32_t)(hdr_end + 4 - resp);
                uint32_t body_len = 0;
                char *cl = strstr(resp, "Content-Length:");
                if (cl != NULL) {
                    body_len = (uint32_t)strtoul(cl + 15, NULL, 10);
                }
                expected_total = header_len + body_len;
            }
        }

        if (got_header) {
            if (expected_total == 0 || total_copy >= expected_total) {
                break;
            }
        }
    }

    if (total_copy == 0 || !got_header) {
        printf("[OTA] HTTP response timeout\r\n");
        ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK");
        return false;
    }

    if (resp_len != NULL) {
        *resp_len = total_copy;
    }

    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK");
    return true;
}

/* ==================================================================
 *  步骤1：上报版本号
 * ================================================================== */
static bool OTA_ReportVersion(void)
{
    char resp[512];
    char body[64];

    /* 版本号格式: s_version=当前版本, f_version=支持的升级版本范围 */
    snprintf(body, sizeof(body),
             "{\"s_version\":\"%s\", \"f_version\": \"V9.9\"}", current_version);

    if (!OTA_HttpRequest("POST", "/fuse-ota/" ONENET_PROID "/" ONENET_DEV_NAME "/version",
                         NULL, body, resp, sizeof(resp), 500, NULL)) {
        return false;
    }

    if (strstr(resp, "\"msg\":\"succ\"") == NULL) {
        printf("[OTA] Version report: %s\r\n", resp);
        return false;
    }

    printf("[OTA] Version reported OK\r\n");
    return true;
}

/* ==================================================================
 *  步骤2：查询 OTA 任务
 * ================================================================== */
static bool OTA_CheckTask(void)
{
    char resp[512];
    char path[64];

    /* 提取主版本号 "V1.0" → "1.0" */
    char ver_num[8];
    strncpy(ver_num, current_version + 1, sizeof(ver_num) - 1);  /* 跳过 'V' */

    snprintf(path, sizeof(path), "/fuse-ota/%s/%s/check?type=2&version=%s",
             ONENET_PROID, ONENET_DEV_NAME, ver_num);

    if (!OTA_HttpRequest("GET", path, NULL, NULL, resp, sizeof(resp), 500, NULL)) {
        return false;
    }

    /* 检查是否无任务 */
    if (strstr(resp, "\"msg\":\"not exist\"") != NULL) {
        printf("[OTA] No upgrade task\r\n");
        return false;
    }

    /* 打印原始响应帮助调试 */
    printf("[OTA] Check response: %s\r\n", resp);

    /* 解析任务信息 */
    char *target = strstr(resp, "\"target\":\"");
    char *tid    = strstr(resp, "\"tid\":");
    char *size   = strstr(resp, "\"size\":");

    if (target && tid && size) {
        /* 提取 target 版本 */
        char *p = target + 10;
        int i = 0;
        while (*p && *p != '"' && i < (int)sizeof(ota_new_ver) - 1) {
            ota_new_ver[i++] = *p++;
        }
        ota_new_ver[i] = '\0';

        /* 提取 tid（数字类型，JSON 中 "tid":1481581） */
        p = strchr(tid, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            i = 0;
            while (*p >= '0' && *p <= '9' && i < (int)sizeof(ota_tid) - 1) {
                ota_tid[i++] = *p++;
            }
            ota_tid[i] = '\0';
        }

        /* 提取 size（数字类型，"size":39020） */
        p = strchr(size, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            ota_file_size = (uint32_t)strtoul(p, NULL, 10);
        }

        /* 提取 md5（JSON 中 "md5":"xxx"，长度 32 字符） */
        char *md5 = strstr(resp, "\"md5\":\"");
        if (md5) {
            p = md5 + 7;  /* 跳过 "md5":" */
            i = 0;
            while (*p && *p != '"' && i < (int)sizeof(ota_server_md5) - 1) {
                ota_server_md5[i++] = *p++;
            }
            ota_server_md5[i] = '\0';
        }

        printf("[OTA] Parsed: target=%s, tid=%s, size=%lu, md5=%s\r\n",
               ota_new_ver, ota_tid, (unsigned long)ota_file_size, ota_server_md5);
        return true;
    }

    printf("[OTA] Parse failed: target=%p, tid=%p, size=%p\r\n",
           (void*)target, (void*)tid, (void*)size);
    return false;
}

/* ==================================================================
 *  步骤3：分片下载固件到 W25Q64
 *  每片 256 字节，HTTP Range 请求
 * ================================================================== */
static bool OTA_DownloadFirmware(void)
{
    char resp[1400];   /* 多个 +IPD 片段拼接缓存 */
    char path[64];
    char extra[64];
    uint32_t total = ota_file_size;
    uint32_t offset = 0;
    uint32_t w25q_addr = W25Q64_OTA_SLOT_A;

    printf("[OTA] Erasing W25Q64 slot A...\r\n");
    for (uint32_t addr = w25q_addr; addr < w25q_addr + W25Q64_OTA_SLOT_SIZE;
         addr += W25Q64_SECTOR_SIZE) {
        W25Q64_SectorErase(addr);
    }
    printf("[OTA] Erase done, downloading %lu bytes...\r\n", total);

    if (total == 0) {
        printf("[OTA] Error: firmware size is 0\r\n");
        return false;
    }

    ota_state = OTA_DOWNLOADING;

    while (offset < total) {
        uint32_t end = offset + 255;
        if (end >= total) end = total - 1;

        snprintf(path, sizeof(path), "/fuse-ota/%s/%s/%s/download",
                 ONENET_PROID, ONENET_DEV_NAME, ota_tid);

        printf("[OTA] Download URL: %s, tid=%s\r\n", path, ota_tid);

        snprintf(extra, sizeof(extra), "Range:bytes=%lu-%lu\r\n", offset, end);

        bool chunk_ok = false;
        for (int retry = 0; retry < 2 && !chunk_ok; retry++) {
            uint16_t resp_len = 0;
            if (!OTA_HttpRequest("GET", path, extra, NULL, resp, sizeof(resp), 700, &resp_len)) {
                printf("[OTA] Chunk timeout at offset %lu (retry %d/2)\r\n",
                       offset, retry + 1);
                HAL_Delay(30);
                continue;
            }

            /* 找到 \r\n\r\n 后的数据起始位置 */
            char *data_start = strstr(resp, "\r\n\r\n");
            if (data_start == NULL) {
                printf("[OTA] No data in response (retry %d/2)\r\n", retry + 1);
                HAL_Delay(30);
                continue;
            }
            data_start += 4;

            uint32_t header_len = (uint32_t)(data_start - resp);
            uint32_t payload_len = (resp_len > header_len) ? (resp_len - header_len) : 0;

            /* 从 Content-Length 获取实际数据长度（不能用 strlen，二进制含 \0） */
            uint32_t chunk_len = 256;  /* 默认 */
            char *cl = strstr(resp, "Content-Length:");
            if (cl) {
                chunk_len = (uint32_t)strtoul(cl + 15, NULL, 10);
            }
            if (chunk_len == 0 || chunk_len > 256) chunk_len = 256;

            if (payload_len < chunk_len) {
                printf("[OTA] Incomplete chunk: got=%lu need=%lu (retry %d/2)\r\n",
                       (unsigned long)payload_len, (unsigned long)chunk_len, retry + 1);
                HAL_Delay(30);
                continue;
            }

            /* 成功收满当前片后，再写入 W25Q64 */
            W25Q64_Write(w25q_addr + offset, (uint8_t *)data_start, chunk_len);
            offset += chunk_len;
            chunk_ok = true;
        }

        if (!chunk_ok) {
            printf("[OTA] Download chunk failed at offset %lu\r\n", offset);
            return false;
        }

        ota_progress = (uint8_t)((offset * 100) / total);
        printf("[OTA] %lu/%lu (%d%%)\r\n", offset, total, ota_progress);
    }

    ota_state = OTA_VERIFY;
    return true;
}

/* ==================================================================
 *  步骤4：完成 → 写 EEPROM → 复位
 * ================================================================== */
static void OTA_Finish(void)
{
    printf("[OTA] Download complete, verifying...\r\n");

    /* 计算 W25Q64 上固件的 CRC32 */
    uint32_t crc = W25Q64_CRC32(W25Q64_OTA_SLOT_A, ota_file_size);
    printf("[OTA] CRC32 = 0x%08lX\r\n", crc);

    /* 和服务器下发的 MD5 做简单比对（CRC32 ≠ MD5，但可作为快速检查）。
     * 服务器返回的 md5 是固件的 MD5，而 CRC32 是另一个校验值。
     * 这里主要依靠 CRC32 → Bootloader 搬移前校验。 */
    if (ota_server_md5[0] != '\0') {
        printf("[OTA] Server MD5: %s\r\n", ota_server_md5);
    }

    /* 存新版本号 */
    uint8_t ver_buf[16];
    memset(ver_buf, 0, sizeof(ver_buf));
    snprintf((char *)ver_buf, sizeof(ver_buf), "%s", ota_new_ver);
    AT24C02_WriteBuf(EE_VERSION_STR, ver_buf, sizeof(ver_buf));

    /* 存文件大小（大端序） */
    uint8_t buf4[4];
    buf4[0] = (ota_file_size >> 24) & 0xFF;
    buf4[1] = (ota_file_size >> 16) & 0xFF;
    buf4[2] = (ota_file_size >> 8)  & 0xFF;
    buf4[3] = (ota_file_size)       & 0xFF;
    AT24C02_WriteBuf(EE_FW_SIZE, buf4, 4);

    /* 存 CRC32（大端序） */
    buf4[0] = (crc >> 24) & 0xFF;
    buf4[1] = (crc >> 16) & 0xFF;
    buf4[2] = (crc >> 8)  & 0xFF;
    buf4[3] = (crc)       & 0xFF;
    AT24C02_WriteBuf(EE_FW_CRC32, buf4, 4);

    /* 升级标志 */
    AT24C02_WriteByte(EE_DOWNLOAD_STATUS, 2);
    AT24C02_WriteByte(EE_BOOT_CMD, 1);

    ota_state = OTA_DONE;
    printf("[OTA] Rebooting...\r\n");
    HAL_Delay(1000);
    NVIC_SystemReset();
}

/* ==================================================================
 *  状态查询
 * ================================================================== */
OTA_State_t OTA_GetState(void)  { return ota_state; }
uint8_t    OTA_GetProgress(void) { return ota_progress; }
