#pragma once

#define PERFORMANCE_MONITOR_TRACK_CYCLES	(5) // one cycle lasts one second

// todo - replace PPCTimer with HighResolutionTimer.h
uint64 PPCTimer_getRawTsc();
uint64 PPCTimer_tscToMicroseconds(uint64 us);

class LattePerfStatTimer
{
public:
	void beginMeasuring()
	{
		timerStart = PPCTimer_getRawTsc();
	}

	void endMeasuring()
	{
		uint64 dif = PPCTimer_getRawTsc() - timerStart;
		currentSum += dif;
	}

	void frameFinished()
	{
		previousFrame = currentSum;
		currentSum = 0;
	}

	uint64 getPreviousFrameValue()
	{
		return previousFrame;
	}

private:
	uint64 currentSum{};
	uint64 previousFrame{};
	uint64 timerStart{};
};

// nesting-safe variant of LattePerfStatTimer: only the outermost begin/end pair is measured.
// Used exclusively by the bottleneck profiling timers below, whose call sites are scoped RAII
// helpers with strictly balanced begin/end pairs. Do NOT use this for the legacy gpuTime_* timers
// above -- some of those have deliberately unbalanced begin/end call sites (e.g. a begin per frame
// with no matching end, or an end skipped on an early-return path) that rely on LattePerfStatTimer's
// unconditional overwrite-on-begin/add-on-end behavior instead of depth tracking.
class LattePerfNestingTimer
{
public:
	// nesting-safe: only the outermost begin/end pair is measured
	void beginMeasuring()
	{
		if (m_nestingDepth++ == 0)
			timerStart = PPCTimer_getRawTsc();
	}

	void endMeasuring()
	{
		cemu_assert_debug(m_nestingDepth > 0);
		if (--m_nestingDepth == 0)
			currentSum += PPCTimer_getRawTsc() - timerStart;
	}

	void frameFinished()
	{
		previousFrame = currentSum;
		currentSum = 0;
	}

	uint64 getPreviousFrameValue()
	{
		return previousFrame;
	}

private:
	uint64 currentSum{};
	uint64 previousFrame{};
	uint64 timerStart{};
	uint32 m_nestingDepth{};
};

class LattePerfStatCounter
{
public:
	void increment()
	{
		m_value++;
	}

	void decrement()
	{
		cemu_assert_debug(m_value > 0);
		m_value--;
	}

	void decrement(uint32 count)
	{
		cemu_assert_debug(count <= m_value);
		m_value -= count;
	}

	uint32 get()
	{
		return m_value;
	}

	void reset()
	{
		m_value = 0;
	}

private:
	std::atomic_uint32_t m_value{};
};

// lightweight per-frame counter for bottleneck stats
// single-writer only: must be written exclusively from the GPU emulation thread (LatteThread)
class LattePerfStatFrameCounter
{
public:
	void increment()
	{
		m_current++;
	}

	void add(uint64 value)
	{
		m_current += value;
	}

	void frameFinished()
	{
		m_previousFrame = m_current;
		m_current = 0;
	}

	uint64 getPreviousFrameValue() const
	{
		return m_previousFrame;
	}

private:
	uint64 m_current{};
	uint64 m_previousFrame{};
};

// Named rather than an unnamed "typedef struct" so that nested members may
// declare member functions: MSVC rejects those inside a class that has a
// typedef name for linkage purposes (C7626), which broke the Windows build once
// bottleneck.addGpuBusyNs was added.
struct performanceMonitor_t
{
	struct
	{
		// CPU
		uint64 lastCycleCount;
		uint64 skippedCycles;
		uint32 recompilerLeaveCount; // increased everytime the recompiler switches back to interpreter
		uint32 threadLeaveCount; // increased everytime a thread gives up it's timeslice
		// GPU
		uint32 lastUpdate;
		uint32 frameCounter;
		uint32 drawCallCounter;
		uint32 fastDrawCallCounter;
		uint32 shaderBindCount;
		uint64 vertexDataUploaded; // amount of vertex data uploaded to GPU (bytes)
		uint64 vertexDataCached; // amount of vertex data reused from GPU cache (bytes)
		uint64 uniformBankUploadedData; // amount of uniform buffer data (excluding remapped uniforms) uploaded to GPU
		uint64 uniformBankUploadedCount; // number of separate uploads for uniformBankDataUploaded
		uint64 indexDataUploaded;
		uint64 indexDataCached;
	}cycle[PERFORMANCE_MONITOR_TRACK_CYCLES];
	sint32 cycleIndex;
	// new stats
	LattePerfStatTimer gpuTime_frameTime;
	LattePerfStatTimer gpuTime_shaderCreate;
	LattePerfStatTimer gpuTime_idleTime; // time spent waiting for new commands from CPU
	LattePerfStatTimer gpuTime_fenceTime; // time spent waiting for fence condition

