
#include <winstd.H>

#include <BoxArray.H>
#include <DistributionMapping.H>
#include <ParallelDescriptor.H>
#include <ParmParse.H>
#include <BLProfiler.H>
#include <FArrayBox.H>
#include <Geometry.H>
#include <VisMF.H>
#include <Utility.H>

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <list>
#include <map>
#include <vector>
#include <queue>
#include <algorithm>
#include <numeric>
#include <string>
#include <cstring>
#include <iomanip>
using std::string;

namespace
{
    bool initialized = false;
    std::map<int, int> rankPNumMap;       // [rank, procNumber]
    std::multimap<int, int> pNumRankMM;   // [procNumber, rank]
    std::map<int, IntVect> pNumTopIVMap;  // [procNumber, topological iv position]
    std::multimap<IntVect, int, IntVect::Compare> topIVpNumMM;
                                          // [topological iv position, procNumber]
    std::vector<int> ranksSFC;
}

namespace
{
    //
    // Set default values for these in Initialize()!!!
    //
    bool   verbose;
    int    sfc_threshold;
    Real   max_efficiency;
}

// We default to SFC.
DistributionMapping::Strategy DistributionMapping::m_Strategy = DistributionMapping::SFC;

DistributionMapping::PVMF DistributionMapping::m_BuildMap = 0;

long DistributionMapping::totalCells(0);
Real DistributionMapping::bytesPerCell(0.0);
Array<int> DistributionMapping::proximityMap;
Array<int> DistributionMapping::proximityOrder;
Array<long> DistributionMapping::totalBoxPoints;

int DistributionMapping::nDistMaps(0);


const Array<int>&
DistributionMapping::ProcessorMap () const
{
    return m_ref->m_pmap;
}

DistributionMapping::Strategy
DistributionMapping::strategy ()
{
    return DistributionMapping::m_Strategy;
}

int
DistributionMapping::CacheSize ()
{
    return m_Cache.size();
}

void
DistributionMapping::strategy (DistributionMapping::Strategy how)
{
    DistributionMapping::m_Strategy = how;

    switch (how)
    {
    case ROUNDROBIN:
        m_BuildMap = &DistributionMapping::RoundRobinProcessorMap;
        break;
    case KNAPSACK:
        m_BuildMap = &DistributionMapping::KnapSackProcessorMap;
        break;
    case SFC:
        m_BuildMap = &DistributionMapping::SFCProcessorMap;
        break;
    case PFC:
        m_BuildMap = &DistributionMapping::PFCProcessorMap;
        break;
    case RRSFC:
        m_BuildMap = &DistributionMapping::RRSFCProcessorMap;
        break;
    default:
        BoxLib::Error("Bad DistributionMapping::Strategy");
    }
}

void
DistributionMapping::SFC_Threshold (int n)
{
    sfc_threshold = std::min(n,1);
}

int
DistributionMapping::SFC_Threshold ()
{
    return sfc_threshold;
}

bool
DistributionMapping::operator== (const DistributionMapping& rhs) const
{
    return m_ref == rhs.m_ref || m_ref->m_pmap == rhs.m_ref->m_pmap;
}

bool
DistributionMapping::operator!= (const DistributionMapping& rhs) const
{
    return !operator==(rhs);
}

void
DistributionMapping::Initialize ()
{
    if (initialized) return;
    //
    // Set defaults here!!!
    //
    verbose          = false;
    sfc_threshold    = 0;
    max_efficiency   = 0.9;

    ParmParse pp("DistributionMapping");

    pp.query("v"      ,          verbose);
    pp.query("verbose",          verbose);
    pp.query("efficiency",       max_efficiency);
    pp.query("sfc_threshold",    sfc_threshold);

    std::string theStrategy;

    if (pp.query("strategy", theStrategy))
    {
        if (theStrategy == "ROUNDROBIN")
        {
            strategy(ROUNDROBIN);
        }
        else if (theStrategy == "KNAPSACK")
        {
            strategy(KNAPSACK);
        }
        else if (theStrategy == "SFC")
        {
            strategy(SFC);
        }
        else if (theStrategy == "PFC")
        {
            strategy(PFC);
	    DistributionMapping::InitProximityMap();
        }
        else if (theStrategy == "RRSFC")
        {
            strategy(RRSFC);
        }
        else
        {
            std::string msg("Unknown strategy: ");
            msg += theStrategy;
            BoxLib::Warning(msg.c_str());
        }
    }
    else
    {
        strategy(m_Strategy);  // default
    }

    if(proximityMap.size() != ParallelDescriptor::NProcs()) {
      //std::cout << "#00#::Initialize: proximityMap not resized yet." << std::endl;
      proximityMap.resize(ParallelDescriptor::NProcs(), 0);
      proximityOrder.resize(ParallelDescriptor::NProcs(), 0);
    }
    totalBoxPoints.resize(ParallelDescriptor::NProcs(), 0);

    DistributionMapping::nDistMaps = 0;

    BoxLib::ExecOnFinalize(DistributionMapping::Finalize);

    initialized = true;
}

void
DistributionMapping::Finalize ()
{
    initialized = false;

    DistributionMapping::FlushCache();

    DistributionMapping::m_BuildMap = 0;

    DistributionMapping::m_Cache.clear();
}

//
// Our cache of processor maps.
//
std::map< int,LnClassPtr<DistributionMapping::Ref> > DistributionMapping::m_Cache;

void
DistributionMapping::Sort (std::vector<LIpair>& vec,
                           bool                 reverse)
{
    if (vec.size() > 1)
    {
	if (reverse) {
	    std::stable_sort(vec.begin(), vec.end(), LIpairGT());
	}
	else {
	    std::stable_sort(vec.begin(), vec.end(), LIpairLT());
	}
    }
}

void
DistributionMapping::LeastUsedCPUs (int         nprocs,
                                    Array<int>& result)
{
    result.resize(nprocs);

#ifdef BL_USE_MPI
    BL_PROFILE("DistributionMapping::LeastUsedCPUs()");

    Array<long> bytes(nprocs);
    long thisbyte = BoxLib::TotalBytesAllocatedInFabs()/1024;

    BL_COMM_PROFILE(BLProfiler::Allgather, sizeof(long), BLProfiler::BeforeCall(),
                    BLProfiler::NoTag());
    MPI_Allgather(&thisbyte,
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  bytes.dataPtr(),
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  ParallelDescriptor::Communicator());
    BL_COMM_PROFILE(BLProfiler::Allgather, sizeof(long), BLProfiler::AfterCall(),
                    BLProfiler::NoTag());

    std::vector<LIpair> LIpairV;

    LIpairV.reserve(nprocs);

    for (int i(0); i < nprocs; ++i)
    {
        LIpairV.push_back(LIpair(bytes[i],i));
    }

    bytes.clear();

    Sort(LIpairV, false);

    for (int i(0); i < nprocs; ++i)
    {
        result[i] = LIpairV[i].second;
    }
#else
    for (int i(0); i < nprocs; ++i)
    {
        result[i] = i;
    }
#endif
}

bool
DistributionMapping::GetMap (const BoxArray& boxes)
{
    const int N = boxes.size();

    BL_ASSERT(m_ref->m_pmap.size() == N + 1);

    std::map< int,LnClassPtr<Ref> >::const_iterator it = m_Cache.find(N+1);

    if (it != m_Cache.end())
    {
        m_ref = it->second;

        BL_ASSERT(m_ref->m_pmap[N] == ParallelDescriptor::MyProc());

        return true;
    }

    return false;
}

void
DistributionMapping::ReplaceCachedProcessorMap (const Array<int>& newProcmapArray)
{
    const int N(newProcmapArray.size());
    BL_ASSERT(m_ref->m_pmap.size() == N);
    BL_ASSERT(newProcmapArray.size() == N);

    for(int iA(0); iA < N; ++iA) {
      m_ref->m_pmap[iA] = newProcmapArray[iA];
    }

}

DistributionMapping::Ref::Ref () {}

DistributionMapping::DistributionMapping ()
    :
    m_ref(new DistributionMapping::Ref)
{
  dmID = nDistMaps++;
}

DistributionMapping::DistributionMapping (const DistributionMapping& rhs)
    :
    m_ref(rhs.m_ref)
{
  dmID = nDistMaps++;
}

DistributionMapping&
DistributionMapping::operator= (const DistributionMapping& rhs)
{
    m_ref = rhs.m_ref;

    return *this;
}

DistributionMapping::Ref::Ref (const Array<int>& pmap)
    :
    m_pmap(pmap)
{}

DistributionMapping::DistributionMapping (const Array<int>& pmap, bool put_in_cache)
    :
    m_ref(new DistributionMapping::Ref(pmap))
{
    dmID = nDistMaps++;

    if (put_in_cache && ParallelDescriptor::NProcs() > 1)
    {
        //
        // We want to save this pmap in the cache.
        // It's an error if a pmap of this length has already been cached.
        //
	std::pair<std::map< int,LnClassPtr<Ref> >::iterator, bool> r;
	r = m_Cache.insert(std::make_pair(m_ref->m_pmap.size(),m_ref));
	if (r.second == false) {
	    BoxLib::Abort("DistributionMapping::DistributionMapping: pmap of given length already exists");
	}
    }
}

DistributionMapping::Ref::Ref (int len)
    :
    m_pmap(len)
{}

DistributionMapping::DistributionMapping (const BoxArray& boxes, int nprocs)
    :
    m_ref(new DistributionMapping::Ref(boxes.size() + 1))
{
    dmID = nDistMaps++;
    define(boxes,nprocs);
}

DistributionMapping::Ref::Ref (const Ref& rhs)
    :
    m_pmap(rhs.m_pmap)
{}

DistributionMapping::DistributionMapping (const DistributionMapping& d1,
                                          const DistributionMapping& d2)
    :
    m_ref(new DistributionMapping::Ref(d1.size() + d2.size() - 1))

