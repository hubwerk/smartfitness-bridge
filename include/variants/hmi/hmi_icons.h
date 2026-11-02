#pragma once

#include "lvgl.h"

// A8 coverage icons generated from the WebHMI artwork by scripts/gen_hmi_icons.py.
extern "C" {
LV_IMAGE_DECLARE(sf_icon_nav_training);
LV_IMAGE_DECLARE(sf_icon_nav_connections);
LV_IMAGE_DECLARE(sf_icon_nav_system);
LV_IMAGE_DECLARE(sf_icon_nav_back);
LV_IMAGE_DECLARE(sf_icon_sport_bike);
LV_IMAGE_DECLARE(sf_icon_sport_treadmill);
LV_IMAGE_DECLARE(sf_icon_sport_rower);
LV_IMAGE_DECLARE(sf_icon_sport_cross_trainer);
LV_IMAGE_DECLARE(sf_icon_sport_step_climber);
LV_IMAGE_DECLARE(sf_icon_sport_stair_climber);
LV_IMAGE_DECLARE(sf_icon_sport_generic);
// Dithered background textures: RGB565 sky, A8 glow disc (recoloured per sport).
LV_IMAGE_DECLARE(sf_tex_sky);
LV_IMAGE_DECLARE(sf_tex_glow);
}
