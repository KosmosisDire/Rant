/* A stdint.h for toolchains that ship none, like the VxWorks GCC 4.3 for i586. It assumes
 * ILP32 with a 64 bit long long and is never amalgamated. See spec/platform.md. */
#ifndef STDINT_H_
#define STDINT_H_

/* 7.18.1.1 exact width */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long long          int64_t;
typedef unsigned long long uint64_t;

/* 7.18.1.2 least and 7.18.1.3 fast widths, the same as exact here */
typedef int8_t   int_least8_t;    typedef uint8_t   uint_least8_t;
typedef int16_t  int_least16_t;   typedef uint16_t  uint_least16_t;
typedef int32_t  int_least32_t;   typedef uint32_t  uint_least32_t;
typedef int64_t  int_least64_t;   typedef uint64_t  uint_least64_t;
typedef int8_t   int_fast8_t;     typedef uint8_t   uint_fast8_t;
typedef int32_t  int_fast16_t;    typedef uint32_t  uint_fast16_t;
typedef int32_t  int_fast32_t;    typedef uint32_t  uint_fast32_t;
typedef int64_t  int_fast64_t;    typedef uint64_t  uint_fast64_t;

/* 7.18.1.4 pointer and 7.18.1.5 greatest width */
#ifdef __LP64__
typedef long           intptr_t;  typedef unsigned long      uintptr_t;
#else
typedef int            intptr_t;  typedef unsigned int       uintptr_t;
#endif
typedef int64_t        intmax_t;  typedef uint64_t           uintmax_t;

/* 7.18.2 limits */
#define INT8_MIN   (-128)
#define INT16_MIN  (-32768)
#define INT32_MIN  (-INT32_MAX - 1)
#define INT64_MIN  (-INT64_MAX - 1)
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647
#define INT64_MAX  9223372036854775807LL
#define UINT8_MAX  255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615ULL

#define INT_LEAST8_MIN   INT8_MIN
#define INT_LEAST16_MIN  INT16_MIN
#define INT_LEAST32_MIN  INT32_MIN
#define INT_LEAST64_MIN  INT64_MIN
#define INT_LEAST8_MAX   INT8_MAX
#define INT_LEAST16_MAX  INT16_MAX
#define INT_LEAST32_MAX  INT32_MAX
#define INT_LEAST64_MAX  INT64_MAX
#define UINT_LEAST8_MAX  UINT8_MAX
#define UINT_LEAST16_MAX UINT16_MAX
#define UINT_LEAST32_MAX UINT32_MAX
#define UINT_LEAST64_MAX UINT64_MAX

#define INT_FAST8_MIN    INT8_MIN
#define INT_FAST16_MIN   INT32_MIN
#define INT_FAST32_MIN   INT32_MIN
#define INT_FAST64_MIN   INT64_MIN
#define INT_FAST8_MAX    INT8_MAX
#define INT_FAST16_MAX   INT32_MAX
#define INT_FAST32_MAX   INT32_MAX
#define INT_FAST64_MAX   INT64_MAX
#define UINT_FAST8_MAX   UINT8_MAX
#define UINT_FAST16_MAX  UINT32_MAX
#define UINT_FAST32_MAX  UINT32_MAX
#define UINT_FAST64_MAX  UINT64_MAX

#ifdef __LP64__
#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#else
#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#endif
#define PTRDIFF_MIN INTPTR_MIN
#define PTRDIFF_MAX INTPTR_MAX
#ifndef SIZE_MAX
#define SIZE_MAX    UINTPTR_MAX
#endif
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define SIG_ATOMIC_MIN INT32_MIN
#define SIG_ATOMIC_MAX INT32_MAX
#ifndef WCHAR_MAX
#define WCHAR_MAX 0x7fffffff
#endif
#ifndef WCHAR_MIN
#define WCHAR_MIN (-WCHAR_MAX - 1)
#endif
#define WINT_MIN INT32_MIN
#define WINT_MAX INT32_MAX

/* 7.18.4 constant macros */
#define INT8_C(v)   (v)
#define INT16_C(v)  (v)
#define INT32_C(v)  (v)
#define INT64_C(v)  (v##LL)
#define UINT8_C(v)  (v##U)
#define UINT16_C(v) (v##U)
#define UINT32_C(v) (v##U)
#define UINT64_C(v) (v##ULL)
#define INTMAX_C(v)  (v##LL)
#define UINTMAX_C(v) (v##ULL)

#endif /* STDINT_H_ */