{
    dmID = nDistMaps++;

    const Array<int>& pmap_1 = d1.ProcessorMap();
    const Array<int>& pmap_2 = d2.ProcessorMap();

    const int L1 = pmap_1.size() - 1; // Length not including sentinel.
    const int L2 = pmap_2.size() - 1; // Length not including sentinel.

    for (int i = 0; i < L1; ++i)
        m_ref->m_pmap[i] = pmap_1[i];

    for (int i = L1, j = 0; j < L2; ++i, ++j)
        m_ref->m_pmap[i] = pmap_2[j];
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[m_ref->m_pmap.size()-1] = ParallelDescriptor::MyProc();
}

void
DistributionMapping::define (const BoxArray& boxes, int nprocs)
{
    Initialize();

    if (m_ref->m_pmap.size() != boxes.size() + 1)
    {
        m_ref->m_pmap.resize(boxes.size() + 1);
    }

    if (nprocs == 1)
    {
        for (int i = 0, N = m_ref->m_pmap.size(); i < N; ++i)
        {
            m_ref->m_pmap[i] = 0;
        }
    }
    else
    {
        if ( ! GetMap(boxes))
        {
	    BL_ASSERT(m_BuildMap != 0);

            (this->*m_BuildMap)(boxes,nprocs);
            //
            // Add the new processor map to the cache.
            //
            m_Cache.insert(std::make_pair(m_ref->m_pmap.size(),m_ref));
        }
    }
}

void
DistributionMapping::define (const Array<int>& pmap)
{
    Initialize();

    if (m_ref->m_pmap.size() != pmap.size())
        m_ref->m_pmap.resize(pmap.size());

    for (unsigned int i=0; i<pmap.size(); ++i)
        m_ref->m_pmap[i] = pmap[i];
}

DistributionMapping::~DistributionMapping () { }

void
DistributionMapping::FlushCache ()
{
    CacheStats(std::cout);
    //
    // Remove maps that aren't referenced anywhere else.
    //
    std::map< int,LnClassPtr<Ref> >::iterator it = m_Cache.begin();

    while (it != m_Cache.end())
    {
        if (it->second.linkCount() == 1)
        {
            m_Cache.erase(it++);
        }
        else
        {
            ++it;
        }
    }
}

void
DistributionMapping::PutInCache ()
{
    if (ParallelDescriptor::NProcs() > 1)
    {
        //
        // We want to save this pmap in the cache.
        // It's an error if a pmap of this length has already been cached.
        //
	std::pair<std::map< int,LnClassPtr<Ref> >::iterator, bool> r;
	r = m_Cache.insert(std::make_pair(m_ref->m_pmap.size(),m_ref));
	if (r.second == false) {
	    BoxLib::Abort("DistributionMapping::PutInCache: pmap of given length already exists");
	}
    }
}

