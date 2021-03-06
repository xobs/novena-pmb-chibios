/*
    ChibiOS/RT - Copyright (C) 2006-2013 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <ch.h>
#include <hal.h>
#include <uart.h>
#include <i2c.h>
#include <stdio.h>
#include <chprintf.h>
#include <shell.h>

#include "gg.h"
#include "chg.h"
#include "pmb.h"
#include "bionic.h"
#include "gitversion.h"

#define STREAM_SERIAL	(&SD1)
#define STREAM		((BaseSequentialStream *)STREAM_SERIAL)
#define SHELL_WA_SIZE	THD_WA_SIZE(2048)
#define I2C_BUS		(&I2CD2)

#define DAC_ADDR 0xd

#define CHARGE_VOLTAGE(cells) (4.2*cells)

static int getVer(void) { return 11; }

static void cmd_i2c(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_mode(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_dac(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_stats(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_leds(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_gg(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_chg(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_setmanuf(BaseSequentialStream *chp, int argc, char **argv);
static void cmd_setchem(BaseSequentialStream *chp, int argc, char **argv);

static char *permafailures[] = {
	"fuse is blown",
	"cell imbalance",
	"safety voltage failure",
	"FET failure",
};

static char *mfgr_states[] = {
	"wake up",
	"normal discharge",
	"???",
	"pre-charge",
	"???",
	"charge",
	"???",
	"charge termination",
	"fault charge terminate",
	"permanent failure",
	"overcurrent",
	"overtemperature",
	"battery failure",
	"sleep",
	"reserved",
	"battery removed",
};


/*
 * Devices on the bus:
 * 0x09 - Battery charger (bq24765)
 * 0x0d - Gas gauge (bq20z95dbt)
 * 0x48 - Voltage monitor (dac081c085)
 */
uint8_t bus_devices[] = {
	0x09,	/* Battery charger (BQ24765) */
	0x0b,	/* Fuel gauge (BQ20Z95) */
	0x0d,	/* Voltage monitor (DAC081C081) */
	0x48,	/* ??? (Not a valid address) */
};

static const SerialConfig ser_cfg = {
	115200,
	0,
	0,
	0,
};

static const ShellCommand commands[] = {
	{"i2c",		cmd_i2c},
	{"mode",	cmd_mode},
	{"dac",		cmd_dac},
	{"stats",	cmd_stats},
	{"leds",	cmd_leds},
	{"gg",		cmd_gg},
	{"chg",		cmd_chg},
	{"setchem",	cmd_setchem},
	{"setmanuf",	cmd_setmanuf},
	{NULL,		NULL} /* Sentinal */
};


/* Directly maps to Gas Gauge flash state Offset 82 */
struct gg_state {
	uint16_t qmax_cell_0; /* Measured in mAh */
	uint16_t qmax_cell_1; /* Measured in mAh */
	uint16_t qmax_cell_2; /* Measured in mAh */
	uint16_t qmax_cell_3; /* Measured in mAh */
	uint16_t qmax_pack;   /* Measured in mAh */
	uint16_t reserved1;	/* Offset 11 */
	uint8_t  update_status; /* Offset 12 */
	uint8_t  reserved2[8];
	int16_t  avg_i_last_run;
	int16_t  avg_p_last_run;
	int16_t  delta_voltage;
} __attribute__((__packed__));


/* Directly maps to Gas Gauge flash Power Offset 68 */
struct gg_power {
	uint16_t flash_update_ok_voltage; /* mV */
	uint16_t shutdown_voltage; /* mV */
	uint8_t  shutdown_time; /* Seconds */
	uint16_t shutdown_voltage_cell0; /* mV */
	uint16_t thing;
} __attribute__((__packed__));

struct gg_config {
	uint16_t cfg_a;
	uint16_t cfg_b;
	uint16_t cfg_c;
	uint16_t permfail_cfg;
	uint16_t nonremove_cfg;
} __attribute__((__packed__));

static const ShellConfig shell_cfg = {
	STREAM,
	commands
};

static inline int _isprint(char c) {
	return c>32 && c<128;
}

