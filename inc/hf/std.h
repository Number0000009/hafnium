/*
 * Copyright 2018 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *str);
int strcmp(const char *a, const char *b);

#define ctz(x) __builtin_ctz(x)

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#define be16toh(v) __builtin_bswap16(v)
#define be32toh(v) __builtin_bswap32(v)
#define be64toh(v) __builtin_bswap64(v)

#define htobe16(v) __builtin_bswap16(v)
#define htobe32(v) __builtin_bswap32(v)
#define htobe64(v) __builtin_bswap64(v)

#define le16toh(v) (v)
#define le32toh(v) (v)
#define le64toh(v) (v)

#define htole16(v) (v)
#define htole32(v) (v)
#define htole64(v) (v)

#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

#define be16toh(v) (v)
#define be32toh(v) (v)
#define be64toh(v) (v)

#define htobe16(v) (v)
#define htobe32(v) (v)
#define htobe64(v) (v)

#define le16toh(v) __builtin_bswap16(v)
#define le32toh(v) __builtin_bswap32(v)
#define le64toh(v) __builtin_bswap64(v)

#define htole16(v) __builtin_bswap16(v)
#define htole32(v) __builtin_bswap32(v)
#define htole64(v) __builtin_bswap64(v)

#else /* __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ && \
	 __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__ */

#error "Unsupported byte order"

#endif
