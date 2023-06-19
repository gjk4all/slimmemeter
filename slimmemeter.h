#ifndef _SLIMMEMETER_H
#define _SLIMMEMETER_H

#include <termios.h>

#define CRC_POLY 0xA001

#define PARNON 0000000
#define NSTOPB 0000000

enum ERRORS {
    E_OK,
    E_CLI_PARAM,
    E_CONF_FILE,
    E_REGEX_COMP,
    E_REGEX_EXEC,
    E_MALLOC,
    E_SERIAL_PORT,
    E_RRD,
    E_FILE_ACCESS
};

enum STATES {
    S_IDLE,
    S_DATA,
    S_CHECKSUM,
    S_READY
};

struct _baud_set {
    unsigned int speed;
    speed_t value;
};

struct _baud_set _baud_table[] = {
    {1200, B1200},
    {2400, B2400},
    {4800, B4800},
    {9600, B9600},
    {19200, B19200},
    {38400, B38400},
    {57600, B57600},
    {115200, B115200}
};

struct _CONFIGSTRUCT {
    char    *serialPortFilename;
    speed_t  serialPortSpeed;
    tcflag_t serialPortBits;
    tcflag_t serialPortParity;
    tcflag_t serialPortStopbits;
    char    *databaseDirectory;
    char    *countersFilename;
    char    *voltageFilename;
    char    *kwInOutFilename
};

typedef struct {
    double kwh_1_in;
    double kwh_2_in;
    double kwh_1_out;
    double kwh_2_out;
    long   tariff;
    double kw_in_max;
    double kw_in_avg;
    double kw_in_min;
    double kw_out_max;
    double kw_out_avg;
    double kw_out_min;
    double v_l1_max;
    double v_l1_avg;
    double v_l1_min;
    double v_l2_max;
    double v_l2_avg;
    double v_l2_min;
    double v_l3_max;
    double v_l3_avg;
    double v_l3_min;
    double i_l1_max;
    double i_l1_avg;
    double i_l1_min;
    double i_l2_max;
    double i_l2_avg;
    double i_l2_min;
    double i_l3_max;
    double i_l3_avg;
    double i_l3_min;
} elec_data;

#endif
