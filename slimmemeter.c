#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <regex.h>
#include <time.h>
#include <rrd.h>
#include <signal.h>

#include "slimmemeter.h"

int serialPort;

unsigned long timestampArray[12];
elec_data * elecDataArray[12];
double * gasDataArray[12];
int storeDataCounter = 0;
int readDataCounter = 0;
int verbose = 0;

char * str_tolower(char * s) {
    for (char * p=s; *p ; p++)
        *p = tolower(*p);

    return s;
}

speed_t get_baudrate(int baudrate) {
    speed_t value = 0;
    int arrSize = sizeof(_baud_table)/sizeof(struct _baud_set);
    
    for (int i = 0; i < arrSize; i++) {
        if (_baud_table[i].speed == baudrate)
            return _baud_table[i].value;
    }

    return 0;
}

void signal_handler(int signal) {
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;

    if (signal == SIGUSR1) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    
        if ((verbose = (verbose == 0)?1:0) == 1) {
            printf("%s - Enable verbose mode", timeStringBuffer);
        }
        else {
            printf("%s - Disable verbose mode", timeStringBuffer);
        }

        fflush(stdout);
    }
}

/*
 * CRC-16 calculation for data frame
 *
 * Uses:
 *   #define CRC_POLY 0xA001  - Polynominal for CRC-16-IBM
 *
 * Parameters:
 *   *data_p  - Pointer to the data
 *   length   - length of the data
 *
 * Returns unsigned short containing the CRC-16 value
 */
unsigned short crc_16(char *data_p) {
    int i, length;
    unsigned short crc = 0;

    length = strlen(data_p);
    if (length == 0)
        return 0;

    do {
        for (i = 0, crc ^= (unsigned short)0xff & *data_p++; i < 8; i++) {
            if (crc & 0x0001)
                crc = (crc >> 1) ^ CRC_POLY;
            else crc >>= 1;
        }
    } while (--length);

    return crc;
}

