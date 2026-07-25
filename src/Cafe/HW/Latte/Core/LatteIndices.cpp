#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Common/cpu_features.h"
#include "util/containers/robin_hood.h"

#if defined(ARCH_X86_64) && defined(__GNUC__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

// key used to look up a previously converted index buffer
// two draws share a cache entry only if all fields match exactly (hash collisions are resolved via operator==, see LatteIndexCacheKeyHash)
struct LatteIndexCacheKey
{
	const void* dataPtr;
	uint32 count;
	LattePrimitiveMode primitiveMode;
	LatteIndexType indexType;
	// primitive-restart index register (VGT_MULTI_PRIM_IB_RESET_INDX) value at decode time. The computed indexMax depends on it: any index
	// equal to this value is treated as a restart marker and excluded from the max (see the fallback in LatteIndices_decode), so two draws
	// over the same buffer with different restart indices can produce different indexMax and must not share an entry
	uint32 primitiveRestartIndex;

	bool operator==(const LatteIndexCacheKey& other) const noexcept
	{
		return dataPtr == other.dataPtr && count == other.count && primitiveMode == other.primitiveMode && indexType == other.indexType && primitiveRestartIndex == other.primitiveRestartIndex;
	}
};

struct LatteIndexCacheKeyHash
{
	size_t operator()(const LatteIndexCacheKey& key) const noexcept
	{
		size_t h = robin_hood::hash<const void*>{}(key.dataPtr);
		auto combine = [&h](size_t v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); };
		combine(robin_hood::hash<uint32>{}(key.count));
		combine(robin_hood::hash<uint32>{}(static_cast<uint32>(key.primitiveMode)));
		combine(robin_hood::hash<uint32>{}(static_cast<uint32>(key.indexType)));
		combine(robin_hood::hash<uint32>{}(key.primitiveRestartIndex));
		return h;
	}
};

struct LatteIndexCacheStruct
{
	struct CacheEntry
	{
		// key fields, duplicated here from LatteIndexCacheKey. lastPtr is load-bearing: LatteIndices_invalidate() only sees CacheEntry
		// values (not the map key) and needs it to compute the entry's guest data range. The rest just make an entry self-describing.
		// Exact key matching on lookup (not trusting the hash alone) is handled by the map itself via LatteIndexCacheKey::operator==
		const void* lastPtr;
		uint32 lastCount;
		LattePrimitiveMode lastPrimitiveMode;
		LatteIndexType lastIndexType;
		uint32 lastPrimitiveRestartIndex;
		uint64 lastUsed;
		// byte size of the guest-memory range starting at lastPtr that the conversion read from. Zero for AUTO (no source data), used by LatteIndices_invalidate()
		uint32 sourceDataSize;
		// cheap sampled hash of the source index data, taken right after conversion. Recomputed and compared on every cache hit to catch the guest
		// CPU rewriting index data in place without ever routing through LatteIndices_invalidate() (this is the common case -- see call sites)
		uint64 validationHash;
		// output
		uint32 indexMax;
		Renderer::INDEX_TYPE renderIndexType;
		uint32 outputCount;
		Renderer::IndexAllocation indexAllocation;
	};
	// NOTE: despite the map being able to hold many entries, it does NOT persist across frames in practice. LatteIndices_invalidateAll()
	// wipes it from LatteCP_signalEnterWait() (at least once per frame via the scanbuffer swap, plus on every command-processor starvation)
	// and from VKRMemoryManager::cleanupBuffers() (per retired command buffer), so entries effectively live for one uninterrupted burst of
	// draw commands. It still beats the old fixed 8-slot array because within a single burst hundreds of distinct meshes (e.g. BotW) no
	// longer evict each other. Removing those invalidateAll() flushes to unlock true cross-frame persistence must NOT be done without first
	// closing the validation-hash sampling gaps (see LatteIndices_decode) -- once the flushes are gone the hash is the only staleness defense
	robin_hood::unordered_flat_map<LatteIndexCacheKey, CacheEntry, LatteIndexCacheKeyHash> entries;
	uint64 currentUsageCounter{0};
} LatteIndexCache{};

// cap on the number of entries kept alive at once, to bound memory/allocation usage. Only checked on insert (i.e. only on a cache miss),
// so it does not add any per-draw overhead on the (hopefully common) cache hit path
constexpr size_t LATTE_INDEX_CACHE_MAX_ENTRIES = 2048;
// on a cap-triggered sweep, entries used more recently than (currentUsageCounter - keepWindow) survive the first pass
constexpr uint64 LATTE_INDEX_CACHE_KEEP_WINDOW = 1024;

