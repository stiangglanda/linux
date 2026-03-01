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

// IOCTL Definition TODO add clear screen
#define GLANDA_IOC_MAGIC 'G'
#define GLANDA_IOC_DRAW_RECT _IOW(GLANDA_IOC_MAGIC, 1, struct glanda_draw_rect_cmd)
#define GLANDA_IOC_DRAW_LINE _IOW(GLANDA_IOC_MAGIC, 2, struct glanda_draw_rect_cmd) // Reuse struct for line TODO maybe change this

#endif