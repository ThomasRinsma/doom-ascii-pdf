//
// Copyright(C) 2022-2024 Wojciech Graj
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     terminal-specific code
//

#include <emscripten.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(WIN32)
#define OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

#define CLK CLOCK_REALTIME

#ifdef __GNUC__
#define UNLIKELY(x) __builtin_expect((x), 0)
#else
#define UNLIKELY(x) (x)
#endif

#define CALL(stmt, format)                      \
	do {                                    \
		if (UNLIKELY(stmt))             \
			I_Error(format, errno); \
	} while (0)
#define CALL_STDOUT(stmt, format) CALL((stmt) == EOF, format)

#define BYTE_TO_TEXT(buf, byte)                      \
	do {                                         \
		*(buf)++ = '0' + (byte) / 100u;      \
		*(buf)++ = '0' + (byte) / 10u % 10u; \
		*(buf)++ = '0' + (byte) % 10u;       \
	} while (0)

#define GRAD_LEN 70u
#define INPUT_BUFFER_LEN 16u
#define EVENT_BUFFER_LEN ((INPUT_BUFFER_LEN)*2u - 1u)

static const char grad[] = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";

struct color_t {
	uint32_t b : 8;
	uint32_t g : 8;
	uint32_t r : 8;
	uint32_t a : 8;
};

static char *output_buffer;
static size_t output_buffer_size;

static unsigned char input_buffer[INPUT_BUFFER_LEN];
static uint16_t event_buffer[EVENT_BUFFER_LEN];
static uint16_t *event_buf_loc;

static void decrementKeyCtrs();

void DG_Init()
{
	/* Longest SGR code: \033[38;2;RRR;GGG;BBBm (length 19)
	 * Maximum 21 bytes per pixel: SGR + 2 x char
	 * 1 Newline character per line
	 * SGR clear code: \033[0m (length 4)
	 */
	output_buffer_size = 21u * DOOMGENERIC_RESX * DOOMGENERIC_RESY + DOOMGENERIC_RESY + 4u;
	output_buffer = malloc(output_buffer_size);

	memset(input_buffer, '\0', INPUT_BUFFER_LEN);
}

void DG_DrawFrame()
{
	/* Clear screen if first frame */
	// static bool first_frame = true;
	// if (first_frame) {
	// 	first_frame = false;
	// 	CALL_STDOUT(fputs("\033[1;1H\033[2J", stdout), "DG_DrawFrame: fputs error %d");
	// }

	/* fill output buffer */
	// uint32_t color = 0xFFFFFF00;
	unsigned row, col;
	struct color_t *pixel = (struct color_t *)DG_ScreenBuffer;
	char *buf = output_buffer;

	for (row = 0; row < DOOMGENERIC_RESY; row++) {
		for (col = 0; col < DOOMGENERIC_RESX; col++) {
			/*
			if ((color ^ *(uint32_t *)pixel) & 0x00FFFFFF) {
				*buf++ = '\033';
				*buf++ = '[';
				*buf++ = '3';
				*buf++ = '8';
				*buf++ = ';';
				*buf++ = '2';
				*buf++ = ';';
				BYTE_TO_TEXT(buf, pixel->r);
				*buf++ = ';';
				BYTE_TO_TEXT(buf, pixel->g);
				*buf++ = ';';
				BYTE_TO_TEXT(buf, pixel->b);
				*buf++ = 'm';
				color = *(uint32_t *)pixel;
			}
			*/
			char v_char = grad[(pixel->r + pixel->g + pixel->b) * GRAD_LEN / 766u];
			*buf++ = v_char;
			*buf++ = v_char;
			pixel++;
		}
		*buf++ = '\n';
	}
	// *buf++ = '\033';
	// *buf++ = '[';
	// *buf++ = '0';
	// *buf = 'm';

	// /* move cursor to top left corner and set bold text*/
	// CALL_STDOUT(fputs("\033[;H\033[1m", stdout), "DG_DrawFrame: fputs error %d");

	// /* flush output buffer */
	// CALL_STDOUT(fputs(output_buffer, stdout), "DG_DrawFrame: fputs error %d");
	printf("%s", output_buffer);

	// /* clear output buffer */
	memset(output_buffer, '\0', buf - output_buffer + 1u);

	EM_ASM(
		draw_frame();
	);

	decrementKeyCtrs();
}

void DG_SleepMs(const uint32_t ms)
{
	// NOP, not called, the DOOM loop is replaced with a setInterval
}

uint32_t DG_GetTicksMs()
{
	// return emscripten_run_script("return Date.now()");
	uint32_t ticks = EM_ASM_INT(
		return Date.now();
	);
	return ticks;
}


#define KEYQUEUE_SIZE 16

static unsigned short s_KeyQueue[KEYQUEUE_SIZE];
static unsigned int s_KeyQueueWriteIndex = 0;
static unsigned int s_KeyQueueReadIndex = 0;

static void addKeyToQueue(int pressed, unsigned char key){
  unsigned short keyData = (pressed << 8) | key;

  s_KeyQueue[s_KeyQueueWriteIndex] = keyData;
  s_KeyQueueWriteIndex++;
  s_KeyQueueWriteIndex %= KEYQUEUE_SIZE;
}


// to store counters for holding direction keys, it's a hack.
#define KEY_HOLD_KEYS 7
#define KEY_HOLD_FRAMES 2
char const keyHoldKeys[KEY_HOLD_KEYS] = {
	KEY_FIRE, KEY_USE, KEY_ENTER,
	KEY_LEFTARROW, KEY_RIGHTARROW, KEY_UPARROW, KEY_DOWNARROW};
int keyHold[KEY_HOLD_KEYS];

void pressKey(char key) {
	int j;

	// add DOWN event 
	addKeyToQueue(1, key);

	// Make sure an UP event fires after KEY_HOLD_FRAMES if needed
	for (j = 0; j < KEY_HOLD_KEYS; ++j) {
		if (keyHoldKeys[j] == key) {
			keyHold[j] = KEY_HOLD_FRAMES;
			break;
		}
	}
}

static void decrementKeyCtrs() {
	int i;

	// decrement the hold delays for the hold keys
	for (i = 0; i < KEY_HOLD_KEYS; ++i) {
		if (keyHold[i] > 0) {
			keyHold[i]--;
			if (keyHold[i] == 0) {
				// unpress after the hold delay is over
				addKeyToQueue(0, keyHoldKeys[i]);
			}
		}
	}
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
  if (s_KeyQueueReadIndex == s_KeyQueueWriteIndex){
    //key queue is empty
    return 0;
  } else {
    unsigned short keyData = s_KeyQueue[s_KeyQueueReadIndex];
    s_KeyQueueReadIndex++;
    s_KeyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;

    return 1;
  }

  return 0;
}

void DG_SetWindowTitle(const char *const title)
{
	// nop
}
