# gateway-linux

## 项目介绍

`gateway-linux` 是一个运行在 Linux 平台上的电机网关转发控制程序，用于在 MQTT 控制台与 CAN 总线电机之间建立双向通信桥梁。程序由原 STM32 + FreeRTOS 网关工程移植而来，保留原有的 MQTT 主题、JSON 字段、CAN 帧 ID、数据打包格式和约 200 ms 的下发—上报节奏；底层改用 pthread、POSIX socket 与 SocketCAN，以便在 Linux 主机上运行。

网关接收控制台下发的目标距离和最大速度，将其转换为 CAN 标准帧发送给电机；电机运行后，网关读取 CAN 状态帧，并将当前距离和运行状态转换为 JSON 消息发布到 MQTT。由此，控制台无需直接接入 CAN 总线，即可完成远程控制和状态监测。CAN 接口波特率由 Linux SocketCAN 在启动前配置，程序本身不修改该设置。

### 核心能力

- **协议转换**：MQTT JSON 指令与 CAN 二进制帧双向转换，字段和帧格式与原工程保持一致。
- **状态上报**：电机运行时持续上报当前距离；收到停止状态后上报一次，并结束本轮接收。
- **并发转发**：3 个 pthread 线程分别处理 MQTT 接收、CAN 下发和状态上报，通过信号量同步，并使用互斥锁保护共享的距离/速度参数。
- **灵活配置**：Broker 地址、端口、主题、CAN 接口和下发—上报间隔均可通过命令行参数配置，默认值与原工程一致。
- **优雅退出**：`SIGINT` / `SIGTERM` 可打断等待，并回收线程、网络和 CAN 资源。

### 工作流程

```text
控制台 --MQTT 下行主题--> mqtt_task（解析 JSON）
                              │ mqtt_semaphore
                              ▼
                      gateway_to_motor_task（发送 CAN ID=1002）
                              │ 等待 --cycle 后置位 can_semaphore
                              ▼
                      motor_to_gateway_task（接收 CAN ID=1001）
                              │ MQTT 上行主题
                              ▼
                            控制台
```

## 对外协议

| 方向 | 通道 | 内容 |
|---|---|---|
| 下行 | MQTT 主题 `console_to_gateway` | `{"target_distance":N,"max_speed":N}` |
| 下行 | CAN 标准帧 ID `1002` | 4 字节：距离高/低 + 速度高/低 |
| 上行 | CAN 标准帧 ID `1001` | data[0..1]=当前距离，data[2..3]=状态（1=运行中，2=停止） |
| 上行 | MQTT 主题 `gateway_to_console` | `{"cur_distance":N,"motor_status":"on|off"}` |

CAN 波特率仍为 500 kbps（由 SocketCAN 接口配置），下行到上行之间仍保留约 200 ms 间隔。

## 目录结构

```
gatway_linux/
├── Makefile
├── src/
│   ├── main.c                 程序入口、信号退出
│   ├── app/App_Main.c         三个业务线程（原 FreeRTOS 任务）
│   ├── int/Int_CAN.c          SocketCAN 实现（原 HAL_CAN）
│   ├── int/Int_MQTT.c         MQTT 业务封装
│   └── platform/              pthread 信号量、时钟、socket MQTT 适配、参数解析
├── third_party/
│   ├── cJSON/                 原工程 cJSON，未改协议逻辑
│   └── paho_mqtt/             原工程 Paho Embedded C + MQTTPacket
└── scripts/                   vcan 与虚拟电机测试脚本
```

## 构建

使用项目根目录的 Makefile：

```bash
make -j4          # 编译，产物为 build/motor_gateway
make clean        # 清理 build 目录
make install      # 安装到 /usr/local/bin，可用 DESTDIR=... 指定安装根目录
```

编译器默认为 `gcc`，可通过 `CC`、`CFLAGS` 覆盖，例如：

```bash
make CC=clang CFLAGS="-O0 -g" -j4
```

## 运行参数

默认值与原工程硬编码值一致：

```text
--broker HOST       默认 192.168.45.48
--port PORT         默认 1883
--client-id ID      默认 mqtt_fz
--can IFACE         默认 can0
--sub TOPIC         默认 console_to_gateway
--pub TOPIC         默认 gateway_to_console
--cycle MS          默认 200
--help
```

示例：

```bash
./build/motor_gateway \
  --broker 127.0.0.1 --port 1883 \
  --can vcan0
```

`SIGINT` / `SIGTERM` 可让三个线程退出并正常回收资源。

## 本地端到端验证

1. 准备虚拟 CAN 接口：

   ```bash
   ./scripts/setup_vcan.sh vcan0
   ```

   真实 CAN 控制器请使用：

   ```bash
   sudo ip link set can0 type can bitrate 500000
   sudo ip link set up can0
   ```

2. 启动 MQTT Broker（例如 mosquitto）：

   ```bash
   mosquitto -p 1883
   ```

3. 启动虚拟电机：

   ```bash
   ./scripts/mock_motor.py --can vcan0
   ```

4. 启动网关：

   ```bash
   ./build/motor_gateway --broker 127.0.0.1 --can vcan0
   ```

5. 另开终端发送控制指令并观察上报：

   ```bash
   mosquitto_sub -h 127.0.0.1 -t gateway_to_console
   mosquitto_pub -h 127.0.0.1 -t console_to_gateway \
     -m '{"target_distance":100,"max_speed":10}'
   ```

   预期：网关收到 JSON 后向 CAN 发 ID=1002 的 4 字节帧，虚拟电机逐步回 ID=1001 的状态帧，网关最终发布 `motor_status:"on"`，停止时发布 `"off"`。

## 与原工程相比保留/变化的内容

保留：

- 3 个业务任务拆成 3 个 pthread 线程，职责和顺序不变。
- 2 个信号量改为 pthread 条件变量实现的二值信号量。
- JSON 字段、MQTT 主题、CAN ID、数据打包格式、200 ms 节奏不变。
- cJSON 和 Paho MQTT 报文编解码源码直接复用。

必要变化（平台适配，不属于业务功能变化）：

- W5500/SPI 硬件协议栈 → Linux TCP socket。
- `HAL_CAN_*` → SocketCAN。
- `vTaskDelay` / `xSemaphore*` → `osal_msleep` / `osal_sem_*`。
- 无限忙等 → 1 秒超时轮询，避免 Linux 下线程占满 CPU。
- 原工程中未注册的 MQTT 1ms 时基 → `CLOCK_MONOTONIC`。
- MQTT 回调里的 payload 复制并补 `'\0'`，避免 cJSON 越界。
- `distance/speed` 增加互斥保护。
- Broker / 接口 / 主题等硬编码改为命令行参数，默认值不变。

## 验证状态

- 使用 GCC 15.2.0 以 `-Wall -Wextra` 编译通过，无警告。
- AddressSanitizer + UBSan 冒烟测试通过，无内存/未定义行为报告。
- `SIGINT` 可正常退出，返回码 0。
- 已使用根目录 `Makefile` 完成全量编译验证；
