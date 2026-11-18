/**
 * @file App_Main.c
 * @brief 网关业务主流程：三个 pthread 线程协作完成 MQTT 下行 -> CAN 下行 -> CAN 上行 -> MQTT 上行。
 *
 * 数据流向：
 *
 *   控制台 --MQTT(console_to_gateway)--> mqtt_task
 *        --信号量 mqtt_semaphore--> gateway_to_motor_task
 *        --CAN(ID=1002)--> 电机
 *
 *   电机 --CAN(ID=1001)--> motor_to_gateway_task
 *        --MQTT(gateway_to_console)--> 控制台
 *
 * 线程模型（对应原 FreeRTOS 工程的三个任务）：
 *   1. mqtt_task             : 循环执行 MQTTYield 收包，回调中解析 JSON 并唤醒下发线程；
 *   2. gateway_to_motor_task : 等待 MQTT 信号量，把距离/速度打包成 CAN 帧发送；
 *   3. motor_to_gateway_task : 等待 CAN 信号量，读取电机状态帧并发布到 MQTT。
 *
 * 同步关系：
 *   - mqtt_semaphore : MQTT 收到有效指令后唤醒 CAN 下发线程；
 *   - can_semaphore  : CAN 下发完成后唤醒 CAN 上报线程；
 *   - g_data_mutex   : 保护 distance / speed 两个共享变量。
 */
#include "App_Main.h"

#include "Int_CAN.h"
#include "Int_MQTT.h"
#include "cJSON.h"
#include "config.h"
#include "log.h"
#include "osal.h"

#include <pthread.h>

/* MQTT 下行信号量：收到有效控制指令后由 mqtt_task 释放 */
static osal_sem_t mqtt_semaphore;

/* CAN 上行信号量：CAN 下发完成后由 gateway_to_motor_task 释放 */
static osal_sem_t can_semaphore;

/* 共享数据互斥锁：保护 distance 与 speed 的并发访问 */
static pthread_mutex_t g_data_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 下发给电机的目标距离（单位由电机协议决定，通常为 mm） */
static uint16_t distance = 0;

/* 下发给电机的最大速度 */
static uint16_t speed = 0;

/* 三个线程入口函数的前置声明 */
static void *mqtt_task(void *arg);
static void *gateway_to_motor_task(void *arg);
static void *motor_to_gateway_task(void *arg);

/* 可被 g_running 提前打断的分片睡眠 */
static void sleep_ms_stop_aware(long ms);

/* 把距离与状态打包成 JSON 并发布到 MQTT */
static void send_json_to_control(uint16_t dis, uint16_t state);

/**
 * @brief 初始化 CAN 与同步对象，创建三个业务线程并等待其结束。
 *
 * 该函数对应原 FreeRTOS 中的任务创建与启动流程，返回时表示所有线程均已退出。
 */
void App_Main(void)
{
    LOGI("motor gateway project start");

    /* 打开并绑定 SocketCAN 接口（接口名由命令行参数决定） */
    Int_CAN_Init();

    /* 初始化两个二值信号量，初值 0 表示尚未有事件发生 */
    osal_sem_init(&mqtt_semaphore, 0);
    osal_sem_init(&can_semaphore, 0);

    pthread_t threads[3];

    /* 线程入口函数表：顺序与 task_names 一一对应，便于循环创建 */
    void *(*task_funcs[3])(void *) = {
        mqtt_task,
        gateway_to_motor_task,
        motor_to_gateway_task,
    };

    /* 线程名称表：仅用于创建失败时的日志定位 */
    const char *task_names[3] = {
        "mqtt_task",
        "gateway_to_motor_task",
        "motor_to_gateway_task",
    };

    /* 依次创建三个线程，并记录成功创建的数量 */
    int created = 0;
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&threads[i], NULL, task_funcs[i], NULL) != 0) {
            LOGE("create %s failed", task_names[i]);
            break;
        }
        created++;
    }

    /* 主线程阻塞等待已创建的线程退出（收到退出信号后各线程会返回） */
    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }

    /* 线程全部结束后释放网络与 CAN 资源 */
    Int_MQTT_Deinit();
    Int_CAN_Deinit();

    /* 销毁同步对象，保证资源不泄漏 */
    osal_sem_destroy(&mqtt_semaphore);
    osal_sem_destroy(&can_semaphore);
    pthread_mutex_destroy(&g_data_mutex);
}

