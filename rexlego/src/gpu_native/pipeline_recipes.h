#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <type_traits>
#include <vector>

#include <d3d12.h>
#include <xxhash.h>
#include "native_gpu_build_info.h"

namespace legodimensions::gpu_native {
namespace {

std::wstring PipelineName(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc);

constexpr uint32_t kMaxRecipes = 8192;
constexpr uint32_t kMaxShaderBytes = 2u << 20;
constexpr uint64_t kMaxRecipeFileBytes = 128ull << 20;
constexpr uint32_t kNoShader = UINT32_MAX;
constexpr auto kShaderStages = std::array{
    &D3D12_GRAPHICS_PIPELINE_STATE_DESC::VS, &D3D12_GRAPHICS_PIPELINE_STATE_DESC::PS,
    &D3D12_GRAPHICS_PIPELINE_STATE_DESC::GS, &D3D12_GRAPHICS_PIPELINE_STATE_DESC::HS,
    &D3D12_GRAPHICS_PIPELINE_STATE_DESC::DS};

template <typename T>
bool ReadRecipeValue(std::istream& file, T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return bool(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template <typename T>
bool WriteRecipeValue(std::ostream& file, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  return bool(file.write(reinterpret_cast<const char*>(&value), sizeof(value)));
}

struct RecipeInput {
  char name[64];
  UINT semantic_index;
  DXGI_FORMAT format;
  UINT slot;
  UINT offset;
  D3D12_INPUT_CLASSIFICATION classification;
  UINT step_rate;
};

// The fixed parts of the descriptor have no pointers. Field order is part of
// the on-disk format. Increment the file version when changing these structs.
struct RecipeState {
  D3D12_BLEND_DESC blend;
  UINT sample_mask;
  D3D12_RASTERIZER_DESC rasterizer;
  D3D12_DEPTH_STENCIL_DESC depth_stencil;
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut;
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
  UINT render_target_count;
  std::array<DXGI_FORMAT, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> render_target_formats;
  DXGI_FORMAT depth_format;
  DXGI_SAMPLE_DESC sample;
  UINT node_mask;
  D3D12_PIPELINE_STATE_FLAGS flags;
};

struct PipelineRecipe {
  std::array<wchar_t, 32> name;
  RecipeState state;
  std::array<uint32_t, kShaderStages.size()> shader_ids;
  std::vector<RecipeInput> inputs;
};

class PipelineRecipes {
 public:
  using Shader = std::vector<uint8_t>;

  bool Record(const std::wstring& name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
    if (recipes_.size() >= kMaxRecipes || names_.contains(name) ||
        desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides ||
        desc.StreamOutput.RasterizedStream || desc.CachedPSO.CachedBlobSizeInBytes ||
        desc.InputLayout.NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return false;

    auto recipe = std::make_shared<PipelineRecipe>();
    std::copy(name.begin(), name.end(), recipe->name.begin());
    recipe->state = {desc.BlendState, desc.SampleMask, desc.RasterizerState, desc.DepthStencilState,
                     desc.IBStripCutValue, desc.PrimitiveTopologyType, desc.NumRenderTargets, {},
                     desc.DSVFormat, desc.SampleDesc, desc.NodeMask, desc.Flags};
    // The existing driver-library key hashes these structs byte for byte.
    std::memcpy(&recipe->state.blend, &desc.BlendState, sizeof(desc.BlendState));
    std::memcpy(&recipe->state.rasterizer, &desc.RasterizerState, sizeof(desc.RasterizerState));
    std::memcpy(&recipe->state.depth_stencil, &desc.DepthStencilState, sizeof(desc.DepthStencilState));
    std::copy(std::begin(desc.RTVFormats), std::end(desc.RTVFormats),
              recipe->state.render_target_formats.begin());
    for (UINT i = 0; i < desc.InputLayout.NumElements; ++i) {
      const auto& element = desc.InputLayout.pInputElementDescs[i];
      RecipeInput input{};
      if (!element.SemanticName) return false;
      const size_t name_length = std::strlen(element.SemanticName);
      if (name_length >= sizeof(input.name)) return false;
      std::memcpy(input.name, element.SemanticName, name_length + 1);
      input.semantic_index = element.SemanticIndex;
      input.format = element.Format;
      input.slot = element.InputSlot;
      input.offset = element.AlignedByteOffset;
      input.classification = element.InputSlotClass;
      input.step_rate = element.InstanceDataStepRate;
      recipe->inputs.push_back(input);
    }
    size_t added_bytes = sizeof(recipe->name) + sizeof(recipe->state) + sizeof(recipe->shader_ids) +
                         sizeof(uint32_t) + recipe->inputs.size() * sizeof(RecipeInput);
    for (auto stage : kShaderStages) {
      const auto& shader = desc.*stage;
      if (shader.BytecodeLength > kMaxShaderBytes ||
          (shader.BytecodeLength && !shader.pShaderBytecode)) return false;
    }
    recipe->shader_ids.fill(kNoShader);
    std::array<uint64_t, kShaderStages.size()> hashes{};
    for (size_t i = 0; i < kShaderStages.size(); ++i) {
      const auto& shader = desc.*kShaderStages[i];
      if (!shader.BytecodeLength) continue;
      hashes[i] = XXH3_64bits(shader.pShaderBytecode, shader.BytecodeLength);
      auto [first, last] = shader_index_.equal_range(hashes[i]);
      for (auto it = first; it != last; ++it) {
        const auto& existing = *shaders_[it->second];
        if (existing.size() == shader.BytecodeLength &&
            std::memcmp(existing.data(), shader.pShaderBytecode, existing.size()) == 0) {
          recipe->shader_ids[i] = it->second;
          break;
        }
      }
      if (recipe->shader_ids[i] == kNoShader) added_bytes += sizeof(uint32_t) + shader.BytecodeLength;
    }
    if (bytes_ + added_bytes > kMaxRecipeFileBytes) return false;
    for (size_t i = 0; i < kShaderStages.size(); ++i) {
      const auto& shader = desc.*kShaderStages[i];
      if (!shader.BytecodeLength || recipe->shader_ids[i] != kNoShader) continue;
      const auto* bytes = static_cast<const uint8_t*>(shader.pShaderBytecode);
      const auto id = static_cast<uint32_t>(shaders_.size());
      shaders_.push_back(std::make_shared<Shader>(bytes, bytes + shader.BytecodeLength));
      shader_index_.emplace(hashes[i], id);
      recipe->shader_ids[i] = id;
    }
    bytes_ += added_bytes;
    names_.insert(name);
    recipes_.push_back(std::move(recipe));
    return true;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC Describe(
      const PipelineRecipe& recipe, ID3D12RootSignature* root,
      std::vector<D3D12_INPUT_ELEMENT_DESC>& inputs) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    for (size_t i = 0; i < kShaderStages.size(); ++i) {
      const auto id = recipe.shader_ids[i];
      if (id != kNoShader) desc.*kShaderStages[i] = {shaders_[id]->data(), shaders_[id]->size()};
    }
    const auto& state = recipe.state;
    std::memcpy(&desc.BlendState, &state.blend, sizeof(desc.BlendState));
    desc.SampleMask = state.sample_mask;
    std::memcpy(&desc.RasterizerState, &state.rasterizer, sizeof(desc.RasterizerState));
    std::memcpy(&desc.DepthStencilState, &state.depth_stencil, sizeof(desc.DepthStencilState));
    desc.IBStripCutValue = state.strip_cut;
    desc.PrimitiveTopologyType = state.topology;
    desc.NumRenderTargets = state.render_target_count;
    std::copy(state.render_target_formats.begin(), state.render_target_formats.end(), desc.RTVFormats);
    desc.DSVFormat = state.depth_format;
    desc.SampleDesc = state.sample;
    desc.NodeMask = state.node_mask;
    desc.Flags = state.flags;
    inputs.clear();
    for (const auto& input : recipe.inputs)
      inputs.push_back({input.name, input.semantic_index, input.format, input.slot, input.offset,
                        input.classification, input.step_rate});
    desc.InputLayout = {inputs.empty() ? nullptr : inputs.data(), static_cast<UINT>(inputs.size())};
    return desc;
  }

  struct Snapshot {
    std::vector<std::shared_ptr<const Shader>> shaders;
    std::vector<std::shared_ptr<const PipelineRecipe>> recipes;

    bool Write(std::ostream& file) const {
      if (!WriteRecipeValue(file, kMagic) || !WriteRecipeValue(file, kNativeGpuBuildFingerprint) ||
          !WriteRecipeValue(file, static_cast<uint32_t>(shaders.size())) ||
          !WriteRecipeValue(file, static_cast<uint32_t>(recipes.size()))) return false;
      for (const auto& shader : shaders)
        if (!WriteRecipeValue(file, static_cast<uint32_t>(shader->size())) ||
            !file.write(reinterpret_cast<const char*>(shader->data()), shader->size())) return false;
      for (const auto& recipe : recipes) {
        if (!WriteRecipeValue(file, recipe->name) || !WriteRecipeValue(file, recipe->state) ||
            !WriteRecipeValue(file, recipe->shader_ids) ||
            !WriteRecipeValue(file, static_cast<uint32_t>(recipe->inputs.size()))) return false;
        for (const auto& input : recipe->inputs)
          if (!WriteRecipeValue(file, input)) return false;
      }
      return true;
    }
  };

  Snapshot TakeSnapshot() const { return {shaders_, recipes_}; }
  const auto& Entries() const { return recipes_; }

  struct MergeResult {
    size_t added = 0;
    size_t duplicates = 0;
    size_t skipped = 0;
  };

  MergeResult Merge(const PipelineRecipes& source) {
    MergeResult result;
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    for (const auto& recipe : source.Entries()) {
      const std::wstring name(recipe->name.begin(), recipe->name.end());
      if (names_.contains(name)) {
        ++result.duplicates;
      } else if (Record(name, source.Describe(*recipe, nullptr, inputs))) {
        ++result.added;
      } else {
        ++result.skipped;
      }
    }
    return result;
  }

  // Read into a temporary database and publish it only after all records pass.
  bool Read(std::istream& file, uint64_t file_size);

 private:
  static constexpr uint64_t kMagic = 0x015350474f47454cull;
  static constexpr size_t kHeaderBytes = sizeof(kMagic) + sizeof(kNativeGpuBuildFingerprint) +
                                         2 * sizeof(uint32_t);
  std::vector<std::shared_ptr<const Shader>> shaders_;
  std::vector<std::shared_ptr<const PipelineRecipe>> recipes_;
  std::unordered_multimap<uint64_t, uint32_t> shader_index_;
  std::unordered_set<std::wstring> names_;
  uint64_t bytes_ = kHeaderBytes;
};

bool PipelineRecipes::Read(std::istream& file, uint64_t file_size) {
  if (file_size < kHeaderBytes || file_size > kMaxRecipeFileBytes) return false;
  uint64_t magic;
  char fingerprint[sizeof(kNativeGpuBuildFingerprint)];
  uint32_t shader_count, recipe_count;
  if (!ReadRecipeValue(file, magic) || magic != kMagic ||
      !ReadRecipeValue(file, fingerprint) ||
      std::memcmp(fingerprint, kNativeGpuBuildFingerprint, sizeof(fingerprint)) ||
      !ReadRecipeValue(file, shader_count) || !ReadRecipeValue(file, recipe_count) ||
      recipe_count > kMaxRecipes || shader_count > recipe_count * kShaderStages.size()) return false;
  PipelineRecipes loaded;
  for (uint32_t id = 0; id < shader_count; ++id) {
    uint32_t size;
    if (!ReadRecipeValue(file, size) || !size || size > kMaxShaderBytes ||
        loaded.bytes_ + sizeof(size) + size > file_size) return false;
    auto shader = std::make_shared<Shader>(size);
    if (!file.read(reinterpret_cast<char*>(shader->data()), size)) return false;
    loaded.shader_index_.emplace(XXH3_64bits(shader->data(), size), id);
    loaded.shaders_.push_back(std::move(shader));
    loaded.bytes_ += sizeof(size) + size;
  }
  std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
  for (uint32_t i = 0; i < recipe_count; ++i) {
    auto recipe = std::make_shared<PipelineRecipe>();
    uint32_t count;
    if (!ReadRecipeValue(file, recipe->name) || !ReadRecipeValue(file, recipe->state) ||
        !ReadRecipeValue(file, recipe->shader_ids) || !ReadRecipeValue(file, count) ||
        count > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT ||
        recipe->state.render_target_count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) return false;
    for (auto id : recipe->shader_ids)
      if (id != kNoShader && id >= shader_count) return false;
    recipe->inputs.resize(count);
    for (auto& input : recipe->inputs)
      if (!ReadRecipeValue(file, input) || !std::memchr(input.name, '\0', sizeof(input.name)))
        return false;
    const std::wstring name(recipe->name.begin(), recipe->name.end());
    if (PipelineName(loaded.Describe(*recipe, nullptr, inputs)) != name ||
        !loaded.names_.insert(name).second) return false;
    loaded.recipes_.push_back(std::move(recipe));
  }
  if (static_cast<uint64_t>(file.tellg()) != file_size) return false;
  loaded.bytes_ = file_size;
  *this = std::move(loaded);
  return true;
}

}  // namespace
}  // namespace legodimensions::gpu_native
