#include "game/protos/scene.h"
#include "formats/error.h"
#include "game/game_player.h"
#include "game/game_state_type.h"
#include "resources/af_loader.h"
#include "resources/bk_loader.h"
#include "resources/ids.h"
#include "resources/resource_files.h"
#include "utils/allocator.h"
#include "utils/log.h"
#include "utils/path.h"
#include "utils/vec.h"
#include "video/vga_state.h"
#include "video/video.h"
#include <stdlib.h>
#include <string.h>

// Some internal functions
void cb_scene_spawn_object(object *parent, int id, vec2i pos, vec2f vel, uint8_t mp_flags, int s, int g,
                           void *userdata);
void cb_scene_destroy_object(object *parent, int id, void *userdata);

// Loads BK file etc.
int scene_create(scene *scene, game_state *gs, int scene_id) {
    if(scene_id == SCENE_NONE) {
        return 1;
    }

    path bk_filename;
    switch(scene_id) {
        case SCENE_TRN_CUTSCENE: {
            game_player *player = game_state_get_player(gs, 0);
            if(player && player->chr && player->chr->bk_name[0] != '\0') {
                bk_filename = get_resource_filename(player->chr->bk_name);
                path_dossify_filename(&bk_filename);
                break;
            }
            log_error("Not a valid time to be going to SCENE_TRN_CUTSCENE");
            return 1;
        }
        default:
            bk_filename = get_resource_filename(get_resource_file(BK_INTRO + (scene_id - 1)));
            break;
        case SCENE_SCOREBOARD:
            bk_filename = get_resource_filename("MAIN.BK");
            break;
        case SCENE_LOBBY:
            bk_filename = get_resource_filename("NETARENA.PCX");
            break;
    }

    // Load BK
    scene->bk_data = omf_calloc(1, sizeof(bk));
    if(load_bk_file(scene->bk_data, &bk_filename)) {
        log_error("Unable to load scene %s (%s)!", scene_get_name(scene_id), path_c(&bk_filename));
        omf_free(scene->bk_data);
        return 1;
    }
    scene->id = scene_id;
    scene->gs = gs;
    scene->af_data[0] = NULL;
    scene->af_data[1] = NULL;
    scene->static_ticks_since_start = 0;
    scene->secret_activated = false;

    // Init functions
    scene->userdata = NULL;
    scene->free = NULL;
    scene->event = NULL;
    scene->render = NULL;
    scene->render_overlay = NULL;
    scene->dynamic_tick = NULL;
    scene->static_tick = NULL;
    scene->input_poll = NULL;
    scene->startup = NULL;
    scene->prio_override = NULL;
    scene->debug = NULL;

    // Set base palette
    vga_state_set_base_palette_from(bk_get_palette(scene->bk_data, 0));
    vga_state_set_remaps_from(bk_get_remaps(scene->bk_data, 0));

    // Set menu colors to the correct position
    palette_set_menu_colors();

    // Index 0 is always black.
    vga_color c = {0, 0, 0};
    vga_state_set_base_palette_index(0, &c);

    // All done.
    log_debug("Loaded scene %s (%s).", scene_get_name(scene_id), path_c(&bk_filename));
    return 0;
}

static sprite *scene_create_mech_gief_idle_pose(const surface *base, int dip) {
    // Leave the feet and lower legs fixed. Compress only the upper part of
    // the sprite, keeping the same origin and palette indices across frames.
    const int fixed_y = 88;
    surface *pixels = omf_calloc(1, sizeof(surface));
    surface_create(pixels, base->w, base->h);
    for(int y = dip; y < base->h; y++) {
        int source_y = y < fixed_y ? (y - dip) * fixed_y / (fixed_y - dip) : y;
        for(int x = 0; x < base->w; x++) {
            pixels->data[y * pixels->w + x] = base->data[source_y * base->w + x];
        }
    }

    sprite *pose = omf_calloc(1, sizeof(sprite));
    sprite_create_custom(pose, vec2i_create(-64, -106), pixels);
    pose->owned = true;
    pose->hit_all_opaque_pixels = true;
    return pose;
}

static sprite *scene_load_mech_gief_pose(const char *resource) {
    path filename = get_resource_filename(resource);
    size_t size = 0;
    if(!path_filesize(&filename, &size) || size == 0) {
        return NULL;
    }

    char *png = omf_malloc(size);
    sd_vga_image image = {0};
    bool loaded = path_read_file(&filename, png, size) &&
                  sd_vga_image_from_png_in_memory(&image, (const unsigned char *)png, size, true, NULL) == SD_SUCCESS;
    omf_free(png);
    if(!loaded || image.w != 144 || image.h != 120) {
        if(image.data) {
            sd_vga_image_free(&image);
        }
        return NULL;
    }

    sprite *pose = omf_calloc(1, sizeof(sprite));
    surface *pixels = omf_calloc(1, sizeof(surface));
    surface_create_from_data(pixels, image.w, image.h, (const unsigned char *)image.data);
    sd_vga_image_free(&image);
    // The stock palette reserves 16 shades per armor channel. Leave the
    // darkest outlines, visor highlights, and orange flame effects fixed.
    if(strncmp(resource, "mech-gief-flame-effect-", sizeof("mech-gief-flame-effect-") - 1) != 0) {
        for(int i = 0; i < pixels->w * pixels->h; i++) {
            const unsigned char color = pixels->data[i];
            if(color >= 176 && color <= 182) {
                pixels->data[i] = PRIMARY * 16 + 2 * (color - 176) + 2;
            } else if(color >= 210 && color <= 221) {
                pixels->data[i] = SECONDARY * 16 + color - 208;
            } else if(color >= 200 && color <= 205) {
                pixels->data[i] = TERTIARY * 16 + 2 * (color - 200) + 2;
            }
        }
    }
    sprite_create_custom(pose, vec2i_create(-64, -106), pixels);
    pose->owned = true;
    pose->hit_all_opaque_pixels = true;
    return pose;
}

