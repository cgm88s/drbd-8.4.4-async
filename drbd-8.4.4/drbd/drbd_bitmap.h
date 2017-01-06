
#ifndef  __DRBD_BITMAP_H__
#define __DRBD_BITMAP_H__

extern struct page **bm_realloc_pages(struct drbd_bitmap *b, unsigned long want);
extern void bm_free_pages(struct page **pages, unsigned long number);
#endif