void
DistributionMapping::RoundRobinDoIt (int                  nboxes,
                                     int                  nprocs,
                                     std::vector<LIpair>* LIpairV)
{
//BoxLib::Abort("RoundRobinDoIt");
    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    if (LIpairV)
    {
        BL_ASSERT(LIpairV->size() == nboxes);

        for (int i = 0; i < nboxes; ++i)
        {
            m_ref->m_pmap[(*LIpairV)[i].second] = ord[i%nprocs];
        }
    }
    else
    {
        for (int i = 0; i < nboxes; ++i)
        {
            m_ref->m_pmap[i] = ord[i%nprocs];
        }
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[nboxes] = ParallelDescriptor::MyProc();
}

void
DistributionMapping::RoundRobinProcessorMap (int nboxes, int nprocs)
{
    BL_ASSERT(nboxes > 0);

    if (m_ref->m_pmap.size() != nboxes + 1)
    {
        m_ref->m_pmap.resize(nboxes + 1);
    }

    RoundRobinDoIt(nboxes, nprocs);
}

void
DistributionMapping::RoundRobinProcessorMap (const BoxArray& boxes, int nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(m_ref->m_pmap.size() == boxes.size() + 1);
    //
    // Create ordering of boxes from largest to smallest.
    // When we round-robin the boxes we want to go from largest
    // to smallest box, starting from the CPU having the least
    // amount of FAB data to the one having the most.  This "should"
    // help even out the FAB data distribution when running on large
    // numbers of CPUs, where the lower levels of the calculation are
    // using RoundRobin to lay out fewer than NProc boxes across
    // the CPUs.
    //
    std::vector<LIpair> LIpairV;

    const int N = boxes.size();

    LIpairV.reserve(N);

    for (int i = 0; i < N; ++i)
    {
        LIpairV.push_back(LIpair(boxes[i].numPts(),i));
    }

    Sort(LIpairV, true);

    RoundRobinDoIt(boxes.size(), nprocs, &LIpairV);
}

class WeightedBox
{
    int  m_boxid;
    long m_weight;
public:
    WeightedBox () {}
    WeightedBox (int b, int w) : m_boxid(b), m_weight(w) {}
    long weight () const { return m_weight; }
    int  boxid ()  const { return m_boxid;  }

    bool operator< (const WeightedBox& rhs) const
    {
        return weight() > rhs.weight();
    }
};

class WeightedBoxList
{
    std::list<WeightedBox>* m_lb;
    long                    m_weight;
public:
    WeightedBoxList (std::list<WeightedBox>* lb) : m_lb(lb), m_weight(0) {}
    long weight () const
    {
        return m_weight;
    }
    void erase (std::list<WeightedBox>::iterator& it)
    {
        m_weight -= it->weight();
        m_lb->erase(it);
    }
    void push_back (const WeightedBox& bx)
    {
        m_weight += bx.weight();
        m_lb->push_back(bx);
    }
    int size () const { return m_lb->size(); }
    std::list<WeightedBox>::const_iterator begin () const { return m_lb->begin(); }
    std::list<WeightedBox>::iterator begin ()             { return m_lb->begin(); }
    std::list<WeightedBox>::const_iterator end () const   { return m_lb->end();   }
    std::list<WeightedBox>::iterator end ()               { return m_lb->end();   }

    bool operator< (const WeightedBoxList& rhs) const
    {
        return weight() > rhs.weight();
    }
};

static
void
knapsack (const std::vector<long>&         wgts,
          int                              nprocs,
          std::vector< std::vector<int> >& result,
          Real&                            efficiency,
          bool                             do_full_knapsack,
	  int                              nmax)
{
    //
    // Sort balls by size largest first.
    //
    result.resize(nprocs);

    std::vector<WeightedBox> lb;
    lb.reserve(wgts.size());
    for (unsigned int i = 0, N = wgts.size(); i < N; ++i)
    {
        lb.push_back(WeightedBox(i, wgts[i]));
    }
    BL_ASSERT(lb.size() == wgts.size());
    std::sort(lb.begin(), lb.end());
    BL_ASSERT(lb.size() == wgts.size());
    //
    // For each ball, starting with heaviest, assign ball to the lightest box.
    //
    std::priority_queue<WeightedBoxList>   wblq;
    std::vector< std::list<WeightedBox>* > vbbs(nprocs);
    for (int i  = 0; i < nprocs; ++i)
    {
        vbbs[i] = new std::list<WeightedBox>;
        wblq.push(WeightedBoxList(vbbs[i]));
    }
    BL_ASSERT(int(wblq.size()) == nprocs);
    std::list<WeightedBoxList> wblqg;
    for (unsigned int i = 0, N = wgts.size(); i < N; ++i)
    {
        WeightedBoxList wbl = wblq.top();
        wblq.pop();
        wbl.push_back(lb[i]);
	if (wbl.size() < nmax) {
	    wblq.push(wbl);
	} else {
	    wblqg.push_back(wbl);
	}
    }
    while (!wblq.empty())
    {
        wblqg.push_back(wblq.top());
        wblq.pop();
    }
    BL_ASSERT(int(wblqg.size()) == nprocs);
    wblqg.sort();
    //
    // Compute the max weight and the sum of the weights.
    //
    Real max_weight = 0;
    Real sum_weight = 0;
    std::list<WeightedBoxList>::iterator it = wblqg.begin();
    for (std::list<WeightedBoxList>::const_iterator End =  wblqg.end(); it != End; ++it)
    {
        long wgt = (*it).weight();
        sum_weight += wgt;
        max_weight = (wgt > max_weight) ? wgt : max_weight;
    }

    efficiency = sum_weight/(nprocs*max_weight);

top:

    std::list<WeightedBoxList>::iterator it_top = wblqg.begin();

    WeightedBoxList wbl_top = *it_top;
    //
    // For each ball in the heaviest box.
    //
    std::list<WeightedBox>::iterator it_wb = wbl_top.begin();

    if (efficiency > max_efficiency || !do_full_knapsack) goto bottom;

    for ( ; it_wb != wbl_top.end(); ++it_wb )
    {
        //
        // For each ball not in the heaviest box.
        //
        std::list<WeightedBoxList>::iterator it_chk = it_top;
        it_chk++;
        for ( ; it_chk != wblqg.end(); ++it_chk)
        {
            WeightedBoxList wbl_chk = *it_chk;
            std::list<WeightedBox>::iterator it_owb = wbl_chk.begin();
            for ( ; it_owb != wbl_chk.end(); ++it_owb)
            {
                //
                // If exchanging these two balls reduces the load balance,
                // then exchange them and go to top.  The way we are doing
                // things, sum_weight cannot change.  So the efficiency will
                // increase if after we switch the two balls *it_wb and
                // *it_owb the max weight is reduced.
                //
                Real w_tb = (*it_top).weight() + (*it_owb).weight() - (*it_wb).weight();
                Real w_ob = (*it_chk).weight() + (*it_wb).weight() - (*it_owb).weight();
                //
                // If the other ball reduces the weight of the top box when
                // swapped, then it will change the efficiency.
                //
                if (w_tb < (*it_top).weight() && w_ob < (*it_top).weight())
                {
                    //
                    // Adjust the sum weight and the max weight.
                    //
                    WeightedBox wb = *it_wb;
                    WeightedBox owb = *it_owb;
                    wblqg.erase(it_top);
                    wblqg.erase(it_chk);
                    wbl_top.erase(it_wb);
                    wbl_chk.erase(it_owb);
                    wbl_top.push_back(owb);
                    wbl_chk.push_back(wb);
                    std::list<WeightedBoxList> tmp;
                    tmp.push_back(wbl_top);
                    tmp.push_back(wbl_chk);
                    tmp.sort();
                    wblqg.merge(tmp);
                    max_weight = (*wblqg.begin()).weight();
                    efficiency = sum_weight/(nprocs*max_weight);
                    goto top;
                }
            }
        }
    }

 bottom:
    //
    // Here I am "load-balanced".
    //
    std::list<WeightedBoxList>::const_iterator cit = wblqg.begin();

    for (int i = 0; i < nprocs; ++i)
    {
        const WeightedBoxList& wbl = *cit;

        result[i].reserve(wbl.size());

        for (std::list<WeightedBox>::const_iterator it1 = wbl.begin(), End = wbl.end();
            it1 != End;
              ++it1)
        {
            result[i].push_back((*it1).boxid());
        }
        ++cit;
    }

    for (int i  = 0; i < nprocs; ++i)
        delete vbbs[i];
}

void
DistributionMapping::KnapSackDoIt (const std::vector<long>& wgts,
                                   int                      nprocs,
                                   Real&                    efficiency,
                                   bool                     do_full_knapsack,
				   int                      nmax)
{
    BL_PROFILE("DistributionMapping::KnapSackDoIt()");

    std::vector< std::vector<int> > vec;

    efficiency = 0;

    knapsack(wgts,nprocs,vec,efficiency,do_full_knapsack,nmax);

    BL_ASSERT(vec.size() == nprocs);

    Array<long> wgts_per_cpu(nprocs,0);

    for (unsigned int i = 0, N = vec.size(); i < N; ++i)
    {
        for (std::vector<int>::iterator lit = vec[i].begin(), End = vec[i].end();
             lit != End;
             ++lit)
        {
            wgts_per_cpu[i] += wgts[*lit];
        }
    }

    std::vector<LIpair> LIpairV;

    LIpairV.reserve(nprocs);

    for (int i = 0; i < nprocs; ++i)
    {
        LIpairV.push_back(LIpair(wgts_per_cpu[i],i));
    }

    Sort(LIpairV, true);

    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    for (unsigned int i = 0, N = vec.size(); i < N; ++i)
    {
        const int idx = LIpairV[i].second;
        const int cpu = ord[i];

        for (std::vector<int>::iterator lit = vec[idx].begin(), End = vec[idx].end();
             lit != End;
             ++lit)
        {
            m_ref->m_pmap[*lit] = cpu;
        }
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[wgts.size()] = ParallelDescriptor::MyProc();

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        std::cout << "KNAPSACK efficiency: " << efficiency << '\n';
    }

}

void
DistributionMapping::KnapSackProcessorMap (const std::vector<long>& wgts,
                                           int                      nprocs,
                                           Real*                    efficiency,
                                           bool                     do_full_knapsack,
					   int                      nmax)
{
    BL_ASSERT(wgts.size() > 0);

    if (m_ref->m_pmap.size() !=  wgts.size() + 1)
    {
        m_ref->m_pmap.resize(wgts.size() + 1);
    }

    if (wgts.size() <= nprocs || nprocs < 2)
    {
        RoundRobinProcessorMap(wgts.size(),nprocs);

        if (efficiency) *efficiency = 1;
    }
    else
    {
        Real eff = 0;
        KnapSackDoIt(wgts, nprocs, eff, do_full_knapsack, nmax);
        if (efficiency) *efficiency = eff;
    }
}

void
DistributionMapping::KnapSackProcessorMap (const BoxArray& boxes,
					   int             nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(m_ref->m_pmap.size() == boxes.size()+1);

    if (boxes.size() <= nprocs || nprocs < 2)
    {
        RoundRobinProcessorMap(boxes,nprocs);
    }
    else
    {
        std::vector<long> wgts(boxes.size());

        for (unsigned int i = 0, N = boxes.size(); i < N; ++i)
            wgts[i] = boxes[i].numPts();

        Real effi = 0;
        bool do_full_knapsack = true;
        KnapSackDoIt(wgts, nprocs, effi, do_full_knapsack);
    }
}

namespace
{
    struct SFCToken
    {
        class Compare
        {
        public:
            bool operator () (const SFCToken& lhs,
                              const SFCToken& rhs) const;
        };

        SFCToken (int box, const IntVect& idx, Real vol)
            :
            m_box(box), m_idx(idx), m_vol(vol) {}

        int     m_box;
        IntVect m_idx;
        Real    m_vol;

        static int MaxPower;
    };
}

int SFCToken::MaxPower = 64;

bool
SFCToken::Compare::operator () (const SFCToken& lhs,
                                const SFCToken& rhs) const
{
    for (int i = SFCToken::MaxPower - 1; i >= 0; --i)
    {
        const int N = (1<<i);

        for (int j = BL_SPACEDIM-1; j >= 0; --j)
        {
            const int il = lhs.m_idx[j]/N;
            const int ir = rhs.m_idx[j]/N;

            if (il < ir)
            {
                return true;
            }
            else if (il > ir)
            {
                return false;
            }
        }
    }
    return false;
}

static
void
Distribute (const std::vector<SFCToken>&     tokens,
            int                              nprocs,
            Real                             volpercpu,
            std::vector< std::vector<int> >& v)

{
    BL_ASSERT(v.size() == nprocs);

    int  K        = 0;
    Real totalvol = 0;

    for (int i = 0; i < nprocs; ++i)
    {
        int  cnt = 0;
        Real vol = 0;

        for ( int TSZ = tokens.size();
              K < TSZ && (i == (nprocs-1) || vol < volpercpu);
              ++cnt, ++K)
        {
            vol += tokens[K].m_vol;

            v[i].push_back(tokens[K].m_box);
        }

        totalvol += vol;

        if ((totalvol/(i+1)) > volpercpu &&
            cnt > 1                      &&
            K < tokens.size())
        {
            --K;
            v[i].pop_back();
            totalvol -= tokens[K].m_vol;
        }
    }

#ifndef NDEBUG
    int cnt = 0;
    for (int i = 0; i < nprocs; ++i)
        cnt += v[i].size();
    BL_ASSERT(cnt == tokens.size());
#endif
}

void
DistributionMapping::SFCProcessorMapDoIt (const BoxArray&          boxes,
                                          const std::vector<long>& wgts,
                                          int                      nprocs)
{
    BL_PROFILE("DistributionMapping::SFCProcessorMapDoIt()");

    std::vector<SFCToken> tokens;

    const int N = boxes.size();

    tokens.reserve(N);

    int maxijk = 0;

    for (int i = 0; i < N; ++i)
    {
        tokens.push_back(SFCToken(i,boxes[i].smallEnd(),wgts[i]));

        const SFCToken& token = tokens.back();

        D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
               maxijk = std::max(maxijk, token.m_idx[1]);,
               maxijk = std::max(maxijk, token.m_idx[2]););
    }
    //
    // Set SFCToken::MaxPower for BoxArray.
    //
    int m = 0;
    for ( ; (1 << m) <= maxijk; ++m) {
        ;  // do nothing
    }
    SFCToken::MaxPower = m;
    //
    // Put'm in Morton space filling curve order.
    //
    std::sort(tokens.begin(), tokens.end(), SFCToken::Compare());
    //
    // Split'm up as equitably as possible per CPU.
    //
    Real volpercpu = 0;
    for (int i = 0, N = tokens.size(); i < N; ++i)
        volpercpu += tokens[i].m_vol;
    volpercpu /= nprocs;

    std::vector< std::vector<int> > vec(nprocs);

    Distribute(tokens,nprocs,volpercpu,vec);

    tokens.clear();

    Array<long> wgts_per_cpu(nprocs,0);

    for (unsigned int i = 0, N = vec.size(); i < N; ++i)
    {
        const std::vector<int>& vi = vec[i];

        for (int j = 0, M = vi.size(); j < M; ++j)
            wgts_per_cpu[i] += wgts[vi[j]];
    }

    std::vector<LIpair> LIpairV;

    LIpairV.reserve(nprocs);

    for (int i = 0; i < nprocs; ++i)
    {
        LIpairV.push_back(LIpair(wgts_per_cpu[i],i));
    }

    Sort(LIpairV, true);

    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    for (int i = 0; i < nprocs; ++i)
    {
        const int cpu = ord[i];
        const int idx = LIpairV[i].second;

        const std::vector<int>& vi = vec[idx];

        for (int j = 0, N = vi.size(); j < N; ++j)
        {
            m_ref->m_pmap[vi[j]] = cpu;
        }
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[boxes.size()] = ParallelDescriptor::MyProc();

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        Real sum_wgt = 0, max_wgt = 0;
        for (int i = 0, N = wgts_per_cpu.size(); i < N; ++i)
        {
            const long W = wgts_per_cpu[i];
            if (W > max_wgt)
                max_wgt = W;
            sum_wgt += W;
        }

        std::cout << "SFC efficiency: " << (sum_wgt/(nprocs*max_wgt)) << '\n';
    }
}

void
DistributionMapping::SFCProcessorMap (const BoxArray& boxes,
                                      int             nprocs)
{
    BL_ASSERT(boxes.size() > 0);

    if (m_ref->m_pmap.size() != boxes.size() + 1)
    {
        m_ref->m_pmap.resize(boxes.size()+1);
    }

    if (boxes.size() < sfc_threshold*nprocs)
    {
        KnapSackProcessorMap(boxes,nprocs);
    }
    else
    {
        std::vector<long> wgts;

        wgts.reserve(boxes.size());

        for (BoxArray::const_iterator it = boxes.begin(), End = boxes.end(); it != End; ++it)
        {
            wgts.push_back(it->volume());
        }

        SFCProcessorMapDoIt(boxes,wgts,nprocs);
    }
}

