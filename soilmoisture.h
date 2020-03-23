/** Handle soil moisture sensor data from the chirp! soil moisture sensors */

#ifndef SOILMOISTURE_H
#define SOILMOISTURE_H

#include <applibs/i2c.h>
#include <applibs/log.h>
#include <errno.h>

typedef struct {
	uint16_t soil_moisture_24;
	uint16_t soil_moisture_26;
} soil_data_t;

int SoilMoistureInit(int i2cfd);

int SoilMoistureMeasure(soil_data_t* data_out);

#endif