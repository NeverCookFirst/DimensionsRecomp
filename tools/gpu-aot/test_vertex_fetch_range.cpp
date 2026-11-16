
#include "gpu_native/vertex_fetch_range.h"
#include "gpu_native/shader_input_usage.h"
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstdlib>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){printf("FAILED %s line %d\n",#x,__LINE__);exit(1);}}while(0)
int main(int argc,char**argv){
 CHECK(argc==2);std::ifstream f(argv[1],std::ios::binary);std::vector<uint8_t>b((std::istreambuf_iterator<char>(f)),{});
 for(auto name:{"POSITION","NORMAL","TANGENT","COLOR","TEXCOORD","BLENDINDICES","BLENDWEIGHT"}) {auto r=ShaderReadsVertexInput(b,name,name==std::string_view("COLOR")?1:0);CHECK(r&&*r);}
 CHECK(ShaderReadsVertexInput(b,"COLOR",0)==false);CHECK(ShaderReadsVertexInput(b,"POSITION",1)==false);
 CHECK(!ShaderReadsVertexInput({},"POSITION",0).has_value());auto shortb=b;shortb.resize(36);CHECK(!ShaderReadsVertexInput(shortb,"POSITION",0).has_value());
 // Exact captured bad draw: stream0 fits, required stream1 does not.
 CHECK(VertexFetchRangeFits(0,254,0,24,0,0,12,8192));
 CHECK(!VertexFetchRangeFits(0,254,0,36,396,24,4,4096));
 CHECK(VertexFetchRangeFits(0,101,0,36,396,24,4,4096));
 // Exact attribute end, signed base, and overflow checks independent of windows.
 CHECK(VertexFetchRangeFits(3,5,-3,24,0,0,12,60));
 CHECK(!VertexFetchRangeFits(3,5,-4,24,0,0,12,60));
 CHECK(!VertexFetchRangeFits(UINT32_MAX,UINT32_MAX,INT32_MAX,UINT32_MAX,UINT32_MAX,0,4,UINT32_MAX));
 CHECK(!VertexFetchRangeFits(0,1,0,4,0,2,4,16));
 CHECK(!VertexFetchRangeFits(0,1,0,4,0,0,0,16));
 CHECK(!VertexFetchRangeFits(2,1,0,4,0,0,4,16));
 CHECK(!VertexFetchRangeFits(0,1,0,4,UINT32_MAX,0,4,16));
 puts("PASS captured shader usage, captured invalid stream, padding, signed base and overflow");
}