void
DistributionMapping::SFCProcessorMap (const BoxArray&          boxes,
                                      const std::vector<long>& wgts,
                                      int                      nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(boxes.size() == wgts.size());

    if (m_ref->m_pmap.size() != wgts.size() + 1)
    {
        m_ref->m_pmap.resize(wgts.size()+1);
    }

    if (boxes.size() < sfc_threshold*nprocs)
    {
        KnapSackProcessorMap(wgts,nprocs);
    }
    else
    {
        SFCProcessorMapDoIt(boxes,wgts,nprocs);
    }
}

void
DistributionMapping::RRSFCDoIt (const BoxArray&          boxes,
				int                      nprocs)
{
    BL_PROFILE("DistributionMapping::RRSFCDoIt()");

    std::vector<SFCToken> tokens;

    const int nboxes = boxes.size();

    tokens.reserve(nboxes);

    int maxijk = 0;

    for (int i = 0; i < nboxes; ++i)
    {
        tokens.push_back(SFCToken(i,boxes[i].smallEnd(),0.0));

        const SFCToken& token = tokens.back();

        D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
               maxijk = std::max(maxijk, token.m_idx[1]);,
               maxijk = std::max(maxijk, token.m_idx[2]););
    }
    //
    // Set SFCToken::MaxPower for BoxArray.
    //
    int m = 0;
    for ( ; (1 << m) <= maxijk; ++m) {
        ;  // do nothing
    }
    SFCToken::MaxPower = m;
    //
    // Put'm in Morton space filling curve order.
    //
    std::sort(tokens.begin(), tokens.end(), SFCToken::Compare());

    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    // Distribute boxes using roundrobin
    for (int i = 0; i < nboxes; ++i) {
	m_ref->m_pmap[i] = ord[i%nprocs];
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[nboxes] = ParallelDescriptor::MyProc();
}

void
DistributionMapping::RRSFCProcessorMap (const BoxArray&          boxes,
                                        int                      nprocs)
{
    BL_ASSERT(boxes.size() > 0);
 
    if (m_ref->m_pmap.size() != boxes.size() + 1)
    {
        m_ref->m_pmap.resize(boxes.size() + 1);
    }

    RRSFCDoIt(boxes,nprocs);
}

namespace
{
    struct PFCToken
    {
        class Compare
        {
        public:
            bool operator () (const PFCToken& lhs,
                              const PFCToken& rhs) const;
        };

        PFCToken (int box, const IntVect& idx, Real vol)
            :
            m_box(box), m_idx(idx), m_vol(vol) {}

        int     m_box;
        IntVect m_idx;
        Real    m_vol;
    };
}



bool
PFCToken::Compare::operator () (const PFCToken& lhs,
                                const PFCToken& rhs) const
{
  return lhs.m_idx.lexLT(rhs.m_idx);
}


void
DistributionMapping::CurrentBytesUsed (int nprocs, Array<long>& result)
{
    result.resize(nprocs);
    Array<long> bytes(nprocs, 0);

#ifdef BL_USE_MPI
    BL_PROFILE("DistributionMapping::CurrentBytesUsed()");

    long thisbyte = BoxLib::TotalBytesAllocatedInFabs();

    BL_COMM_PROFILE(BLProfiler::Allgather, sizeof(long), BLProfiler::BeforeCall(),
                    BLProfiler::NoTag());
    MPI_Allgather(&thisbyte,
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  bytes.dataPtr(),
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  ParallelDescriptor::Communicator());
    BL_COMM_PROFILE(BLProfiler::Allgather, sizeof(long), BLProfiler::AfterCall(),
                    BLProfiler::NoTag());
#endif

    for (int i(0); i < nprocs; ++i)
    {
        result[i] = bytes[i];
    }
if(ParallelDescriptor::IOProcessor()) {
  std::cout << "**********************************" << std::endl;
  for(int i(0); i < result.size(); ++i) {
    std::cout << "currentBytes[" << i << "] = " << result[i] << std::endl;
  }
  std::cout << "**********************************" << std::endl;
  static int count(0);
  std::stringstream dfss;
  dfss << "CurrentBytes.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < result.size(); ++i) {
    bos << i << ' ' << result[i] << '\n';
  }
  bos.close();
}

}


void
DistributionMapping::CurrentCellsUsed (int nprocs, Array<long>& result)
{
    result.resize(nprocs);
    Array<long> cells(nprocs, 0);

#ifdef BL_USE_MPI
    BL_PROFILE("DistributionMapping::CurrentCellsUsed()");

    long thiscell = BoxLib::TotalCellsAllocatedInFabs();

    BL_COMM_PROFILE(BLProfiler::Allgather, sizeof(long), BLProfiler::BeforeCall(),
                    BLProfiler::NoTag());
    MPI_Allgather(&thiscell,
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  cells.dataPtr(),
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  ParallelDescriptor::Communicator());
    BL_COMM_PROFILE(BLProfiler::Allgather, sizeof(long), BLProfiler::AfterCall(),
                    BLProfiler::NoTag());
#endif

    for(int i(0); i < nprocs; ++i) {
      result[i] = cells[i];
    }
}


void
DistributionMapping::PFCProcessorMapDoIt (const BoxArray&          boxes,
                                          const std::vector<long>& wgts,
                                          int                      nprocs)
{
    BL_PROFILE("DistributionMapping::PFCProcessorMapDoIt()");

    std::vector< std::vector<int> > vec(nprocs);
    std::vector<PFCToken> tokens;
    tokens.reserve(boxes.size());
    int maxijk(0);

    for(int i(0), N(boxes.size()); i < N; ++i) {
        tokens.push_back(PFCToken(i, boxes[i].smallEnd(), wgts[i]));
        const PFCToken &token = tokens.back();
        D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
               maxijk = std::max(maxijk, token.m_idx[1]);,
               maxijk = std::max(maxijk, token.m_idx[2]););
    }

    std::sort(tokens.begin(), tokens.end(), PFCToken::Compare());  // sfc order

    Real totC(0.0);
    Array<long> aCurrentCells;
    CurrentCellsUsed(nprocs, aCurrentCells);
    if(ParallelDescriptor::IOProcessor()) {
      for(int i(0); i < aCurrentCells.size(); ++i) {
        std::cout << "aCurrentCells[" << i << "] = " << aCurrentCells[i] << std::endl;
	totC += aCurrentCells[i];
      }
    }

    long totalCurrentCells(0);
    for(int i(0); i < nprocs; ++i) {
      totalCurrentCells += aCurrentCells[i];
    }

      int  K(0);
      Real totalvol(0.0), volpercpu(0.0), ccScale(1.0);
      const int Navg(tokens.size() / nprocs);
      long totalNewCells(0), totalNewCellsB(0);
      for(int i(0); i < tokens.size(); ++i) {        // new cells to add
        totalNewCells  += tokens[i].m_vol;
        totalNewCellsB += boxes[i].numPts();
      }
      if(totalNewCells != totalNewCellsB) {
        BoxLib::Abort("tnc");
      }
      volpercpu = static_cast<Real>(totalNewCells) / nprocs;

      Array<long> scaledCurrentCells(aCurrentCells.size(), 0);
      if(totalCurrentCells > 0) {
        ccScale = static_cast<Real>(totalNewCells) / totalCurrentCells;
      }
      for(int i(0); i < aCurrentCells.size(); ++i) {
        scaledCurrentCells[i] = ccScale * aCurrentCells[i];
      }

      Array<long> newVolPerCPU(nprocs, 0);
      if(totalCurrentCells > 0) {
        for(int i(0); i < newVolPerCPU.size(); ++i) {
          newVolPerCPU[i] = (2.0 * volpercpu) - scaledCurrentCells[i];
        }
      } else {
        for(int i(0); i < newVolPerCPU.size(); ++i) {
          newVolPerCPU[i] = volpercpu;
        }
      }

      for(int i(0); i < nprocs; ++i) {
        int  cnt(0);
        Real vol(0.0);
	long accVol(0);
        vec[i].reserve(Navg + 2);

        for(int TSZ(tokens.size()); K < TSZ &&
	    //(i == (nprocs-1) || vol < (newVolPerCPU[i] - tokens[K].m_vol / 2));
	    (i == (nprocs-1) || vol < (newVolPerCPU[i] - 0));
            ++cnt, ++K)
        {
            vol += tokens[K].m_vol;
            accVol += tokens[K].m_vol;
            vec[i].push_back(tokens[K].m_box);
        }

        totalvol += vol;
        //if((totalvol / (i + 1)) > (newVolPerCPU[i] + tokens[K].m_vol / 2) &&
        if((totalvol / (i + 1)) > (newVolPerCPU[i] + 0) &&
	   cnt > 1 && K < tokens.size())
	{
            --K;
            vec[i].pop_back();
            totalvol -= tokens[K].m_vol;
            accVol -= tokens[K].m_vol;
        }
      aCurrentCells[i] += accVol;

	int extra(newVolPerCPU[i] - accVol);
        if(extra != 0 && i < nprocs - 1) {  // add the difference to the rest
	  extra /= nprocs - (i + 1);
	  for(int ii(i+1); ii < nprocs; ++ii) {
	    newVolPerCPU[ii] += extra;
	  }
        }
      }


if(ParallelDescriptor::IOProcessor()) {
  long npoints(0);
  for(int i(0); i < boxes.size(); ++i) {
    npoints += boxes[i].numPts();
  }

  static int count(0);
  std::stringstream dfss;
  dfss << "CurrentCellsAcc.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < aCurrentCells.size(); ++i) {
    bos << i << ' ' << aCurrentCells[i] << '\n';
  }
  bos.close();
}

    tokens.clear();
    Array<long> wgts_per_cpu(nprocs, 0);
    for (unsigned int i(0), N(vec.size()); i < N; ++i) {
        const std::vector<int>& vi = vec[i];
        for (int j(0), M(vi.size()); j < M; ++j) {
            wgts_per_cpu[i] += wgts[vi[j]];
	}
    }

    for (int i(0); i < nprocs; ++i) {
        const std::vector<int> &vi = vec[i];

        for(int j(0), N(vi.size()); j < N; ++j) {
          m_ref->m_pmap[vi[j]] = ProximityMap(i);
        }
    }

    // Set sentinel equal to our processor number.
    m_ref->m_pmap[boxes.size()] = ParallelDescriptor::MyProc();

    if(ParallelDescriptor::IOProcessor()) {
        Real sum_wgt = 0, max_wgt = 0;
        for(int i = 0, N = wgts_per_cpu.size(); i < N; ++i) {
            const long W = wgts_per_cpu[i];
            if(W > max_wgt) {
              max_wgt = W;
	    }
            sum_wgt += W;
        }
        std::cout << "PFC efficiency: " << (sum_wgt/(nprocs*max_wgt)) << '\n';
    }
}


