/**
 * @file Int_MQTT.c
 * @brief MQTT 业务封装：基于 Paho Embedded C + Linux TCP socket 实现。
 *
 * 与原工程的差异仅在底层：
 *   - 原 W5500/SPI 网络层 -> mqtt_interface.c 中的 Linux socket 适配；
 *   - 收包仍由 MQTTYield 驱动，主题、QoS、回调逻辑保持不变。
 *
 * 线程安全：
 *   mqtt_client / mqtt_network / 收发缓冲区由 g_mqtt_mutex 保护，
 *   因为 MQTT 接收线程与业务上报线程都会访问同一个客户端实例。
 */
#include "Int_MQTT.h"

#include "App_Main.h"
#include "MQTTClient.h"
#include "config.h"
#include "log.h"
#include "mqtt_interface.h"
#include "osal.h"

#include <pthread.h>
#include <string.h>

/* MQTT 收发缓冲区大小 */
#define BUFFER_SIZE 1024

/* Paho 网络句柄编号，Linux 下仅作占位 */
#define SOCKET_NUM 0

/* Paho MQTT 客户端实例 */
static MQTTClient mqtt_client;

/* Paho 网络抽象，函数指针指向 Linux socket 实现 */
static Network mqtt_network;

/* 发送缓冲区，由 Paho 内部使用 */
static uint8_t send_buffer[BUFFER_SIZE] = {0};

/* 接收缓冲区，由 Paho 内部使用 */
static uint8_t read_buffer[BUFFER_SIZE] = {0};

/* 保护 MQTT 客户端及收发缓冲区的互斥锁 */
static pthread_mutex_t g_mqtt_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 是否已完成连接与订阅，1=已初始化 */
static int g_initialized = 0;

/* MQTT 消息到达回调（订阅时注册给 Paho） */
static void mqtt_message_callback(MessageData *data);

/**
 * @brief 初始化 MQTT：建立 TCP 连接、完成 MQTT 握手并订阅下行主题。
 *
 * 函数可重复调用：已初始化时直接返回。
 * 任一步骤失败都会断开网络并保持未初始化状态，由调用方在下一轮重试。
 */
void Int_MQTT_Init(void)
{
    pthread_mutex_lock(&g_mqtt_mutex);
    /* 幂等保护：已经初始化成功就不再重复建连 */
    if (g_initialized) {
        pthread_mutex_unlock(&g_mqtt_mutex);
        return;
    }

    /* 初始化网络抽象，并连接 Broker 的 TCP 端口 */
    NewNetwork(&mqtt_network, SOCKET_NUM);
    if (ConnectNetworkHost(&mqtt_network, g_config.broker_host,
                           g_config.broker_port) != SOCK_OK) {
        LOGE("connect MQTT broker %s:%u failed", g_config.broker_host,
             (unsigned int)g_config.broker_port);
        pthread_mutex_unlock(&g_mqtt_mutex);
        return;
    }
    LOGI("connected to MQTT broker %s:%u", g_config.broker_host,
         (unsigned int)g_config.broker_port);

    /* 绑定网络与收发缓冲区，命令超时设为 1000 ms */
    MQTTClientInit(&mqtt_client, &mqtt_network, 1000,
                   send_buffer, BUFFER_SIZE, read_buffer, BUFFER_SIZE);

    /* 填充 MQTT CONNECT 参数：客户端 ID、关闭遗嘱、不启用保活 */
    MQTTPacket_connectData options = MQTTPacket_connectData_initializer;
    options.clientID.cstring = (char *)g_config.client_id;
    options.keepAliveInterval = 0;
    options.willFlag = 0;

    /* 发送 CONNECT 并等待 CONNACK */
    int state = MQTTConnect(&mqtt_client, &options);
    if (state != SUCCESSS) {
        LOGE("MQTTConnect failed: %d", state);
        mqtt_network.disconnect(&mqtt_network);
        pthread_mutex_unlock(&g_mqtt_mutex);
        return;
    }
    LOGI("MQTT connected as %s", g_config.client_id);

    /* 订阅下行主题，收到消息后进入 mqtt_message_callback */
    state = MQTTSubscribe(&mqtt_client, g_config.sub_topic, QOS0,
                          mqtt_message_callback);
    if (state != SUCCESSS) {
        LOGE("MQTTSubscribe %s failed: %d", g_config.sub_topic, state);
        mqtt_network.disconnect(&mqtt_network);
        pthread_mutex_unlock(&g_mqtt_mutex);
        return;
    }
    LOGI("subscribed to %s", g_config.sub_topic);

    /* 所有步骤成功后才标记为已初始化 */
    g_initialized = 1;
    pthread_mutex_unlock(&g_mqtt_mutex);
}

