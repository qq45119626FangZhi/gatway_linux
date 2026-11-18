/**
 * @file mqtt_interface.c
 * @brief Paho MQTT 平台适配实现：用 Linux TCP socket 替代原 W5500/SPI 网络层。
 *
 * 原工程通过 W5500 硬件协议栈收发数据；移植到 Linux 后：
 *   - Timer 系列函数改为基于 CLOCK_MONOTONIC 计算；
 *   - Network 的读写函数改为 poll + recv/send；
 *   - 连接函数改为 getaddrinfo + 非阻塞 connect + poll 超时。
 *
 * Paho 的上层逻辑（报文编解码、QoS、订阅）完全复用，协议行为不变。
 */
#include "mqtt_interface.h"

#include "osal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * @brief 原 1 ms 定时器中断入口，Linux 下已由 CLOCK_MONOTONIC 取代，空实现。
 */
void MilliTimer_Handler(void)
{
    /* No longer needed: Timer reads CLOCK_MONOTONIC directly. */
}

/**
 * @brief 初始化定时器，清零内部字段。
 */
void TimerInit(Timer *timer)
{
    timer->systick_period = 0;
    timer->end_time = 0;
}

/**
 * @brief 判断定时器是否到期。
 * @return 1=已到期，0=未到期
 */
char TimerIsExpired(Timer *timer)
{
    return (osal_now_ms() >= (uint64_t)timer->end_time) ? 1 : 0;
}

/**
 * @brief 设置毫秒级倒计时。
 * @param timeout 超时毫秒数
 */
void TimerCountdownMS(Timer *timer, unsigned int timeout)
{
    timer->end_time = (unsigned long)(osal_now_ms() + timeout);
}

/**
 * @brief 设置秒级倒计时。
 * @param timeout 超时秒数
 */
void TimerCountdown(Timer *timer, unsigned int timeout)
{
    timer->end_time = (unsigned long)(osal_now_ms() + (uint64_t)timeout * 1000ULL);
}

/**
 * @brief 返回距离到期的剩余毫秒数。
 *
 * 结果上限截断为 INT_MAX，避免返回值溢出。
 */
int TimerLeftMS(Timer *timer)
{
    uint64_t now = osal_now_ms();
    if (now >= (uint64_t)timer->end_time) {
        return 0;
    }

    uint64_t left = (uint64_t)timer->end_time - now;
    return (left > INT_MAX) ? INT_MAX : (int)left;
}

/**
 * @brief 从 TCP socket 读取指定长度的数据。
 *
 * 循环 poll + recv，直到读满 len、超时或连接出错。
 * 该函数用于 Paho 读取固定长度的 MQTT 报文头/报文体。
 *
 * @param n          Network 对象
 * @param buffer     接收缓冲区
 * @param len        期望读取的字节数
 * @param timeout_ms poll 超时毫秒数
 * @return 实际读取字节数；无数据或出错返回 SOCK_ERROR
 */
int linux_read(Network *n, unsigned char *buffer, int len, long timeout_ms)
{
    int total = 0;

    if (n == NULL || n->my_socket < 0 || len <= 0) {
        return SOCK_ERROR;
    }

    while (total < len) {
        struct pollfd pfd;
        pfd.fd = n->my_socket;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc == 0) {
            /* 超时：返回已经读到的部分，没有则返回错误 */
            break;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SOCK_ERROR;
        }
        /* 连接异常事件直接判错 */
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return SOCK_ERROR;
        }

        ssize_t got = recv(n->my_socket, buffer + total, (size_t)(len - total), 0);
        if (got == 0) {
            /* 对端关闭连接 */
            break;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SOCK_ERROR;
        }
        total += (int)got;
    }

    return (total > 0) ? total : SOCK_ERROR;
}

/**
 * @brief 向 TCP socket 写入指定长度的数据。
 *
 * 循环 poll + send，处理短写与 EINTR，直到写满或超时/出错。
 *
 * @param n          Network 对象
 * @param buffer     待发送数据
 * @param len        期望写入的字节数
 * @param timeout_ms poll 超时毫秒数
 * @return 实际写入字节数；无数据或出错返回 SOCK_ERROR
 */
int linux_write(Network *n, unsigned char *buffer, int len, long timeout_ms)
{
    int total = 0;

    if (n == NULL || n->my_socket < 0 || len <= 0) {
        return SOCK_ERROR;
    }

    while (total < len) {
        struct pollfd pfd;
        pfd.fd = n->my_socket;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SOCK_ERROR;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return SOCK_ERROR;
        }

        ssize_t sent = send(n->my_socket, buffer + total, (size_t)(len - total), 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return SOCK_ERROR;
        }
        total += (int)sent;
    }

    return (total > 0) ? total : SOCK_ERROR;
}

/**
 * @brief 关闭网络连接并复位 socket 句柄。
 */
void linux_disconnect(Network *n)
{
    if (n != NULL && n->my_socket >= 0) {
        close(n->my_socket);
        n->my_socket = -1;
    }
}

/**
 * @brief 初始化 Network 对象，挂接 Linux 的读写与断开实现。
 * @param sn 原工程 socket 编号，Linux 下未使用
 */
void NewNetwork(Network *n, int sn)
{
    (void)sn;
    n->my_socket = -1;
    n->mqttread = linux_read;
    n->mqttwrite = linux_write;
    n->disconnect = linux_disconnect;
}

/**
 * @brief 通过主机名或点分 IP 连接 Broker。
 *
 * 使用 getaddrinfo 解析地址，先以非阻塞方式 connect，
 * 再用 poll 等待 3 秒判断连接是否完成，成功后恢复为阻塞模式，
 * 以配合 Paho 依赖超时参数的读写接口。
 *
 * @return SOCK_OK=成功；SOCK_ERROR=失败
 */
int ConnectNetworkHost(Network *n, const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       /* 仅使用 IPv4 */
    hints.ai_socktype = SOCK_STREAM; /* TCP */

    /* 端口转换为字符串，供 getaddrinfo 使用 */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned int)port);

    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        return SOCK_ERROR;
    }

    /* 遍历解析结果，尝试建立连接 */
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        /* 设置为非阻塞，便于用 poll 控制连接超时 */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            break;
        }
        if (errno == EINPROGRESS) {
            /* 连接进行中：等待可写事件，最多 3 秒 */
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 3000);
            if (pr > 0) {
                /* 通过 SO_ERROR 判断连接是否真正成功 */
                int sock_err = 0;
                socklen_t len = sizeof(sock_err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len);
                if (sock_err == 0) {
                    rc = 0;
                    break;
                }
            }
        }

        /* 本次候选地址连接失败，关闭后尝试下一个 */
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        return SOCK_ERROR;
    }

    /* 恢复阻塞模式：读写接口通过 poll 自行控制超时 */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    n->my_socket = fd;
    return SOCK_OK;
}

/**
 * @brief 用 4 字节 IP 地址连接 Broker（兼容原 W5500 接口）。
 *
 * @param ip 长度为 4 的 IP 字节数组
 */
int ConnectNetwork(Network *n, uint8_t *ip, uint16_t port)
{
    char host[INET_ADDRSTRLEN];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return ConnectNetworkHost(n, host, port);
}
