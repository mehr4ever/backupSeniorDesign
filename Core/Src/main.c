// includes
#include "main.h"
#include "fonts.h"
#include "ili9341.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>



#define NUM_MANUAL_SPEEDS 3

#define IR_PWM_FREQ_HZ        10000
#define IR_PWM_DUTY_PERCENT   5.0f

#define VIB_SAMPLE_COUNT     10
#define VIB_SAMPLE_DELAY_MS  5
#define VIB_THRESH_OFF  50
#define VIB_THRESH_LOW  500
#define VIB_THRESH_MODERATE 1000

#define IR_HIGH_THRESH 120 // change
#define IR_MODERATE_THRESH 85 // change
#define IR_LOW_THRESH 50 // change


#define BTN_DEBOUNCE_MS  200

#define CAN_TX_STD_ID          0x123U
#define CAN_TX_INTERVAL_ITERS  100U

#define TFT_COL_LEFT    5
#define TFT_ROW_TITLE   10
#define TFT_ROW_MODE    60
#define TFT_ROW_SPEED   100
#define TFT_ROW_RAIN    140
#define TFT_ROW_VIB     170
#define TFT_ROW_IR0     200


CAN_HandleTypeDef hcan1;
ADC_HandleTypeDef hadc1; // IR adc
SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart2;
TIM_HandleTypeDef htim2;  // PWM timer
TIM_HandleTypeDef htim3;  // PWM PB2




volatile int     g_wiper_speed   = 0;
char             g_intensity[16] = "Off";
volatile uint8_t g_auto_mode     = 0;
volatile uint32_t g_iteration    = 0;
volatile uint8_t  g_btn1_pressed = 0;   
volatile uint8_t  g_btn2_pressed = 0;   
volatile uint32_t g_btn1_count   = 0;   
volatile uint32_t g_btn2_count   = 0;

static int     prev_rain     = -1;
static uint32_t prev_vib     = UINT32_MAX;
static uint16_t prev_ir0     = UINT16_MAX;
static int     prev_speed    = -1;
volatile uint8_t prev_mode     = 255;  // invalid initial value to force TFT update on first run

#define VIB_CHANGE_THRESH 100  // only update TFT if vib changes by this much to reduce flicker
#define IR_CHANGE_THRESH 1   // only update TFT if IR changes by this much to reduce flicker

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_SPI1_Init(void);
static void MX_CAN1_Init(void);
static void PWM_Init(uint32_t freq_hz);
static void PWM_SetDuty(uint8_t channel, float duty_pct);
static void     TFT_DrawInitialScreen(void);
static void     TFT_UpdateMode(void);
static void     TFT_UpdateSpeed(void);
static void     TFT_UpdateSensorData(uint32_t avg_vib, uint16_t ir_ch10, int      rain_detected);
 
static uint16_t ADC_ReadChannel(uint32_t channel);
static uint16_t ADC_ReadPC0(void);
static uint16_t ADC_ReadPC1(void);
static float    ADC_ToVoltage(uint16_t adc_val);
 
static void     AutoMode_Process();
static void     CAN_TrySend(CAN_TxHeaderTypeDef *hdr, uint8_t *data);
 


