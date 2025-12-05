
#include "ti_msp_dl_config.h"

#if 1
/* 在假定 CPU_CLK = 24MHz 时，此延迟大约为 0.5 秒 */
#endif
#define DELAY (12000000)

/* 设置为你实际规划的主 Flash 地址，需对齐到扇区边界，并落在可写主存区域 */
#define THRESH_FLASH_ADDR   (0x00001600)   /* 示例地址：请替换为你的实际地址 */
#define THRESH_MAGIC        (0x54485245u)   /* 'THRE' */
#define THRESH_SECTOR_SIZE  (0x00001000u)   /* 示例扇区大小 4KB：按芯片实际值调整 */

/* 可选：避免重复保存 */
static uint8_t calib_saved_once = 0;

/* ---------- Local prototypes ---------- */
static bool flash_unprotect_erase(uint32_t addr);
static bool flash_program_bytes(uint32_t addr, const uint8_t *data, uint32_t len);
static bool flash_verify_bytes(uint32_t addr, const uint8_t *data, uint32_t len);

volatile uint16_t ADC_Value[32];

uint16_t adc_get_value[9];//单次采集的ADC值存储地
uint8_t ADC_Value_digital;//8位数据，1的“位”对应检测到黑线
uint8_t receive_data = 0;       //用于串口接收
uint8_t time_count = 0;     //定时器测试变量
volatile uint8_t g_adc_round = 0;//通道变换参数，为奇偶数时更换采集通道

/* 用于存储4次采集的八个通道的数据 */
static uint16_t adc_samples[8][4];
static uint8_t adc_sample_cnt = 0; /* 0-3，当记到3时，进行滤波和数字转换 */

/* 上下限阈值数组（运行时内存，仅校准阶段更新，不再持久化） */
static uint16_t adc_upper_thresh[8] = {3000,3000,3000,3000,3000,3000,3000,3000};
static uint16_t adc_lower_thresh[8] = {1000,1000,1000,1000,1000,1000,1000,1000};

/* 上一次通道的均值 (在 xunji_adc_to_digital中更新) */
static uint16_t adc_last_mean[8] = {0};

/* 按键状态检测 (TIMER_2 每5ms触发一次) */
static uint8_t key_samples[4] = {0}; /* 4次采集 1 = 按下（低电平）, 0 = 没按下（高电平） */
static uint8_t key_sample_idx = 0;
static uint16_t key_press_ticks = 0; /* 计数器，用于判断长按短按 */
static uint8_t key_prev_state = 0; /* 判断按键是否松开0=松开,1=按下 */
static uint8_t key_long_detected = 0; /* 用于避免重复触发长按事件 */

/*按键事件，短按（<500ms），长按（>500ms）*/
volatile uint8_t key_short_flag = 0;
volatile uint8_t key_long_flag = 0;

/* 校准状态机 */
/* 0 = 空闲, 1 = 等待黑色+短按, 2 = 等待白色+短按, 3 = 完成 */
static uint8_t calib_stage = 0;
static uint16_t calib_black[8];
static uint16_t calib_white[8];

/* 用于LED显示计数，当完成校准后LED会全量1s，然后恢复默认 */
static uint16_t calib_show_ticks = 0;

/* 阈值数据结构 */
typedef struct {
    uint32_t magic;          // 标志位，用于判断数据是否有效
    uint16_t upper[8];
    uint16_t lower[8];
} ThresholdData;

void xunji_adc_to_digital(void);
void LED_show(void);

void SetupOddChannels(void);//ͨ������
void SetupEvenChannels(void);

static bool flash_erase_sector(uint32_t addr);
static bool flash_program_64(uint32_t addr, const uint32_t data32[2]);
static bool flash_program_32(uint32_t addr, const uint32_t *data32);
static bool flash_verify_magic(uint32_t addr, uint32_t expected_magic);
void load_thresholds_from_flash_if_valid(void);
bool save_thresholds_to_flash_atomic(void);


