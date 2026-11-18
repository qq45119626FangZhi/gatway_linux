/**
 * @file mqtt_interface.h
 * @brief Paho MQTT 平台适配接口：Timer 与 Network 的 Linux 实现声明。
 *
 * 该文件保持与原 Paho/WIZnet 移植层相同的类型与函数签名，
 * 使第三方 MQTTClient 源码无需修改即可在 Linux 上编译使用。
 */
#ifndef __MQTT_INTERFACE_H_
#define __MQTT_INTERFACE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* socket 操作返回值约定 */
#define SOCK_OK 0
#define SOCK_ERROR (-1)

/*
 * 保留给原 Paho/WIZnet 移植层的 1 ms 定时器入口。
 * Linux 下 Timer 直接读取 CLOCK_MONOTONIC，因此该函数为空实现。
 */
void MilliTimer_Handler(void);

/* Paho 定时器对象：记录超时截止时间（毫秒） */
typedef struct Timer Timer;
struct Timer {
    unsigned long systick_period;  /* 原 SysTick 周期，Linux 下未使用 */
    unsigned long end_time;        /* 超时截止时间戳（osal_now_ms） */
};

/* Paho 网络对象：socket 句柄及读/写/断开函数指针 */
typedef struct Network Network;
struct Network {
    int my_socket;                                          /* TCP socket 句柄 */
    int (*mqttread)(Network *, unsigned char *, int, long);  /* 带超时读取 */
    int (*mqttwrite)(Network *, unsigned char *, int, long); /* 带超时写入 */
    void (*disconnect)(Network *);                           /* 断开连接 */
};

/* 初始化定时器 */
void TimerInit(Timer *timer);

/* 判断定时器是否已到期，返回 1=到期，0=未到期 */
char TimerIsExpired(Timer *timer);

/* 设置毫秒级倒计时 */
void TimerCountdownMS(Timer *timer, unsigned int timeout);

/* 设置秒级倒计时 */
void TimerCountdown(Timer *timer, unsigned int timeout);

/* 返回距离到期的剩余毫秒数，已到期返回 0 */
int TimerLeftMS(Timer *timer);

/* 从 socket 读取最多 len 字节，超时返回已读字节数或 SOCK_ERROR */
int linux_read(Network *n, unsigned char *buffer, int len, long timeout_ms);

/* 向 socket 写入 len 字节，超时返回已写字节数或 SOCK_ERROR */
int linux_write(Network *n, unsigned char *buffer, int len, long timeout_ms);

/* 关闭 socket 并把句柄置为 -1 */
void linux_disconnect(Network *n);

/* 初始化 Network 对象并挂接 Linux 实现（sn 参数仅作兼容占位） */
void NewNetwork(Network *n, int sn);

/* 用 4 字节 IP 连接 Broker（兼容原接口） */
int ConnectNetwork(Network *n, uint8_t *ip, uint16_t port);

/* 用主机名或点分 IP 连接 Broker */
int ConnectNetworkHost(Network *n, const char *host, uint16_t port);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_INTERFACE_H_ */
