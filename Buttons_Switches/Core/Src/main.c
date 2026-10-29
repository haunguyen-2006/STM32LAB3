#include "main.h"

/* =========================================================
 * STM32F103C8/compatible HAL example for Lab 3
 * TIM2 update interrupt: every 10 ms
 *
 * Assumptions:
 * - Traffic LEDs are active LOW (GPIO LOW = LED ON).
 * - Seven-segment displays are common-anode, active LOW segments.
 * - PNP digit enables are active LOW.
 * - Buttons are active LOW, wired to GND, with pull-ups enabled.
 * - EN0/EN1: vertical tens/ones.
 * - EN2/EN3: horizontal tens/ones.
 * ========================================================= */

TIM_HandleTypeDef htim2;

#define TIMER_CYCLE_MS        10U
#define TICKS_PER_SECOND      (1000U / TIMER_CYCLE_MS)

/* One displayed countdown unit every 0.5 real seconds. */
#define TRAFFIC_COUNTDOWN_TICKS_PER_SECOND (TICKS_PER_SECOND / 2U)

#define DEBOUNCE_SAMPLES      2U
#define DISPLAY_DIGIT_MS      2U
#define BLINK_HALF_PERIOD_TICKS 25U
#define BUTTON2_LONG_PRESS_TICKS (1500U / TIMER_CYCLE_MS)

/* -------------------- Modes -------------------- */
typedef enum
{
    MODE_NORMAL = 1,
    MODE_SET_RED = 2,
    MODE_SET_YELLOW = 3,
    MODE_SET_GREEN = 4
} OperatingMode;

typedef enum
{
    PHASE_VERTICAL_GREEN = 0,
    PHASE_VERTICAL_YELLOW,
    PHASE_HORIZONTAL_GREEN,
    PHASE_HORIZONTAL_YELLOW
} TrafficPhase;

/* -------------------- Application state -------------------- */
static volatile uint32_t timer10msTicks = 0;

static volatile uint8_t buttonEvents[3] = {0, 0, 0};
static volatile uint8_t button2DecreaseEvent = 0U;

/* Button 2:
 * Short press = increase time.
 * Hold for 1 second = decrease time once.
 */
static uint16_t button2HoldTicks = 0U;
static uint8_t button2IsPressed = 0U;
static uint8_t button2LongPressHandled = 0U;

/* Default timing:
 * Red = Green + Yellow = 10 seconds.
 */
static uint8_t redTimeSeconds = 10;
static uint8_t yellowTimeSeconds = 3;
static uint8_t greenTimeSeconds = 7;

static OperatingMode mode = MODE_NORMAL;
static TrafficPhase trafficPhase = PHASE_VERTICAL_GREEN;

static uint32_t phaseElapsedTicks = 0;
static uint32_t blinkElapsedTicks = 0;

static uint8_t blinkOn = 1;
static uint8_t selectedTimeSeconds = 10;

static uint8_t verticalDisplayValue = 7;
static uint8_t horizontalDisplayValue = 10;

/* -------------------- Function prototypes -------------------- */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);

static void DebounceButtons10ms(void);
static uint8_t TakeButtonEvent(uint8_t index);
static uint8_t TakeButton2DecreaseEvent(void);
static uint8_t ProcessButtonEvents(void);

static void ProcessOne10msTick(void);
static void AdvanceTrafficPhase(void);
static uint8_t CurrentPhaseDurationSeconds(void);
static uint8_t TicksToSecondsCeil(uint32_t ticks);

static void UpdateDisplayValues(void);
static void UpdateTrafficLEDs(void);

static void ScanSevenSegmentDisplays(void);
static void AllDigitsOff(void);
static void WriteDigitSegments(uint8_t verticalDisplay, uint8_t digit);

static void ConfirmSelectedTime(void);
static void EnterNextMode(void);

static uint8_t GetConfigMin(void);
static uint8_t GetConfigMax(void);
static void IncreaseSelectedTime(void);
static void DecreaseSelectedTime(void);

