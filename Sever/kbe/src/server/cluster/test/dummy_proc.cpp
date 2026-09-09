// Copyright 2008-2025 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com
/*
	dummy_proc — ctl(远程起停)测试用的"伪组件进程"。

	为什么需要它: cluster 的 MSG_STOP_SERVER 在本机走不带 /f 的 taskkill(Windows),
	它只向进程的顶层窗口投递 WM_CLOSE; 纯控制台进程没有窗口会收到 taskkill 的
	"只能强制终止" 错误, 无法验证优雅停止路径。本程序创建隐藏消息窗口,
	收到 WM_CLOSE/Destroy 后正常退出, 从而让 STOP 指令可以验证成功路径。

	行为:
	    - 启动后打印 DUMMY_PID=<pid> 并保持运行(消息循环);
	    - 收到 WM_CLOSE(即 taskkill 不带 /f 投递) -> 退出码 0;
	    - 可选参数 <seconds>: 自退出时间(默认无限, 0/负值也视为无限);
	    - KILL(带 /f taskkill / SIGKILL) 对其同样有效。

	跨平台: Windows 用 Win32 消息窗口; Linux 用 SIGTERM/SIGINT 信号处理。
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
	#include <windows.h>

	static const char* CLASS_NAME = "KbeDummyProcWnd";
	static volatile bool g_closing = false;

	static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		switch(msg)
		{
		case WM_CLOSE:
			g_closing = true;
			DestroyWindow(hwnd);			// 触发 WM_DESTROY -> 由 DefWindowProc 收尾
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		case WM_ENDSESSION:
			g_closing = true;
			PostQuitMessage(0);
			return 0;
		default:
			return DefWindowProc(hwnd, msg, wp, lp);
		}
	}

	int platformMain(unsigned autoExitSec)
	{
		WNDCLASS wc;
		std::memset(&wc, 0, sizeof(wc));
		wc.lpfnWndProc = wndProc;
		wc.hInstance = GetModuleHandle(NULL);
		wc.lpszClassName = CLASS_NAME;
		if(!RegisterClass(&wc))
			return 2;

		// 隐藏消息窗口(不可见, 只提供可接收 WM_CLOSE 的顶层句柄)
		HWND hwnd = CreateWindowEx(0, CLASS_NAME, "kbe-dummy", WS_OVERLAPPEDWINDOW,
			CW_USEDEFAULT, CW_USEDEFAULT, 200, 100, NULL, NULL, wc.hInstance, NULL);
		if(!hwnd)
			return 3;

		std::printf("DUMMY_PID=%lu\n", (unsigned long)GetCurrentProcessId());
		std::fflush(stdout);

		unsigned long t0 = GetTickCount();
		MSG msg;
		for(;;)
		{
			while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				if(msg.message == WM_QUIT)
				{
					std::printf("DUMMY_EXIT=graceful\n");
					std::fflush(stdout);
					return 0;
				}
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			if(g_closing)
			{
				std::printf("DUMMY_EXIT=graceful\n");
				std::fflush(stdout);
				return 0;
			}
			if(autoExitSec > 0 && (GetTickCount() - t0) / 1000 >= autoExitSec)
			{
				std::printf("DUMMY_EXIT=auto\n");
				std::fflush(stdout);
				return 0;
			}
			// 不忙等: 等待消息或 200ms 定时唤醒(兼顾自动退出检查)
			MsgWaitForMultipleObjects(0, NULL, FALSE, 200, QS_ALLINPUT);
		}
	}

#else
	#include <csignal>
	#include <unistd.h>

	static volatile sig_atomic_t g_closing = 0;
	static void onSignal(int) { g_closing = 1; }

	int platformMain(unsigned autoExitSec)
	{
		signal(SIGTERM, onSignal);
		signal(SIGINT, onSignal);
		std::printf("DUMMY_PID=%d\n", (int)getpid());
		std::fflush(stdout);

		std::time_t t0 = std::time(NULL);
		while(!g_closing)
		{
			if(autoExitSec > 0 && (unsigned)(std::time(NULL) - t0) >= autoExitSec)
				break;
			usleep(200 * 1000);
		}
		std::printf("DUMMY_EXIT=%s\n", g_closing ? "graceful" : "auto");
		std::fflush(stdout);
		return 0;
	}
#endif

int main(int argc, char** argv)
{
	unsigned autoExitSec = 0;
	if(argc >= 2)
	{
		long v = std::atol(argv[1]);
		if(v > 0) autoExitSec = (unsigned)v;
	}
	return platformMain(autoExitSec);
}
