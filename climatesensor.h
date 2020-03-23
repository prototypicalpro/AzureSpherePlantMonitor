/** Handle sensor hub input from the lsm6dso gyro, getting pressure and tempurature */

#ifndef CLIMATESENSOR_H
#define CLIMATESENSOR_H

#include "lsm6dso_reg.h"
#include "lps22hh_reg.h"
#include <applibs/i2c.h>
#include <applibs/log.h>
#include <time.h>
#include <string.h>
#include <errno.h>

typedef struct {
	float avg_tempurature;
	float avg_pressure;
	int num_samples;
} climate_data_t;

int ClimateSensorInit(int i2cfd);

int ClimateSensorMeasure(climate_data_t* data_out);

#endif