/* -------------------- Button filters -------------------- */
typedef struct
{
    GPIO_PinState lastRawState;
    GPIO_PinState stableState;
    uint8_t stableSampleCount;
} ButtonFilter;

static ButtonFilter buttonFilters[3];

static const uint16_t buttonPins[3] =
{
    BUT1_Pin,
    BUT2_Pin,
    BUT3_Pin
};

/* -------------------- Seven-segment patterns -------------------- */
/* Common-anode:
 * Bit 0 = segment a, bit 6 = segment g.
 * 0 = segment ON, 1 = segment OFF.
 */
static const uint8_t segmentCode[10] =
{
    0x40, /* 0 */
    0x79, /* 1 */
    0x24, /* 2 */
    0x30, /* 3 */
    0x19, /* 4 */
    0x12, /* 5 */
    0x02, /* 6 */
    0x78, /* 7 */
    0x00, /* 8 */
    0x10  /* 9 */
};

static const uint16_t verticalSegmentPins[7] =
{
    SEG1_0_Pin,
    SEG1_1_Pin,
    SEG1_2_Pin,
    SEG1_3_Pin,
    SEG1_4_Pin,
    SEG1_5_Pin,
    SEG1_6_Pin
};

static const uint16_t horizontalSegmentPins[7] =
{
    SEG2_0_Pin,
    SEG2_1_Pin,
    SEG2_2_Pin,
    SEG2_3_Pin,
    SEG2_4_Pin,
    SEG2_5_Pin,
    SEG2_6_Pin
};

/* =========================================================
 * MAIN
 * ========================================================= */
int main(void)
{
    uint32_t processedTicks = 0;

    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_TIM2_Init();

    /*
     * Initialize button filters from the actual input levels.
     * Prevent a false button press when the program starts.
     */
    for (uint8_t i = 0; i < 3; i++)
    {
        GPIO_PinState initialState =
            HAL_GPIO_ReadPin(BUTTON_GPIO_Port, buttonPins[i]);

        buttonFilters[i].lastRawState = initialState;
        buttonFilters[i].stableState = initialState;
        buttonFilters[i].stableSampleCount = DEBOUNCE_SAMPLES;
        buttonEvents[i] = 0U;
    }

    button2DecreaseEvent = 0U;
    button2HoldTicks = 0U;
    button2IsPressed = 0U;
    button2LongPressHandled = 0U;

    AllDigitsOff();
    UpdateDisplayValues();
    UpdateTrafficLEDs();

    if (HAL_TIM_Base_Start_IT(&htim2) != HAL_OK)
    {
        Error_Handler();
    }

    while (1)
    {
        uint8_t refreshDisplay = 0U;
        uint8_t refreshTraffic = 0U;

        /* Process every elapsed 10 ms timer tick. */
        while (processedTicks != timer10msTicks)
        {
            OperatingMode oldMode = mode;
            TrafficPhase oldPhase = trafficPhase;
            uint8_t oldBlinkOn = blinkOn;

            processedTicks++;
            ProcessOne10msTick();

            refreshDisplay = 1U;

            if ((oldMode != mode) ||
                (oldPhase != trafficPhase) ||
                (oldBlinkOn != blinkOn))
            {
                refreshTraffic = 1U;
            }
        }

        if (ProcessButtonEvents())
        {
            refreshDisplay = 1U;
            refreshTraffic = 1U;
        }

        if (refreshDisplay)
        {
            UpdateDisplayValues();
        }

        if (refreshTraffic)
        {
            UpdateTrafficLEDs();
        }

        /* Keep multiplexed seven-segment displays responsive. */
        ScanSevenSegmentDisplays();

        /* Sleep until the next interrupt. */
        __WFI();
    }
}

/* =========================================================
 * TIM2 CALLBACK
 * ========================================================= */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM2)
    {
        timer10msTicks++;
        DebounceButtons10ms();
    }
}

/* =========================================================
 * BUTTON DEBOUNCING
 * ========================================================= */
