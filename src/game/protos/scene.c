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
    sprite_create_custom(pose, vec2i_create(-64, -106), pixels);
    pose->owned = true;
    return pose;
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
    af_move *idle = af_get_move(fighter, ANIM_IDLE);
    if(fighter->original_idle && idle && animation_get_sprite_count(&idle->ani) == 3) {
        for(int frame = 0; frame < 3; frame++) {
            animation_append_sprite(startup, sprite_copy(animation_get_sprite(&idle->ani, frame)));
        }
        str_set_c(&startup->animation_string, "A5-B8-C5-D7-C2");
    } else {
        animation_append_sprite(startup, sprite_copy(poses[3]));
        str_set_c(&startup->animation_string, "A5-B22");
    }
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
    // Relative to the victim's mirrored facing, x-9 gives the attacker's
    // grip X. The victim's airborne offset follows the piledriver pose
    // directly in player_run_apply, so it cannot drift out of sync.
    str_set_c(&grab->footer_string,
              "ex-9y+8D5-efx-9D21-efx-9y+13D4-L6-M20");
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