uint64 LatteIndices_GetNextUsageIndex()
{
	return LatteIndexCache.currentUsageCounter++;
}

// computes the byte size of the source (guest) index data that a conversion reads. AUTO indices are generated without reading any source
// data, so they have no range to invalidate and are not eligible for the staleness validation hash either
uint32 LatteIndices_calculateSourceDataSize(LatteIndexType indexType, uint32 count)
{
	if (indexType == LatteIndexType::AUTO)
		return 0;
	if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		return count * sizeof(uint16);
	if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
		return count * sizeof(uint32);
	cemu_assert_suspicious();
	return 0;
}

// cheap sampled hash of the source index data, in the spirit of LatteTexture_CalculateTextureDataHash() in LatteTextureCache.cpp: instead of
// hashing the full (potentially huge) index buffer we only sample the first and last 64 bytes plus a 64 byte block every 2048 bytes. This is
// far cheaper than a full reconversion while still catching the vast majority of in-place edits a game would realistically perform
uint64 LatteIndices_calculateValidationHash(const void* data, uint32 sizeInBytes)
{
	if (sizeInBytes == 0)
		return 0;
	const uint8* base = (const uint8*)data;
	uint64 hashVal = 0;
	auto foldBlock = [&hashVal](const uint8* blockPtr, uint32 blockSize)
	{
		uint32 numQwords = blockSize / sizeof(uint64);
		for (uint32 i = 0; i < numQwords; i++)
		{
			uint64 v;
			memcpy(&v, blockPtr, sizeof(uint64)); // avoid unaligned-access UB, the guest pointer has no alignment guarantee
			hashVal ^= v;
			hashVal = std::rotl<uint64>(hashVal, 5) + 0x9E3779B97F4A7C15ULL;
			blockPtr += sizeof(uint64);
		}
	};
	uint32 headSize = std::min<uint32>(sizeInBytes, 64);
	foldBlock(base, headSize);
	if (sizeInBytes > 64)
	{
		uint32 tailSize = std::min<uint32>(sizeInBytes, 64);
		foldBlock(base + sizeInBytes - tailSize, tailSize);
	}
	for (uint32 offset = 2048; offset + 64 <= sizeInBytes; offset += 2048)
		foldBlock(base + offset, 64);
	return hashVal;
}

// releases the renderer-side allocation of every entry matching the predicate and erases them from the map
template<typename TPredicate>
static void LatteIndexCache_EvictIf(TPredicate&& predicate)
{
	for (auto it = LatteIndexCache.entries.begin(); it != LatteIndexCache.entries.end();)
	{
		if (predicate(it->second))
		{
			g_renderer->indexData_releaseIndexMemory(it->second.indexAllocation);
			it = LatteIndexCache.entries.erase(it);
		}
		else
			++it;
	}
}

// invoked only right before inserting a new entry (i.e. only on a cache miss), never on the hit path
static void LatteIndexCache_EnforceCapacity()
{
	if (LatteIndexCache.entries.size() < LATTE_INDEX_CACHE_MAX_ENTRIES)
		return;
	uint64 currentUsage = LatteIndexCache.currentUsageCounter;
	uint64 cutoff = (currentUsage > LATTE_INDEX_CACHE_KEEP_WINDOW) ? (currentUsage - LATTE_INDEX_CACHE_KEEP_WINDOW) : 0;
	LatteIndexCache_EvictIf([cutoff](const auto& entry) { return entry.lastUsed < cutoff; });
	if (LatteIndexCache.entries.size() < LATTE_INDEX_CACHE_MAX_ENTRIES)
		return;
	// keep-window sweep didn't free anything (cache filled up with very recent entries within a short burst) -- fall back to trimming
	// everything at or below the median usage index once, which guarantees forward progress
	std::vector<uint64> ages;
	ages.reserve(LatteIndexCache.entries.size());
	for (auto& it : LatteIndexCache.entries)
		ages.push_back(it.second.lastUsed);
	size_t medianPos = ages.size() / 2;
	std::nth_element(ages.begin(), ages.begin() + medianPos, ages.end());
	uint64 medianUsage = ages[medianPos];
	LatteIndexCache_EvictIf([medianUsage](const auto& entry) { return entry.lastUsed <= medianUsage; });
}

