#pragma once
#include <memory>
#include <cstdint>
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
namespace rex::ui { class ImGuiDrawer; class WindowedAppContext; class ImGuiDialog; class ImmediateDrawer; struct FrameStats; }
namespace legodimensions::gpu_native {
struct OverlayPacket;
std::shared_ptr<const OverlayPacket> NativeOverlayPacket();
std::unique_ptr<rex::ui::ImmediateDrawer> CreateNativeImmediateDrawer();
std::unique_ptr<rex::ui::ImGuiDialog> CreateNativeGraphicsMenu(rex::ui::ImGuiDrawer*);
// UI-thread install/detach. The renderer only queues asynchronous CPU work;
// it never waits for the UI while holding recording/device locks.
void AttachNativeOverlays(rex::ui::WindowedAppContext&, rex::ui::ImGuiDrawer*);
void DetachNativeOverlays();
void QueueNativeOverlayFrame(uint32_t width, uint32_t height);
unsigned PrepareNativeOverlayPipelines(ID3D12Device*, uint32_t format);
void RenderNativeOverlays(ID3D12Device*, ID3D12GraphicsCommandList*,
    uint32_t format, uint32_t slot, uint64_t serial, uint32_t width, uint32_t height);
void SetNativeOverlayDeviceReady(bool ready);
void ResetNativeOverlayGpu(); // Only after all submission fences complete.
void PublishNativeOverlayStats(double fps, double milliseconds, uint64_t frame);
rex::ui::FrameStats NativeOverlayStats();
} // namespace legodimensions::gpu_native
