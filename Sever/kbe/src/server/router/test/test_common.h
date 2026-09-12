// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

/*
	test_common.h —— 极简测试框架。

	本仓库没有 gtest/catch 等单元测试框架(引入第三方依赖需要改动全平台构建)，
	因此这里提供一个几十行的最小实现：用例注册 + 断言计数 + 机读输出 + 退出码。

	输出约定(与 cluster/test/test_client.cpp 保持一致，便于脚本解析)：
		[ RUN      ] <CASE-ID>  <描述>
		[       OK ] <CASE-ID>
		[   FAILED ] <CASE-ID>
		[FAIL] file:line  expr        —— 失败断言明细
		==== <suite>: N case(s), M failed ====

	退出码：0 = 全部通过；1 = 有用例失败。
*/

#ifndef KBE_ROUTER_TEST_COMMON_H
#define KBE_ROUTER_TEST_COMMON_H

#include <cstdio>
#include <string>

class TestRunner
{
public:
	TestRunner() : total_(0), failed_(0), skipped_(0), curFailed_(false), curSkipped_(false) {}

	void beginCase(const char* id, const char* desc)
	{
		curCase_ = id;
		curFailed_ = false;
		curSkipped_ = false;
		++total_;
		printf("[ RUN      ] %-8s %s\n", id, desc);
	}

	/**
		跳过当前用例：环境不满足(如没有 router 进程、等待时间过长)时使用。
		跳过不计入用例总数、不计失败，但在汇总里显式列出，避免"静默不测"。
	*/
	void skipCase(const char* reason)
	{
		--total_;
		++skipped_;
		curSkipped_ = true;
		printf("[    SKIP  ] %s  (%s)\n", curCase_.c_str(), reason);
		fflush(stdout);
	}

	void check(bool ok, const char* expr, const char* file, int line)
	{
		if(ok)
			return;

		curFailed_ = true;
		printf("             [FAIL] %s:%d  %s\n", file, line, expr);
		fflush(stdout);
	}

	void endCase()
	{
		// 已 skip 的用例不再输出 OK/FAILED，避免"跳过"被误读成"通过"。
		if(curSkipped_)
		{
			fflush(stdout);
			return;
		}

		if(curFailed_)
		{
			++failed_;
			printf("[   FAILED ] %s\n", curCase_.c_str());
		}
		else
		{
			printf("[       OK ] %s\n", curCase_.c_str());
		}

		fflush(stdout);
	}

	int summary(const char* suite) const
	{
		if(skipped_ > 0)
		{
			printf("\n==== %s: %d case(s), %d failed, %d skipped ====\n",
				suite, total_, failed_, skipped_);
		}
		else
		{
			printf("\n==== %s: %d case(s), %d failed ====\n", suite, total_, failed_);
		}

		fflush(stdout);
		return failed_ == 0 ? 0 : 1;
	}

private:
	int			total_;
	int			failed_;
	int			skipped_;
	bool		curFailed_;
	bool		curSkipped_;
	std::string	curCase_;
};

extern TestRunner g_runner;

#define BEGIN_CASE(id, desc)	g_runner.beginCase(id, desc)
#define END_CASE()				g_runner.endCase()
#define CHECK(cond)				g_runner.check((cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ_INT(a, b)		g_runner.check(((long long)(a)) == ((long long)(b)), #a " == " #b, __FILE__, __LINE__)
#define CHECK_EQ_STR(a, b)		g_runner.check(std::string(a) == std::string(b), #a " == " #b, __FILE__, __LINE__)

#endif // KBE_ROUTER_TEST_COMMON_H