animation *scene_create_mech_gief_preview(void) {
    sprite *pose = scene_load_mech_gief_pose("mech-gief-idle.png");
    if(!pose) {
        return NULL;
    }
    animation *preview = create_animation_from_single(pose, vec2i_create(0, 0));
    str_set_c(&preview->animation_string, "A1");
    return preview;
}

static void scene_load_mech_gief_idle(af *fighter) {
    af_move *idle = af_get_move(fighter, ANIM_IDLE);
    if(!idle) {
        return;
    }
    sprite *pose = scene_load_mech_gief_pose("mech-gief-idle.png");
    if(!pose) {
        log_warn("Mech Gief idle sprite unavailable, using base animation.");
        return;
    }

    animation *replacement = create_animation_from_single(pose, idle->ani.start_pos);
    replacement->id = ANIM_IDLE;
    animation_append_sprite(replacement, scene_create_mech_gief_idle_pose(pose->data, 2));
    animation_append_sprite(replacement, scene_create_mech_gief_idle_pose(pose->data, 4));
    str_set_c(&replacement->animation_string, "A8-B5-C7-B5");

    // Keep the original surfaces alive for other animations that reference them.
    fighter->original_idle = replacement;
    animation original = idle->ani;
    idle->ani = *replacement;
    *replacement = original;
}

static sprite *scene_rotate_mech_gief_spin(const sprite *source) {
    const surface *base = source->data;
    surface *pixels = omf_calloc(1, sizeof(surface));
    surface_create(pixels, base->h, base->w);
    for(int y = 0; y < base->h; y++) {
        for(int x = 0; x < base->w; x++) {
            pixels->data[x * pixels->w + base->h - 1 - y] = base->data[y * base->w + x];
        }
    }

    // Rotate around the grip from anchors.json, keeping its world offset
    // at (+9, -50) after each quarter turn.
    int grip_x = 9 - source->pos.x;
    int grip_y = -50 - source->pos.y;
    sprite *pose = omf_calloc(1, sizeof(sprite));
    sprite_create_custom(pose, vec2i_create(9 - (base->h - 1 - grip_y), -50 - grip_x), pixels);
    pose->owned = true;
    pose->hit_all_opaque_pixels = true;
    return pose;
}

