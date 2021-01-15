/*
 * ebusd - daemon for communication with eBUS heating systems.
 * Copyright (C) 2020-2021 John Baier <ebusd@ebusd.eu>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <argp.h>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <cstring>
#include "intelhex/intelhexclass.h"


/** the version string of the program. */
const char *argp_program_version = "eBUS adapter PIC firmware loader";

/** the documentation of the program. */
static const char argpdoc[] =
    "A tool for loading firmware to the eBUS adapter PIC."
    "\vPORT is the serial port to use (e.g./dev/ttyUSB0)";

static const char argpargsdoc[] = "PORT";

/** the definition of the known program arguments. */
static const struct argp_option argpoptions[] = {
    {"verbose", 'v', nullptr, 0, "enable verbose output", 0 },
    {"dhcp",    'd', nullptr, 0, "set dynamic IP address via DHCP", 0 },
    {"ip",      'i', "IP",    0, "set fix IP address (e.g. 192.168.0.10)", 0 },
    {"mask",    'm', "MASK",  0, "set fix IP mask (e.g. 24)", 0 },
    {"macip",   'M', nullptr, 0, "set the MAC address suffix from the IP address", 0 },
    {"flash",   'f', "FILE",  0, "flash the FILE to the device", 0 },
    {"reset",   'r', nullptr, 0, "reset the device at the end on success", 0 },
    {"slow",    's', nullptr, 0, "use low speed for transfer", 0 },
    {nullptr,          0,        nullptr,    0, nullptr, 0 },
};

static bool verbose = false;
static bool setDhcp = false;
static bool setIp = false;
static uint8_t setIpAddress[] = {0, 0, 0, 0};
static bool setMacFromIp = false;
static bool setMask = false;
static uint8_t setMaskLen = 0x1f;
static char* flashFile = nullptr;
static bool reset = false;
static bool lowSpeed = false;

bool parseByte(const char *arg, uint8_t minValue, uint8_t maxValue, uint8_t *result) {
  char* strEnd = nullptr;
  unsigned long value = 0;
  strEnd = nullptr;
  value = strtoul(arg, &strEnd, 10);
  if (strEnd == nullptr || strEnd == arg || *strEnd != 0) {
    return false;
  }
  if (value<minValue || value>maxValue) {
    return false;
  }
  *result = (uint8_t)value;
  return true;
}

bool parseShort(const char *arg, uint16_t minValue, uint16_t maxValue, uint16_t *result) {
  char* strEnd = nullptr;
  unsigned long value = 0;
  strEnd = nullptr;
  value = strtoul(arg, &strEnd, 10);
  if (strEnd == nullptr || strEnd == arg || *strEnd != 0) {
    return false;
  }
  if (value<minValue || value>maxValue) {
    return false;
  }
  *result = (uint16_t)value;
  return true;
}

error_t parse_opt(int key, char *arg, struct argp_state *state) {
  char *ip = nullptr, *part = nullptr;
  int pos = 0, sum = 0;
  struct stat st;
  switch (key) {
    case 'v':  // --verbose
      verbose = true;
      break;
    case 'd':  // --dhcp
      if (setIp || setMask) {
        argp_error(state, "either DHCP or IP address is needed");
        return EINVAL;
      }
      setDhcp = true;
      break;
    case 'i':  // --ip=192.168.0.10
      if (arg == nullptr || arg[0] == 0) {
        argp_error(state, "invalid IP address");
        return EINVAL;
      }
      if (setDhcp) {
        argp_error(state, "either DHCP or IP address is needed");
        return EINVAL;
      }
      ip = strdup(arg);
      part = strtok(ip, ".");

      for (pos=0; part && pos < 4; pos++) {
        if (!parseByte(part, 0, 255, setIpAddress+pos)) {
          break;
        }
        sum += setIpAddress[pos];
        part = strtok(nullptr, ".");
      }
      free(ip);
      if (pos != 4 || part || sum == 0) {
        argp_error(state, "invalid IP address");
        return EINVAL;
      }
      setIp = true;
      break;
    case 'm':
      if (arg == nullptr || arg[0] == 0) {
        argp_error(state, "invalid IP mask");
        return EINVAL;
      }
      if (setDhcp) {
        argp_error(state, "either DHCP or IP address is needed");
        return EINVAL;
      }
      if (!parseByte(arg, 0, 0x1e, &setMaskLen)) {
        argp_error(state, "invalid IP mask");
        return EINVAL;
      }
      setMask = true;
      break;
    case 'M':
      setMacFromIp = true;
      break;
    case 'f':
      if (arg == nullptr || arg[0] == 0 || stat(arg, &st) != 0 || !S_ISREG(st.st_mode)) {
        argp_error(state, "invalid flash file");
        return EINVAL;
      }
      flashFile = arg;
      break;
    case 'r':
      reset = true;
      break;
    case 's':
      lowSpeed = true;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
  }
  return 0;
}

