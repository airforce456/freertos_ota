/**
 * 文件名： onenet.c
 * 说明：   OneNET MQTT 云平台数据交互（HAL 移植版）
 *         原 NET 包作者：张继瑞，适配 HAL + FreeRTOS
 */
#include "main.h"
#include "FreeRTOS.h"
#include "usart.h"
#include "esp8266.h"
#include "onenet.h"
#include "mqttkit.h"
#include "base64.h"
#include "hmac_sha1.h"
#include "cJSON.h"
#include "ota_download.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* MQTT malloc/free → FreeRTOS 堆 */
#define malloc  pvPortMalloc
#define free    vPortFree

char onenet_devid[16];
char onenet_key[48];

/* ---- 内部延时 ---- */
static void DelayXms(unsigned short ms) { HAL_Delay(ms); }

/* ==================================================================
 *  OTA_UrlEncode — sign 的 URL 编码
 * ================================================================== */
static unsigned char OTA_UrlEncode(char *sign)
{
    char sign_t[40];
    unsigned char i = 0, j = 0;
    unsigned char sign_len = strlen(sign);

    if (sign == NULL || sign_len < 28) return 1;

    for (; i < sign_len; i++) { sign_t[i] = sign[i]; sign[i] = 0; }
    sign_t[i] = 0;

    for (i = 0, j = 0; i < sign_len; i++) {
        switch (sign_t[i]) {
            case '+': strcat(sign + j, "%2B"); j += 3; break;
            case ' ': strcat(sign + j, "%20"); j += 3; break;
            case '/': strcat(sign + j, "%2F"); j += 3; break;
            case '?': strcat(sign + j, "%3F"); j += 3; break;
            case '%': strcat(sign + j, "%25"); j += 3; break;
            case '#': strcat(sign + j, "%23"); j += 3; break;
            case '&': strcat(sign + j, "%26"); j += 3; break;
            case '=': strcat(sign + j, "%3D"); j += 3; break;
            default:  sign[j] = sign_t[i];    j++;     break;
        }
    }
    sign[j] = 0;
    return 0;
}

/* ==================================================================
 *  OneNET_Authorization — 计算 MQTT 鉴权 Token
 * ================================================================== */
#define METHOD "sha1"

static unsigned char OneNET_Authorization(char *ver, char *res, unsigned int et,
    char *access_key, char *dev_name, char *auth_buf, unsigned short auth_len, bool flag)
{
    size_t olen = 0;
    char sign_buf[64], hmac_buf[64], acc_base64[64], str_sig[72];

    if (!ver || !res || et < 1564562581 || !access_key || !auth_buf || auth_len < 120)
        return 1;

    memset(acc_base64, 0, sizeof(acc_base64));
    BASE64_Decode((unsigned char *)acc_base64, sizeof(acc_base64), &olen,
                  (unsigned char *)access_key, strlen(access_key));

    memset(str_sig, 0, sizeof(str_sig));
    if (flag)
        snprintf(str_sig, sizeof(str_sig), "%d\n%s\nproducts/%s\n%s", et, METHOD, res, ver);
    else
        snprintf(str_sig, sizeof(str_sig), "%d\n%s\nproducts/%s/devices/%s\n%s",
                 et, METHOD, res, dev_name, ver);

    memset(hmac_buf, 0, sizeof(hmac_buf));
    hmac_sha1((unsigned char *)acc_base64, strlen(acc_base64),
              (unsigned char *)str_sig, strlen(str_sig), (unsigned char *)hmac_buf);

    olen = 0;
    memset(sign_buf, 0, sizeof(sign_buf));
    BASE64_Encode((unsigned char *)sign_buf, sizeof(sign_buf), &olen,
                  (unsigned char *)hmac_buf, strlen(hmac_buf));

    OTA_UrlEncode(sign_buf);

    if (flag)
        snprintf(auth_buf, auth_len, "version=%s&res=products%%2F%s&et=%d&method=%s&sign=%s",
                 ver, res, et, METHOD, sign_buf);
    else
        snprintf(auth_buf, auth_len,
                 "version=%s&res=products%%2F%s%%2Fdevices%%2F%s&et=%d&method=%s&sign=%s",
                 ver, res, dev_name, et, METHOD, sign_buf);
    return 0;
}