int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_USART2_UART_Init();
  MX_SPI1_Init();
  MX_CAN1_Init();

  ILI9341_Unselect();
  ILI9341_Init();
  TFT_DrawInitialScreen();

  PWM_Init(IR_PWM_FREQ_HZ);
  PWM_SetDuty(2, IR_PWM_DUTY_PERCENT); 
  PWM_SetDuty(4, IR_PWM_DUTY_PERCENT);  
 
  CAN_FilterTypeDef filter = {0};
  filter.FilterBank           = 0;
  filter.FilterMode           = CAN_FILTERMODE_IDMASK;
  filter.FilterScale          = CAN_FILTERSCALE_32BIT;
  filter.FilterIdHigh         = 0x0000;
  filter.FilterIdLow          = 0x0000;
  filter.FilterMaskIdHigh     = 0x0000;   /* don't-care → accept all */
  filter.FilterMaskIdLow      = 0x0000;
  filter.FilterFIFOAssignment = CAN_RX_FIFO0;
  filter.FilterActivation     = ENABLE;
  filter.SlaveStartFilterBank = 14;
  HAL_CAN_ConfigFilter(&hcan1, &filter);
  
  HAL_CAN_Start(&hcan1);
  
  CAN_TxHeaderTypeDef TxHeader = {0};
  TxHeader.StdId              = CAN_TX_STD_ID;
  TxHeader.IDE                = CAN_ID_STD;
  TxHeader.RTR                = CAN_RTR_DATA;
  TxHeader.DLC                = 8;
  TxHeader.TransmitGlobalTime = DISABLE;
  
  uint8_t TxData[8] = {0};
  
  while (1) {
    g_iteration++;
 
    /* ── Button 1 (PA8): step manual speed ─────────────────────────── */
    if (g_btn1_pressed) {
      g_btn1_pressed = 0;
 
      if (!g_auto_mode)   /* only allow manual speed change in MANUAL mode */
      {
        g_wiper_speed = (g_wiper_speed + 1) % (NUM_MANUAL_SPEEDS + 1);

        // Keep g_intensity in sync with manual speed
        switch (g_wiper_speed) {
            case 0:  strcpy(g_intensity, "Off");      break;
            case 1:  strcpy(g_intensity, "Low");      break;
            case 2:  strcpy(g_intensity, "Moderate"); break;
            case 3:  strcpy(g_intensity, "High");     break;
        }

        if (g_wiper_speed != prev_speed || (prev_mode != g_auto_mode)) {
          prev_speed = g_wiper_speed;
          TFT_UpdateSpeed();
        }
 
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "[BTN1] Manual speed → %d\r\n", g_wiper_speed);
        HAL_UART_Transmit(&huart2, (uint8_t*)dbg, strlen(dbg), 100);
      }
    }

 
    /* ── Button 2 (PB10): toggle AUTO / MANUAL ──────────────────────── */
    if (g_btn2_pressed) {
            g_btn2_pressed = 0;
            g_auto_mode    = !g_auto_mode;
 
            if (!g_auto_mode)
            {
                /* Returning to manual — reset speed to OFF */
                g_wiper_speed = 0;
                /* Clear automatic sensor data */
                TFT_ClearRow((uint16_t)TFT_ROW_RAIN);
                TFT_ClearRow((uint16_t)TFT_ROW_VIB);
                TFT_ClearRow((uint16_t)TFT_ROW_IR0);
                strcpy(g_intensity, "Off");   // add thisi
                if (g_wiper_speed != prev_speed) {
                  prev_speed = g_wiper_speed;
                  TFT_UpdateSpeed();
                }
            } else {
              prev_rain = -1;
              prev_vib  = UINT32_MAX;
              prev_ir0  = UINT16_MAX;
            }
            if (prev_mode != g_auto_mode) {
              prev_mode = g_auto_mode;  
              TFT_UpdateMode();
                
            }
 
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "[BTN2] Mode → %s\r\n",
                     g_auto_mode ? "AUTO" : "MANUAL");
            HAL_UART_Transmit(&huart2, (uint8_t*)dbg, strlen(dbg), 100);
        }
 
        /* ── Automatic mode: sensor processing ──────────────────────────── */
        if (g_auto_mode)
        {
            AutoMode_Process();
        }
 
        // In main(), replace the iteration-based CAN block with:
        static uint32_t last_can_tx = 0;
        uint32_t now = HAL_GetTick();
        if (now - last_can_tx >= 500)   /* transmit every 500 ms */
        {
            last_can_tx = now;
            memset(TxData, 0, sizeof(TxData));
            TxData[0] = (uint8_t)g_wiper_speed;
            TxData[1] = g_auto_mode;
            strncpy((char*)&TxData[2], g_intensity, 5);
            CAN_TrySend(&TxHeader, TxData);
        }
    }
}


