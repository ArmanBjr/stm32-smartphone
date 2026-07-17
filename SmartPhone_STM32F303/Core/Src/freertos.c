/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "serial.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* Diagnostic hooks for the Phase 3 boot-loop investigation (see
 * FreeRTOSConfig.h's "USER CODE BEGIN Defines" comment for the full
 * rationale). LOG() uses the existing ring-buffer+IT UART driver
 * (serial.c), same one used for every other boot/debug message since
 * Phase 0, so this needs no new peripheral setup. If the board really is
 * stack-overflowing or heap-exhausting, this line is expected to appear on
 * the UART terminal (Termite) right before the LCD freezes/resets -- if it
 * never appears, that rules out both hypotheses and the search moves
 * elsewhere (e.g. capture RCC->CSR's *RSTF flags in the debugger to confirm
 * IWDG is even the reset source at all). */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  (void)xTask;
  LOG("[STACK OVERFLOW] task=%s", pcTaskName);
  taskDISABLE_INTERRUPTS();
  for (;;) {
    /* deliberately not feeding IWDG -- let it reset so the next boot is
     * observable too, but hold here long enough for the UART TX above to
     * actually drain before the reset happens. */
  }
}

void vApplicationMallocFailedHook(void)
{
  LOG("[MALLOC FAILED]");
  taskDISABLE_INTERRUPTS();
  for (;;) {
  }
}

/* USER CODE END Application */