/* ==================================================================
 *  OneNET_RegisterDevice — 在 OneNET 平台注册设备
 * ================================================================== */
bool OneNET_RegisterDevice(void)
{
    bool result = true;
    unsigned short send_len = 11 + strlen(ONENET_DEV_NAME);
    char auth_buf[144], *data_ptr = NULL;
    static char send_buf[512];

    {
        int retry = 0;
        while (ESP8266_SendCmd("AT+CIPSTART=\"TCP\",\"mqtts.heclouds.com\",1883\r\n", "CONNECT")) {
            if (++retry > 10) {
                printf("[ONENET] Register TCP timeout\r\n");
                return 1;
            }
            DelayXms(500);
        }
    }

    OneNET_Authorization("2018-10-31", ONENET_PROID, 1956499200,
                         ONENET_AUTH_KEY, NULL, auth_buf, sizeof(auth_buf), 1);

    snprintf(send_buf, sizeof(send_buf),
        "POST /mqtt/v1/devices/reg HTTP/1.1\r\n"
        "Authorization:%s\r\n"
        "Host:ota.heclouds.com\r\n"
        "Content-Type:application/json\r\n"
        "Content-Length:%d\r\n\r\n"
        "{\"name\":\"%s\"}",
        auth_buf, send_len, ONENET_DEV_NAME);

    ESP8266_SendData((unsigned char *)send_buf, strlen(send_buf));
    data_ptr = (char *)ESP8266_GetIPD(250);

    if (data_ptr) data_ptr = strstr(data_ptr, "device_id");

    if (data_ptr) {
        char name[16]; int pid = 0;
        if (sscanf(data_ptr,
            "device_id\" : \"%[^\"]\",\r\n\"name\" : \"%[^\"]\",\r\n\r\n\"pid\" : %d,\r\n\"key\" : \"%[^\"]\"",
            onenet_devid, name, &pid, onenet_key) == 4) {
            printf("[ONENET] Registered: %s, %s, %d, %s\r\n", onenet_devid, name, pid, onenet_key);
            result = false;
        }
    }

    ESP8266_SendCmd("AT+CIPCLOSE\r\n", "OK");
    return result;
}

/* ==================================================================
 *  OneNet_DevLink — 建立 MQTT 连接
 * ================================================================== */
bool OneNet_DevLink(void)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};
    unsigned char *dataPtr;
    char auth_buf[160];
    bool status = 1;

    OneNET_Authorization("2018-10-31", ONENET_PROID, 1956499200,
                         ONENET_AUTH_KEY, ONENET_DEV_NAME, auth_buf, sizeof(auth_buf), 0);

    printf("[ONENET] DevLink: %s, %s\r\n", ONENET_DEV_NAME, ONENET_PROID);
    printf("[ONENET] Token: %s\r\n", auth_buf);

    {
        int retry = 0;
        while (ESP8266_SendCmd("AT+CIPSTART=\"TCP\",\"mqtts.heclouds.com\",1883\r\n", "CONNECT")) {
            if (++retry > 10) {
                printf("[ONENET] TCP connect timeout (retried %d times)\r\n", retry);
                return 1;
            }
            printf("[ONENET] TCP retry %d...\r\n", retry);
            DelayXms(500);
        }
    }

    if (MQTT_PacketConnect(ONENET_PROID, auth_buf, ONENET_DEV_NAME,
                           256, 1, MQTT_QOS_LEVEL0, NULL, NULL, 0, &mqttPacket) == 0) {
        ESP8266_SendData(mqttPacket._data, mqttPacket._len);
        dataPtr = ESP8266_GetIPD(250);

        if (dataPtr && MQTT_UnPacketRecv(dataPtr) == MQTT_PKT_CONNACK) {
            switch (MQTT_UnPacketConnectAck(dataPtr)) {
                case 0: printf("[ONENET] MQTT Connected!\r\n"); status = 0; break;
                case 1: printf("[ONENET] MQTT: protocol error\r\n"); break;
                case 2: printf("[ONENET] MQTT: bad clientid\r\n"); break;
                case 3: printf("[ONENET] MQTT: server error\r\n"); break;
                case 4: printf("[ONENET] MQTT: bad user/pass\r\n"); break;
                case 5: printf("[ONENET] MQTT: bad token\r\n"); break;
                default:printf("[ONENET] MQTT: unknown error\r\n"); break;
            }
        }
        MQTT_DeleteBuffer(&mqttPacket);
    } else {
        printf("[ONENET] MQTT_PacketConnect failed\r\n");
    }
    return status;
}

