# 基于 MSPM0C1104 的 8 路巡线传感器采集与标定系统

用电赛（电子设计竞赛）的巡线小车传感器前端。MSPM0C1104 用 **12 位 ADC 序列模式**
轮流采 8 路红外巡线传感器，做均值滤波 + **上下限双阈值滞后判决**，把结果压成
一个 8 位状态字，通过 UART 按可调间隔发给主控；阈值可以在现场用按键校准后
存进 Flash。

工程最初由 TI 的 `gpio_toggle_output` 例程改建而来，所以文件名还叫那个——
但代码里已经没有一个 GPIO 翻转逻辑了，见下面「命名说明」。

```
8 路红外传感器 ──模拟电压──▶ PA27 / ADC0
                                  │  TIMG8 (500µs) 事件触发
                            12-bit SAR ADC
                            序列模式，4 个 MEM 槽
                                  │  MEM3 转换完成中断
                         奇/偶通道组交替切换
                                  │
                    4 次采样 → 均值 → 双阈值判决
                                  │
                         8 位状态字 (bit=1 表示黑线)
                        ┌─────────┴─────────┐
                    4 颗 LED            UART0 115200
                    (PA2/4/6/11)        可调 2~20ms
```

## 硬件平台

| 项目 | 说明 |
|---|---|
| MCU | TI **MSPM0C1104**，Arm Cortex-M0+，24 MHz，16 KB Flash / 1 KB SRAM，VSSOP-20 |
| 开发板 | LP-MSPM0C1104 LaunchPad（要求 Rev E3 或更新） |
| 传感器 | 8 路红外巡线模块，模拟输出接 PA27 |
| 开发环境 | CCS（Theia 版，`.ccsproject` 记录为 70.2.0） |
| SDK | MSPM0 SDK **2.10.00.04**（`.syscfg` 里写死的版本） |
| 架构 | NoRTOS 裸机，全部靠中断驱动 |

## 目录结构

```
gpio_toggle_output_LP_MSPM0C1104_nortos_ticlang/
├── gpio_toggle_output.c        全部业务代码（约 740 行，单文件）
├── gpio_toggle_output.syscfg   SysConfig 图形化配置（ADC/定时器/UART/GPIO）
├── 原理报告.md                 设计说明：外设分配、算法推导、时序分析
├── .ccsproject / .cproject      CCS 工程文件
├── .project / .settings/
├── targetConfigs/              调试器配置（XDS-110）
└── README.md
```

`Debug/` 不在仓库里——CCS 每次编译重新生成，见 `.gitignore`。

## 采集原理

### 为什么需要奇偶通道组切换

MSPM0C1104 的 ADC12 只有 **4 个 MEM 槽**，但要采 8 路。所以代码把 8 路分成两组，
在 ADC 中断里交替重配通道：

| 组 | MEM0 | MEM1 | MEM2 | MEM3 | 函数 |
|---|---|---|---|---|---|
| 奇数组 | CH5 | CH0 | CH1 | CH2 | `SetupOddChannels()` |
| 偶数组 | CH3 | CH4 | CH9 | CH8 | `SetupEvenChannels()` |

一次事件触发连续转换 4 个槽（`DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT`），MEM3
转换完成后进中断；中断里把通道组换到另一组，下一轮触发就采另外 4 路。
`g_adc_round` 记录当前是第几轮。

### 均值滤波 + 双阈值滞后

`xunji_adc_to_digital()` 每轮把 8 路值塞进 `adc_samples[8][4]`，**攒满 4 次**才处理：

```c
mean = 4 次采样之和 / 4;

if (mean > upper_thresh[ch])      bit = 1;   // 判定为黑线
else if (mean < lower_thresh[ch]) bit = 0;
else                              /* 中间区间：保持上一位不变 */ ;
```

中间那段是**滞回区间**——ADC 值在阈值附近抖动时，输出不会来回跳。这是这套代码
最实用的一处设计，去掉它巡线会在黑白边界上抽搐。

默认阈值 `upper = 3000`、`lower = 1000`（12 位，满量程 4095）。

## 现场校准

按键（PA0）短按进入两级校准状态机：

| `calib_stage` | 状态 | LED 指示 |
|---|---|---|
| 0 | 空闲 | 正常显示 8 路状态 |
| 1 | 等待把传感器放到**黑线**上，短按确认 | LED1 灭，其余亮 |
| 2 | 等待把传感器放到**白底**上，短按确认 | LED3/LED4 灭，LED1/LED2 亮 |
| 3 | 校准完成，阈值已算好并写 Flash | 全亮约 1 秒后恢复 |

