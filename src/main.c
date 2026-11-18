/**
 * @file main.c
 * @brief 程序入口：解析命令行参数、注册退出信号，然后启动网关业务主流程。
 */
#include "App_Main.h"
#include "config.h"

#include <signal.h>
#include <stdlib.h>

/*
 * 全局运行标志：
 * - volatile 防止编译器把它优化进寄存器，保证信号处理函数修改后各线程能读到；
 * - sig_atomic_t 保证在信号处理函数中读写是原子的；
 * - 1 表示运行中，0 表示收到退出信号。
 */
volatile sig_atomic_t g_running = 1;

/**
 * @brief SIGINT / SIGTERM 信号处理函数。
 *
 * 这里只做置位操作，不调用 printf、加锁等非异步信号安全函数。
 */
static void on_signal(int sig)
{
    (void)sig;      /* 信号编号未使用，显式忽略以消除编译告警 */
    g_running = 0;  /* 通知所有业务线程退出各自循环 */
}

/**
 * @brief 程序入口。
 * @return 打印帮助或正常结束时返回 EXIT_SUCCESS；参数错误返回 EXIT_FAILURE。
 */
int main(int argc, char **argv)
{
    /* 解析命令行参数：0=正常，1=已打印帮助，-1=参数错误 */
    int rc = config_parse(argc, argv);
    if (rc > 0) {
        return EXIT_SUCCESS;
    }
    if (rc < 0) {
        return EXIT_FAILURE;
    }

    /* 收到 Ctrl+C / kill 时优雅退出；忽略写已关闭 socket 产生的 SIGPIPE */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* 启动三个业务线程，并阻塞等待它们全部退出 */
    App_Main();
    return EXIT_SUCCESS;
}
