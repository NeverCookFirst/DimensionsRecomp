#pragma once
#include <memory>
namespace plume { struct RenderCommandList; }
namespace legodimensions::gpu_native {
// One hardware query per actual draw, shared by all active guest scopes.
// Hardware BEGIN/END never cross a command-list submission boundary.
class QueryDrawScope {
 public:
  // sample_scale converts host samples to guest-resolution samples (logical area over host area).
  QueryDrawScope(plume::RenderCommandList* commands, double sample_scale);
  ~QueryDrawScope();
  QueryDrawScope(const QueryDrawScope&) = delete;
  QueryDrawScope& operator=(const QueryDrawScope&) = delete;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void FailActiveQueries();
void ResetQueryResources();
}
