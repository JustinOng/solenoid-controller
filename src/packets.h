#ifndef PACKETS_H
#define PACKETS_H

#include <stdint.h>

enum packet_type {
  request = 1,
  status,
  data
};

enum packet_status {
  initialising = 1,
  initialising_failure,
  active,
  timeout
};

typedef struct __attribute__((packed)) {
  uint8_t type;
} packet_request_t;

typedef struct __attribute__((packed)) {
  uint8_t status;
} packet_status_t;

typedef struct __attribute__((packed)) {
  uint8_t status;
  uint8_t target_status[64];
  uint16_t distances[64];
} packet_data_t;

constexpr uint32_t PACKET_HEADER = 0xDEADBEEF;
constexpr uint32_t PACKET_TRAILER = 0xA5A5A5A5;

// Length of all the common parts of a packet
constexpr int PACKET_COMMON_LEN = 6;

typedef struct __attribute__((packed)) {
  uint32_t header = PACKET_HEADER;
  uint8_t destination;
  uint8_t type;
  union {
    packet_request_t request;
    packet_status_t status;
    packet_data_t data;
  };
} packet_t;

#endif
