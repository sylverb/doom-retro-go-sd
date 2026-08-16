//
// rg_input.h — retro-go's odroid_input types, vendored so the core reads input
// the retro-go way (odroid_input_read_gamepad into an odroid_gamepad_state_t)
// without including the firmware tree. These ARE retro-go symbols.
//
#ifndef RG_INPUT_H
#define RG_INPUT_H

#include <stdint.h>

/* EXACT mirror of real retro-go's enum + struct (offsets/size are ABI: the
 * firmware fills this through the table). Slot SEMANTICS on the G&W, straight
 * from retro-go's odroid_input.c:
 *   START  <- physical GAME      SELECT <- physical TIME
 *   VOLUME <- physical PAUSE/SET (the overlay/menu trigger; MENU is unused)
 *   X      <- physical START     Y      <- physical SELECT  (Zelda model)
 */
typedef enum {
    ODROID_INPUT_UP = 0,
    ODROID_INPUT_RIGHT,
    ODROID_INPUT_DOWN,
    ODROID_INPUT_LEFT,
    ODROID_INPUT_SELECT,
    ODROID_INPUT_START,
    ODROID_INPUT_A,
    ODROID_INPUT_B,
    ODROID_INPUT_MENU,
    ODROID_INPUT_VOLUME,
    ODROID_INPUT_POWER,
    ODROID_INPUT_X,
    ODROID_INPUT_Y,
    ODROID_INPUT_MAX
} odroid_input_t;

typedef struct {
    uint8_t values[ODROID_INPUT_MAX];
    uint16_t bitmask;
} odroid_gamepad_state_t;

void odroid_input_read_gamepad(odroid_gamepad_state_t *out_state);

#endif // RG_INPUT_H
