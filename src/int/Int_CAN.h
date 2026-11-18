/**
 * @file Int_CAN.h
 * @brief CAN 接口层：基于 Linux SocketCAN 的收发封装，替代原 STM32 HAL_CAN。
 */
#ifndef __INT_CAN_H__
#define __INT_CAN_H__

#include <stdint.h>

/**
 * CAN 接收消息结构（与原 MCU 侧 HAL 的接收结构保持兼容）。
 */
typedef struct {
    uint32_t id;              /* 标准帧 ID（仅低 11 位有效） */
    uint32_t message_length;  /* 数据长度 DLC */
    uint8_t data[9];          /* 数据区：最大 8 字节，额外预留 1 字节 */
} CAN_RX_MESSAGE;

/* 打开并绑定 SocketCAN 接口，接口名取自 g_config.can_iface */
void Int_CAN_Init(void);

/* 发送一帧数据，固定使用发送 ID 1002 */
void Int_CAN_SendData(uint8_t *data, uint8_t length);

/*
 * 读取 CAN 帧。
 * @param message        接收缓冲区数组
 * @param message_length 传入缓冲区容量，返回实际读取到的帧数
 */
void Int_CAN_ReceiveData(CAN_RX_MESSAGE *message, uint8_t *message_length);

/* 关闭 SocketCAN 套接字并复位内部句柄 */
void Int_CAN_Deinit(void);

#endif /* __INT_CAN_H__ */
