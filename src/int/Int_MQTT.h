/**
 * @file Int_MQTT.h
 * @brief MQTT 业务接口层：封装 Paho Embedded C 的连接、订阅、发布与收包。
 */
#ifndef __INT_MQTT_H__
#define __INT_MQTT_H__

#include <stdint.h>

/* 连接 Broker 并订阅下行主题；内部带幂等保护 */
void Int_MQTT_Init(void);

/* 向上行主题发布一条以 '\0' 结尾的字符串消息 */
void Int_MQTT_SendData(uint8_t *data);

/* 驱动 Paho 收包（内部调用 MQTTYield），收到消息时触发解析回调 */
void Int_MQTT_ReceiveData(void);

/* 断开 MQTT 连接并复位初始化状态 */
void Int_MQTT_Deinit(void);

#endif /* __INT_MQTT_H__ */