/**
 * @brief MQTT 接收线程。
 *
 * 负责建立 MQTT 连接、订阅下行主题，然后不断调用 Int_MQTT_ReceiveData()
 * 驱动 Paho 的收包流程；收到消息时会触发 mqtt_message_callback()。
 */
static void *mqtt_task(void *arg)
{
    (void)arg;  /* 线程参数未使用 */

    Int_MQTT_Init();

    /* 只要程序仍在运行，就持续处理 MQTT 收包 */
    while (g_running) {
        Int_MQTT_ReceiveData();
    }
    return NULL;
}

/**
 * @brief 下行 JSON 解析回调（由 MQTT 消息回调间接调用）。
 *
 * JSON 样例：{"target_distance":100,"max_speed":10}
 * 解析成功后更新共享的 distance / speed，并释放 mqtt_semaphore
 * 唤醒 gateway_to_motor_task 执行 CAN 下发。
 *
 * @param json_string 以 '\0' 结尾的 JSON 文本，函数内部不负责释放。
 */
void json_data_handler(char *json_string)
{
    if (json_string == NULL) {
        return;
    }

    /* 解析 JSON 文本为 cJSON 对象树 */
    cJSON *json_object = cJSON_Parse(json_string);
    if (json_object == NULL) {
        LOGE("invalid json: %s", json_string);
        return;
    }

    /* 读取目标距离与最大速度两个字段 */
    cJSON *distance_item = cJSON_GetObjectItem(json_object, "target_distance");
    cJSON *speed_item = cJSON_GetObjectItem(json_object, "max_speed");
    if (distance_item == NULL || speed_item == NULL) {
        LOGE("json missing target_distance/max_speed: %s", json_string);
        cJSON_Delete(json_object);
        return;
    }

    /* 协议字段为整数，统一截断为 16 位后参与 CAN 打包 */
    uint16_t new_distance = (uint16_t)distance_item->valueint;
    uint16_t new_speed = (uint16_t)speed_item->valueint;

    /* 加锁更新共享变量，避免与 CAN 下发线程读取时发生数据竞争 */
    pthread_mutex_lock(&g_data_mutex);
    distance = new_distance;
    speed = new_speed;
    pthread_mutex_unlock(&g_data_mutex);

    /* JSON 对象使用完毕，立即释放 */
    cJSON_Delete(json_object);

    /* 距离或速度为 0 视为无效指令，不触发 CAN 下发 */
    if (new_speed == 0 || new_distance == 0) {
        LOGE("json_data_handler params is NULL");
        return;
    }

    /* 唤醒 CAN 下发线程 */
    osal_sem_post(&mqtt_semaphore);
}

/**
 * @brief CAN 下发线程：把 MQTT 指令转换为 CAN 帧发送给电机。
 *
 * CAN 帧格式（标准帧 ID=1002，DLC=4）：
 *   data[0..1] = 目标距离（大端）
 *   data[2..3] = 最大速度（大端）
 */
static void *gateway_to_motor_task(void *arg)
{
    (void)arg;  /* 线程参数未使用 */

    while (g_running) {
        /* 最多等待 1 秒，超时后回到循环检查 g_running，避免永久阻塞 */
        if (osal_sem_wait(&mqtt_semaphore, 1000) != 0) {
            continue;
        }

        /* 加锁读取共享变量，拿到本次要下发的快照 */
        pthread_mutex_lock(&g_data_mutex);
        uint16_t current_distance = distance;
        uint16_t current_speed = speed;
        pthread_mutex_unlock(&g_data_mutex);

        /* 按大端序打包成 4 字节 CAN 数据 */
        uint8_t data[4] = {0};
        data[0] = current_distance >> 8;
        data[1] = current_distance & 0xFF;
        data[2] = current_speed >> 8;
        data[3] = current_speed & 0xFF;

        /* 通过 SocketCAN 发送到电机 */
        Int_CAN_SendData(data, 4);

        /* 保留原工程约 200 ms 的下发-上报间隔，同时保证可被退出信号打断 */
        sleep_ms_stop_aware(g_config.cycle_ms);

        /* 通知 CAN 上报线程开始接收电机状态 */
        osal_sem_post(&can_semaphore);
    }
    return NULL;
}

