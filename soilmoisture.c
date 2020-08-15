#include "soilmoisture.h"

const static I2C_DeviceAddress soil_addr_1 = (0x24 & 0xFEU);
const static I2C_DeviceAddress soil_addr_2 = (0x26 & 0xFEU);

static int soil_i2cfd = -1;

typedef union {
	uint16_t u16bit;
	uint8_t u8bit[2];
} axis1bit16_t;

inline static void swap(uint8_t* a, uint8_t* b) {
	uint8_t c = *a;
	*a = *b;
	*b = c;
}

int SoilMoistureInit(int i2cfd) {
	// store i2cfd
	soil_i2cfd = i2cfd;
	// verify both sensors are attatched
	axis1bit16_t out;
	const uint8_t reg = 0;
	I2CMaster_SetTimeout(i2cfd, 500);
	int ret = I2CMaster_WriteThenRead(i2cfd, soil_addr_1, &reg, 1, out.u8bit, 2);
	swap(out.u8bit, out.u8bit + 1);
	if (ret < 0 || out.u16bit > 10000U)
		Log_Debug("Soil sensor 1 not found!\n");
	int ret2 = I2CMaster_WriteThenRead(i2cfd, soil_addr_2, &reg, 1, out.u8bit, 2);
	swap(out.u8bit, out.u8bit + 1);
	if (ret2 < 0 || out.u16bit > 10000U)
		Log_Debug("Soil sensor 2 not found!\n");

	return 0;
}

int SoilMoistureMeasure(soil_data_t* data_out) {
	// read data!
	//  first sensor
	axis1bit16_t out;
	const uint8_t reg = 0;
	int ret = I2CMaster_WriteThenRead(soil_i2cfd, soil_addr_1, &reg, 1, out.u8bit, 2);
	swap(out.u8bit, out.u8bit + 1);
	if (ret < 0 || out.u16bit > 10000U) {
		Log_Debug("Soil sensor 1 not found!\n");
		data_out->soil_moisture_24 = 0;
	}
	else
		data_out->soil_moisture_24 = out.u16bit;
	// second sensor
	int ret2 = I2CMaster_WriteThenRead(soil_i2cfd, soil_addr_2, &reg, 1, out.u8bit, 2);
	swap(out.u8bit, out.u8bit + 1);
	if (ret2 < 0 || out.u16bit > 10000U) {
		Log_Debug("Soil sensor 2 not found!\n");
		data_out->soil_moisture_26 = 0;
	}
	else
		data_out->soil_moisture_26 = out.u16bit;

	return 0;
}