static void AutoMode_Process()
{
  GPIO_PinState rain_pin1 = HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5);
  GPIO_PinState rain_pin2 = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9);
  int rain_detected = (rain_pin1 == GPIO_PIN_RESET) || (rain_pin2 == GPIO_PIN_RESET);
 
  uint32_t total_vib = 0;
  uint32_t total_ir0 = 0;
  
  uint32_t avg_vib = 0;
  uint32_t avg_IR  = 0;
  uint32_t IR_speed = 0;
 
  rain_detected=1;
  if (rain_detected)
  {
    for (int i = 0; i < VIB_SAMPLE_COUNT; i++)
    {
      // read Vibration from PA0 (Channel 0)
      // TODO: ADD OTHER VIBRATION READING
      total_vib += ADC_ReadChannel(ADC_CHANNEL_0);
      
      // read IR readings
      total_ir0 += ADC_ReadPC0();
      HAL_Delay(VIB_SAMPLE_DELAY_MS);
    }

    // calculate averages
    avg_vib = total_vib / VIB_SAMPLE_COUNT;
    avg_IR = total_ir0 / VIB_SAMPLE_COUNT;

    if (avg_IR < IR_LOW_THRESH)           IR_speed = 3; // high
    else if (avg_IR < IR_MODERATE_THRESH) IR_speed = 2; // moderate
    else if (avg_IR < IR_HIGH_THRESH)     IR_speed = 1; // low
    else                                  IR_speed = 0; // off

    if (IR_speed == 1 && avg_vib >= VIB_THRESH_MODERATE)
    {
      g_wiper_speed = 2;
      strcpy(g_intensity, "Moderate");
    }
    else if (IR_speed == 3 && avg_vib <= VIB_THRESH_LOW)
    {
      g_wiper_speed = 2;
      strcpy(g_intensity, "Moderate");
    }
    else if (avg_vib <= VIB_THRESH_OFF)
    {
      g_wiper_speed = 0;
      strcpy(g_intensity, "Off");
    }
    else
    {
      g_wiper_speed = IR_speed;
      if (IR_speed == 1)      strcpy(g_intensity, "Low");
      else if (IR_speed == 2) strcpy(g_intensity, "Moderate");
      else if (IR_speed == 3) strcpy(g_intensity, "High");
      else                    strcpy(g_intensity, "Off");
    }
 
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);   // rain LED on
  }
  else
  {
    strcpy(g_intensity, "Off");
    g_wiper_speed = 0;
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET); // rain LED off
  }
 
  if (g_wiper_speed != prev_speed) {
    prev_speed = g_wiper_speed;
    TFT_UpdateSpeed();
  }
 
  if (rain_detected != prev_rain || 
      (abs((int32_t)avg_vib - (int32_t)prev_vib) > VIB_CHANGE_THRESH) ||
      (abs((int32_t)avg_IR - (int32_t)prev_ir0) > IR_CHANGE_THRESH))
  {
    prev_rain = rain_detected;
    prev_vib  = avg_vib;
    prev_ir0  = (uint16_t)avg_IR;
    TFT_UpdateSensorData(avg_vib, (uint16_t)avg_IR, rain_detected);
  }

  static uint32_t auto_iter = 0;
  if (++auto_iter % 100 == 0)
  {
    char msg[180];
    snprintf(msg, sizeof(msg), "[AUTO] Rain:%d | Vib:%lu | IR:%lu | Spd:%d (%s)\r\n",
                 rain_detected, avg_vib, avg_IR, g_wiper_speed, g_intensity);
    HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 200);
  }
}

static void CAN_TrySend(CAN_TxHeaderTypeDef *hdr, uint8_t *data)
{
  uint32_t mailbox;
  if (HAL_CAN_AddTxMessage(&hcan1, hdr, data, &mailbox) != HAL_OK)
  {
    const char *err = "[CAN] TX mailbox full or error\r\n";
    HAL_UART_Transmit(&huart2, (uint8_t*)err, strlen(err), 100);
  }
  else
  {
    char dbg[80];
    snprintf(dbg, sizeof(dbg), "[CAN] TX → speed=%d mode=%s intensity=%s\r\n",g_wiper_speed, g_auto_mode ? "AUTO" : "MANUAL", g_intensity);
    HAL_UART_Transmit(&huart2, (uint8_t*)dbg, strlen(dbg), 100);
  }
}
 


