# ESP32-P4 SHT30 + CJ702 + WiFi (via ESP32-C6 SDIO)

## 概述

ESP32-P4 通过 UART 读取 SHT30 温湿度传感器，通过 SDIO 连接 ESP32-C6 作为 WiFi 协处理器（ESP-Hosted-MCU），实现联网功能。

## 硬件接线

### SHT30 → ESP32-P4 (UART1)

| SHT30 | ESP32-P4 |
|-------|----------|
| GND   | GND      |
| TXD   | GPIO5 (UART1_RX) |
| RXD   | GPIO4 (UART1_TX) |
| VCC   | 3.3V     |

### ESP32-P4 ↔ ESP32-C6 (SDIO, 4-bit)

| SDIO 信号 | P4 (Host) | C6 (Slave) |
|-----------|-----------|------------|
| CLK       | GPIO18    | GPIO19     |
| CMD       | GPIO19    | GPIO18     |
| DATA0     | GPIO14    | GPIO20     |
| DATA1     | GPIO15    | GPIO21     |
| DATA2     | GPIO16    | GPIO22     |
| DATA3     | GPIO17    | GPIO23     |
| RST       | GPIO54    | EN         |

- CMD 及 DATA0~DATA3 需外接 10K~51K 上拉电阻
- CLK 线不需上拉，尽量短且包地

## 软件架构

```
app_main()
├── nvs_flash_init()
├── esp_hosted_init()        // SDIO → C6 初始化 (ESP-Hosted-MCU)
├── esp_netif_init()
├── esp_wifi_init()          // 通过 RPC 转发到 C6 执行
├── esp_wifi_set_mode(STA)
├── esp_wifi_start()         // 自动连接 AP
└── uart_init() + loop       // 读取 SHT30 温湿度
```

## 依赖组件

由 `main/idf_component.yml` 管理：
- `espressif/esp_wifi_remote` — 透明 WiFi API（RPC 到 C6）
- `espressif/esp_hosted` — SDIO 传输层

## 配置

```bash
idf.py menuconfig
```

| 菜单路径 | 说明 |
|----------|------|
| WiFi Configuration → WiFi SSID | 路由器名称 |
| WiFi Configuration → WiFi Password | 路由器密码 |
| Component config → ESP-Hosted config | SDIO 引脚/时钟等（已预设为 Function EV Board 引脚） |

> 已通过 `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y` 预设正确引脚。

## 编译 & 烧录 & 监视

```bash
idf.py -p /dev/ttyACM0 -b 460800 build flash monitor
```

## ESP32-C6 从机固件

C6 需预烧 ESP-Hosted slave 固件（SDIO 模式）。如未烧录：

```bash
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave && idf.py set-target esp32c6
idf.py -p /dev/ttyUSBx flash
```

## 预期日志

```
I (1903) sdio_wrapper: SDIO master: Slot 1, 4-bit, 20000 KHz
I (2043) H_SDIO_DRV: Card init success
I (2073) transport: Identified slave [esp32c6]
I (2093) transport: Features: WLAN
I (2603) WiFi: Connecting to AP...
I (5703) Station mode: Connected
I (6753) WiFi: Got IP: 192.168.3.213
I (2903) SHT30: Humidity: 65.9%RH, Temperature: 24.4C
```

## SHT30 参数

- 波特率: 9600 8N1
- 默认自动上报（每秒一帧 `R:xxx.xRH yyy.yC\r\n`）
- 命令: `Auto\r\n` / `Hand\r\n` / `Read\r\n`

## 许可

MIT
