# ESP32-S3 鲑鱼跑信息显示器

在 AMOLED 屏幕上实时显示 Splatoon 3 鲑鱼跑（Salmon Run）的地图、时间、武器和 BOSS 信息。

## 环境要求

| 项目 | 版本 |
|------|------|
| ESP-IDF | v5.3.2 |
| 目标芯片 | ESP32-S3 |

## 依赖组件

| 组件 | 来源 | 说明 |
|------|------|------|
| LVGL | managed_components（自动下载） | v8.4 UI 框架 |
| cJSON | managed_components（自动下载） | JSON 解析库 |
| esp_io_expander | managed_components（自动下载） | IO 扩展器抽象层 |
| esp_io_expander_tca9554 | managed_components（自动下载） | TCA9554 驱动 |
| esp_lcd_sh8601 | components（项目自带） | SH8601 AMOLED 驱动 |
| esp_lcd_touch_ft5x06 | components（项目自带） | FT5x06 触摸驱动 |
| esp_lcd_touch | components（项目自带） | 触摸抽象层 |
| lodepng | main/lodepng（项目自带） | PNG 解码库 |
| esp_http_server | ESP-IDF 内置 | Web 配网服务器 |

> `managed_components` 目录未提交到仓库，首次编译时 ESP-IDF 会根据 `idf_component.yml` 自动下载。

## 硬件平台

| 项目 | 规格 |
|------|------|
| MCU | ESP32-S3（8MB PSRAM, 4MB Flash） |
| 显示屏 | 368×448 AMOLED（SH8601, QSPI 接口） |
| 触摸 | FT5x06（I2C 接口，本项目未使用） |
| IO 扩展 | TCA9554（I2C 地址 0x20） |
| 按钮 | GPIO0（侧面按钮，当前未启用） |

## 功能实现

### 1. WiFi 连接与自动配网

**首次使用（无 WiFi 配置）：**
- 设备自动创建热点 `SalmonRun-Setup`（无密码）
- 手机连接热点，自动弹出配网页面
- 输入 WiFi 名称和密码，点击 "Save & Reboot"
- 设备保存配置到 NVS 并重启

**正常使用（已有 WiFi 配置）：**
- 从 NVS 读取 WiFi 凭据
- 自动连接，等待 IP 获取（最多 30 秒）
- 超时未连接成功则自动退回配网模式
- 数据获取完成后自动断开 WiFi 节省功耗

**倒计时结束刷新：**
- 倒计时结束后等待 5 秒
- 临时开启 WiFi 获取新数据
- 获取完成后立即断开 WiFi

### 2. SNTP 时间同步

- NTP 服务器：`ntp.aliyun.com`
- 时区：`CST-8`（中国标准时间，UTC+8）
- 等待时间同步完成（年份 >= 2025）后才进行数据获取
- 最多等待 30 秒

### 3. HTTPS 数据获取

- 数据源：`https://splatoon3.ink/data/schedules.json`
- 使用 ESP-IDF 的 `esp_http_client` 组件
- TLS 证书验证：使用 `esp_crt_bundle_attach`（系统证书包）
- 超时：20 秒
- 失败重试：最多 3 次，每次间隔 2 秒
- 动态内存分配：初始按 content-length 分配，不足时自动 realloc 扩展（每次 +16KB）

### 4. JSON 解析

使用 cJSON 库解析 API 返回的 JSON 数据，提取当前正在进行的鲑鱼跑信息：

**数据路径：**
```
data.coopGroupingSchedule.regularSchedules.nodes[]
```

**解析内容：**
- `startTime` / `endTime`：日程起止时间（ISO 8601 格式，转换为 UTC+8）
- `setting.coopStage.name`：地图英文名称
- `setting.coopStage.image.url`：地图图片 URL
- `setting.boss.name`：BOSS 鲑鱼英文名称
- `setting.weapons[].image.url`：武器图片 URL

**时间处理：**
- 解析 ISO 8601 时间字符串（`%d-%d-%dT%d:%d:%dZ`）
- 转换为 `time_t` 并加上 8 小时（UTC+8）
- 与当前时间比较，找到正在进行的日程

### 5. 中文翻译

**地图名称翻译（`stage_cn` 函数）：**