	LattePerfStatTimer gpuTime_dcStageTextures; // drawcall texture/mrt setup
	LattePerfStatTimer gpuTime_dcStageVertexMgr; // drawcall vertex setup and upload
	LattePerfStatTimer gpuTime_dcStageShaderAndUniformMgr; // drawcall shader setup and uniform management/upload
	LattePerfStatTimer gpuTime_dcStageIndexMgr; // drawcall index data setup and upload
	LattePerfStatTimer gpuTime_dcStageMRT; // drawcall render target API

	LattePerfStatTimer gpuTime_dcStageDrawcallAPI; // drawcall api call
	LattePerfStatTimer gpuTime_waitForAsync; // waiting for operations to complete (e.g. GX2DrawDone or force texture readback) Also includes texture readback and occlusion query polling logic

	// generic
	uint32 numCompiledVS; // number of compiled vertex shader programs
	uint32 numCompiledGS; // number of compiled geometry shader programs
	uint32 numCompiledPS; // number of compiled pixel shader programs

	// Vulkan
	struct  
	{
		LattePerfStatCounter numDescriptorSets;
		LattePerfStatCounter numDescriptorDynUniformBuffers;
		LattePerfStatCounter numDescriptorStorageBuffers;
		LattePerfStatCounter numDescriptorSamplerTextures;
		LattePerfStatCounter numGraphicPipelines;
		LattePerfStatCounter numImages;
		LattePerfStatCounter numImageViews;
		LattePerfStatCounter numSamplers;
		LattePerfStatCounter numRenderPass;
		LattePerfStatCounter numFramebuffer;

		// per frame
		LattePerfStatCounter numDrawBarriersPerFrame;
		LattePerfStatCounter numBeginRenderpassPerFrame;
	}vk;

	// calculated stats (per frame)
	struct
	{
		uint32 indexDataUploadPerFrame;
	}stats;

