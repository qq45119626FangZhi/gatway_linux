# gateway-linux

Linux 版智能网关。

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