static void DebounceButtons10ms(void)
{
    for (uint8_t i = 0; i < 3; i++)
    {
        GPIO_PinState rawState =
            HAL_GPIO_ReadPin(BUTTON_GPIO_Port, buttonPins[i]);

        if (rawState != buttonFilters[i].lastRawState)
        {
            buttonFilters[i].lastRawState = rawState;
            buttonFilters[i].stableSampleCount = 0;
        }
        else if (buttonFilters[i].stableSampleCount < DEBOUNCE_SAMPLES)
        {
            buttonFilters[i].stableSampleCount++;
        }

        if ((buttonFilters[i].stableSampleCount >= DEBOUNCE_SAMPLES) &&
            (buttonFilters[i].stableState != rawState))
        {
            buttonFilters[i].stableState = rawState;

            if (i == 1U)
            {
                /* Button 2 is processed on release to detect
                 * short press versus long press.
                 */
                if (rawState == GPIO_PIN_RESET)
                {
                    button2IsPressed = 1U;
                    button2HoldTicks = 0U;
                    button2LongPressHandled = 0U;
                }
                else
                {
                    /* Released before 1 second = increase. */
                    if (button2IsPressed &&
                        !button2LongPressHandled)
                    {
                        buttonEvents[1] = 1U;
                    }

                    button2IsPressed = 0U;
                    button2HoldTicks = 0U;
                    button2LongPressHandled = 0U;
                }
            }
            else if (rawState == GPIO_PIN_RESET)
            {
                /* Button 1 and Button 3 trigger on press. */
                buttonEvents[i] = 1U;
            }
        }
    }

    /* Hold Button 2 for 1 second to generate one decrease event. */
    if (button2IsPressed &&
        (buttonFilters[1].stableState == GPIO_PIN_RESET) &&
        !button2LongPressHandled)
    {
        if (button2HoldTicks < BUTTON2_LONG_PRESS_TICKS)
        {
            button2HoldTicks++;
        }

        if (button2HoldTicks >= BUTTON2_LONG_PRESS_TICKS)
        {
            button2DecreaseEvent = 1U;
            button2LongPressHandled = 1U;
        }
    }
}

static uint8_t TakeButtonEvent(uint8_t index)
{
    uint8_t event;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    event = buttonEvents[index];
    buttonEvents[index] = 0U;

    if (primask == 0U)
    {
        __enable_irq();
    }

    return event;
}

static uint8_t TakeButton2DecreaseEvent(void)
{
    uint8_t event;
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    event = button2DecreaseEvent;
    button2DecreaseEvent = 0U;

    if (primask == 0U)
    {
        __enable_irq();
    }

    return event;
}

/* =========================================================
 * MODE CONTROL
 * ========================================================= */
static void EnterNextMode(void)
{
    switch (mode)
    {
        case MODE_NORMAL:
            mode = MODE_SET_RED;
            selectedTimeSeconds = redTimeSeconds;
            break;

        case MODE_SET_RED:
            mode = MODE_SET_YELLOW;
            selectedTimeSeconds = yellowTimeSeconds;
            break;

        case MODE_SET_YELLOW:
            mode = MODE_SET_GREEN;
            selectedTimeSeconds = greenTimeSeconds;
            break;

        case MODE_SET_GREEN:
        default:
            mode = MODE_NORMAL;
            trafficPhase = PHASE_VERTICAL_GREEN;
            phaseElapsedTicks = 0U;
            break;
    }

    blinkElapsedTicks = 0U;
    blinkOn = 1U;
}

/* Minimum valid value for the selected timing. */
static uint8_t GetConfigMin(void)
{
    if (mode == MODE_SET_RED)
    {
        /* Red = green + yellow, and green must be at least 1. */
        return (uint8_t)(yellowTimeSeconds + 1U);
    }

    return 1U;
}

/* Maximum valid value, maintaining red = green + yellow. */
static uint8_t GetConfigMax(void)
{
    if (mode == MODE_SET_RED)
    {
        return 99U;
    }

    if (mode == MODE_SET_YELLOW)
    {
        return (uint8_t)(99U - greenTimeSeconds);
    }

    if (mode == MODE_SET_GREEN)
    {
        return (uint8_t)(99U - yellowTimeSeconds);
    }

    return 99U;
}

