/**
 * @file osal.c
 * @brief OS 抽象层实现：Linux 下用 CLOCK_MONOTONIC、nanosleep 与 pthread 条件变量
 *        替代 FreeRTOS 的 vTaskDelay / xSemaphoreTake / xSemaphoreGive。
 */
#include "osal.h"

#include <errno.h>
#include <time.h>

/**
 * @brief 获取单调时钟的毫秒时间戳。
 *
 * 使用 CLOCK_MONOTONIC：即使系统时间被手动修改也不会回退，
 * 适合用于超时计算（原工程的 1 ms SysTick 时基也由它替代）。
 */
uint64_t osal_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/**
 * @brief 毫秒级睡眠。
 *
 * nanosleep 被信号打断时会返回剩余时间，这里循环续睡，
 * 保证调用者确实睡满指定时长；ms <= 0 时直接返回。
 */
void osal_msleep(long ms)
{
    if (ms <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;

    /* 被 EINTR 打断时，req 会被更新为剩余时间，继续睡即可 */
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        /* retry with the remaining time */
    }
}

/**
 * @brief 初始化二值/计数信号量。
 * @param sem   信号量对象
 * @param value 初始计数，0 表示尚未有事件
 */
void osal_sem_init(osal_sem_t *sem, int value)
{
    pthread_mutex_init(&sem->mutex, NULL);
    pthread_cond_init(&sem->cond, NULL);
    sem->value = value;
}

/**
 * @brief 释放信号量（对应 xSemaphoreGive）。
 *
 * 加锁后计数加一，并唤醒一个正在等待的线程。
 */
void osal_sem_post(osal_sem_t *sem)
{
    pthread_mutex_lock(&sem->mutex);
    sem->value++;
    pthread_cond_signal(&sem->cond);
    pthread_mutex_unlock(&sem->mutex);
}

/**
 * @brief 等待信号量（对应 xSemaphoreTake）。
 *
 * @param sem        信号量对象
 * @param timeout_ms 超时毫秒数；<0 表示永久等待
 * @return 0=成功；-1=超时或等待出错
 */
int osal_sem_wait(osal_sem_t *sem, long timeout_ms)
{
    pthread_mutex_lock(&sem->mutex);

    if (timeout_ms < 0) {
        /* 永久等待：只要计数为 0 就继续睡 */
        while (sem->value == 0) {
            pthread_cond_wait(&sem->cond, &sem->mutex);
        }
    } else {
        /* 计算绝对超时时间：pthread_cond_timedwait 默认使用 CLOCK_REALTIME */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        /* 纳秒进位处理 */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        /* 等待直到计数非 0、超时或出错；被信号打断时重试 */
        while (sem->value == 0) {
            int rc = pthread_cond_timedwait(&sem->cond, &sem->mutex, &deadline);
            if (rc == EINTR) {
                continue;
            }
            if (rc != 0) {
                /* 超时（ETIMEDOUT）或其他错误：返回失败 */
                pthread_mutex_unlock(&sem->mutex);
                return -1;
            }
        }
    }

    /* 成功获取：计数减一；二值信号量不会降到负数 */
    sem->value--;
    pthread_mutex_unlock(&sem->mutex);
    return 0;
}

/**
 * @brief 销毁信号量，释放 pthread 内部资源。
 */
void osal_sem_destroy(osal_sem_t *sem)
{
    pthread_cond_destroy(&sem->cond);
    pthread_mutex_destroy(&sem->mutex);
}
