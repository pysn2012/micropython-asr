# K10 ASR模块编译配置

# 源文件
SRC = k10_asr.c
OBJ = k10_asr.o

# 编译选项
CFLAGS = -Wall -Wextra -std=c99 -O2
INCLUDES = -I../../py -I../../ports/esp32

# 默认目标
all: $(OBJ)

# 编译规则
$(OBJ): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -c $(SRC) -o $(OBJ)

# 清理
clean:
	rm -f $(OBJ)

# 安装到MicroPython
install: $(OBJ)
	@echo "将编译好的模块复制到MicroPython构建目录"
	@echo "请手动将k10_asr.o复制到相应的构建目录"

# 帮助
help:
	@echo "可用目标:"
	@echo "  all     - 编译ASR模块"
	@echo "  clean   - 清理编译文件"
	@echo "  install - 安装到MicroPython"
	@echo "  help    - 显示此帮助信息"

.PHONY: all clean install help
