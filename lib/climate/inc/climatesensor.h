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
#include <stdbool.h>

typedef struct {
	double avg_tempurature;
	double avg_pressure;
	int num_samples;
} climate_data_t;

typedef struct {
	int i2cfd;
	uint8_t addr;
} _handle_ctx_t;

typedef struct {
	_handle_ctx_t _press_handle_ctx;
	_handle_ctx_t _gyro_handle_ctx;
	stmdev_ctx_t _press_ctx;
	stmdev_ctx_t _ag_ctx;
	bool _is_active;
} climate_t;

int ClimateSensorInit(climate_t* climate, int i2cfd);
int ClimateSensorMeasure(climate_t* climate, climate_data_t* data_out);
bool ClimateSensorIsOk(climate_t* climate);

#endif