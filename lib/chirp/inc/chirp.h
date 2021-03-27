/** Handle soil moisture sensor data from the chirp! soil moisture sensors */

#ifndef CHIRP_H
#define CHIRP_H

#include <applibs/i2c.h>
#include <applibs/log.h>
#include <errno.h>
#include <stdbool.h>

#define CHIRP_ADDR_1 (0x24 & 0xFEU)
#define CHIRP_ADDR_2 (0x26 & 0xFEU)

typedef struct {
	uint16_t soil_moisture;
} chirp_data_t;

typedef struct {
	int _fd;
	I2C_DeviceAddress _addr;
	bool _is_active;
} chirp_t;


int ChirpInit(chirp_t* chirp, int i2cfd, I2C_DeviceAddress addr);
int ChirpMeasure(chirp_t* chirp, chirp_data_t* data_out);
bool ChirpIsOk(chirp_t* chirp);

#endif