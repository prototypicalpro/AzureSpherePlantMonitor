#include <time.h>
#include <string.h>

#include "chirp.h"

typedef union {
	uint16_t u16bit;
	uint8_t u8bit[2];
} axis1bit16_t;

int ChirpInit(chirp_t* chirp, int i2c_fd, I2C_DeviceAddress addr) {
	chirp->_fd = i2c_fd;
	chirp->_addr = addr;
	chirp->_is_active = false;
	// verify sensor is attatched
	axis1bit16_t out;
	const uint8_t reg = 0;
	int ret = I2CMaster_WriteThenRead(chirp->_fd, addr, &reg, 1, out.u8bit, 2);
	if (ret < 0 || out.u16bit != 1) {
		Log_Debug("Soil sensor with addr %i not found! Error: %s\n", chirp->_addr, strerror(errno));
		return -1;
	}

	chirp->_is_active = true;
	return 0;
}

int ChirpMeasure(chirp_t* chirp, chirp_data_t* data_out) {
	// read data!
	if (!chirp->_is_active)
		return -1;

	axis1bit16_t out;
	uint8_t reg = 0;
	// trigger sensor
	int ret = I2CMaster_WriteThenRead(chirp->_fd, chirp->_addr, &reg, 1, out.u8bit, 2);
	if (ret < 0 || out.u16bit != 1) {
		Log_Debug("Soil sensor with addr %i not found!\n", chirp->_addr);
		chirp->_is_active = false;
		return -1;
	}

	// wait for cycle to complete
	const struct timespec sleep_time = { .tv_sec = 2, .tv_nsec = 0 };
	struct timespec sleep_time_rem = {};
	int err = clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time, &sleep_time_rem);
	if (err == EINTR)
		clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_time_rem, NULL);

	// read value
	reg = 1;
	ret = I2CMaster_WriteThenRead(chirp->_fd, chirp->_addr, &reg, 1, out.u8bit, 2);
	if (ret < 0 || out.u16bit > 10000U) {
		Log_Debug("Soil sensor with addr %i did not read correctly!\n", chirp->_addr);
		chirp->_is_active = false;
		return -1;
	}
	data_out->soil_moisture = out.u16bit;
	return 0;
}

bool ChirpIsOk(chirp_t* chirp) { return chirp->_is_active; }