// START: copy from generated bootloader

#define WRITE_FLASH_BLOCKSIZE    32
#define ERASE_FLASH_BLOCKSIZE    32
#define END_FLASH                0x4000

// Frame Format
//
//  [<COMMAND><DATALEN><ADDRL><ADDRH><ADDRU><...DATA...>]
// These values are negative because the FSR is set to PACKET_DATA to minimize FSR reloads.
typedef union
{
  struct __attribute__((__packed__))
  {
    uint8_t     command;
    uint16_t    data_length;
    uint8_t     EE_key_1;
    uint8_t     EE_key_2;
    uint8_t     address_L;
    uint8_t     address_H;
    uint8_t     address_U;
    uint8_t     address_unused;
    uint8_t     data[2*WRITE_FLASH_BLOCKSIZE];
  };
  uint8_t  buffer[2*WRITE_FLASH_BLOCKSIZE+9];
}frame_t;

#define  STX   0x55

#define  READ_VERSION   0
#define  READ_FLASH     1
#define  WRITE_FLASH    2
#define  ERASE_FLASH    3
#define  READ_EE_DATA   4
#define  WRITE_EE_DATA  5
#define  READ_CONFIG    6
#define  WRITE_CONFIG   7
#define  CALC_CHECKSUM  8
#define  RESET_DEVICE   9
#define  CALC_CRC       10

#define MINOR_VERSION   0x08       // Version
#define MAJOR_VERSION   0x00
//#define STX             0x55       // Actually code 0x55 is 'U'  But this is what the autobaud feature of the PIC16F1 EUSART is looking for
#define ERROR_ADDRESS_OUT_OF_RANGE   0xFE
#define ERROR_INVALID_COMMAND        0xFF
#define COMMAND_SUCCESS              0x01

// END: copy from generated bootloader

#define FRAME_HEADER_LEN 9
#define FRAME_MAX_LEN (FRAME_HEADER_LEN+2*WRITE_FLASH_BLOCKSIZE)
#define BAUDRATE_LOW B115200
#define BAUDRATE_HIGH B921600
#define WAIT_BYTE_TRANSFERRED_MILLIS 200
#define WAIT_BITRATE_DETECTION_MICROS 100
#define WAIT_RESPONSE_TIMEOUT_MILLIS 100
// size of flash in bytes
#define END_FLASH_BYTES (END_FLASH*2)
// size of boot block in words
#define END_BOOT 0x0400
// size of boot block in bytes
#define END_BOOT_BYTES (END_BOOT*2)

long long getTime() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec*1000+ts.tv_nsec/1000000;
}

ssize_t waitWrite(int fd, uint8_t *data, size_t len, int timeoutMillis) {
  int ret;
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLOUT | POLLERR | POLLHUP;
  ret = poll(&pfd, 1, timeoutMillis);
  if (ret >= 0 && pfd.revents & (POLLERR | POLLHUP)) {
    return -1;
  }
  if (ret <= 0) {
    return ret;
  }
  ret = write(fd, data, len);
  if (ret < 0) {
    return ret;
  }
#ifdef DEBUG_RAW
  std::cout << "> " << std::dec << static_cast<unsigned>(ret) << "/" << static_cast<unsigned>(len) << ":" << std::hex;
  for (int pos = 0; pos < ret; pos++) {
    std::cout << " " << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[pos]);
  }
  std::cout << std::endl;
#endif
  return ret;
}

ssize_t waitRead(int fd, uint8_t *data, size_t len, int timeoutMillis) {
  int ret;
  struct pollfd pfd;
  pfd.fd = fd;
  pfd.events = POLLIN | POLLERR | POLLHUP;
  ret = poll(&pfd, 1, timeoutMillis);
  if (ret >= 0 && pfd.revents & (POLLERR | POLLHUP)) {
    return -1;
  }
  if (ret <= 0) {
    return ret;
  }
  ret = read(fd, data, len);
  if (ret < 0) {
    return ret;
  }
#ifdef DEBUG_RAW
  std::cout << "< " << std::dec << static_cast<unsigned>(ret) << "/" << static_cast<unsigned>(len) << ":" << std::hex;
  for (int pos = 0; pos < ret; pos++) {
    std::cout << " " << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[pos]);
  }
  std::cout << std::endl;