int print_hex_offset(BaseSequentialStream *chp, uint8_t *block, int count, int offset) {
    int byte;
    count += offset;
    block -= offset;
    for ( ; offset<count; offset+=16) {
        chprintf(chp, "%08x ", offset);

        for (byte=0; byte<16; byte++) {
            if (byte == 8)
                chprintf(chp, " ");
            if (offset+byte < count)
                chprintf(chp, " %02x", block[offset+byte]&0xff);
            else
                chprintf(chp, "   ");
        }

        chprintf(chp, "  |");
        for (byte=0; byte<16 && byte+offset<count; byte++)
            chprintf(chp, "%c", _isprint(block[offset+byte]) ?
                                    block[offset+byte] :
                                    '.');
        chprintf(chp, "|\r\n");
    }
    return 0;
}

int print_hex(BaseSequentialStream *chp, uint8_t *block, int count) {
    return print_hex_offset(chp, block, count, 0);
}



static void cmd_i2c(BaseSequentialStream *chp, int argc, char **argv) {
	uint16_t val;
	uint8_t *ptr;
	uint8_t reg;
	int ret;

	ptr = (uint8_t *)&val;
	if (argc != 1) {
		chprintf(chp, "Usage: i2c [address] to get gas gauge\r\n");
		return;
	}
	extern int gg_getword(struct I2CDriver *driver, uint8_t reg, void *word);

	reg = _strtoul(argv[0], NULL, 0);
	ret = gg_getword(I2C_BUS, reg, &val);
	if (ret < 0) {
		chprintf(chp, "Unable to get register: 0x%x\r\n", ret);
		return;
	}
	chprintf(chp, "Register 0x%02x: 0x%04x (%02x %02x)\r\n", reg, val, ptr[0], ptr[1]);
	return;
}

static void cmd_mode(BaseSequentialStream *chp, int argc, char **argv) {
	if (argc == 1 && argv[0] && argv[0][0] == 'p') {
		chprintf(chp, "Setting primary mode...");
		gg_setprimary(I2C_BUS);
		chprintf(chp, " Set\r\n");
	}
	else if (argc == 1 && argv[0] && argv[0][0] == 's') {
		chprintf(chp, "Setting secondary mode...");
		gg_setsecondary(I2C_BUS);
		chprintf(chp, " Set\r\n");
	}
	else {
		uint8_t mode[2];
		gg_getmode(I2C_BUS, mode);
		chprintf(chp, "Current mode:\r\n");
		chprintf(chp, "\tInternal charge controller%s supported\r\n",
				mode[1] & (1<<0)?"":" NOT");
		chprintf(chp, "\tPrimary battery support%s supported\r\n",
				mode[1] & (1<<1)?"":" NOT");
		if (mode[1] & (1<<7))
			chprintf(chp, "\tConditioning cycle requested\r\n");
		chprintf(chp, "\tInternal charge control %sabled\r\n",
				mode[0] & (1<<0)?"EN":"DIS");
		chprintf(chp, "\t%s battery\r\n",
				mode[0] & (1<<1)?"Primary":"Secondary");
		chprintf(chp, "\tAlarm broadcasts %sabled\r\n",
				mode[0] & (1<<5)?"DIS":"EN");
		chprintf(chp, "\tCharge broadcasts %sabled\r\n",
				mode[0] & (1<<6)?"DIS":"EN");
		chprintf(chp, "\tCapacity measured in %s\r\n",
				mode[0] & (1<<7)?"10 mW":"mA");
	}
	return;
}

static void cmd_dac(BaseSequentialStream *chp, int argc, char **argv) {
	void *driver = I2C_BUS;
	systime_t timeout = TIME_INFINITE;
	uint16_t result;
	int status;
	(void)argc;
	(void)argv;

	i2cAcquireBus(driver);
	status = i2cMasterReceiveTimeout(driver, DAC_ADDR,
			(void *)&result, sizeof(result),
			timeout);
	i2cReleaseBus(driver);

	if (status != RDY_OK) {
		if (status == RDY_TIMEOUT)
			chprintf(chp, "Unable to read from DAC: timeout\r\n");
		else
			chprintf(chp, "Unable to read from DAC: 0x%x\r\n",
					i2cGetErrors(driver));
		return;
	}
	
	chprintf(chp, "DAC: 0x%04x (%d)\r\n", result, result);
	return;
}

