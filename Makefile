##########################################################################################################################
# MPU6050 100Hz 采样 + OLED 10Hz 显示 + PA0 按键切换 —— Makefile
# 芯片: STM32F103C8T6 (Blue Pill, 64KB Flash / 20KB SRAM)
# 工具链: GNU Arm Embedded Toolchain 10.2021.10 + MSYS2 make (结构对齐 adc-oled 工程)
# 构建产物: build/mpu6050-oled.{elf,hex,bin}
##########################################################################################################################

######################################
# target
######################################
TARGET = mpu6050-oled

######################################
# building variables
######################################
# debug build?
DEBUG = 1
# optimization
OPT = -Os

#######################################
# paths
#######################################
BUILD_DIR = build

######################################
# source
######################################
# C sources（应用层 + HAL 最小驱动集 + 系统文件）
#   User/main.c             - 主循环 + SysTick
#   User/stm32f1xx_it.c     - SysTick 中断
#   User/MyI2C.c            - OLED 软件 I2C（PB8/PB9）
#   User/My_OLED.c          - SSD1306 OLED 驱动
#   User/mpu6050/mpu6050_i2c.c - MPU6050 独立软件 I2C（PB10/PB11）
#   User/mpu6050/mpu6050.c     - MPU6050 驱动 + 互补滤波
#   User/key/key.c          - PA0 按键（非阻塞消抖 + 长短按）
#   User/oled_view/oled_view.c - OLED 4 状态显示状态机
C_SOURCES =  \
User/main.c \
User/hardfault_rtt.c \
User/stm32f1xx_it.c \
User/MyI2C.c \
User/My_OLED.c \
User/mpu6050/mpu6050_i2c.c \
User/mpu6050/mpu6050.c \
User/brt_band.c \
User/adc_brt.c \
User/key/key.c \
User/oled_view/oled_view.c \
User/matrix/matrix7219.c \
User/tetris/tetris.c \
ThirdParty/RTT/SEGGER_RTT.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_cortex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_adc.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_adc_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_dma.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_gpio_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_spi.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_rcc_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_flash_ex.c \
Drivers/STM32F1xx_HAL_Driver/Src/stm32f1xx_hal_pwr.c \
Drivers/CMSIS/Device/ST/STM32F1xx/Source/Templates/system_stm32f1xx.c

# ASM sources（CubeMX 生成的 GCC 启动文件）
ASM_SOURCES =  \
startup_stm32f103xb.s

#######################################
# binaries
#######################################
PREFIX = arm-none-eabi-
# gcc 编译器路径可通过 make GCC_PATH=xxx 指定，或在环境变量 PATH 中
ifdef GCC_PATH
CC = $(GCC_PATH)/$(PREFIX)gcc
AS = $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP = $(GCC_PATH)/$(PREFIX)objcopy
SZ = $(GCC_PATH)/$(PREFIX)size
else
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size
endif
HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

#######################################
# CFLAGS
#######################################
# cpu
CPU = -mcpu=cortex-m3

# mcu
MCU = $(CPU) -mthumb

# C defines（HAL 库必需：USE_HAL_DRIVER + 芯片型号 STM32F103xB）
C_DEFS =  \
-DUSE_HAL_DRIVER \
-DSTM32F103xB

# C includes
C_INCLUDES =  \
-IUser \
-IUser/mpu6050 \
-IUser/oled_view \
-IUser/key \
-IUser/matrix \
-IUser/tetris \
-IThirdParty/RTT \
-IDrivers/STM32F1xx_HAL_Driver/Inc \
-IDrivers/STM32F1xx_HAL_Driver/Inc/Legacy \
-IDrivers/CMSIS/Device/ST/STM32F1xx/Include \
-IDrivers/CMSIS/Include

# compile gcc flags
ASFLAGS = $(MCU) $(OPT) -Wall -fdata-sections -ffunction-sections

CFLAGS += $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections -fno-common

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2
endif

# Generate dependency information
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

#######################################
# LDFLAGS
#######################################
# link script
LDSCRIPT = STM32F103xx_FLASH.ld

# libraries（nano.specs 精简 C 库 + nosys 无主机系统调用桩；
#   -u _printf_float = 链入浮点 printf 支持, 否则 %.2f 在 nano 下输出空串）
LIBS = -lc -lm -lnosys -u _printf_float
LIBDIR =
LDFLAGS = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# default action: build all
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

#######################################
# build the application
#######################################
# list of objects
OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
# list of ASM program objects
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf | $(BUILD_DIR)
	$(BIN) $< $@

$(BUILD_DIR):
	mkdir $@

#######################################
# flash（OpenOCD + ST-Link 烧录，ST-Link 需已连接）
#######################################
flash: $(BUILD_DIR)/$(TARGET).bin
	openocd -f openocd.cfg -c "program $(BUILD_DIR)/$(TARGET).bin 0x08000000 verify reset exit"

#######################################
# clean up
#######################################
clean:
	-rm -fR $(BUILD_DIR)

#######################################
# dependencies
#######################################
-include $(wildcard $(BUILD_DIR)/*.d)

# *** EOF ***