#endif
  return ret;
}

ssize_t sendReceiveFrame(int fd, frame_t& frame, size_t sendDataLen, ssize_t fixReceiveDataLen,
                         int responseTimeoutExtraMillis = 0, bool hideErrors = false) {
  // send 0x55 for auto baud detection in PIC
  unsigned char ch = STX;
  ssize_t cnt = waitWrite(fd, &ch, 1, WAIT_BYTE_TRANSFERRED_MILLIS);
  if (cnt < 0) {
    if (!hideErrors) {
      std::cerr << "write sync failed" << std::endl;
    }
    return cnt;
  }
  if (cnt == 0) {
    if (!hideErrors) {
      std::cerr << "write sync timed out" << std::endl;
    }
    return cnt;
  }
  // wait for bitrate detection to finish in PIC
  usleep(WAIT_BITRATE_DETECTION_MICROS);
  uint8_t writeCommand = frame.command;
  size_t len = FRAME_HEADER_LEN+sendDataLen;
  for (size_t pos=0; pos < len; ) {
    cnt = waitWrite(fd, frame.buffer+pos, len-pos, WAIT_BYTE_TRANSFERRED_MILLIS);
    if (cnt < 0) {
      if (!hideErrors) {
        std::cerr << "write data failed" << std::endl;
      }
      return cnt;
    }
    if (cnt == 0) {
      if (!hideErrors) {
        std::cerr << "write data timed out" << std::endl;
      }
      return -1;
    }
    pos += cnt;
  }
  cnt = waitRead(fd, &ch, 1, WAIT_RESPONSE_TIMEOUT_MILLIS + responseTimeoutExtraMillis);
  if (cnt < 0) {
    if (!hideErrors) {
      std::cerr << "read sync failed" << std::endl;
    }
    return cnt;
  }
  if (cnt == 0) {
    if (!hideErrors) {
      std::cerr << "read sync timed out" << std::endl;
    }
    return -1;
  }
  if (ch != STX) {
    if (!hideErrors) {
      std::cerr << "did not receive sync: 0x" << std::setfill('0') << std::setw(2) << std::hex
                << static_cast<unsigned>(ch) << std::endl;
    }
    return -1;
  }
  // read the answer from the device
  len = FRAME_HEADER_LEN;  // start with the header itself
  for (size_t pos=0; pos < len; ) {
    cnt = waitRead(fd, frame.buffer+pos, len-pos, WAIT_BYTE_TRANSFERRED_MILLIS);
    if (cnt < 0) {
      if (!hideErrors) {
        std::cerr << "read data failed" << std::endl;
      }
      return cnt;
    }
    if (cnt == 0) {
      if (!hideErrors) {
        std::cerr << "read data timed out" << std::endl;
      }
      return -1;
    }
    pos += cnt;
    if (pos == FRAME_HEADER_LEN) {
      if (fixReceiveDataLen < 0) {
        len += frame.data_length;
      } else {
        len += fixReceiveDataLen;
      }
      fixReceiveDataLen = 0;
    }
  }
  uint8_t dummy[4];
  waitRead(fd, dummy, 4, WAIT_BYTE_TRANSFERRED_MILLIS);  // read away potential nonsense tail
  if (frame.command != writeCommand) {
    if (!hideErrors) {
      std::cerr << "unexpected answer" << std::endl;
    }
    return -1;
  }
  return 0;
}