static void IncreaseSelectedTime(void)
{
    uint8_t minValue = GetConfigMin();
    uint8_t maxValue = GetConfigMax();

    if ((selectedTimeSeconds < minValue) ||
        (selectedTimeSeconds >= maxValue))
    {
        selectedTimeSeconds = minValue;
    }
    else
    {
        selectedTimeSeconds++;
    }
}

static void DecreaseSelectedTime(void)
{
    uint8_t minValue = GetConfigMin();
    uint8_t maxValue = GetConfigMax();

    /* Wrap to the maximum when the minimum is reached. */
    if ((selectedTimeSeconds <= minValue) ||
        (selectedTimeSeconds > maxValue))
    {
        selectedTimeSeconds = maxValue;
    }
    else
    {
        selectedTimeSeconds--;
    }
}

/* Save the selected value with Button 3. */
static void ConfirmSelectedTime(void)
{
    if (mode == MODE_SET_RED)
    {
        redTimeSeconds = selectedTimeSeconds;

        /* Red = green + yellow. */
        greenTimeSeconds =
            (uint8_t)(redTimeSeconds - yellowTimeSeconds);
    }
    else if (mode == MODE_SET_YELLOW)
    {
        yellowTimeSeconds = selectedTimeSeconds;

        redTimeSeconds =
            (uint8_t)(greenTimeSeconds + yellowTimeSeconds);
    }
    else if (mode == MODE_SET_GREEN)
    {
        greenTimeSeconds = selectedTimeSeconds;

        redTimeSeconds =
            (uint8_t)(greenTimeSeconds + yellowTimeSeconds);
    }

    /* Remain in the current configuration mode after saving. */
    blinkElapsedTicks = 0U;
    blinkOn = 1U;
}

static uint8_t ProcessButtonEvents(void)
{
    uint8_t changed = 0U;

    /* Button 1: Mode 1 -> Mode 2 -> Mode 3 -> Mode 4 -> Mode 1. */
    if (TakeButtonEvent(0))
    {
        EnterNextMode();
        changed = 1U;
    }

    /*
     * Button 2:
     * Short press = increase by 1.
     * Hold for 1 second = decrease by 1.
     */
    uint8_t button2ShortPress = TakeButtonEvent(1);
    uint8_t button2LongPress = TakeButton2DecreaseEvent();

    if (mode != MODE_NORMAL)
    {
        if (button2ShortPress)
        {
            IncreaseSelectedTime();
            changed = 1U;
        }

        if (button2LongPress)
        {
            DecreaseSelectedTime();
            changed = 1U;
        }
    }

    /* Button 3 confirms timing only in configuration modes. */
    if ((mode != MODE_NORMAL) && TakeButtonEvent(2))
    {
        ConfirmSelectedTime();
        changed = 1U;
    }

    return changed;
}

/* =========================================================
 * TRAFFIC LIGHT STATE MACHINE AND COUNTDOWN
 * ========================================================= */
static uint8_t CurrentPhaseDurationSeconds(void)
{
    if ((trafficPhase == PHASE_VERTICAL_YELLOW) ||
        (trafficPhase == PHASE_HORIZONTAL_YELLOW))
    {
        return yellowTimeSeconds;
    }

    return greenTimeSeconds;
}

static void AdvanceTrafficPhase(void)
{
    switch (trafficPhase)
    {
        case PHASE_VERTICAL_GREEN:
            trafficPhase = PHASE_VERTICAL_YELLOW;
            break;

        case PHASE_VERTICAL_YELLOW:
            trafficPhase = PHASE_HORIZONTAL_GREEN;
            break;

        case PHASE_HORIZONTAL_GREEN:
            trafficPhase = PHASE_HORIZONTAL_YELLOW;
            break;

        case PHASE_HORIZONTAL_YELLOW:
        default:
            trafficPhase = PHASE_VERTICAL_GREEN;
            break;
    }

    phaseElapsedTicks = 0U;
}

