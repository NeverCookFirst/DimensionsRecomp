#pragma once

#include <functional>
#include <initializer_list>

namespace legodimensions::gpu_native {

bool NativeParallelDrawsEnabled();
void FlushNativeDrawBatch();
void FlushFullDrawBatch();
// Flushes a partial batch once it holds a whole preparation chunk.
void FlushFullDrawBatchIdle();
bool NativeDrawBatchPending();
// Runs a queued non-draw task on the coordinator; its commands join the batch as a prepared work item.
void RecordDeferredNativeTask(std::function<void()>& task);
// A deferred task that replaces a resource waits here for pending preparation that references it.
void WaitPendingPreparationFor(std::initializer_list<unsigned> addresses);
// Runs on the recorder once every tape captured before it has been replayed.
void QueueNativeReplayTask(std::function<void()> task);
// Coordinator: flushes the open batch and waits for every posted preparation job.
void WaitAllPreparation();

} // namespace legodimensions::gpu_native
