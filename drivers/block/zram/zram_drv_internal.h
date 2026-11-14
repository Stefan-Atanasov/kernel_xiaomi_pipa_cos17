#ifndef _ZRAM_DRV_INTERNAL_H_
#define _ZRAM_DRV_INTERNAL_H_

/*
 * zram slot helpers shared by zram_drv.c and hybridswap/ (as in the OPPO Pad 1
 * kernel, where they are macros here; kept as inline functions because our
 * zram carries the idle-count / MIUI additions).
 */
#include <linux/bit_spinlock.h>
#include <linux/genhd.h>
#include "zram_drv.h"

static inline void zram_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static inline void zram_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

/* flag operations require table entry bit_spin_lock() being held */
static inline bool zram_test_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static inline void zram_set_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

static inline void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static inline size_t zram_get_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static inline void zram_set_obj_size(struct zram *zram,
					u32 index, size_t size)
{
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

/*
 * hybridswap keeps the zsmalloc handle directly in table[].entry; this is
 * valid because HYBRIDSWAP depends on !ZRAM_DEDUP (zram_entry_alloc() then
 * returns the bare handle).
 */
#define zram_get_handle(zram, index) ((unsigned long)(zram->table[index].entry))

#define zram_set_handle(zram, index, handle_val) \
	(zram->table[index].entry = (struct zram_entry *)handle_val)

#ifdef CONFIG_HYBRIDSWAP_ASYNC_COMPRESS
extern int async_compress_page(struct zram *zram, struct page* page);
extern void update_zram_index(struct zram *zram, u32 index, unsigned long page);
#endif
#endif /* _ZRAM_DRV_INTERNAL_H_ */