int read_config(struct _CONFIGSTRUCT *config, char *configFilename) {
    regex_t reEmptyLine;
    regex_t reCommentedOut;
    regex_t reKeyValuePair;
    regmatch_t pmatch[4];
    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;
    int reError;
    int lineNumber = 0;
    char key[32];
    char value[256];
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;
 
    // Compile regular expressions
    if ((reError = regcomp(&reEmptyLine, "^\\s*$", REG_NOSUB | REG_EXTENDED)) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        regerror(reError, &reEmptyLine, value, 256);
        fprintf(stderr, "%s - RE compile of \"^\\s*$\" failed: %s\n", "^\\s*$", timeStringBuffer, value);
        return E_REGEX_COMP;
    }
    if ((reError = regcomp(&reCommentedOut, "^\\s*[;#]", REG_NOSUB | REG_EXTENDED)) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        regerror(reError, &reCommentedOut, value, 256);
        fprintf(stderr, "%s - RE compile of \"^\\s*[;#]\" failed: %s\n", "^\\s*$", timeStringBuffer, value);
        return E_REGEX_COMP;
    }
    if ((reError = regcomp(&reKeyValuePair, "^\\s*(\\S+)\\s*[=:]\\s*(.+?)\\s*$", REG_NEWLINE | REG_EXTENDED)) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        regerror(reError, &reKeyValuePair, value, 256);
        fprintf(stderr, "%s - RE compile of \"^\\s*(\\S+)\\s*[=:]\\s*(.+?)\\s*$\" failed: %s\n", "^\\s*$", timeStringBuffer, value);
        return E_REGEX_COMP;
    }

    // Open config filew
    fp = fopen(configFilename, "r");
    if (fp == NULL) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error %i from open configfile %s: %s\n", timeStringBuffer, errno, configFilename, strerror(errno));
        return E_CONF_FILE;
    }

    // Read the configfile
    while ((read = getline(&line, &len, fp)) != -1) {
        lineNumber++;

        // Skip empty lines
        if (regexec(&reEmptyLine, line, 0, NULL, 0) == 0)
            continue;
        // Skip commented lines
        if (regexec(&reCommentedOut, line, 0, NULL, 0) == 0)
            continue;
        // Exit if the line doesn't match key value syntax
        if ((reError = regexec(&reKeyValuePair, line, 4, pmatch, 0)) != 0) {
            regerror(reError, &reKeyValuePair, value, 256);
            line[strlen(line) - 1] = '\0';
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Syntax error (%s) on line %d in file \"%s\": \"%s\"\n", timeStringBuffer, value, lineNumber, configFilename, line);
            return E_CONF_FILE;
        }

        // Get key/value from input
        memcpy(key, &line[pmatch[1].rm_so], pmatch[1].rm_eo - pmatch[1].rm_so);
        key[pmatch[1].rm_eo - pmatch[1].rm_so] = '\0';
        str_tolower(key);

        memcpy(value, &line[pmatch[2].rm_so], pmatch[2].rm_eo - pmatch[2].rm_so);
        value[pmatch[2].rm_eo - pmatch[2].rm_so] = '\0';

        if (strcmp(key, "device") == 0) {
            if (config->serialPortFilename != NULL)
                free(config->serialPortFilename);
            if ((config->serialPortFilename = (char *)malloc(strlen(value) + 1)) == NULL) {
                msgtime = time(NULL);
                tm_info = localtime(&msgtime);
                strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(stderr, "%s - Error claiming memory for serial device name: %s\n", timeStringBuffer, strerror(errno));
                return E_MALLOC;
            }
            strcpy(config->serialPortFilename, value);
            continue;
        }
        if ((strcmp(key, "speed") == 0) || (strcmp(key, "baud") == 0)) {
            if ((config->serialPortSpeed = get_baudrate(atoi(value))) == 0) {
                msgtime = time(NULL);
                tm_info = localtime(&msgtime);
                strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(stderr, "%s - Invalid baudrate value: %s\n", timeStringBuffer, value);
                return E_CONF_FILE;
            }
            continue;
        }
        if (strcmp(key, "parity") == 0) {
            str_tolower(value);

            if ((strcmp(value, "n") == 0) || (strcmp(value, "none") == 0))
                config->serialPortParity = PARNON;
            else if ((strcmp(value, "e") == 0) || (strcmp(value, "even") == 0))
                config->serialPortParity = PARENB;
            else if ((strcmp(value, "o") == 0) || (strcmp(value, "odd") == 0))
                config->serialPortParity = PARENB | PARODD;
            else {
                msgtime = time(NULL);
                tm_info = localtime(&msgtime);
                strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(stderr, "%s - Invalid parity: %s\n", timeStringBuffer, value);
                return E_CONF_FILE;
            }
            continue;
        }
        if ((strcmp(key, "bits") == 0) || (strcmp(key, "databits") == 0)) {
            int bits = atoi(value);

            if ((bits < 5) || (bits > 8)) {
                msgtime = time(NULL);
                tm_info = localtime(&msgtime);
                strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(stderr, "%s - Invalid number of bits: %s\n", timeStringBuffer, value);
                return E_CONF_FILE;
            }
            config->serialPortBits = ((bits - 5) << 4);
            continue;
        }
        if (strcmp(key, "stopbits") == 0) {
            int stopbits = atoi(value);

            if ((stopbits < 1) || (stopbits > 2)) {
                msgtime = time(NULL);
                tm_info = localtime(&msgtime);
                strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(stderr, "%s - Invalid number of stopbits: %s\n", timeStringBuffer, value);
                return E_CONF_FILE;
            }
            config->serialPortStopbits = ((stopbits - 1) << 6);
            continue;
        }
        if (strcmp(key, "db-directory") == 0) {
            if (config->databaseDirectory != NULL)
                free(config->databaseDirectory);
            if ((config->databaseDirectory = (char *)malloc(strlen(value) + 1)) == NULL) {
                msgtime = time(NULL);
                tm_info = localtime(&msgtime);
                strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                fprintf(stderr, "%s - Error claiming memory for database directory name: %s\n", timeStringBuffer, strerror(errno));
                return E_MALLOC;
            }
            strcpy(config->databaseDirectory, value);
            continue;
        }
        
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error, unknown key \"%s\" on line %d in config file \"%s\"\n", timeStringBuffer, key, lineNumber, configFilename);
        return E_CONF_FILE;
    }

    fclose(fp);
    if (line)
        free(line);

    return 0;
}

int init_serial(struct _CONFIGSTRUCT * config) {
    struct termios tty;
    int localSerialPort = open(config->serialPortFilename, O_RDWR);
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;

    if (localSerialPort < 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error %i from open serialport %s: %s\n", timeStringBuffer, errno, config->serialPortFilename, strerror(errno));
        return -1;
    }

    if (tcgetattr(localSerialPort, &tty) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error %i from tcgetattr: %s\n", timeStringBuffer, errno, strerror(errno));
        return -2;
    }

    tty.c_cflag &= ~PARENB;         // No parity
    tty.c_cflag |= config->serialPortParity;
    tty.c_cflag &= ~CSTOPB;         // 1 stopbit
    tty.c_cflag |= config->serialPortStopbits;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= config->serialPortBits;             // 8 bits
    tty.c_cflag &= ~CRTSCTS;        // No hw flow control
    tty.c_cflag |= CREAD | CLOCAL;

    tty.c_lflag &= ~ICANON;
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ISIG;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);

    tty.c_oflag &= ~OPOST;
    tty.c_oflag &= ~ONLCR;

    cfsetispeed(&tty, config->serialPortSpeed);
    cfsetospeed(&tty, config->serialPortSpeed);

    if (tcsetattr(localSerialPort, TCSANOW, &tty) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error %i from tcsetattr: %s\n", timeStringBuffer, errno, strerror(errno));
        return -3;
    }

    return localSerialPort;
}