//volatile uint8_t *readData8 = ((volatile uint8_t*)THRESH_FLASH_ADDR);
volatile uint8_t *td11 = (volatile uint8_t *)THRESH_FLASH_ADDR;
volatile uint8_t *td12 = (volatile uint8_t *)THRESH_FLASH_ADDR + 32;
int main(void)
{
    SYSCFG_DL_init();

    load_thresholds_from_flash_if_valid();

    NVIC_ClearPendingIRQ(UART_0_INST_INT_IRQN);
    NVIC_EnableIRQ(UART_0_INST_INT_IRQN);//串口
    NVIC_EnableIRQ(TIMER_0_INST_INT_IRQN);//用于发送
    NVIC_EnableIRQ(TIMER_2_INST_INT_IRQN);//用于按键

    NVIC_EnableIRQ(ADC12_0_INST_INT_IRQN);//ADC采集完成的中断
    DL_ADC12_startConversion(ADC12_0_INST);//启动ADC
    DL_TimerA_startCounter(TIMER_1_INST);//启动TIM1用于触发ADC

    DL_GPIO_setPins(GPIO_LEDS_PORT,GPIO_LEDS_USER_LED_1_PIN);



    while (1) {
        LED_show();//LED显示
        if(calib_stage==3)
        {
            bool a = save_thresholds_to_flash_atomic();
        }
        
        delay_cycles(100000);

    }
}

/* ---------- 将内存阈值写入 Flash（调用点见下文） ---------- */
bool save_thresholds_to_flash_atomic(void)
{
    /* 仅在尚未保存且校准完成时调用（外部检查 calib_stage==3） */
    if (calib_saved_once == 0) {
        return true;
    }

    /* 1) 将 adc_upper_thresh / adc_lower_thresh 打包为 4 个 64-bit 单元（8 个 32-bit） */
    uint32_t word32[8]; /* 4 * 64-bit = 8 * 32-bit */
    for (int i = 0; i < 4; i++) {
        /* 低 32 位：upper[2*i] | (upper[2*i+1] << 16) */
        uint32_t low32 = ((uint32_t)adc_upper_thresh[2*i]) | (((uint32_t)adc_upper_thresh[2*i + 1]) << 16);
        /* 高 32 位：lower[2*i] | (lower[2*i+1] << 16) */
        uint32_t high32 = ((uint32_t)adc_lower_thresh[2*i]) | (((uint32_t)adc_lower_thresh[2*i + 1]) << 16);
        word32[2*i + 0] = low32;
        word32[2*i + 1] = high32;
    }

    /* 2) 擦除目标扇区（一次） */
    if (!flash_erase_sector(THRESH_FLASH_ADDR)) {
        return false;
    }

    /* 3) 逐个 64-bit 写入（每次写入前清状态并解除保护以保险） */
    for (int i = 0; i < 4; i++) {
        uint32_t target_addr = THRESH_FLASH_ADDR + (i * 8); /* 每个 64-bit 占 8 字节 */
        if (!flash_program_64(target_addr, &word32[2*i])) {
            return false;
        }
    }
   
    
    /* 4) 最后写入 magic（放在结构末尾偏移 32） */
    uint32_t magic_addr = THRESH_FLASH_ADDR + 32; /* 32 字节偏移，magic 占 4 字节 */
     if (!flash_program_32(magic_addr, (uint32_t[]){THRESH_MAGIC})) {
         return false;
     }
   
    /* 5) 写后校验 magic */
     if (!flash_verify_magic(THRESH_FLASH_ADDR, THRESH_MAGIC)) {
         return false;
     }
    //uint8_t readData = *readData8;
    calib_saved_once = 1;
    return true;
}

/* ---------- 上电加载函数：若 flash 中有有效 magic，则把阈值读回内存 ---------- */
void load_thresholds_from_flash_if_valid(void)
{
     const ThresholdData *td = (const ThresholdData *)THRESH_FLASH_ADDR;
    // if (td->magic != THRESH_MAGIC) {
    //     /* 无效：保留运行时默认阈值（或触发重新校准） */
    //     return;
    // }

    /* 有效：拷贝到运行时阈值数组 */
    for (uint8_t i = 0; i < 8; i++) {
        adc_upper_thresh[i] = td->upper[i];
        adc_lower_thresh[i] = td->lower[i];
    }

    /* 标记已加载（避免重复保存） */
    calib_saved_once = 1;
}