int readVersion(int fd, bool verbose = true) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = READ_VERSION;
  ssize_t ret = sendReceiveFrame(fd, frame, 0, 16);
  if (ret != 0) {
    return ret;
  }
  if (frame.data[0] != MINOR_VERSION || frame.data[1] != MAJOR_VERSION) {
    std::cerr << "unexpected version" << std::endl;
    return -1;
  }
  if (verbose) {
    std::cout << "Max packet size: " << static_cast<unsigned>(frame.data[2] | (frame.data[3] << 8)) << std::endl;
  }
  std::cout << "Device ID: " << std::setfill('0') << std::setw(4) << std::hex
            << static_cast<unsigned>(frame.data[6] | (frame.data[7] << 8));
  if (frame.data[6] == 0xb0 && frame.data[7] == 0x30) {
    std::cout << " (PIC16F15356)";
  }
  std::cout << std::endl;
  if (verbose) {
    std::cout << "Blocksize erase: " << std::dec << static_cast<unsigned>(frame.data[10]) << std::endl;
    std::cout << "Blocksize write: " << std::dec << static_cast<unsigned>(frame.data[11]) << std::endl;
    std::cout << "User ID 1: " << std::setfill('0') << std::setw(2) << std::hex
              << static_cast<unsigned>(frame.data[12]) << std::endl;
    std::cout << "User ID 2: " << std::setfill('0') << std::setw(2) << std::hex
              << static_cast<unsigned>(frame.data[13]) << std::endl;
    std::cout << "User ID 3: " << std::setfill('0') << std::setw(2) << std::hex
              << static_cast<unsigned>(frame.data[14]) << std::endl;
    std::cout << "User ID 4: " << std::setfill('0') << std::setw(2) << std::hex
              << static_cast<unsigned>(frame.data[15]) << std::endl;
  }
  return 0;
}

int printFrameData(frame_t frame, bool skipHigh) {
  uint16_t address = (frame.address_H << 8)|frame.address_L;
  int pos;
  std::cout << std::hex;
  for (pos = 0; pos < frame.data_length;) {
    if ((pos%16) == 0) {
      std::cout << std::setw(4) << static_cast<unsigned>(address) << ":";
    }
    std::cout << " " << std::setw(2) << static_cast<unsigned>(frame.data[pos++]);
    if (skipHigh) {
      pos++;
    } else if (pos < frame.data_length) {
      std::cout << " " << std::setw(2) << static_cast<unsigned>(frame.data[pos++]);
    }
    address++;
    if ((pos%16) == 0) {
      std::cout << std::endl;
    }
  }
  if ((pos%16) != 0) {
    std::cout << std::endl;
  }
  return 0;
}

int printFrame(frame_t frame) {
  std::cout << "command:     0x" << std::setfill('0') << std::setw(2) << std::hex
            << static_cast<unsigned>(frame.command) << std::endl;
  std::cout << "data_length: " << std::dec << static_cast<unsigned>(frame.data_length) << std::endl;
  std::cout << "address:     0x" << std::setw(2) << std::hex << static_cast<unsigned>(frame.address_H) << std::setw(2)
            << std::hex << static_cast<unsigned>(frame.address_L);
  for (int pos = 0; pos < frame.data_length; ) {
    if ((pos%16) == 0) {
      std::cout << std::endl << std::setw(4) << static_cast<unsigned>(pos) << ":" << std::endl;
    }
    std::cout << " " << std::setw(2) << static_cast<unsigned>(frame.data[pos++]);
    pos++;
  }
  std::cout << std::endl;
  return 0;
}

int readConfig(int fd, uint16_t address, uint16_t len, bool skipHigh = false, bool print = true,
               uint8_t* storeData = nullptr) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = READ_CONFIG;
  frame.data_length = len;
  frame.address_L = address&0xff;
  frame.address_H = (address>>8)&0xff;
  ssize_t ret = sendReceiveFrame(fd, frame, 0, len);
  if (ret != 0) {
    return ret;
  }
  if (print) {
    printFrameData(frame, skipHigh);
  }
  if (storeData) {
    memcpy(storeData, frame.data, len);
  }
  return 0;
}

int writeConfig(int fd, uint16_t address, uint16_t len, uint8_t* data) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = WRITE_CONFIG;
  frame.data_length = len;
  frame.EE_key_1 = 0x55;
  frame.EE_key_2 = 0xaa;
  frame.address_L = address&0xff;
  frame.address_H = (address>>8)&0xff;
  memcpy(frame.data, data, len);
  ssize_t ret = sendReceiveFrame(fd, frame, len, 1, 50);
  if (ret != 0) {
    return ret;
  }
  if (frame.data[0] != COMMAND_SUCCESS) {
    return -1;
  }
  return 0;
}

int readFlash(int fd, uint16_t address, bool skipHigh = false, bool print = true, uint8_t* storeData = nullptr) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = READ_FLASH;
  frame.data_length = 0x10;
  frame.address_L = address&0xff;
  frame.address_H = (address>>8)&0xff;
  ssize_t ret = sendReceiveFrame(fd, frame, 0, -1);
  if (ret != 0) {
    return ret;
  }
  if (print) {
    printFrameData(frame, skipHigh);
  }
  if (storeData) {
    memcpy(storeData, frame.data, 0x10);
  }
  return 0;
}