	// bottleneck profiling stats (use via the LATTE_PERF_* macros below; collection is gated by g_lattePerfStatsEnabled)
	// all values must be written only from the GPU emulation thread (LatteThread)
	struct
	{
		// timers (raw TSC sums, latched at frame end)
		LattePerfNestingTimer tmrShaderUpdate;     // shader state selection incl. PS input table and aux hash
		LattePerfNestingTimer tmrFboUpdate;        // render target derivation (LatteMRT::UpdateCurrentFBO)
		LattePerfNestingTimer tmrTextureUpdate;    // per-draw texture binding sync (LatteTexture_updateTextures)
		LattePerfNestingTimer tmrTextureHashTick;  // periodic texture change-detection hashing
		LattePerfNestingTimer tmrTextureUpload;    // texture slice decode + upload
		LattePerfNestingTimer tmrUniformUpdate;    // uniform gather + upload (full and incremental paths)
		LattePerfNestingTimer tmrIndexDecode;      // index cache lookup + conversion + upload
		LattePerfNestingTimer tmrBufferCacheSync;  // vertex/uniform buffer cache sync (LatteBufferCache_Sync)
		LattePerfNestingTimer tmrPipelineLookup;   // pipeline state hash + cache lookup/create
		LattePerfNestingTimer tmrDescriptorSets;   // descriptor set state hash + lookup/build
		LattePerfNestingTimer tmrRenderpass;       // render pass begin/end handling
		LattePerfNestingTimer tmrSubmit;           // command buffer submission (CPU side)
		LattePerfNestingTimer tmrGpuWait;          // CPU blocked on GPU fences (incl. present pacing waits)
		LattePerfNestingTimer tmrIdleSpin;         // command processor starved (waiting for PM4 data)
		LattePerfNestingTimer tmrFenceWait;        // guest fence waits (IT_WAIT_REG_MEM)
		// counters (latched at frame end)
		LattePerfStatFrameCounter cntDrawsFirst;
		LattePerfStatFrameCounter cntDrawsFast;
		LattePerfStatFrameCounter cntSeqEndTexture;    // draw sequences ended by a texture state change
		LattePerfStatFrameCounter cntSeqEndContextReg; // draw sequences ended by a context register write
		LattePerfStatFrameCounter cntIndexCacheHit;
		LattePerfStatFrameCounter cntIndexCacheMiss;
		LattePerfStatFrameCounter cntPipelineMiss;
		LattePerfStatFrameCounter cntDescSetMiss;
		LattePerfStatFrameCounter cntAsyncSkippedDraws; // draws skipped while their pipeline compiles asynchronously
		LattePerfStatFrameCounter cntSubmits;
		LattePerfStatFrameCounter cntSubmitsForced;     // submits forced outside the drawcall threshold (acquire/readback/query/wait)
		LattePerfStatFrameCounter cntOcclusionQueries;
		LattePerfStatFrameCounter cntTextureReloads;
		LattePerfStatFrameCounter cntTextureReloadSlices;
		LattePerfStatFrameCounter cntBytesUniformUpload;
		LattePerfStatFrameCounter cntBytesTextureUpload;
		LattePerfStatFrameCounter cntBytesIndexUpload;
		LattePerfStatFrameCounter cntVsyncLateUs;       // sum of microseconds by which emulated vsync events fired late
		// GPU busy time measured via Vulkan timestamp queries (nanoseconds, accumulated as results arrive)
		uint64 gpuBusyNsAccum{};
		uint64 gpuBusyNsPrevFrame{};

		void addGpuBusyNs(uint64 ns)
		{
			gpuBusyNsAccum += ns;
		}
	}bottleneck;
};

extern performanceMonitor_t performanceMonitor;

// runtime toggle for bottleneck profiling
// mirrors the overlay debug setting; also forced on while CSV dumping (CEMU_PERFSTATS_CSV env var) is active
// when false, all LATTE_PERF_* call sites cost a single predictable branch
extern bool g_lattePerfStatsEnabled;

void LattePerformanceMonitor_frameEnd();
void LattePerformanceMonitor_frameBegin();

// RAII scope timer for bottleneck stats. Timers are nesting-safe (only the outermost scope of the same timer is measured)
class LattePerfScopedTimer
{
public:
	LattePerfScopedTimer(LattePerfNestingTimer& timer)
		: m_timer(g_lattePerfStatsEnabled ? &timer : nullptr)
	{
		if (m_timer)
			m_timer->beginMeasuring();
	}

	~LattePerfScopedTimer()
	{
		if (m_timer)
			m_timer->endMeasuring();
	}

private:
	LattePerfNestingTimer* m_timer;
};

#define _LATTE_PERF_CONCAT2(__a, __b) __a##__b
#define _LATTE_PERF_CONCAT(__a, __b) _LATTE_PERF_CONCAT2(__a, __b)

// measure the remainder of the enclosing scope, e.g. LATTE_PERF_SCOPE(tmrIndexDecode);
#define LATTE_PERF_SCOPE(__timer) LattePerfScopedTimer _LATTE_PERF_CONCAT(_perfScope_, __LINE__)(performanceMonitor.bottleneck.__timer)
// increment a per-frame counter, e.g. LATTE_PERF_COUNT(cntDrawsFirst);
#define LATTE_PERF_COUNT(__counter) do { if (g_lattePerfStatsEnabled) performanceMonitor.bottleneck.__counter.increment(); } while (0)
// add a value to a per-frame counter, e.g. LATTE_PERF_ADD(cntBytesIndexUpload, size);
#define LATTE_PERF_ADD(__counter, __value) do { if (g_lattePerfStatsEnabled) performanceMonitor.bottleneck.__counter.add(__value); } while (0)

#define beginPerfMonProfiling(__obj) if( THasProfiling ) __obj.beginMeasuring()
#define endPerfMonProfiling(__obj) if( THasProfiling ) __obj.endMeasuring()