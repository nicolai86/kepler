/*
Copyright 2019 Ryan Caltabiano <https://github.com/XScorpion2>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "oled.h"
#include "i2c_master.h"
#include <string.h>
#include <hal.h>


#include "glcdfont.c"
#include "timer.h"
#include "print.h"

#include <string.h>

#define PROGMEM
#define memcpy_P(des, src, len) memcpy(des, src, len)

// Used commands from spec sheet: https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
// Fundamental Commands
#define CONTRAST                0x81
#define DISPLAY_ALL_ON          0xA5
#define DISPLAY_ALL_ON_RESUME   0xA4
#define NORMAL_DISPLAY          0xA6
#define DISPLAY_ON              0xAF
#define DISPLAY_OFF             0xAE

// Scrolling Commands
#define ACTIVATE_SCROLL         0x2F
#define DEACTIVATE_SCROLL       0x2E
#define SCROLL_RIGHT            0x26
#define SCROLL_LEFT             0x27
#define SCROLL_RIGHT_UP         0x29
#define SCROLL_LEFT_UP          0x2A

// Addressing Setting Commands
#define MEMORY_MODE             0x20
#define COLUMN_ADDR             0x21
#define PAGE_ADDR               0x22

// Hardware Configuration Commands
#define DISPLAY_START_LINE      0x40
#define SEGMENT_REMAP           0xA0
#define SEGMENT_REMAP_INV       0xA1
#define MULTIPLEX_RATIO         0xA8
#define COM_SCAN_INC            0xC0
#define COM_SCAN_DEC            0xC8
#define DISPLAY_OFFSET          0xD3
#define COM_PINS                0xDA

// Timing & Driving Commands
#define DISPLAY_CLOCK           0xD5
#define PRE_CHARGE_PERIOD       0xD9
#define VCOM_DETECT             0xDB

// Charge Pump Commands
#define CHARGE_PUMP             0x8D

// Misc defines
#define OLED_TIMEOUT 60000
#define OLED_BLOCK_COUNT (sizeof(OLED_BLOCK_TYPE) * 8)
#define OLED_BLOCK_SIZE (OLED_MATRIX_SIZE / OLED_BLOCK_COUNT)

// i2c defines
#define I2C_CMD 0x00
#define I2C_DATA 0x40
#define I2C_TRANSMIT_P(data) oled_i2c_transmit((OLED_DISPLAY_ADDRESS << 1), &data[0], sizeof(data), I2C_TIMEOUT)
#define I2C_STOP() oled_i2c_stop()
#define I2C_TRANSMIT(data) oled_i2c_transmit((OLED_DISPLAY_ADDRESS << 1), &data[0], sizeof(data), I2C_TIMEOUT)
#define I2C_WRITE_REG(mode, data, size) oled_i2c_writeReg((OLED_DISPLAY_ADDRESS << 1), mode, data, size, I2C_TIMEOUT)

#define HAS_FLAGS(bits, flags) ((bits & flags) == flags)

// Display buffer's is the same as the OLED memory layout
// this is so we don't end up with rounding errors with
// parts of the display unusable or don't get cleared correctly
// and also allows for drawing & inverting
uint8_t          oled_buffer[OLED_MATRIX_SIZE];
uint8_t         *oled_cursor;
OLED_BLOCK_TYPE  oled_dirty = 0;
bool             oled_initialized = false;
bool             oled_active = false;
bool             oled_scrolling = false;
uint8_t          oled_rotation = 0;
uint8_t          oled_rotation_width = 0;
#if !defined(OLED_DISABLE_TIMEOUT)
uint16_t         oled_last_activity;
#endif

static uint8_t oled_i2c_address;

// This configures the I2C clock to 400khz assuming a 72Mhz clock
// For more info : https://www.st.com/en/embedded-software/stsw-stm32126.html
static const I2CConfig oledi2cconfig = {
    STM32_TIMINGR_PRESC(15U) |
    STM32_TIMINGR_SCLDEL(4U) | STM32_TIMINGR_SDADEL(2U) |
    STM32_TIMINGR_SCLH(15U)  | STM32_TIMINGR_SCLL(21U),
    0,
    0
};

static i2c_status_t chibios_to_qmk(const msg_t *status)
{
    switch (*status) {
        case I2C_NO_ERROR:
            return I2C_STATUS_SUCCESS;
        case I2C_TIMEOUT:
            return I2C_STATUS_TIMEOUT;
        // I2C_BUS_ERROR, I2C_ARBITRATION_LOST, I2C_ACK_FAILURE, I2C_OVERRUN, I2C_PEC_ERROR, I2C_SMB_ALERT
        default:
            return I2C_STATUS_ERROR;
    }
}


i2c_status_t oled_i2c_writeReg(uint8_t devaddr, uint8_t regaddr, const uint8_t *data, uint16_t length, uint16_t timeout)
{
    oled_i2c_address = devaddr;
    i2cStart(&I2CD2, &oledi2cconfig);

    uint8_t complete_packet[length + 1];
    for (uint8_t i = 0; i < length; i++) {
        complete_packet[i + 1] = data[i];
    }
    complete_packet[0] = regaddr;

    msg_t status = i2cMasterTransmitTimeout(&I2CD2, (oled_i2c_address >> 1), complete_packet, length + 1, 0, 0, MS2ST(timeout));
    return chibios_to_qmk(&status);
}

i2c_status_t oled_i2c_transmit(uint8_t address, const uint8_t *data, uint16_t length, uint16_t timeout)
{
    oled_i2c_address = address;
    i2cStart(&I2CD2, &oledi2cconfig);
    msg_t status = i2cMasterTransmitTimeout(&I2CD2, (oled_i2c_address >> 1), data, length, 0, 0, MS2ST(timeout));
    return chibios_to_qmk(&status);
}

void oled_i2c_stop(void)
{
    i2cStop(&I2CD2);
}

void oled_i2c_init(void)
{
    palSetPadMode(GPIOA, 9, PAL_MODE_INPUT);
    palSetPadMode(GPIOA, 10, PAL_MODE_INPUT);

    chThdSleepMilliseconds(10);

    palSetPadMode(GPIOA, 9, PAL_MODE_ALTERNATE(4) | PAL_STM32_OTYPE_OPENDRAIN);
    palSetPadMode(GPIOA, 10, PAL_MODE_ALTERNATE(4) | PAL_STM32_OTYPE_OPENDRAIN);

    oled_i2c_address = OLED_DISPLAY_ADDRESS << 1;
    i2cStart(&I2CD2, &oledi2cconfig);
}

static void InvertCharacter(uint8_t *cursor)
{
    const uint8_t *end = cursor + OLED_FONT_WIDTH;
    while (cursor < end) {
        *cursor = ~(*cursor);
        cursor++;
    }
}


bool oled_init(uint8_t rotation)
{
    oled_rotation = oled_init_user(rotation);
    if (!HAS_FLAGS(oled_rotation, OLED_ROTATION_90)) {
        oled_rotation_width = OLED_DISPLAY_WIDTH;
    } else {
        oled_rotation_width = OLED_DISPLAY_HEIGHT;
    }
    oled_i2c_init();

    static const uint8_t PROGMEM display_setup1[] = {
        I2C_CMD,
        DISPLAY_OFF,
        DISPLAY_CLOCK, 0x80,
        MULTIPLEX_RATIO, OLED_DISPLAY_HEIGHT - 1,
        DISPLAY_OFFSET, 0x00,
        DISPLAY_START_LINE | 0x00,
        CHARGE_PUMP, 0x14,
        MEMORY_MODE, 0x00,
    }; // Horizontal addressing mode
    if (I2C_TRANSMIT_P(display_setup1) != I2C_STATUS_SUCCESS) {
        print("oled_init cmd set 1 failed\n");
        return false;
    }
    I2C_STOP();

    if (!HAS_FLAGS(oled_rotation, OLED_ROTATION_180)) {
        static const uint8_t PROGMEM display_normal[] = {
            I2C_CMD,
            SEGMENT_REMAP_INV,
            COM_SCAN_DEC
        };
        if (I2C_TRANSMIT_P(display_normal) != I2C_STATUS_SUCCESS) {
            print("oled_init cmd normal rotation failed\n");
            return false;
        }
    } else {
        static const uint8_t PROGMEM display_flipped[] = {
            I2C_CMD,
            SEGMENT_REMAP,
            COM_SCAN_INC
        };
        if (I2C_TRANSMIT_P(display_flipped) != I2C_STATUS_SUCCESS) {
            print("display_flipped failed\n");
            return false;
        }
    }
    I2C_STOP();

    static const uint8_t PROGMEM display_setup2[] = {
        I2C_CMD,
        COM_PINS, 0x02,
        CONTRAST, 0x8F,
        PRE_CHARGE_PERIOD, 0xF1,
        VCOM_DETECT, 0x40,
        DISPLAY_ALL_ON_RESUME,
        NORMAL_DISPLAY,
        DEACTIVATE_SCROLL,
        DISPLAY_ON,
        CONTRAST, 0
    };
    if (I2C_TRANSMIT_P(display_setup2) != I2C_STATUS_SUCCESS) {
        print("display_setup2 failed\n");
        return false;
    }
    I2C_STOP();

    oled_clear();

    oled_initialized = true;
    oled_active = true;
    oled_scrolling = false;
    return true;
}

__attribute__((weak))
oled_rotation_t oled_init_user(oled_rotation_t rotation)
{
    return rotation;
}

void oled_clear(void)
{
    memset(oled_buffer, 0, sizeof(oled_buffer));
    oled_cursor = &oled_buffer[0];
    oled_dirty = -1; // -1 will be max value as long as display_dirty is unsigned type
}

static void calc_bounds(uint8_t update_start, uint8_t *cmd_array)
{
    cmd_array[1] = OLED_BLOCK_SIZE * update_start % OLED_DISPLAY_WIDTH;
    cmd_array[4] = OLED_BLOCK_SIZE * update_start / OLED_DISPLAY_WIDTH;
    cmd_array[2] = (OLED_BLOCK_SIZE + OLED_DISPLAY_WIDTH - 1) % OLED_DISPLAY_WIDTH + cmd_array[1];
    cmd_array[5] = (OLED_BLOCK_SIZE + OLED_DISPLAY_WIDTH - 1) / OLED_DISPLAY_WIDTH - 1;
}

static void calc_bounds_90(uint8_t update_start, uint8_t *cmd_array)
{
    cmd_array[1] = OLED_BLOCK_SIZE * update_start / OLED_DISPLAY_HEIGHT * 8;
    cmd_array[4] = OLED_BLOCK_SIZE * update_start % OLED_DISPLAY_HEIGHT;
    cmd_array[2] = (OLED_BLOCK_SIZE + OLED_DISPLAY_HEIGHT - 1) / OLED_DISPLAY_HEIGHT * 8 - 1 + cmd_array[1];;
    cmd_array[5] = (OLED_BLOCK_SIZE + OLED_DISPLAY_HEIGHT - 1) % OLED_DISPLAY_HEIGHT / 8;
}

uint8_t crot(uint8_t a, int8_t n)
{
    const uint8_t mask = 0x7;
    n &= mask;
    return a << n | a >> (-n & mask);
}

static void rotate_90(const uint8_t *src, uint8_t *dest)
{
    for (uint8_t i = 0, shift = 7; i < 8; ++i, --shift) {
        uint8_t selector = (1 << i);
        for (uint8_t j = 0; j < 8; ++j) {
            dest[i] |= crot(src[j] & selector, shift - (int8_t)j);
        }
    }
}

void oled_render(void)
{
    // Do we have work to do?
    if (!oled_dirty || oled_scrolling) {
        return;
    }

    // Find first dirty block
    uint8_t update_start = 0;
    while (!(oled_dirty & (1 << update_start))) {
        ++update_start;
    }

    // Set column & page position
    static uint8_t display_start[] = {
        I2C_CMD,
        COLUMN_ADDR, 0, OLED_DISPLAY_WIDTH - 1,
        PAGE_ADDR, 0, OLED_DISPLAY_HEIGHT / 8 - 1
    };
    if (!HAS_FLAGS(oled_rotation, OLED_ROTATION_90)) {
        calc_bounds(update_start, &display_start[1]); // Offset from I2C_CMD byte at the start
    } else {
        calc_bounds_90(update_start, &display_start[1]); // Offset from I2C_CMD byte at the start
    }

    // Send column & page position
    if (I2C_TRANSMIT(display_start) != I2C_STATUS_SUCCESS) {
        print("oled_render offset command failed\n");
        return;
    }

    if (!HAS_FLAGS(oled_rotation, OLED_ROTATION_90)) {
        // Send render data chunk as is
        if (I2C_WRITE_REG(I2C_DATA, &oled_buffer[OLED_BLOCK_SIZE * update_start], OLED_BLOCK_SIZE) != I2C_STATUS_SUCCESS) {
            print("oled_render data failed\n");
            return;
        }
    } else {
        // Rotate the render chunks
        const static uint8_t source_map[] = OLED_SOURCE_MAP;
        const static uint8_t target_map[] = OLED_TARGET_MAP;

        static uint8_t temp_buffer[OLED_BLOCK_SIZE];
        memset(temp_buffer, 0, sizeof(temp_buffer));
        for (uint8_t i = 0; i < sizeof(source_map); ++i) {
            rotate_90(&oled_buffer[OLED_BLOCK_SIZE * update_start + source_map[i]], &temp_buffer[target_map[i]]);
        }

        // Send render data chunk after rotating
        if (I2C_WRITE_REG(I2C_DATA, &temp_buffer[0], OLED_BLOCK_SIZE) != I2C_STATUS_SUCCESS) {
            print("oled_render data failed\n");
            return;
        }
    }

    // Turn on display if it is off
    oled_on();

    // Clear dirty flag
    oled_dirty &= ~(1 << update_start);
}

void oled_set_cursor(uint8_t col, uint8_t line)
{
    uint16_t index = line * oled_rotation_width + col * OLED_FONT_WIDTH;

    // Out of bounds?
    if (index >= OLED_MATRIX_SIZE) {
        index = 0;
    }

    oled_cursor = &oled_buffer[index];
}

void oled_advance_page(bool clearPageRemainder)
{
    uint16_t index = oled_cursor - &oled_buffer[0];
    uint8_t remaining = oled_rotation_width - (index % oled_rotation_width);

    if (clearPageRemainder) {
        // Remaining Char count
        remaining = remaining / OLED_FONT_WIDTH;

        // Write empty character until next line
        while (remaining--) {
            oled_write_char(' ', false);
        }
    } else {
        // Next page index out of bounds?
        if (index + remaining >= OLED_MATRIX_SIZE) {
            index = 0;
            remaining = 0;
        }

        oled_cursor = &oled_buffer[index + remaining];
    }
}

void oled_advance_char(void)
{
    uint16_t nextIndex = oled_cursor - &oled_buffer[0] + OLED_FONT_WIDTH;
    uint8_t remainingSpace = oled_rotation_width - (nextIndex % oled_rotation_width);

    // Do we have enough space on the current line for the next character
    if (remainingSpace < OLED_FONT_WIDTH) {
        nextIndex += remainingSpace;
    }

    // Did we go out of bounds
    if (nextIndex >= OLED_MATRIX_SIZE) {
        nextIndex = 0;
    }

    // Update cursor position
    oled_cursor = &oled_buffer[nextIndex];
}

// Main handler that writes character data to the display buffer
void oled_write_char(const char data, bool invert)
{
    // Advance to the next line if newline
    if (data == '\n') {
        // Old source wrote ' ' until end of line...
        oled_advance_page(true);
        return;
    }

    // copy the current render buffer to check for dirty after
    static uint8_t oled_temp_buffer[OLED_FONT_WIDTH];
    memcpy(&oled_temp_buffer, oled_cursor, OLED_FONT_WIDTH);

    // set the reder buffer data
    uint8_t cast_data = (uint8_t)data; // font based on unsigned type for index
    if (cast_data < OLED_FONT_START || cast_data > OLED_FONT_END) {
        memset(oled_cursor, 0x00, OLED_FONT_WIDTH);
    } else {
        const uint8_t *glyph = &font[(cast_data - OLED_FONT_START) * OLED_FONT_WIDTH];
        memcpy_P(oled_cursor, glyph, OLED_FONT_WIDTH);
    }

    // Invert if needed
    if (invert) {
        InvertCharacter(oled_cursor);
    }

    // Dirty check
    if (memcmp(&oled_temp_buffer, oled_cursor, OLED_FONT_WIDTH)) {
        uint16_t index = oled_cursor - &oled_buffer[0];
        oled_dirty |= (1 << (index / OLED_BLOCK_SIZE));
        // Edgecase check if the written data spans the 2 chunks
        oled_dirty |= (1 << ((index + OLED_FONT_WIDTH) / OLED_BLOCK_SIZE));
    }

    // Finally move to the next char
    oled_advance_char();
}

void oled_write(const char *data, bool invert)
{
    const char *end = data + strlen(data);
    while (data < end) {
        oled_write_char(*data, invert);
        data++;
    }
}

void oled_write_ln(const char *data, bool invert)
{
    oled_write(data, invert);
    oled_advance_page(true);
}

bool oled_on(void)
{
#if !defined(OLED_DISABLE_TIMEOUT)
    oled_last_activity = timer_read();
#endif

    static const uint8_t PROGMEM display_on[] = { I2C_CMD, DISPLAY_ON };
    if (!oled_active) {
        if (I2C_TRANSMIT_P(display_on) != I2C_STATUS_SUCCESS) {
            print("oled_on cmd failed\n");
            return oled_active;
        }
        oled_active = true;
    }
    return oled_active;
}

bool oled_off(void)
{
    static const uint8_t PROGMEM display_off[] = { I2C_CMD, DISPLAY_OFF };
    if (oled_active) {
        if (I2C_TRANSMIT_P(display_off) != I2C_STATUS_SUCCESS) {
            print("oled_off cmd failed\n");
            return oled_active;
        }
        oled_active = false;
    }
    return !oled_active;
}

bool oled_scroll_right(void)
{
    // Dont enable scrolling if we need to update the display
    // This prevents scrolling of bad data from starting the scroll too early after init
    if (!oled_dirty && !oled_scrolling) {
        static const uint8_t PROGMEM display_scroll_right[] = {
            I2C_CMD, SCROLL_RIGHT, 0x00, 0x00, 0x00, 0x0F, 0x00, 0xFF, ACTIVATE_SCROLL
        };
        if (I2C_TRANSMIT_P(display_scroll_right) != I2C_STATUS_SUCCESS) {
            print("oled_scroll_right cmd failed\n");
            return oled_scrolling;
        }
        oled_scrolling = true;
    }
    return oled_scrolling;
}

bool oled_scroll_left(void)
{
    // Dont enable scrolling if we need to update the display
    // This prevents scrolling of bad data from starting the scroll too early after init
    if (!oled_dirty && !oled_scrolling) {
        static const uint8_t PROGMEM display_scroll_left[] = {
            I2C_CMD, SCROLL_LEFT, 0x00, 0x00, 0x00, 0x0F, 0x00, 0xFF, ACTIVATE_SCROLL
        };
        if (I2C_TRANSMIT_P(display_scroll_left) != I2C_STATUS_SUCCESS) {
            print("oled_scroll_left cmd failed\n");
            return oled_scrolling;
        }
        oled_scrolling = true;
    }
    return oled_scrolling;
}

bool oled_scroll_off(void)
{
    if (oled_scrolling) {
        static const uint8_t PROGMEM display_scroll_off[] = { I2C_CMD, DEACTIVATE_SCROLL };
        if (I2C_TRANSMIT_P(display_scroll_off) != I2C_STATUS_SUCCESS) {
            print("oled_scroll_off cmd failed\n");
            return oled_scrolling;
        }
        oled_scrolling = false;
    }
    return !oled_scrolling;
}

uint8_t oled_max_chars(void)
{
    if (!HAS_FLAGS(oled_rotation, OLED_ROTATION_90)) {
        return OLED_DISPLAY_WIDTH / OLED_FONT_WIDTH;
    }
    return OLED_DISPLAY_HEIGHT / OLED_FONT_WIDTH;
}

uint8_t oled_max_lines(void)
{
    if (!HAS_FLAGS(oled_rotation, OLED_ROTATION_90)) {
        return OLED_DISPLAY_HEIGHT / OLED_FONT_HEIGHT;
    }
    return OLED_DISPLAY_WIDTH / OLED_FONT_HEIGHT;
}

void oled_task(void)
{
    if (!oled_initialized) {
        return;
    }

    oled_set_cursor(0, 0);

    oled_task_user();

    // Smart render system, no need to check for dirty
    oled_render();

    // Display timeout check
#if !defined(OLED_DISABLE_TIMEOUT)
    if (oled_active && timer_elapsed(oled_last_activity) > OLED_TIMEOUT) {
        oled_off();
    }
#endif
}

__attribute__((weak))
void oled_task_user(void)
{
}