void
DistributionMapping::PFCProcessorMap (const BoxArray& boxes,
                                      int             nprocs)
{
    BL_ASSERT(boxes.size() > 0);

    if (m_ref->m_pmap.size() != boxes.size() + 1) {
        m_ref->m_pmap.resize(boxes.size()+1);
    }

    std::vector<long> wgts;
    wgts.reserve(boxes.size());

    for (BoxArray::const_iterator it = boxes.begin(), End = boxes.end(); it != End; ++it)
    {
      wgts.push_back(it->numPts());
    }
    PFCProcessorMapDoIt(boxes,wgts,nprocs);
}


void
DistributionMapping::PFCProcessorMap (const BoxArray&          boxes,
                                      const std::vector<long>& wgts,
                                      int                      nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(boxes.size() == wgts.size());

    if (m_ref->m_pmap.size() != wgts.size() + 1) {
        m_ref->m_pmap.resize(wgts.size()+1);
    }
    PFCProcessorMapDoIt(boxes,wgts,nprocs);
}



namespace
{
    struct PFCMultiLevelToken
    {
        class Compare
        {
        public:
            bool operator () (const PFCMultiLevelToken& lhs,
                              const PFCMultiLevelToken& rhs) const;
        };

        PFCMultiLevelToken (int level, int idxAll, int idxLevel,
                            const IntVect &boxiv, const IntVect &fineiv,
                            Real vol)
            :
            m_level(level), m_idxAll(idxAll), m_idxLevel(idxLevel),
            m_boxiv(boxiv), m_fineiv(fineiv), m_vol(vol) {}

        int     m_level, m_idxAll, m_idxLevel;
        IntVect m_boxiv, m_fineiv;
        Real    m_vol;
    };
}


bool
PFCMultiLevelToken::Compare::operator () (const PFCMultiLevelToken& lhs,
                                          const PFCMultiLevelToken& rhs) const
{
  return lhs.m_fineiv.lexLT(rhs.m_fineiv);
}




Array<Array<int> >
DistributionMapping::MultiLevelMapPFC (const Array<IntVect>  &refRatio,
                                       const Array<BoxArray> &allBoxes,
				       int maxgrid)
{
    BL_PROFILE("DistributionMapping::MultiLevelMapPFC()");

    using std::cout;
    using std::endl;

    int nProcs(ParallelDescriptor::NProcs());
    long totalCells(0);
    int nLevels(allBoxes.size());
    int finestLevel(nLevels - 1);
    int nBoxes(0);
    for(int level(0); level < nLevels; ++level) {
      nBoxes += allBoxes[level].size();
      totalCells += allBoxes[level].numPts();
    }

    std::vector< std::vector<int> > vec(nProcs);
    std::vector<PFCMultiLevelToken> tokens;
    tokens.reserve(nBoxes);
    int idxAll(0);
    IntVect cRR(IntVect::TheUnitVector());

    for(int level(finestLevel); level >= 0; --level) {
      for(int i(0), N(allBoxes[level].size()); i < N; ++i) {
	Box box(allBoxes[level][i]);
	Box fine(BoxLib::refine(box, cRR));
        tokens.push_back(PFCMultiLevelToken(level, idxAll, i,
	                 box.smallEnd(), fine.smallEnd(), box.numPts()));
      }
      if(level > 0) {
        cRR *= refRatio[level - 1];
      }
      ++idxAll;
    }

    std::sort(tokens.begin(), tokens.end(), PFCMultiLevelToken::Compare());  // sfc order

      int  K(0);
      Real totalvol(0.0), volpercpu(0.0);
      const int Navg(tokens.size() / nProcs);
      volpercpu = static_cast<Real>(totalCells) / nProcs;

      Array<long> newVolPerCPU(nProcs, volpercpu);

      for(int iProc(0); iProc < nProcs; ++iProc) {
        int  cnt(0);
        Real vol(0.0);
	long accVol(0);
        vec[iProc].reserve(Navg + 2);

        for(int TSZ(tokens.size()); K < TSZ &&
	    (iProc == (nProcs-1) || vol < (newVolPerCPU[iProc]));
            ++cnt, ++K)
        {
            vol += tokens[K].m_vol;
            accVol += tokens[K].m_vol;
            vec[iProc].push_back(K);
        }

        totalvol += vol;
        if((totalvol / (iProc + 1)) > (newVolPerCPU[iProc]) &&
	   cnt > 1 && K < tokens.size())
	{
            --K;
            vec[iProc].pop_back();
            totalvol -= tokens[K].m_vol;
            accVol -= tokens[K].m_vol;
        }

	int extra(newVolPerCPU[iProc] - accVol);
        if(extra != 0 && iProc < nProcs - 1) {  // add the difference to the rest
	  extra /= nProcs - (iProc + 1);
	  for(int ip(iProc + 1); ip < nProcs; ++ip) {
	    newVolPerCPU[ip] += extra;
	  }
        }
      }

    Array<Array<int> > localPMaps(nLevels);
    for(int n(0); n < localPMaps.size(); ++n) {
      localPMaps[n].resize(allBoxes[n].size() + 1, -1);
      // Set sentinel equal to our processor number.
      localPMaps[n][allBoxes[n].size()] = ParallelDescriptor::MyProc();
    }

    bool bStagger(false);
    if(bStagger) {
      int staggerOffset(12);
      Array<int> staggeredProxMap(proximityMap.size());

      int nSets(nProcs / staggerOffset);
      int nRemainder(nProcs % staggerOffset);
      int nCount(0);
      for(int iS(0); iS < staggerOffset; ++iS) {
        for(int nS(0); nS < nSets * staggerOffset; nS += staggerOffset) {
	  staggeredProxMap[nCount++] = nS + iS;
        }
      }
      for(int iR(0); iR < nRemainder; ++iR) {
        int index((staggerOffset * nSets) + iR);;
        staggeredProxMap[index] = index;;
      }
      for(int iProc(0); iProc < nProcs; ++iProc) {
        if(ParallelDescriptor::IOProcessor()) {
	  std::cout << "staggeredProxMap[" << iProc << "] = " << staggeredProxMap[iProc] << std::endl;
        }
        if(staggeredProxMap[iProc] >= nProcs) {
	  std::cout << "Stagger:  ERROR!" << std::endl;
          BoxLib::Abort("*****");
	}
      }

      for(int iProc(0); iProc < nProcs; ++iProc) {
        const std::vector<int> &vi = vec[iProc];
        for(int j(0), N(vi.size()); j < N; ++j) {
	  PFCMultiLevelToken &pt = tokens[vi[j]];
	  int level(pt.m_level);
	  int idxLevel(pt.m_idxLevel);
	  int staggeredProc(staggeredProxMap[iProc]);
	  localPMaps[level][idxLevel] = staggeredProc;
        }
      }
    } else {
      for(int iProc(0); iProc < nProcs; ++iProc) {
        const std::vector<int> &vi = vec[iProc];
        for(int j(0), N(vi.size()); j < N; ++j) {
	  PFCMultiLevelToken &pt = tokens[vi[j]];
	  int level(pt.m_level);
	  int idxLevel(pt.m_idxLevel);
	  localPMaps[level][idxLevel] = ProximityMap(iProc);
        }
      }
    }

    tokens.clear();

if(ParallelDescriptor::IOProcessor()) {
  Real maxGridPts(maxgrid * maxgrid * maxgrid);
  Array<Array<int> > boxesPerProc(nProcs);
  Array<Real> ncells(nProcs, 0);
  int ib(0), nb(0);
  for(int n(0); n < allBoxes.size(); ++n) {
    nb += allBoxes[n].size();
  }
  std::cout << "nb = " << nb << std::endl;
  Array<long> ncellsPerBox(nb, 0);
  for(int n(0); n < localPMaps.size(); ++n) {
    for(int i(0); i < localPMaps[n].size() - 1; ++i) {
      int index(localPMaps[n][i]);
      ncells[index] += allBoxes[n][i].d_numPts() / maxGridPts;
      if(ib > ncellsPerBox.size()) {
        std::cout << "ib ncellsPerBox.size() = " << ib << "  " << ncellsPerBox.size() << std::endl;
      }
      ncellsPerBox[ib] = allBoxes[n][i].numPts();
      boxesPerProc[index].push_back(allBoxes[n][i].numPts());
      ++ib;
    }
  }
  static int count(0);
  std::stringstream dfss;
  dfss << "MLMB_" << count << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < ncells.size(); ++i) {
    bos << i << ' ' << std::setprecision(8) << ncells[i] << '\n';
  }
  bos.close();
  std::stringstream dfsspb;
  dfsspb << "MLMPERB_" << count << ".xgr";
  std::ofstream bospb(dfsspb.str().c_str());
  for(int i(0); i < ncellsPerBox.size(); ++i) {
    bospb << i << ' ' << std::setprecision(8) << ncellsPerBox[i] << '\n';
  }
  bospb.close();
  std::stringstream bpproc;
  bpproc << "BPPROC_" << count << ".xgr";
  std::ofstream sbpp(bpproc.str().c_str());
  for(int n(0); n < boxesPerProc.size(); ++n) {
    sbpp << n;
    for(int i(0); i < boxesPerProc[n].size(); ++i) {
      sbpp << ' ' << boxesPerProc[n][i];
    }
    sbpp << '\n';
  }
  sbpp.close();
  ++count;
}

    return localPMaps;
}