void LatteIndices_invalidate(const void* memPtr, uint32 size)
{
	const uint8* rangeBegin = (const uint8*)memPtr;
	const uint8* rangeEnd = rangeBegin + size;
	LatteIndexCache_EvictIf([rangeBegin, rangeEnd](const auto& entry)
	{
		if (entry.sourceDataSize == 0) // AUTO entries have no source data, nothing to invalidate
			return false;
		const uint8* entryBegin = (const uint8*)entry.lastPtr;
		const uint8* entryEnd = entryBegin + entry.sourceDataSize;
		return entryBegin < rangeEnd && rangeBegin < entryEnd;
	});
}

void LatteIndices_invalidateAll()
{
	LatteIndexCache_EvictIf([](const auto&) { return true; });
}

uint32 LatteIndices_calculateIndexOutputSize(LattePrimitiveMode primitiveMode, LatteIndexType indexType, uint32 count)
{
	if (primitiveMode == LattePrimitiveMode::QUADS)
	{
		sint32 numQuads = count / 4;
		if (indexType == LatteIndexType::AUTO)
		{
			if(count <= 0xFFFF)
				return numQuads * 6 * sizeof(uint16);
			return numQuads * 6 * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return numQuads * 6 * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return numQuads * 6 * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if (primitiveMode == LattePrimitiveMode::QUAD_STRIP)
	{
		if (count <= 3)
		{
			return 0;
		}
		sint32 numQuads = (count-2) / 2;
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
				return numQuads * 6 * sizeof(uint16);
			return numQuads * 6 * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return numQuads * 6 * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return numQuads * 6 * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if (primitiveMode == LattePrimitiveMode::LINE_LOOP)
	{
		count++; // one extra vertex to reconnect the LINE_STRIP to the beginning
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
				return count * sizeof(uint16);
			return count * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return count * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return count * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN && g_renderer->GetType() == RendererAPI::Metal)
	{
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
				return count * sizeof(uint16);
			return count * sizeof(uint32);
		}
		if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
			return count * sizeof(uint16);
		if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
			return count * sizeof(uint32);
		cemu_assert_suspicious();
		return 0;
	}
	else if(indexType == LatteIndexType::AUTO)
		return 0;
	else if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		return count * sizeof(uint16);
	else if (indexType == LatteIndexType::U32_BE || indexType == LatteIndexType::U32_LE)
		return count * sizeof(uint32);
	return 0;
}

template<typename T>
void LatteIndices_convertBE(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (uint32 i = 0; i < count; i++)
	{
		T v = *src;
		*dst = v;
		indexMax = std::max(indexMax, (uint32)v);
		dst++;
		src++;
	}
}

template<typename T>
void LatteIndices_convertLE(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const T* src = (T*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (uint32 i = 0; i < count; i++)
	{
		T v = *src;
		*dst = v;
		indexMax = std::max(indexMax, (uint32)v);
		dst++;
		src++;
	}
}

template<typename T>
void LatteIndices_unpackQuadsAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	sint32 numQuads = count / 4;
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = src[0];
		T idx1 = src[1];
		T idx2 = src[2];
		T idx3 = src[3];
		indexMax = std::max(indexMax, (uint32)idx0);
		indexMax = std::max(indexMax, (uint32)idx1);
		indexMax = std::max(indexMax, (uint32)idx2);
		indexMax = std::max(indexMax, (uint32)idx3);
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx0;
		dst[4] = idx2;
		dst[5] = idx3;
		src += 4;
		dst += 6;
	}
}

template<typename T>
void LatteIndices_generateAutoQuadIndices(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	sint32 numQuads = count / 4;
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = i * 4 + 0;
		T idx1 = i * 4 + 1;
		T idx2 = i * 4 + 2;
		T idx3 = i * 4 + 3;
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx0;
		dst[4] = idx2;
		dst[5] = idx3;
		src += 4;
		dst += 6;
	}
	indexMax = std::max(count, 1u) - 1;
}

template<typename T>
void LatteIndices_unpackQuadStripAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count <= 3)
		return;
	sint32 numQuads = (count - 2) / 2;
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = src[0];
		T idx1 = src[1];
		T idx2 = src[2];
		T idx3 = src[3];
		indexMax = std::max(indexMax, (uint32)idx0);
		indexMax = std::max(indexMax, (uint32)idx1);
		indexMax = std::max(indexMax, (uint32)idx2);
		indexMax = std::max(indexMax, (uint32)idx3);
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx2;
		dst[4] = idx1;
		dst[5] = idx3;
		src += 2;
		dst += 6;
	}
}

