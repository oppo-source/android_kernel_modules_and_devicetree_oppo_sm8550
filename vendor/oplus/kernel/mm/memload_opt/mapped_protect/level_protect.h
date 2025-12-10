#ifndef _LEVEL_PROTECT_H_
#define _LEVEL_PROTECT_H_

extern int page_should_be_level_protect(struct page *page, bool *should_protect);
extern void level_protect_proc_init(void);
extern void level_protect_proc_remove(void);

#endif