static void cmd_stats(BaseSequentialStream *chp, int argc, char **argv) {
	uint8_t str[16];
	uint8_t stats[2];
	int16_t word = 0;
	uint16_t minutes;
	uint8_t byte = 0;
	void *driver = I2C_BUS;
	int cell;
	int ret;
	(void)argc;
	(void)argv;

	ret = gg_manuf(driver, str);
	if (ret < 0)
		chprintf(chp, "Manufacturer:       error 0x%x\r\n", ret);
	else
		chprintf(chp, "Manufacturer:       %s\r\n", str);

	ret = gg_partname(driver, str);
	if (ret < 0)
		chprintf(chp, "Part name:          error 0x%x\r\n", ret);
	else
		chprintf(chp, "Part name:          %s\r\n", str);

	ret = gg_getfirmwareversion(driver, &word);
	if (ret < 0)
		chprintf(chp, "Firmware ver:       error 0x%x\r\n", ret);
	else
		chprintf(chp, "Firmware ver:       0x%04x\r\n", word);

	ret = gg_getstate(driver, stats);
	if (ret < 0)
		chprintf(chp, "State:              error 0x%x\r\n", ret);
	else {
		int chgfet, dsgfet;
		switch (stats[0]>>6) {
		case 0:
			chgfet = 1;
			dsgfet = 1;
			break;
		case 1:
			chgfet = 0;
			dsgfet = 1;
			break;
		case 2:
			chgfet = 0;
			dsgfet = 0;
			break;
		case 3:
		default:
			chgfet = 1;
			dsgfet = 0;
			break;
		}
		chprintf(chp, "Charge FET:         %s\r\n", chgfet?"on":"off");
		chprintf(chp, "Discharge FET:      %s\r\n", dsgfet?"on":"off");
		chprintf(chp, "State:              %s\r\n", mfgr_states[stats[0] & 0xf]);
		if ((stats[0] & 0xf) == 0x9)
			chprintf(chp, "PermaFailure:       %s\r\n",
					permafailures[(stats[0] >> 4) & 3]);
	}

	ret = gg_timetofull(I2C_BUS, &minutes);
	if (ret < 0)
		chprintf(chp, "Time until full:    error 0x%x\r\n", ret);
	else
		chprintf(chp, "Time until full:    %d minutes\r\n", minutes);

	ret = gg_timetoempty(I2C_BUS, &minutes);
	if (ret < 0)
		chprintf(chp, "Time until empty:   error 0x%x\r\n", ret);
	else
		chprintf(chp, "Time until empty:   %d minutes\r\n", minutes);

	ret = gg_chem(driver, str);
	if (ret < 0)
		chprintf(chp, "Chemistry:          error 0x%x\r\n", ret);
	else
		chprintf(chp, "Chemistry:          %s\r\n", str);

	ret = gg_serial(driver, &word);
	if (ret < 0)
		chprintf(chp, "Serial number:      error 0x%x\r\n", ret);
	else
		chprintf(chp, "Serial number:      0x%04x\r\n", word);

	ret = gg_percent(driver, &byte);
	if (ret < 0)
		chprintf(chp, "Capacity:           error 0x%x\r\n", ret);
	else
		chprintf(chp, "Capacity:           %d%%\r\n", byte);

	ret = gg_fullcapacity(driver, &word);
	if (ret < 0)
		chprintf(chp, "Full capacity:      error 0x%x\r\n", ret);
	else
		chprintf(chp, "Full capacity:      %d mAh\r\n", word);

	ret = gg_designcapacity(driver, &word);
	if (ret < 0)
		chprintf(chp, "Design capacity:    error 0x%x\r\n", ret);
	else
		chprintf(chp, "Design capacity:    %d mAh\r\n", word);

	ret = gg_temperature(driver, &word);
	if (ret < 0)
		chprintf(chp, "Temperature:        error 0x%x\r\n", ret);
	else
		chprintf(chp, "Temperature:        %d.%d C\r\n", word/10, word-(10*(word/10)));

	ret = gg_getcells(driver, &byte);
	if (ret < 0)
		chprintf(chp, "Cell count:         error 0x%x\r\n", ret);
	else
		chprintf(chp, "Cell count:         %d cells\r\n", byte);

	ret = gg_voltage(driver, &word);
	if (ret < 0)
		chprintf(chp, "Voltage:            error 0x%x\r\n", ret);
	else
		chprintf(chp, "Voltage:            %d mV\r\n", word);

	ret = gg_current(driver, &word);
	if (ret < 0)
		chprintf(chp, "Current:            error 0x%x\r\n", ret);
	else
		chprintf(chp, "Current:            %d mA\r\n", word);

	ret = gg_charging_current(driver, &word);
	if (ret < 0)
		chprintf(chp, "Charging current:   error 0x%x\r\n", ret);
	else
		chprintf(chp, "Charging current:   %d mA\r\n", word);

	ret = gg_charging_voltage(driver, &word);
	if (ret < 0)
		chprintf(chp, "Charging voltage:   error 0x%x\r\n", ret);
	else
		chprintf(chp, "Charging voltage:   %d mV\r\n", word);

	ret = gg_average_current(driver, &word);
	if (ret < 0)
		chprintf(chp, "Avg current:        error 0x%x\r\n", ret);
	else
		chprintf(chp, "Avg current:        %d mA\r\n", word);

	for (cell=1; cell<=4; cell++) {
		ret = gg_cellvoltage(driver, cell, &word);
		if (ret < 0)
			chprintf(chp, "Cell %d voltage:     error 0x%x\r\n",
					cell, ret);
		else
			chprintf(chp, "Cell %d voltage:     %d mV\r\n",
					cell, word);
	}

	chprintf(chp, "Alarms:\r\n");
	ret = gg_getstatus(driver, stats);
	uint8_t tmp = stats[0];
	stats[0] = stats[1];
	stats[1] = tmp;
	if (stats[0] & (1<<7))
		chprintf(chp, "    OVERCHARGED ALARM\r\n");
	if (stats[0] & (1<<6))
		chprintf(chp, "    TERMINATE CHARGE ALARM\r\n");
	if (stats[0] & (1<<4))
		chprintf(chp, "    OVER TEMP ALARM\r\n");
	if (stats[0] & (1<<3))
		chprintf(chp, "    TERMINATE DISCHARGE ALARM\r\n");
	if (stats[0] & (1<<1))
		chprintf(chp, "    REMAINING CAPACITY ALARM\r\n");
	if (stats[0] & (1<<0))
		chprintf(chp, "    REMAINING TIME ALARM\r\n");

	chprintf(chp, "Charge state:\r\n");
	if (stats[1] & (1<<7))
		chprintf(chp, "    Battery initialized\r\n");
	if (stats[1] & (1<<6))
		chprintf(chp, "    Battery discharging/relaxing\r\n");
	if (stats[1] & (1<<5))
		chprintf(chp, "    Battery fully charged\r\n");
	if (stats[1] & (1<<4))
		chprintf(chp, "    Battery fully discharged\r\n");

	if (stats[1] & 0xf)
		chprintf(chp, "STATUS ERROR CODE: 0x%x\r\n", stats[1]&0xf);
	else
		chprintf(chp, "No errors detected\r\n");

	return;
}

