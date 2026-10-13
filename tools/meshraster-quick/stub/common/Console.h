// Scratch harness only (branch meshraster-quick): stands in for the emulator's console so MeshRaster.cpp builds alone.
#pragma once

#include <cstdarg>
#include <cstdio>

struct HarnessConsole
{
	static void Print(const char* tag, const char* format, va_list args)
	{
		std::fputs(tag, stdout);
		std::vfprintf(stdout, format, args);
		std::fputc('\n', stdout);
		std::fflush(stdout);
	}
	void WriteLn(const char* format, ...) const
	{
		va_list args;
		va_start(args, format);
		Print("", format, args);
		va_end(args);
	}
	void Warning(const char* format, ...) const
	{
		va_list args;
		va_start(args, format);
		Print("WARNING: ", format, args);
		va_end(args);
	}
	void Error(const char* format, ...) const
	{
		va_list args;
		va_start(args, format);
		Print("ERROR: ", format, args);
		va_end(args);
	}
};

inline HarnessConsole Console;
