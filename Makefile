# =============================================================================
# motor_gateway Linux 版构建脚本
#
# 功能：
#   make            编译并链接，产物为 build/motor_gateway
#   make -jN        并行编译
#   make clean      删除 build 目录及全部中间产物
#   make install    安装到 /usr/local/bin（可用 DESTDIR 指定安装根目录）
#
# 可覆盖变量：
#   make CC=clang CFLAGS="-O0 -g"
# =============================================================================

# C 编译器：默认 gcc，命令行传入的 CC 优先
CC := gcc

# C 语言标准：gnu11 允许使用 POSIX/GNU 扩展（如 pthread、nanosleep）
CSTD := -std=gnu11

# 优化选项：默认 -O2
CFLAGS ?= -O2

# 告警选项：开启常用告警，并忽略未使用参数的告警
WARNINGS := -Wall -Wextra -Wno-unused-parameter

# 线程库编译/链接选项
THREAD_FLAGS := -pthread

# 头文件搜索路径：覆盖项目各层目录及第三方库
INCLUDES := -Isrc \
            -Isrc/app \
            -Isrc/int \
            -Isrc/platform \
            -Ithird_party/cJSON \
            -Ithird_party/paho_mqtt \
            -Ithird_party/paho_mqtt/MQTTPacket/src

# 预处理选项：
#   -MMD -MP 生成头文件依赖文件（.d），头文件改动后自动触发重编译
CPPFLAGS := $(INCLUDES) -MMD -MP

# 构建输出目录
BUILD_DIR := build

# 最终可执行文件路径
TARGET := $(BUILD_DIR)/motor_gateway

# 全部需要编译的 C 源文件（顺序不影响结果）
SRCS := \
    src/main.c \
    src/app/App_Main.c \
    src/int/Int_CAN.c \
    src/int/Int_MQTT.c \
    src/platform/mqtt_interface.c \
    src/platform/osal.c \
    src/platform/config.c \
    third_party/cJSON/cJSON.c \
    third_party/paho_mqtt/MQTTClient.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTPacket.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTFormat.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTConnectClient.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTConnectServer.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTSerializePublish.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTDeserializePublish.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTSubscribeClient.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTSubscribeServer.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTUnsubscribeClient.c \
    third_party/paho_mqtt/MQTTPacket/src/MQTTUnsubscribeServer.c

# 由源文件列表推导目标文件列表：src/main.c -> build/src/main.o
OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))

# 由目标文件列表推导依赖文件列表：build/src/main.o -> build/src/main.d
DEPS := $(OBJS:.o=.d)

# 声明伪目标，避免与同名文件冲突
.PHONY: all clean install

# 默认目标：只编译不安装
all: $(TARGET)

# 链接规则：把所有目标文件链接为最终可执行文件
$(TARGET): $(OBJS)
	$(CC) $(CSTD) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) -o $@ $^

# 编译规则：每个 .c 编译为对应的 .o
# 自动创建目标文件所在的多级目录（如 build/third_party/paho_mqtt/MQTTPacket/src）
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CSTD) $(CFLAGS) $(WARNINGS) $(THREAD_FLAGS) $(CPPFLAGS) -c -o $@ $<

# 安装规则：复制可执行文件到系统 bin 目录
install: $(TARGET)
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/

# 清理规则：删除整个 build 目录
clean:
	rm -rf $(BUILD_DIR)

# 引入自动生成的依赖文件（首次编译时不存在，make 会忽略）
-include $(DEPS)