/* ---------- Flash 操作封装（基于你提供的 DL_FlashCTL API） ---------- */
static bool flash_erase_sector(uint32_t addr)
{
    DL_FLASHCTL_COMMAND_STATUS status;

    DL_FlashCTL_executeClearStatus(FLASHCTL);
    DL_FlashCTL_unprotectSector(FLASHCTL, addr, DL_FLASHCTL_REGION_SELECT_MAIN);

    /* 擦除整扇区：使用示例 API（若你的 SDK 名称不同请替换） */
    status = DL_FlashCTL_eraseMemoryFromRAM(FLASHCTL, addr, DL_FLASHCTL_COMMAND_SIZE_SECTOR);
    if (status == DL_FLASHCTL_COMMAND_STATUS_FAILED) {
        return false;
    }
    return true;
}

static bool flash_program_64(uint32_t addr, const uint32_t data32[2])
{
    DL_FLASHCTL_COMMAND_STATUS status;

    DL_FlashCTL_executeClearStatus(FLASHCTL);
    DL_FlashCTL_unprotectSector(FLASHCTL, addr, DL_FLASHCTL_REGION_SELECT_MAIN);

    /* programMemoryFromRAM64 接受指向两个 32-bit 单元的指针 */
    status = DL_FlashCTL_programMemoryFromRAM64(FLASHCTL, addr, (uint32_t *)data32);
    if (status == DL_FLASHCTL_COMMAND_STATUS_FAILED) {
        return false;
    }
    return true;
}

static bool flash_program_32(uint32_t addr, const uint32_t *data32)
{
    DL_FLASHCTL_COMMAND_STATUS status;

    DL_FlashCTL_executeClearStatus(FLASHCTL);
    DL_FlashCTL_unprotectSector(FLASHCTL, addr, DL_FLASHCTL_REGION_SELECT_MAIN);

    status = DL_FlashCTL_programMemoryFromRAM32(FLASHCTL, addr, (uint32_t *)data32);
    if (status == DL_FLASHCTL_COMMAND_STATUS_FAILED) {
        return false;
    }
    return true;
}

static bool flash_verify_magic(uint32_t addr, uint32_t expected_magic)
{
    const uint32_t *p = (const uint32_t *)(addr + 32);
    /* 直接读取 flash 地址并比较 */
    if (*p != expected_magic) {
        return false;
    }
    return true;
}

void ADC12_0_INST_IRQHandler(void)//每500us调用一次
{
    switch (DL_ADC12_getPendingInterrupt(ADC12_0_INST)) {
        case DL_ADC12_IIDX_MEM3_RESULT_LOADED:

            DL_ADC12_disableConversions(ADC12_0_INST);
            g_adc_round++;
            if(g_adc_round % 2 == 1)
            {
                adc_get_value[3] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_0);
                adc_get_value[2] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_1);
                adc_get_value[1] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_2);
                adc_get_value[0] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_3);
                SetupOddChannels();

            }
            if(g_adc_round % 2 == 0)
            {
                adc_get_value[7] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_0);
                adc_get_value[6] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_1);
                adc_get_value[5] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_2);
                adc_get_value[4] = DL_ADC12_getMemResult(ADC12_0_INST, DL_ADC12_MEM_IDX_3);
                SetupEvenChannels();
            }
            xunji_adc_to_digital();//ADC转uint8

            DL_ADC12_enableConversions(ADC12_0_INST);
            DL_ADC12_startConversion(ADC12_0_INST);

            break;
        default:
            break;
    }
}



void UART_0_INST_IRQHandler(void)
{
    receive_data = DL_UART_Main_receiveData(UART_0_INST);
    if(receive_data == 1)DL_UART_Main_transmitData(UART_0_INST,adc_upper_thresh[0]);

    
    if(receive_data == 2)DL_UART_Main_transmitData(UART_0_INST,*td11);
}

void TIMER_0_INST_IRQHandler(void)//该定时器用于每10ms发送一次数据
{
    time_count++;
    if(receive_data == 0)DL_UART_Main_transmitData(UART_0_INST,ADC_Value_digital);
    
    /* 处理校准 LED 超时（每个 tick 约 10ms） */
    if (calib_show_ticks > 0) {
        calib_show_ticks--;
        if (calib_show_ticks == 0) {
             
            /* 超时到期：退出校准模式并恢复正常显示 */
            calib_stage = 0;
        }
    }
}

