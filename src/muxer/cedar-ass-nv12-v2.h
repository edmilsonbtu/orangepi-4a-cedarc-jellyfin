#ifndef CEDAR_ASS_NV12_H
#define CEDAR_ASS_NV12_H

#include <ass/ass.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static ASS_Library *cedar_ass_library;
static ASS_Renderer *cedar_ass_renderer;
static ASS_Track *cedar_ass_track;

static int cedar_ass_init(int width, int height)
{
    const char *file = getenv("CEDAR_SUBTITLE_FILE");
    const char *fonts = getenv("CEDAR_SUBTITLE_FONTS");

    if (!file || !*file)
        return 0;

    cedar_ass_library = ass_library_init();
    if (!cedar_ass_library)
        return -1;

    if (fonts && *fonts)
        ass_set_fonts_dir(cedar_ass_library, fonts);

    cedar_ass_renderer = ass_renderer_init(cedar_ass_library);
    if (!cedar_ass_renderer)
        return -1;

    ass_set_frame_size(cedar_ass_renderer, width, height);
    ass_set_storage_size(cedar_ass_renderer, width, height);
    ass_set_fonts(cedar_ass_renderer, NULL, "sans-serif", 1, NULL, 1);

    cedar_ass_track = ass_read_file(cedar_ass_library, (char *)file, NULL);
    if (!cedar_ass_track)
        return -1;

    fprintf(stderr, "CEDAR_ASS_ENABLED=%s\n", file);
    return 0;
}

static inline uint8_t cedar_clip(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}


static void cedar_ass_render_nv12(
    uint8_t *yplane, uint8_t *uvplane,
    int stride, int width, int height, int64_t pts_us)
{
    if (!cedar_ass_track || pts_us < 0)
        return;

    int changed = 0;
    ASS_Image *img = ass_render_frame(
        cedar_ass_renderer, cedar_ass_track,
        pts_us / 1000, &changed
    );

    for (; img; img = img->next) {
        int r = (img->color >> 24) & 255;
        int g = (img->color >> 16) & 255;
        int b = (img->color >> 8) & 255;
        int opacity = 255 - (img->color & 255);

        int yy = cedar_clip((47*r + 157*g + 16*b + 128)/256 + 16);
        int uu = cedar_clip((-26*r - 87*g + 112*b + 128)/256 + 128);
        int vv = cedar_clip((112*r - 102*g - 10*b + 128)/256 + 128);

        int x0 = img->dst_x < 0 ? 0 : img->dst_x;
        int y0 = img->dst_y < 0 ? 0 : img->dst_y;
        int x1 = img->dst_x + img->w;
        int y1 = img->dst_y + img->h;

        if (x1 > width) x1 = width;
        if (y1 > height) y1 = height;
        if (x0 >= x1 || y0 >= y1) continue;

        for (int by = y0 & ~1; by < y1; by += 2) {
            for (int bx = x0 & ~1; bx < x1; bx += 2) {
                int alpha_sum = 0;
                int samples = 0;

                for (int dy = 0; dy < 2; dy++) {
                    for (int dx = 0; dx < 2; dx++) {
                        int px = bx + dx;
                        int py = by + dy;

                        if (px >= width || py >= height)
                            continue;

                        samples++;

                        if (px < x0 || px >= x1 ||
                            py < y0 || py >= y1)
                            continue;

                        int ix = px - img->dst_x;
                        int iy = py - img->dst_y;

                        int alpha =
                            (img->bitmap[iy * img->stride + ix] *
                             opacity + 127) / 255;

                        alpha_sum += alpha;

                        uint8_t *yp = yplane + py * stride + px;
                        *yp = (uint8_t)(
                            (*yp * (255-alpha) + yy*alpha + 127) / 255
                        );
                    }
                }

                if (samples == 0 || alpha_sum == 0)
                    continue;

                int alpha_uv = (alpha_sum + samples/2) / samples;
                int uvindex = (by/2)*stride + bx;

                uint8_t *up = uvplane + uvindex;
                uint8_t *vp = up + 1;

                *up = (uint8_t)(
                    (*up * (255-alpha_uv) + uu*alpha_uv + 127)/255
                );
                *vp = (uint8_t)(
                    (*vp * (255-alpha_uv) + vv*alpha_uv + 127)/255
                );
            }
        }
    }
}
static void cedar_ass_close(void)
{
    if (cedar_ass_track)
        ass_free_track(cedar_ass_track);
    if (cedar_ass_renderer)
        ass_renderer_done(cedar_ass_renderer);
    if (cedar_ass_library)
        ass_library_done(cedar_ass_library);
}

#endif