int init_rrd_database(struct _CONFIGSTRUCT *config) {
    // Check counter databases
    int result;
    int baseLength;
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;

    const char *createCounterDB[] = {
        "DS:KWh_1_in:DCOUNTER:900:0.0:99999.0",
        "DS:KWh_2_in:DCOUNTER:900:0.0:99999.0",
        "DS:KWh_1_out:DCOUNTER:900:0.0:99999.0",
        "DS:KWh_2_out:DCOUNTER:900:0.0:99999.0",
        "DS:gas_in:DCOUNTER:900:0.0:99999.0",
        "RRA:LAST:0.5:1:800",
        "RRA:AVERAGE:0.5:6:800",
        "RRA:AVERAGE:0.5:24:800",
        "RRA:AVERAGE:0.5:288:800",
        "RRA:MAX:0.5:6:800",
        "RRA:MAX:0.5:24:800",
        "RRA:MAX:0.5:288:800",
        "RRA:MIN:0.5:6:800",
        "RRA:MIN:0.5:24:800",
        "RRA:MIN:0.5:288:800",
        NULL
    };
    const char *createVoltageDB[] = {
        "DS:V_max:GAUGE:900:0.0:999.0",
        "DS:V_avg:GAUGE:900:0.0:999.0",
        "DS:V_min:GAUGE:900:0.0:999.0",
        "RRA:LAST:0.5:1:800",
        "RRA:AVERAGE:0.5:6:800",
        "RRA:AVERAGE:0.5:24:800",
        "RRA:AVERAGE:0.5:288:800",
        "RRA:MAX:0.5:6:800",
        "RRA:MAX:0.5:24:800",
        "RRA:MAX:0.5:288:800",
        "RRA:MIN:0.5:6:800",
        "RRA:MIN:0.5:24:800",
        "RRA:MIN:0.5:288:800",
        NULL
    };
    const char *createKwinoutDB[] = {
        "DS:KW_max_in:GAUGE:900:0.0:999.0",
        "DS:KW_avg_in:GAUGE:900:0.0:999.0",
        "DS:KW_min_in:GAUGE:900:0.0:999.0",
        "DS:KW_max_out:GAUGE:900:0.0:999.0",
        "DS:KW_avg_out:GAUGE:900:0.0:999.0",
        "DS:KW_min_out:GAUGE:900:0.0:999.0",
        "RRA:LAST:0.5:1:800",
        "RRA:AVERAGE:0.5:6:800",
        "RRA:AVERAGE:0.5:24:800",
        "RRA:AVERAGE:0.5:288:800",
        "RRA:MAX:0.5:6:800",
        "RRA:MAX:0.5:24:800",
        "RRA:MAX:0.5:288:800",
        "RRA:MIN:0.5:6:800",
        "RRA:MIN:0.5:24:800",
        "RRA:MIN:0.5:288:800",
        NULL
    };

    baseLength = strlen(config->databaseDirectory) + 15;

    if (access(config->databaseDirectory, R_OK | W_OK | X_OK) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Cannot write in directory %s\n", timeStringBuffer, config->databaseDirectory);
        return E_FILE_ACCESS;
    }

    if ((config->countersFilename = (char *)malloc(baseLength)) == NULL) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error claiming memory for counters filename name: %s\n", timeStringBuffer, strerror(errno));
        return E_MALLOC;
    }
    strcpy(config->countersFilename, config->databaseDirectory);
    strcat(config->countersFilename, "/counters.rrd");

    if (access(config->countersFilename, F_OK) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        printf("%s - Create counters database file %s\n", timeStringBuffer, config->countersFilename);
        fflush(stdout);
        rrd_clear_error();
        result = rrd_create_r(config->countersFilename, 300, 0, 15, createCounterDB);

        if (rrd_test_error()) {
            fprintf(stderr, "%s - RRD create error: %s\n", timeStringBuffer, rrd_get_error());
            return E_RRD;
        }
    }

    if ((config->voltageFilename = (char *)malloc(baseLength)) == NULL) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error claiming memory for voltage filename name: %s\n", timeStringBuffer, strerror(errno));
        return E_MALLOC;
    }
    strcpy(config->voltageFilename, config->databaseDirectory);
    strcat(config->voltageFilename, "/voltage.rrd");

    if (access(config->voltageFilename, F_OK) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        printf("%s - Create voltage database file %s\n", timeStringBuffer, config->voltageFilename);
        fflush(stdout);
        rrd_clear_error();
        result = rrd_create_r(config->voltageFilename, 300, 0, 13, createVoltageDB);

        if (rrd_test_error()) {
            fprintf(stderr, "%s - RRD create error: %s\n", timeStringBuffer, rrd_get_error());
            return E_RRD;
        }
    }

    if ((config->kwInOutFilename = (char *)malloc(baseLength)) == NULL) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error claiming memory for database directory name: %s\n", timeStringBuffer, strerror(errno));
        return E_MALLOC;
    }
    strcpy(config->kwInOutFilename, config->databaseDirectory);
    strcat(config->kwInOutFilename, "/kwinout.rrd");

    if (access(config->kwInOutFilename, F_OK) != 0) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        printf("%s - Create kw database file %s\n\n", timeStringBuffer, config->kwInOutFilename);
        fflush(stdout);
        rrd_clear_error();
        result = rrd_create_r(config->kwInOutFilename, 300, 0, 16, createKwinoutDB);

        if (rrd_test_error()) {
            fprintf(stderr, "%s - RRD create error: %s\n", timeStringBuffer, rrd_get_error());
            return E_RRD;
        }
    }

    return E_OK;
}

