#include "game/protos/scene.h"
#include "formats/error.h"
#include "formats/move.h"
#include "game/game_player.h"
#include "game/game_state.h"
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

static sprite *scene_create_custom_idle_pose(const surface *base, int dip) {
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

static uint8_t scene_match_preview_color(vga_color color, const vga_palette *palette) {
    // Skip transparency, player colors, and the menu cursor colors.
    uint8_t match = 96;
    int best_distance = 3 * 255 * 255 + 1;
    for(int index = 96; index < 246; index++) {
        const vga_color candidate = palette->colors[index];
        const int r = color.r - candidate.r;
        const int g = color.g - candidate.g;
        const int b = color.b - candidate.b;
        const int distance = r * r + g * g + b * b;
        if(distance < best_distance) {
            best_distance = distance;
            match = index;
        }
    }
    return match;
}

static sprite *scene_load_mech_gief_pose_with_palette(const char *resource, const vga_palette *menu_palette) {
    path filename = get_resource_filename(resource);
    size_t size = 0;
    if(!path_filesize(&filename, &size) || size == 0) {
        return NULL;
    }

    char *png = omf_malloc(size);
    sd_vga_image image = {0};
    vga_palette source_palette = {0};
    bool loaded = path_read_file(&filename, png, size) &&
                  sd_vga_image_from_png_in_memory(&image, (const unsigned char *)png, size, true,
                                                  menu_palette ? &source_palette : NULL) == SD_SUCCESS;
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
    const bool gi_only = strncmp(resource, "shoto-", sizeof("shoto-") - 1) == 0;
    const bool pulse_orb = strncmp(resource, "shoto-pulse-orb-", sizeof("shoto-pulse-orb-") - 1) == 0;
    // The stock palette reserves 16 shades per armor channel. Leave the
    // darkest outlines, visor highlights, and orange flame effects fixed.
    if(strncmp(resource, "mech-gief-flame-effect-", sizeof("mech-gief-flame-effect-") - 1) != 0) {
        uint8_t colors[256] = {0};
        for(int color = 1; color < 256; color++) {
            if(color >= 176 && color <= 182) {
                colors[color] = PRIMARY * 16 + 2 * (color - 176) + 2;
            } else if(!gi_only && color >= 210 && color <= 221) {
                colors[color] = SECONDARY * 16 + color - 208;
            } else if(!gi_only && color >= 200 && color <= 205) {
                colors[color] = TERTIARY * 16 + 2 * (color - 200) + 2;
            } else {
                colors[color] = menu_palette ? scene_match_preview_color(source_palette.colors[color], menu_palette)
                                            : color;
            }
        }
        for(int i = 0; i < pixels->w * pixels->h; i++) {
            pixels->data[i] = colors[pixels->data[i]];
        }
    }
    sprite_create_custom(pose, vec2i_create(-64, pulse_orb ? -60 : -106), pixels);
    pose->owned = true;
    pose->hit_all_opaque_pixels = true;
    return pose;
}

static sprite *scene_load_mech_gief_pose(const char *resource) {
    return scene_load_mech_gief_pose_with_palette(resource, NULL);
}

static void scene_set_custom_idle_loop(animation *ani, const sprite *pose) {
    animation_append_sprite(ani, scene_create_custom_idle_pose(pose->data, 2));
    animation_append_sprite(ani, scene_create_custom_idle_pose(pose->data, 4));
    str_set_c(&ani->animation_string, "A8-B5-C7-B5");
}

animation *scene_create_mech_gief_preview(scene *sc, bool versus) {
    const vga_palette *palette = bk_get_palette(sc->bk_data, 0);
    sprite *pose = versus ? scene_load_mech_gief_pose_with_palette("mech-gief-versus.png", palette) : NULL;
    if(!pose) {
        pose = scene_load_mech_gief_pose_with_palette("mech-gief-idle.png", palette);
    }
    if(!pose) {
        return NULL;
    }
    animation *preview = create_animation_from_single(pose, vec2i_create(0, 0));
    if(versus) {
        str_set_c(&preview->animation_string, "A1");
    } else {
        scene_set_custom_idle_loop(preview, pose);
    }
    return preview;
}

animation *scene_create_shoto_preview(scene *sc) {
    sprite *pose = scene_load_mech_gief_pose_with_palette("shoto-idle.png", bk_get_palette(sc->bk_data, 0));
    if(!pose) {
        return NULL;
    }
    animation *preview = create_animation_from_single(pose, vec2i_create(0, 0));
    scene_set_custom_idle_loop(preview, pose);
    return preview;
}

static void scene_load_custom_idle(af *fighter, const char *resource) {
    af_move *idle = af_get_move(fighter, ANIM_IDLE);
    if(!idle) {
        return;
    }
    sprite *pose = scene_load_mech_gief_pose(resource);
    if(!pose) {
        log_warn("Idle sprite %s unavailable, using base animation.", resource);
        return;
    }

    animation *replacement = create_animation_from_single(pose, idle->ani.start_pos);
    replacement->id = ANIM_IDLE;
    scene_set_custom_idle_loop(replacement, pose);

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

static void scene_load_mech_gief_hip_toss(af *fighter) {
    const af_move *stock_throw = af_get_move(fighter, MECH_GIEF_GRAB);
    if(!stock_throw || af_get_move(fighter, MECH_GIEF_HIP_TOSS) ||
       af_get_move(fighter, MECH_GIEF_HIP_TOSS_FOLLOWTHROUGH)) {
        return;
    }

    static const char *const resources[] = {
        "mech-gief-hip-toss-reach.png",
        "mech-gief-hip-toss-grab.png",
        "mech-gief-hip-toss-toss.png",
        "mech-gief-hip-toss-recover.png",
    };
    sprite *poses[4];
    for(int i = 0; i < 4; i++) {
        poses[i] = scene_load_mech_gief_pose(resources[i]);
        if(!poses[i]) {
            for(int j = 0; j < i; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Mech Gief hip toss sprites unavailable.");
            return;
        }
    }

    af_move *reach = omf_calloc(1, sizeof(af_move));
    reach->id = MECH_GIEF_HIP_TOSS;
    reach->category = CAT_CLOSE;
    reach->extra_string_selector = ESS_SPECIAL_ARM;
    reach->successor_id = stock_throw->successor_id;
    reach->next_move = MECH_GIEF_HIP_TOSS_FOLLOWTHROUGH;
    reach->throw_duration = 7;
    reach->damage = stock_throw->damage * 3 / 4;
    reach->stun = stock_throw->stun * 3 / 4;
    reach->points = stock_throw->points;
    str_from_c(&reach->move_string, "P6");
    // Hold the opponent in front of the grip through the grab, then toss them away.
    str_from_c(&reach->footer_string, "ex-27D7-vx-5y-3L6-M20");
    animation *startup = create_animation_from_single(poses[0], stock_throw->ani.start_pos);
    startup->id = reach->id;
    animation_append_sprite(startup, sprite_copy(poses[3]));
    str_set_c(&startup->animation_string, "A4-B12");
    reach->ani = *startup;
    omf_free(startup);

    af_move *finish = omf_calloc(1, sizeof(af_move));
    finish->id = MECH_GIEF_HIP_TOSS_FOLLOWTHROUGH;
    str_from_c(&finish->move_string, "!");
    str_create(&finish->footer_string);
    animation *followthrough = create_animation_from_single(poses[1], stock_throw->ani.start_pos);
    followthrough->id = finish->id;
    animation_append_sprite(followthrough, poses[2]);
    animation_append_sprite(followthrough, poses[3]);
    str_set_c(&followthrough->animation_string, "zzA7-s25l50sp12B5-C10");
    finish->ani = *followthrough;
    omf_free(followthrough);

    array_set(&fighter->moves, reach->id, reach);
    array_set(&fighter->moves, finish->id, finish);
    fighter->sound_translation_table[25] = 70;
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
    grab->extra_string_selector = ESS_SPECIAL_ARM;
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

static void scene_load_shoto_movement(af *fighter) {
    static const struct {
        int id;
        int original_slot;
        const char *name;
        const char *timing;
    } moves[] = {
        {ANIM_WALKING, 3, "walk", "A7-B6-C7-D6"},
        {ANIM_JUMPING, MECH_GIEF_JUMP_SLOT, "jump", "A3-B7-C120"},
    };

    for(size_t i = 0; i < sizeof(moves) / sizeof(moves[0]); i++) {
        af_move *move = af_get_move(fighter, moves[i].id);
        if(!move) {
            continue;
        }
        sprite *poses[4];
        int count = 0;
        for(; count < 4; count++) {
            char filename[32];
            snprintf(filename, sizeof(filename), "shoto-%s-%d.png", moves[i].name, count + 1);
            poses[count] = scene_load_mech_gief_pose(filename);
            if(!poses[count]) {
                log_warn("Shoto movement sprite %s unavailable.", filename);
                break;
            }
        }
        if(count != 4) {
            for(int j = 0; j < count; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            continue;
        }
        animation *replacement = create_animation_from_single(poses[0], move->ani.start_pos);
        replacement->id = move->id;
        for(int j = 1; j < 4; j++) {
            animation_append_sprite(replacement, poses[j]);
        }
        str_set_c(&replacement->animation_string, moves[i].timing);

        // Use the existing custom jump and landing path, and keep shared base surfaces alive.
        fighter->original_mech_gief_moves[moves[i].original_slot] = replacement;
        animation original = move->ani;
        move->ani = *replacement;
        *replacement = original;
    }
}

static void scene_load_shoto_normals(af *fighter) {
    // Use Jaguar's hit reactions and damage as the baseline for Shoto's normals.
    static const struct {
        int id;
        const char *poses[3];
        const char *timing;
        mech_gief_hit_region strike;
        const char *command;
        int template_id;
    } moves[SHOTO_NORMAL_COUNT] = {
        {42, {"shoto-stand-punch-1.png", "shoto-stand-punch-2.png", "shoto-stand-punch-3.png"},
         "A4-cps5l40sp7B4-C7", {14, -68, 42, -56}},
        {41, {"shoto-stand-kick-1.png", "shoto-stand-kick-2.png", "shoto-stand-kick-3.png"},
         "A6-cps5l40sp7B5-C9", {12, -70, 30, -50}},
        {44, {"shoto-air-punch-1.png", "shoto-air-punch-2.png", "shoto-air-punch-3.png"},
         "A4-cps5l40sp7B6-C7", {8, -56, 28, -40}},
        {43, {"shoto-air-kick-1.png", "shoto-air-kick-2.png", "shoto-air-kick-3.png"},
         "A5-cps5l40sp7B7-C8", {17, -53, 48, -45}},
        {30, {"shoto-crouch-punch-1.png", "shoto-crouch-punch-2.png", "shoto-crouch-punch-3.png"},
         "A4-cps5l40sp7B4-C7", {16, -52, 49, -42}},
        {19, {"shoto-crouch-kick-1.png", "shoto-crouch-kick-2.png", "shoto-crouch-kick-3.png"},
         "A7-cps5l40sp7B5-C10", {16, -22, 49, -10}},
        {17, {"shoto-forward-punch-1.png", "shoto-forward-punch-2.png", "shoto-forward-punch-3.png"},
         "A5-x+4cps5l40sp7B4-C9", {28, -66, 72, -54}, "P6", 0},
        {SHOTO_FORWARD_KICK, {"shoto-forward-kick-1.png", "shoto-forward-kick-2.png", "shoto-forward-kick-3.png"},
         "A7-x+4cps5l40sp7B5-C11", {20, -66, 40, -50}, "K6", 41},
        {15, {"shoto-forward-air-punch-1.png", "shoto-forward-air-punch-2.png", "shoto-forward-air-punch-3.png"},
         "A5-cps5l40sp7B6-C8", {20, -46, 38, -28}, "P9", 0},
        {SHOTO_FORWARD_AIR_KICK, {"shoto-forward-air-kick-1.png", "shoto-forward-air-kick-2.png", "shoto-forward-air-kick-3.png"},
         "A6-cps5l40sp7B6-C9", {23, -53, 47, -37}, "K9", 43},
        {23, {"shoto-down-forward-punch-1.png", "shoto-down-forward-punch-2.png", "shoto-down-forward-punch-3.png"},
         "A5-cps5l40sp7B4-C9", {22, -78, 42, -56}, "P3", 0},
        {18, {"shoto-down-forward-kick-1.png", "shoto-down-forward-kick-2.png", "shoto-down-forward-kick-3.png"},
         "A7-cps5l40sp7B5-C10", {24, -18, 39, -2}, "K3", 0},
        {27, {"shoto-back-punch-1.png", "shoto-back-punch-2.png", "shoto-back-punch-3.png"},
         "A5-cps5l40sp7B4-C8", {10, -70, 29, -53}, "P4", 0},
        {24, {"shoto-back-kick-1.png", "shoto-back-kick-2.png", "shoto-back-kick-3.png"},
         "A7-cps5l40sp7B5-C10", {14, -83, 30, -63}, "K4", 0},
        {SHOTO_BACK_AIR_PUNCH, {"shoto-back-air-punch-1.png", "shoto-back-air-punch-2.png", "shoto-back-air-punch-3.png"},
         "A5-cps5l40sp7B5-C8", {6, -62, 18, -42}, "P7", 44},
        {SHOTO_BACK_AIR_KICK, {"shoto-back-air-kick-1.png", "shoto-back-air-kick-2.png", "shoto-back-air-kick-3.png"},
         "A6-cps5l40sp7B6-C9", {17, -26, 34, -10}, "K7", 43},
        {32, {"shoto-down-back-punch-1.png", "shoto-down-back-punch-2.png", "shoto-down-back-punch-3.png"},
         "A5-cps5l40sp7B4-C8", {12, -49, 30, -35}, "P1", 0},
        {33, {"shoto-down-back-kick-1.png", "shoto-down-back-kick-2.png", "shoto-down-back-kick-3.png"},
         "A8-cps5l40sp7B5-C12", {25, -17, 53, -3}, "K1", 0},
    };

    for(int i = 0; i < SHOTO_NORMAL_COUNT; i++) {
        af_move *move = af_get_move(fighter, moves[i].id);
        const af_move *base = moves[i].template_id ? af_get_move(fighter, moves[i].template_id) : move;
        if(!base) {
            continue;
        }
        sprite *poses[3];
        int count = 0;
        while(count < 3 && (poses[count] = scene_load_mech_gief_pose(moves[i].poses[count])) != NULL) {
            count++;
        }
        if(count != 3) {
            for(int j = 0; j < count; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Shoto normal sprite %s unavailable.", moves[i].poses[count]);
            continue;
        }

        animation *replacement = create_animation_from_single(poses[0], base->ani.start_pos);
        replacement->id = moves[i].id;
        animation_append_sprite(replacement, poses[1]);
        animation_append_sprite(replacement, poses[2]);
        str_set_c(&replacement->animation_string, moves[i].timing);
        scene_add_mech_gief_hit_region(replacement, moves[i].strike, 1);

        if(move) {
            // Other base animations can still refer to the original surfaces.
            fighter->original_shoto_normals[i] = replacement;
            animation original = move->ani;
            move->ani = *replacement;
            *replacement = original;
        } else {
            move = omf_calloc(1, sizeof(af_move));
            move->id = moves[i].id;
            move->category = base->category;
            move->points = base->points;
            move->damage = base->damage;
            move->block_damage = base->block_damage;
            move->block_stun = base->block_stun;
            move->stun = base->stun;
            move->extra_string_selector = base->extra_string_selector;
            move->ani = *replacement;
            omf_free(replacement);
            str_create(&move->move_string);
            str_from(&move->footer_string, &base->footer_string);
            array_set(&fighter->moves, move->id, move);
        }
        if(moves[i].command) {
            str_set_c(&move->move_string, moves[i].command);
            move->pos_constraints = 0;
            move->next_move = 0;
            move->successor_id = 0;
        }

        if(move->id == 33) {
            // Down-back kick sweeps the opponent into a knockdown.
            str_set_c(&move->footer_string, "sp13s2l35D1-E2-L5-M20");
        }
        if(move->id == 44) {
            af_move *stock_down_punch = af_get_move(fighter, 39);
            if(stock_down_punch && stock_down_punch->category == CAT_JUMPING) {
                str_set_c(&stock_down_punch->move_string, "!");
            }
        }
    }
}

static void scene_load_shoto_pulse_shot(af *fighter) {
    static const char *const resources[] = {
        "shoto-pulse-shot-1.png",
        "shoto-pulse-shot-2.png",
        "shoto-pulse-shot-3.png",
        "shoto-pulse-shot-4.png",
        "shoto-pulse-orb-1.png",
        "shoto-pulse-orb-2.png",
        "shoto-pulse-orb-3.png",
        "shoto-pulse-burn-1.png",
        "shoto-pulse-burn-2.png",
        "shoto-pulse-burn-3.png",
    };
    if(af_get_move(fighter, SHOTO_PULSE_SHOT) || af_get_move(fighter, SHOTO_PULSE_ORB) ||
       af_get_move(fighter, SHOTO_PULSE_BURN)) {
        return;
    }

    sprite *poses[10];
    for(int i = 0; i < 10; i++) {
        poses[i] = scene_load_mech_gief_pose(resources[i]);
        if(!poses[i]) {
            for(int j = 0; j < i; j++) {
                sprite_free(poses[j]);
                omf_free(poses[j]);
            }
            log_warn("Pulse Shot sprite %s unavailable.", resources[i]);
            return;
        }
    }

    animation *cast = create_animation_from_single(poses[0], vec2i_create(0, 0));
    cast->id = SHOTO_PULSE_SHOT;
    for(int i = 1; i < 4; i++) {
        animation_append_sprite(cast, poses[i]);
    }
    str_set_c(&cast->animation_string, "A4-B5-m64mx58my-51s22l55sp10C4-D11");

    af_move *shot = omf_calloc(1, sizeof(af_move));
    shot->id = cast->id;
    shot->category = CAT_MEDIUM;
    shot->ani = *cast;
    omf_free(cast);
    str_from_c(&shot->move_string, "P632");
    str_create(&shot->footer_string);

    animation *flight = create_animation_from_single(poses[4], vec2i_create(0, 0));
    flight->id = SHOTO_PULSE_ORB;
    animation_append_sprite(flight, poses[5]);
    animation_append_sprite(flight, poses[6]);
    str_set_c(&flight->animation_string, "vx+5A3-B3-C3-A3-B3-C3-A3-B3-C3-A3-B3-C3-A3-B3-C3-A3-B3-C3-A3-B3-C3");
    for(int i = 0; i < 3; i++) {
        scene_add_mech_gief_hit_region(flight, (mech_gief_hit_region){-8, -4, 16, 4}, i);
    }

    af_move *orb = omf_calloc(1, sizeof(af_move));
    orb->id = flight->id;
    orb->category = CAT_PROJECTILE;
    orb->damage = 15;
    orb->block_stun = 4;
    orb->points = 8;
    orb->ani = *flight;
    omf_free(flight);
    str_from_c(&orb->move_string, "!");
    str_from_format(&orb->footer_string, "pd%dptd1ptp5ptr13A2-B3-C4", SHOTO_PULSE_BURN_COLOR);

    animation *effect = create_animation_from_single(poses[7], vec2i_create(0, 0));
    effect->id = SHOTO_PULSE_BURN;
    animation_append_sprite(effect, poses[8]);
    animation_append_sprite(effect, poses[9]);
    af_move *burn = omf_calloc(1, sizeof(af_move));
    burn->id = effect->id;
    burn->ani = *effect;
    omf_free(effect);
    str_from_c(&burn->move_string, "!");
    str_create(&burn->footer_string);

    array_set(&fighter->moves, shot->id, shot);
    array_set(&fighter->moves, orb->id, orb);
    array_set(&fighter->moves, burn->id, burn);
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
        {ANIM_STUNNED,
         {"mech-gief-standing-hit-flinch.png", "mech-gief-standing-hit-recoil.png", "mech-gief-standing-hit-recover.png"},
         "A8-B9-C8-B9", {{0, 0, 0, 0}}, 0, 0},
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

    animation *replacement = create_animation_from_single(poses[0], damage->ani.start_pos);
    replacement->id = ANIM_DAMAGE;
    str_set_c(&replacement->animation_string, str_c(&damage->ani.animation_string));
    // Opponents' throw scripts address the victim by frame letter.
    // Keep L/M/N as knockdown poses and use Mech Gief's recoil for other letters.
    for(int i = 1; i <= 'Y' - 'A'; i++) {
        if(i >= 'L' - 'A' && i <= 'N' - 'A') {
            const int poses_by_slot[] = {1, 3, 2};
            animation_append_sprite(replacement, poses[poses_by_slot[i - ('L' - 'A')]]);
        } else {
            sprite *recoil = omf_calloc(1, sizeof(sprite));
            *recoil = *poses[0];
            recoil->owned = false;
            animation_append_sprite(replacement, recoil);
        }
    }
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

    int resource_id = har_to_resource(game_state_team_base_har(player->pilot->har_id));
    if(load_af_file(scene->af_data[player_id], resource_id)) {
        log_error("Unable to load HAR %s (%s)!", har_get_name(player->pilot->har_id), get_resource_name(resource_id));
        return 1;
    }

    if(game_state_selected_robot(scene->gs, player_id) == TEAM_HAR_MECH_GIEF) {
        // Nova's P252 command matches Down, neutral, Down + Punch.
        // Keep the animation data available for references, but unbind the command.
        af_move *double_down_punch = af_get_move(scene->af_data[player_id], 16);
        if(double_down_punch && str_equal_c(&double_down_punch->move_string, "P252")) {
            str_set_c(&double_down_punch->move_string, "!");
        }
        // Let Grappler's regular airborne punch handle Down + Punch as well.
        af_move *air_down_punch = af_get_move(scene->af_data[player_id], 34);
        if(air_down_punch && air_down_punch->category == CAT_JUMPING &&
           str_equal_c(&air_down_punch->move_string, "P2")) {
            str_set_c(&air_down_punch->move_string, "!");
        }
        scene_load_custom_idle(scene->af_data[player_id], "mech-gief-idle.png");
        scene_load_mech_gief_hip_toss(scene->af_data[player_id]);
        scene_load_mech_gief_piledriver(scene->af_data[player_id]);
        scene_load_mech_gief_standing_attacks(scene->af_data[player_id]);
        scene_load_mech_gief_move_animations(scene->af_data[player_id]);
        scene_load_mech_gief_knockdown(scene->af_data[player_id]);
        scene_load_mech_gief_hit_reactions(scene->af_data[player_id]);
        scene_load_mech_gief_flame_burst(scene->af_data[player_id]);
        scene_load_mech_gief_lariat(scene->af_data[player_id]);
    } else if(game_state_selected_robot(scene->gs, player_id) == TEAM_HAR_SHOTO) {
        scene_load_custom_idle(scene->af_data[player_id], "shoto-idle.png");
        scene_load_shoto_movement(scene->af_data[player_id]);
        scene_load_shoto_normals(scene->af_data[player_id]);
        scene_load_shoto_pulse_shot(scene->af_data[player_id]);
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