/**
 * @brief 发布一条字符串消息到上行主题（QoS0，不保留）。
 *
 * @param data 以 '\0' 结尾的字符串；NULL 或未连接时静默返回。
 */
void Int_MQTT_SendData(uint8_t *data)
{
    if (data == NULL) {
        return;
    }

    pthread_mutex_lock(&g_mqtt_mutex);
    /* 未初始化或连接已断开时不发送，避免操作无效 socket */
    if (!g_initialized || !mqtt_client.isconnected) {
        pthread_mutex_unlock(&g_mqtt_mutex);
        return;
    }

    /* 组装 Paho 消息结构 */
    MQTTMessage message;
    message.payload = data;
    message.payloadlen = strlen((char *)data);
    message.qos = QOS0;
    message.dup = 0;
    message.retained = 0;

    /* 发布并检查返回值 */
    int state = MQTTPublish(&mqtt_client, g_config.pub_topic, &message);
    if (state != SUCCESSS) {
        LOGE("MQTTPublish failed: %d, payload=%s", state, (char *)data);
    }
    pthread_mutex_unlock(&g_mqtt_mutex);
}

/**
 * @brief 驱动 MQTT 收包。
 *
 * MQTTYield 负责读取并分发服务端报文，最长阻塞 1000 ms；
 * 未初始化时短暂休眠，避免线程忙等占满 CPU。
 */
void Int_MQTT_ReceiveData(void)
{
    pthread_mutex_lock(&g_mqtt_mutex);
    if (!g_initialized) {
        pthread_mutex_unlock(&g_mqtt_mutex);
        osal_msleep(200);
        return;
    }

    /* 处理网络报文，回调可能在本函数内部被调用 */
    MQTTYield(&mqtt_client, 1000);
    pthread_mutex_unlock(&g_mqtt_mutex);
}

/**
 * @brief 断开 MQTT 连接并复位初始化状态。
 */
void Int_MQTT_Deinit(void)
{
    pthread_mutex_lock(&g_mqtt_mutex);
    if (g_initialized) {
        mqtt_network.disconnect(&mqtt_network);
        g_initialized = 0;
    }
    pthread_mutex_unlock(&g_mqtt_mutex);
}

/**
 * @brief Paho 消息到达回调。
 *
 * 将 payload 复制到本地缓冲区并补 '\0'，再交给业务层解析。
 * 复制的原因是 MQTT 报文缓冲区不保证以 '\0' 结尾，直接交给 cJSON 会越界。
 *
 * @param data Paho 传入的消息数据（含主题与 payload）
 */
static void mqtt_message_callback(MessageData *data)
{
    char payload[BUFFER_SIZE];

    /* 防御空指针：连接异常时 Paho 可能传入空消息 */
    if (data == NULL || data->message == NULL || data->message->payload == NULL) {
        return;
    }

    /* 限制长度并保证字符串以 '\0' 结尾 */
    size_t len = data->message->payloadlen;
    if (len >= sizeof(payload)) {
        len = sizeof(payload) - 1;
    }
    memcpy(payload, data->message->payload, len);
    payload[len] = '\0';

    /* 进入业务解析：更新共享数据并唤醒 CAN 下发线程 */
    json_data_handler(payload);
}