static void cmd_leds(BaseSequentialStream *chp, int argc, char **argv) {
	int ret;
	int state;

	if (argc == 1 && argv[0][0] == '+')
		state = 1;
	else if (argc == 1 && argv[0][0] == '-')
		state = -1;
	else
		state = 0;

	ret = gg_setleds(I2C_BUS, state);
	if (ret)
		chprintf(chp, "Unable to set LEDs: 0x%x\r\n", ret);
	else
		chprintf(chp, "LEDs set to %d\r\n", state);

	return;
}

static void cmd_gg(BaseSequentialStream *chp, int argc, char **argv) {
	int ret;

	if (argc > 0 && !_strcasecmp(argv[0], "dsg")) {
		if (argc == 2 && (argv[1][0] == '+' || argv[1][0] == '-')) {
			if (argv[1][0] == '+')
				ret = gg_forcedsg(I2C_BUS, 1);
			else
				ret = gg_forcedsg(I2C_BUS, 0);
			if (ret < 0)
				chprintf(chp, "Unable to force DSG fet: %d\r\n", ret);
			else
				chprintf(chp, "Discharge FET forced %s\r\n", (argv[1][0]=='+')>0?"on":"off");
		}
		else {
			chprintf(chp, "Usage: gg dsg +/-\r\n");
			return;
		}
	}
	else if (argc > 0 && !_strcasecmp(argv[0], "setup")) {
		uint16_t capacity;
		int cells;

		if (argc != 3) {
			chprintf(chp, "Usage: gg setup [cells] [capacity in mAh]\r\n");
			return;
		}
		cells = _strtoul(argv[1], NULL, 0);
		capacity = _strtoul(argv[2], NULL, 0);

		if (cells != 3 && cells != 4) {
			chprintf(chp, "Error: Only support 3 or 4 cells\r\n");
			return;
		}

		chprintf(chp, "Setting cell count... ");
		ret = gg_setcells(I2C_BUS, cells);
		if (ret < 0)
			chprintf(chp, "Unable to set %d cells: 0x%x\r\n",
					cells, ret);
		else
			chprintf(chp, "Set %d-cell mode\r\n", cells);

		chprintf(chp, "Setting capacity... ");
		ret = gg_setcapacity(I2C_BUS, cells, capacity);
		if (ret < 0)
			chprintf(chp, "Unable to set capacity: 0x%x\r\n", ret);
		else
			chprintf(chp, "Set capacity of %d cells to %d mAh\r\n",
					cells, capacity);
	}
	else {
		chprintf(chp,
			"Usage:\r\n"
			"gg dsg +/-            Force dsg fet on or off\r\n"
			"gg setup [3/4] [mAh]  Set up charger with [3/4] cells and a capacity of [mAh]\r\n"
			"gg cal                Calibrate battery pack\r\n"
			);
		return;
	}

}

