#ifndef GLANDA_UAPI_H
#define GLANDA_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

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

struct glanda_clear_cmd {
    __u32 color;
};

// IOCTL Definition
#define GLANDA_IOC_MAGIC 'G'
#define GLANDA_IOC_CLEAR     _IOW(GLANDA_IOC_MAGIC, 1, struct glanda_clear_cmd)
#define GLANDA_IOC_DRAW_RECT _IOW(GLANDA_IOC_MAGIC, 2, struct glanda_draw_rect_cmd)
#define GLANDA_IOC_DRAW_LINE _IOW(GLANDA_IOC_MAGIC, 3, struct glanda_draw_line_cmd)


#endif