static void ProcessOne10msTick(void)
{
    if (mode == MODE_NORMAL)
    {
        uint32_t durationTicks =
            (uint32_t)CurrentPhaseDurationSeconds() *
            TRAFFIC_COUNTDOWN_TICKS_PER_SECOND;

        phaseElapsedTicks++;

        if (phaseElapsedTicks >= durationTicks)
        {
            AdvanceTrafficPhase();
        }
    }
    else
    {
        blinkElapsedTicks++;

        if (blinkElapsedTicks >= BLINK_HALF_PERIOD_TICKS)
        {
            blinkElapsedTicks = 0U;
            blinkOn = (uint8_t)!blinkOn;
        }
    }
}

/* Convert remaining ticks to a displayed number, rounding up. */
static uint8_t TicksToSecondsCeil(uint32_t ticks)
{
    uint32_t seconds =
        (ticks + TRAFFIC_COUNTDOWN_TICKS_PER_SECOND - 1U) /
        TRAFFIC_COUNTDOWN_TICKS_PER_SECOND;

    if (seconds > 99U)
    {
        seconds = 99U;
    }

    return (uint8_t)seconds;
}

static void UpdateDisplayValues(void)
{
    if (mode != MODE_NORMAL)
    {
        verticalDisplayValue = selectedTimeSeconds;
        horizontalDisplayValue = (uint8_t)mode;
        return;
    }

    uint32_t phaseDurationTicks =
        (uint32_t)CurrentPhaseDurationSeconds() *
        TRAFFIC_COUNTDOWN_TICKS_PER_SECOND;

    uint32_t remainingTicks = 0U;

    if (phaseElapsedTicks < phaseDurationTicks)
    {
        remainingTicks = phaseDurationTicks - phaseElapsedTicks;
    }

    switch (trafficPhase)
    {
        case PHASE_VERTICAL_GREEN:
            verticalDisplayValue =
                TicksToSecondsCeil(remainingTicks);

            horizontalDisplayValue =
                TicksToSecondsCeil(
                    remainingTicks +
                    (uint32_t)yellowTimeSeconds *
                    TRAFFIC_COUNTDOWN_TICKS_PER_SECOND);
            break;

        case PHASE_VERTICAL_YELLOW:
            verticalDisplayValue =
                TicksToSecondsCeil(remainingTicks);

            horizontalDisplayValue =
                TicksToSecondsCeil(remainingTicks);
            break;

        case PHASE_HORIZONTAL_GREEN:
            horizontalDisplayValue =
                TicksToSecondsCeil(remainingTicks);

            verticalDisplayValue =
                TicksToSecondsCeil(
                    remainingTicks +
                    (uint32_t)yellowTimeSeconds *
                    TRAFFIC_COUNTDOWN_TICKS_PER_SECOND);
            break;

        case PHASE_HORIZONTAL_YELLOW:
        default:
            verticalDisplayValue =
                TicksToSecondsCeil(remainingTicks);

            horizontalDisplayValue =
                TicksToSecondsCeil(remainingTicks);
            break;
    }
}

/* =========================================================
 * TRAFFIC LED OUTPUTS
 * Active LOW: GPIO RESET = ON, GPIO SET = OFF.
 * ========================================================= */
