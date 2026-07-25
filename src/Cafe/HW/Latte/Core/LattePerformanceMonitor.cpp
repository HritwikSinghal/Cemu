#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "WindowSystem.h"
#include "config/CemuConfig.h"

#include <cstdlib>
#include <fstream>

performanceMonitor_t performanceMonitor{};

bool g_lattePerfStatsEnabled{false};

// optional per-frame CSV dump for offline analysis, enabled via the CEMU_PERFSTATS_CSV environment variable
static std::ofstream s_perfCSV;
static bool s_perfCSVActive{false};
static bool s_perfCSVChecked{false};
static uint64 s_perfFrameIndex{0};
static uint64 s_perfLastFrameTsc{0};

static void LattePerformanceMonitor_bottleneckFrameEnd()
{
	auto& bn = performanceMonitor.bottleneck;
	// latch timers
	bn.tmrShaderUpdate.frameFinished();
	bn.tmrFboUpdate.frameFinished();
	bn.tmrTextureUpdate.frameFinished();
	bn.tmrTextureHashTick.frameFinished();
	bn.tmrTextureUpload.frameFinished();
	bn.tmrUniformUpdate.frameFinished();
	bn.tmrIndexDecode.frameFinished();
	bn.tmrBufferCacheSync.frameFinished();
	bn.tmrPipelineLookup.frameFinished();
	bn.tmrDescriptorSets.frameFinished();
	bn.tmrRenderpass.frameFinished();
	bn.tmrSubmit.frameFinished();
	bn.tmrGpuWait.frameFinished();
	bn.tmrIdleSpin.frameFinished();
	bn.tmrFenceWait.frameFinished();
	// latch counters
	bn.cntDrawsFirst.frameFinished();
	bn.cntDrawsFast.frameFinished();
	bn.cntSeqEndTexture.frameFinished();
	bn.cntSeqEndContextReg.frameFinished();
	bn.cntIndexCacheHit.frameFinished();
	bn.cntIndexCacheMiss.frameFinished();
	bn.cntPipelineMiss.frameFinished();
	bn.cntDescSetMiss.frameFinished();
	bn.cntAsyncSkippedDraws.frameFinished();
	bn.cntSubmits.frameFinished();
	bn.cntSubmitsForced.frameFinished();
	bn.cntOcclusionQueries.frameFinished();
	bn.cntTextureReloads.frameFinished();
	bn.cntTextureReloadSlices.frameFinished();
	bn.cntBytesUniformUpload.frameFinished();
	bn.cntBytesTextureUpload.frameFinished();
	bn.cntBytesIndexUpload.frameFinished();
	bn.cntVsyncLateUs.frameFinished();
	// latch GPU busy time -- accumulated as command buffer fences retire (VulkanRenderer::
	// ProcessFinishedCommandBuffers), which can lag the submitting CPU frame by a variable amount,
	// so a single row's gpuBusyUs jitters; treat the column as a short rolling estimate when analyzing
	bn.gpuBusyNsPrevFrame = bn.gpuBusyNsAccum;
	bn.gpuBusyNsAccum = 0;
	// frame wall time
	uint64 nowTsc = PPCTimer_getRawTsc();
	uint64 frameTimeUs = (s_perfLastFrameTsc != 0) ? PPCTimer_tscToMicroseconds(nowTsc - s_perfLastFrameTsc) : 0;
	s_perfLastFrameTsc = nowTsc;
	s_perfFrameIndex++;
	if (s_perfCSVActive && g_lattePerfStatsEnabled)
	{
		auto tUs = [](LattePerfNestingTimer& t) -> uint64 { return PPCTimer_tscToMicroseconds(t.getPreviousFrameValue()); };
		s_perfCSV << s_perfFrameIndex << ',' << frameTimeUs << ',' << (bn.gpuBusyNsPrevFrame / 1000)
			<< ',' << tUs(bn.tmrShaderUpdate) << ',' << tUs(bn.tmrFboUpdate) << ',' << tUs(bn.tmrTextureUpdate)
			<< ',' << tUs(bn.tmrTextureHashTick) << ',' << tUs(bn.tmrTextureUpload) << ',' << tUs(bn.tmrUniformUpdate)
			<< ',' << tUs(bn.tmrIndexDecode) << ',' << tUs(bn.tmrBufferCacheSync) << ',' << tUs(bn.tmrPipelineLookup)
			<< ',' << tUs(bn.tmrDescriptorSets) << ',' << tUs(bn.tmrRenderpass) << ',' << tUs(bn.tmrSubmit)
			<< ',' << tUs(bn.tmrGpuWait) << ',' << tUs(bn.tmrIdleSpin) << ',' << tUs(bn.tmrFenceWait)
			<< ',' << bn.cntDrawsFirst.getPreviousFrameValue() << ',' << bn.cntDrawsFast.getPreviousFrameValue()
			<< ',' << bn.cntSeqEndTexture.getPreviousFrameValue() << ',' << bn.cntSeqEndContextReg.getPreviousFrameValue()
			<< ',' << bn.cntIndexCacheHit.getPreviousFrameValue() << ',' << bn.cntIndexCacheMiss.getPreviousFrameValue()
			<< ',' << bn.cntPipelineMiss.getPreviousFrameValue() << ',' << bn.cntDescSetMiss.getPreviousFrameValue()
			<< ',' << bn.cntAsyncSkippedDraws.getPreviousFrameValue() << ',' << bn.cntSubmits.getPreviousFrameValue()
			<< ',' << bn.cntSubmitsForced.getPreviousFrameValue() << ',' << bn.cntOcclusionQueries.getPreviousFrameValue()
			<< ',' << bn.cntTextureReloads.getPreviousFrameValue() << ',' << bn.cntTextureReloadSlices.getPreviousFrameValue()
			<< ',' << bn.cntBytesUniformUpload.getPreviousFrameValue() << ',' << bn.cntBytesTextureUpload.getPreviousFrameValue()
			<< ',' << bn.cntBytesIndexUpload.getPreviousFrameValue() << ',' << bn.cntVsyncLateUs.getPreviousFrameValue()
			<< '\n';
		// flush every row (one per frame while profiling) so a crash or forced kill doesn't lose the whole dump
		s_perfCSV.flush();
	}
}