static void scene_load_mech_gief_piledriver(af *fighter) {
    af_move *grab = af_get_move(fighter, MECH_GIEF_GRAB);
    if(!grab || af_get_move(fighter, MECH_GIEF_PILEDRIVER)) {
        return;
    }
    static const char *const resources[] = {
        "mech-gief-grab.png", "mech-gief-airborne-spin.png", "mech-gief-slam.png", "mech-gief-recovery.png",
    };
    sprite *poses[4];
    for(int i = 0; i < 4; i++) {
        poses[i] = scene_load_mech_gief_pose(resources[i]);
        if(!poses[i]) {
            for(int j = 0; j < i; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Mech Gief Piledriver sprites unavailable, using base throw.");
            return;
        }
    }

    animation *startup = create_animation_from_single(sprite_copy(poses[0]), grab->ani.start_pos);
    startup->id = MECH_GIEF_GRAB;
    // A can connect. B holds the same pose during punishable recovery.
    animation_append_sprite(startup, sprite_copy(poses[0]));
    str_set_c(&startup->animation_string, "A5-B22");
    // Preserve references into the stock throw's shared sprite surfaces.
    fighter->original_throw = startup;
    animation original = grab->ani;
    grab->ani = *startup;
    *startup = original;

    af_move *finish = omf_calloc(1, sizeof(af_move));
    animation *sequence = create_animation_from_single(poses[0], grab->ani.start_pos);
    for(int i = 1; i < 4; i++) {
        animation_append_sprite(sequence, poses[i]);
    }
    sprite *spin = poses[1];
    for(int turn = 0; turn < 3; turn++) {
        spin = scene_rotate_mech_gief_spin(spin);
        animation_append_sprite(sequence, spin);
    }
    sequence->id = MECH_GIEF_PILEDRIVER;
    // Lift the rendered pose while the fighter stays anchored within arena bounds.
    str_set_c(&sequence->animation_string, "zzA5-zzs24l40sp12oy-38B4-zzoy-62E4-zzoy-84F5-zzoy-66G4-zzoy-38B4-bb7s25l50sp12C6-D14");
    // Restore Nova's stock throw impact sound for this fighter instance.
    fighter->sound_translation_table[25] = 70;
    finish->id = MECH_GIEF_PILEDRIVER;
    finish->ani = *sequence;
    omf_free(sequence);
    str_from_c(&finish->move_string, "!");
    str_create(&finish->footer_string);
    array_set(&fighter->moves, finish->id, finish);

    str_set_c(&grab->move_string, "P632");
    grab->next_move = MECH_GIEF_PILEDRIVER;
    grab->successor_id = 35;
    grab->throw_duration = 26;
    grab->damage = (grab->damage * 115 + 50) / 100;
    // Relative to the victim's mirrored facing, x-9 gives the attacker's
    // grip X. During the rise the renderer pins the victim to the
    // airborne grip, so it shares Mech Gief's visual lift.
    str_set_c(&grab->footer_string,
              "ex-9y+8D5-efx-9D21-efx-9y+13D4-L6-M20");
}

// Collision samples are relative to the fighter origin and mirror with facing.
typedef struct {
    int left, top, right, bottom;
} mech_gief_hit_region;

static void scene_add_mech_gief_hit_region(animation *ani, mech_gief_hit_region region, int active_frame) {
    for(int y = region.top; y <= region.bottom; y += 4) {
        for(int x = region.left; x <= region.right; x += 4) {
            collision_coord point = {.pos = {x, y}, .frame_index = active_frame};
            vector_append(&ani->collision_coords, &point);
        }
    }
}

static void scene_load_mech_gief_standing_attacks(af *fighter) {
    static const struct {
        int move_id;
        const char *poses[3];
        const char *timing;
        mech_gief_hit_region hit_regions[2];
    } attacks[] = {
        {MECH_GIEF_NEUTRAL_PUNCH, {"mech-gief-punch-startup.png", "mech-gief-punch-active.png", "mech-gief-punch-recovery.png"},
         "A5-cps5l40sp7B4-C10", {{0, -68, 40, -52}, {36, -76, 76, -60}}},
        {MECH_GIEF_NEUTRAL_KICK, {"mech-gief-kick-startup.png", "mech-gief-kick-active.png", "mech-gief-kick-recovery.png"},
         "A7-cps5l40sp7B4-C13", {{0, -58, 36, -34}, {32, -62, 68, -42}}},
    };
    for(int i = 0; i < 2; i++) {
        af_move *move = af_get_move(fighter, attacks[i].move_id);
        if(!move) {
            continue;
        }

        sprite *poses[3];
        int count = 0;
        while(count < 3 && (poses[count] = scene_load_mech_gief_pose(attacks[i].poses[count])) != NULL) {
            count++;
        }
        if(count != 3) {
            for(int j = 0; j < count; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Mech Gief standing attack sprites unavailable, using base animation.");
            continue;
        }

        animation *pose = create_animation_from_single(poses[0], move->ani.start_pos);
        pose->id = move->id;
        animation_append_sprite(pose, poses[1]);
        animation_append_sprite(pose, poses[2]);
        str_set_c(&pose->animation_string, attacks[i].timing);
        for(int j = 0; j < 2; j++) {
            scene_add_mech_gief_hit_region(pose, attacks[i].hit_regions[j], 1);
        }

        // Keep the stock animation alive for any shared sprite references.
        fighter->original_standing_attacks[i] = pose;
        animation original = move->ani;
        move->ani = *pose;
        *pose = original;
    }
}

static void scene_load_mech_gief_move_animations(af *fighter) {
    static const struct {
        int id;
        const char *poses[4];
        const char *timing;
        mech_gief_hit_region hit_regions[2];
        int hit_region_count;
        int active_frame;
    } moves[] = {
        [MECH_GIEF_JUMP_SLOT] = {ANIM_JUMPING,
         {"mech-gief-jump-anticipation.png", "mech-gief-jump-takeoff.png", "mech-gief-jump-apex.png",
          "mech-gief-jump-landing.png"},
         "A3-B7-C120", {{0, 0, 0, 0}}, 0},
        {MECH_GIEF_BODY_PRESS,
         {"mech-gief-body-press-startup.png", "mech-gief-body-press-active.png",
          "mech-gief-body-press-follow-through.png", "mech-gief-body-press-recovery.png"},
         "A5-cps5l37sp7B8-C4-D6", {{-36, -56, 36, -24}, {32, -60, 60, -36}}, 2},
        {MECH_GIEF_DROPKICK,
         {"mech-gief-dropkick-startup.png", "mech-gief-dropkick-active.png",
          "mech-gief-dropkick-follow-through.png", "mech-gief-dropkick-recovery.png"},
         "A6-cps5l37sp7B7-C5-D7", {{-12, -52, 40, -24}, {36, -72, 72, -20}}, 2},
        {ANIM_WALKING,
         {"mech-gief-walk-near-contact.png", "mech-gief-walk-far-passing.png", "mech-gief-walk-far-contact.png",
          "mech-gief-walk-near-passing.png"},
         "A7-B6-C7-D6", {{0, 0, 0, 0}}, 0},
        [MECH_GIEF_CROUCH_SLOT] = {ANIM_CROUCHING,
         {"mech-gief-crouch-lower.png", "mech-gief-crouch-deep.png", "mech-gief-crouch-hold.png",
          "mech-gief-crouch-rise.png"},
         "A4-B12-C12-d4C1", {{0, 0, 0, 0}}, 0},
        {MECH_GIEF_CROUCH_PUNCH, // Down + Punch.
         {"mech-gief-crouch-punch-startup.png", "mech-gief-crouch-punch-active.png", "mech-gief-crouch-punch-retract.png",
          "mech-gief-crouch-punch-recovery.png"},
         "A5-cps5l33sp7B4-C3-D4", {{0, -53, 36, -37}, {32, -57, 60, -41}}, 2},
        {MECH_GIEF_CROUCH_KICK, // Down + Kick.
         {"mech-gief-crouch-kick-startup.png", "mech-gief-crouch-kick-active.png", "mech-gief-crouch-kick-retract.png",
          "mech-gief-crouch-kick-recovery.png"},
         "A6-cps5l36sp7B5-C4-D5", {{0, -38, 36, -18}, {32, -38, 64, -14}}, 2},
        [MECH_GIEF_STANDING_BLOCK_SLOT] = {ANIM_STANDING_BLOCK,
         {"mech-gief-standing-block-start.png", "mech-gief-standing-block-brace.png", "mech-gief-standing-block-end.png"},
         "A1-B1-C1", {{0, 0, 0, 0}}, 0},
        [MECH_GIEF_CROUCH_BLOCK_SLOT] = {ANIM_CROUCHING_BLOCK,
         {"mech-gief-crouch-block-start.png", "mech-gief-crouch-block-brace.png", "mech-gief-crouch-block-end.png"},
         "A1-B1-C1", {{0, 0, 0, 0}}, 0},
        {MECH_GIEF_HEEL_SWEEP, // Down-Back + Kick.
         {"mech-gief-heel-sweep-startup.png", "mech-gief-heel-sweep-active.png", "mech-gief-heel-sweep-retract.png",
          "mech-gief-heel-sweep-recovery.png"},
         "A7-cps5l52sp7B4-C5-D7", {{0, -32, 36, -12}, {32, -28, 72, -4}}, 2},
        {MECH_GIEF_HIGH_KICK, // Back + Kick.
         {"mech-gief-high-kick-startup.png", "mech-gief-high-kick-active.png", "mech-gief-high-kick-retract.png",
          "mech-gief-high-kick-recovery.png"},
         "A6-cps5l30sp7B5-C5-D6", {{0, -80, 36, -56}, {32, -100, 56, -72}}, 2},
        {MECH_GIEF_TURNING_ELBOW, // Back + Punch.
         {"mech-gief-turning-elbow-startup.png", "mech-gief-turning-elbow-active.png",
          "mech-gief-turning-elbow-retract.png", "mech-gief-turning-elbow-recovery.png"},
         "sp7sf-19l49s5A4-aicpB4-njmC3-njmD3", {{-4, -80, 32, -48}, {24, -92, 56, -56}}, 2},
        {MECH_GIEF_LUNGING_PUNCH, // Forward + Punch.
         {"mech-gief-lunging-punch-startup.png", "mech-gief-lunging-punch-active.png",
          "mech-gief-lunging-punch-retract.png", "mech-gief-lunging-punch-recovery.png"},
         "sp7sf10l20s5A5-cpB4-njmC3-njmD4", {{0, -72, 40, -52}, {36, -76, 76, -60}}, 2},
        {MECH_GIEF_LONG_SIDE_KICK, // Forward + Kick.
         {"mech-gief-long-side-kick-startup.png", "mech-gief-long-side-kick-active.png",
          "mech-gief-long-side-kick-retract.png", "mech-gief-long-side-kick-recovery.png"},
         "sp7sf9l21s5A6-cpB4-njlC3-njlD4", {{0, -60, 36, -36}, {32, -68, 72, -44}}, 2},
        {MECH_GIEF_LOW_UPPERCUT, // Down-Forward + Punch.
         {"mech-gief-low-uppercut-1.png", "mech-gief-low-uppercut-2.png", "mech-gief-low-uppercut-3.png",
          "mech-gief-low-uppercut-4.png"},
         "A3-B2-cps5l30sp7C4-njmD5", {{0, -64, 32, -44}, {24, -88, 44, -56}}, 2, 2},
        {MECH_GIEF_HAMMERFIST, // Down-Back + Punch.
         {"mech-gief-hammerfist-1.png", "mech-gief-hammerfist-2.png", "mech-gief-hammerfist-3.png",
          "mech-gief-hammerfist-4.png"},
         "A3-B3-cps5l30sp7C5-nD5", {{0, -56, 36, -32}, {28, -48, 52, -24}}, 2, 2},
        {MECH_GIEF_HEEL_STOMP, // Down-Forward + Kick.
         {"mech-gief-heel-stomp-1.png", "mech-gief-heel-stomp-2.png", "mech-gief-heel-stomp-3.png",
          "mech-gief-heel-stomp-4.png"},
         "A3-B3-cps5l30sp7C4-njlD6", {{0, -36, 32, -12}, {28, -28, 56, -4}}, 2, 2},
        [MECH_GIEF_GET_UP_SLOT] = {ANIM_STANDUP,
         {"mech-gief-get-up-1.png", "mech-gief-get-up-2.png", "mech-gief-get-up-3.png", "mech-gief-get-up-4.png"},
         "zzA2-zzB3-zzC3-D1", {{0, 0, 0, 0}}, 0, 0},
        {ANIM_VICTORY,
         {"mech-gief-victory-1.png", "mech-gief-victory-2.png", "mech-gief-victory-3.png", "mech-gief-victory-4.png"},
         "jfA4-jfB6-jfC6-jfD18-d16jfD1", {{0, 0, 0, 0}}, 0, 0},
        {ANIM_DEFEAT,
         {"mech-gief-defeat-1.png", "mech-gief-defeat-2.png", "mech-gief-defeat-3.png", "mech-gief-defeat-4.png"},
         "A5-B6-C6-D1200", {{0, 0, 0, 0}}, 0, 0},
        {MECH_GIEF_HYDRAULIC_CRUSH,
         {"mech-gief-hydraulic-crush-1.png", "mech-gief-hydraulic-crush-2.png",
          "mech-gief-hydraulic-crush-3.png", "mech-gief-hydraulic-crush-4.png"},
         "bm10amebex+42A1-uabeA6-beB9-bes25l50sp12C5-beD12", {{0, 0, 0, 0}}, 0, 0},
    };
    _Static_assert(sizeof(moves) / sizeof(moves[0]) == MECH_GIEF_KNOCKDOWN_SLOT, "Mech Gief move slots");
    for(size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        af_move *move = af_get_move(fighter, moves[i].id);
        if(!move) {
            continue;
        }

        sprite *poses[4];
        int count = 0;
        const int pose_count = moves[i].poses[3] ? 4 : 3;
        while(count < pose_count && (poses[count] = scene_load_mech_gief_pose(moves[i].poses[count])) != NULL) {
            count++;
        }
        if(count != pose_count) {
            for(int j = 0; j < count; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Mech Gief sprites unavailable for move %d, using base animation.", move->id);
            continue;
        }

        animation *replacement = create_animation_from_single(poses[0], move->ani.start_pos);
        replacement->id = move->id;
        for(int j = 1; j < pose_count; j++) {
            animation_append_sprite(replacement, poses[j]);
        }
        str_set_c(&replacement->animation_string, moves[i].timing);
        for(int j = 0; j < moves[i].hit_region_count; j++) {
            scene_add_mech_gief_hit_region(replacement, moves[i].hit_regions[j], moves[i].active_frame ? moves[i].active_frame : 1);
        }

        // Other stock animations may still reference the original sprite surfaces.
        fighter->original_mech_gief_moves[i] = replacement;
        animation original = move->ani;
        move->ani = *replacement;
        *replacement = original;
        if(move->id == MECH_GIEF_HYDRAULIC_CRUSH) {
            str_set_c(&move->footer_string, "ex-38M7-ex-25y-64hL9-ex-32gM5-M12");
            str_set_c(&move->move_string, "P412");
        }
    }
}

static void scene_load_mech_gief_knockdown(af *fighter) {
    static const char *const names[] = {
        "mech-gief-knockdown-1.png", "mech-gief-knockdown-2.png",
        "mech-gief-knockdown-3.png", "mech-gief-knockdown-4.png",
    };
    af_move *damage = af_get_move(fighter, ANIM_DAMAGE);
    // Nova uses A through X. Add the opening recoil at Y and keep the engine's L/M/N semantics.
    if(!damage || animation_get_sprite_count(&damage->ani) != 24) {
        return;
    }
    sprite *poses[4];
    int count = 0;
    while(count < 4 && (poses[count] = scene_load_mech_gief_pose(names[count])) != NULL) {
        count++;
    }
    if(count != 4) {
        for(int i = 0; i < count; i++) {
            sprite_free(poses[i]);
            omf_free(poses[i]);
        }
        return;
    }

    sprite *first = omf_calloc(1, sizeof(sprite));
    const sprite *stock_first = animation_get_sprite(&damage->ani, 0);
    if(stock_first) {
        *first = *stock_first;
        first->owned = false;
    }
    animation *replacement = create_animation_from_single(first, damage->ani.start_pos);
    replacement->id = ANIM_DAMAGE;
    str_set_c(&replacement->animation_string, str_c(&damage->ani.animation_string));
    for(int i = 1; i < 24; i++) {
        if(i >= 11 && i <= 13) {
            const int poses_by_slot[] = {1, 3, 2};
            animation_append_sprite(replacement, poses[poses_by_slot[i - 11]]);
        } else {
            const sprite *stock = animation_get_sprite(&damage->ani, i);
            sprite *borrowed = omf_calloc(1, sizeof(sprite));
            if(stock) {
                *borrowed = *stock;
                borrowed->owned = false;
            }
            animation_append_sprite(replacement, borrowed);
        }
    }
    animation_append_sprite(replacement, poses[0]);
    fighter->original_mech_gief_moves[MECH_GIEF_KNOCKDOWN_SLOT] = replacement;
    animation original = damage->ani;
    damage->ani = *replacement;
    *replacement = original;
}

static void scene_load_mech_gief_hit_reactions(af *fighter) {
    static const char *const names[MECH_GIEF_HIT_COUNT][3] = {
        [MECH_GIEF_HIT_STANDING] =
            {"mech-gief-standing-hit-flinch.png", "mech-gief-standing-hit-recoil.png", "mech-gief-standing-hit-recover.png"},
        [MECH_GIEF_HIT_CROUCHING] =
            {"mech-gief-crouch-hit-flinch.png", "mech-gief-crouch-hit-recoil.png", "mech-gief-crouch-hit-recover.png"},
        [MECH_GIEF_HIT_AIRBORNE] =
            {"mech-gief-air-hit-flinch.png", "mech-gief-air-hit-recoil.png", "mech-gief-air-hit-recover.png"},
    };
    // Reuse the frame letters expected by the attacker's hit script.
    // Slots L onward retain the stock knockdowns and special reactions.
    static const int hit_pose[] = {0, 2, 1, 0, 2, 1, 0, 1, 2, 0, 2};
    af_move *damage = af_get_move(fighter, ANIM_DAMAGE);
    if(!damage || animation_get_sprite_count(&damage->ani) < (int)(sizeof(hit_pose) / sizeof(hit_pose[0]))) {
        return;
    }

    for(int i = 0; i < MECH_GIEF_HIT_COUNT; i++) {
        sprite *poses[3];
        int count = 0;
        while(count < 3 && (poses[count] = scene_load_mech_gief_pose(names[i][count])) != NULL) {
            count++;
        }
        if(count != 3) {
            for(int j = 0; j < count; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Mech Gief hit reaction %d unavailable, using base animation.", i);
            continue;
        }

        animation *reaction = create_animation_from_single(poses[0], damage->ani.start_pos);
        reaction->id = ANIM_DAMAGE;
        str_set_c(&reaction->animation_string, str_c(&damage->ani.animation_string));
        // B owns recovery and C owns recoil. Later slots borrow these surfaces.
        animation_append_sprite(reaction, poses[2]);
        animation_append_sprite(reaction, poses[1]);
        for(int j = 3; j < animation_get_sprite_count(&damage->ani); j++) {
            sprite *frame = omf_calloc(1, sizeof(sprite));
            if(j < (int)(sizeof(hit_pose) / sizeof(hit_pose[0]))) {
                *frame = *poses[hit_pose[j]];
            } else {
                const sprite *stock = animation_get_sprite(&damage->ani, j);
                if(stock) {
                    *frame = *stock;
                }
            }
            frame->owned = false;
            animation_append_sprite(reaction, frame);
        }
        fighter->mech_gief_reactions[i] = reaction;
    }
}

static void scene_load_mech_gief_flame_burst(af *fighter) {
    static const char *const names[] = {
        "mech-gief-flame-burst-1.png", "mech-gief-flame-burst-2.png",
        "mech-gief-flame-burst-3.png", "mech-gief-flame-burst-4.png",
        "mech-gief-flame-effect-1.png", "mech-gief-flame-effect-2.png",
        "mech-gief-flame-effect-3.png", "mech-gief-flame-effect-4.png",
    };
    // Keep Nova's Down-Back, Back + Punch input.
    af_move *burst = af_get_move(fighter, MECH_GIEF_FLAME_BURST);
    if(!burst) {
        return;
    }

    sprite *poses[8];
    int count = 0;
    while(count < 8 && (poses[count] = scene_load_mech_gief_pose(names[count])) != NULL) {
        count++;
    }
    if(count != 8) {
        for(int i = 0; i < count; i++) {
            sprite_free(poses[i]);
            omf_free(poses[i]);
        }
        return;
    }

    animation *attack = create_animation_from_single(poses[0], burst->ani.start_pos);
    animation_append_sprite(attack, poses[1]);
    animation_append_sprite(attack, poses[2]);
    animation_append_sprite(attack, sprite_copy(poses[2]));
    animation_append_sprite(attack, poses[3]);
    animation_append_sprite(attack, sprite_copy(poses[3]));
    attack->id = burst->id;
    str_set_c(&attack->animation_string, "A5-s5l30sp7B2-cpC5-D4-E2-F8");

    // Attach the effect frames to the mouth. Keep their pixels out of the HAR silhouette.
    for(int i = 4; i < 8; i++) {
        poses[i]->pos.x += 34;
        poses[i]->pos.y -= 61;
    }
    animation *effect = create_animation_from_single(poses[4], burst->ani.start_pos);
    for(int i = 5; i < 8; i++) {
        animation_append_sprite(effect, poses[i]);
    }
    fighter->mech_gief_flame_effect = effect;

    // The full jet and flicker share one hit window.
    scene_add_mech_gief_hit_region(attack, (mech_gief_hit_region){38, -80, 110, -54}, 2);
    scene_add_mech_gief_hit_region(attack, (mech_gief_hit_region){38, -79, 106, -55}, 3);

    fighter->original_mech_gief_moves[MECH_GIEF_FLAME_BURST_SLOT] = attack;
    animation original_attack = burst->ani;
    burst->ani = *attack;
    *attack = original_attack;

    burst->next_move = 0;
    burst->successor_id = 0;
    burst->damage = 22;
    // Use the same orange palette flash as Pyros and push the opponent back.
    str_set_c(&burst->footer_string, "kpd240pp55ptp6ptd1ptr40sp13s2l30A2-B3-C3-L5-M400");
}

static void scene_load_mech_gief_lariat(af *fighter) {
    static const char *const names[] = {
        "mech-gief-lariat-1.png", "mech-gief-lariat-2.png", "mech-gief-lariat-3.png", "mech-gief-lariat-4.png",
        "mech-gief-lariat-5.png", "mech-gief-lariat-6.png", "mech-gief-lariat-7.png", "mech-gief-lariat-8.png",
    };
    const af_move *side_kick = af_get_move(fighter, MECH_GIEF_LONG_SIDE_KICK);
    if(!side_kick || af_get_move(fighter, MECH_GIEF_LARIAT)) {
        return;
    }

    sprite *poses[8];
    int pose_count = 0;
    while(pose_count < 8 && (poses[pose_count] = scene_load_mech_gief_pose(names[pose_count])) != NULL) {
        pose_count++;
    }
    if(pose_count != 8) {
        for(int i = 0; i < pose_count; i++) {
            sprite_free(poses[i]);
            omf_free(poses[i]);
        }
        return;
    }

    af_move *lariat = omf_calloc(1, sizeof(af_move));
    animation *attack = create_animation_from_single(poses[0], side_kick->ani.start_pos);
    for(int i = 1; i < 8; i++) {
        animation_append_sprite(attack, poses[i]);
    }
    attack->id = MECH_GIEF_LARIAT;
    // Spin twice. The second revolution opens a fresh hit window.
    str_set_c(&attack->animation_string,
              "hA5-hzpcps5l40sp7B2-hzpC2-hzpD2-hzpE2-hzpq2cps5l40sp7B2-hzpC2-hzpD2-hzpE2-hF3-hG4-hH5");

    scene_add_mech_gief_hit_region(attack, (mech_gief_hit_region){-54, -77, 54, -55}, 1);
    scene_add_mech_gief_hit_region(attack, (mech_gief_hit_region){-54, -77, 54, -55}, 2);
    scene_add_mech_gief_hit_region(attack, (mech_gief_hit_region){-32, -75, 32, -57}, 3);
    scene_add_mech_gief_hit_region(attack, (mech_gief_hit_region){-46, -76, 54, -54}, 4);

    lariat->id = MECH_GIEF_LARIAT;
    lariat->ani = *attack;
    omf_free(attack);
    lariat->category = CAT_MEDIUM;
    lariat->damage = 26;
    lariat->block_stun = side_kick->block_stun;
    lariat->points = side_kick->points;
    str_from_c(&lariat->move_string, "K632");
    str_from_c(&lariat->footer_string, "sp13s2l35D1-E2-F5-E2-D2");
    array_set(&fighter->moves, lariat->id, lariat);
}

int scene_load_har(scene *scene, int player_id) {
    game_player *player = game_state_get_player(scene->gs, player_id);
    if(scene->af_data[player_id]) {
        af_free(scene->af_data[player_id]);
        omf_free(scene->af_data[player_id]);
    }
    scene->af_data[player_id] = omf_calloc(1, sizeof(af));

    int resource_id = har_to_resource(player->pilot->har_id);
    if(load_af_file(scene->af_data[player_id], resource_id)) {
        log_error("Unable to load HAR %s (%s)!", har_get_name(player->pilot->har_id), get_resource_name(resource_id));
        return 1;
    }

    const team_battle *team = &scene->gs->team;
    if(team->active && team->fighter[player_id] < TEAM_BATTLE_SIZE &&
       team->roster[player_id][team->fighter[player_id]] == TEAM_HAR_MECH_GIEF) {
        scene_load_mech_gief_idle(scene->af_data[player_id]);
        scene_load_mech_gief_piledriver(scene->af_data[player_id]);
        scene_load_mech_gief_standing_attacks(scene->af_data[player_id]);
        scene_load_mech_gief_move_animations(scene->af_data[player_id]);
        scene_load_mech_gief_knockdown(scene->af_data[player_id]);
        scene_load_mech_gief_hit_reactions(scene->af_data[player_id]);
        scene_load_mech_gief_flame_burst(scene->af_data[player_id]);
        scene_load_mech_gief_lariat(scene->af_data[player_id]);
    }

    log_debug("Loaded HAR %s (%s).", har_get_name(player->pilot->har_id), get_resource_name(resource_id));
    return 0;
}

void scene_init(scene *scene) {
    int m_load;
    int m_repeat;

    // Bootstrap animations
    iterator it;
    hashmap_iter_begin(&scene->bk_data->infos, &it);
    hashmap_pair *pair = NULL;
    foreach(it, pair) {
        bk_info *info = (bk_info *)pair->value;

        // Ask scene if this animation should be played on start
        scene_startup(scene, info->ani.id, &m_load, &m_repeat);

        // Start up animations
        if(m_load) {
            object *obj = omf_calloc(1, sizeof(object));
            object_create(obj, scene->gs, info->ani.start_pos, vec2f_create(0, 0));
            object_set_stl(obj, scene->bk_data->sound_translation_table);
            object_set_animation(obj, &info->ani);
            object_set_repeat(obj, m_repeat);
            object_set_spawn_cb(obj, cb_scene_spawn_object, (void *)scene);
            object_set_destroy_cb(obj, cb_scene_destroy_object, (void *)scene);
            int o_prio = scene_anim_prio_override(scene, info->ani.id);
            o_prio = (o_prio != -1) ? o_prio : RENDER_LAYER_BOTTOM;
            game_state_add_object(scene->gs, obj, o_prio, 0, 0);
            log_debug("Scene bootstrap: Animation %d started.", info->ani.id);
        }
    }

    // init tick timer
    ticktimer_init(&scene->tick_timer);
}

int scene_clone(scene *src, scene *dst, game_state *gs) {
    memcpy(dst, src, sizeof(scene));
    ticktimer_clone(&src->tick_timer, &dst->tick_timer);
    dst->gs = gs;
    if(src->clone) {
        src->clone(src, dst);
    }
    return 0;
}

int scene_clone_free(scene *sc) {
    if(sc->clone_free) {
        sc->clone_free(sc);
    }
    ticktimer_close(&sc->tick_timer);
    omf_free(sc);
    return 0;
}

void scene_set_userdata(scene *scene, void *userdata) {
    scene->userdata = userdata;
}

void *scene_get_userdata(const scene *scene) {
    return scene->userdata;
}

void scene_startup(scene *scene, int id, int *m_load, int *m_repeat) {
    *m_load = 0;
    *m_repeat = 0;
    if(scene->startup != NULL) {
        scene->startup(scene, id, m_load, m_repeat);
    }
}

bool scene_event(scene *scene, SDL_Event *event) {
    if(scene->event != NULL) {
        return scene->event(scene, event);
    }
    return false;
}

void scene_render_overlay(scene *scene) {
    if(scene->render_overlay != NULL) {
        scene->render_overlay(scene);
    }
}

void scene_render(scene *scene) {
    video_draw(&scene->bk_data->background, 0, 0);

    if(scene->render != NULL) {
        scene->render(scene);
    }
}

void scene_debug(scene *scene) {
    if(scene->debug) {
        scene->debug(scene);
    }
}

void scene_palette_transform(scene *scene) {
    if(scene->palette_transform) {
        scene->palette_transform(scene);
    }
}

int scene_anim_prio_override(scene *scene, int anim_id) {
    if(scene->prio_override != NULL) {
        return scene->prio_override(scene, anim_id);
    }
    return -1;
}

void scene_static_tick(scene *scene, int paused) {
    scene->static_ticks_since_start++;
    if(scene->static_tick != NULL) {
        scene->static_tick(scene, paused);
    }
}

void scene_dynamic_tick(scene *scene, int paused) {
    // Tick timers
    if(!paused) {
        ticktimer_run(&scene->tick_timer, scene);
    }

    if(scene->dynamic_tick != NULL) {
        scene->dynamic_tick(scene, paused);
    }
}

void scene_input_poll(scene *scene) {
    if(scene->static_ticks_since_start < 25 && !scene_is_arena(scene)) {
        // Wait a bit at scene startup so that inputs from previous scene
        // don't bleed to this one, unless we're in an arena.
        return;
    }
    if(scene->gs->this_id != scene->gs->next_id) {
        // Ignore inputs during scene fadeout
        return;
    }
    if(scene->input_poll != NULL) {
        scene->input_poll(scene);
    }
}

void scene_free(scene *scene) {
    if(scene->free != NULL) {
        scene->free(scene);
    }
    bk_free(scene->bk_data);
    omf_free(scene->bk_data);
    if(scene->af_data[0]) {
        af_free(scene->af_data[0]);
        omf_free(scene->af_data[0]);
    }
    if(scene->af_data[1]) {
        af_free(scene->af_data[1]);
        omf_free(scene->af_data[1]);
    }
    ticktimer_close(&scene->tick_timer);
}

void scene_set_free_cb(scene *scene, scene_free_cb cbfunc) {
    scene->free = cbfunc;
}

void scene_set_clone_free_cb(scene *scene, scene_clone_free_cb cbfunc) {
    scene->clone_free = cbfunc;
}

void scene_set_event_cb(scene *scene, scene_event_cb cbfunc) {
    scene->event = cbfunc;
}

void scene_set_render_cb(scene *scene, scene_render_cb cbfunc) {
    scene->render = cbfunc;
}

void scene_set_render_overlay_cb(scene *scene, scene_render_overlay_cb cbfunc) {
    scene->render_overlay = cbfunc;
}

void scene_set_debug_cb(scene *scene, scene_debug_cb cbfunc) {
    scene->debug = cbfunc;
}

void scene_set_palette_transform_cb(scene *scene, scene_palette_transform_cb cbfunc) {
    scene->palette_transform = cbfunc;
}

void scene_set_startup_cb(scene *scene, scene_startup_cb cbfunc) {
    scene->startup = cbfunc;
}

void scene_set_dynamic_tick_cb(scene *scene, scene_tick_cb cbfunc) {
    scene->dynamic_tick = cbfunc;
}

void scene_set_static_tick_cb(scene *scene, scene_tick_cb cbfunc) {
    scene->static_tick = cbfunc;
}

void scene_set_anim_prio_override_cb(scene *scene, scene_anim_prio_override_cb cbfunc) {
    scene->prio_override = cbfunc;
}

bool scene_is_arena(scene *scene) {
    return (scene->id >= SCENE_ARENA0 && scene->id <= SCENE_ARENA4);
}

void scene_set_input_poll_cb(scene *scene, scene_input_poll_cb cbfunc) {
    scene->input_poll = cbfunc;
}

void cb_scene_spawn_object(object *parent, int id, vec2i pos, vec2f vel, uint8_t mp_flags, int s, int g,
                           void *userdata) {
    scene *sc = (scene *)userdata;

    // Get next animation
    bk_info *info = bk_get_info(sc->bk_data, id);
    if(info != NULL) {
        object *obj = omf_calloc(1, sizeof(object));
        object_create(obj, parent->gs, vec2i_add(pos, info->ani.start_pos), vel);
        object_set_stl(obj, object_get_stl(parent));
        object_set_animation(obj, &info->ani);
        object_set_spawn_cb(obj, cb_scene_spawn_object, userdata);
        object_set_destroy_cb(obj, cb_scene_destroy_object, userdata);
        if(info->probability == 1) {
            object_set_repeat(obj, 1);
        }
        if(mp_flags & 0x1) {
            object_set_animation_effects(obj, EFFECT_SATURATE);
        }
        game_state_add_object(parent->gs, obj, RENDER_LAYER_BOTTOM, 0, 0);
    }
}

void cb_scene_destroy_object(object *parent, int id, void *userdata) {
    game_state_del_animation(parent->gs, id);
}
