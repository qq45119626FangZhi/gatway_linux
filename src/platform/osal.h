/**
 * @file osal.h
 * @brief OS 抽象层：用 pthread 实现原 FreeRTOS 的延时与二值信号量。
 */
#ifndef __OSAL_H__
#define __OSAL_H__

#include <pthread.h>
#include <stdint.h>

/**
 * 基于互斥锁 + 条件变量实现的计数信号量。
 *
 * 原工程使用 FreeRTOS 二值信号量，这里通过 value 计数实现相同语义：
 *   - post 时 value 加一并唤醒等待者；
 *   - wait 时若 value 为 0 则阻塞，否则减一后返回。
 */
typedef struct {
    pthread_mutex_t mutex;  /* 保护 value 与条件等待 */
    pthread_cond_t cond;    /* 用于唤醒等待线程 */
    int value;              /* 当前可用资源计数 */
} osal_sem_t;

/* 初始化信号量，value 为初始计数（0 表示二值信号量的“未触发”状态） */
void osal_sem_init(osal_sem_t *sem, int value);

/* 释放信号量：计数加一并唤醒一个等待线程 */
void osal_sem_post(osal_sem_t *sem);

/*
 * 等待信号量。
 * @param timeout_ms 超时毫秒数；<0 表示永久等待
 * @return 0=成功获取；-1=超时或错误
 */
int osal_sem_wait(osal_sem_t *sem, long timeout_ms);

/* 销毁信号量内部的互斥锁与条件变量 */
void osal_sem_destroy(osal_sem_t *sem);

/* 获取单调递增的毫秒时间戳（不受系统时间调整影响） */
uint64_t osal_now_ms(void);

/* 毫秒级睡眠，被信号打断时自动续睡剩余时间 */
void osal_msleep(long ms);

#endif /* __OSAL_H__ */