void TFT_ClearRow(uint16_t y)
{
  ILI9341_FillRectangle(TFT_COL_LEFT, y, 230, 26, ILI9341_BLACK);
}

static void TFT_DrawInitialScreen(void)
{
  ILI9341_FillScreen(ILI9341_BLACK);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_TITLE, "WIPER CTRL", Font_16x26, ILI9341_CYAN, ILI9341_BLACK);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_MODE, "MODE : MANUAL", Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_SPEED, "SPEED: OFF", Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_RAIN, "RAIN : ---", Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_VIB, "VIB  : ---", Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_IR0, "IR   : ---", Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
}
 
static void TFT_UpdateMode(void)
{
  TFT_ClearRow(TFT_ROW_MODE);
  if (g_auto_mode)
    ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_MODE, "MODE : AUTO  ", Font_16x26, ILI9341_GREEN, ILI9341_BLACK);
  else
    ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_MODE, "MODE : MANUAL", Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
}
 

static void TFT_UpdateSpeed(void)
{
  TFT_ClearRow(TFT_ROW_SPEED);
  char buf[20];
  if (g_wiper_speed == 0)
    snprintf(buf, sizeof(buf), "SPEED: OFF   ");
  else
    snprintf(buf, sizeof(buf), "SPEED: %d     ", g_wiper_speed);
  ILI9341_WriteString(TFT_COL_LEFT, TFT_ROW_SPEED, buf, Font_16x26, ILI9341_YELLOW, ILI9341_BLACK);
}
 
static void TFT_UpdateSensorData(uint32_t avg_vib, uint16_t ir_ch10, int rain_detected)
{
  char rain_buf[10];
  char vib_buf[10];
  char ir0_buf[11];
 
  snprintf(rain_buf, sizeof(rain_buf), " %s", rain_detected ? "YES" : "NO ");
  snprintf(vib_buf, sizeof(vib_buf), "%4lu", avg_vib);
  snprintf(ir0_buf, sizeof(ir0_buf), " %.1fmV", ADC_ToVoltage(ir_ch10));

  // print all at once
  ILI9341_WriteString(100, TFT_ROW_RAIN, rain_buf, Font_16x26, rain_detected ? ILI9341_RED : ILI9341_WHITE, ILI9341_BLACK);
  ILI9341_WriteString(100, TFT_ROW_VIB, vib_buf, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
  ILI9341_WriteString(100, TFT_ROW_IR0, ir0_buf, Font_16x26, ILI9341_WHITE, ILI9341_BLACK);
}
 
static uint16_t ADC_ReadChannel(uint32_t channel)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  sConfig.Channel      = channel;
  sConfig.Rank         = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  HAL_ADC_ConfigChannel(&hadc1, &sConfig);
 
  HAL_ADC_Start(&hadc1);
  HAL_ADC_PollForConversion(&hadc1, 10);
  uint16_t val = HAL_ADC_GetValue(&hadc1);
  HAL_ADC_Stop(&hadc1);
  return val;
}
 
static uint16_t ADC_ReadPC0(void) { return ADC_ReadChannel(ADC_CHANNEL_10); }
static uint16_t ADC_ReadPC1(void) { return ADC_ReadChannel(ADC_CHANNEL_11); }
 
static float ADC_ToVoltage(uint16_t adc_val)
{
    return ((adc_val / 4095.0f) * 3.3f) * 1000.0;
}
 

