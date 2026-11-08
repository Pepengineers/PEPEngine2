#pragma once
#include <Windows.h>
#include <crtdbg.h>
#include <cstring>
namespace AssertCapture {
inline int Narrow(int type,char* message,int*){
    if(type!=_CRT_ASSERT)return FALSE;
    DWORD written=0;WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,static_cast<DWORD>(std::strlen(message)),&written,nullptr);
    ExitProcess(3);
}
inline int Wide(int type,wchar_t* message,int*){
    if(type!=_CRT_ASSERT)return FALSE;
    char text[16384];const int count=WideCharToMultiByte(CP_UTF8,0,message,-1,text,sizeof(text),nullptr,nullptr);
    DWORD written=0;if(count>0)WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,count-1,&written,nullptr);
    ExitProcess(3);
}
struct Init { Init(){_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL,Narrow);_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,Wide);} };
inline Init Register;
}
