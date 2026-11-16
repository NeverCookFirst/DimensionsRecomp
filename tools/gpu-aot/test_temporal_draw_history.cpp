#include <cassert>
#include "gpu_native/temporal_draw_history.h"
#include "gpu_native/temporal_texture_history.h"
using namespace legodimensions::gpu_native;
int main() {
  TemporalDrawHistory history(2);
  TemporalDrawKey key{};
  TemporalVertexConstants a{}, b{};
  a[20]=b[20]=42;b[0]=7;
  assert(!history.Record(key,a).valid);
  history.EndFrame(true);
  auto previous=history.Record(key,b);
  assert(previous.valid && previous.constants==a);
  assert(history.Record(key,b).valid); // Same instance repeated identically.
  history.EndFrame(true);
  assert(history.Record(key,b).constants==b);
  auto changed=b;changed[20]=43;
  assert(!history.Record(key,changed).valid); // Noncamera state / hash collision.
  history.EndFrame(true);
  assert(!history.Record(key,changed).valid); // Ambiguous duplicate last frame.
  history.EndFrame(false);
  assert(!history.Record(key,changed).valid); // Failed presentation drops history.
  history.EndFrame(true);
  auto reused=key;reused[1]=99; // Changed geometry bytes/generation at same address.
  assert(!history.Record(reused,changed).valid);
  history.EndFrame(true);
  assert(!history.Record(key,changed).valid); // Disappeared instance cannot resurrect.
  assert(history.Record(reused,changed).valid);
  auto another=reused;another[2]=100;
  assert(!history.Record(another,changed).valid);
  auto overflow=another;overflow[2]=101;
  assert(!history.Record(overflow,changed).valid);
  history.EndFrame(true);
  assert(!history.Record(overflow,changed).valid);
  history.Reset();assert(!history.Record(reused,changed).valid);
  history.Reset();
  a={};b={};a[48*4]=b[48*4]=1;
  assert(!history.Record(key,a,true).valid);history.EndFrame(true);
  b[0]=7;b[4*4]=19;b[19*4+1]=9;b[92*4]=9; // camera, view and wind evolve.
  previous=history.Record(key,b,true);
  assert(previous.valid && previous.constants==a);history.EndFrame(true);
  b[51*4]=100;assert(!history.Record(key,b,true).valid); // object relocation.
  history.EndFrame(true);assert(!history.Record(key,b,false).valid); // profile change.

  // Actual texture content keys: camera motion may reuse unchanged CPU texels;
  // replaced animation data at the same resource/descriptor must reject it.
  TemporalTextureHistory textures;
  textures.required=(1u<<16)|(1u<<17);
  assert(!textures.Complete());
  textures.Observe(16,0,true); // Zero is a valid content hash, not a sentinel.
  assert(!textures.Complete());
  textures.Observe(17,42,false);assert(!textures.Complete());
  textures.Observe(17,42,true);assert(textures.Complete());
  textures.Observe(32,1,true); // An invalid slot cannot shift by 32.
  auto texture_key=key;
  std::copy(textures.content.begin(),textures.content.end(),texture_key.begin()+120);
  history.Reset();a={};b={};b[0]=1;
  assert(!history.Record(texture_key,a).valid);history.EndFrame(true);
  assert(history.Record(texture_key,b).valid);history.EndFrame(true);
  textures.Observe(17,43,true);
  std::copy(textures.content.begin(),textures.content.end(),texture_key.begin()+120);
  assert(!history.Record(texture_key,b).valid);history.EndFrame(true);
  assert(history.Record(texture_key,b).valid);
  b[19*4]=123;assert(!history.Record(texture_key,b).valid); // Unknown wind stays strict.
  textures.Observe(16,0,false);assert(!textures.Complete());

}