static void UpdateTrafficLEDs(void)
{
    uint16_t onMask = 0U;

    const uint16_t allPins =
        R_1_Pin | G_1_Pin | Y_1_Pin |
        R_2_Pin | G_2_Pin | Y_2_Pin;

    if (mode == MODE_NORMAL)
    {
        /* Vertical green, horizontal red. */
        switch (trafficPhase)
        {
            case PHASE_VERTICAL_GREEN:
                onMask = G_1_Pin | R_2_Pin;
                break;

            case PHASE_VERTICAL_YELLOW:
                onMask = Y_1_Pin | R_2_Pin;
                break;

            case PHASE_HORIZONTAL_GREEN:
                onMask = R_1_Pin | G_2_Pin;
                break;

            case PHASE_HORIZONTAL_YELLOW:
            default:
                onMask = R_1_Pin | Y_2_Pin;
                break;
        }
    }
    else if (blinkOn)
    {
        /* Configuration mode: blink only the selected color. */
        if (mode == MODE_SET_RED)
        {
            onMask = R_1_Pin | R_2_Pin;
        }
        else if (mode == MODE_SET_YELLOW)
        {
            onMask = Y_1_Pin | Y_2_Pin;
        }
        else if (mode == MODE_SET_GREEN)
        {
            onMask = G_1_Pin | G_2_Pin;
        }
    }

    /* Set inactive pins HIGH and active pins LOW in one write. */
    TRAFFIC_LED_GPIO_Port->BSRR =
        ((uint32_t)(allPins & (uint16_t)(~onMask))) |
        ((uint32_t)onMask << 16U);
}

/* =========================================================
 * FOUR-DIGIT MULTIPLEX DISPLAY
 * ========================================================= */
static void AllDigitsOff(void)
{
    HAL_GPIO_WritePin(
        SEG_ENABLE_GPIO_Port,
        EN0_Pin | EN1_Pin | EN2_Pin | EN3_Pin,
        GPIO_PIN_SET);
}