/* ---- 传感器数据（由 main.c 在上传前更新） ---- */
extern float onenet_ax_g, onenet_ay_g, onenet_az_g;
extern uint32_t onenet_count;

/* ==================================================================
 *  OneNet_FillBuf — 构建 JSON 数据包（上报 MPU6050 加速度）
 * ================================================================== */
static unsigned char OneNet_FillBuf(char *buf)
{
    char text[48];
    int ax = (int)(onenet_ax_g * 1000);
    int ay = (int)(onenet_ay_g * 1000);
    int az = (int)(onenet_az_g * 1000);

    strcpy(buf, "{\"id\":\"123\",\"params\":{");
	
	memset(text, 0, sizeof(text));
	sprintf(text, "\"led\":{\"value\":%s},",  "false");
	strcat(buf, text);
	
	memset(text, 0, sizeof(text));
	sprintf(text, "\"Target\":{\"value\":%d},", 2);
	strcat(buf, text);
		
	memset(text, 0, sizeof(text));
	sprintf(text, "\"temp1\":{\"value\":%d},", az);
	strcat(buf, text);
	
	memset(text, 0, sizeof(text));
	sprintf(text, "\"temp2\":{\"value\":%d},", 3);
	strcat(buf, text);
	
	memset(text, 0, sizeof(text));
	sprintf(text, "\"temp3\":{\"value\":%d}", 100);
	strcat(buf, text);
	
	strcat(buf, "}}");
    return strlen(buf);
}

/* ==================================================================
 *  OneNet_SendData — 上传数据到 OneNET
 * ================================================================== */
void OneNet_SendData(void)
{
    MQTT_PACKET_STRUCTURE mqttPacket = {NULL, 0, 0, 0};
    char buf[256];
    short body_len = 0, i = 0;

    memset(buf, 0, sizeof(buf));
    body_len = OneNet_FillBuf(buf);

    if (body_len) {
        if (MQTT_PacketSaveData(ONENET_PROID, ONENET_DEV_NAME,
                                body_len, NULL, &mqttPacket) == 0) {
            for (; i < body_len; i++)
                mqttPacket._data[mqttPacket._len++] = buf[i];
            ESP8266_SendData(mqttPacket._data, mqttPacket._len);
            printf("[ONENET] Sent %d bytes\r\n", mqttPacket._len);
            MQTT_DeleteBuffer(&mqttPacket);
        }
    }
}

/* ==================================================================
 *  OneNET_Subscribe — 订阅下行 Topic
 * ================================================================== */