void init_arrays() {
    for (int i = 0; i < 10; i++) {
        timestampArray[i] = 0;
        elecDataArray[i] = NULL;
        gasDataArray[i] = NULL;
    }
}

int update_rrd_database(struct _CONFIGSTRUCT *config) {
    char values[128];
    int result;
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;
    const char *updateCounters[] = {
        values,
        NULL
    };

    sprintf(values, "%ld:%1.3lf:%1.3lf:%1.3lf:%1.3lf:%1.3lf", timestampArray[readDataCounter] + 300, elecDataArray[readDataCounter]->kwh_1_in, elecDataArray[readDataCounter]->kwh_2_in, elecDataArray[readDataCounter]->kwh_1_out, elecDataArray[readDataCounter]->kwh_2_out, *gasDataArray[readDataCounter]);

    rrd_clear_error();
    result = rrd_update_r(config->countersFilename, NULL, 1, updateCounters);

    if (rrd_test_error()) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - RRD error in file %s: %s\n", timeStringBuffer, config->countersFilename, rrd_get_error());
        return E_RRD;
    }

    sprintf(values, "%ld:%1.3lf:%1.3lf:%1.3lf", timestampArray[readDataCounter] + 300, elecDataArray[readDataCounter]->v_l1_max, elecDataArray[readDataCounter]->v_l1_avg, elecDataArray[readDataCounter]->v_l1_min);

    rrd_clear_error();
    result = rrd_update_r(config->voltageFilename, NULL, 1, updateCounters);

    if (rrd_test_error()) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - RRD error in file %s: %s\n", timeStringBuffer, config->voltageFilename, rrd_get_error());
        return E_RRD;
    }

    sprintf(values, "%ld:%1.3lf:%1.3lf:%1.3lf:%1.3lf:%1.3lf:%1.3lf", timestampArray[readDataCounter] + 300, elecDataArray[readDataCounter]->kw_in_max, elecDataArray[readDataCounter]->kw_in_avg, elecDataArray[readDataCounter]->kw_in_min, elecDataArray[readDataCounter]->kw_out_max, elecDataArray[readDataCounter]->kw_out_avg, elecDataArray[readDataCounter]->kw_out_min);

    rrd_clear_error();
    result = rrd_update_r(config->kwInOutFilename, NULL, 1, updateCounters);

    if (rrd_test_error()) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - RRD error in file %s: %s\n", timeStringBuffer, config->kwInOutFilename, rrd_get_error());
        return E_RRD;
    }

    timestampArray[readDataCounter] = 0;
    free(elecDataArray[readDataCounter]);
    elecDataArray[readDataCounter] = NULL;
    free(gasDataArray[readDataCounter]);
    gasDataArray[readDataCounter] = NULL;

    if (readDataCounter != storeDataCounter) {
        readDataCounter++;
        if (readDataCounter >= 10)
            readDataCounter = 0;
    }

    return E_OK;
}

int print_data(struct _CONFIGSTRUCT *config) {
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;
    int result = 0;

    tm_info = localtime(&timestampArray[readDataCounter]);
    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);
    printf("-------------------------------------------------------\n");
    printf("Report time : %s\n", timeStringBuffer);
    printf("-------------------------------------------------------\n");
    printf("Tariff group              1               2\n");
    printf("Energy consumed   : %10.3lf KWh  %10.3lf KWh\n", elecDataArray[readDataCounter]->kwh_1_in, elecDataArray[readDataCounter]->kwh_2_in);
    printf("Energt delivered  : %10.3lf KWh  %10.3lf KWh\n", elecDataArray[readDataCounter]->kwh_1_out, elecDataArray[readDataCounter]->kwh_2_out);
    printf("                      min         avg         max\n");
    printf("Power consumption : %7.3lf KW  %7.3lf KW  %7.3lf KW\n", elecDataArray[readDataCounter]->kw_in_min, elecDataArray[readDataCounter]->kw_in_avg, elecDataArray[readDataCounter]->kw_in_max);
    printf("Power delivery    : %7.3lf KW  %7.3lf KW  %7.3lf KW\n", elecDataArray[readDataCounter]->kw_out_min, elecDataArray[readDataCounter]->kw_out_avg, elecDataArray[readDataCounter]->kw_out_max);
    printf("L1 voltage drift  : %7.3lf V   %7.3lf V   %7.3lf V\n", elecDataArray[readDataCounter]->v_l1_min, elecDataArray[readDataCounter]->v_l1_avg, elecDataArray[readDataCounter]->v_l1_max);
    printf("L1 Currents       : %7.3lf A   %7.3lf A   %7.3lf A\n", elecDataArray[readDataCounter]->i_l1_min, elecDataArray[readDataCounter]->i_l1_avg, elecDataArray[readDataCounter]->i_l1_max);
    printf("\n");
    printf("Gas consumpion    : %10.3lf m3\n", *gasDataArray[readDataCounter]);
    printf("\n");
    fflush(stdout);

    return result;
}

