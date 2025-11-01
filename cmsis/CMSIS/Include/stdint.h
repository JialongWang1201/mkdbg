#ifndef _STDINT_H_
#define _STDINT_H_

typedef signed char         int8_t;
typedef unsigned char       uint8_t;

typedef short               int16_t;
typedef unsigned short      uint16_t;

typedef int                 int32_t;
typedef unsigned int        uint32_t;

typedef long long           int64_t;
typedef unsigned long long  uint64_t;

/* 指针相关 */
typedef int32_t             intptr_t;
typedef uint32_t            uintptr_t;

/* 至少 N 位的整数（简单处理成等价类型） */
typedef int8_t              int_least8_t;
typedef uint8_t             uint_least8_t;
typedef int16_t             int_least16_t;
typedef uint16_t            uint_least16_t;
typedef int32_t             int_least32_t;
typedef uint32_t            uint_least32_t;
typedef int64_t             int_least64_t;
typedef uint64_t            uint_least64_t;

/* 这里先不定义各种 *_MAX 宏，HAL/CMSIS 一般用不到它们 */

#endif /* _STDINT_H_ */
