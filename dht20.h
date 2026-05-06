// dht20.h — DHT20 air temperature + humidity reader.
// Lives on Port A, internal I2C bus, 10-second cadence.

#pragma once

void dht20_start();   // creates and starts the FreeRTOS task