int store_data(unsigned long timestamp, elec_data * eCummPointer, double * gCummPointer, int counter) {

    if (counter == 0)
        return 0;
    
    // Calculate averages
    eCummPointer->kw_in_avg /= counter;
    eCummPointer->kw_out_avg /= counter;
    eCummPointer->v_l1_avg /= counter;
    eCummPointer->v_l2_avg /= counter;
    eCummPointer->v_l3_avg /= counter;
    eCummPointer->i_l1_avg /= counter;
    eCummPointer->i_l2_avg /= counter;
    eCummPointer->i_l3_avg /= counter;

    timestampArray[storeDataCounter] = timestamp;
    elecDataArray[storeDataCounter] = eCummPointer;
    gasDataArray[storeDataCounter] = gCummPointer;
    storeDataCounter++;

    if (storeDataCounter >= 10) {
        storeDataCounter = 0;
        if (storeDataCounter == readDataCounter) {
            readDataCounter++;
            if (readDataCounter >= 10)
                readDataCounter = 0;

            timestampArray[readDataCounter] = 0;
            free (elecDataArray[readDataCounter]);
            free (gasDataArray[readDataCounter]);
        }
    }

    return 0;
}

int parse_block(char * dataPointer) {
    static regex_t * reElecPointer = NULL;
    static regex_t * reGasPointer = NULL;
    static int counter = 0;
    static elec_data * eCummPointer = NULL;
    static double * gCummPointer = NULL;
    static unsigned long lastMeasureTime = 0;

    regmatch_t matchPointer[5];
    double tempValue;
    unsigned long currentMeasureTime;
    char error[80];
    char key[15];
    char value[15];
    char * currLinePointer;
    char * nextLinePointer;
    int reError;
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;

    if (reElecPointer == NULL) {
        reElecPointer = (regex_t *)malloc(sizeof(regex_t));
        if ((reError = regcomp(reElecPointer, "^([^(]+)\\(([0-9.]{1,10})\\*.*\\)", REG_EXTENDED)) != 0) {
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            regerror(reError, reElecPointer, error, 80);
            fprintf(stderr, "%s - ERROR RE compile of \"^([^(]+)\\(([0-9.]{1,10})\\*.*\\)\" failed: %s\n", timeStringBuffer, error);
            return E_REGEX_COMP;
        }
    }

    if (reGasPointer == NULL) {
        reGasPointer = (regex_t *)malloc(sizeof(regex_t));
        if ((reError = regcomp(reGasPointer, "^([^(]+)\\([^(]+\\)\\(([0-9.]{1,10})\\*.*\\)", REG_EXTENDED)) != 0) {
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            regerror(reError, reGasPointer, error, 80);
            fprintf(stderr, "%s - ERROR RE compile of \"^([^(]+)\\([^(]+\\)\\(([0-9.]{1,10})\\*.*\\)\" failed: %s\n", timeStringBuffer, error);
            return E_REGEX_COMP;
        }
    }

    if ((nextLinePointer = dataPointer) == NULL) {
        // No data to parse.
        return 0;
    }

    currentMeasureTime = (unsigned long)time(NULL);

    if (lastMeasureTime == 0) {
        lastMeasureTime = currentMeasureTime / 300;
    }

    if ((currentMeasureTime / 300) != lastMeasureTime) {
        printf("\n");
        store_data(lastMeasureTime * 300, eCummPointer, gCummPointer, counter);
        counter = 0;
        lastMeasureTime = currentMeasureTime / 300;
    }

    if (counter == 0) {
        if ((eCummPointer = (elec_data *)malloc(sizeof(elec_data))) == NULL) {
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Error, could not allocate %d bytes of memory!", timeStringBuffer, sizeof(elec_data));
            return E_MALLOC;
        }
        memset(eCummPointer, '\0', sizeof(elec_data));

        if ((gCummPointer = (double *)malloc(sizeof(double))) == NULL) {
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Error, could not allocate %d bytes of memory!", timeStringBuffer, sizeof(double));
            return E_MALLOC;
        }
    }

    while ((currLinePointer = nextLinePointer)) {
        if ((nextLinePointer = strchr(currLinePointer, '\n')) == NULL) {
            break;
        }


        *nextLinePointer = '\0';
        nextLinePointer++;

        // Skip header
        if ((*currLinePointer == '/') || (*currLinePointer == '\r')){
            continue;
        }

        // End of datablock reached
        if (*currLinePointer == '!')
            break;

        // Parse line
        if (regexec(reElecPointer, currLinePointer, 4, matchPointer, 0) != 0) {
            if (regexec(reGasPointer, currLinePointer, 4, matchPointer, 0) != 0) {
                continue;
            }
        }

        // Get key/value from input
        if ((matchPointer[1].rm_eo - matchPointer[1].rm_so) >=  sizeof(key)) {
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Error, size of match > key!", timeStringBuffer);
            return E_REGEX_EXEC;
        }
        memcpy(key, &currLinePointer[matchPointer[1].rm_so], matchPointer[1].rm_eo - matchPointer[1].rm_so);
        key[matchPointer[1].rm_eo - matchPointer[1].rm_so] = '\0';


        if ((matchPointer[2].rm_eo - matchPointer[2].rm_so) >=  sizeof(value)) {
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Error, size of match > value!", timeStringBuffer);
            return E_REGEX_EXEC;
        }
        memcpy(value, &currLinePointer[matchPointer[2].rm_so], matchPointer[2].rm_eo - matchPointer[2].rm_so);
        value[matchPointer[2].rm_eo - matchPointer[2].rm_so] = '\0';

        if (strcmp(key, "1-0:1.8.1") == 0)  {
            // Usage KWh tariff 1
            eCummPointer->kwh_1_in = atof(value);
            continue;
        }

        if (strcmp(key, "1-0:1.8.2") == 0)  {
            // Usage KWh tariff 2
            eCummPointer->kwh_2_in = atof(value);
            continue;
        }
        
        if (strcmp(key, "1-0:2.8.1") == 0)  {
            // Delivery KWh tariff 1
            eCummPointer->kwh_1_out = atof(value);
            continue;
        }
        
        if (strcmp(key, "1-0:2.8.2") == 0)  {
            // Delivery KWh tariff 2
            eCummPointer->kwh_2_out = atof(value);
            continue;
        }
        
        if (strcmp(key, "1-0:1.7.0") == 0)  {
            // Actual power usage KW
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->kw_in_min)) {
                eCummPointer->kw_in_min = tempValue;
            }
            eCummPointer->kw_in_avg += tempValue;
            if (tempValue > eCummPointer->kw_in_max) {
                eCummPointer->kw_in_max = tempValue;
            }
        }
        
        if (strcmp(key, "1-0:2.7.0") == 0)  {
            // Actual power delivery KW
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->kw_out_min)) {
                eCummPointer->kw_out_min = tempValue;
            }
            eCummPointer->kw_out_avg += tempValue;
            if (tempValue > eCummPointer->kw_out_max) {
                eCummPointer->kw_out_max = tempValue;
            }
        }
        
        if (strcmp(key, "1-0:31.7.0") == 0)  {
            // Actual current L1 in A
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->i_l1_min)) {
                eCummPointer->i_l1_min = tempValue;
            }
            eCummPointer->i_l1_avg += tempValue;
            if (tempValue > eCummPointer->i_l1_max) {
                eCummPointer->i_l1_max = tempValue;
            }
        }
        
        if (strcmp(key, "1-0:32.7.0") == 0)  {
            // Actual voltage L1 in V
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->v_l1_min)) {
                eCummPointer->v_l1_min = tempValue;
            }
            eCummPointer->v_l1_avg += tempValue;
            if (tempValue > eCummPointer->v_l1_max) {
                eCummPointer->v_l1_max = tempValue;
            }
        }

        if (strcmp(key, "1-0:51.7.0") == 0)  {
            // Actual current L1 in A
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->i_l2_min)) {
                eCummPointer->i_l2_min = tempValue;
            }
            eCummPointer->i_l2_avg += tempValue;
            if (tempValue > eCummPointer->i_l2_max) {
                eCummPointer->i_l2_max = tempValue;
            }
        }

        if (strcmp(key, "1-0:52.7.0") == 0)  {
            // Actual voltage L1 in V
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->v_l2_min)) {
                eCummPointer->v_l2_min = tempValue;
            }
            eCummPointer->v_l2_avg += tempValue;
            if (tempValue > eCummPointer->v_l2_max) {
                eCummPointer->v_l2_max = tempValue;
            }
        }

        if (strcmp(key, "1-0:71.7.0") == 0)  {
            // Actual current L1 in A
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->i_l3_min)) {
                eCummPointer->i_l3_min = tempValue;
            }
            eCummPointer->i_l3_avg += tempValue;
            if (tempValue > eCummPointer->i_l3_max) {
                eCummPointer->i_l3_max = tempValue;
            }
        }
        
        if (strcmp(key, "1-0:72.7.0") == 0)  {
            // Actual voltage L1 in V
            tempValue = atof(value);
            if ((counter == 0) || (tempValue < eCummPointer->v_l3_min)) {
                eCummPointer->v_l3_min = tempValue;
            }
            eCummPointer->v_l3_avg += tempValue;
            if (tempValue > eCummPointer->v_l3_max) {
                eCummPointer->v_l3_max = tempValue;
            }
        }
        
        if (strcmp(key, "1-0:21.7.0") == 0)  {
            // Actual power usage L1 in KW
        }

        if (strcmp(key, "1-0:22.7.0") == 0)  {
            // Actual power delivery L1 in KW
        }
        
        if (strcmp(key, "1-0:41.7.0") == 0)  {
            // Actual power usage L1 in KW
        }

        if (strcmp(key, "1-0:42.7.0") == 0)  {
            // Actual power delivery L1 in KW
        }
        
        if (strcmp(key, "1-0:61.7.0") == 0)  {
            // Actual power usage L1 in KW
        }

        if (strcmp(key, "1-0:62.7.0") == 0)  {
            // Actual power delivery L1 in KW
        }

        if (strcmp(key, "0-1:24.2.1") == 0)  {
            // Gas delivery m3
            *gCummPointer = atof(value);
        }
    }

    counter++;
    return 0;
}

