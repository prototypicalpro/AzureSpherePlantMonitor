/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

// This file defines the mapping from the Avnet MT3620 Starter Kit (SK) to the
// 'sample hardware' abstraction used by the samples at https://github.com/Azure/azure-sphere-samples.
// Some peripherals are on-board on the Avnet MT3620 SK, while other peripherals must be attached externally if needed.

// This file is autogenerated from ../../plant_sk.json.  Do not edit it directly.

#pragma once
#include "avnet_mt3620_sk.h"

// MT3620 SK: User Button A.
#define BUTTON_1 AVNET_MT3620_SK_USER_BUTTON_A

// MT3620 SK: User Button B.
#define BUTTON_2 AVNET_MT3620_SK_USER_BUTTON_B

// MT3620 SK: PWM LED controller for status LEDs
#define STATUS_PWM_CONTROLLER AVNET_MT3620_SK_PWM_CONTROLLER1

// MT3620 SK: Channel 0 for app status LED
#define APP_STATUS_PWM_CHANNEL MT3620_PWM_CHANNEL0

// MT3620 SK: Channel 1 for WLAN status LED
#define WLAN_STATUS_PWM_CHANNEL MT3620_PWM_CHANNEL1

// MT3620 SK: PWM LED controller for user LEDs
#define USER_PWM_CONTROLLER AVNET_MT3620_SK_PWM_CONTROLLER2

// MT3620 SK: User LED Red channel
#define USER_LED_RED_PWM_CHANNEL MT3620_PWM_CHANNEL0

// MT3620 SK: User LED Green channel
#define USER_LED_GREEN_PWM_CHANNEL MT3620_PWM_CHANNEL1

// MT3620 SK: User LED Blue channel
#define USER_LED_BLUE_PWM_CHANNEL MT3620_PWM_CHANNEL2

// MT3620 SK: ADC Potentiometer controller
#define LIGHT_ADC_CONTROLLER AVNET_MT3620_SK_ADC_CONTROLLER0

// MT3620 SK: Connect external potentiometer to ADC controller 0, channel 1 using SOCKET1 AN. In the app manifest, it is only necessary to request the capability for the ADC Group Controller, SAMPLE_POTENTIOMETER_ADC_CONTROLLER.
#define LIGHT_ADC_CHANNEL MT3620_ADC_CHANNEL0

// ISU2 I2C is shared between GROVE Connector, OLED DISPLAY Connector, SOCKET1 and SOCKET2. On GROVE Connector: pin 15 (SDA) and pin 10 (SCL). On OLED Display connector: pin 4 (SDA) and pin 3 (SCL).On SOCKET1/2: SDA (SDA) and SCL (SCL)
#define CLIMATE_I2C_CONTROLLER AVNET_AESMS_ISU2_I2C

// MT3620 SK: Connect SOCKET1 RX (RX) to SOCKET1 TX (TX).
#define UART AVNET_MT3620_SK_ISU0_UART