static void PWM_Init(uint32_t freq_hz)
{
  __HAL_RCC_TIM2_CLK_ENABLE();
  __HAL_RCC_TIM3_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
 
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin       = GPIO_PIN_2; 
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_NOPULL;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF1_TIM2;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
 
  // PB1 = TIM3_CH4
  GPIO_InitStruct.Pin       = GPIO_PIN_1;
  GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull      = GPIO_NOPULL;
  GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);


  uint32_t pclk1    = HAL_RCC_GetPCLK1Freq();
  uint32_t prescaler = (pclk1 / 1000000U) - 1U;   /* 1 MHz timer clock */
  uint32_t period    = (1000000U / freq_hz) - 1U;

  uint32_t pclk1b   = HAL_RCC_GetPCLK1Freq();
  uint32_t prescaler3 = (pclk1b / 1000000U) - 1U;
  uint32_t period3    = (1000000U / freq_hz) - 1U;
 
  htim2.Instance               = TIM2;
  htim2.Init.Prescaler         = prescaler;
  htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim2.Init.Period            = period;
  htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  HAL_TIM_PWM_Init(&htim2);

  htim3.Instance           = TIM3;
  htim3.Init.Prescaler     = prescaler3;
  htim3.Init.CounterMode   = TIM_COUNTERMODE_UP;
  htim3.Init.Period        = period3;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  HAL_TIM_PWM_Init(&htim3);
 
  TIM_OC_InitTypeDef sConfigOC = {0};
  sConfigOC.OCMode     = TIM_OCMODE_PWM1;
  sConfigOC.Pulse      = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

  TIM_OC_InitTypeDef sConfigOC3 = {0};
  sConfigOC3.OCMode     = TIM_OCMODE_PWM1;
  sConfigOC3.Pulse      = 0;
  sConfigOC3.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC3.OCFastMode = TIM_OCFAST_DISABLE;
 
  HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_3);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);


  HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC3, TIM_CHANNEL_4);
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);
}
 
static void PWM_SetDuty(uint8_t channel, float duty_pct)
{
  if (duty_pct < 0.0f)   duty_pct = 0.0f;
  if (duty_pct > 100.0f) duty_pct = 100.0f;
 
  uint32_t period = __HAL_TIM_GET_AUTORELOAD(&htim2);
  uint32_t pulse  = (uint32_t)((duty_pct / 100.0f) * (float)(period + 1U));

  uint32_t period3 = __HAL_TIM_GET_AUTORELOAD(&htim3);
  uint32_t pulse3  = (uint32_t)((duty_pct / 100.0f) * (float)(period3 + 1U));
 
  if (channel == 2)
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, pulse);
  else if (channel == 4)
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, pulse3);
}
 
void PWM_SetFrequency(uint32_t freq_hz)
{
  uint32_t pclk1    = HAL_RCC_GetPCLK1Freq();
  uint32_t prescaler = (pclk1 / 1000000U) - 1U;
  uint32_t period    = (1000000U / freq_hz) - 1U;
 
  __HAL_TIM_DISABLE(&htim2);
  __HAL_TIM_SET_PRESCALER(&htim2, prescaler);
  __HAL_TIM_SET_AUTORELOAD(&htim2, period);
  __HAL_TIM_SET_COUNTER(&htim2, 0);
  __HAL_TIM_ENABLE(&htim2);

  uint32_t pclk1b   = HAL_RCC_GetPCLK1Freq();
  uint32_t prescaler3 = (pclk1b / 1000000U) - 1U;
  uint32_t period3    = (1000000U / freq_hz) - 1U;

  __HAL_TIM_DISABLE(&htim3);
  __HAL_TIM_SET_PRESCALER(&htim3, prescaler3);
  __HAL_TIM_SET_AUTORELOAD(&htim3, period3);
  __HAL_TIM_SET_COUNTER(&htim3, 0);
  __HAL_TIM_ENABLE(&htim3);
}
 
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_8)         /* PA8 — speed button */
  {
    static uint32_t prev_tick = 0;
    uint32_t now = HAL_GetTick();
    if (now - prev_tick > BTN_DEBOUNCE_MS)
    {
      g_btn1_count++;
      g_btn1_pressed = 1;
      prev_tick = now;
    }
  }
  else if (GPIO_Pin == GPIO_PIN_10)   /* PB10 — mode button */
  {
    static uint32_t prev_tick = 0;
    uint32_t now = HAL_GetTick();
    if (now - prev_tick > BTN_DEBOUNCE_MS)
    {
      g_btn2_count++;
      g_btn2_pressed = 1;
      prev_tick = now;
    }
  }
}
 

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
 
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);
 
  /* HSI only, no PLL — system runs at 16 MHz */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();
 
  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                     | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK) Error_Handler();
}
 
