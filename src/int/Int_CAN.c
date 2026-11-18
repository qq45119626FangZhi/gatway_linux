/**
 * @file Int_CAN.c
 * @brief CAN 接口层实现：使用 Linux SocketCAN（PF_CAN / CAN_RAW）完成收发。
 *
 * 与 MCU 侧的对应关系：
 *   HAL_CAN_Init / 过滤器配置  -> Int_CAN_Init
 *   HAL_CAN_AddTxMessage       -> Int_CAN_SendData
 *   HAL_CAN_GetRxMessage       -> Int_CAN_ReceiveData
 *
 * 帧 ID 约定（与原工程一致）：
 *   1001：电机 -> 网关（上行状态）
 *   1002：网关 -> 电机（下行控制）
 */
#include "Int_CAN.h"

#include "config.h"
#include "log.h"

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* 电机上报状态使用的标准帧 ID */
#define CAN_ID_MOTOR 1001

/* 网关下发控制使用的标准帧 ID */
#define CAN_ID_GATEWAY 1002

/* CAN 收发的 poll 超时时间（毫秒），避免线程永久阻塞 */
#define CAN_RX_TIMEOUT_MS 1000

/* SocketCAN 套接字句柄，-1 表示尚未初始化或已关闭 */
static int g_can_fd = -1;

/**
 * @brief 初始化 SocketCAN：创建套接字、绑定接口、设置接收过滤器。
 *
 * 失败时只记录日志并保持 g_can_fd = -1，后续收发接口会安全返回。
 */
void Int_CAN_Init(void)
{
    /* 创建 CAN 原始套接字 */
    g_can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (g_can_fd < 0) {
        LOGE("open SocketCAN failed: %s", strerror(errno));
        return;
    }

    /* 通过接口名查询网络接口索引（如 can0 / vcan0） */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, g_config.can_iface, IFNAMSIZ - 1);
    if (ioctl(g_can_fd, SIOCGIFINDEX, &ifr) < 0) {
        LOGE("CAN interface %s not found: %s", g_config.can_iface, strerror(errno));
        close(g_can_fd);
        g_can_fd = -1;
        return;
    }

    /* 绑定到指定 CAN 接口 */
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(g_can_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("bind CAN interface failed: %s", strerror(errno));
        close(g_can_fd);
        g_can_fd = -1;
        return;
    }

    /*
     * 原 STM32 工程使用 ID 列表模式配置过滤器，等效于放行需要的一组 ID。
     * 这里保留相同语义：
     *   - 第一个过滤器精确匹配电机上报 ID 1001；
     *   - 第二个过滤器 mask=0，表示不限制 ID，即也接收总线上其他标准帧。
     * 这样上行状态帧不会被内核过滤掉。
     */
    struct can_filter rfilter[2];
    rfilter[0].can_id = CAN_ID_MOTOR;
    rfilter[0].can_mask = CAN_SFF_MASK;
    rfilter[1].can_id = 0;
    rfilter[1].can_mask = CAN_SFF_MASK;
    if (setsockopt(g_can_fd, SOL_CAN_RAW, CAN_RAW_FILTER, rfilter, sizeof(rfilter)) < 0) {
        LOGE("set CAN filter failed: %s", strerror(errno));
    }

    LOGI("CAN ready on %s (rx id=%d, tx id=%d)", g_config.can_iface,
         CAN_ID_MOTOR, CAN_ID_GATEWAY);
}

/**
 * @brief 关闭 SocketCAN 套接字并复位句柄。
 */
void Int_CAN_Deinit(void)
{
    if (g_can_fd >= 0) {
        close(g_can_fd);
        g_can_fd = -1;
    }
}

/**
 * @brief 发送一帧 CAN 标准帧。
 *
 * 使用 poll 等待套接字可写，最多等待 CAN_RX_TIMEOUT_MS，避免发送缓冲满时卡死。
 *
 * @param data   待发送数据
 * @param length 数据长度，超过 CAN 最大长度时自动截断
 */
void Int_CAN_SendData(uint8_t *data, uint8_t length)
{
    /* 未初始化或初始化失败时直接返回 */
    if (g_can_fd < 0) {
        LOGE("CAN is not initialized");
        return;
    }
    /* 经典 CAN 单帧最多 8 字节，超出部分截断 */
    if (length > CAN_MAX_DLEN) {
        length = CAN_MAX_DLEN;
    }

    /* 组装 CAN 帧：固定使用网关发送 ID 1002 */
    struct can_frame frame;
    memset(&frame, 0, sizeof(frame));
    frame.can_id = CAN_ID_GATEWAY;
    frame.can_dlc = length;
    memcpy(frame.data, data, length);

    /* 等待套接字可写 */
    struct pollfd pfd;
    pfd.fd = g_can_fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    int rc = poll(&pfd, 1, CAN_RX_TIMEOUT_MS);
    if (rc <= 0 || !(pfd.revents & POLLOUT)) {
        LOGE("CAN send timeout");
        return;
    }

    /* 写入一帧；返回值必须等于结构体大小才算成功 */
    ssize_t n = write(g_can_fd, &frame, sizeof(frame));
    if (n != (ssize_t)sizeof(frame)) {
        LOGE("CAN send failed: %s", strerror(errno));
    }
}

/**
 * @brief 批量读取 CAN 帧（一次最多 3 帧）。
 *
 * 先 poll 等待第一帧，随后用零超时 poll 继续尝试读取，
 * 把已到达的帧尽量一次性取完，减少系统调用次数。
 *
 * @param message        接收缓冲区数组
 * @param message_length 传入容量，返回实际读取帧数（无数据时为 0）
 */
void Int_CAN_ReceiveData(CAN_RX_MESSAGE *message, uint8_t *message_length)
{
    uint8_t max_count = 3;  /* 单次调用最多读取 3 帧 */
    uint8_t count = 0;

    /* 参数保护，避免空指针解引用 */
    if (message == NULL || message_length == NULL) {
        return;
    }
    /* 调用方传入的容量小于 3 时，以调用方容量为准 */
    if (*message_length > 0 && *message_length < max_count) {
        max_count = *message_length;
    }
    /* 套接字未初始化：返回 0 帧 */
    if (g_can_fd < 0) {
        *message_length = 0;
        return;
    }

    /* 等待第一帧数据到达，最多等待 1 秒 */
    struct pollfd pfd;
    pfd.fd = g_can_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int rc = poll(&pfd, 1, CAN_RX_TIMEOUT_MS);
    if (rc <= 0 || !(pfd.revents & POLLIN)) {
        *message_length = 0;
        return;
    }

    /* 循环读取，直到缓冲区满或没有更多已就绪的数据 */
    while (count < max_count) {
        struct can_frame frame;
        ssize_t n = read(g_can_fd, &frame, sizeof(frame));
        if (n != (ssize_t)sizeof(frame)) {
            break;
        }

        /* 拷贝帧 ID、DLC 与数据到上层结构 */
        message[count].id = frame.can_id & CAN_SFF_MASK;
        message[count].message_length = frame.can_dlc;
        memset(message[count].data, 0, sizeof(message[count].data));
        memcpy(message[count].data, frame.data, frame.can_dlc);
        count++;

        /* 零超时检查是否还有下一帧，没有则提前结束 */
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) {
            break;
        }
    }

    *message_length = count;
}