template<typename T>
void LatteIndices_unpackLineLoopAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count <= 0)
		return;
	const betype<T>* src = (betype<T>*)indexDataInput;
	T firstIndex = *src;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < (sint32)count; i++)
	{
		T idx = *src;
		indexMax = std::max(indexMax, (uint32)idx);
		*dst = idx;
		src++;
		dst++;
	}
	*dst = firstIndex;
}

template<typename T>
void LatteIndices_generateAutoQuadStripIndices(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count <= 3)
		return;
	sint32 numQuads = (count - 2) / 2;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < numQuads; i++)
	{
		T idx0 = i * 2 + 0;
		T idx1 = i * 2 + 1;
		T idx2 = i * 2 + 2;
		T idx3 = i * 2 + 3;
		dst[0] = idx0;
		dst[1] = idx1;
		dst[2] = idx2;
		dst[3] = idx2;
		dst[4] = idx1;
		dst[5] = idx3;
		dst += 6;
	}
	indexMax = std::max(count, 1u) - 1;
}


template<typename T>
void LatteIndices_generateAutoLineLoopIndices(void* indexDataOutput, uint32 count, uint32& indexMax)
{
	if (count == 0)
		return;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < (sint32)count; i++)
	{
		*dst = (T)i;
		dst++;
	}
	*dst = 0;
	dst++;
	indexMax = std::max(count, 1u) - 1;
}

template<typename T>
void LatteIndices_unpackTriangleFanAndConvert(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	// TODO: check this
	for (sint32 i = 0; i < count; i++)
	{
	    uint32 i0;
		if (i % 2 == 0)
		    i0 = i / 2;
        else
            i0 = count - 1 - i / 2;
        T idx = src[i0];
		indexMax = std::max(indexMax, (uint32)idx);
		dst[i] = idx;
	}
}

template<typename T>
void LatteIndices_generateAutoTriangleFanIndices(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const betype<T>* src = (betype<T>*)indexDataInput;
	T* dst = (T*)indexDataOutput;
	for (sint32 i = 0; i < count; i++)
	{
		T idx = i;
		if (idx % 2 == 0)
            idx = idx / 2;
        else
            idx = count - 1 - idx / 2;
		dst[i] = idx;
	}
	indexMax = std::max(count, 1u) - 1;
}

