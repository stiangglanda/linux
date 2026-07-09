#ifndef _GLANDA_DRM_H_
#define _GLANDA_DRM_H_

#include <linux/types.h>
#include <drm/drm.h>

struct glanda_clear_cmd {
    __u32 color;
};

struct glanda_draw_rect_cmd {
    __u16 x;
    __u16 y;
    __u16 w;
    __u16 h;
    __u32 color;
};

struct glanda_draw_line_cmd {
    __u16 x0;
    __u16 y0;
    __u16 x1;
    __u16 y1;
    __u32 color;
};

#define DRM_GLANDA_CLEAR     0x00
#define DRM_GLANDA_DRAW_RECT 0x01
#define DRM_GLANDA_DRAW_LINE 0x02

#define DRM_IOCTL_GLANDA_CLEAR     DRM_IOW(DRM_COMMAND_BASE + DRM_GLANDA_CLEAR, struct glanda_clear_cmd)
#define DRM_IOCTL_GLANDA_DRAW_RECT DRM_IOW(DRM_COMMAND_BASE + DRM_GLANDA_DRAW_RECT, struct glanda_draw_rect_cmd)
#define DRM_IOCTL_GLANDA_DRAW_LINE DRM_IOW(DRM_COMMAND_BASE + DRM_GLANDA_DRAW_LINE, struct glanda_draw_line_cmd)

#endif