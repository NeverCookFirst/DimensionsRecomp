#include <cassert>
#include "gpu_native/temporal_composition.h"
using namespace legodimensions::gpu_native;
int main(){
  TemporalCompositionDraw base;
  base.vertex=0x99EA1E96047332E2ull;base.pixel=0x663E3BD3A64A708Eull;
  base.source=base.target=base.depth={1280,720,1129,635};
  base.count=1536;base.pixel_textures=1;base.color_mask=15;
  base.full_viewport=base.full_scissor=true;
  assert(CanComposeTemporalScene(base));
  for(auto width:{640u,960u,1129u,1280u,1920u}){
    auto draw=base;draw.source.width=draw.target.width=draw.depth.width=width;
    for(auto vertex:{0x99EA1E96047332E2ull,0x22C61B6515A75A6Aull})
      for(auto pixel:{0x663E3BD3A64A708Eull,0x69BC5914F33872EAull}){
        draw.vertex=vertex;draw.pixel=pixel;assert(CanComposeTemporalScene(draw));
      }
  }
  for(auto field:{&TemporalCompositionDraw::indexed,&TemporalCompositionDraw::extra_targets,
      &TemporalCompositionDraw::depth_test,&TemporalCompositionDraw::stencil_test,
      &TemporalCompositionDraw::blend,&TemporalCompositionDraw::alpha_test,&TemporalCompositionDraw::same_resource}){
    auto draw=base;draw.*field=true;assert(!CanComposeTemporalScene(draw));
  }
  for(auto field:{&TemporalCompositionDraw::full_viewport,&TemporalCompositionDraw::full_scissor}){
    auto draw=base;draw.*field=false;assert(!CanComposeTemporalScene(draw));
  }
  auto draw=base;draw.vertex^=1;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.pixel^=1;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.count=6;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.start=1;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.vertex_textures=1;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.pixel_textures=3;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.color_mask=7;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.source.width=0;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.depth.width=1920;assert(!CanComposeTemporalScene(draw));
  draw=base;draw.target.logical_width=640;assert(!CanComposeTemporalScene(draw));
}
