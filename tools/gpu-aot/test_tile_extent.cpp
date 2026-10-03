#include "../../rexlego/src/gpu_native/tile_extent.h"
#include <array>
#include <cassert>
#include <iostream>

using namespace legodimensions::gpu_native;
int main() {
  std::array tiles{NativeTileRect{0, 0, 1280, 512}, NativeTileRect{0, 512, 1280, 720}};
  auto extent = LogicalTileExtent(tiles);
  assert(extent.width == 1280 && extent.height == 720);
  std::swap(tiles[0], tiles[1]);
  assert(LogicalTileExtent(tiles).height == 720);
  tiles[0].top = 513; // Hole between the guest tiles.
  assert(!LogicalTileExtent(tiles).width);
  tiles[0].top = 511; // Overlap is not a full-frame partition.
  assert(!LogicalTileExtent(tiles).width);
  tiles[0].top = -1;
  assert(!LogicalTileExtent(tiles).width);
  assert(!LogicalTileExtent({}).width);
  std::array grid{NativeTileRect{0,0,640,360}, NativeTileRect{640,0,1280,360},
                  NativeTileRect{0,360,640,720}, NativeTileRect{640,360,1280,720}};
  assert(LogicalTileExtent(grid).height == 720);
  grid[3].right = 16385;
  assert(!LogicalTileExtent(grid).width);
  std::cout << "tile extent tests passed\n";
}
