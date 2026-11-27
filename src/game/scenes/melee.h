#ifndef MELEE_H
#define MELEE_H

#include "game/protos/scene.h"
#include "resources/sprite.h"

bool melee_load_world_portrait(scene *scene, sprite *out, unsigned int photo_id);

int melee_create(scene *scene);

#endif // MELEE_H
