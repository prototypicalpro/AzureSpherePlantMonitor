/** Handle humidity data from the SHT31D sensor */

#ifndef HUMIDITY_H
#define HUMIDITY_H

#include <applibs/i2c.h>
#include <applibs/log.h>
#include <errno.h>

typedef struct {
	float humidity;
} humidity_data_t;

int HumidityInit(int i2cfd);

int HumidityMeasure(humidity_data_t* data_out);

#endif