void LattePerformanceMonitor_frameEnd()
{
	LattePerformanceMonitor_bottleneckFrameEnd();
	// per-frame stats
	performanceMonitor.gpuTime_shaderCreate.frameFinished();
	performanceMonitor.gpuTime_frameTime.frameFinished();
	performanceMonitor.gpuTime_idleTime.frameFinished();
	performanceMonitor.gpuTime_fenceTime.frameFinished();

	performanceMonitor.gpuTime_dcStageTextures.frameFinished();
	performanceMonitor.gpuTime_dcStageVertexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageShaderAndUniformMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageIndexMgr.frameFinished();
	performanceMonitor.gpuTime_dcStageMRT.frameFinished();
	performanceMonitor.gpuTime_dcStageDrawcallAPI.frameFinished();
	performanceMonitor.gpuTime_waitForAsync.frameFinished();

	uint32 elapsedTime = GetTickCount() - performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate;
	if (elapsedTime >= 1000)
	{
		bool isFirstUpdate = performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate == 0;
		// sum up raw stats
		uint32 totalElapsedTime = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 totalElapsedTimeFPS = GetTickCount() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastUpdate;
		uint32 elapsedFrames = 0;
		uint32 elapsedFrames2S = 0; // elapsed frames for last two entries (seconds)
		uint64 skippedCycles = 0;
		uint64 vertexDataUploaded = 0;
		uint64 vertexDataCached = 0;
		uint64 uniformBankUploadedData = 0;
		uint64 uniformBankUploadedCount = 0;
		uint64 indexDataUploaded = 0;
		uint64 indexDataCached = 0;
		uint32 frameCounter = 0;
		uint32 drawCallCounter = 0;
		uint32 fastDrawCallCounter = 0;
		uint32 shaderBindCounter = 0;
		uint32 recompilerLeaveCount = 0;
		uint32 threadLeaveCount = 0;
		for (sint32 i = 0; i < PERFORMANCE_MONITOR_TRACK_CYCLES; i++)
		{
			elapsedFrames += performanceMonitor.cycle[i].frameCounter;
			skippedCycles += performanceMonitor.cycle[i].skippedCycles;
			vertexDataUploaded += performanceMonitor.cycle[i].vertexDataUploaded;
			vertexDataCached += performanceMonitor.cycle[i].vertexDataCached;
			uniformBankUploadedData += performanceMonitor.cycle[i].uniformBankUploadedData;
			uniformBankUploadedCount += performanceMonitor.cycle[i].uniformBankUploadedCount;
			indexDataUploaded += performanceMonitor.cycle[i].indexDataUploaded;
			indexDataCached += performanceMonitor.cycle[i].indexDataCached;
			frameCounter += performanceMonitor.cycle[i].frameCounter;
			drawCallCounter += performanceMonitor.cycle[i].drawCallCounter;
			fastDrawCallCounter += performanceMonitor.cycle[i].fastDrawCallCounter;
			shaderBindCounter += performanceMonitor.cycle[i].shaderBindCount;
			recompilerLeaveCount += performanceMonitor.cycle[i].recompilerLeaveCount;
			threadLeaveCount += performanceMonitor.cycle[i].threadLeaveCount;
		}
		elapsedFrames = std::max<uint32>(elapsedFrames, 1);
		elapsedFrames2S = performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 0) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S += performanceMonitor.cycle[(performanceMonitor.cycleIndex + PERFORMANCE_MONITOR_TRACK_CYCLES - 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].frameCounter;
		elapsedFrames2S = std::max<uint32>(elapsedFrames2S, 1);
		// calculate stats
		uint64 passedCycles = PPCInterpreter_getMainCoreCycleCounter() - performanceMonitor.cycle[(performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES].lastCycleCount;
		passedCycles -= skippedCycles;
		uint64 vertexDataUploadPerFrame = (vertexDataUploaded / (uint64)elapsedFrames);
		vertexDataUploadPerFrame /= 1024ULL;
		uint64 vertexDataCachedPerFrame = (vertexDataCached / (uint64)elapsedFrames);
		vertexDataCachedPerFrame /= 1024ULL;
		uint64 uniformBankDataUploadedPerFrame = (uniformBankUploadedData / (uint64)elapsedFrames);
		uniformBankDataUploadedPerFrame /= 1024ULL;
		uint32 uniformBankCountUploadedPerFrame = (uint32)(uniformBankUploadedCount / (uint64)elapsedFrames);
		uint64 indexDataUploadPerFrame = (indexDataUploaded / (uint64)elapsedFrames);

		double fps = (double)elapsedFrames2S * 1000.0 / (double)totalElapsedTimeFPS;
		uint32 shaderBindsPerFrame = shaderBindCounter / elapsedFrames;
		passedCycles = passedCycles * 1000ULL / totalElapsedTime;
		uint32 rlps = (uint32)((uint64)recompilerLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		uint32 tlps = (uint32)((uint64)threadLeaveCount * 1000ULL / (uint64)totalElapsedTime);
		// set stats
		performanceMonitor.stats.indexDataUploadPerFrame = indexDataUploadPerFrame;
		// next counter cycle
		sint32 nextCycleIndex = (performanceMonitor.cycleIndex + 1) % PERFORMANCE_MONITOR_TRACK_CYCLES;
		performanceMonitor.cycle[nextCycleIndex].drawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].fastDrawCallCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].frameCounter = 0;
		performanceMonitor.cycle[nextCycleIndex].shaderBindCount = 0;
		performanceMonitor.cycle[nextCycleIndex].lastCycleCount = PPCInterpreter_getMainCoreCycleCounter();
		performanceMonitor.cycle[nextCycleIndex].skippedCycles = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].vertexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedData = 0;
		performanceMonitor.cycle[nextCycleIndex].uniformBankUploadedCount = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataUploaded = 0;
		performanceMonitor.cycle[nextCycleIndex].indexDataCached = 0;
		performanceMonitor.cycle[nextCycleIndex].recompilerLeaveCount = 0;
		performanceMonitor.cycle[nextCycleIndex].threadLeaveCount = 0;
		performanceMonitor.cycleIndex = nextCycleIndex;

		// next update in 1 second
		performanceMonitor.cycle[performanceMonitor.cycleIndex].lastUpdate = GetTickCount();

		if (isFirstUpdate)
		{
			LatteOverlay_updateStats(0.0, 0, 0);
			WindowSystem::UpdateWindowTitles(false, false, 0.0);
		}
		else
		{
			LatteOverlay_updateStats(fps, drawCallCounter / elapsedFrames, fastDrawCallCounter / elapsedFrames);
			WindowSystem::UpdateWindowTitles(false, false, fps);
		}
	}
}