int writeFlash(int fd, uint16_t address, uint16_t len, uint8_t* data, bool hideErrors = false) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = WRITE_FLASH;
  frame.data_length = len;
  frame.EE_key_1 = 0x55;
  frame.EE_key_2 = 0xaa;
  frame.address_L = address&0xff;
  frame.address_H = (address>>8)&0xff;
  memcpy(frame.data, data, len);
  ssize_t ret = sendReceiveFrame(fd, frame, len, 1, len*30, hideErrors);
  if (ret != 0) {
    return ret;
  }
  if (frame.data[0] != COMMAND_SUCCESS) {
    return -1;
  }
  return 0;
}

int eraseFlash(int fd, uint16_t address, uint16_t len) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = ERASE_FLASH;
  frame.data_length = (len+ERASE_FLASH_BLOCKSIZE-1)/ERASE_FLASH_BLOCKSIZE;
  frame.EE_key_1 = 0x55;
  frame.EE_key_2 = 0xaa;
  frame.address_L = address&0xff;
  frame.address_H = (address>>8)&0xff;
  ssize_t ret = sendReceiveFrame(fd, frame, 0, 1, frame.data_length*5);
  if (ret != 0) {
    return ret;
  }
  if (frame.data[0] != COMMAND_SUCCESS) {
    return -frame.data[0]-1;
  }
  return 0;
}

int calcChecksum(int fd, uint16_t address, uint16_t len) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = CALC_CHECKSUM;
  frame.data_length = len;
  frame.address_L = address&0xff;
  frame.address_H = (address>>8)&0xff;
  ssize_t ret = sendReceiveFrame(fd, frame, 0, 2, len*30);
  if (ret != 0) {
    return ret;
  }
  return frame.data[0] | (frame.data[1] << 8);
}

int resetDevice(int fd) {
  frame_t frame;
  memset(frame.buffer, 0, FRAME_MAX_LEN);
  frame.command = RESET_DEVICE;
  ssize_t ret = sendReceiveFrame(fd, frame, 0, 1);
  if (ret != 0) {
    return ret;
  }
  if (frame.data[0] != COMMAND_SUCCESS) {
    return -frame.data[0]-1;
  }
  return 0;
}

struct termios termios_original;

int openSerial(std::string port) {
  // open serial port
  int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);  // non-blocking IO: | O_NONBLOCK);
  if (fd == -1) {
    std::cerr << "unable to open " << port << std::endl;
    return -1;
  }

  if (flock(fd, LOCK_EX|LOCK_NB)) {
    close(fd);
    std::cerr << "unable to lock " << port << std::endl;
    return -1;
  }

  // backup terminal settings
  tcgetattr(fd, &termios_original);

  // configure terminal settings
  struct termios termios;
  memset(&termios, 0, sizeof(termios));

  if (cfsetspeed(&termios, lowSpeed ? BAUDRATE_LOW : BAUDRATE_HIGH) != 0) {
    std::cerr << "unable to set speed " << std::endl;
    close(fd);
    return -1;
  }
  termios.c_iflag |= 0;
  termios.c_oflag |= 0;
  termios.c_cflag |= CS8 | CREAD | CLOCAL;
  termios.c_lflag |= 0;
  termios.c_cc[VMIN] = 1;
  termios.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &termios) != 0) {
    std::cerr << "unable to set serial " << std::endl;
    close(fd);
    return -1;
  }
  return fd;
}

void closeSerial(int fd) {
  tcsetattr(fd, TCSANOW, &termios_original);
  close(fd);
}