/**
 * @brief 把当前距离与电机状态组装成 JSON 并发布到 MQTT 上行主题。
 *
 * 上行格式：{"cur_distance":N,"motor_status":"on|off"}
 *
 * @param dis   当前距离
 * @param state 电机状态：1=运行中，2=停止
 */
static void send_json_to_control(uint16_t dis, uint16_t state)
{
    /* 创建 JSON 对象并填充两个字段 */
    cJSON *json_object = cJSON_CreateObject();
    cJSON_AddNumberToObject(json_object, "cur_distance", dis);
    cJSON_AddStringToObject(json_object, "motor_status", state == 1 ? "on" : "off");

    /* 序列化为紧凑字符串 */
    char *json_string = cJSON_PrintUnformatted(json_object);
    if (json_string != NULL) {
        /* 发送到 MQTT，注意不能把 cJSON 内部缓冲区直接交给 Paho 长期持有 */
        Int_MQTT_SendData((uint8_t *)json_string);
        cJSON_free(json_string);
    }
    cJSON_Delete(json_object);
}

/**
 * @brief CAN 上报线程：读取电机状态帧并发布到 MQTT。
 *
 * CAN 帧格式（标准帧 ID=1001，DLC=4）：
 *   data[0..1] = 当前距离（大端）
 *   data[2..3] = 状态（1=运行中，2=停止）
 *
 * 运行中时持续上报；收到停止状态后上报一次，然后退出内层循环，
 * 等待下一次 MQTT 指令触发新的接收周期。
 */
static void *motor_to_gateway_task(void *arg)
{
    (void)arg;  /* 线程参数未使用 */

    while (g_running) {
        /* 等待 CAN 下发线程的通知，最多 1 秒超时以检查退出标志 */
        if (osal_sem_wait(&can_semaphore, 1000) != 0) {
            continue;
        }

        /* 内层循环：持续读取电机上报的 CAN 帧 */
        while (g_running) {
            CAN_RX_MESSAGE message[3] = {0};  /* 一次最多缓存 3 帧 */
            uint8_t message_size = 3;         /* 传入容量，返回实际帧数 */

            Int_CAN_ReceiveData(message, &message_size);
            if (!g_running) {
                break;
            }

            /* 逐帧解析距离与状态 */
            for (uint8_t i = 0; i < message_size; i++) {
                uint8_t *data = message[i].data;
                uint16_t dis = (uint16_t)((data[0] << 8) | data[1]);
                uint16_t status = (uint16_t)((data[2] << 8) | data[3]);

                /* 状态 1：电机运行中，持续上报当前距离 */
                if (status == 1) {
                    LOGI("running: %d mm", dis);
                    send_json_to_control(dis, status);
                }
                /* 状态 2：电机已停止，上报一次后结束本轮接收 */
                if (status == 2) {
                    LOGI("stop: %d mm", dis);
                    send_json_to_control(dis, status);
                    goto end_while;
                }
            }
        }
    end_while:
        ;
    }
    return NULL;
}

/**
 * @brief 可被退出标志打断的毫秒级睡眠。
 *
 * 将长睡眠拆成 50 ms 的小片，每片结束后检查 g_running，
 * 使进程收到 SIGINT/SIGTERM 后能快速退出，而不是等满整个周期。
 *
 * @param ms 需要睡眠的总毫秒数
 */
static void sleep_ms_stop_aware(long ms)
{
    while (ms > 0 && g_running) {
        long step = (ms > 50) ? 50 : ms;
        osal_msleep(step);
        ms -= step;
    }
}
