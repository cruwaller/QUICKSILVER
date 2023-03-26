#include "io/blackbox_device.h"

#include <string.h>

#include "core/looptime.h"
#include "io/blackbox_device_flash.h"
#include "io/blackbox_device_sdcard.h"
#include "project.h"
#include "util/cbor_helper.h"
#include "util/util.h"

#ifdef ENABLE_BLACKBOX

blackbox_device_bounds_t blackbox_bounds;
blackbox_device_header_t blackbox_device_header;

static uint8_t encode_buffer_data[BLACKBOX_ENCODE_BUFFER_SIZE];
ring_buffer_t blackbox_encode_buffer = {
    .buffer = encode_buffer_data,
    .head = 0,
    .tail = 0,
    .size = BLACKBOX_ENCODE_BUFFER_SIZE,
};
uint8_t blackbox_write_buffer[BLACKBOX_WRITE_BUFFER_SIZE];

#define MEMBER CBOR_ENCODE_MEMBER
#define STR_MEMBER CBOR_ENCODE_STR_MEMBER
#define ARRAY_MEMBER CBOR_ENCODE_ARRAY_MEMBER
#define STR_ARRAY_MEMBER CBOR_ENCODE_STR_ARRAY_MEMBER

CBOR_START_STRUCT_ENCODER(blackbox_device_file_t)
BLACKBOX_DEVICE_FILE_MEMBERS
CBOR_END_STRUCT_ENCODER()

CBOR_START_STRUCT_ENCODER(blackbox_device_header_t)
BLACKBOX_DEVICE_HEADER_MEMBERS
CBOR_END_STRUCT_ENCODER()

#undef MEMBER
#undef STR_MEMBER
#undef ARRAY_MEMBER
#undef STR_ARRAY_MEMBER

#define MEMBER CBOR_DECODE_MEMBER
#define STR_MEMBER CBOR_DECODE_STR_MEMBER
#define ARRAY_MEMBER CBOR_DECODE_ARRAY_MEMBER
#define STR_ARRAY_MEMBER CBOR_DECODE_STR_ARRAY_MEMBER

CBOR_START_STRUCT_DECODER(blackbox_device_file_t)
BLACKBOX_DEVICE_FILE_MEMBERS
CBOR_END_STRUCT_DECODER()

CBOR_START_STRUCT_DECODER(blackbox_device_header_t)
BLACKBOX_DEVICE_HEADER_MEMBERS
CBOR_END_STRUCT_DECODER()

#undef MEMBER
#undef STR_MEMBER
#undef ARRAY_MEMBER
#undef STR_ARRAY_MEMBER

blackbox_device_file_t *blackbox_current_file() {
  return &blackbox_device_header.files[blackbox_device_header.file_num - 1];
}

void blackbox_device_init() {
#ifdef USE_M25P16
  blackbox_device_flash_init();
#endif
#ifdef USE_SDCARD
  blackbox_device_sdcard_init();
#endif

  blackbox_device_header.magic = BLACKBOX_HEADER_MAGIC;
  blackbox_device_header.file_num = 0;
}

blackbox_device_result_t blackbox_device_update() {
#ifdef USE_M25P16
  return blackbox_device_flash_update();
#endif
#ifdef USE_SDCARD
  return blackbox_device_sdcard_update();
#endif
}

uint32_t blackbox_device_usage() {
#ifdef USE_M25P16
  return blackbox_device_flash_usage();
#endif
#ifdef USE_SDCARD
  return blackbox_device_sdcard_usage();
#endif
}

bool blackbox_device_ready() {
#ifdef USE_M25P16
  return blackbox_device_flash_ready();
#endif
#ifdef USE_SDCARD
  return blackbox_device_sdcard_ready();
#endif
}

void blackbox_device_write_header() {
#ifdef USE_M25P16
  return blackbox_device_flash_write_header();
#endif
#ifdef USE_SDCARD
  return blackbox_device_sdcard_write_header();
#endif
}

void blackbox_device_reset() {
#ifdef USE_M25P16
  blackbox_device_flash_reset();
#endif
#ifdef USE_SDCARD
  blackbox_device_sdcard_reset();
#endif

  blackbox_device_header.magic = BLACKBOX_HEADER_MAGIC;
  blackbox_device_header.file_num = 0;

  looptime_reset();
}

bool blackbox_device_restart(uint32_t field_flags, uint32_t blackbox_rate, uint32_t looptime) {
  if (blackbox_device_header.file_num >= BLACKBOX_DEVICE_MAX_FILES) {
    return false;
  }
  if (!blackbox_device_ready()) {
    return false;
  }

  const uint32_t offset = MEMORY_ALIGN(blackbox_device_usage(), blackbox_bounds.page_size);
  if (offset >= blackbox_bounds.total_size) {
    // flash is full
    return false;
  }

  blackbox_device_header.files[blackbox_device_header.file_num].field_flags = field_flags;
  blackbox_device_header.files[blackbox_device_header.file_num].looptime = looptime;
  blackbox_device_header.files[blackbox_device_header.file_num].blackbox_rate = blackbox_rate;
  blackbox_device_header.files[blackbox_device_header.file_num].size = 0;
  blackbox_device_header.files[blackbox_device_header.file_num].start = offset;
  blackbox_device_header.file_num++;

  blackbox_device_write_header();

  ring_buffer_clear(&blackbox_encode_buffer);

  return true;
}

void blackbox_device_finish() {
  if (blackbox_current_file()->size == 0) {
    // file was empty, lets remove it
    blackbox_device_header.file_num--;
  }

#ifdef USE_M25P16
  blackbox_device_flash_flush();
#endif
#ifdef USE_SDCARD
  blackbox_device_sdcard_flush();
#endif
}

void blackbox_device_read(const uint32_t file_index, const uint32_t offset, uint8_t *buffer, const uint32_t size) {
#ifdef USE_M25P16
  blackbox_device_flash_read(file_index, offset, buffer, size);
#endif
#ifdef USE_SDCARD
  blackbox_device_sdcard_read(file_index, offset, buffer, size);
#endif
}

cbor_result_t blackbox_device_write(const uint32_t field_flags, const blackbox_t *b) {
  uint8_t buffer[BLACKBOX_WRITE_BUFFER_SIZE];

  cbor_value_t enc;
  cbor_encoder_init(&enc, buffer, BLACKBOX_WRITE_BUFFER_SIZE);

  cbor_result_t res = cbor_encode_blackbox_t(&enc, b, field_flags);
  if (res < CBOR_OK) {
    return res;
  }

  const uint32_t len = cbor_encoder_len(&enc);
  if (len >= ring_buffer_free(&blackbox_encode_buffer)) {
    return res;
  }

  ring_buffer_write_multi(&blackbox_encode_buffer, buffer, len);
  return res;
}

#endif