| 英文 | 中文 |
|------|------|
| Spawning Grounds | 鲑坝 |
| Marooner's Bay | 漂浮落难船 |
| Sockeye Station | 新卷堡 |
| Salmonid Smokeyard | 时不知鲑烟熏工房 |
| Jammin' Salmon Junction | 生筋子系统交流道遗址 |
| Gone Fission Hydroplant | 麦年海洋发电所 |
| Ruins of Ark Polaris | 鲑鱼心斗技场 |

**BOSS 鲑鱼：**

| 英文 | 图片 |
|------|------|
| Megalodontia | king-megalodontia-D9zENODp.png |
| Cohozuna | king-cohozuna-C2i2-iKq.png |
| Triumvirate | king-triumvirate-DJIjCid3.png |
| Horrorboros | king-horrorboros-B21AjmXk.png |

### 6. 倒计时

- 每秒更新一次（通过 LVGL 定时器）
- 格式：`还剩X小时XX分XX秒`（有小时时）或 `还剩XX分XX秒`（无小时时）
- 日程结束时显示 `已结束`
- 使用 `snprintf` 手动格式化（ESP32 的 newlib 不支持 `%-m` 去前导零格式）
- **倒计时结束后自动刷新**：检测到 `已结束` 后等待 5 秒，重新联网获取下一阶段数据，获取完成后自动断开 WiFi

### 7. 时间范围显示

- 格式：`M/D HH:MM ~ M/D HH:MM`（如 `5/27 08:00 ~ 5/29 00:00`）
- 无前导零（使用 `snprintf` 的 `%d` 格式）

### 8. 图片渲染

**PNG 解码流程：**
1. HTTP 下载 PNG 原始数据
2. 使用 lodepng 解码为 RGBA 格式
3. 最近邻缩放到目标尺寸
4. 转换为 RGB565+Alpha 格式（大端序）
5. 释放 PNG 原始数据和中间 RGBA 数据
6. 设置 LVGL 图片描述符

**图片尺寸：**

| 图片 | 目标尺寸 | 说明 |
|------|----------|------|
| 武器图片 | 80×80 | 等比例缩放 |
| 地图图片 | 378 宽，最大 200 高 | 等比例缩放 |
| BOSS 图片 | 50×50 | 等比例缩放 |

**颜色格式转换（`rgba_to_rgb565a` 函数）：**
- 输入：RGBA 8888（每像素 4 字节）
- 输出：RGB565 + Alpha（每像素 3 字节，大端序）
- RGB565 编码：`((R>>3)<<11) | ((G>>2)<<5) | (B>>3)`
- 字节序：高字节在前（大端序，适配 SH8601 显示控制器）

### 9. UI 布局

**屏幕尺寸：** 368×448 像素

**元素创建顺序（z-order 从底到顶）：**
1. 地图图片（`img_map`）
2. BOSS 图片（`img_boss`）
3. 武器图片（`img_weapon[0..3]`）
4. 地图名字（`lbl_map_name`）— 黑底白字
5. 时间范围（`lbl_time_range`）
6. 倒计时（`lbl_countdown`）
7. 状态文字（`lbl_status`）— 居中
8. 发放武器标题（`lbl_weapon_title`）

**坐标布局：**

