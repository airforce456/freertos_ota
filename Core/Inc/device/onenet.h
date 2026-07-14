#ifndef _ONENET_H_
#define _ONENET_H_

#include <stdbool.h>

/* OneNET 平台接入密钥（从平台控制台获取） */
#define ONENET_PROID       "9dc95AKVYS"
#define ONENET_AUTH_KEY    "RjNqbFNqSHBaRnA5UmlyRENsMDBYM3FMYTlYMk9Dck8="
#define ONENET_DEV_NAME    "d1"

/* 全局：设备注册后返回的 devid 和 key */
extern char onenet_devid[16];
extern char onenet_key[48];

bool OneNET_RegisterDevice(void);
bool OneNet_DevLink(void);
void OneNet_SendData(void);
void OneNET_Subscribe(void);
void OneNet_RevPro(unsigned char *cmd);

#endif