上下限由黑白两次采样值算出（`calib_black[8]` / `calib_white[8]`），
存进 Flash 的 `THRESH_FLASH_ADDR = 0x1800`，结构是
`ThresholdData { uint32 magic; uint16 upper[8]; uint16 lower[8]; }`，
上电由 `load_all_from_flash()` 校验 `magic` 后恢复。

> 地址选 0x1800 是因为「本代码实际占用约 1660 字节，故在 1800 后开辟」——
> 原注释这么写的。如果你加了代码导致体积超过 0x1800，这个地址要往后挪，
> 否则会覆盖程序本身。

## 串口命令

UART0，**115200-8-N-1**，命令以 `\r\n` 结尾：

| 命令 | 作用 | 返回 |
|---|---|---|
| `STOP` | 停止自动上报 | `TESTING` |
| `START` | 恢复自动上报，并把当前挡位存 Flash | — |
| `CHANGE UARTMS X` | 改上报间隔为 X ms，可选 **2/4/5/6/8/10/20** | `FINISH` / `error` |
| `CHECK THRESH` | 打印 8 路的上下限阈值 | 16 个数值 |

默认间隔 10 ms。命令解析在 `UART_0_INST_IRQHandler()` 里逐字节收，
攒满一行后由 `uart_process_command()` 处理。

## 中断与时序

| 定时器 | 周期 | 用途 |
|---|---|---|
| `TIMER_0`（TIMG14） | 1 ms | 时基，驱动 UART 上报节拍和按键计时 |
| `TIMER_1`（TIMG8） | 500 µs | **只做 ADC 触发源**，本身不开中断 |
| `TIMER_2`（TIMA0） | 5 ms | 按键扫描（4 次采样消抖，短按 <500 ms / 长按 >500 ms） |

四个中断服务函数：

| 函数 | 触发条件 |
|---|---|
| `ADC12_0_INST_IRQHandler()` | 每 500 µs，MEM3 转换完成 |
| `UART_0_INST_IRQHandler()` | 收到一个字节 |
| `TIMER_0_INST_IRQHandler()` | 每 1 ms |
| `TIMER_2_INST_IRQHandler()` | 每 5 ms（按键） |

主循环 `while(1)` 里只有 `LED_show()` 之类的非实时工作。

## 引脚分配

全部来自 `gpio_toggle_output.syscfg`，不是注释：

| 功能 | 引脚 | 说明 |
|---|---|---|
| 传感器模拟输入 | **PA27**（ADC0）| 8 路按奇偶两组分时切换 ADC 通道，详见上文「采集原理」 |
| UART0 TX | **PA23** | 115200 |
| UART0 RX | **PA18** | |
| 按键 | **PA0** | 输入，内部上拉 |
| LED1~4 | **PA2 / PA4 / PA6 / PA11** | 4 颗 LED 交替显示 8 路状态 |
| SWD | PA20 / PA19 | 调试占用，见 TI 文档的 J101 跳线说明 |

## 构建

需要 **CCS**（Code Composer Studio，Theia 版）和 **MSPM0 SDK 2.10.00.04**。

1. CCS → `File → Import → CCS Projects`，选本目录
2. 确认 `Properties → General → MSPM0 SDK` 指向已安装的 2.10.00.04
3. `Build Project`
4. 用 XDS-110 调试器烧录（配置见 `targetConfigs/MSPM0C1104.ccxml`）

首次编译会自动生成 `Debug/`（makefile、`.o`、`.out`），不用管。

## 命名说明

工程是从 TI 的 `gpio_toggle_output` 例程改建的，文件名、工程名、文件夹名
全都还叫 `gpio_toggle_output_LP_MSPM0C1104_nortos_ticlang`，但代码内容已经是
另一套东西了。改工程名会牵动 `.ccsproject` / `.cproject` / `.syscfg` 里的
多处引用，风险大于收益，所以保留原名。

同理，`README.html` 是 TI 自动生成的例程文档（描述的是 GPIO 翻转例程），
已经和实际代码无关，仓库里没有保留。

## 踩过的坑

- **`Debug/` 不要提交。** 100 多个文件，其中 100 个是 clangd 索引缓存
  （`.idx`），跟编译结果没关系，纯噪声。
- **`.clangd` 不要提交。** 文件头自己写着 `do not add it to source-control`。
- **`THRESH_FLASH_ADDR` 要留余量。** 见上面校准一节。
- **ADC 只有 4 个 MEM 槽。** 加通道时记得同步改奇偶两组的分组表。
- **`.syscfg` 是配置的唯一来源。** `Debug/ti_msp_dl_config.c/h` 是它生成的，
  别手改生成文件，改 `.syscfg`。

## 许可

[MIT](LICENSE)。代码里不含任何第三方 GPL 组件，可以自由使用。

第三方组件（TI SDK、SysConfig 生成文件）的说明见 [NOTICE.md](NOTICE.md)。