| 元素 | 对齐方式 | x | y | 字体 | 颜色 |
|------|----------|---|---|------|------|
| 地图起止时间 | TOP_MID | 0 | 40 | 22px 粗体 | 白色 |
| BOSS 图片 | TOP_MID | -120 | 90 | 50×50 | — |
| 倒计时 | TOP_MID | 30 | 105 | 24px 粗体 | 黄色 (#FEEF65) |
| 地图名字 | TOP_MID | 0 | 153 | 18px 粗体 | 白色（黑底） |
| 地图图片 | TOP_MID | 0 | 158 | 378 宽 | — |
| 发放武器 | TOP_MID | 0 | 368 | 18px 粗体 | 白色 |
| 武器图片 | TOP_MID | 分布 | 384 | 80×80 | — |
| 状态文字 | CENTER | 0 | 0 | 16px | 白色 |

**武器图片水平分布：**
- 屏幕宽度 368，分为 4 等分（每份 92px）
- 每个武器图片（80px）居中在各自的 92px 区间内

### 10. 字体

所有 CJK 字体使用 Hiragino Sans GB W6（粗体）生成，使用 `lv_font_conv` 工具。

**生成参数：**
- `--bpp 4`：4 位抗锯齿
- `--no-compress`：不压缩（`bitmap_format=0`，LVGL 兼容性要求）
- `--no-prefilter`：不预过滤
- `--format lvgl`：LVGL 格式

**字符集（51 个汉字 + ASCII）：**
```
不交分卷发器场址坝堡子小工已年心房所技放斗新时束武洋流浮海漂烟熏生电知秒筋系结统船落还道遗难鱼鲑麦剩
```

**字体文件：**

| 文件 | 大小 | 用途 |
|------|------|------|
| lv_font_pingfang_16_cjk.c | 16px | 状态文字 |
| lv_font_pingfang_18_cjk.c | 18px | 地图名字、发放武器 |
| lv_font_pingfang_22_cjk.c | 22px | 时间范围 |
| lv_font_pingfang_24_cjk.c | 24px | 倒计时 |
| lv_font_montserrat_32 | 32px | LVGL 内置（未使用） |

### 11. LVGL 配置

- 缓冲区高度：10 行（双缓冲，DMA 内存）
- 刷新回调：`flush_cb` → `esp_lcd_panel_draw_bitmap`
- 圆角回调：`round_cb`（坐标对齐到偶数）
- Tick 周期：10ms
- LVGL 任务：5-50ms 动态延迟
- 背景：黑色，不透明
- 屏幕：禁用滚动和滚动条

### 12. 显示控制器初始化

SH8601 AMOLED 控制器通过 QSPI 接口初始化：

```c
初始化命令序列：
0x11: Sleep Out（等待 120ms）
0x44: 设置列地址范围（0x01D1 = 465，即 368-1+96-1+1）
0x35: 开启 Tear Effect
0x53: 开启亮度控制
0x2A: 列地址设置（0-367）
0x2B: 行地址设置（0-447）
0x51: 亮度设置（初始 0x00，后设为 0x80）
0x29: Display On
```

### 13. IO 扩展器（TCA9554）

用于控制显示屏的电源和复位：

| 引脚 | 方向 | 初始值 | 说明 |
|------|------|--------|------|
| IO0 | 输出 | 高 | 电源控制 |
| IO1 | 输出 | 高 | 电源控制 |
| IO2 | 输出 | 高 | 电源控制 |
| IO7 | 输出 | 低→高 | 复位信号（200ms 延迟后拉高） |

### 14. 电源管理

- CPU 频率：固定 80MHz（`max_freq_mhz = 80, min_freq_mhz = 80`）
- 禁用 light sleep（`light_sleep_enable = false`）

## 数据流

```
启动
  │
  ├─ 初始化 NVS、电源管理、I2C、IO 扩展器、SPI、显示屏、LVGL
  │
  ├─ 创建 LVGL 任务（后台运行 lv_timer_handler）
  │
  ├─ 创建 UI 元素（全部隐藏，显示 "Loading..."）
  │
  └─ 创建 fetch_worker 任务
       │
       ├─ 延迟 2 秒
       │
       ├─ 初始化 WiFi 并连接
       │
       ├─ 等待 IP 获取（最多 30 秒）
       │
       ├─ SNTP 时间同步（最多 30 秒）
       │
       ├─ 循环最多 3 次：
       │    ├─ HTTPS 获取 schedules.json
       │    ├─ JSON 解析找到当前日程
       │    ├─ 更新 UI 文字（地图名、时间、倒计时）
       │    ├─ 下载并显示武器图片（4 张）
       │    ├─ 下载并显示地图图片
       │    ├─ 下载并显示 BOSS 图片
       │    └─ 成功则跳出循环
       │
       ├─ 失败则显示 "Fetch Failed"
       │
       ├─ 断开 WiFi，停止 WiFi
       │
       └─ 删除任务
```

## 文件结构

```
06_SALMON_RUN/
├── CMakeLists.txt                      # 项目构建配置
├── partitions.csv                      # 分区表
├── sdkconfig                           # ESP-IDF 配置
├── sdkconfig.defaults                  # 默认配置
├── README.md                           # 本文档
├── main/
│   ├── salmon_run.c                    # 主程序（722 行）
│   ├── lv_font_pingfang_16_cjk.c      # 16px 粗体中文字体
│   ├── lv_font_pingfang_18_cjk.c      # 18px 粗体中文字体
│   ├── lv_font_pingfang_22_cjk.c      # 22px 粗体中文字体
│   ├── lv_font_pingfang_24_cjk.c      # 24px 粗体中文字体
│   ├── CMakeLists.txt                  # 组件构建配置
│   └── idf_component.yml              # 组件依赖声明
├── components/
│   ├── esp_lcd_sh8601/                 # SH8601 AMOLED 驱动
│   ├── espressif__esp_lcd_touch_ft5x06/ # FT5x06 触摸驱动
│   ├── espressif__esp_lcd_touch/       # 触摸抽象层
│   └── espressif__cmake_utilities/     # CMake 工具
├── managed_components/
│   ├── lvgl__lvgl/                     # LVGL 8.4 UI 框架
│   ├── espressif__cJSON/               # JSON 解析库
│   ├── espressif__esp_io_expander/     # IO 扩展器抽象层
│   └── espressif__esp_io_expander_tca9554/ # TCA9554 驱动
└── build/                              # 编译输出
```

## API 数据源

| 数据 | URL | 说明 |
|------|-----|------|
| 日程 JSON | `https://splatoon3.ink/data/schedules.json` | 包含所有模式的日程 |
| BOSS 图片 | `https://splatoon3.ink/assets/king-{name}.png` | BOSS 鲑鱼插画 |
| 武器图片 | `https://splatoon3.ink/assets/splatnet/v3/weapon_illust/{hash}_0.png` | 武器插画 |
| 地图图片 | `https://splatoon3.ink/assets/splatnet/v3/stage_img/icon/high_resolution/{hash}_0.png` | 地图缩略图 |

## 构建与烧录

### 1. 编译

```bash
# 设置 ESP-IDF 环境
source ~/.espressif/v5.3.2/esp-idf/export.sh

# 首次编译会自动下载 managed_components 依赖
idf.py build
```

### 2. 烧录

```bash
# 替换为实际串口
idf.py -p /dev/cu.usbmodem21201 flash

# 串口监控
idf.py -p /dev/cu.usbmodem21201 monitor
```

### 3. 首次配网

1. 烧录后设备自动进入配网模式
2. 屏幕显示 "WiFi Setup" 和热点名称
3. 手机连接 `SalmonRun-Setup` 热点
4. 浏览器自动弹出配网页面（如未弹出，访问 `192.168.4.1`）
5. 输入 WiFi 名称和密码，点击 "Save & Reboot"
6. 设备重启后自动连接 WiFi 并获取数据

> WiFi 配置保存在 NVS 中，重新烧录固件不会丢失。如需重新配网，清除 NVS 分区即可。

## 已解决的问题

| 问题 | 根因 | 解决方案 |
|------|------|----------|
| LoadProhibited 崩溃 | `free(png)` 后 LVGL 仍引用已释放数据 | 解码后立即释放 PNG，不保留原始数据 |
| 霓虹色显示 | RGB565 字节序错误（小端序） | 改为大端序（高字节在前） |
| 中文方块 | 字体 `bitmap_format=1` 与 LVGL 不兼容 | 使用 `--no-compress` 生成（`bitmap_format=0`） |
| 32px 字体不显示 | 同上 | 同上 |
| 地图名被遮盖 | 图片后创建覆盖了文字 | 调整创建顺序：图片先创建，文字后创建 |
| WiFi 未连接就发请求 | 缺少 IP 获取等待 | 添加 `esp_netif_get_ip_info()` 轮询 |
| 屏幕底部白色条 | LVGL 默认滚动条 | 禁用 `LV_OBJ_FLAG_SCROLLABLE` 和 `LV_SCROLLBAR_MODE_OFF` |
| 按钮导致启动循环 | GPIO0 是 strapping pin，配置干扰启动 | 暂时禁用按钮功能 |

## 当前限制

- **无深度休眠**：GPIO0 按钮功能暂未实现（strapping pin 冲突）
- **字体字符集有限**：仅包含当前显示所需的 52 个汉字，新增翻译需重新生成字体

## License

本项目仅供个人学习使用。Splatoon 3 游戏内容版权归 Nintendo 所有。
