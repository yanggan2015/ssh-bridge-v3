#include "relay_ctrl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static uint8_t a0_checksum(const uint8_t *data, size_t len) {
    unsigned sum = 0;
    size_t i;
    for (i = 0; i < len; ++i) sum += data[i];
    return (uint8_t)(sum & 0xFF);
}

static void build_a0_set(uint8_t out[4], int channel, int wire_on) {
    uint8_t d[3];
    d[0] = 0xA0;
    d[1] = (uint8_t)channel;
    d[2] = wire_on ? 0x01 : 0x00;
    out[0] = d[0];
    out[1] = d[1];
    out[2] = d[2];
    out[3] = a0_checksum(d, 3);
}

static int logical_to_wire(int logical_on, int io_inverted) {
    if (io_inverted) return logical_on ? 0 : 1;
    return logical_on ? 1 : 0;
}

#ifdef _WIN32
static HANDLE open_port(const char *port, int baudrate, char *err, size_t errcap) {
    char path[64];
    DCB dcb;
    COMMTIMEOUTS to;
    HANDLE h;
    if (!port || !port[0]) {
        snprintf(err, errcap, "empty port");
        return INVALID_HANDLE_VALUE;
    }
    if (port[0] == '\\')
        snprintf(path, sizeof(path), "%s", port);
    else
        snprintf(path, sizeof(path), "\\\\.\\%s", port);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(err, errcap, "open %s failed (%lu)", path, (unsigned long)GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        CloseHandle(h);
        snprintf(err, errcap, "GetCommState failed");
        return INVALID_HANDLE_VALUE;
    }
    dcb.BaudRate = (DWORD)(baudrate > 0 ? baudrate : 9600);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) {
        CloseHandle(h);
        snprintf(err, errcap, "SetCommState failed");
        return INVALID_HANDLE_VALUE;
    }
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = 50;
    to.ReadTotalTimeoutConstant = 500;
    to.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

static int write_port(HANDLE h, const uint8_t *data, size_t len) {
    DWORD n = 0;
    if (!WriteFile(h, data, (DWORD)len, &n, NULL)) return -1;
    return n == (DWORD)len ? 0 : -1;
}
#else
static speed_t baud_flag(int baud) {
    switch (baud) {
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return B9600;
    }
}

static int open_port(const char *port, int baudrate, char *err, size_t errcap) {
    struct termios tio;
    int fd;
    if (!port || !port[0]) {
        snprintf(err, errcap, "empty port");
        return -1;
    }
    fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        snprintf(err, errcap, "open %s failed", port);
        return -1;
    }
    if (tcgetattr(fd, &tio) != 0) {
        close(fd);
        snprintf(err, errcap, "tcgetattr failed");
        return -1;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_flag(baudrate));
    cfsetospeed(&tio, baud_flag(baudrate));
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 5;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close(fd);
        snprintf(err, errcap, "tcsetattr failed");
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int write_port(int fd, const uint8_t *data, size_t len) {
    ssize_t n = write(fd, data, len);
    return n == (ssize_t)len ? 0 : -1;
}
#endif

int relay_ctrl_set(const char *port, int baudrate, int channel, int logical_on,
                   int io_inverted, int post_write_delay_ms, char *err, size_t errcap) {
    uint8_t frame[4];
    int wire;
    int delay = post_write_delay_ms > 0 ? post_write_delay_ms : 100;
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    if (channel < 1) {
        snprintf(err, errcap, "channel must be >= 1");
        return -1;
    }
    wire = logical_to_wire(logical_on ? 1 : 0, io_inverted);
    build_a0_set(frame, channel, wire);
#ifdef _WIN32
    h = open_port(port, baudrate, err, errcap);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (write_port(h, frame, 4) != 0) {
        CloseHandle(h);
        snprintf(err, errcap, "write failed");
        return -1;
    }
    sleep_ms((unsigned)delay);
    CloseHandle(h);
#else
    fd = open_port(port, baudrate, err, errcap);
    if (fd < 0) return -1;
    if (write_port(fd, frame, 4) != 0) {
        close(fd);
        snprintf(err, errcap, "write failed");
        return -1;
    }
    sleep_ms((unsigned)delay);
    close(fd);
#endif
    if (err && errcap) err[0] = '\0';
    return 0;
}

int relay_ctrl_pulse(const char *port, int baudrate, int channel, double hold_seconds,
                     int polarity_inverted, int io_inverted, int post_write_delay_ms,
                     char *err, size_t errcap) {
    int idle = polarity_inverted ? 0 : 1;
    int pulse = polarity_inverted ? 1 : 0;
    unsigned hold_ms;
    if (hold_seconds <= 0) hold_seconds = 1.0;
    hold_ms = (unsigned)(hold_seconds * 1000.0);
    if (relay_ctrl_set(port, baudrate, channel, pulse, io_inverted, post_write_delay_ms, err,
                       errcap) != 0)
        return -1;
    sleep_ms(hold_ms);
    if (relay_ctrl_set(port, baudrate, channel, idle, io_inverted, post_write_delay_ms, err,
                       errcap) != 0)
        return -1;
    return 0;
}

int relay_ctrl_run_mode(const char *port, int baudrate, int channel, const char *mode,
                        double hold_seconds, int polarity_inverted, int io_inverted,
                        int post_write_delay_ms, char *err, size_t errcap) {
    if (!mode || !mode[0]) mode = "pulse";
    if (strcmp(mode, "hold_on") == 0 || strcmp(mode, "on") == 0)
        return relay_ctrl_set(port, baudrate, channel, 1, io_inverted, post_write_delay_ms, err,
                              errcap);
    if (strcmp(mode, "hold_off") == 0 || strcmp(mode, "off") == 0)
        return relay_ctrl_set(port, baudrate, channel, 0, io_inverted, post_write_delay_ms, err,
                              errcap);
    return relay_ctrl_pulse(port, baudrate, channel, hold_seconds, polarity_inverted, io_inverted,
                            post_write_delay_ms, err, errcap);
}