static void cmd_setchem(BaseSequentialStream *chp, int argc, char **argv) {
	uint8_t buf[4];
	unsigned int i;
	int ret;

	if (argc != 1) {
		chprintf(chp, "Usage: setchem [chemistry]\r\n");
		return;
	}

	_memset(buf, 0, sizeof(buf));
	for (i=0; i<sizeof(buf) && argv[0][i]; i++)
		buf[i] = argv[0][i];
	ret = gg_setchem(I2C_BUS, buf);
	if (ret < 0)
		chprintf(chp, "Unable to set chemistry: 0x%x\r\n", ret);
	else
		chprintf(chp, "Updated chemistry to: %c%c%c%c\r\n",
				buf[0], buf[1], buf[2], buf[3]);
}

static void cmd_setmanuf(BaseSequentialStream *chp, int argc, char **argv) {
	uint8_t manuf[11];
	unsigned int i;
	int ret;
	if (argc != 1) {
		chprintf(chp, "Usage: setmanuf [manufacturer]\r\n");
		return;
	}

	_memset(manuf, 0, sizeof(manuf));
	for (i=0; i<sizeof(manuf) && argv[0][i]; i++)
		manuf[i] = argv[0][i];

	ret = gg_setmanuf(I2C_BUS, manuf);
	if (ret) {
		chprintf(chp, "Unable to set manufacturer: 0x%x\r\n", ret);
		return;
	}

	chprintf(chp, "Manufacturer set to: ");
	for (i=0; i<11 && manuf[i]; i++)
		chprintf(chp, "%c", manuf[i]);
	chprintf(chp, "\r\n");

	return;
}

static void cmd_chg(BaseSequentialStream *chp, int argc, char **argv) {
	int ret;

	if (argc == 0) {
		uint16_t word = 0;
		uint16_t current, voltage, input;
		chprintf(chp, "\tCurrent is measured in mA, voltage in mV\r\n");

		ret = chg_getmanuf(I2C_BUS, &word);
		if (ret < 0)
			chprintf(chp, "\tError getting manufacturer: 0x%x\r\n", ret);
		chprintf(chp, "\tChager manufacturer ID: 0x%04x\r\n", word);

		chg_getdevice(I2C_BUS, &word);
		chprintf(chp, "\tChager device ID: 0x%04x\r\n", word);

		ret = chg_get(I2C_BUS, &current, &voltage, &input);
		chprintf(chp, "Charger state: %dmA @ %dmV (input: %dmA)\r\n",
				current, voltage, input);
		chprintf(chp, "Usage: chg [current] [voltage] [[input]]\r\n");
		return;
	}

	if (argc == 1) {
		chprintf(chp, "Disabling charging\r\n");
		ret = chg_set(I2C_BUS, 0, 0, 0);
		if (ret < 0)
			chprintf(chp, "Error setting charge: %d\n", ret);
		return;
	}

	if (argc == 2 && argv[1][0] == '+') {
		chprintf(chp, "Enabling CHG_CE\r\n");
		palWritePad(GPIOA, PA12, 1);
	}
	else if (argc == 2 && argv[1][0] == '-') {
		chprintf(chp, "Disabling CHG_CE\r\n");
		palWritePad(GPIOA, PA12, 0);
	}
	else {
		uint32_t current, voltage, input;
		input = 1024; /* mA */
		current = _strtoul(argv[0], NULL, 0);
		voltage = _strtoul(argv[1], NULL, 0);
		if (argc > 2)
			input = _strtoul(argv[2], NULL, 0);

		/* Figure/check current */
		if (current > 8064) {
			chprintf(chp, "Error: That's too much current\r\n");
			return;
		}
		if (current < 128) {
			chprintf(chp, "Error: 128 mA is the minimum charge current\r\n");
			return;
		}

		/* Figure/check voltage */
		if (voltage > 19200) {
			chprintf(chp, "Error: That's an awful lot of voltage\r\n");
			return;
		}

		if (voltage < 1024) {
			chprintf(chp, "Error: Too little voltage (1024 mV min)\r\n");
			return;
		}

		/* Figure/check input current */
		if (input > 11004) {
			chprintf(chp,
			"Error: 11004 mA is the max supported input current\r\n");
			return;
		}
		if (input < 256) {
			chprintf(chp, "Error: Input current must be at least 256 mA\r\n");
			return;
		}

		chprintf(chp, "Setting charger: %dmA @ %dmV (input: %dmA)... ",
				current, voltage, input);
		ret = chg_set(I2C_BUS, current, voltage, input);
		if (ret < 0)
			chprintf(chp, "Error: 0x%x\r\n", ret);
		else
			chprintf(chp, "Ok\r\n");
	}

	return;
}

