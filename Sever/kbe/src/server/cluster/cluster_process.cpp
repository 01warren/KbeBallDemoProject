// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "cluster_process.h"

#include "helper/debug_helper.h"
#include "common/strutil.h"

#if KBE_PLATFORM != PLATFORM_WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace KBEngine
{
namespace ClusterProcess
{

namespace
{

#if KBE_PLATFORM != PLATFORM_WIN32
// 是否仍以该 pid 运行(进程表探测; 允许僵尸瞬态存在)
bool processExists(uint32 pid)
{
	if(pid == 0)
		return false;

	int ret = kill((pid_t)pid, 0);
	return ret == 0 || errno == EPERM;
}

void microSleep(uint32 us)
{
	::usleep(us);
}

#else

bool processExists(uint32 pid)
{
	if(pid == 0)
		return false;

	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if(h == NULL)
		return false;

	DWORD code = 0;
	BOOL ok = GetExitCodeProcess(h, &code);
	CloseHandle(h);

	// STILL_ACTIVE 或查询失败(权限等)时按"仍存在"处理
	return !ok || code == STILL_ACTIVE;
}

void microSleep(uint32 us)
{
	::Sleep(us / 1000);
}

#endif

// Windows: 组装 taskkill 命令行; Linux 直接使用信号
#if KBE_PLATFORM == PLATFORM_WIN32
std::string killCmdLine(uint32 pid, bool force)
{
	std::string cmd = "taskkill";
	if(force)
		cmd += " /f";
	cmd += " /t /pid ";
	cmd += std::to_string(pid);
	return cmd;
}
#endif

}	// end anonymous

//-------------------------------------------------------------------------------------
bool startProcess(int32 uid, COMPONENT_TYPE componentType, uint64 cid, uint16 gus, std::string& err)
{
	if(componentType < 0 || componentType >= COMPONENT_END_TYPE)
	{
		err = "invalid componentType";
		return false;
	}

	std::string cmdLine;
	std::string rootPath;
	std::string resPath;
	std::string binPath;

#if KBE_PLATFORM != PLATFORM_WIN32
	// 环境变量由启动脚本(cluster 与组件同源)在父进程导出; 未导出时返回明确错误
	const char* envRoot = getenv("KBE_ROOT");
	const char* envRes = getenv("KBE_RES_PATH");
	const char* envBin = getenv("KBE_BIN_PATH");

	rootPath = envRoot ? envRoot : "";
	resPath = envRes ? envRes : "";
	binPath = envBin ? envBin : "";

	if(rootPath.empty() || resPath.empty() || binPath.empty())
	{
		err = "KBE_ROOT/KBE_RES_PATH/KBE_BIN_PATH not set, cannot start server process";
		return false;
	}

	// 在父进程中准备好 argv(子进程在 fork 后仅执行 async-signal-safe 操作)
	std::string scid = fmt::format("--cid={}", cid);
	std::string sgus = fmt::format("--gus={}", gus);

	cmdLine = binPath + COMPONENT_NAME_EX(componentType);

	const char* argv[6];
	const char** pArgv = argv;
	*pArgv++ = cmdLine.c_str();
	*pArgv++ = scid.c_str();
	*pArgv++ = sgus.c_str();
	*pArgv = NULL;

	// 子进程继承父进程环境, 预先把 KBE 环境补进父进程(对父进程无副作用)
	setenv("KBE_ROOT", rootPath.c_str(), 1);
	setenv("KBE_RES_PATH", resPath.c_str(), 1);
	setenv("KBE_BIN_PATH", binPath.c_str(), 1);

	// CLOEXEC 管道: 子进程 exec 成功 -> 写端随 exec 关闭(读到 EOF);
	// exec 失败 -> 子进程写入 errno 后退出(读到字节)。避免竞态判断 exec 成败。
	int pipefd[2] = { -1, -1 };
	if(pipe(pipefd) != 0)
	{
		err = fmt::format("pipe() failed: {}", strerror(errno));
		return false;
	}

	int flags = fcntl(pipefd[1], F_GETFD, 0);
	if(flags >= 0)
		fcntl(pipefd[1], F_SETFD, flags | FD_CLOEXEC);

	pid_t pid = fork();
	if(pid < 0)
	{
		::close(pipefd[0]);
		::close(pipefd[1]);
		err = fmt::format("fork() failed: {}", strerror(errno));
		return false;
	}

	if(pid == 0)
	{
		// ---- 子进程: 仅允许 async-signal-safe 调用 ----
		::close(pipefd[0]);

		if(uid > 0)
		{
			if(setuid((uid_t)uid) == -1)
			{
				// setuid 失败也要通知父进程, 避免父进程超时误判为已启动
				uint8_t ecode = (uint8_t)errno;
				(void)!::write(pipefd[1], &ecode, 1);
				_exit(1);
			}
		}

		execv(cmdLine.c_str(), (char* const*)argv);

		// exec 失败: 通知父进程后退出
		uint8_t code = (uint8_t)errno;
		(void)!::write(pipefd[1], &code, 1);
		_exit(127);
	}

	// ---- 父进程 ----
	::close(pipefd[1]);

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(pipefd[0], &rfds);
	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;

	int sel = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
	if(sel > 0)
	{
		uint8_t code = 0;
		ssize_t n = ::read(pipefd[0], &code, 1);
		if(n == 0)
		{
			// EOF: exec 成功, 子进程已变成目标程序继续运行
			::close(pipefd[0]);
			INFO_MSG(fmt::format("ClusterProcess::startProcess: spawned '{}' (uid={}, cid={}, gus={}, pid={})\n",
				cmdLine, uid, cid, gus, (uint64)pid));
			return true;
		}

		if(n > 0)
		{
			::close(pipefd[0]);
			::waitpid(pid, NULL, 0);		// 回收立即退出的失败子进程
			err = fmt::format("exec '{}' failed: {}", cmdLine, strerror((int)code));
			return false;
		}
	}

	// 超时/异常: 仍认为已拉起(子进程处于 exec 路径), 由 SIGCHLD 回收
	::close(pipefd[0]);
	INFO_MSG(fmt::format("ClusterProcess::startProcess: spawned(assumed) '{}' (uid={}, cid={}, gus={}, pid={})\n",
		cmdLine, uid, cid, gus, (uint64)pid));
	return true;
#else
	// ---- Windows: CreateProcess ----
	const char* envBin = getenv("KBE_BIN_PATH");
	std::string bin = envBin ? envBin : "";

	if(bin.empty())
	{
		err = "KBE_BIN_PATH not found, cannot start server process";
		return false;
	}

	std::string exePath = bin + COMPONENT_NAME_EX(componentType) + ".exe";
	std::string cmdline = "\"" + exePath + "\"" +
		fmt::format(" --cid={}", cid) +
		fmt::format(" --gus={}", gus);

	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	wchar_t* szCmdline = KBEngine::strutil::char2wchar(cmdline.c_str());

	wchar_t currdir[MAX_PATH];
	DWORD currdirLen = GetCurrentDirectoryW(MAX_PATH, currdir);

	if(!CreateProcessW(NULL, szCmdline, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
		currdirLen > 0 ? currdir : NULL, &si, &pi))
	{
		err = fmt::format("CreateProcess '{}' failed: {}", exePath, (int)GetLastError());
		free(szCmdline);
		return false;
	}

	free(szCmdline);

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	INFO_MSG(fmt::format("ClusterProcess::startProcess: spawned '{}' (uid={}, cid={}, gus={}, pid={})\n",
		exePath, uid, cid, gus, (uint64)pi.dwProcessId));
	return true;
#endif
}

//-------------------------------------------------------------------------------------
bool stopProcess(COMPONENT_TYPE componentType, const std::vector<CtlTarget>& targets, std::string& err)
{
	(void)componentType;
	(void)err;

	for(size_t i = 0; i < targets.size(); ++i)
	{
		const CtlTarget& t = targets[i];

		if(!processExists(t.pid))
			continue;	// 已不在运行, 视为成功(与 machine 清理行为一致)

#if KBE_PLATFORM != PLATFORM_WIN32
		// 引擎 ServerApp 将 SIGINT 注册为优雅退出
		if(kill((pid_t)t.pid, SIGINT) != 0)
		{
			err = fmt::format("kill(pid={}, SIGINT) failed: {}", t.pid, strerror(errno));
			return false;
		}
#else
		// Windows: 不带 /f 的 taskkill 请求关闭
		int ret = ::system(killCmdLine(t.pid, false).c_str());
		if(ret != 0)
		{
			err = fmt::format("taskkill(pid={}) failed, ret={}", t.pid, ret);
			return false;
		}
#endif
	}

	return true;
}

//-------------------------------------------------------------------------------------
bool killProcess(const std::vector<CtlTarget>& targets, std::string& err)
{
	for(size_t i = 0; i < targets.size(); ++i)
	{
		const CtlTarget& t = targets[i];

		if(!processExists(t.pid))
			continue;	// 已消失

		bool terminated = false;
		for(int attempt = 0; attempt < 15 && !terminated; ++attempt)
		{
#if KBE_PLATFORM != PLATFORM_WIN32
			if(kill((pid_t)t.pid, SIGKILL) != 0 && errno != ESRCH)
			{
				err = fmt::format("kill(pid={}, SIGKILL) failed: {}", t.pid, strerror(errno));
				return false;
			}
#else
			if(::system(killCmdLine(t.pid, true).c_str()) != 0)
			{
				err = fmt::format("taskkill /f(pid={}) failed", t.pid);
				return false;
			}
#endif

			for(int k = 0; k < 10; ++k)
			{
				if(!processExists(t.pid))
				{
					terminated = true;
					break;
				}
				microSleep(100 * 1000);
			}
		}

		if(!terminated)
		{
			err = fmt::format("kill(pid={}) timeout: process still alive", t.pid);
			return false;
		}
	}

	return true;
}

}
}