void TIMER_2_INST_IRQHandler(void)//用于按键
{
    /* 每5ms采集一次引脚，并保留4次*/
    uint32_t val = DL_GPIO_readPins(GPIO_KEY_PORT, GPIO_KEY_PIN_0_PIN);
    /* val == 0 说明按键按下 */
    key_samples[key_sample_idx] = (val == 0) ? 1u : 0u;
    key_sample_idx = (key_sample_idx + 1) & 0x03;

    /* 判断检测到3次以上按下时，视为按下按键 */
    uint8_t pressed_count = key_samples[0] + key_samples[1] + key_samples[2] + key_samples[3];
    uint8_t pressed = (pressed_count >= 3) ? 1u : 0u;

    if (pressed) {
        /* 仍处于按下 */
        if (!key_prev_state) {
            /* 新的按下沿 */
            key_prev_state = 1;
            key_press_ticks = 0;
            key_long_detected = 0;
        }
        key_press_ticks++; /* 每个 tick = 5ms */

        /* 检查长按：500ms -> 5ms 的 tick 需 100 次（500/5 = 100） */
        if (!key_long_detected && (key_press_ticks * 5 >= 500)) {
            key_long_detected = 1;
            key_long_flag = 1; /* 通知主逻辑 */
            /* 长按进入校准模式 */
            calib_stage = 1; /* 等待黑色样本 */

            /* 指示用户：仅点亮 LED1 */
            DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN | GPIO_LEDS_USER_LED_3_PIN | GPIO_LEDS_USER_LED_4_PIN);
            DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
        }
    } else {
        /* 未按下 */
        if (key_prev_state) {
            /* 松开沿 */
            if (!key_long_detected) {
                /* 检测到短按 */
                key_short_flag = 1;

                /* 若处于校准模式，处理短按事件 */
                if (calib_stage == 1) {
                    /* 从上次均值中记录黑色参考 */
                    for (uint8_t ch = 0; ch < 8; ch++) {
                        calib_black[ch] = adc_last_mean[ch];
                    }
                    /* 进入白色阶段并通过点亮2个LED指示 */
                    calib_stage = 2;
                    DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN | GPIO_LEDS_USER_LED_2_PIN);
                    DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_3_PIN | GPIO_LEDS_USER_LED_4_PIN);
                } else if (calib_stage == 2) {
                    /* 记录白色参考 */
                    for (uint8_t ch = 0; ch < 8; ch++) {
                        calib_white[ch] = adc_last_mean[ch];
                    }

                    /* 计算阈值：取黑白间的1/3和2/3 */
                    for (uint8_t ch = 0; ch < 8; ch++) {
                        uint16_t b = calib_black[ch];
                        uint16_t w = calib_white[ch];
                        uint16_t lo, hi;
                        if (b <= w) {
                            uint16_t diff = w - b;
                            lo = b + diff / 3u;
                            hi = b + (2u * diff) / 3u;
                        } else {
                            /* 读数反向：交换角色 */
                            uint16_t diff = b - w;
                            /* 此处 w 为较暗，b 为较亮 */
                            lo = w + diff / 3u;
                            hi = w + (2u * diff) / 3u;
                        }
                        adc_lower_thresh[ch] = lo;
                        adc_upper_thresh[ch] = hi;
                    }

                    calib_stage = 3; /* 完成 */
                   
                    /* 指示完成：短时间点亮所有 LED（由 TIMER_0 处理） */
                    calib_show_ticks = 100; /* 100 * 10ms ≈ 1s */
                }
            }
            key_prev_state = 0;
            key_press_ticks = 0;
            key_long_detected = 0;
        }
    }
}

