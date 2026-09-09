// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com
// 静态 libcurl（VC15 工程）以 CRT dllimport 方式引用旧名 __imp_write / __imp__mbspbrk，
// 新版 UCRT 不再导出这些符号。本文件提供等义数据符号以完成链接（KBE_CLUSTER 专用）。
#include <io.h>
#include <mbstring.h>

namespace KBEngine {

extern "C" int __cdecl write_imp_impl(int fd, const void* buf, unsigned int cnt)
{
	return (int)_write(fd, buf, cnt);
}

extern "C" unsigned char* __cdecl mbspbrk_imp_impl(const unsigned char* str, const unsigned char* set)
{
	if (!str || !set)
		return nullptr;

	for (const unsigned char* p = str; *p; ++p)
	{
		for (const unsigned char* q = set; *q; ++q)
		{
			if (*p == *q)
				return (unsigned char*)p;
		}
	}

	return nullptr;
}

extern "C" void* __imp_write = (void*)&write_imp_impl;
extern "C" void* __imp__mbspbrk = (void*)&mbspbrk_imp_impl;

}
