# ESP32-P4 SHT30 + CJ702 + WiFi + MQTT (via ESP32-C6 SDIO)

## 概述

ESP32-P4 通过 UART1 读取 SHT30 温湿度传感器、UART2 读取 CJ702 空气质量传感器（eCO₂/eCH₂O/TVOC/PM2.5/PM10/温湿度），通过 SDIO 连接 ESP32-C6 作为 WiFi 协处理器（ESP-Hosted-MCU），数据通过 MQTT 发布到云端。

## 硬件接线

### SHT30 → ESP32-P4 (UART1)

| SHT30 | ESP32-P4         |
| ----- | ---------------- |
| GND   | GND              |
| TXD   | GPIO5 (UART1_RX) |
| RXD   | GPIO4 (UART1_TX) |
| VCC   | 3.3V             |

### CJ702 → ESP32-P4 (UART2)

| CJ702 | ESP32-P4         |
| ----- | ---------------- |
| GND   | GND              |
| TXD   | GPIO3 (UART2_RX) |
| VCC   | 3.3V             |

> CJ702 为单工通信（只发不收），无需连接 RX。

### ESP32-P4 ↔ ESP32-C6 (SDIO, 4-bit)

| SDIO 信号 | P4 (Host) | C6 (Slave) |
| --------- | --------- | ---------- |
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
├── esp_hosted_init()            // SDIO → C6 初始化 (ESP-Hosted-MCU)
├── esp_netif_init()
├── esp_wifi_init()              // 通过 RPC 转发到 C6 执行
├── esp_wifi_set_mode(STA)
├── esp_wifi_start()             // 自动连接 AP
├── uart_init()                  // UART1 → SHT30 读取
├── uart_init_cj702()            // UART2 → CJ702 读取
├── sht30_read_task()            // 解析 SHT30 帧，支持 Immediate 发布
├── cj702_read_task()            // 解析 CJ702 协议帧，支持 Immediate 发布
└── mqtt_publish_task()          // 等待 WiFi+MQTT 就绪，按间隔发布
```

## 依赖组件

由 `main/idf_component.yml` 管理：

- `espressif/esp_wifi_remote` — 透明 WiFi API（RPC 到 C6）
- `espressif/esp_hosted` — SDIO 传输层

## 配置

```bash
idf.py menuconfig
```

**Project Configuration （项目级配置）**

| 菜单路径                                                          | 说明                                      |
| ----------------------------------------------------------------- | ----------------------------------------- |
| Project Configuration → WiFi Configuration → WiFi SSID            | 路由器名称                                |
| Project Configuration → WiFi Configuration → WiFi Password        | 路由器密码                                |
| Project Configuration → MQTT Configuration → Broker URI           | MQTT 代理地址（支持 tcp/ssl/ws/wss）      |
| Project Configuration → MQTT Configuration → Publish Interval (s) | 定时发布间隔（1–3600 秒）                 |
| Project Configuration → SHT30 Configuration → Publish Topic       | SHT30 MQTT 主题                           |
| Project Configuration → SHT30 Configuration → Publish Mode        | Timed（定时） / Immediate（每帧立即发布） |
| Project Configuration → CJ702 Configuration → Publish Topic       | CJ702 MQTT 主题                           |
| Project Configuration → CJ702 Configuration → Publish Mode        | Timed（定时） / Immediate（每帧立即发布） |

> SDIO 引脚配置通过 `CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y` 预设。

## 编译 & 烧录 & 监视

```bash
idf.py -p /dev/ttyACM0 -b 460800 build flash monitor
```

## CJ702 传感器

- 波特率: 9600 8N1
- 数据帧: 17 字节
- 帧格式:

```
byte[0]   = 0x3C (帧头)
byte[1]   = 0x02 (帧头)
byte[2:3] = eCO₂ (ppb, 大端)
byte[4:5] = eCH₂O (ppb, 大端)
byte[6:7] = TVOC (ppb, 大端)
byte[8:9] = PM2.5 (µg/m³, 大端)
byte[10:11] = PM10 (µg/m³, 大端)
byte[12]   = 温度整数部分 (bit7=符号)
byte[13]   = 温度小数部分 (0.1°C)
byte[14]   = 湿度整数部分 (%)
byte[15]   = 湿度小数部分 (0.1%)
byte[16]   = 校验和 (byte[0]~byte[15] 累加和)
```

## SHT30 参数

- 波特率: 9600 8N1
- 模式: 默认自动上报（每秒一帧 `R:xxx.xRH yyy.yC\r\n`）
- 命令: `Auto\r\n` / `Hand\r\n` / `Read\r\n`

## MQTT 数据格式

### SHT30 (JSON)

```json
{ "humidity": 52.3, "temperature": 25.1 }
```

### CJ702 (JSON)

```json
{
	"eco2": 424,
	"ech2o": 2,
	"tvoc": 15,
	"pm25": 12,
	"pm10": 14,
	"temperature": 24.4,
	"humidity": 54.6
}
```

## 预期日志

```
I (1903) sdio_wrapper: SDIO master: Slot 1, 4-bit, 20000 KHz
I (2043) H_SDIO_DRV: Card init success
I (2073) transport: Identified slave [esp32c6]
I (2603) WiFi: Connecting to AP...
I (2766) SHT30: Humidity: 52.3%RH, Temperature: 25.0C
I (3726) CJ702: eCO2=419ppb eCH2O=2ppb TVOC=12ppb PM2.5=12ug/m3 PM10=14ug/m3 Temp=24.3C Hum=54.7%
I (5796) Station mode: Connected
I (6866) WiFi: Got IP: 192.168.3.213
I (8176) MQTT: Connected to broker
I (8186) MQTT: Published to /sensors/sht30: {"humidity":52.2,"temperature":25.1}
I (8186) MQTT: Published to /sensors/cj702: {"eco2":422,"ech2o":2,"tvoc":14,...}
```

## ESP32-C6 从机固件

C6 需预烧 ESP-Hosted slave 固件（SDIO 模式）：

```bash
idf.py create-project-from-example "espressif/esp_hosted:slave"
cd slave && idf.py set-target esp32c6
idf.py -p /dev/ttyUSBx flash
```

## 许可

MIT
