/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f3xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f3xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* Diagnostic instrumentation added while chasing the Phase 3 boot-loop
 * report ("SmartPhone" logo repeats, board keeps resetting). serial.c's
 * LOG() is interrupt-driven (HAL_UART_Transmit_IT, drained by
 * USART1_IRQHandler) so it CANNOT be used here: HardFault_Handler runs at
 * the highest exception priority, above every maskable interrupt, so the
 * UART TX-complete interrupt that would normally drain the ring buffer can
 * never fire while we're stuck below. This prints directly with polled
 * HAL_UART_Transmit() (busy-waits on hardware flags, no interrupt needed)
 * so the fault reason survives even from here. Captures the classic
 * ARM Cortex-M stacked-exception-frame registers (r0-r3, r12, lr, pc, xpsr)
 * plus the fault status registers (CFSR/HFSR/MMFAR/BFAR) -- the same class
 * of evidence the Phase 0 TIM1/TIM16 bug was diagnosed from via debugger,
 * just captured automatically over UART instead of requiring a live
 * debugger session. */
extern UART_HandleTypeDef huart1;

void HardFault_Handler_C(uint32_t *stacked)
{
  uint32_t r0  = stacked[0];
  uint32_t r1  = stacked[1];
  uint32_t r2  = stacked[2];
  uint32_t r3  = stacked[3];
  uint32_t r12 = stacked[4];
  uint32_t lr  = stacked[5];
  uint32_t pc  = stacked[6];
  uint32_t psr = stacked[7];
  uint32_t cfsr  = SCB->CFSR;
  uint32_t hfsr  = SCB->HFSR;
  uint32_t mmfar = SCB->MMFAR;
  uint32_t bfar  = SCB->BFAR;
  char buf[240];
  int n;

  n = snprintf(buf, sizeof(buf),
               "\r\n[HARDFAULT] PC=0x%08lX LR=0x%08lX PSR=0x%08lX\r\n"
               "[HARDFAULT] R0=0x%08lX R1=0x%08lX R2=0x%08lX R3=0x%08lX R12=0x%08lX\r\n"
               "[HARDFAULT] CFSR=0x%08lX HFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX\r\n",
               (unsigned long)pc, (unsigned long)lr, (unsigned long)psr,
               (unsigned long)r0, (unsigned long)r1, (unsigned long)r2,
               (unsigned long)r3, (unsigned long)r12,
               (unsigned long)cfsr, (unsigned long)hfsr,
               (unsigned long)mmfar, (unsigned long)bfar);
  if (n > 0) {
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 500u);
  }

  for (;;) {
    /* deliberately not feeding IWDG -- let it reset so the fault dump above
     * (already fully transmitted via the blocking call, unlike LOG()) is
     * visible on the terminal, then the board recovers on its own. */
  }
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern TIM_HandleTypeDef htim2;
extern TIM_HandleTypeDef htim3;
extern TIM_HandleTypeDef htim6;
extern TIM_HandleTypeDef htim7;
extern TIM_HandleTypeDef htim16;
extern UART_HandleTypeDef huart1;
extern TIM_HandleTypeDef htim1;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M4 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
  while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
/* USER CODE BEGIN HardFault_IRQn 0 */
/* `naked` (also declared so in stm32f3xx_it.h): a naked function has NO
 * compiler-generated prologue/epilogue, which is required here so the
 * stack pointer is still exactly where the CPU left it when we read it
 * below -- any prologue would push registers first and throw off the
 * "which stack has the fault frame" read. Per the Cortex-M EXC_RETURN
 * convention, LR's bit 2 tells us whether MSP or PSP was active at fault
 * time; we grab that stack pointer into r0 (the C calling-convention's
 * first-argument register) and tail-jump into HardFault_Handler_C() above,
 * which does the actual UART reporting and never returns. Because this is
 * naked, it must contain ONLY the asm block -- no C statements (not even
 * an unreachable while(1)) are valid in a naked function per GCC's rules. */
