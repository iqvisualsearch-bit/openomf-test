#ifndef AF_H
#define AF_H

#include "resources/af_move.h"
#include "utils/allocator.h"
#include "utils/array.h"
#include "utils/fixedpt.h"

enum {
    MECH_GIEF_JUMP_SLOT = 0,
    MECH_GIEF_CROUCH_SLOT = 4,
    MECH_GIEF_STANDING_BLOCK_SLOT = 7,
    MECH_GIEF_CROUCH_BLOCK_SLOT = 8,
    MECH_GIEF_MOVE_COUNT = 14,
};
enum { MECH_GIEF_HIT_STANDING, MECH_GIEF_HIT_CROUCHING, MECH_GIEF_HIT_AIRBORNE, MECH_GIEF_HIT_COUNT };

typedef struct af_t {
    unsigned int id;
    int32_t endurance;
    unsigned int health;
    fixedpt forward_speed;
    fixedpt reverse_speed;
    fixedpt jump_speed;
    fixedpt fall_speed;
    array sprites;
    array moves;
    animation *original_idle;
    animation *original_throw;
    animation *original_standing_attacks[2];
    animation *original_mech_gief_moves[MECH_GIEF_MOVE_COUNT];
    animation *mech_gief_reactions[MECH_GIEF_HIT_COUNT];
    char sound_translation_table[30];
} af;

void af_create(af *a, void *src, str *name);
af_move *af_get_move(const af *a, int id);
void af_free(af *a);

#endif // AF_H