int calcFileChecksum(uint8_t* storeFirstBlock = nullptr) {
  std::ifstream inStream;
  inStream.open(flashFile, ifstream::in);
  if (!inStream.good()) {
    std::cerr << "unable to open file" << std::endl;
    return -1;
  }
  intelhex ih;
  inStream >> ih;
  if (ih.getNoErrors() > 0 || ih.getNoWarnings() > 0) {
    std::cerr << "unable to read file" << std::endl;
    return -1;
  }
  unsigned long startAddr = 0, endAddr = 0;
  if (!ih.startAddress(&startAddr) || !ih.endAddress(&endAddr)) {
    std::cerr << "unable to read file" << std::endl;
    return -1;
  }
  if (startAddr < END_BOOT_BYTES || endAddr >= END_FLASH_BYTES || endAddr < startAddr || (startAddr&0xf) != 0) {
    std::cerr << "invalid address range" << std::endl;
    return -1;
  }
  ih.begin();
  unsigned long nextAddr = ih.currentAddress();
  if (nextAddr != END_BOOT_BYTES) {
    std::cerr << "unexpected start address in file." << std::endl;
    return -1;
  }
  unsigned long blockStart = END_BOOT_BYTES;
  uint16_t checkSum = 0;
  while (blockStart < END_FLASH_BYTES && nextAddr < END_FLASH_BYTES) {
    for (int pos = 0; pos < WRITE_FLASH_BLOCKSIZE; pos++, nextAddr++) {
      unsigned long addr = ih.currentAddress();
      uint8_t value = (pos&0x1) == 1 ? 0x3f : 0xff;
      if (addr == nextAddr && ih.getData(&value)) {
        ih.incrementAddress();
      }
      if (storeFirstBlock && nextAddr < END_BOOT_BYTES+0x10) {
        storeFirstBlock[pos] = value;
      }
      checkSum += ((uint16_t)value) << ((pos&0x1)*8);
    }
    blockStart += WRITE_FLASH_BLOCKSIZE;
  }
  return checkSum;
}

void printFileChecksum() {
  uint8_t data[0x10];
  int checkSum = calcFileChecksum(data);
  int newFirmwareVersion = -1;
  if (data[0x2*2] == 0xae && data[0x2*2+1] == 0x34 && data[0x3*2+1] == 0x34) {
    newFirmwareVersion = data[0x3*2];
  }
  std::cout
    << "New firmware version: " << static_cast<unsigned>(newFirmwareVersion)
    << " [" << std::hex << std::setw (4) << std::setfill('0') << static_cast<signed>(checkSum) << "]" << std::endl;
}

bool flashPic(int fd) {
  std::ifstream inStream;
  inStream.open(flashFile, ifstream::in);
  if (!inStream.good()) {
    std::cerr << "unable to open file" << std::endl;
    return false;
  }
  intelhex ih;
//  if (verbose) {
//    ih.verboseOn();
//  }
  inStream >> ih;
  if (ih.getNoErrors() > 0 || ih.getNoWarnings() > 0) {
    std::cerr << "errors or warnings while reading the file:" << std::endl;
    string str;
    while (ih.popNextWarning(str)) {
      std::cerr << "warning: " << str << std::endl;
    }
    while (ih.popNextError(str)) {
      std::cerr << "error: " << str << std::endl;
    }
    return false;
  }
  unsigned long startAddr = 0, endAddr = 0;
  if (!ih.startAddress(&startAddr) || !ih.endAddress(&endAddr)) {
    std::cerr << "unable to read file" << std::endl;
    return false;
  }
  if (verbose) {
    std::cout << "flashing bytes 0x"
              << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(startAddr)
              << " - 0x"
              << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(endAddr)
              << std::endl;
  }
  if (startAddr < END_BOOT_BYTES || endAddr >= END_FLASH_BYTES || endAddr < startAddr || (startAddr&0xf) != 0) {
    std::cerr << "invalid address range" << std::endl;
    return false;
  }
  ih.begin();
  uint8_t buf[WRITE_FLASH_BLOCKSIZE];
  unsigned long nextAddr = ih.currentAddress();
  if (nextAddr != END_BOOT_BYTES) {
    std::cerr << "unexpected start address in file: 0x" << std::hex << std::setfill('0') << std::setw(4)
              << static_cast<unsigned>(nextAddr) << std::endl;
    return false;
  }
  unsigned long blockStart = END_BOOT_BYTES;
  uint16_t checkSum = 0;
  int eraseRes = eraseFlash(fd, blockStart/2, (endAddr-blockStart)/2);
  if (eraseRes != 0) {
    std::cerr << "erasing flash failed: " << static_cast<signed>(-eraseRes-1) << std::endl;
    return false;
  }
  std::cout << "erasing flash: done." << std::endl;
  std::cout << "flashing: 0x" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(nextAddr/2)
            << " - 0x" << static_cast<unsigned>(endAddr/2) << std::endl;
  size_t blocks = 0;
  while (blockStart < endAddr) {
    bool blank = true;
    for (int pos = 0; pos < WRITE_FLASH_BLOCKSIZE; pos++, nextAddr++) {
      unsigned long addr = ih.currentAddress();
      uint8_t value = (pos&0x1) == 1 ? 0x3f : 0xff;
      if (addr == nextAddr && ih.getData(&value)) {
        ih.incrementAddress();
        blank = false;
      }
      buf[pos] = value;
      checkSum += ((uint16_t)value) << ((pos&0x1)*8);
    }
    if (!blank) {
      if (blocks == 0) {
        std::cout << std::endl << "0x" << std::hex << std::setfill('0') << std::setw(4)
                  << static_cast<unsigned>(blockStart/2) << " ";
      }
      if (writeFlash(fd, blockStart/2, WRITE_FLASH_BLOCKSIZE, buf, true) != 0) {
        // repeat once silently:
        if (writeFlash(fd, blockStart/2, WRITE_FLASH_BLOCKSIZE, buf) != 0) {
          std::cerr << "unable to write flash at 0x" << std::hex << std::setfill('0') << std::setw(4)
                    << static_cast<unsigned>(blockStart/2) << std::endl;
          return false;
        }
      }
      std::cout << ".";
      if (++blocks >= 64) {
        blocks = 0;
      }
      std::cout.flush();
    }
    blockStart += WRITE_FLASH_BLOCKSIZE;
  }
  std::cout << std::endl << "flashing finished." << std::endl;
  int picSum = calcChecksum(fd, startAddr/2, blockStart-startAddr);
  if (picSum < 0) {
    std::cout << "unable to read checksum." << std::endl;
    return false;
  }
  if (picSum != checkSum) {
    std::cout << "unexpected checksum." << std::endl;
    return false;
  }
  std::cout << "flashing succeeded." << std::endl;
  return true;
}