void LattePerformanceMonitor_frameBegin()
{
	performanceMonitor.vk.numDrawBarriersPerFrame.reset();
	performanceMonitor.vk.numBeginRenderpassPerFrame.reset();

	// the CSV stream is opened once per process by design: CEMU_PERFSTATS_CSV is read a single time here
	// and the file stays open (with a flush per row, see LattePerformanceMonitor_bottleneckFrameEnd) for
	// the lifetime of the process, rather than being reopened per profiling session.
	if (!s_perfCSVChecked)
	{
		s_perfCSVChecked = true;
		if (const char* csvPath = std::getenv("CEMU_PERFSTATS_CSV"))
		{
			s_perfCSV.open(csvPath, std::ios::out | std::ios::trunc);
			s_perfCSVActive = s_perfCSV.is_open();
			if (s_perfCSVActive)
				s_perfCSV << "frame,frameUs,gpuBusyUs,shaderUpdateUs,fboUpdateUs,textureUpdateUs,textureHashUs,textureUploadUs,uniformUs,indexUs,bufferSyncUs,pipelineUs,descriptorSetsUs,renderpassUs,submitUs,gpuWaitUs,idleSpinUs,guestFenceUs,drawsFirst,drawsFast,seqEndTexture,seqEndContextReg,indexCacheHit,indexCacheMiss,pipelineMiss,descSetMiss,asyncSkippedDraws,submits,submitsForced,occlusionQueries,textureReloads,textureReloadSlices,bytesUniform,bytesTexture,bytesIndex,vsyncLateUs\n";
		}
	}
	// runtime toggle: collect stats while the debug overlay is shown or a CSV dump is running
	g_lattePerfStatsEnabled = GetConfig().overlay.debug || s_perfCSVActive;
}