void xunji_adc_to_digital(void)//每采集4次进行一次处理，包括均值滤波，转数字
{
    /* 将当前 8 路采样值存入样本缓存 */
    for (uint8_t ch = 0; ch < 8; ch++) {
        adc_samples[ch][adc_sample_cnt] = adc_get_value[ch];
    }

    /* 当收集到 4 次样本后，计算均值并应用阈值判断 */
    adc_sample_cnt++;
    if (adc_sample_cnt >= 4) {
        uint8_t new_digital = ADC_Value_digital; /* 基于前一次状态开始（用于滞后） */

        for (uint8_t ch = 0; ch < 8; ch++) {
            uint32_t sum = 0;
            for (uint8_t k = 0; k < 4; k++) {
                sum += adc_samples[ch][k];
            }
            uint16_t mean = (uint16_t)(sum / 4u);

            /* 保存该通道的均值供校准使用 */
            adc_last_mean[ch] = mean;

            if (mean > adc_upper_thresh[ch]) {
                new_digital |= (1u << ch);
            } else if (mean < adc_lower_thresh[ch]) {
                new_digital &= (uint8_t)~(1u << ch);
            } else {
                /* 中间区间：保持之前的值（滞后） */
            }
        }

        ADC_Value_digital = new_digital;

        /* 重置计数以开始下一轮累积 */
        adc_sample_cnt = 0;
    }
}

void LED_show(void)
{
    /* 若处于校准阶段，则显示固定指示而非 ADC 位
     * calib_stage: 1 = 等待黑色（LED1 点亮），
     *              2 = 等待白色（LED3+LED4 点亮），
     *              3 = 校准完成（所有 LED 点亮）
     */
    if (calib_stage == 1) {
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN | GPIO_LEDS_USER_LED_3_PIN | GPIO_LEDS_USER_LED_4_PIN);
        return;
    } else if (calib_stage == 2) {
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_3_PIN | GPIO_LEDS_USER_LED_4_PIN);
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN | GPIO_LEDS_USER_LED_2_PIN);
        return;
    } else if (calib_stage == 3) {
        /* 校准完成：点亮所有 LED */
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN | GPIO_LEDS_USER_LED_2_PIN | GPIO_LEDS_USER_LED_3_PIN | GPIO_LEDS_USER_LED_4_PIN);
        return;
    }
    /* 交替显示：若 g_adc_round 为偶数 -> 显示低四位；为奇数 -> 显示高四位（右移后映射到 0..3 位） */
    uint8_t bits;
    if ((g_adc_round & 1) == 0) {
        bits = ADC_Value_digital & 0x0F; /* 低 4 位 */
    } else {
        bits = (ADC_Value_digital >> 4) & 0x0F; /* 高 4 位右移到 0..3 */
    }

    /* 将位 0..3 映射到 LED 1..4 */
    if (bits & (1u << 0))
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);
    else
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_1_PIN);

    if (bits & (1u << 1))
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN);
    else
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_2_PIN);

    if (bits & (1u << 2))
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_3_PIN);
    else
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_3_PIN);

    if (bits & (1u << 3))
        DL_GPIO_clearPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_4_PIN);
    else
        DL_GPIO_setPins(GPIO_LEDS_PORT, GPIO_LEDS_USER_LED_4_PIN);
}

void SetupOddChannels(void) 
{
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_0,
        DL_ADC12_INPUT_CHAN_5, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_1,
        DL_ADC12_INPUT_CHAN_0, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_2,
        DL_ADC12_INPUT_CHAN_1, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_3,
        DL_ADC12_INPUT_CHAN_2, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
}

void SetupEvenChannels(void) 
{
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_0,
        DL_ADC12_INPUT_CHAN_3, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_1,
        DL_ADC12_INPUT_CHAN_4, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_2,
        DL_ADC12_INPUT_CHAN_9, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
    DL_ADC12_configConversionMem(ADC12_0_INST, ADC12_0_ADCMEM_3,
        DL_ADC12_INPUT_CHAN_8, DL_ADC12_REFERENCE_VOLTAGE_VDDA, DL_ADC12_SAMPLE_TIMER_SOURCE_SCOMP0, DL_ADC12_AVERAGING_MODE_DISABLED,
        DL_ADC12_BURN_OUT_SOURCE_DISABLED, DL_ADC12_TRIGGER_MODE_TRIGGER_NEXT, DL_ADC12_WINDOWS_COMP_MODE_DISABLED);
}