Array<Array<int> >
DistributionMapping::MultiLevelMapRandom (const Array<IntVect>  &refRatio,
                                          const Array<BoxArray> &allBoxes,
					  int maxgrid)
{
    BL_PROFILE("DistributionMapping::MultiLevelMapRandom()");

    Array<Array<int> > localPMaps(allBoxes.size());
    for(int n(0); n < localPMaps.size(); ++n) {
      localPMaps[n].resize(allBoxes[n].size() + 1, -1);

      if(ParallelDescriptor::IOProcessor()) {
        for(int ir(0); ir < localPMaps[n].size() - 1; ++ir) {
          localPMaps[n][ir] = BoxLib::Random_int(ParallelDescriptor::NProcs());
        }
      }
      ParallelDescriptor::Bcast(localPMaps[n].dataPtr(), localPMaps[n].size());

      // Set sentinel equal to our processor number.
      localPMaps[n][allBoxes[n].size()] = ParallelDescriptor::MyProc();
    }

    return localPMaps;
}


Array<Array<int> >
DistributionMapping::MultiLevelMapKnapSack (const Array<IntVect>  &refRatio,
                                            const Array<BoxArray> &allBoxes,
					    int maxgrid)
{
    BL_PROFILE("DistributionMapping::MultiLevelMapKnapSack()");

    int nProcs(ParallelDescriptor::NProcs());
    std::vector<std::vector<int> > weightsPerCPU;
    Real efficiency(0.0);
    bool doFullKnapSack(true);
    int nMax(std::numeric_limits<int>::max());

    Array<long> weights;
    for(int n(0); n < allBoxes.size(); ++n) {
      const BoxArray &aba = allBoxes[n];
      for(int b(0); b < aba.size(); ++b) {
        weights.push_back(aba[b].numPts());
      }
    }

    knapsack(weights, nProcs, weightsPerCPU, efficiency, doFullKnapSack, nMax);

    int count(0);
    for(int cpu(0); cpu < weightsPerCPU.size(); ++cpu) {
      for(int b(0); b < weightsPerCPU[cpu].size(); ++b) {
        //weights[count++] = weightsPerCPU[cpu][b];
        weights[weightsPerCPU[cpu][b]] = cpu;
      }
    }
    count = 0;

    Array<Array<int> > localPMaps(allBoxes.size());
    for(int n(0); n < localPMaps.size(); ++n) {
      localPMaps[n].resize(allBoxes[n].size() + 1, -1);

      if(ParallelDescriptor::IOProcessor()) {
        for(int ir(0); ir < localPMaps[n].size() - 1; ++ir) {
          localPMaps[n][ir] = weights[count++];
        }
      }
      ParallelDescriptor::Bcast(localPMaps[n].dataPtr(), localPMaps[n].size());

      // Set sentinel equal to our processor number.
      localPMaps[n][allBoxes[n].size()] = ParallelDescriptor::MyProc();
    }

    return localPMaps;
}


void
DistributionMapping::PFCMultiLevelMap (const Array<IntVect>  &refRatio,
                                       const Array<BoxArray> &allBoxes)
{
    BL_PROFILE("DistributionMapping::PFCMultiLevelMap()");

    bool IOP(ParallelDescriptor::IOProcessor());
    using std::cout;
    using std::endl;

    int nprocs(ParallelDescriptor::NProcs());
    long totalCells(0);
    int nLevels(allBoxes.size());
    int finestLevel(nLevels - 1);
    int nBoxes(0);
    for(int level(0); level < nLevels; ++level) {
      nBoxes += allBoxes[level].size();
      totalCells += allBoxes[level].numPts();
    }

    std::vector< std::vector<int> > vec(nprocs);
    std::vector<PFCMultiLevelToken> tokens;
    tokens.reserve(nBoxes);
    int idxAll(0);
    IntVect cRR(IntVect::TheUnitVector());

    for(int level(finestLevel); level >= 0; --level) {
      for(int i(0), N(allBoxes[level].size()); i < N; ++i) {
	Box box(allBoxes[level][i]);
	Box fine(BoxLib::refine(box, cRR));
        tokens.push_back(PFCMultiLevelToken(level, idxAll, i,
	                 box.smallEnd(), fine.smallEnd(), box.numPts()));
      }
      if(level > 0) {
        cRR *= refRatio[level - 1];
      }
      ++idxAll;
    }

if(IOP) cout << "==============" << endl;
ParallelDescriptor::Barrier();
    std::sort(tokens.begin(), tokens.end(), PFCMultiLevelToken::Compare());  // sfc order

    //long totalCurrentCells(0);

      int  K(0);
      Real totalvol(0.0), volpercpu(0.0);
      const int Navg(tokens.size() / nprocs);
      volpercpu = static_cast<Real>(totalCells) / nprocs;

      Array<long> newVolPerCPU(nprocs, volpercpu);

      for(int iProc(0); iProc < nprocs; ++iProc) {
        int  cnt(0);
        Real vol(0.0);
	long accVol(0);
        vec[iProc].reserve(Navg + 2);

        for(int TSZ(tokens.size()); K < TSZ &&
	    (iProc == (nprocs-1) || vol < (newVolPerCPU[iProc]));
            ++cnt, ++K)
        {
            vol += tokens[K].m_vol;
            accVol += tokens[K].m_vol;
            vec[iProc].push_back(K);
        }

        totalvol += vol;
        if((totalvol / (iProc + 1)) > (newVolPerCPU[iProc]) &&
	   cnt > 1 && K < tokens.size())
	{
            --K;
            vec[iProc].pop_back();
            totalvol -= tokens[K].m_vol;
            accVol -= tokens[K].m_vol;
        }

	int extra(newVolPerCPU[iProc] - accVol);
        if(extra != 0 && iProc < nprocs - 1) {  // add the difference to the rest
	  extra /= nprocs - (iProc + 1);
	  for(int ip(iProc + 1); ip < nprocs; ++ip) {
	    newVolPerCPU[ip] += extra;
	  }
        }
      }

    Array<Array<int> > localPMaps(nLevels);
    for(int n(0); n < localPMaps.size(); ++n) {
      localPMaps[n].resize(allBoxes[n].size() + 1, -1);
      // Set sentinel equal to our processor number.
      localPMaps[n][allBoxes[n].size()] = ParallelDescriptor::MyProc();
    }

    for(int iProc(0); iProc < nprocs; ++iProc) {
      const std::vector<int> &vi = vec[iProc];
      for(int j(0), N(vi.size()); j < N; ++j) {
	PFCMultiLevelToken &pt = tokens[vi[j]];
	int level(pt.m_level);
	int idxLevel(pt.m_idxLevel);
	localPMaps[level][idxLevel] = ProximityMap(iProc);
      }
    }

    for(int n(0); n < localPMaps.size(); ++n) {
      for(int i(0); i < localPMaps[n].size(); ++i) {
        if(localPMaps[n][i] == -1) {
	  std::cout << "*********** n i == -1:  " << n << "  " << i << std::endl;
	}
if(IOP) cout << "localPMaps[" << n << "][" << i << "] = " << localPMaps[n][i] << endl;
      }
    }

    tokens.clear();
    /*
    Array<long> wgts_per_cpu(nprocs, 0);
    for (unsigned int i(0), N(vec.size()); i < N; ++i) {
        const std::vector<int>& vi = vec[i];
        for (int j(0), M(vi.size()); j < M; ++j) {
            wgts_per_cpu[i] += wgts[vi[j]];
	}
    }
    */


    DistributionMapping::FlushCache();

    for(int n(0); n < localPMaps.size(); ++n) {
      LnClassPtr<Ref> m_ref(new DistributionMapping::Ref(localPMaps[n]));
      m_Cache.insert(std::make_pair(m_ref->m_pmap.size(),m_ref));
    }


    if(ParallelDescriptor::IOProcessor()) {

      Array<long> ncells(nprocs, 0);
      for(int n(0); n < localPMaps.size(); ++n) {
        for(int i(0); i < localPMaps[n].size() - 1; ++i) {
          int index(localPMaps[n][i]);
          ncells[index] += allBoxes[n][i].numPts();
        }
      }
      Real sum_wgt(0.0), max_wgt(0.0);
      for(int i(0), N(ncells.size()); i < N; ++i) {
        const long W(ncells[i]);
        if(W > max_wgt) {
          max_wgt = W;
	}
        sum_wgt += W;
      }
      std::cout << "PFC efficiency: " << (sum_wgt/(nprocs*max_wgt)) << '\n';
    }
}




std::string
DistributionMapping::GetProcName() {
  int resultLen(-1);
  char cProcName[MPI_MAX_PROCESSOR_NAME + 11];
#ifdef BL_USE_MPI
  MPI_Get_processor_name(cProcName, &resultLen);
#endif
  if(resultLen < 1) {
    strcpy(cProcName, "NoProcName");
  }
  return(std::string(cProcName));
}


int
DistributionMapping::GetProcNumber() {
#ifdef BL_HOPPER
  std::string procName(GetProcName());
  return(atoi(procName.substr(3, string::npos).c_str()));
#else
#ifdef BL_SIM_HOPPER
  //static int procNumber = (100 * ParallelDescriptor::MyProc()) % 6527;
  static int procNumber = ParallelDescriptor::MyProc();
  std::cout << ParallelDescriptor::MyProc() << "||procNumber = " << procNumber << std::endl;
  return(procNumber);
#else
  return(ParallelDescriptor::MyProc());
#endif
#endif
}