void readIpSettings(int fd) {
  uint8_t mac[] = {0xae, 0xb0, 0x53, 0xef, 0xfe, 0xef};  // "Adapter-eBUS3" + (UserID or MUI)
  uint8_t ip[4] = {0, 0, 0, 0};
  bool useMUI = true;
  uint8_t maskLen = 0;
  uint8_t configData[8];
  readConfig(fd, 0x0000, 8, false, false, configData);  // User ID
  useMUI = (configData[1]&0x20) != 0;  // if highest bit is set, then use MUI. if cleared, use User ID
  maskLen = configData[1]&0x1f;
  for (int i=0; i < 4; i++) {
    ip[i] = configData[i*2];
    if (!useMUI && i > 0) {
      mac[2+i] = configData[i*2];
    }
  }
  if (useMUI) {
    // read MUI to build uniqueMAC address
    // start with MUI6, end with MUI8 (MUI9 is reserved)
    readConfig(fd, 0x0106, 8, true, false, configData);  // MUI
    for (int i=0; i < 3; i++) {
      mac[3+i] = configData[i*2];
    }
  }
  std::cout << "MAC address:";
  for (int i=0; i < 6; i++) {
    std::cout << (i == 0?' ':':') << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(mac[i]);
  }
  std::cout << std::endl;
  if (maskLen == 0x1f || (ip[0]|ip[1]|ip[2]|ip[3]) == 0) {
    std::cout << "IP address: DHCP" << std::endl;
  } else {
    std::cout << "IP address:";
    for (int i=0; i < 4; i++) {
      std::cout << (i == 0?' ':'.') << std::dec << static_cast<unsigned>(ip[i]);
    }
    std::cout << "/" << std::dec << static_cast<unsigned>(maskLen) << std::endl;
    /*
    // build gateway
    for (uint8_t pos=0; pos < 4; pos++) {
      mask[pos] = maskLen >= 8 ? 255 : maskLen<=0 ? 0 : (255^((1 << (8-maskLen))-1));
      ip[pos] &= mask[pos];
      maskLen = maskLen >= 8 ? maskLen-8 : 0;
    }
    ip[3] |= 1; // first address in network is used as gateway (not needed anyway))
    std::cout << "IP gateway:";
    for (int i=0; i < 4; i++) {
      std::cout << (i == 0?' ':'.') << std::dec << static_cast<unsigned>(ip[i]);
    }
    std::cout << std::endl;
    */
  }
}

bool writeIpSettings(int fd) {
  std::cout << "Writing IP settings: ";
  uint8_t configData[] = {0xff, 0x3f, 0xff, 0x3f, 0xff, 0x3f, 0xff, 0x3f};
  if (setMacFromIp) {
    configData[1] &= ~0x20;  // set useMUI
  }
  configData[1] = (configData[1]&~0x1f) | (setMaskLen&0x1f);
  if (setIp) {
    for (int i = 0; i < 4; i++) {
      configData[i * 2] = setIpAddress[i];
    }
  }
  if (writeConfig(fd, 0x0000, 8, configData) != 0) {
    std::cerr << "failed" << std::endl;
    return false;
  }
  std::cout << "done." << std::endl;
  return true;
}

