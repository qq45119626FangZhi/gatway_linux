/**
 * @file log.h
 * @brief 轻量日志宏：输出级别、函数名与线程 ID，便于多线程调试。
 */
#ifndef __LOG_H__
#define __LOG_H__

#include <pthread.h>
#include <stdio.h>

/**
 * 信息级日志宏。
 * 输出格式：[I][函数名][tid=线程ID] 消息
 * 自动刷新 stdout，保证程序异常退出时日志不丢失。
 */
#define LOGI(fmt, ...)                                                           \
    do {                                                                         \
        fprintf(stdout, "[I][%s][tid=%lu] " fmt "\n", __func__,                  \
                (unsigned long)pthread_self(), ##__VA_ARGS__);                   \
        fflush(stdout);                                                          \
    } while (0)

/**
 * 错误级日志宏。
 * 输出到 stderr，格式与 LOGI 相同。
 */
#define LOGE(fmt, ...)                                                           \
    do {                                                                         \
        fprintf(stderr, "[E][%s][tid=%lu] " fmt "\n", __func__,                  \
                (unsigned long)pthread_self(), ##__VA_ARGS__);                   \
        fflush(stderr);                                                          \
    } while (0)

#endif /* __LOG_H__ */