void help_message(char * name) {
    printf("Usage: %s [OPTIONS]\n\nOptions:\n  -c|--config <configfile>\n  -d|--device <serialdevice>\n  -s|--speed <serialspeed>\n  -p|--parity <parity>\n  -b|--bits <databits>\n  -t|--stopbits <stopbits>\n  --dbdir|--db-directory <Directory to store databases>\n\n", name);
    fflush(stdout);
}

int main(int argc, char **argv) {
    struct _CONFIGSTRUCT config;
    char defaultConfigFile[] = "/etc/slimmemeter.conf";

    char checksumStr[5];
    char iobuffer[8192];
    char dataBlock[2048];
    char buffer;
    char * configFile = NULL;
    char * dataPointer = dataBlock;
    char * checksumPointer;
    char * defaultDirectory;
    int result;
    int status = S_IDLE;
    int numchars;
    int rrdErrors = 0;
    unsigned short crc;
    char timeStringBuffer[26];
    struct tm * tm_info;
    time_t msgtime;

    // Catch SIGUSR1
    signal(SIGUSR1, signal_handler);

    // Prepare config to defaults
    if ((config.databaseDirectory = (char *)malloc(sizeof(char) * 2)) == NULL) {
        msgtime = time(NULL);
        tm_info = localtime(&msgtime);
        strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

        fprintf(stderr, "%s - Error claiming memory for database directory name: %s\n", timeStringBuffer, strerror(errno));
        return E_MALLOC;
    }

    config.serialPortFilename = NULL;
    config.serialPortSpeed = B115200;
    config.serialPortBits = CS8;
    config.serialPortParity = PARNON;
    config.serialPortStopbits = NSTOPB;
    strcpy(config.databaseDirectory, ".");
    config.countersFilename = NULL;
    config.voltageFilename = NULL;
    config.kwInOutFilename = NULL;

    // Check cmdline parameters for configfile
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if ((strcmp(argv[i], "-c") == 0) || (strcmp(argv[i], "--config") == 0)) {
                configFile = argv[i + 1];
                break;
            }
        }
    }

    // Set configfile to default if not defined
    if (configFile == NULL)
        configFile = defaultConfigFile;

    // Read configfile
    if ((result = read_config(&config, configFile)) != 0)
        return result;

    // Check cmdline parameters for configfile
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if ((strcmp(argv[i], "-h") == 0) || (strcmp(argv[i], "--help") == 0)) {
                help_message(argv[0]);
                return 0;
            }
            if ((strcmp(argv[i], "-c") == 0) || (strcmp(argv[i], "--config") == 0)) {
                // Skip, already read
                i++;
                continue;
            }
            if ((strcmp(argv[i], "-d") == 0) || (strcmp(argv[i], "--device") == 0)) {
                i++;
                if (config.serialPortFilename != NULL)
                    free(config.serialPortFilename);
                if ((config.serialPortFilename = (char *)malloc(strlen(argv[i]) + 1)) == NULL) {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - Error claiming memory for serial device name: %s\n", timeStringBuffer, strerror(errno));
                    return E_MALLOC;
                }
                strcpy(config.serialPortFilename, argv[i]);
                continue;
            }
            if ((strcmp(argv[i], "-s") == 0) || (strcmp(argv[i], "--speed") == 0)) {
                if ((config.serialPortSpeed = get_baudrate(atoi(argv[++i]))) == 0) {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - Invalid baudrate value: %s\n", timeStringBuffer, argv[i]);
                    return E_CLI_PARAM;
                }
                continue;
            }
            if ((strcmp(argv[i], "-p") == 0) || (strcmp(argv[i], "--parity") == 0)) {
                char * val = str_tolower(argv[++i]);

                if ((strcmp(val, "n") == 0) || (strcmp(val, "none") == 0))
                    config.serialPortParity = PARNON;
                else if ((strcmp(val, "e") == 0) || (strcmp(val, "even") == 0))
                    config.serialPortParity = PARENB;
                else if ((strcmp(val, "o") == 0) || (strcmp(val, "odd") == 0))
                    config.serialPortParity = PARENB | PARODD;
                else {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - Invalid parity: %s\n", timeStringBuffer, argv[i]);
                    return E_CLI_PARAM;
                }
                continue;
            }
            if ((strcmp(argv[i], "-b") == 0) || (strcmp(argv[i], "--bits") == 0)) {
                int bits = atoi(argv[++i]);
                if ((bits < 5) || (bits > 8)) {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - Invalid number of bits: %s\n", timeStringBuffer, argv[i]);
                    return E_CLI_PARAM;
                }
                config.serialPortBits = ((bits - 5) << 4);
                continue;
            }
            if ((strcmp(argv[i], "-t") == 0) || (strcmp(argv[i], "--stopbits") == 0)) {
                int stopbits = atoi(argv[++i]);
                if ((stopbits < 1) || (stopbits > 2)) {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - Invalid number of stopbits: %s\n", timeStringBuffer, argv[i]);
                    return E_CLI_PARAM;
                }
                config.serialPortStopbits = ((stopbits - 1) << 6);
                continue;
            }
            if ((strcmp(argv[i], "--dbdir") == 0) || (strcmp(argv[i], "--db-directory") == 0)) {
                if ((config.databaseDirectory = (char *)malloc(sizeof(char) * (strlen(argv[++i]) + 1))) == NULL) {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - Error claiming memory for database directory name: %s\n", timeStringBuffer, strerror(errno));
                    return E_MALLOC;
                }
                strcpy(config.databaseDirectory, argv[i]);
                continue;
            }
            if ((strcmp(argv[i], "-v") == 0) || (strcmp(argv[i], "--verbose") == 0)) {
                verbose = 1;
                continue;
            }

            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Unknown option \"%s\"\n\n", timeStringBuffer, argv[i]);
            help_message(argv[0]);
            return E_CLI_PARAM;
        }
    }

    printf("Configuration\nConfigfile: \"%s\"\nSerialPort: \"%s\"\nSpeed: %07o\nBits: %07o\nParity: %07o\nStopbits: %07o\nDatabase directory: %s\n\n", configFile, config.serialPortFilename, config.serialPortSpeed, config.serialPortBits, config.serialPortParity, config.serialPortStopbits, config.databaseDirectory);
    fflush(stdout);

    if ((serialPort = init_serial(&config)) < 0) {
        return E_SERIAL_PORT;
    }

    if (init_rrd_database(&config) != E_OK) {
        return E_RRD;
    }

    init_arrays();

    while (1) {
        result = read(serialPort, iobuffer, 8192);
        if (result == 0) {
            // End of file
            break;
        }
        if (result == -1) {
            // Error condition
            msgtime = time(NULL);
            tm_info = localtime(&msgtime);
            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

            fprintf(stderr, "%s - Error while reading serial port \"%s\": %s\n", timeStringBuffer, config.serialPortFilename, strerror(errno));
            return E_SERIAL_PORT;
        }
        for (int index = 0; index < result; index++){
            buffer= iobuffer[index];



            switch (status) {
            case S_IDLE:
                if (buffer != '/')
                    break;

                dataPointer = dataBlock;
                status = S_DATA;
            case S_DATA:
                if(dataPointer>=dataBlock+sizeof (dataBlock)){
                    status = S_IDLE;
                    break;
                }

                *dataPointer = buffer;
                dataPointer++;

                if (buffer == '!') {
                    *dataPointer = '\0';
                    checksumPointer = checksumStr;
                    status = S_CHECKSUM;
                    numchars = 4;
                }

                break;
            case S_CHECKSUM:
                if (numchars-- > 0) {
                    *checksumPointer++ = buffer;
                }
                else {
                    *checksumPointer = '\0';
                    status = S_READY;
                }

                if (buffer == '\n') {
                    *checksumPointer = '\0';
                    status = S_READY;
                }

                break;
            case S_READY:
                crc = crc_16(dataBlock);
                status = S_IDLE;

                if (crc != (unsigned short)strtol(checksumStr, NULL, 16)) {
                    msgtime = time(NULL);
                    tm_info = localtime(&msgtime);
                    strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                    fprintf(stderr, "%s - CRC error in datagram\n", timeStringBuffer);
                    break;
                }

                if ((result = parse_block(dataBlock)) != E_OK) {
                    goto EXIT;
                }

                while (timestampArray[readDataCounter] != 0) {
                    if (verbose != 0)
                        print_data(NULL);

                    if ((result = update_rrd_database(&config)) != E_OK) {
                        rrdErrors++;

                        if (rrdErrors >= 9) {
                            msgtime = time(NULL);
                            tm_info = localtime(&msgtime);
                            strftime(timeStringBuffer, 26, "%Y-%m-%d %H:%M:%S", tm_info);

                            fprintf(stderr, "%s - To many errors updating RRD files", timeStringBuffer);
                            goto EXIT;
                        }
                    }
                    else {
                        rrdErrors = 0;
                    }
                }

                break;
            default:
                dataPointer = dataBlock;
                status = S_IDLE;

            }
        }
    }

EXIT:
    close(serialPort);

    return result;
}
