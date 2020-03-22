/*
 * Copyright (c) 2014-2015 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Mitch Hayenga
 */

#ifndef __MEM_CACHE_PREFETCH_QUEUED_HH__
#define __MEM_CACHE_PREFETCH_QUEUED_HH__

#include <cstdint>
#include <list>
#include <utility>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/cache/prefetch_filter/pref_info.hh"
#include "mem/packet.hh"

struct QueuedPrefetcherParams;

class QueuedPrefetcher : public BasePrefetcher
{
  public:
    /// 最近三次触发预取的PC地址
    uint64_t recentTriggerPC_[2] {0};

    /// 预取度，由子类初始化
    uint8_t degree_ = 0;

    /// 为了避免降级预取的颠簸，这里会记录最发出的降级预取
    std::list<std::pair<Addr, uint8_t>> recentLevelDownPref_;

    /// 对AddrPriority进行修改，以获得额外的信息，修改不影响原生预取结构
    class AddrPriority {
    public:
        AddrPriority(const Addr & addr, const int32_t& prio) :
                first(addr), second(prio) {}
        
        // 地址
        Addr first;
        // 优先级大小
        int32_t second;
        // 预取相关的信息
        prefetch_filter::PrefetchInfo info_;
    };

  protected:
    struct DeferredPacket {
        /** Prefetch info corresponding to this packet */
        PrefetchInfo pfInfo;
        /** Time when this prefetch becomes ready */
        Tick tick;
        /** The memory packet generated by this prefetch */
        PacketPtr pkt;
        /** The priority of this prefetch */
        int32_t priority;

        /**
         * Constructor
         * @param pfi PrefechInfo object associated to this packet
         * @param t Time when this prefetch becomes ready
         * @param p PacketPtr with the memory request of the prefetch
         * @param prio This prefetch priority
         */
        DeferredPacket(PrefetchInfo const &pfi, Tick t, PacketPtr p,
                       int32_t prio) : pfInfo(pfi), tick(t), pkt(p),
                       priority(prio) {
        }

        bool operator>(const DeferredPacket& that) const
        {
            return priority > that.priority;
        }
        bool operator<(const DeferredPacket& that) const
        {
            return priority < that.priority;
        }
        bool operator<=(const DeferredPacket& that) const
        {
            return !(*this > that);
        }
    };

    std::list<DeferredPacket> pfq;

    // PARAMETERS

    /** Maximum size of the prefetch queue */
    const unsigned queueSize;

    /** Cycles after generation when a prefetch can first be issued */
    const Cycles latency;

    /** Squash queued prefetch if demand access observed */
    const bool queueSquash;

    /** Filter prefetches if already queued */
    const bool queueFilter;

    /** Snoop the cache before generating prefetch (cheating basically) */
    const bool cacheSnoop;

    /** Tag prefetch with PC of generating access? */
    const bool tagPrefetch;

    using const_iterator = std::list<DeferredPacket>::const_iterator;
    const_iterator inPrefetch(const PrefetchInfo &pfi) const;
    using iterator = std::list<DeferredPacket>::iterator;
    iterator inPrefetch(const PrefetchInfo &pfi);

    // STATS
    Stats::Scalar pfIdentified;
    Stats::Scalar pfBufferHit;
    Stats::Scalar pfInCache;
    Stats::Scalar pfRemovedFull;
    Stats::Scalar pfSpanPage;

  public:
    QueuedPrefetcher(const QueuedPrefetcherParams *p);
    virtual ~QueuedPrefetcher();

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void insert(const PacketPtr &pkt, PrefetchInfo &new_pfi, int32_t priority,
            uint8_t targetCacheLevel_ = 255);

    virtual void calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses) = 0;
    PacketPtr getPacket() override;

    Tick nextPrefetchReadyTime() const override
    {
        return pfq.empty() ? MaxTick : pfq.front().tick;
    }

    void regStats() override;
};

#endif //__MEM_CACHE_PREFETCH_QUEUED_HH__

