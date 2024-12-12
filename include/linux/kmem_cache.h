/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kmem cache manager
 *
 * Copyright (C) 2024 Tencent Cloud.
 */
#ifndef KMEM_CACHE_H
#define KMEM_CACHE_H

#include <linux/list.h>

void *kmem_cache_get_free_page(gfp_t gfp_flags);
void kmem_cache_put_page(void *object);

#endif /* KMEM_CACHE_H */
