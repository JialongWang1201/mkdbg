#ifndef STATIC_ASSERT_H
#define STATIC_ASSERT_H

#define STATIC_ASSERT(cond, name) typedef char static_assert_##name[(cond) ? 1 : -1]

#endif