#if defined(ARCH_X86_64)
ATTRIBUTE_AVX2
void LatteIndices_fastConvertU16_AVX2(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// using AVX + AVX2 we can process 16 indices at a time
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count16 = count >> 4;
	sint32 countRemaining = count & 15;
	if (count16)
	{
		__m256i mMin = _mm256_set_epi16((sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF,
						           		(sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF, (sint16)0xFFFF);
		__m256i mMax = _mm256_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000);
		__m256i mShuffle16Swap = _mm256_set_epi8(30, 31, 28, 29, 26, 27, 24, 25, 22, 23, 20, 21, 18, 19, 16, 17, 14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);

		do
		{
			__m256i mIndexData = _mm256_loadu_si256((const __m256i*)indicesU16BE);
			indicesU16BE += 16;
			_mm_prefetch((const char*)indicesU16BE, _MM_HINT_T0);
			// endian swap
			mIndexData = _mm256_shuffle_epi8(mIndexData, mShuffle16Swap);
			_mm256_store_si256((__m256i*)indexOutput, mIndexData);
			mMax = _mm256_max_epu16(mIndexData, mMax);
			indexOutput += 16;
		} while (--count16);

		// fold 32 to 16 byte
		mMax = _mm256_max_epu16(mMax, _mm256_permute2x128_si256(mMax, mMax, 1));
		// fold 16 to 8 byte
		mMax = _mm256_max_epu16(mMax, _mm256_shuffle_epi32(mMax, (2 << 0) | (3 << 2) | (2 << 4) | (3 << 6)));

		uint16* mMaxU16 = (uint16*)&mMax;

		indexMax = std::max(indexMax, (uint32)mMaxU16[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[1]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[2]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[3]);
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update max
	indexMax = std::max(indexMax, _maxIndex);
}

ATTRIBUTE_SSE41
void LatteIndices_fastConvertU16_SSE41(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// SSSE3 & SSE4.1 optimized decoding
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;
	if (count8)
	{
		__m128i mMax = _mm_set_epi16(0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000);
		__m128i mTemp;
		__m128i* mRawIndices = (__m128i*)indicesU16BE;
		indicesU16BE += count8 * 8;
		__m128i* mOutputIndices = (__m128i*)indexOutput;
		indexOutput += count8 * 8;
		__m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
		while (count8--)
		{
			mTemp = _mm_loadu_si128(mRawIndices);
			mRawIndices++;
			mTemp = _mm_shuffle_epi8(mTemp, shufmask);
			mMax = _mm_max_epu16(mMax, mTemp);
			_mm_store_si128(mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint16* mMaxU16 = (uint16*)&mMax;

		indexMax = std::max(indexMax, (uint32)mMaxU16[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[1]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[2]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[3]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[4]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[5]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[6]);
		indexMax = std::max(indexMax, (uint32)mMaxU16[7]);
	}
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	indexMax = std::max(indexMax, _maxIndex);
}

ATTRIBUTE_AVX2
void LatteIndices_fastConvertU32_AVX2(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	// using AVX + AVX2 we can process 8 indices at a time
	const uint32* indicesU32BE = (const uint32*)indexDataInput;
	uint32* indexOutput = (uint32*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;
	if (count8)
	{
		__m256i mMax = _mm256_set_epi32(0, 0, 0, 0, 0, 0, 0, 0);
		__m256i mShuffle32Swap = _mm256_set_epi8(28,29,30,31,
			24,25,26,27,
			20,21,22,23,
			16,17,18,19,
			12,13,14,15,
			8,9,10,11,
			4,5,6,7,
			0,1,2,3);
		// unaligned
		do
		{
			__m256i mIndexData = _mm256_loadu_si256((const __m256i*)indicesU32BE);
			indicesU32BE += 8;
			_mm_prefetch((const char*)indicesU32BE, _MM_HINT_T0);
			// endian swap
			mIndexData = _mm256_shuffle_epi8(mIndexData, mShuffle32Swap);
			_mm256_store_si256((__m256i*)indexOutput, mIndexData);
			mMax = _mm256_max_epu32(mIndexData, mMax);
			indexOutput += 8;
		} while (--count8);

		// fold 32 to 16 byte
		mMax = _mm256_max_epu32(mMax, _mm256_permute2x128_si256(mMax, mMax, 1));
		// fold 16 to 8 byte
		mMax = _mm256_max_epu32(mMax, _mm256_shuffle_epi32(mMax, (2 << 0) | (3 << 2) | (2 << 4) | (3 << 6)));

		uint32* mMaxU32 = (uint32*)&mMax;
		indexMax = std::max(indexMax, (uint32)mMaxU32[0]);
		indexMax = std::max(indexMax, (uint32)mMaxU32[1]);
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint32 idx = _swapEndianU32(*indicesU32BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU32BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}
#elif defined(__aarch64__)

void LatteIndices_fastConvertU16_NEON(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const uint16* indicesU16BE = (const uint16*)indexDataInput;
	uint16* indexOutput = (uint16*)indexDataOutput;
	sint32 count8 = count >> 3;
	sint32 countRemaining = count & 7;

	if (count8)
	{
		uint16x8_t mMax = vdupq_n_u16(0x0000);
		uint16x8_t mTemp;
		uint16x8_t* mRawIndices = (uint16x8_t*) indicesU16BE;
		indicesU16BE += count8 * 8;
		uint16x8_t* mOutputIndices = (uint16x8_t*) indexOutput;
		indexOutput += count8 * 8;

		while (count8--)
		{
			mTemp = vld1q_u16((uint16*)mRawIndices);
			mRawIndices++;
			mTemp = vrev16q_u8(mTemp);
			mMax = vmaxq_u16(mMax, mTemp);
			vst1q_u16((uint16*)mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint16* mMaxU16 = (uint16*)&mMax;

		for (int i = 0; i < 8; ++i) {
			indexMax = std::max(indexMax, (uint32)mMaxU16[i]);
		}
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint16 idx = _swapEndianU16(*indicesU16BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU16BE++;
		_maxIndex = std::max(_maxIndex, (uint32)idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}

void LatteIndices_fastConvertU32_NEON(const void* indexDataInput, void* indexDataOutput, uint32 count, uint32& indexMax)
{
	const uint32* indicesU32BE = (const uint32*)indexDataInput;
	uint32* indexOutput = (uint32*)indexDataOutput;
	sint32 count8 = count >> 2;
	sint32 countRemaining = count & 3;

	if (count8)
	{
		uint32x4_t mMax = vdupq_n_u32(0x00000000);
		uint32x4_t mTemp;
		uint32x4_t* mRawIndices = (uint32x4_t*) indicesU32BE;
		indicesU32BE += count8 * 4;
		uint32x4_t* mOutputIndices = (uint32x4_t*) indexOutput;
		indexOutput += count8 * 4;

		while (count8--)
		{
			mTemp = vld1q_u32((uint32*)mRawIndices);
			mRawIndices++;
			mTemp = vrev32q_u8(mTemp);
			mMax = vmaxq_u32(mMax, mTemp);
			vst1q_u32((uint32*)mOutputIndices, mTemp);
			mOutputIndices++;
		}

		uint32* mMaxU32 = (uint32*)&mMax;

		for (int i = 0; i < 4; ++i) {
			indexMax = std::max(indexMax, mMaxU32[i]);
		}
	}
	// process remaining indices
	uint32 _maxIndex = 0;
	for (sint32 i = countRemaining; (--i) >= 0;)
	{
		uint32 idx = _swapEndianU32(*indicesU32BE);
		*indexOutput = idx;
		indexOutput++;
		indicesU32BE++;
		_maxIndex = std::max(_maxIndex, idx);
	}
	// update min/max
	indexMax = std::max(indexMax, _maxIndex);
}

#endif

template<typename T>
void _LatteIndices_alternativeCalculateIndexMax(const void* indexData, uint32 count, uint32 primitiveRestartIndex, uint32& indexMax)
{
	cemu_assert_debug(count != 0);
	const betype<T>* idxPtrT = (betype<T>*)indexData;
	T _indexMax = *idxPtrT;
	cemu_assert_debug(primitiveRestartIndex <= std::numeric_limits<T>::max());
	T restartIndexT = (T)primitiveRestartIndex;
	while (count)
	{
		T idx = *idxPtrT;
		if (idx != restartIndexT)
		{
			_indexMax = std::max(_indexMax, idx);
		}
		idxPtrT++;
		count--;
	}
	indexMax = _indexMax;
}

// calculate min and max index while taking primitive restart into account
// fallback implementation in case the fast path gives us invalid results
void LatteIndices_alternativeCalculateIndexMax(const void* indexData, LatteIndexType indexType, uint32 count, uint32& indexMax)
{
	if (count == 0)
	{
		indexMax = 0;
		return;
	}
	uint32 primitiveRestartIndex = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();

	if (indexType == LatteIndexType::U16_BE)
	{
		_LatteIndices_alternativeCalculateIndexMax<uint16>(indexData, count, primitiveRestartIndex, indexMax);
	}
	else if (indexType == LatteIndexType::U32_BE)
	{
		_LatteIndices_alternativeCalculateIndexMax<uint32>(indexData, count, primitiveRestartIndex, indexMax);
	}
	else
	{
		cemu_assert_debug(false);
	}
}

void LatteIndices_decode(const void* indexData, LatteIndexType indexType, uint32 count, LattePrimitiveMode primitiveMode, uint32& indexMax, Renderer::INDEX_TYPE& renderIndexType, uint32& outputCount, Renderer::IndexAllocation& indexAllocation)
{
	LATTE_PERF_SCOPE(tmrIndexDecode);
	// what this should do:
	// [x] use fast SIMD-based index decoding
	// [x] unpack QUAD indices to triangle indices
	// [x] calculate min and max index, be careful about primitive restart index
	// [x] decode data directly into coherent memory buffer?
	// [x] better cache implementation (keyed map instead of the old 8-slot array; note: the per-frame invalidateAll() flushes currently
	//     limit reuse to within a single draw burst, not truly across frames -- see the note on LatteIndexCacheStruct::entries)

	// reuse from cache if data didn't change. This only hits within a single uninterrupted burst of draws -- the map is flushed several
	// times per frame by LatteIndices_invalidateAll() (see the note on LatteIndexCacheStruct::entries) -- but within a burst it beats the
	// old fixed 8-slot LRU array, which almost never held the right entry once more than 8 distinct meshes were drawn between flushes
	uint32 sourceDataSize = LatteIndices_calculateSourceDataSize(indexType, count);
	// read up front because it participates in the cache key (the restart index feeds into indexMax, see the fallback further down)
	uint32 primitiveRestartIndex = LatteGPUState.contextNew.VGT_MULTI_PRIM_IB_RESET_INDX.get_RESTART_INDEX();
	LatteIndexCacheKey lookupKey{indexData, count, primitiveMode, indexType, primitiveRestartIndex};
	auto cacheIt = LatteIndexCache.entries.find(lookupKey);
	if (cacheIt != LatteIndexCache.entries.end())
	{
		auto& cacheEntry = cacheIt->second;
		// validate that the guest CPU hasn't rewritten the source data in place since the entry was cached without routing through
		// LatteIndices_invalidate(). While the invalidateAll() flushes above remain, this sampled hash is only a secondary defense that
		// catches in-place rewrites within a burst; its sampling (64B head/tail + one 64B block per 2048B) has blind spots of up to
		// ~1984 bytes, acceptable only because the per-frame flushes bound how long a stale entry can survive
		bool stillValid = (sourceDataSize == 0) || (LatteIndices_calculateValidationHash(indexData, sourceDataSize) == cacheEntry.validationHash);
		if (stillValid)
		{
			LATTE_PERF_COUNT(cntIndexCacheHit);
			indexMax = cacheEntry.indexMax;
			renderIndexType = cacheEntry.renderIndexType;
			outputCount = cacheEntry.outputCount;
			indexAllocation = cacheEntry.indexAllocation;
			cacheEntry.lastUsed = LatteIndices_GetNextUsageIndex();
			return;
		}
		// stale entry: release its allocation now and fall through to reconvert. Counts as a miss like any other non-hit
		g_renderer->indexData_releaseIndexMemory(cacheEntry.indexAllocation);
		LatteIndexCache.entries.erase(cacheIt);
	}
	LATTE_PERF_COUNT(cntIndexCacheMiss);

	outputCount = 0;
	if (indexType == LatteIndexType::AUTO)
		renderIndexType = Renderer::INDEX_TYPE::NONE;
	else if (indexType == LatteIndexType::U16_BE || indexType == LatteIndexType::U16_LE)
		renderIndexType = Renderer::INDEX_TYPE::U16;
	else if (indexType == LatteIndexType::U32_BE)
		renderIndexType = Renderer::INDEX_TYPE::U32;
	else
		cemu_assert_debug(false);

	// calculate index output size
	uint32 indexOutputSize = LatteIndices_calculateIndexOutputSize(primitiveMode, indexType, count);
	if (indexOutputSize == 0)
	{
		outputCount = count;
		indexMax = std::max(count, 1u)-1;
		renderIndexType = Renderer::INDEX_TYPE::NONE;
		indexAllocation = {};
		return; // no indices
	}
	// query index buffer from renderer
	indexAllocation = g_renderer->indexData_reserveIndexMemory(indexOutputSize);
	void* indexOutputPtr = indexAllocation.mem;

	// decode indices
	indexMax = std::numeric_limits<uint32>::min();
	if (primitiveMode == LattePrimitiveMode::QUADS)
	{
		// unpack quads into triangles
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoQuadIndices<uint16>(indexData, indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoQuadIndices<uint32>(indexData, indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
		}
		else if (indexType == LatteIndexType::U16_BE)
			LatteIndices_unpackQuadsAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			LatteIndices_unpackQuadsAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
		else
			cemu_assert_debug(false);
		outputCount = count / 4 * 6;
	}
	else if (primitiveMode == LattePrimitiveMode::QUAD_STRIP)
	{
		// unpack quad strip into triangles
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoQuadStripIndices<uint16>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoQuadStripIndices<uint32>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
		}
		else if (indexType == LatteIndexType::U16_BE)
			LatteIndices_unpackQuadStripAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			LatteIndices_unpackQuadStripAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
		else
			cemu_assert_debug(false);
		if (count >= 2)
			outputCount = (count - 2) / 2 * 6;
		else
			outputCount = 0;
	}
	else if (primitiveMode == LattePrimitiveMode::LINE_LOOP)
	{
		// unpack line loop into line strip with extra reconnecting vertex
		if (indexType == LatteIndexType::AUTO)
		{
			if (count <= 0xFFFF)
			{
				LatteIndices_generateAutoLineLoopIndices<uint16>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U16;
			}
			else
			{
				LatteIndices_generateAutoLineLoopIndices<uint32>(indexOutputPtr, count, indexMax);
				renderIndexType = Renderer::INDEX_TYPE::U32;
			}
		}
		else if (indexType == LatteIndexType::U16_BE)
			LatteIndices_unpackLineLoopAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
		else if (indexType == LatteIndexType::U32_BE)
			LatteIndices_unpackLineLoopAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
		else
			cemu_assert_debug(false);
		outputCount = count + 1;
	}
	else if (primitiveMode == LattePrimitiveMode::TRIANGLE_FAN && g_renderer->GetType() == RendererAPI::Metal)
	{
        if (indexType == LatteIndexType::AUTO)
    	{
    		if (count <= 0xFFFF)
    		{
    			LatteIndices_generateAutoTriangleFanIndices<uint16>(indexData, indexOutputPtr, count, indexMax);
    			renderIndexType = Renderer::INDEX_TYPE::U16;
    		}
    		else
    		{
    			LatteIndices_generateAutoTriangleFanIndices<uint32>(indexData, indexOutputPtr, count, indexMax);
    			renderIndexType = Renderer::INDEX_TYPE::U32;
    		}
    	}
    	else if (indexType == LatteIndexType::U16_BE)
    		LatteIndices_unpackTriangleFanAndConvert<uint16>(indexData, indexOutputPtr, count, indexMax);
    	else if (indexType == LatteIndexType::U32_BE)
    		LatteIndices_unpackTriangleFanAndConvert<uint32>(indexData, indexOutputPtr, count, indexMax);
    	else
    		cemu_assert_debug(false);
    	outputCount = count;
	}
	else
	{
		if (indexType == LatteIndexType::U16_BE)
		{
#if defined(ARCH_X86_64)
			if (g_CPUFeatures.x86.avx2)
				LatteIndices_fastConvertU16_AVX2(indexData, indexOutputPtr, count, indexMax);
			else if (g_CPUFeatures.x86.sse4_1 && g_CPUFeatures.x86.ssse3)
				LatteIndices_fastConvertU16_SSE41(indexData, indexOutputPtr, count, indexMax);
			else
				LatteIndices_convertBE<uint16>(indexData, indexOutputPtr, count, indexMax);
#elif defined(__aarch64__)
			LatteIndices_fastConvertU16_NEON(indexData, indexOutputPtr, count, indexMax);
#else
			LatteIndices_convertBE<uint16>(indexData, indexOutputPtr, count, indexMax);
#endif
		}
		else if (indexType == LatteIndexType::U32_BE)
		{
#if defined(ARCH_X86_64)
			if (g_CPUFeatures.x86.avx2)
				LatteIndices_fastConvertU32_AVX2(indexData, indexOutputPtr, count, indexMax);
			else
				LatteIndices_convertBE<uint32>(indexData, indexOutputPtr, count, indexMax);
#elif defined(__aarch64__)
			LatteIndices_fastConvertU32_NEON(indexData, indexOutputPtr, count, indexMax);
#else
			LatteIndices_convertBE<uint32>(indexData, indexOutputPtr, count, indexMax);
#endif
		}
		else if (indexType == LatteIndexType::U16_LE)
		{
			LatteIndices_convertLE<uint16>(indexData, indexOutputPtr, count, indexMax);
		}
		else if (indexType == LatteIndexType::U32_LE)
		{
			LatteIndices_convertLE<uint32>(indexData, indexOutputPtr, count, indexMax);
		}
		else
			cemu_assert_debug(false);
		outputCount = count;
	}
	// the above algorithms use a fast approach to get indexMax which does not filter out indices matching primitiveRestartIndex
	// here we use a fallback in case the determined index equals the primitive restart index
	if (primitiveRestartIndex == indexMax)
	{
		// recalculate index range but filter out primitive restart index
		LatteIndices_alternativeCalculateIndexMax(indexData, indexType, count, indexMax);
	}
	g_renderer->indexData_uploadIndexMemory(indexAllocation);
	LATTE_PERF_ADD(cntBytesIndexUpload, indexOutputSize);
	performanceMonitor.cycle[performanceMonitor.cycleIndex].indexDataUploaded += indexOutputSize;
	// make room if the cache has grown past its cap. Only happens here, on a miss that is about to insert a new entry, so it never
	// adds overhead to the cache hit path
	LatteIndexCache_EnforceCapacity();
	// insert new cache entry (this also correctly replaces the stale entry erased earlier in the validation-hash-mismatch case)
	LatteIndexCacheStruct::CacheEntry newEntry{};
	newEntry.lastPtr = indexData;
	newEntry.lastCount = count;
	newEntry.lastPrimitiveMode = primitiveMode;
	newEntry.lastIndexType = indexType;
	newEntry.lastPrimitiveRestartIndex = primitiveRestartIndex;
	newEntry.sourceDataSize = sourceDataSize;
	newEntry.validationHash = (sourceDataSize != 0) ? LatteIndices_calculateValidationHash(indexData, sourceDataSize) : 0;
	newEntry.indexMax = indexMax;
	newEntry.renderIndexType = renderIndexType;
	newEntry.outputCount = outputCount;
	newEntry.indexAllocation = indexAllocation;
	newEntry.lastUsed = LatteIndices_GetNextUsageIndex();
	LatteIndexCache.entries.insert_or_assign(lookupKey, std::move(newEntry));
}