void OneNET_Subscribe(void)
{
    MQTT_PACKET_STRUCTURE mqtt_packet = {NULL, 0, 0, 0};
    char topic_buf[80];
    const char *topic = topic_buf;

    /* 订阅属性下发 */
    snprintf(topic_buf, sizeof(topic_buf), "$sys/%s/%s/thing/property/set",
             ONENET_PROID, ONENET_DEV_NAME);
    printf("[ONENET] Subscribe: %s\r\n", topic_buf);
    if (MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID, MQTT_QOS_LEVEL0,
                             &topic, 1, &mqtt_packet) == 0) {
        ESP8266_SendData(mqtt_packet._data, mqtt_packet._len);
        MQTT_DeleteBuffer(&mqtt_packet);
    }

    /* 订阅 OTA 升级通知 */
    mqtt_packet._data = NULL; mqtt_packet._len = 0;
    snprintf(topic_buf, sizeof(topic_buf), "$sys/%s/%s/ota/firmware/get",
             ONENET_PROID, ONENET_DEV_NAME);
    printf("[ONENET] Subscribe: %s\r\n", topic_buf);
    if (MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID + 1, MQTT_QOS_LEVEL0,
                             &topic, 1, &mqtt_packet) == 0) {
        ESP8266_SendData(mqtt_packet._data, mqtt_packet._len);
        MQTT_DeleteBuffer(&mqtt_packet);
    }

    /* 订阅 OTA 数据分片 */
    mqtt_packet._data = NULL; mqtt_packet._len = 0;
    snprintf(topic_buf, sizeof(topic_buf), "$sys/%s/%s/ota/firmware/data",
             ONENET_PROID, ONENET_DEV_NAME);
    printf("[ONENET] Subscribe: %s\r\n", topic_buf);
    if (MQTT_PacketSubscribe(MQTT_SUBSCRIBE_ID + 2, MQTT_QOS_LEVEL0,
                             &topic, 1, &mqtt_packet) == 0) {
        ESP8266_SendData(mqtt_packet._data, mqtt_packet._len);
        MQTT_DeleteBuffer(&mqtt_packet);
    }
}

/* ==================================================================
 *  OneNet_RevPro — 处理平台下发的命令
 * ================================================================== */
void OneNet_RevPro(unsigned char *cmd)
{
    char *req_payload = NULL, *cmdid_topic = NULL;
    unsigned short topic_len = 0, req_len = 0;
    unsigned char qos = 0;
    static unsigned short pkt_id = 0;
    unsigned char type = MQTT_UnPacketRecv(cmd);

    switch (type) {
    case MQTT_PKT_PUBLISH:
        if (MQTT_UnPacketPublish(cmd, &cmdid_topic, &topic_len,
                                 &req_payload, &req_len, &qos, &pkt_id) == 0) {
            printf("[ONENET] Publish: topic=%s, payload=%s\r\n", cmdid_topic, req_payload);

            /* 先检查是否为 OTA 升级消息 */
            if (OTA_ProcessMessage(cmdid_topic, req_payload, req_len)) {
                break;  /* OTA 已处理，跳过其他解析 */
            }

            cJSON *raw = cJSON_Parse(req_payload);
            if (raw) {
                cJSON *params = cJSON_GetObjectItem(raw, "params");
                if (params) {
                    cJSON *led = cJSON_GetObjectItem(params, "led");
                    if (led) {
                        printf("[ONENET] CMD: led=%d\r\n", led->valueint);
                    }
                }
                cJSON_Delete(raw);
            }
        }
        break;

    case MQTT_PKT_PUBACK:
        if (MQTT_UnPacketPublishAck(cmd) == 0)
            printf("[ONENET] PUBACK OK\r\n");
        break;

    case MQTT_PKT_SUBACK:
        if (MQTT_UnPacketSubscribe(cmd) == 0)
            printf("[ONENET] SUBACK OK\r\n");
        break;

    default:
        break;
    }

    ESP8266_Clear();

    if (type == MQTT_PKT_PUBLISH || type == MQTT_PKT_CMD) {
        MQTT_FreeBuffer(cmdid_topic);
        MQTT_FreeBuffer(req_payload);
    }
}