static void WriteDigitSegments(uint8_t verticalDisplay, uint8_t digit)
{
    uint8_t pattern = segmentCode[digit % 10U];

    const uint16_t *pins =
        verticalDisplay ? verticalSegmentPins : horizontalSegmentPins;

    for (uint8_t bit = 0; bit < 7; bit++)
    {
        HAL_GPIO_WritePin(
            GPIOB,
            pins[bit],
            (pattern & (1U << bit)) ?
            GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

static void ScanSevenSegmentDisplays(void)
{
    static uint32_t lastScanMs = 0U;
    static uint8_t digitIndex = 0U;

    uint32_t now = HAL_GetTick();

    uint8_t value;
    uint8_t verticalDisplay;

    if ((now - lastScanMs) < DISPLAY_DIGIT_MS)
    {
        return;
    }

    lastScanMs = now;

    AllDigitsOff();

    switch (digitIndex)
    {
        case 0: /* Vertical tens */
            value = (uint8_t)(verticalDisplayValue / 10U);
            verticalDisplay = 1U;

            WriteDigitSegments(verticalDisplay, value);

            HAL_GPIO_WritePin(
                SEG_ENABLE_GPIO_Port,
                EN0_Pin,
                GPIO_PIN_RESET);
            break;

        case 1: /* Vertical ones */
            value = (uint8_t)(verticalDisplayValue % 10U);
            verticalDisplay = 1U;

            WriteDigitSegments(verticalDisplay, value);

            HAL_GPIO_WritePin(
                SEG_ENABLE_GPIO_Port,
                EN1_Pin,
                GPIO_PIN_RESET);
            break;

        case 2: /* Horizontal tens */
            value = (uint8_t)(horizontalDisplayValue / 10U);
            verticalDisplay = 0U;

            WriteDigitSegments(verticalDisplay, value);

            HAL_GPIO_WritePin(
                SEG_ENABLE_GPIO_Port,
                EN2_Pin,
                GPIO_PIN_RESET);
            break;

        case 3: /* Horizontal ones */
        default:
            value = (uint8_t)(horizontalDisplayValue % 10U);
            verticalDisplay = 0U;

            WriteDigitSegments(verticalDisplay, value);

            HAL_GPIO_WritePin(
                SEG_ENABLE_GPIO_Port,
                EN3_Pin,
                GPIO_PIN_RESET);
            break;
    }

    digitIndex = (uint8_t)((digitIndex + 1U) % 4U);
}

/* =========================================================
 * CLOCK CONFIGURATION
 * HSI = 8 MHz, APB1 = 8 MHz.
 * ========================================================= */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue =
        RCC_HSICALIBRATION_DEFAULT;

    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;

    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(
            &RCC_ClkInitStruct,
            FLASH_LATENCY_0) != HAL_OK)
    {
        Error_Handler();
    }
}

/* =========================================================
 * TIM2 CONFIGURATION
 * 8 MHz / (7999 + 1) = 1 kHz.
 * ARR = 9 => interrupt every 10 ms.
 * ========================================================= */
static void MX_TIM2_Init(void)
{
    TIM_ClockConfigTypeDef sClockSourceConfig = {0};
    TIM_MasterConfigTypeDef sMasterConfig = {0};

    htim2.Instance = TIM2;

    htim2.Init.Prescaler = 7999;
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 9;
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload =
        TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
    {
        Error_Handler();
    }

    sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;

    if (HAL_TIM_ConfigClockSource(
            &htim2,
            &sClockSourceConfig) != HAL_OK)
    {
        Error_Handler();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
    sMasterConfig.MasterSlaveMode =
        TIM_MASTERSLAVEMODE_DISABLE;

    if (HAL_TIMEx_MasterConfigSynchronization(
            &htim2,
            &sMasterConfig) != HAL_OK)
    {
        Error_Handler();
    }
}

/* =========================================================
 * GPIO CONFIGURATION
 * ========================================================= */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    /*
     * PA13-PA15 are used for buttons.
     * This disables SWJ/JTAG to use those pins as GPIO.
     * It also disables normal SWD debugging.
     */
    __HAL_AFIO_REMAP_SWJ_DISABLE();

    /* Traffic LEDs: active LOW; HIGH means OFF. */
    GPIO_InitStruct.Pin =
        R_1_Pin | G_1_Pin | Y_1_Pin |
        R_2_Pin | G_2_Pin | Y_2_Pin;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(TRAFFIC_LED_GPIO_Port, &GPIO_InitStruct);

    HAL_GPIO_WritePin(
        TRAFFIC_LED_GPIO_Port,
        R_1_Pin | G_1_Pin | Y_1_Pin |
        R_2_Pin | G_2_Pin | Y_2_Pin,
        GPIO_PIN_SET);

    /* Digit enables: active LOW; HIGH means OFF. */
    GPIO_InitStruct.Pin = EN0_Pin | EN1_Pin | EN2_Pin | EN3_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(SEG_ENABLE_GPIO_Port, &GPIO_InitStruct);

    AllDigitsOff();

    /* Vertical segment lines PB0-PB6. */
    GPIO_InitStruct.Pin =
        SEG1_0_Pin | SEG1_1_Pin | SEG1_2_Pin |
        SEG1_3_Pin | SEG1_4_Pin | SEG1_5_Pin |
        SEG1_6_Pin;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(SEG1_GPIO_Port, &GPIO_InitStruct);

    HAL_GPIO_WritePin(
        SEG1_GPIO_Port,
        SEG1_0_Pin | SEG1_1_Pin | SEG1_2_Pin |
        SEG1_3_Pin | SEG1_4_Pin | SEG1_5_Pin |
        SEG1_6_Pin,
        GPIO_PIN_SET);

    /* Horizontal segment lines PB9-PB15. */
    GPIO_InitStruct.Pin =
        SEG2_0_Pin | SEG2_1_Pin | SEG2_2_Pin |
        SEG2_3_Pin | SEG2_4_Pin | SEG2_5_Pin |
        SEG2_6_Pin;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    HAL_GPIO_Init(SEG2_GPIO_Port, &GPIO_InitStruct);

    HAL_GPIO_WritePin(
        SEG2_GPIO_Port,
        SEG2_0_Pin | SEG2_1_Pin | SEG2_2_Pin |
        SEG2_3_Pin | SEG2_4_Pin | SEG2_5_Pin |
        SEG2_6_Pin,
        GPIO_PIN_SET);

    /* Buttons PA13-PA15: active LOW, internal pull-up. */
    GPIO_InitStruct.Pin = BUT1_Pin | BUT2_Pin | BUT3_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;

    HAL_GPIO_Init(BUTTON_GPIO_Port, &GPIO_InitStruct);
}

/* =========================================================
 * ERROR HANDLER
 * ========================================================= */
void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}
