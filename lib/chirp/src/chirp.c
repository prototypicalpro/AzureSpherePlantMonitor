#include "chirp.h"

typedef union {
	uint16_t u16bit;
	uint8_t u8bit[2];
} axis1bit16_t;

inline static void swap(uint8_t* a, uint8_t* b) {
	uint8_t c = *a;
	*a = *b;
	*b = c;
}

int ChirpInit(chirp_t* chirp, int i2c_fd, I2C_DeviceAddress addr) {
	chirp->_fd = i2c_fd;
	chirp->_addr = addr;
	chirp->_is_active = false;
	// verify sensor is attatched
	axis1bit16_t out;
	const uint8_t reg = 0;
	int ret = I2CMaster_WriteThenRead(chirp->_fd, addr, &reg, 1, out.u8bit, 2);
	swap(out.u8bit, out.u8bit + 1);
	if (ret < 0 || out.u16bit > 10000U) {
		Log_Debug("Soil sensor with addr %i not found!\n", chirp->_addr);
		return -1;
	}

	chirp->_is_active = true;
	return 0;
}

int ChirpMeasure(chirp_t* chirp, chirp_data_t* data_out) {
	// read data!
	if (!chirp->_is_active)
		return -1;
	//  first sensor
	axis1bit16_t out;
	const uint8_t reg = 0;
	int ret = I2CMaster_WriteThenRead(chirp->_fd, chirp->_addr, &reg, 1, out.u8bit, 2);
	swap(out.u8bit, out.u8bit + 1);
	if (ret < 0 || out.u16bit > 10000U) {
		Log_Debug("Soil sensor with addr %i not found!\n", chirp->_addr);
		return -1;
	}
	data_out->soil_moisture = out.u16bit;
	return 0;
}

bool ChirpIsOk(chirp_t* chirp) { return chirp->_is_active; }