#include "climatesensor.h"

typedef struct {
	int i2cfd;
	uint8_t addr;
} handle_ctx_t;

typedef struct {
	float temp;
	float pressure;
} climate_sample_t;

static stmdev_ctx_t press_ctx;
static stmdev_ctx_t ag_ctx;

static handle_ctx_t gyro_handle_ctx = {
	-1,
	(LSM6DSO_I2C_ADD_L & 0xFEU) >> 1
};

static handle_ctx_t press_handle_ctx = {
	-1,
	(LPS22HH_I2C_ADD_L & 0xFEU) >> 1
};

/*
 * @brief  Write generic device register (platform dependent)
 *
 * @param  handle    customizable argument. In this examples is used in
 *                   order to select the correct sensor bus handler.
 * @param  reg       register to write
 * @param  bufp      pointer to data to write in register reg
 * @param  len       number of consecutive register to write
 *
 */
static int32_t platform_write(void* handle, uint8_t Reg, uint8_t* Bufp,
	uint16_t len)
{
	handle_ctx_t* ctx = (handle_ctx_t*)handle;
	uint8_t buf_cpy[len + 1];

	buf_cpy[0] = Reg;
	for (uint16_t i = 0; i < len; i++)
		buf_cpy[i + 1] = Bufp[i];
	int ret = I2CMaster_Write(ctx->i2cfd, ctx->addr, buf_cpy, (size_t)(len + 1));
	if (ret == -1) {
		Log_Debug("I2C write fail\n");
	}
	return ret != -1 ? 0 : -1;
}

/*
 * @brief  Read generic device register (platform dependent)
 *
 * @param  handle    customizable argument. In this examples is used in
 *                   order to select the correct sensor bus handler.
 * @param  reg       register to read
 * @param  bufp      pointer to buffer that store the data read
 * @param  len       number of consecutive register to read
 *
 */
static int32_t platform_read(void* handle, uint8_t Reg, uint8_t* Bufp,
	uint16_t len)
{
	handle_ctx_t* ctx = (handle_ctx_t* )handle;
	int ret = I2CMaster_WriteThenRead(ctx->i2cfd, ctx->addr, &Reg, 1, Bufp, len);
	if (ret == -1) {
		Log_Debug("I2C read fail\n");
	}
	return ret != -1 ? 0 : -1;
}

int ClimateSensorInit(int i2cfd) {
	// initialize global contexts
	gyro_handle_ctx.i2cfd = i2cfd;
	press_handle_ctx.i2cfd = i2cfd;
	
	
	uint8_t whoamI, rst;
	/* Initialize lsm6dso driver interface */
	ag_ctx.write_reg = platform_write;
	ag_ctx.read_reg = platform_read;
	ag_ctx.handle = &gyro_handle_ctx;

	/* Initialize lps22hh driver interface */
	press_ctx.read_reg = platform_read;
	press_ctx.write_reg = platform_write;
	press_ctx.handle = &press_handle_ctx;

	/*
	 * Check Connected devices.
	 */
	 /* Check lsm6dso ID. */
	lsm6dso_device_id_get(&ag_ctx, &whoamI);
	if (whoamI != LSM6DSO_ID) {
		Log_Debug("Could not find accel\n");
		return -1;
	}

	/* Restore default configuration. */
	lsm6dso_reset_set(&ag_ctx, PROPERTY_ENABLE);
	do {
		lsm6dso_reset_get(&ag_ctx, &rst);
	} while (rst);

	/* Disable I3C interface.*/
	lsm6dso_i3c_disable_set(&ag_ctx, LSM6DSO_I3C_DISABLE);

	/* Disable all sensors */
	lsm6dso_xl_data_rate_set(&ag_ctx, LSM6DSO_XL_ODR_OFF);
	lsm6dso_gy_data_rate_set(&ag_ctx, LSM6DSO_GY_ODR_OFF);

	/* Some hardware require to enable pull up on master I2C interface. */
    // lsm6dso_sh_pin_mode_set(&ag_ctx, LSM6DSO_INTERNAL_PULL_UP);

    /* Disable I2C master */
    lsm6dso_sh_master_set(&ag_ctx, PROPERTY_DISABLE);

	/* Enable I2C pass-through */
	lsm6dso_sh_pass_through_set(&ag_ctx, PROPERTY_ENABLE);
	
    /* Check if LPS22HH connected to Sensor Hub. */
	lps22hh_device_id_get(&press_ctx, &whoamI);
	if (whoamI != LPS22HH_ID) {
		Log_Debug("Could not find pressure sensor\n");
		return -1;
	}

	// Restore the default configuration
	lps22hh_reset_set(&press_ctx, PROPERTY_ENABLE);
	do {
		lps22hh_reset_get(&press_ctx, &rst);
	} while (rst);

	/* Configure LPS22HH. */
	lps22hh_i3c_interface_set(&press_ctx, LPS22HH_I3C_DISABLE);
	// lps22hh_block_data_update_set(&press_ctx, PROPERTY_ENABLE);
	lps22hh_fifo_mode_set(&press_ctx, LPS22HH_FIFO_MODE);
	lps22hh_data_rate_set(&press_ctx, LPS22HH_1_Hz_LOW_NOISE);
	lps22hh_lp_bandwidth_set(&press_ctx, LPS22HH_LPF_ODR_DIV_20);
	
	return 0;
}

typedef union {
	int32_t i32bit;
	uint8_t u8bit[4];
} axis1bit32_t;

typedef union {
	int16_t i16bit;
	uint8_t u8bit[2];
} axis1bit16_t;

int ClimateSensorMeasure(climate_data_t* data_out) {
	/* Read number of samples in FIFO. */
	uint8_t fifo_level = 0;

	int32_t ret = lps22hh_fifo_data_level_get(&press_ctx, &fifo_level);
	if (ret == -1) {
		Log_Debug("Failed to get fifo data level\n");
		return -1;
	}

	int cached_level = fifo_level;
	float float_cached_level = (float)fifo_level;
	climate_sample_t total = {};

	while (fifo_level) {
		static axis1bit32_t data_raw_pressure;
		static axis1bit16_t data_raw_temperature;

		lps22hh_pressure_raw_get(&press_ctx, data_raw_pressure.u8bit);
		lps22hh_temperature_raw_get(&press_ctx, data_raw_temperature.u8bit);

		float pressure_hPa = lps22hh_from_lsb_to_hpa(data_raw_pressure.i32bit);
		float lps22hhTemperature_degC = lps22hh_from_lsb_to_celsius(data_raw_temperature.i16bit);

		total.temp += lps22hhTemperature_degC;
		total.pressure += pressure_hPa;
		
		fifo_level--;
	}

	// reset fifo
	lps22hh_fifo_mode_set(&press_ctx, LPS22HH_BYPASS_MODE);
	lps22hh_fifo_mode_set(&press_ctx, LPS22HH_FIFO_MODE);

	data_out->avg_pressure = total.pressure / float_cached_level;
	data_out->avg_tempurature = total.temp / float_cached_level;
	data_out->num_samples = cached_level;
	return 0;
}