static VirtualTimer vt;
static void callTogglePower(void *arg)
{
	expchannel_t channel = ((uint32_t)arg) & 0xffff;

	/* If the button has gone high again, it was a glitch, ignore it */
	if (channel == 13 && palReadPad(GPIOA, channel))
		return;
	if (channel == 14 && palReadPad(GPIOB, channel))
		return;

	pmb_toggle_power();
}

static void pwr_callback(EXTDriver *extp, expchannel_t channel) {
	(void)extp;
	(void)channel;
	chSysLockFromIsr();
	uint32_t arg = channel;

	/* PB14 is internal button, PA13 is internal button */
	if (channel == 14)
		arg |= (palReadPad(GPIOB, channel) << 16);
	else if (channel == 13)
		arg |= (palReadPad(GPIOA, channel) << 16);

	if (!chVTIsArmedI(&vt))
		chVTSetI(&vt, MS2ST(200), callTogglePower, (void *)arg);
	chSysUnlockFromIsr();
}

static const EXTConfig extcfg = {
	{
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_DISABLED, NULL},
		{EXT_CH_MODE_FALLING_EDGE /* PA13 */
			| EXT_CH_MODE_AUTOSTART
			| EXT_MODE_GPIOA, pwr_callback},
		{EXT_CH_MODE_FALLING_EDGE /* PB14 */
			| EXT_CH_MODE_AUTOSTART
			| EXT_MODE_GPIOB, pwr_callback},
		{EXT_CH_MODE_DISABLED, NULL}
	}
};



static WORKING_AREA(shell_wa, SHELL_WA_SIZE);

/*
 * Application entry point.
 */
int main(void) {
	Thread *shell_thr = NULL;

	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device
	 *   drivers and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread
	 *   and the RTOS is active.
	 */
	halInit();
	chSysInit();
	shellInit();

	pmb_smbus_init(I2C_BUS);
	gg_init(I2C_BUS);
	chThdSleepMilliseconds(200);

	sdStart(STREAM_SERIAL, &ser_cfg);

	chprintf(STREAM, "\r\n~Resetting (Ver %d, git version %s)~\r\n",
			getVer(), gitversion);
	chThdSleepMilliseconds(10);

	/*
	 * The battery charger has its own thread, because it has a
	 * watchdog that must be updated once every 170 seconds.
	 * This thread takes care of that for us.
	 */
	chg_runthread(I2C_BUS);

	/*
	 * The overall power management board must monitor CHG_CRIT, which
	 * tells us if the battery is in a critical state.
	 * If this happens, the mainboard is powered off.
	 */
	pmb_runthread(I2C_BUS);

	/* Begin listening to GPIOs (e.g. the button) */
	extStart(&EXTD1, &extcfg);

	/* Enable ImpedenceTrack(TM), to log charge status */
	gg_setitenable(I2C_BUS);

	/* Set the battery to Primary, so we can power ourselves off it */
	gg_setprimary(I2C_BUS);

	while (1) {
		if (!shell_thr)
			shell_thr = shellCreateStatic(&shell_cfg, shell_wa,
						SHELL_WA_SIZE, NORMALPRIO);
		else if (chThdTerminated(shell_thr)) {

			/* Recovers memory of the previous shell. */
			chThdRelease(shell_thr);

			/* Triggers spawning of a new shell. */
			shell_thr = NULL;

			chprintf(STREAM, "\r\nShell exited...\r\n");
		}
		chThdSleepMilliseconds(500);
	}

	return 0;
}
