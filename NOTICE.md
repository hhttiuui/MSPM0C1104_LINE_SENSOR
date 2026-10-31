# 第三方组件与许可声明

本仓库的**自研部分**（`gpio_toggle_output.c`、`gpio_toggle_output.syscfg`、
`原理报告.md`）由 hhttiuui 编写，以 [MIT](LICENSE) 发布。

选 MIT 而不是 GPL：这个工程**不包含任何 GPL 代码**，`libraries/` 那种情况
在这里不存在，所以可以自由选择许可。

下面逐项说明用到的第三方内容。

---

## 1. TI MSPM0 SDK — TI 商业软件许可

本仓库**不包含 SDK 源码**。编译时需要你自行安装
**MSPM0 SDK 2.10.00.04**，并遵守 TI 的软件许可协议
（`license_mspm0_sdk_2_10_00_04.txt`，随 SDK 一起分发）。

这份协议不是 BSD/MIT/GPL 之类的开源许可，是 TI 自己的商业许可。要点：
- SDK 可免费用来开发 TI 器件；
- 编译产物（Output）可以自由分发；
- SDK 源码本身不应原样再分发。

所以本仓库里没有 `startup_mspm0c110x_ticlang.c`、`ti/driverlib/*` 这些文件
——它们来自 SDK，构建时自动引用。

---

## 2. SysConfig 生成文件

`Debug/ti_msp_dl_config.c` / `.h` 是 TI SysConfig 工具根据
`gpio_toggle_output.syscfg` **自动生成**的，属于 TI 许可里的 "Output"，
可以随你的项目分发。

但它们**不在本仓库里**（`Debug/` 已加入 `.gitignore`）——第一次 Build 时
会由 SysConfig 重新生成，不需要手写也不需要提交。

如果你想修改引脚或外设配置，改 `gpio_toggle_output.syscfg`，不要改生成文件。

---

## 3. TI 例程出处

工程骨架来自 MSPM0 SDK 自带的例程：

```
examples/nortos/LP_MSPM0C1104/driverlib/gpio_toggle_output/ticlang/
```

这个出处记录在 `.ccsproject` 的 `<origin>` 字段里。原例程本身是 TI 的
示例代码，按上面第 1 条处理。

例程原有的 `README.html`（描述 GPIO 翻转例程的自动生成文档）已经删除——
它和现在的代码内容无关，留着只会误导。

---

## 4. 构建工具（不入库）

- **CCS（Code Composer Studio，Theia 版）** — 免费，需自行安装
- **MSPM0 SDK 2.10.00.04** — 见上
- **XDS-110 调试器** — LP-MSPM0C1104 板载

本仓库不包含任何编译器、SDK 或调试器文件。