void
DistributionMapping::InitProximityMap()
{
  static bool pMapInited(false);
  if(pMapInited) {
    return;
  }

  int nProcs(ParallelDescriptor::NProcs());
  Array<int> procNumbers(nProcs, -1);

  proximityMap.resize(ParallelDescriptor::NProcs(), 0);
  proximityOrder.resize(ParallelDescriptor::NProcs(), 0);

#ifdef BL_USE_MPI
  int procNumber(GetProcNumber());
  MPI_Allgather(&procNumber, 1, ParallelDescriptor::Mpi_typemap<int>::type(),
                procNumbers.dataPtr(), 1, ParallelDescriptor::Mpi_typemap<int>::type(),
                ParallelDescriptor::Communicator());
#endif

  for(int i(0); i < procNumbers.size(); ++i) {
    pNumRankMM.insert(std::pair<int, int>(procNumbers[i], i));
    rankPNumMap.insert(std::pair<int, int>(i, procNumbers[i]));
  }

  // order ranks by procNumber
  Array<int> pNumOrderRank(nProcs, -1);
  int pnor(0);
  for(std::multimap<int, int>::iterator mmit = pNumRankMM.begin();
      mmit != pNumRankMM.end(); ++mmit)
  {
    pNumOrderRank[pnor++] = mmit->second;
  }

  if(ParallelDescriptor::IOProcessor())
  {
    bool bRandomClusters(false);
    Box tBox;
    FArrayBox tFab;
#ifdef BL_SIM_HOPPER
    std::ifstream ifs("topolcoords.simhopper.3d.fab");
#else
    std::ifstream ifs("topolcoords.3d.fab");
#endif
    if( ! ifs.good() && ! bRandomClusters)
    {
      std::cerr << "**** In DistributionMapping::InitProximityMap():  "
                << "cannot open topolcoords.3d.fab   using defaults." << std::endl;

#ifdef BL_SIM_HOPPER
      // set a reasonable default
#if (BL_SPACEDIM == 1)
      tBox = Box(IntVect(0), IntVect(3263), IntVect(0));
#elif (BL_SPACEDIM == 2)
      tBox = Box(IntVect(0,0), IntVect(16,191), IntVect(0,0));
#else
      tBox = Box(IntVect(0,0,0), IntVect(16,7,23), IntVect(0,0,0));
#endif
      tFab.resize(tBox, 2);
      tFab.setVal(-1.0);
      int i(0);
      for(IntVect iv(tBox.smallEnd()); iv <= tBox.bigEnd(); tBox.next(iv)) {
        tFab(iv, 0) = i++;
        tFab(iv, 1) = i++;
      }

#ifdef BL_SIM_HOPPER_MAKE_TCFAB
      std::ofstream ostFab("topolcoords.simhopper.3d.fab");
      tFab.writeOn(ostFab);
      ostFab.close();
#endif

#else
      // dont use proximity mapping
      for(int i(0); i < proximityMap.size(); ++i) {
        proximityMap[i] = i;
      }
      for(int i(0); i < proximityOrder.size(); ++i) {
        proximityOrder[i] = i;
      }
#endif

    } else if(bRandomClusters) {
      if(proximityMap.size() != proximityOrder.size()) {
        BoxLib::Abort("**** Error:  prox size bad.");
      }
      Array<int> rSS(proximityMap.size());
      BoxLib::UniqueRandomSubset(rSS, proximityMap.size(), proximityMap.size());
      for(int i(0); i < proximityMap.size(); ++i) {
	std::cout << "rSS[" << i << "] = " << rSS[i] << std::endl;
        proximityMap[i]   = rSS[i];
        proximityOrder[i] = rSS[i];
      }
    } else {

      tFab.readFrom(ifs);
      ifs.close();

      tBox = tFab.box();
      std::cout << "tBox = " << tBox << "  ncomp = " << tFab.nComp() << std::endl;

      for(int nc(0); nc < tFab.nComp(); ++nc) {
        for(IntVect iv(tBox.smallEnd()); iv <= tBox.bigEnd(); tBox.next(iv)) {
          int pnum(tFab(iv, nc));
          if(pnum >= 0) {
            //std::cout << ">>>> iv pnum = " << iv << "  " << pnum << std::endl;
            pNumTopIVMap.insert(std::pair<int, IntVect>(pnum, iv));
	    topIVpNumMM.insert(std::pair<IntVect, int>(iv, pnum));
          }
        }
      }

      // ------------------------------- make sfc from tFab
      std::vector<SFCToken> tFabTokens;  // use SFCToken here instead of PFC
      tFabTokens.reserve(tBox.numPts());
      int maxijk(0);

      int i(0);
      for(IntVect iv(tBox.smallEnd()); iv <= tBox.bigEnd(); tBox.next(iv))
      {
          tFabTokens.push_back(SFCToken(i++, iv, 1.0));
          const SFCToken &token = tFabTokens.back();

          D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
                 maxijk = std::max(maxijk, token.m_idx[1]);,
                 maxijk = std::max(maxijk, token.m_idx[2]););
      }
      // Set SFCToken::MaxPower for BoxArray.
      int m(0);
      for ( ; (1<<m) <= maxijk; m++)
      {
        // do nothing
      }
      SFCToken::MaxPower = m;
      std::sort(tFabTokens.begin(), tFabTokens.end(), SFCToken::Compare());  // sfc order
      FArrayBox tFabSFC(tBox, 1);
      tFabSFC.setVal(-1.0);
      for(int i(0); i < tFabTokens.size(); ++i)
      {
	IntVect &iv = tFabTokens[i].m_idx;
        tFabSFC(iv) = i;
      }
      std::ofstream tfofs("tFabSFC.3d.fab");
      tFabSFC.writeOn(tfofs);
      tfofs.close();
      // ------------------------------- end make sfc from tFab

      // ------------------------------- order ranks by topological sfc
      std::vector<IntVect> nodesSFC;
      std::cout << std::endl << "----------- order ranks by topological sfc" << std::endl;
      for(int i(0); i < tFabTokens.size(); ++i) {
        IntVect &iv = tFabTokens[i].m_idx;
        std::vector<int> ivRanks = RanksFromTopIV(iv);
        if(ivRanks.size() > 0) {
          nodesSFC.push_back(iv);
          std::cout << "---- iv ranks = " << iv << "  ";
          for(int ivr(0); ivr < ivRanks.size(); ++ivr) {
            ranksSFC.push_back(ivRanks[ivr]);
            std::cout << ivRanks[ivr] << "  ";
          }
          std::cout << std::endl;
        }
      }
      if(ranksSFC.size() != nProcs) {
        std::cerr << "**** Error:  ranksSFC.size() != nProcs:  " << ranksSFC.size()
                  << "  " <<  nProcs << std::endl;
      }
      std::cout << "++++++++++++++++++++++++" << std::endl;
      if(proximityMap.size() != ParallelDescriptor::NProcs()) {
	//std::cout << "####::InitProximityMap: proximityMap not resized yet." << std::endl;
        proximityMap.resize(ParallelDescriptor::NProcs(), 0);
        proximityOrder.resize(ParallelDescriptor::NProcs(), 0);
      }
      for(int i(0); i < ranksSFC.size(); ++i) {
        std::cout << "++++ rank ranksSFC = " << i << "  " << ranksSFC[i] << std::endl;
	proximityMap[i] = ranksSFC[i];
      }
      std::map<int, int> proximityOrderMap;  // [proximityMap[rank], rank]
      for(int i(0); i < proximityMap.size(); ++i) {
	proximityOrderMap.insert(std::pair<int, int>(proximityMap[i], i));
      }
      for(std::map<int, int>::iterator it = proximityOrderMap.begin();
          it != proximityOrderMap.end(); ++it)
      {
        proximityOrder[it->first] = it->second;
      }
      for(int i(0); i < proximityOrder.size(); ++i) {
        std::cout << "++++ rank proximityOrder = " << i << "  "
	          << proximityOrder[i] << std::endl;
      }
      std::cout << "----------- end order ranks by topological sfc" << std::endl;

      FArrayBox nodeFab(tBox);
      nodeFab.setVal(-nProcs);
      for(int i(0); i < nProcs; ++i) {
        IntVect iv(DistributionMapping::TopIVFromRank(i));
        nodeFab(iv) = i;  // this overwrites previous ones
        std::cout << "rank pNum topiv = " << i << "  "
                  << DistributionMapping::ProcNumberFromRank(i) << "  " << iv << std::endl;
      }
      std::ofstream osNodeFab("nodes.3d.fab");
      if( ! osNodeFab.good()) {
        std::cerr << "Error:  could not open nodes.3d.fab" << std::endl;
      } else {
        nodeFab.writeOn(osNodeFab);
        osNodeFab.close();
      }

      std::ofstream rpo("RankProxOrder.txt");
      if( ! rpo.good()) {
        std::cerr << "Error:  could not open RankProxOrder.txt" << std::endl;
      } else {
        rpo << proximityOrder.size() << '\n';
        for(int i(0); i < proximityOrder.size(); ++i) {
	  rpo << i << ' ' << proximityOrder[i] << '\n';
        }
        rpo.close();
      }
    }
  }

  ParallelDescriptor::Bcast(proximityMap.dataPtr(), proximityMap.size(),
                            ParallelDescriptor::IOProcessorNumber());
  ParallelDescriptor::Bcast(proximityOrder.dataPtr(), proximityOrder.size(),
                            ParallelDescriptor::IOProcessorNumber());
  pMapInited = true;
}


int
DistributionMapping::NHops(const Box &tbox, const IntVect &ivfrom, const IntVect &ivto)
{
  int nhops(0);
  for(int d(0); d < BL_SPACEDIM; ++d) {
    int bl(tbox.length(d));
    int ivl(std::min(ivfrom[d], ivto[d]));
    int ivh(std::max(ivfrom[d], ivto[d]));
    int dist(std::min(ivh - ivl, ivl + bl - ivh));
    nhops += dist;
  }
  return nhops;
}


int
DistributionMapping::ProcNumberFromRank(const int rank) {
  int procnum(-1);
  std::map<int, int>::iterator it = rankPNumMap.find(rank);
  if(it == rankPNumMap.end()) {
    if(ParallelDescriptor::IOProcessor()) {
      std::cerr << "**** Error in ProcNumberFromRank:  rank not found:  "
                << rank << std::endl;
    }
  } else {
    procnum = it->second;
    if(procnum != rankPNumMap[rank]) {
      std::cerr << "**** Error in ProcNumberFromRank:  rank not matched:  "
                << rank << std::endl;
    }
  }
  return procnum;
}


