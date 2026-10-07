"""Exercise the actual prelink output transaction and emitter under I/O failures.

The Windows variant uses controlled API shims on the host, proving branch/flag
selection and failure propagation; it does not replace a Windows SDK compile.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
parser.add_argument('--negative-control', action='store_true',
                    help='Use the original direct, unchecked publication strategy; it must fail')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'tools/gpu-aot/prelink_shader_cache.cpp').read_text()
helper = source[source.index('class PrelinkOutput {'):source.index('void EmitBytes(')]
start = source.index('void EmitBytes(')
emitter = source[start:source.index('\n}  // namespace', start)]
start = source.index('  PrelinkOutput output(argv[1]);')
publication = source[start:source.index('\n  std::printf("prelink: %zu variant', start)]
# Only I/O boundaries are intercepted. The actual production transaction,
# emission body, main failure returns and complete generated metadata remain.
helper = helper.replace('std::fflush(', 'FaultFlush(').replace('std::fclose(', 'FaultClose(')
helper = helper.replace('fsync(', 'FaultSync(').replace('std::rename(', 'FaultReplace(')
publication = publication.replace('  if (!f) {', '  InjectWriteFailure(f);\n  if (!f) {', 1)
if args.negative_control:
    publication = publication.replace('  PrelinkOutput output(argv[1]);\n  FILE* f = output.file();',
                                      '  FILE* f = std::fopen(argv[1], "wb");')
    begin = publication.index('  if (!output.commit())')
    publication = publication[:begin] + '  std::fclose(f);\n'

preamble = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
enum class Fault { None, Write, BufferedWrite, Flush, Sync, Close, Replace };
Fault fault=Fault::None;
std::filesystem::path observed;
std::string prior;
int close_calls=0;
std::string Read(const std::filesystem::path& path) {
  std::ifstream stream(path,std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),{});
}
void Write(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream stream(path,std::ios::binary); stream<<bytes; assert(stream);
}
int FaultFlush(FILE* file) {
  if (!observed.empty()) assert(Read(observed)==prior && "old bank changed before commit");
  const int result=std::fflush(file);
  return fault==Fault::Flush ? EOF : result;
}
int FaultSync(int fd) { const int result=fsync(fd); return fault==Fault::Sync ? -1 : result; }
int FaultClose(FILE* file) {
  ++close_calls; const int result=std::fclose(file);
  return fault==Fault::Close ? EOF : result;
}
int FaultReplace(const char* from,const char* to) {
  if(fault==Fault::Replace) { errno=EIO; return -1; }
  return std::rename(from,to);
}
void InjectWriteFailure(FILE* file) {
  if(!file || (fault!=Fault::Write && fault!=Fault::BufferedWrite)) return;
  if(fault==Fault::Write) assert(setvbuf(file,nullptr,_IONBF,0)==0);
  int full=open("/dev/full",O_WRONLY); assert(full>=0);
  assert(dup2(full,fileno(file))>=0); close(full);
}
#ifdef TEST_WINDOWS_API
// Standard library headers above retain their host ABI. Only the actual
// production Windows branch below is selected and its OS boundaries shimmed.
#define _WIN32
constexpr int _O_CREAT=O_CREAT,_O_EXCL=O_EXCL,_O_WRONLY=O_WRONLY,_O_BINARY=0x8000;
constexpr int _SH_DENYRW=0x10,_S_IREAD=0400,_S_IWRITE=0200;
constexpr unsigned MOVEFILE_REPLACE_EXISTING=1,MOVEFILE_WRITE_THROUGH=8;
unsigned GetCurrentProcessId() { return static_cast<unsigned>(getpid()); }
int _wsopen_s(int* fd,const char* name,int flags,int share,int permissions) {
  assert(flags==(_O_CREAT|_O_EXCL|_O_WRONLY|_O_BINARY));
  assert(share==_SH_DENYRW && permissions==(_S_IREAD|_S_IWRITE));
  *fd=open(name,O_CREAT|O_EXCL|O_WRONLY,0600); return *fd<0?errno:0;
}
FILE* _wfdopen(int fd,const wchar_t* mode) {
  assert(std::wstring(mode)==L"wb"); return fdopen(fd,"wb");
}
int _close(int fd) { return close(fd); }
int _fileno(FILE* file) { return fileno(file); }
int _commit(int fd) { return FaultSync(fd); }
bool MoveFileExW(const char* from,const char* to,unsigned flags) {
  assert(flags==(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));
  return FaultReplace(from,to)==0;
}
#endif
'''
body = r'''
int Produce(const std::filesystem::path& path) {
  const std::string destination=path.string();
  const char* argv[]={nullptr,destination.c_str()};
  struct OutEntry { uint64_t hash; uint32_t masked,offset,size; };
  std::vector<OutEntry> entries{{0x1234,1,0,32}};
  std::vector<uint8_t> blob(32,7),comp{0,255,34,92,128};
  size_t comp_len=comp.size();
''' + publication + r'''
  return 0;
}
bool HasTemporary(const std::filesystem::path& parent,const std::string& prefix) {
  for(const auto& row:std::filesystem::directory_iterator(parent))
    if(row.path().filename().string().find(prefix)==0) return true;
  return false;
}
int main(int argc,char** argv) {
  assert(argc==2); std::filesystem::path root=argv[1];
  std::filesystem::create_directories(root);
  auto target=root/"bank with spaces.cpp";
  Write(target,"previous validated bank\n"); observed=target; prior=Read(target);
  // A crashed/concurrent transaction's temporary file must not be opened or
  // removed. Exclusive creation skips its name instead of truncating it.
  auto collision=std::filesystem::path(target.string()+".tmp."+std::to_string(getpid())+".0");
  Write(collision,"other transaction");
  assert(Produce(target)==0);
  assert(Read(collision)=="other transaction"); std::filesystem::remove(collision);
  std::string complete=Read(target);
  assert(complete.find("const size_t g_linkedDxilCacheCompressedSize = 5;")!=std::string::npos);
  assert(complete.find("\\x00\\xFF\\x22\\x5C\\x80")!=std::string::npos);
  assert(!HasTemporary(root,"bank with spaces.cpp.tmp."));
  for(Fault value:{Fault::Write,Fault::BufferedWrite,Fault::Flush,Fault::Sync,Fault::Close,Fault::Replace}) {
    fault=value; Write(target,complete); prior=complete; close_calls=0;
    assert(Produce(target)==1 && "failed publication reported success");
    assert(Read(target)==complete && "failed publication destroyed previous bank");
    assert(close_calls==1 && "temporary FILE must close once");
    assert(!HasTemporary(root,"bank with spaces.cpp.tmp."));
  }
  fault=Fault::None; observed.clear();
  // Real rename/MoveFile replacement failure, without an injected return code.
  auto directory_target=root/"existing directory";
  std::filesystem::create_directory(directory_target);
  Write(directory_target/"keep","untouched");
  assert(Produce(directory_target)==1);
  assert(Read(directory_target/"keep")=="untouched");
  assert(!HasTemporary(root,"existing directory.tmp."));
  auto impossible=root/"parent is a file"; Write(impossible,"keep parent");
  assert(Produce(impossible/"output.cpp")==1); assert(Read(impossible)=="keep parent");
  // An abandoned transaction/exception path cleans only its sibling file.
  Write(target,complete);
  { PrelinkOutput aborted(target.string().c_str()); assert(aborted.file());
    std::fputs("incomplete",aborted.file()); }
  assert(Read(target)==complete && !HasTemporary(root,"bank with spaces.cpp.tmp."));
  auto fresh=root/"fresh.cpp"; assert(Produce(fresh)==0); assert(Read(fresh)==complete);
  { PrelinkOutput committed(target.string().c_str()); assert(committed.file());
    std::fputs("replacement",committed.file()); assert(committed.commit());
    assert(!committed.commit()); }
  assert(Read(target)=="replacement");
}
'''
args.output.mkdir(parents=True, exist_ok=True)
output = args.output.resolve()
for name, defines in (('posix', []), ('windows-api', ['-DTEST_WINDOWS_API'])):
    cpp = output / (name + '.cpp')
    executable = output / (name + '.exe')
    cpp.write_text(preamble + helper + emitter + body)
    subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', *defines,
                    str(cpp), '-o', str(executable)], check=True, timeout=20)
    # Outputs are separate per invocation, preserving the failed negative-control evidence.
    run_directory = Path(tempfile.mkdtemp(prefix=name + '-run-', dir=output))
    result = subprocess.run([str(executable), str(run_directory)],
                            text=True, capture_output=True, timeout=8)
    if args.negative_control:
        assert result.returncode != 0 and 'failed publication reported success' in result.stderr, result.stderr
    else:
        assert result.returncode == 0, result.stderr
print('PASS: actual prelink emitter/transaction preserves prior banks on write, buffered flush, '
      'sync, close, replacement and creation failure; exclusive siblings and cleanup checked '
      'for POSIX and controlled Windows API branches' if not args.negative_control else
      'PASS: original unchecked direct-publication strategy rejected by the production fixture')
