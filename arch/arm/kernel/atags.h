#ifdef CONFIG_ATAGS_PROC
extern void save_atags(struct tag *tags);
#else
//mini6410
static inline void save_atags(struct tag *tags) { }
#endif
