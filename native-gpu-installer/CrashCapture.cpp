#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <processsnapshot.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "CrashLogPolicy.h"
#pragma comment(lib,"dbghelp.lib")
#pragma comment(lib,"psapi.lib")
namespace fs = std::filesystem;

static uint64_t ticks(FILETIME t) { return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime; }
static void status(const fs::path& run,const std::string& s) {
  std::ofstream f(run/L"dump-status.txt",std::ios::app); f<<GetTickCount64()<<" "<<s<<"\n";f.flush();
}
// S_FALSE identifies an HPSS snapshot handle to DbgHelp; S_OK incorrectly
// treats it as a live process and fails with HRESULT_FROM_NT(STATUS_INVALID_HANDLE).
static BOOL CALLBACK snapshotCallback(PVOID,const PMINIDUMP_CALLBACK_INPUT in,PMINIDUMP_CALLBACK_OUTPUT out) {
  if(in->CallbackType==IsProcessSnapshotCallback) out->Status=S_FALSE;
  return TRUE;
}
static bool dump(HANDLE process,DWORD pid,const fs::path& run,const wchar_t* prefix,MINIDUMP_EXCEPTION_INFORMATION* exception=nullptr) {
  const fs::path file=run/(std::wstring(prefix)+L"-"+std::to_wstring(GetTickCount64())+L".dmp");
  HANDLE out=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(out==INVALID_HANDLE_VALUE) {status(run,"create dump failed "+std::to_string(GetLastError()));return false;}
  const auto full=MINIDUMP_TYPE(MiniDumpWithFullMemory|MiniDumpWithFullMemoryInfo|MiniDumpWithThreadInfo|MiniDumpWithUnloadedModules|MiniDumpWithHandleData|MiniDumpIgnoreInaccessibleMemory);
  HPSS snapshot=nullptr;
  // A clone preserves committed guest memory if the original exits during a large dump.
  DWORD capture=PssCaptureSnapshot(process,PSS_CAPTURE_FLAGS(PSS_CAPTURE_VA_CLONE|PSS_CAPTURE_THREADS|PSS_CAPTURE_THREAD_CONTEXT|PSS_CAPTURE_HANDLES),CONTEXT_ALL,&snapshot);
  MINIDUMP_CALLBACK_INFORMATION callback{snapshotCallback,nullptr};
  BOOL ok=FALSE;
  if(capture==ERROR_SUCCESS) {
    ok=MiniDumpWriteDump((HANDLE)snapshot,pid,out,full,exception,nullptr,&callback);
    status(run,"snapshot full dump result="+std::to_string(ok)+" error="+std::to_string(ok?0:GetLastError()));
    PssFreeSnapshot(GetCurrentProcess(),snapshot);
  } else status(run,"snapshot unavailable error="+std::to_string(capture));
  if(!ok) {
    SetFilePointer(out,0,nullptr,FILE_BEGIN);SetEndOfFile(out);
    ok=MiniDumpWriteDump(process,pid,out,full,exception,nullptr,nullptr);
    status(run,"direct full dump result="+std::to_string(ok)+" error="+std::to_string(ok?0:GetLastError()));
  }
  if(!ok) {
    SetFilePointer(out,0,nullptr,FILE_BEGIN);SetEndOfFile(out);
    const auto fallback=MINIDUMP_TYPE(MiniDumpWithPrivateReadWriteMemory|MiniDumpWithIndirectlyReferencedMemory|MiniDumpWithFullMemoryInfo|MiniDumpWithThreadInfo|MiniDumpIgnoreInaccessibleMemory);
    ok=MiniDumpWriteDump(process,pid,out,fallback,exception,nullptr,nullptr);
    status(run,"fallback private/stack dump result="+std::to_string(ok)+" error="+std::to_string(ok?0:GetLastError()));
  }
  CloseHandle(out);
  if(!ok)DeleteFileW(file.c_str());
  return ok!=FALSE;
}
static bool fatal(const fs::path& log,uint64_t& offset,FatalLogTail& tail) {
  // Incremental tail; keep partial lines so a split [FATAL] marker is still detected.
  std::ifstream input(log,std::ios::binary);if(!input)return false;
  input.seekg(0,std::ios::end);auto end=input.tellg();if(end<0)return false;
  if(uint64_t(end)<offset){offset=0;tail.Reset();}
  input.seekg(offset);
  bool found=false;
  char block[65536];
  while(input.read(block,sizeof(block))||input.gcount()){
    const auto count=size_t(input.gcount());
    found=tail.Consume(std::string_view(block,count))||found;
    offset+=count;
  }
  return found;
}
static void metrics(HANDLE p,std::ofstream& out) {
  PROCESS_MEMORY_COUNTERS_EX mem{};mem.cb=sizeof(mem);GetProcessMemoryInfo(p,(PROCESS_MEMORY_COUNTERS*)&mem,sizeof(mem));
  FILETIME c,e,k,u;GetProcessTimes(p,&c,&e,&k,&u);DWORD handles=0;GetProcessHandleCount(p,&handles);
  out<<GetTickCount64()<<","<<mem.WorkingSetSize<<","<<mem.PrivateUsage<<","<<ticks(k)<<","<<ticks(u)<<","<<handles<<"\n";out.flush();
}
int wmain(int argc,wchar_t** argv) {
  if(argc>=6 && std::wstring(argv[1])==L"--hang") {
    DWORD pid=wcstoul(argv[2],nullptr,10);fs::path expected=fs::absolute(argv[3]),run=fs::absolute(argv[4]);
    HANDLE p=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE|PROCESS_CREATE_PROCESS, FALSE,pid);
    if(!p)return 2;
    wchar_t image[32768];DWORD n=32768;FILETIME c,e,k,u;
    bool valid=QueryFullProcessImageNameW(p,0,image,&n)&&GetProcessTimes(p,&c,&e,&k,&u)&&_wcsicmp(image,expected.c_str())==0&&ticks(c)==_wcstoui64(argv[5],nullptr,10);
    if(!valid){CloseHandle(p);return 3;}
    bool ok=dump(p,pid,run,L"hang-full");CloseHandle(p);return ok?0:4;
  }
  const bool observe=argc==6 && std::wstring(argv[1])==L"--observe";
  if(argc!=4 && !observe)return 2;
  bool debug=std::wstring(argv[1])==L"--debug";
  if(!debug && !observe && std::wstring(argv[1])!=L"--watch")return 2;
  fs::path exe=fs::absolute(argv[2]),run=fs::absolute(argv[3]);
  fs::create_directories(run);
  std::wstring cmd=L"\""+exe.wstring()+L"\"";
  STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION proc{};
  if(observe) {
    proc.dwProcessId=wcstoul(argv[4],nullptr,10);
    proc.hProcess=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_DUP_HANDLE|
                              PROCESS_CREATE_PROCESS|PROCESS_TERMINATE|SYNCHRONIZE,
                              FALSE,proc.dwProcessId);
    if(!proc.hProcess)return 2;
    wchar_t image[32768]; DWORD size=32768; FILETIME created,exited,kernel,user;
    if(!QueryFullProcessImageNameW(proc.hProcess,0,image,&size) ||
       _wcsicmp(image,exe.c_str())!=0 ||
       !GetProcessTimes(proc.hProcess,&created,&exited,&kernel,&user) ||
       ticks(created)!=_wcstoui64(argv[5],nullptr,10)) {
      CloseHandle(proc.hProcess);
      return 3;
    }
  } else if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,debug?DEBUG_ONLY_THIS_PROCESS:0,nullptr,run.c_str(),&startup,&proc)) {
    status(run,"CreateProcess failed "+std::to_string(GetLastError()));return 5;
  }
  FILETIME c,e,k,u;GetProcessTimes(proc.hProcess,&c,&e,&k,&u);
  {std::ofstream f(run/L"process.json");f<<"{\"pid\":"<<proc.dwProcessId<<",\"creation_time\":"<<ticks(c)<<"}";}
  if(proc.hThread)CloseHandle(proc.hThread);
  bool fatalSeen=false,captured=false,captureAttempted=false,initialBreak=false,terminatedAfterFatal=false;uint64_t offset=0,lastMetrics=0;FatalLogTail tail;
  std::ofstream samples(run/L"process-metrics.csv");samples<<"uptime_ms,working_set_bytes,private_bytes,kernel_100ns,user_100ns,handles\n";
  DWORD exitCode=0;
  if(debug)DebugSetProcessKillOnExit(FALSE);
  for(;;) {
    bool done=false;
    if(debug) {
      DEBUG_EVENT event{};
      if(WaitForDebugEvent(&event,100)) {
        DWORD result=DBG_CONTINUE;
        if(event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT && event.u.CreateProcessInfo.hFile)CloseHandle(event.u.CreateProcessInfo.hFile);
        if(event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT && event.u.LoadDll.hFile)CloseHandle(event.u.LoadDll.hFile);
        if(event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT) {
          const auto& ex=event.u.Exception;DWORD code=ex.ExceptionRecord.ExceptionCode;
          bool systemBreak=code==EXCEPTION_BREAKPOINT && ex.dwFirstChance && !initialBreak;
          if(systemBreak)initialBreak=true;
          else {
            result=DBG_EXCEPTION_NOT_HANDLED; // Let all guest/MMIO/texture-watch handlers run normally.
            if(!ex.dwFirstChance || code==0xC0000409 || code==0xC0000602) {
              fatalSeen=true;
              status(run,"fatal exception="+std::to_string(code)+" first_chance="+std::to_string(ex.dwFirstChance)+" address="+std::to_string((uintptr_t)ex.ExceptionRecord.ExceptionAddress));
              if(!captureAttempted) {
                captureAttempted=true;
                HANDLE thread=OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,event.dwThreadId);CONTEXT context{};context.ContextFlags=CONTEXT_ALL;
                EXCEPTION_RECORD record=ex.ExceptionRecord;EXCEPTION_POINTERS pointers{&record,&context};
                MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId,&pointers,FALSE};
                bool hasContext=thread && GetThreadContext(thread,&context);
                captured=dump(proc.hProcess,proc.dwProcessId,run,L"crash-full",hasContext?&info:nullptr);
                if(thread)CloseHandle(thread);
              }
            }
          }
        }
        if(event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT){exitCode=event.u.ExitProcess.dwExitCode;done=true;}
        ContinueDebugEvent(event.dwProcessId,event.dwThreadId,result);
      } else if(GetLastError()!=ERROR_SEM_TIMEOUT && WaitForSingleObject(proc.hProcess,0)==WAIT_OBJECT_0)done=true;
    } else done=WaitForSingleObject(proc.hProcess,100)==WAIT_OBJECT_0;
    if(fatal(run/L"game.log",offset,tail)) {
      fatalSeen=true;
      if(!captureAttempted && !done){captureAttempted=true;captured=dump(proc.hProcess,proc.dwProcessId,run,L"fatal-full");}
      // A fatal handler may hold locks forever after its log was written.
      // Keep the dump attempt first, then retire only the child we launched.
      // Manual dump-result messages are excluded by FatalLogTail.
      if(!done && WaitForSingleObject(proc.hProcess,0)==WAIT_TIMEOUT) {
        terminatedAfterFatal=TerminateProcess(proc.hProcess,0x40000015)!=FALSE;
        status(run,terminatedAfterFatal?"closed owned process after fatal capture attempt":
            "owned fatal process termination failed error="+std::to_string(GetLastError()));
      }
    }
    if(GetTickCount64()-lastMetrics>=1000){metrics(proc.hProcess,samples);lastMetrics=GetTickCount64();}
    if(done)break;
  }
  GetExitCodeProcess(proc.hProcess,&exitCode);CloseHandle(proc.hProcess);
  {std::ofstream f(run/L"exit.json");f<<"{\"exit_code\":"<<exitCode<<",\"fatal_seen\":"<<(fatalSeen?"true":"false")<<",\"external_dump_written\":"<<(captured?"true":"false")<<",\"debug_capture\":"<<(debug?"true":"false")<<",\"owned_process_terminated_after_fatal\":"<<(terminatedAfterFatal?"true":"false")<<"}";}
  return 0;
}
