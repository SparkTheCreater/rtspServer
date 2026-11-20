#include "filter.h"

static int copy_open(struct frame_filter *self)
{
    (void)self;
    return 0;
}

static int copy_push(struct frame_filter *self, const struct frame *in,
                     void (*emit)(void *user, const struct frame *out), void *user)
{
    (void)self;
    if (!in || !in->data || in->len == 0 || !emit)
        return -1;
    emit(user, in);
    return 0;
}

static void copy_close(struct frame_filter *self)
{
    (void)self;
}

void filter_copy_init(struct frame_filter *filter)
{
    filter->name = "copy";
    filter->open = copy_open;
    filter->push = copy_push;
    filter->close = copy_close;
    filter->priv = NULL;
    filter->open(filter);
}
