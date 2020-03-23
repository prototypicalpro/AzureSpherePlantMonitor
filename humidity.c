#include "humidity.h"

const static I2C_DeviceAddress humid_addr = 0x44;
const static uint16_t SHT3XD_CMD_READ_SERIAL_NUMBER = 0x3780;
const static uint16_t SHT3XD_CMD_SOFT_RESET = 0x30A2;
const static uint16_t SHT3XD_CMD_PERIODIC_HALF_H = 0x2032;
const static uint16_t SHT3XD_CMD_FETCH_DATA = 0xE000;

static int humid_fd = -1;

int HumidityInit(int i2cfd) {
	humid_fd = i2cfd;
	// verify that the sensor attatched is the SHT31D
	const uint8_t ser_cmd[2] = { (uint8_t)(SHT3XD_CMD_READ_SERIAL_NUMBER >> 8), SHT3XD_CMD_READ_SERIAL_NUMBER & 0xFFU };
	uint8_t out[6];
	int ret = I2CMaster_WriteThenRead(i2cfd, humid_addr, ser_cmd, sizeof(ser_cmd), out, sizeof(out));
	if (ret < 0) {
		Log_Debug("Initialize humid failed\n");
		return -1;
	}
	// ignore CRC check for now
	uint32_t serial = ((uint32_t)(out[0]) << 24) | ((uint32_t)(out[1]) << 16) | ((uint32_t)out[3] << 8) | (uint32_t)(out[4]);
	if (serial == 0 || serial == 0xFFFFU) {
		Log_Debug("Got invalid humidity serial number!\n");
		return -1;
	}
	Log_Debug("Found humidity sensor with serial %u\n", serial);
	// reset the sensor, and set it into periodic mode at the slowest setting
	const uint8_t reset_cmd[2] = { (uint8_t)(SHT3XD_CMD_SOFT_RESET >> 8), SHT3XD_CMD_SOFT_RESET & 0xFFU };
	const uint8_t poll_cmd[2] = { (uint8_t)(SHT3XD_CMD_PERIODIC_HALF_H >> 8), SHT3XD_CMD_PERIODIC_HALF_H & 0xFFU };
	if (I2CMaster_Write(i2cfd, humid_addr, reset_cmd, sizeof(reset_cmd)) < 0
		|| I2CMaster_Write(i2cfd, humid_addr, poll_cmd, sizeof(poll_cmd)) < 0) {
		
		Log_Debug("Configuring humid failed\n");
		return -1;
	}

	return 0;
}

int HumidityMeasure(humidity_data_t* data_out) {
	// read the sensor!
	const uint8_t fetch_cmd[2] = { (uint8_t)(SHT3XD_CMD_FETCH_DATA >> 8), SHT3XD_CMD_FETCH_DATA & 0xFFU };
	uint8_t out[6];
	int ret = I2CMaster_WriteThenRead(humid_fd, humid_addr, fetch_cmd, sizeof(fetch_cmd), out, sizeof(out));
	if (ret < 0) {
		Log_Debug("Failed to read humid\n");
		data_out->humidity = 0;
		return -1;
	}
	// translate the humidity bytes (3-4) into a float
	uint16_t humid = (uint16_t)((out[3] << 8) | out[4]);
	data_out->humidity = 100.0f * (float)humid / 65535.0f;
	return 0;
}