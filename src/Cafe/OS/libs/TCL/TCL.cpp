#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/TCL/TCL.h"

#include "HW/Latte/Core/LattePM4.h"

#include <condition_variable>

namespace TCL
{
	SysAllocator<coreinit::OSEvent> s_updateRetirementEvent;
	uint64 s_currentRetireMarker = 0;

	struct TCLStatePPC // mapped into PPC space
	{
		uint64be gpuRetireMarker; // written by GPU
	};

	SysAllocator<TCLStatePPC> s_tclStatePPC;

	// called from GPU for timestamp EOP event
	void TCLGPUNotifyNewRetirementTimestamp()
	{
		// gpuRetireMarker is updated via event eop command
		__OSLockScheduler();
		coreinit::OSSignalEventAllInternal(s_updateRetirementEvent.GetPtr());
		__OSUnlockScheduler();
	}

	int TCLTimestamp(TCLTimestampId id, uint64be* timestampOut)
	{
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			MEMPTR<uint32> b;
			// this is the timestamp of the last buffer that was retired by the GPU
			stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
			*timestampOut = retireTimestamp.load();
			return 0;
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLTimestamp(): Unsupported timestamp ID {}", (uint32)id);
			*timestampOut = 0;
			return 0;
		}
	}

	int TCLWaitTimestamp(TCLTimestampId id, uint64 waitTs, uint64 timeout)
	{
		if (id == TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED)
		{
			while ( true )
			{
				stdx::atomic_ref<uint64be> retireTimestamp(s_tclStatePPC->gpuRetireMarker);
				uint64 currentTimestamp = retireTimestamp.load();
				if (currentTimestamp >= waitTs)
					return 0;
				coreinit::OSWaitEvent(s_updateRetirementEvent.GetPtr());
			}
		}
		else
		{
			cemuLog_log(LogType::Force, "TCLWaitTimestamp(): Unsupported timestamp ID {}", (uint32)id);
		}
		return 0;
	}

	static constexpr uint32 TCL_RING_BUFFER_SIZE = 4096; // in U32s

	std::atomic<uint32> tclRingBufferA[TCL_RING_BUFFER_SIZE];
	std::atomic<uint32> tclRingBufferA_readIndex{0};
	std::atomic<uint32> tclRingBufferA_writeIndex{0};

	// Consumer (Latte GPU thread) blocking-wait state, used by TCLWaitForRBData(). Kept separate
	// from the ring buffer indices themselves: on the common "ring already has data" path, no lock
	// is ever touched.
	std::mutex s_ringConsumerWaitMutex;
	std::condition_variable s_ringConsumerWaitCV;
	std::atomic<bool> s_ringConsumerWaiting{false};

	// GPU code calls this to grab the next command word
	bool TCLGPUReadRBWord(uint32& cmdWord)
	{
		uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::relaxed);
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::acquire);
		if (readIndex == writeIndex)
			return false;
		cmdWord = tclRingBufferA[readIndex].load(std::memory_order::relaxed);
		tclRingBufferA_readIndex.store((readIndex + 1) % TCL_RING_BUFFER_SIZE, std::memory_order::release);
		return true;
	}

	// Blocks the calling (Latte GPU) thread until either new ring buffer data is published or
	// timeoutUs microseconds have elapsed, whichever comes first. Called from the consumer's idle
	// spin loop instead of yield()-ing unconditionally, so the thread can actually sleep instead of
	// busy-polling. The wait is bounded (never indefinite) because the caller still needs to service
	// the polled vsync timer and other periodic duties at bounded latency even while the ring is empty.
	void TCLWaitForRBData(uint32 timeoutUs)
	{
		std::unique_lock lock(s_ringConsumerWaitMutex);
		s_ringConsumerWaiting.store(true, std::memory_order::relaxed);
		// StoreLoad ordering barrier for the sleep/wake handshake with the producer side
		// (TCLNotifyRBDataAvailable()). This is a Dekker-style race: we store the waiting flag then
		// load the write index below, while the producer stores the write index then loads the flag.
		// Under TSO (and the C++ memory model) each side's store may linger in its store buffer past
		// its own subsequent load, so without a barrier the consumer can read a stale (empty) write
		// index while the producer reads a stale waiting==false -- the notify is skipped and we sleep
		// the whole timeout with work already queued. A matching seq_cst fence on each side puts the
		// four accesses (our flag-store + index-load here, the producer's index-store + flag-load) into
		// a single total order, so at least one side must observe the other's store: either we see the
		// newly published data and return immediately, or the producer sees the waiting flag set and
		// takes s_ringConsumerWaitMutex to notify. Because that notify path also takes the mutex, which
		// we hold here until wait_for() atomically releases it, the notify cannot slip in between this
		// re-check and the wait: it either fired before us (harmless -- the outer loop rechecks) or it
		// blocks on the mutex until we are genuinely waiting.
		std::atomic_thread_fence(std::memory_order::seq_cst);
		uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::relaxed);
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::acquire);
		if (readIndex != writeIndex)
		{
			s_ringConsumerWaiting.store(false, std::memory_order::relaxed);
			return;
		}
		s_ringConsumerWaitCV.wait_for(lock, std::chrono::microseconds(timeoutUs));
		s_ringConsumerWaiting.store(false, std::memory_order::relaxed);
	}

	// Called by producer-side code right after it publishes new ring buffer data (i.e. right after the
	// release-store of tclRingBufferA_writeIndex). Cheap when nobody is waiting: a seq_cst fence plus a
	// single relaxed atomic load, so the common (no-waiter) case still never touches the mutex.
	static void TCLNotifyRBDataAvailable()
	{
		// Producer half of the StoreLoad handshake documented in TCLWaitForRBData(): this fence orders
		// the caller's preceding release-store of tclRingBufferA_writeIndex ahead of the flag load
		// below, mirroring the consumer's fence between its flag-store and index-load. The two seq_cst
		// fences give the four accesses one total order, so we cannot both miss the waiting flag and
		// leave a just-woken-checking consumer having missed the data we published.
		std::atomic_thread_fence(std::memory_order::seq_cst);
		if (!s_ringConsumerWaiting.load(std::memory_order::relaxed))
			return;
		std::lock_guard lock(s_ringConsumerWaitMutex);
		s_ringConsumerWaitCV.notify_one();
	}

	void TCLWaitForRBSpace(uint32be numU32s)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);
		uint32 spinCount = 0;
		while (true)
		{
			uint32 readIndex = tclRingBufferA_readIndex.load(std::memory_order::acquire);
			uint32 distance = (readIndex + TCL_RING_BUFFER_SIZE - writeIndex) & (TCL_RING_BUFFER_SIZE - 1);
			if (writeIndex == readIndex) // buffer completely empty
				distance = TCL_RING_BUFFER_SIZE;
			if (distance >= numU32s + 1) // assume distance minus one, because we are never allowed to completely wrap around
				break;
			// ring full is rare (consumer is normally far faster than the producer). Spin briefly first,
			// then fall back to yielding so a stalled consumer doesn't leave us pegging the core.
			if (spinCount < 4096)
			{
				_mm_pause();
				spinCount++;
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	// this function assumes that TCLWaitForRBSpace was called and that there is enough space
	void TCLWriteCmd(uint32be* cmd, uint32 cmdLen)
	{
		uint32 writeIndex = tclRingBufferA_writeIndex.load(std::memory_order::relaxed);

		while (cmdLen > 0)
		{
			tclRingBufferA[writeIndex].store(*cmd, std::memory_order::relaxed);
			writeIndex++;
			writeIndex &= (TCL_RING_BUFFER_SIZE - 1);
			cmd++;
			cmdLen--;
		}

		tclRingBufferA_writeIndex.store(writeIndex, std::memory_order::release);
		TCLNotifyRBDataAvailable(); // wake the consumer if it is blocked in TCLWaitForRBData(). Also
		                            // covers TCLSubmitRetireMarker(), which publishes via this same function.
	}

	#define EVENT_TYPE_TS		5

	void TCLSubmitRetireMarker(bool triggerEventInterrupt)
	{
		s_currentRetireMarker++;
		uint32be cmd[6];
		cmd[0] = pm4HeaderType3(IT_EVENT_WRITE_EOP, 5);
		cmd[1] = (4 | (EVENT_TYPE_TS << 8)); // event type (bits 8-15) and event index (bits 0-7).
		cmd[2] = MEMPTR<void>(&s_tclStatePPC->gpuRetireMarker).GetMPTR(); // address lower 32bits + data sel bits
		cmd[3] = 0x40000000; // select 64bit write, lower 16 bits are the upper bits of the address
		if (triggerEventInterrupt)
			cmd[3] |= 0x2000000; // trigger interrupt after value has been written
		cmd[4] = (uint32)s_currentRetireMarker; // data lower 32 bits
		cmd[5] = (uint32)(s_currentRetireMarker>>32); // data higher 32 bits
		TCLWriteCmd(cmd, 6);
	}

	int TCLSubmitToRing(uint32be* cmd, uint32 cmdLen, betype<TCLSubmissionFlag>* controlFlags, uint64be* timestampValueOut)
	{
		TCLSubmissionFlag flags = *controlFlags;
		cemu_assert_debug(timestampValueOut); // handle case where this is null

		// make sure there is enough space to submit all commands at one
		uint32 totalCommandLength = cmdLen;
		totalCommandLength += 6; // space needed for TCLSubmitRetireMarker

		TCLWaitForRBSpace(totalCommandLength);

		// submit command buffer
		TCLWriteCmd(cmd, cmdLen);

		// create new marker timestamp and tell GPU to write it to our variable after its done processing the command
		if ((HAS_FLAG(flags, TCLSubmissionFlag::USE_RETIRED_MARKER)))
		{
			TCLSubmitRetireMarker(!HAS_FLAG(flags, TCLSubmissionFlag::NO_MARKER_INTERRUPT));
			*timestampValueOut = s_currentRetireMarker; // incremented before each submit
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return 0;
	}

	class : public COSModule
	{
		public:
		std::string_view GetName() override
		{
			return "tcl";
		}

		void RPLMapped() override
		{
			cafeExportRegister("TCL", TCLSubmitToRing, LogType::Placeholder);
			cafeExportRegister("TCL", TCLTimestamp, LogType::Placeholder);
			cafeExportRegister("TCL", TCLWaitTimestamp, LogType::Placeholder);
		};

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
				coreinit::OSInitEvent(s_updateRetirementEvent.GetPtr(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
			}
			else if (reason == coreinit::RplEntryReason::Unloaded)
			{
				s_currentRetireMarker = 0;
				s_tclStatePPC->gpuRetireMarker = 0;
			}
		}
	}s_COStclModule;

	COSModule* GetModule()
	{
		return &s_COStclModule;
	}
}