std::vector<int>
DistributionMapping::RanksFromProcNumber(const int procnum) {
  std::vector<int> ranks;
  std::pair<std::multimap<int, int>::iterator, std::multimap<int, int>::iterator> mmiter;
  mmiter = pNumRankMM.equal_range(procnum);
  for(std::multimap<int, int>::iterator it = mmiter.first; it != mmiter.second; ++it) {
    ranks.push_back(it->second);
  }
  return ranks;
}


IntVect
DistributionMapping::TopIVFromProcNumber(const int procnum) {
  IntVect iv;
  std::map<int, IntVect>::iterator it = pNumTopIVMap.find(procnum);
  if(it == pNumTopIVMap.end()) {
    if(ParallelDescriptor::IOProcessor()) {
      std::cerr << "**** Error in TopIVFromProcNumber:  procnum not found:  "
                << procnum << std::endl;
    }
  } else {
    iv = it->second;
    if(iv != pNumTopIVMap[procnum]) {
      std::cerr << "**** Error in TopIVFromProcNumber:  procnum not matched:  "
                << procnum << std::endl;
    }
  }
  return iv;
}


std::vector<int>
DistributionMapping::ProcNumbersFromTopIV(const IntVect &iv) {
  std::vector<int> pnums;
  std::pair<std::multimap<IntVect, int, IntVect::Compare>::iterator,
            std::multimap<IntVect, int, IntVect::Compare>::iterator> mmiter;
  mmiter = topIVpNumMM.equal_range(iv);
  for(std::multimap<IntVect, int, IntVect::Compare>::iterator it = mmiter.first;
      it != mmiter.second; ++it)
  {
    pnums.push_back(it->second);
  }
  return pnums;
}


IntVect
DistributionMapping::TopIVFromRank(const int rank) {
  return TopIVFromProcNumber(ProcNumberFromRank(rank));
}


std::vector<int>
DistributionMapping::RanksFromTopIV(const IntVect &iv) {
  std::vector<int> ranks;
  std::vector<int> pnums = ProcNumbersFromTopIV(iv);
  for(int i(0); i < pnums.size(); ++i) {
    std::vector<int> rfpn = RanksFromProcNumber(pnums[i]);
    for(int r(0); r < rfpn.size(); ++r) {
      ranks.push_back(rfpn[r]);
    }
  }
  return ranks;
}


void
DistributionMapping::CacheStats (std::ostream& os)
{
    if (ParallelDescriptor::IOProcessor() && m_Cache.size())
    {
        os << "DistributionMapping::m_Cache.size() = "
           << m_Cache.size()
           << " [ (refs,size): ";

        for (std::map< int,LnClassPtr<Ref> >::const_iterator it = m_Cache.begin();
             it != m_Cache.end();
             ++it)
        {
            os << '(' << it->second.linkCount() << ',' << it->second->m_pmap.size()-1 << ") ";
        }

        os << "]\n";
    }
}


void
DistributionMapping::PrintDiagnostics(const std::string &filename)
{
    int nprocs(ParallelDescriptor::NProcs());
    Array<long> bytes(nprocs, 0);

    long thisbyte = BoxLib::TotalBytesAllocatedInFabs();

    ParallelDescriptor::Gather(&thisbyte,
                               1,
                               bytes.dataPtr(),
                               1,
                               ParallelDescriptor::IOProcessorNumber());

    if(ParallelDescriptor::IOProcessor()) {
      std::ofstream bos(filename.c_str());
      for(int i(0); i < nprocs; ++i) {
        bos << i << ' ' << bytes[i] << '\n';
      }
      bos.close();
    }
    ParallelDescriptor::Barrier();
}


void DistributionMapping::ReadCheckPointHeader(const std::string &filename,
                                               Array<IntVect>  &refRatio,
                                               Array<BoxArray> &allBoxes)
{
    const std::string CheckPointVersion("CheckPointVersion_1.0");
    Array<Geometry> geom;
    int i, max_level, finest_level;
    Real calcTime;
    Array<Real> dt_min;
    Array<Real> dt_level;
    Array<int> level_steps;
    Array<int> level_count;

    // Open the checkpoint header file for reading.
    std::string File(filename);
    File += "/Header";

    VisMF::IO_Buffer io_buffer(VisMF::IO_Buffer_Size);

    Array<char> fileCharPtr;
    ParallelDescriptor::ReadAndBcastFile(File, fileCharPtr);
    std::string fileCharPtrString(fileCharPtr.dataPtr());
    std::istringstream is(fileCharPtrString, std::istringstream::in);

    // Attempt to differentiate between old and new CheckPointFiles.
    int         spdim;
    bool        new_checkpoint_format = false;
    std::string first_line;

    std::getline(is,first_line);

    if(first_line == CheckPointVersion) {
      new_checkpoint_format = true;
      is >> spdim;
    } else {
      spdim = atoi(first_line.c_str());
    }

    if(spdim != BL_SPACEDIM) {
      std::cerr << "Amr::restart(): bad spacedim = " << spdim << '\n';
      BoxLib::Abort();
    }

    is >> calcTime;
    is >> max_level;
    is >> finest_level;

    if(ParallelDescriptor::IOProcessor()) {
      std::cout << "**** fl sd ct ml flev = " << first_line << "  "
                << spdim << "  " << calcTime << "  " << max_level << "  " 
		<< finest_level << std::endl;
    }
    geom.resize(max_level + 1);
    refRatio.resize(max_level);
    dt_min.resize(max_level + 1);
    dt_level.resize(max_level + 1);
    level_steps.resize(max_level + 1);
    level_count.resize(max_level + 1);
    allBoxes.resize(max_level + 1);

    if (max_level >= max_level) {
       for (i = 0; i <= max_level; ++i) { is >> geom[i]; }
       for (i = 0; i <  max_level; ++i) { is >> refRatio[i]; }
       for (i = 0; i <= max_level; ++i) { is >> dt_level[i]; }

       if(new_checkpoint_format) {
         for(i = 0; i <= max_level; ++i) { is >> dt_min[i]; }
       } else {
         for(i = 0; i <= max_level; ++i) dt_min[i] = dt_level[i];
       }

       Array<int>  n_cycle_in;
       n_cycle_in.resize(max_level+1);
       for(i = 0; i <= max_level; ++i) { is >> n_cycle_in[i];  }
       for(i = 0; i <= max_level; ++i) { is >> level_steps[i]; }
       for(i = 0; i <= max_level; ++i) { is >> level_count[i]; }

       // Read levels.
       int lev, level, nstate;
       Geometry levelGeom;
       for(lev = 0; lev <= finest_level; ++lev) {
         if(ParallelDescriptor::IOProcessor()) {
           std::cout << "  -----------  reading level " << lev << std::endl;
         }
         // ------------ amr_level[lev].restart(*this, is);
	 is >> level;
	 is >> levelGeom;
	 allBoxes[lev].readFrom(is);
	 is >> nstate;

	 for(int ins(0); ins < nstate; ++ins) {
	   // ------------ state.restart(...);
	   Box domain;
	   BoxArray stateGrids;
	   Real old_time_start, old_time_stop, new_time_start, new_time_stop;
	   int nsets;
	   std::string mf_name;

	   is >> domain;
	   stateGrids.readFrom(is);
	   is >> old_time_start >> old_time_stop;
	   is >> new_time_start >> new_time_stop;
	   is >> nsets;
	   if(nsets >= 1) { is >> mf_name; }
	   if(nsets == 2) { is >> mf_name; }
	 }

       }

    } else {
    }
    if(ParallelDescriptor::IOProcessor()) {
      std::cout << "--------------------------------------" << std::endl;
      for(int i(0); i < refRatio.size(); ++i) {
        std::cout << "refRatio[" << i << "] = " << refRatio[i] << std::endl;
      }
      for(int i(0); i < allBoxes.size(); ++i) {
        std::cout << "allBoxes[" << i << "].size() = " << allBoxes[i].size() << std::endl;
      }
      std::cout << "--------------------------------------" << std::endl;
    }

    ParallelDescriptor::Barrier();


}

#ifdef BL_USE_MPI
void
DistributionMapping::SendDistributionMappingToSidecars(DistributionMapping *dm)
{
    const int MPI_IntraGroup_Broadcast_Rank = ParallelDescriptor::IOProcessor() ? MPI_ROOT : MPI_PROC_NULL;

    if (ParallelDescriptor::InCompGroup())
    {
        int ProcessorMapSize(dm->size());
        Array<int> ProcessorMap(dm->ProcessorMap());
        ParallelDescriptor::Bcast(&ProcessorMapSize, 1, MPI_IntraGroup_Broadcast_Rank, ParallelDescriptor::CommunicatorInter());
        ParallelDescriptor::Bcast(&ProcessorMap[0], ProcessorMapSize, MPI_IntraGroup_Broadcast_Rank, ParallelDescriptor::CommunicatorInter());
    }
    else
    {
        int ProcessorMapSize;
        ParallelDescriptor::Bcast(&ProcessorMapSize, 1, 0, ParallelDescriptor::CommunicatorInter());
        Array<int> ProcessorMap(ProcessorMapSize);
        ParallelDescriptor::Bcast(&ProcessorMap[0], ProcessorMapSize, 0, ParallelDescriptor::CommunicatorInter());

        dm->define(ProcessorMap);
    }
}
#endif


std::ostream&
operator<< (std::ostream&              os,
            const DistributionMapping& pmap)
{
    os << "(DistributionMapping" << '\n';
    //
    // Do not print the sentinel value.
    //
    for (int i = 0; i < pmap.ProcessorMap().size() - 1; ++i)
    {
        os << "m_pmap[" << i << "] = " << pmap.ProcessorMap()[i] << '\n';
    }

    os << ')' << '\n';

    if (os.fail())
        BoxLib::Error("operator<<(ostream &, DistributionMapping &) failed");

    return os;
}
