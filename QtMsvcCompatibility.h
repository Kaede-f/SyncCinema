#pragma once

// 本机的 Anaconda Qt 5.15 构建于较早版本的 MSVC，它会把部分裸指针
// 包装成 stdext::checked_array_iterator。VS 2026 已经删除了这套非标准 API。
//
// 这个头文件只由 CMake 在“Qt 5 + 新版 MSVC”组合下强制包含，把 Qt 的
// 兼容宏退化为普通指针。Qt 自己在非 MSVC 平台也是这样定义的，因此不会
// 改变容器算法的业务语义。以后换成官方 Qt 6 时，此文件不会参与编译。
#include <QtCore/qcompilerdetection.h>

#if defined(_MSC_VER) && _MSC_VER >= 1950
#undef QT_MAKE_UNCHECKED_ARRAY_ITERATOR
#undef QT_MAKE_CHECKED_ARRAY_ITERATOR
#define QT_MAKE_UNCHECKED_ARRAY_ITERATOR(pointer) (pointer)
#define QT_MAKE_CHECKED_ARRAY_ITERATOR(pointer, size) (pointer)
#endif