__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile
  (
    " tst lr, #4                   \n"
    " ite eq                       \n"
    " mrseq r0, msp                \n"
    " mrsne r0, psp                \n"
    " ldr r1, =HardFault_Handler_C \n"
    " bx r1                        \n"
  );
}
/* USER CODE END HardFault_IRQn 0 */

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F3xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f3xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles EXTI line3 interrupt.
  */
void EXTI3_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI3_IRQn 0 */

  /* USER CODE END EXTI3_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_3);
  /* USER CODE BEGIN EXTI3_IRQn 1 */

  /* USER CODE END EXTI3_IRQn 1 */
}

/**
  * @brief This function handles ADC1 and ADC2 interrupts.
  */
void ADC1_2_IRQHandler(void)
{
  /* USER CODE BEGIN ADC1_2_IRQn 0 */

  /* USER CODE END ADC1_2_IRQn 0 */
  HAL_ADC_IRQHandler(&hadc1);
  HAL_ADC_IRQHandler(&hadc2);
  /* USER CODE BEGIN ADC1_2_IRQn 1 */

  /* USER CODE END ADC1_2_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[9:5] interrupts.
  */
void EXTI9_5_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI9_5_IRQn 0 */

  /* USER CODE END EXTI9_5_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_9);
  /* USER CODE BEGIN EXTI9_5_IRQn 1 */

  /* USER CODE END EXTI9_5_IRQn 1 */
}

/**
  * @brief This function handles TIM1 update and TIM16 interrupts.
  */
void TIM1_UP_TIM16_IRQHandler(void)
{
  /* USER CODE BEGIN TIM1_UP_TIM16_IRQn 0 */
  /* HAL_Init() -> HAL_InitTick() starts TIM1 and enables this shared vector
   * BEFORE MX_TIM16_Init() ever runs (that happens later in main()). The
   * very first TIM1 tick interrupt therefore fires while htim16.Instance is
   * still NULL (zero-initialized .bss), and unconditionally calling
   * HAL_TIM_IRQHandler(&htim16) below dereferences that NULL Instance --
   * confirmed via debugger as a HardFault inside HAL_Init(), before any
   * application code runs. Guard until TIM16 is actually initialized. */
  if (htim16.Instance == NULL) {
    HAL_TIM_IRQHandler(&htim1);
    return;
  }
  /* USER CODE END TIM1_UP_TIM16_IRQn 0 */
  HAL_TIM_IRQHandler(&htim1);
  HAL_TIM_IRQHandler(&htim16);
  /* USER CODE BEGIN TIM1_UP_TIM16_IRQn 1 */

  /* USER CODE END TIM1_UP_TIM16_IRQn 1 */
}

/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */

  /* USER CODE END TIM2_IRQn 0 */
  HAL_TIM_IRQHandler(&htim2);
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles TIM3 global interrupt.
  */
void TIM3_IRQHandler(void)
{
  /* USER CODE BEGIN TIM3_IRQn 0 */

  /* USER CODE END TIM3_IRQn 0 */
  HAL_TIM_IRQHandler(&htim3);
  /* USER CODE BEGIN TIM3_IRQn 1 */

  /* USER CODE END TIM3_IRQn 1 */
}

/**
  * @brief This function handles USART1 global interrupt / USART1 wake-up interrupt through EXTI line 25.
  */
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */

  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
  /* USER CODE BEGIN USART1_IRQn 1 */

  /* USER CODE END USART1_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[15:10] interrupts.
  */
void EXTI15_10_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI15_10_IRQn 0 */

  /* USER CODE END EXTI15_10_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_10);
  /* USER CODE BEGIN EXTI15_10_IRQn 1 */

  /* USER CODE END EXTI15_10_IRQn 1 */
}

/**
  * @brief This function handles Timer 6 interrupt and DAC underrun interrupts.
  */
void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */

  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */

  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/**
  * @brief This function handles TIM7 global interrupt.
  */
void TIM7_IRQHandler(void)
{
  /* USER CODE BEGIN TIM7_IRQn 0 */

  /* USER CODE END TIM7_IRQn 0 */
  HAL_TIM_IRQHandler(&htim7);
  /* USER CODE BEGIN TIM7_IRQn 1 */

  /* USER CODE END TIM7_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
