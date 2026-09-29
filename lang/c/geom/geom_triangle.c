/* See geom_triangle.h. */
#include "geom_triangle.h"

#define EDGE_FUNC_SHIFT 5        /* the edge functions' fixed point: 1/32 pixel */

int g_geom_cull = 1;             /* GDL_GEOMODE: 0 = draw both faces */

void geom_raster_set_scissor(geom_raster_state_t *st, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    st->scissor_start_x_fx = x << EDGE_FUNC_SHIFT;
    st->scissor_start_y_fx = y << EDGE_FUNC_SHIFT;
    st->scissor_end_x_fx = (int32_t)(w << EDGE_FUNC_SHIFT) + st->scissor_start_x_fx;
    st->scissor_end_y_fx = (int32_t)(h << EDGE_FUNC_SHIFT) + st->scissor_start_y_fx;
}

void geom_set_cull(int on) {
    g_geom_cull = on;
}
