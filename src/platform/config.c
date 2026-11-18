/**
 * @file config.c
 * @brief 命令行参数解析：把原工程中的硬编码参数改为可在启动时覆盖。
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 全局配置默认值。
 * 这些值与移植前的 STM32 工程保持一致，保证默认行为不变。
 */
gateway_config_t g_config = {
    .broker_host = "192.168.45.48",
    .broker_port = 1883,
    .client_id = "mqtt_fz",
    .can_iface = "can0",
    .sub_topic = "console_to_gateway",
    .pub_topic = "gateway_to_console",
    .cycle_ms = 200,
};

/**
 * @brief 打印命令行帮助。
 * @param prog 程序名（argv[0]）
 */
void config_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --broker HOST       MQTT broker address (default: %s)\n", g_config.broker_host);
    printf("  --port PORT         MQTT broker port (default: %u)\n", g_config.broker_port);
    printf("  --client-id ID      MQTT client id (default: %s)\n", g_config.client_id);
    printf("  --can IFACE         SocketCAN interface (default: %s)\n", g_config.can_iface);
    printf("  --sub TOPIC         MQTT subscribe topic (default: %s)\n", g_config.sub_topic);
    printf("  --pub TOPIC         MQTT publish topic (default: %s)\n", g_config.pub_topic);
    printf("  --cycle MS          Delay before CAN receive, in ms (default: %ld)\n", g_config.cycle_ms);
    printf("  --help              Show this help\n");
}

/**
 * @brief 校验当前选项后面是否还有取值，并把索引推进到该取值。
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @param i    当前选项下标；成功时自增指向取值
 * @return 0=成功；-1=缺少取值
 */
static int need_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", argv[*i]);
        return -1;
    }
    (*i)++;
    return 0;
}

/**
 * @brief 逐个解析命令行选项。
 *
 * 支持的选项见 config_usage()。所有字符串参数直接指向 argv，
 * 因此其生命周期覆盖整个进程运行期。
 *
 * @return 0=成功；1=打印帮助；-1=参数错误
 */
int config_parse(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--broker") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            g_config.broker_host = argv[i];
        } else if (strcmp(argv[i], "--port") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            /* 端口必须落在合法范围 1~65535 */
            long port = strtol(argv[i], NULL, 10);
            if (port < 1 || port > 65535) {
                fprintf(stderr, "invalid port: %s\n", argv[i]);
                return -1;
            }
            g_config.broker_port = (uint16_t)port;
        } else if (strcmp(argv[i], "--client-id") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            g_config.client_id = argv[i];
        } else if (strcmp(argv[i], "--can") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            g_config.can_iface = argv[i];
        } else if (strcmp(argv[i], "--sub") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            g_config.sub_topic = argv[i];
        } else if (strcmp(argv[i], "--pub") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            g_config.pub_topic = argv[i];
        } else if (strcmp(argv[i], "--cycle") == 0) {
            if (need_value(argc, argv, &i) != 0) return -1;
            /* 周期限制在 0~60000 ms，避免异常值导致长时间等待 */
            long cycle = strtol(argv[i], NULL, 10);
            if (cycle < 0 || cycle > 60000) {
                fprintf(stderr, "invalid cycle: %s\n", argv[i]);
                return -1;
            }
            g_config.cycle_ms = cycle;
        } else if (strcmp(argv[i], "--help") == 0) {
            config_usage(argv[0]);
            return 1;
        } else {
            /* 未知选项：打印帮助并返回错误 */
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            config_usage(argv[0]);
            return -1;
        }
    }
    return 0;
}
