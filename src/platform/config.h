/**
 * @file config.h
 * @brief 运行参数定义：Broker、CAN 接口、主题与周期等配置项。
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <stdint.h>

/**
 * 网关运行时配置。
 *
 * 默认值与移植前工程中的硬编码值保持一致，可通过命令行参数覆盖。
 */
typedef struct {
    const char *broker_host;  /* MQTT Broker 地址 */
    uint16_t broker_port;     /* MQTT Broker 端口 */
    const char *client_id;    /* MQTT 客户端 ID */
    const char *can_iface;    /* SocketCAN 接口名，如 can0 / vcan0 */
    const char *sub_topic;    /* 下行订阅主题 */
    const char *pub_topic;    /* 上行发布主题 */
    long cycle_ms;            /* 下发后到开始接收之间的延时（毫秒） */
} gateway_config_t;

/* 全局配置实例，定义在 config.c */
extern gateway_config_t g_config;

/*
 * 解析命令行参数。
 * @return 0=解析成功；1=已打印帮助；-1=参数错误
 */
int config_parse(int argc, char **argv);

/* 打印命令行帮助信息 */
void config_usage(const char *prog);

#endif /* __CONFIG_H__ */
