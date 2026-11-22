#include "formats/af.h"
#include "resources/af.h"
#include "resources/modmanager.h"
#include "resources/sprite.h"
#include <string.h>

void af_create(af *a, void *src, str *name) {
    sd_af_file *sdaf = (sd_af_file *)src;

    // Trivial stuff
    a->id = sdaf->fighter_id;
    a->endurance = sdaf->endurance;
    a->health = sdaf->health;
    a->forward_speed = sdaf->forward_speed;
    a->reverse_speed = sdaf->reverse_speed;
    a->jump_speed = sdaf->jump_speed;
    a->fall_speed = sdaf->fall_speed;

    // Sound translation table
    memcpy(a->sound_translation_table, sdaf->sound_table, 30);

    // Set defaults like master.dat
    // TODO: These may change according to pilot and HAR... Find out how.
    a->sound_translation_table[25] = 0;
    a->sound_translation_table[26] = 0;
    a->sound_translation_table[27] = 0;

    array_create(&a->moves);
    array_create(&a->sprites);
    a->original_idle = NULL;
    a->original_throw = NULL;
    a->original_standing_attacks[0] = NULL;
    a->original_standing_attacks[1] = NULL;
    memset(a->original_mech_gief_moves, 0, sizeof(a->original_mech_gief_moves));
    memset(a->mech_gief_reactions, 0, sizeof(a->mech_gief_reactions));

    // Moves
    for(int i = 0; i < MAX_AF_MOVES; i++) {
        sd_move *sd_move_ptr = sd_af_get_move(sdaf, i);
        if(sd_move_ptr != NULL) {
            af_move *move = omf_calloc(1, sizeof(af_move));
            af_move_create(name, move, &a->sprites, (void *)sd_move_ptr, i);
            array_set(&a->moves, i, move);
        }
    }

    modmanager_get_fighter_header(name, a);
}

af_move *af_get_move(const af *a, int id) {
    return array_get(&a->moves, id);
}

void af_free(af *a) {
    iterator it;
    af_move *move = NULL;
    array_iter_begin(&a->moves, &it);
    foreach(it, move) {
        af_move_free(move);
        omf_free(move);
    }
    if(a->original_idle) {
        // Some base animations reference surfaces owned by the original idle.
        animation_free(a->original_idle);
        omf_free(a->original_idle);
    }
    if(a->original_throw) {
        animation_free(a->original_throw);
        omf_free(a->original_throw);
    }
    for(int i = 0; i < 2; i++) {
        if(a->original_standing_attacks[i]) {
            animation_free(a->original_standing_attacks[i]);
            omf_free(a->original_standing_attacks[i]);
        }
    }
    for(size_t i = 0; i < sizeof(a->original_mech_gief_moves) / sizeof(a->original_mech_gief_moves[0]); i++) {
        if(a->original_mech_gief_moves[i]) {
            animation_free(a->original_mech_gief_moves[i]);
            omf_free(a->original_mech_gief_moves[i]);
        }
    }
    for(int i = 0; i < MECH_GIEF_HIT_COUNT; i++) {
        if(a->mech_gief_reactions[i]) {
            animation_free(a->mech_gief_reactions[i]);
            omf_free(a->mech_gief_reactions[i]);
        }
    }
    array_free(&a->moves);
    array_free(&a->sprites);
}
