/**
 * @file App_Main.h
 * @brief 网关应用层对外接口：全局运行标志与业务主流程入口。
 */
#ifndef __APP_MAIN_H__
#define __APP_MAIN_H__

#include <signal.h>

/* 全局运行标志，定义在 main.c，供各线程轮询退出条件 */
extern volatile sig_atomic_t g_running;

/* 创建三个业务线程，并阻塞等待它们全部退出 */
void App_Main(void);

/* MQTT 消息回调中调用：解析下行 JSON，并唤醒 CAN 下发线程 */
void json_data_handler(char *json_string);

#endif /* __APP_MAIN_H__ */
