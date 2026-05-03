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

# 全量重建（Python 路径变更时）
python3 /home/xiaozhu/.espressif/v5.5.4/esp-idf/tools/idf.py fullclean build
```

## Kconfig 层级

`main/Kconfig.projbuild`:

```
Project Configuration
├── WiFi Configuration
│   ├── WiFi SSID
│   └── WiFi Password
├── MQTT Configuration
│   ├── Broker URI
│   └── Publish Interval (1–3600s)
├── SHT30 Configuration
│   ├── Publish Topic
│   └── Publish Mode (Timed / Immediate)
└── CJ702 Configuration
    ├── Publish Topic
    └── Publish Mode (Timed / Immediate)
```

## 硬件接线

### SHT30 → ESP32-P4 UART1

| SHT30 | ESP32-P4         |
| ----- | ---------------- |
| GND   | GND              |
| TXD   | GPIO5 (UART1_RX) |
| RXD   | GPIO4 (UART1_TX) |
| VCC   | 3.3V             |

### CJ702 → ESP32-P4 UART2

| CJ702 | ESP32-P4         |
| ----- | ---------------- |
| GND   | GND              |
| TXD   | GPIO3 (UART2_RX) |
| VCC   | 3.3V             |

### ESP32-P4 ↔ ESP32-C6 SDIO

| SDIO 信号 | P4 (Host) | C6 (Slave) |
| --------- | --------- | ---------- |
| CLK       | GPIO18    | GPIO19     |
| CMD       | GPIO19    | GPIO18     |
| DATA0     | GPIO14    | GPIO20     |
| DATA1     | GPIO15    | GPIO21     |
| DATA2     | GPIO16    | GPIO22     |
| DATA3     | GPIO17    | GPIO23     |
| RST       | GPIO54    | EN         |

## SHT30 参数

- 波特率: 9600 8N1
- 模式: 默认自动上报（每秒一帧 `R:xxx.xRH yyy.yC\r\n`）
- 命令: `Auto\r\n` / `Hand\r\n` / `Read\r\n`

## CJ702 参数

- 波特率: 9600 8N1
- 帧长: 17 字节，帧头 `0x3C 0x02`
- 数据: eCO₂/eCH₂O/TVOC/PM2.5/PM10/温度/湿度
- 校验: byte[0]~byte[15] 累加和
