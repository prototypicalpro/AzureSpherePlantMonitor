/** Handle humidity data from the SHT31D sensor */

#ifndef SHT31D_H
#define SHT31D_H

#include <errno.h>
#include <stdbool.h>

#include <applibs/i2c.h>
#include <applibs/log.h>

typedef struct {
	double humidity;
} humidity_data_t;

typedef struct {
	int _fd;
	bool _is_active;
} humidity_t;

int HumidityInit(humidity_t* humidity, int i2cfd);
int HumidityMeasure(humidity_t* humidity, humidity_data_t* data_out);
bool HumidityIsOk(humidity_t* humidity);

#endif