void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
  if (hcan->Instance == CAN1)
  {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_RCC_CAN1_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
 
    GPIO_InitStruct.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
  }
}
 
static void MX_CAN1_Init(void)
{
  hcan1.Instance                  = CAN1;
  hcan1.Init.Prescaler            = 9;
  hcan1.Init.Mode                 = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth        = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1             = CAN_BS1_12TQ;
  hcan1.Init.TimeSeg2             = CAN_BS2_3TQ;
  hcan1.Init.TimeTriggeredMode    = DISABLE;
  hcan1.Init.AutoBusOff           = DISABLE;
  hcan1.Init.AutoWakeUp           = DISABLE;
  hcan1.Init.AutoRetransmission   = ENABLE;
  hcan1.Init.ReceiveFifoLocked    = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK) Error_Handler();
}
 
static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
 
  hadc1.Instance                   = ADC1;
  hadc1.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution            = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode          = DISABLE;   /* single-channel mode */
  hadc1.Init.ContinuousConvMode    = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion       = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();
 
  /* Default channel; will be overridden per-call by ADC_ReadChannel() */
  sConfig.Channel      = ADC_CHANNEL_0;
  sConfig.Rank         = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) Error_Handler();
}
 
static void MX_SPI1_Init(void)
{
  hspi1.Instance               = SPI1;
  hspi1.Init.Mode              = SPI_MODE_MASTER;
  hspi1.Init.Direction         = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize          = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity       = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase          = SPI_PHASE_1EDGE;
  hspi1.Init.NSS               = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit          = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode            = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial     = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK) Error_Handler();
}
 
static void MX_USART2_UART_Init(void)
{
  huart2.Instance          = USART2;
  huart2.Init.BaudRate     = 115200;
  huart2.Init.WordLength   = UART_WORDLENGTH_8B;
  huart2.Init.StopBits     = UART_STOPBITS_1;
  huart2.Init.Parity       = UART_PARITY_NONE;
  huart2.Init.Mode         = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}
 
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
 
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
 
  /* ── Output pins ─────────────────────────────────────────────────────── */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7,  GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_10, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6,  GPIO_PIN_RESET);
 
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
 
  GPIO_InitStruct.Pin = GPIO_PIN_7;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);        /* PC7  — TFT CS or similar */
 
  GPIO_InitStruct.Pin = GPIO_PIN_10;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);        /* PA10 — rain indicator LED */
 
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);        /* PA5  — rain LED (LD2) */
 
  GPIO_InitStruct.Pin = GPIO_PIN_6;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);        /* PB6  — spare output */
 
  /* ── Input pins ──────────────────────────────────────────────────────── */
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
 
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);        /* PA9  — spare input */
 
  GPIO_InitStruct.Pin = GPIO_PIN_5;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);        /* PB5  — rain sensor DO */
 
  /* ── EXTI: PA8 (button 1) and PB10 (button 2) ───────────────────────── */
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
 
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);        /* PA8  — speed button */
 
  GPIO_InitStruct.Pin = GPIO_PIN_10;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);        /* PB10 — mode button  */
 
  /* ── Analog: PC0, PC1 — IR receiver ADC inputs ───────────────────────── */
  GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Pin  = GPIO_PIN_0 | GPIO_PIN_1;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);
 
  /* ── NVIC for EXTI ───────────────────────────────────────────────────── */
  HAL_NVIC_SetPriority(EXTI9_5_IRQn,   1, 0);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
 
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}
 
void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}
 
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file; (void)line;
}
#endif