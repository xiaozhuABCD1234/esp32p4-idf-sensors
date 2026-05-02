# ESP32-P4 开发环境（当前项目）

## ESP-IDF
- 路径: `/home/xiaozhu/.espressif/v5.5.4/esp-idf`
- 版本: v5.5.4
- Python: `/home/xiaozhu/.espressif/tools/python/v5.5.4/venv/bin/python`
- idf.py: `/home/xiaozhu/.espressif/v5.5.4/esp-idf/tools/idf.py`

**使用前先加载环境变量：**
```bash
source /home/xiaozhu/.espressif/v5.5.4/esp-idf/export.sh
```

## 常用命令
```bash
# 编译
python3 /home/xiaozhu/.espressif/v5.5.4/esp-idf/tools/idf.py build

# 烧录 (端口 /dev/ttyACM0)
python3 /home/xiaozhu/.espressif/v5.5.4/esp-idf/tools/idf.py -p /dev/ttyACM0 -b 460800 flash

# 串口监视器
python3 /home/xiaozhu/.espressif/v5.5.4/esp-idf/tools/idf.py -p /dev/ttyACM0 -b 115200 monitor

# 编译+烧录+监视
python3 /home/xiaozhu/.espressif/v5.5.4/esp-idf/tools/idf.py -p /dev/ttyACM0 -b 460800 build flash monitor
```

## 硬件接线 (SHT30 → ESP32-P4 UART1)
| SHT30 | ESP32-P4 |
|-------|----------|
| GND   | GND      |
| TXD   | GPIO5 (UART1_RX) |
| RXD   | GPIO4 (UART1_TX) |
| VCC   | 3.3V     |

## SHT30 参数
- 波特率: 9600 8N1
- 模式: 默认自动上报（每秒一帧 `R:xxx.xRH yyy.yC\r\n`）
- 命令: `Auto\r\n` / `Hand\r\n` / `Read\r\n`