int main(int argc, char* argv[]) {
  struct argp aargp = { argpoptions, parse_opt, argpargsdoc, argpdoc, nullptr, nullptr, nullptr };
  int arg_index = -1;
  setenv("ARGP_HELP_FMT", "no-dup-args-note", 0);

  if (argp_parse(&aargp, argc, argv, ARGP_IN_ORDER, &arg_index, nullptr) != 0) {
    std::cerr << "invalid arguments" << std::endl;
    exit(EXIT_FAILURE);
  }

  if (setIp != setMask || (setMacFromIp && !setIp)) {
    std::cerr << "incomplete IP arguments" << std::endl;
    arg_index = argc;  // force help output
  }
  if (argc-arg_index < 1) {
    if (flashFile) {
      printFileChecksum();
      exit(EXIT_SUCCESS);
    } else {
      argp_help(&aargp, stderr, ARGP_HELP_STD_ERR, "ebuspicloader");
      exit(EXIT_FAILURE);
    }
  }

  int fd = openSerial(argv[arg_index]);
  if (fd < 0) {
    exit(EXIT_FAILURE);
  }

  // read version
  if (readVersion(fd, verbose) != 0) {
    closeSerial(fd);
    exit(EXIT_FAILURE);
  }
  uint8_t data[0x10];
  if (verbose) {
    std::cout << "User ID:" << std::endl;
    readConfig(fd, 0x0000, 8);  // User ID
    std::cout << "Rev ID, Device ID:" << std::endl;
  }
  readConfig(fd, 0x0005, 4, false, verbose, data);  // Rev ID and Device ID
  std::cout << "Device revision: " << static_cast<unsigned>(((data[1]&0xf) << 2) | ((data[0]&0xc0)>>6))
            << "." << static_cast<unsigned>(data[0]&0x3f) << std::endl;
  if (verbose) {
    std::cout << "Configuration words:" << std::endl;
    readConfig(fd, 0x0007, 5*2);  // Configuration Words
    std::cout << "MUI:" << std::endl;
    readConfig(fd, 0x0100, 9*2, true);  // MUI
    std::cout << "EUI:"<< std::endl;
    readConfig(fd, 0x010a, 8*2);  // EUI
  }
  if (verbose) {
    std::cout << "Flash:" << std::endl;
  }
  readFlash(fd, 0x0000, false, false, data);
  int bootloaderVersion = -1;
  if (data[0x2*2] == 0xab && data[0x2*2+1] == 0x34 && data[0x3*2+1] == 0x34) {
    bootloaderVersion = data[0x3*2];
    int picSum = calcChecksum(fd, 0x0000, END_BOOT_BYTES);
    std::cout
      << "Bootloader version: " << static_cast<unsigned>(bootloaderVersion)
      << " [" << std::hex << std::setw (4) << std::setfill('0') << static_cast<signed>(picSum) << "]" << std::endl;
  } else {
    std::cerr << "Bootloader version not found" << std::endl;
  }
  readFlash(fd, END_BOOT, false, false, data);
  int firmwareVersion = -1;
  if (data[0x2*2] == 0xae && data[0x2*2+1] == 0x34 && data[0x3*2+1] == 0x34) {
    firmwareVersion = data[0x3*2];
    int picSum = calcChecksum(fd, END_BOOT, END_FLASH_BYTES-END_BOOT_BYTES);
    std::cout
      << "Firmware version: " << static_cast<unsigned>(firmwareVersion)
      << " [" << std::hex << std::setw (4) << std::setfill('0') << static_cast<signed>(picSum) << "]" << std::endl;
  } else {
    std::cout << "Firmware version not found" << std::endl;
  }
  readIpSettings(fd);
  std::cout << std::endl;
  bool success = true;
  if (flashFile) {
    printFileChecksum();
    if (!flashPic(fd)) {
      success = false;
    }
  }
  if (setIp || setDhcp) {
    if (writeIpSettings(fd)) {
      std::cout << "IP settings changed to:" << std::endl;
      readIpSettings(fd);
    } else {
      success = false;
    }
  }
  if (reset && success) {
    std::cout << "resetting device." << std::endl;
    resetDevice(fd);
  }

  closeSerial(fd);
  return 0;
}
