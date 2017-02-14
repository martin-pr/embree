// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "bvh.h"
#include "../geometry/primitive.h"
#include "../builders/bvh_builder_msmblur.h"
#include "../builders/heuristic_binning_array_aligned.h"
#include "../builders/heuristic_binning_array_unaligned.h"
#include "../builders/heuristic_timesplit_array.h"

namespace embree
{
  namespace isa
  {
    struct BVHMBuilderHairMSMBlur
    {
      /*! settings for msmblur builder */
      struct Settings
      {
        /*! default settings */
        Settings () 
        : branchingFactor(2), maxDepth(32), logBlockSize(0), minLeafSize(1), maxLeafSize(8) {}

      public:
        size_t branchingFactor;  //!< branching factor of BVH to build
        size_t maxDepth;         //!< maximal depth of BVH to build
        size_t logBlockSize;     //!< log2 of blocksize for SAH heuristic
        size_t minLeafSize;      //!< minimal size of a leaf
        size_t maxLeafSize;      //!< maximal size of a leaf
      };

      struct BuildRecord
      {
      public:
	__forceinline BuildRecord () {}
        
        __forceinline BuildRecord (size_t depth) 
          : depth(depth) {}
        
        __forceinline BuildRecord (const SetMB& prims, size_t depth = 0) 
          : depth(depth), prims(prims) {}
        
        __forceinline size_t size() const { 
          return prims.object_range.size(); 
        }
        
      public:
	size_t depth;       //!< depth of the root of this subtree
	SetMB prims;        //!< the list of primitives
      };
      
      template<typename NodeRef,
        typename RecalculatePrimRef, 
        typename CreateAllocFunc,
        typename CreateAlignedNodeFunc, 
        typename UpdateAlignedNodeFunc, 
        typename CreateUnalignedNodeFunc, 
        typename UpdateUnalignedNodeFunc, 
        typename CreateAlignedNode4DFunc,
        typename UpdateAlignedNode4DFunc,
        typename CreateLeafFunc, 
        typename ProgressMonitor>
        
        class BuilderT : private Settings
      {
        ALIGNED_CLASS;
        
        static const size_t MAX_BRANCHING_FACTOR =  8;         //!< maximal supported BVH branching factor
        static const size_t MIN_LARGE_LEAF_LEVELS = 8;         //!< create balanced tree if we are that many levels before the maximal tree depth
        static const size_t SINGLE_THREADED_THRESHOLD = 4096;  //!< threshold to switch to single threaded build
        
        static const size_t travCostAligned = 1;
        static const size_t travCostUnaligned = 5;
        static const size_t intCost = 6;
        
        typedef FastAllocator::ThreadLocal2* Allocator;
        typedef SharedVector<mvector<PrimRefMB>> SharedPrimRefVector;
        typedef LocalChildListT<BuildRecord,MAX_BRANCHING_FACTOR> LocalChildList;
        
        typedef HeuristicMBlurTemporalSplit<PrimRefMB,RecalculatePrimRef,NUM_TEMPORAL_BINS> HeuristicTemporal;
        typedef HeuristicArrayBinningMB<PrimRefMB,NUM_OBJECT_BINS> HeuristicBinning;
        typedef UnalignedHeuristicArrayBinningMB<PrimRefMB,NUM_OBJECT_BINS> UnalignedHeuristicBinning;
        
      public:
        
        BuilderT (Scene* scene,
                  const RecalculatePrimRef& recalculatePrimRef,
                  const CreateAllocFunc& createAlloc, 
                  const CreateAlignedNodeFunc& createAlignedNode, 
                  const UpdateAlignedNodeFunc& updateAlignedNode, 
                  const CreateUnalignedNodeFunc& createUnalignedNode, 
                  const UpdateUnalignedNodeFunc& updateUnalignedNode, 
                  const CreateAlignedNode4DFunc& createAlignedNode4D, 
                  const UpdateAlignedNode4DFunc& updateAlignedNode4D, 
                  const CreateLeafFunc& createLeaf,
                  const ProgressMonitor& progressMonitor,
                  const Settings settings)
          : Settings(settings), 
          scene(scene),
          recalculatePrimRef(recalculatePrimRef),
          createAlloc(createAlloc), 
          createAlignedNode(createAlignedNode), updateAlignedNode(updateAlignedNode), 
          createUnalignedNode(createUnalignedNode), updateUnalignedNode(updateUnalignedNode), 
          createAlignedNode4D(createAlignedNode4D), updateAlignedNode4D(updateAlignedNode4D),
          createLeaf(createLeaf),
          progressMonitor(progressMonitor),
          unalignedHeuristic(scene),
          temporalSplitHeuristic(scene->device,recalculatePrimRef) {}
        
        /*! entry point into builder */
        std::pair<NodeRef,LBBox3fa> operator() (mvector<PrimRefMB>& prims, const PrimInfoMB& pinfo) 
        {
          BuildRecord record(SetMB(pinfo,&prims),1);
          auto root = recurse(record,nullptr,true);
          _mm_mfence(); // to allow non-temporal stores during build
          return root;
        }
        
      private:
        
        void splitFallback(const SetMB& set, SetMB& lset, SetMB& rset)
        {
          mvector<PrimRefMB>& prims = *set.prims;
          
          const size_t begin = set.object_range.begin();
          const size_t end   = set.object_range.end();
          const size_t center = (begin + end)/2;
          
          PrimInfoMB linfo = empty;
          for (size_t i=begin; i<center; i++)
            linfo.add_primref(prims[i]);
          
          PrimInfoMB rinfo = empty;
          for (size_t i=center; i<end; i++)
            rinfo.add_primref(prims[i]);	
          
          new (&lset) SetMB(linfo,set.prims,range<size_t>(begin,center),set.time_range);
          new (&rset) SetMB(rinfo,set.prims,range<size_t>(center,end  ),set.time_range);
        }
        
        /*! creates a large leaf that could be larger than supported by the BVH */
        std::pair<NodeRef,LBBox3fa> createLargeLeaf(BuildRecord& current, Allocator alloc)
        {
          /* this should never occur but is a fatal error */
          if (current.depth > maxDepth) 
            throw_RTCError(RTC_UNKNOWN_ERROR,"depth limit reached");
          
          /* create leaf for few primitives */
          if (current.size() <= maxLeafSize)
            return createLeaf(current.prims,alloc);
          
          /* fill all children by always splitting the largest one */
          LocalChildList children(current);
          
          do {
            
            /* find best child with largest bounding box area */
            int bestChild = -1;
            size_t bestSize = 0;
            for (unsigned i=0; i<children.size(); i++)
            {
              /* ignore leaves as they cannot get split */
              if (children[i].size() <= maxLeafSize)
                continue;
              
              /* remember child with largest size */
              if (children[i].size() > bestSize) { 
                bestSize = children[i].size();
                bestChild = i;
              }
            }
            if (bestChild == -1) break;
            
            /*! split best child into left and right child */
            BuildRecord left(current.depth+1);
            BuildRecord right(current.depth+1);
            splitFallback(children[bestChild].prims,left.prims,right.prims);
            children.split(bestChild,left,right,nullptr);
            
          } while (children.size() < branchingFactor);
          
          /* create node */
          NodeRef node = createAlignedNode(alloc);

          LBBox3fa bounds = empty;
          for (size_t i=0; i<children.size(); i++) {
            const LBBox3fa cbounds = children[i].prims.linearBounds(recalculatePrimRef);
            const auto child = createLargeLeaf(children[i],alloc);
            bounds.extend(cbounds);
            updateAlignedNode(node,i,std::make_tuple(child.first,cbounds,children[i].prims.time_range));
          }

          return std::make_pair(node,bounds);
        }
        
        /*! performs split */
        std::unique_ptr<mvector<PrimRefMB>> split(const BuildRecord& current, BuildRecord& lrecord, BuildRecord& rrecord, bool& aligned, bool& timesplit)
        {
          /* variable to track the SAH of the best splitting approach */
          float bestSAH = inf;
          const float leafSAH = intCost*float(current.prims.num_time_segments)*current.prims.halfArea();
          
          /* perform standard binning in aligned space */
          HeuristicBinning::Split alignedObjectSplit;
          alignedObjectSplit = alignedHeuristic.find(current.prims,0);
          float alignedObjectSAH = travCostAligned*current.prims.halfArea() + intCost*alignedObjectSplit.splitSAH();
          bestSAH = min(alignedObjectSAH,bestSAH);
          
          /* perform standard binning in unaligned space */
          UnalignedHeuristicBinning::Split unalignedObjectSplit;
          LinearSpace3fa uspace;
          float unalignedObjectSAH = inf;
          if (alignedObjectSAH > 0.7f*leafSAH) {
            uspace = unalignedHeuristic.computeAlignedSpaceMB(scene,current.prims); 
            const SetMB sset = unalignedHeuristic.computePrimInfoMB(scene,current.prims,uspace);
            unalignedObjectSplit = unalignedHeuristic.find(sset,0,uspace);    	
            unalignedObjectSAH = travCostUnaligned*current.prims.halfArea() + intCost*unalignedObjectSplit.splitSAH();
            bestSAH = min(unalignedObjectSAH,bestSAH);
          }
          
          /* do temporal splits only if the the time range is big enough */
          float temporal_split_sah = inf;
          typename HeuristicTemporal::Split temporal_split;
          if (current.prims.time_range.size() > 1.01f/float(current.prims.max_num_time_segments)) {
            temporal_split = temporalSplitHeuristic.find(current.prims, 0);
            temporal_split_sah = travCostAligned*current.prims.halfArea() + intCost*temporal_split.splitSAH();
            bestSAH = min(temporal_split_sah,bestSAH);
          }
          
          /* perform fallback split */
          if (!std::isfinite(bestSAH))
          {
            current.prims.deterministic_order();
            splitFallback(current.prims,lrecord.prims,rrecord.prims);
            return nullptr;
          }
          else if (bestSAH == temporal_split_sah) {
            timesplit = true;
            return temporalSplitHeuristic.split(temporal_split,current.prims,lrecord.prims,rrecord.prims);
          }
          /* perform aligned split if this is best */
          else if (bestSAH == alignedObjectSAH) {
            alignedHeuristic.split(alignedObjectSplit,current.prims,lrecord.prims,rrecord.prims);
            return nullptr;
          }
          /* perform unaligned split if this is best */
          else if (bestSAH == unalignedObjectSAH) {
            unalignedHeuristic.split(unalignedObjectSplit,uspace,current.prims,lrecord.prims,rrecord.prims);
            aligned = false;
            return nullptr;
          }
          /* otherwise perform fallback split */
          else {
            current.prims.deterministic_order();
            splitFallback(current.prims,lrecord.prims,rrecord.prims);
            return nullptr;
          }
        }
        
        /*! recursive build */
        std::pair<NodeRef,LBBox3fa> recurse(BuildRecord& current, Allocator alloc, bool toplevel)
        {
          /* get thread local allocator */
          if (alloc == nullptr) 
            alloc = createAlloc();
          
          /* call memory monitor function to signal progress */
          if (toplevel && current.size() <= SINGLE_THREADED_THRESHOLD)
            progressMonitor(current.size());
          
          /* create leaf node */
          if (current.depth+MIN_LARGE_LEAF_LEVELS >= maxDepth || current.size() <= minLeafSize) {
            current.prims.deterministic_order();
            return createLargeLeaf(current,alloc);
          }
          
          /* fill all children by always splitting the one with the largest surface area */
          LocalChildList children(current);
          bool aligned = true;
          bool timesplit = false;
          
          do {
            
            /* find best child with largest bounding box area */
            ssize_t bestChild = -1;
            float bestArea = neg_inf;
            for (size_t i=0; i<children.size(); i++)
            {
              /* ignore leaves as they cannot get split */
              if (children[i].size() <= minLeafSize)
                continue;
              
              /* remember child with largest area */
              const float A = children[i].prims.halfArea();
              if (A > bestArea) { 
                bestArea = children[i].prims.halfArea();
                bestChild = i;
              }
            }
            if (bestChild == -1) break;
            
            /*! split best child into left and right child */
            BuildRecord left(current.depth+1);
            BuildRecord right(current.depth+1);
            std::unique_ptr<mvector<PrimRefMB>> new_vector = split(children[bestChild],left,right,aligned,timesplit);
            children.split(bestChild,left,right,std::move(new_vector));
            
          } while (children.size() < branchingFactor); 
          assert(children.size() > 1);
          
          /* create time split node */
          if (timesplit)
          {
            const NodeRef node = createAlignedNode4D(alloc);

            LBBox3fa cbounds[MAX_BRANCHING_FACTOR];
            for (size_t i=0; i<children.size(); i++)
              cbounds[i] = children[i].prims.linearBounds(recalculatePrimRef);
            
            /* spawn tasks or ... */
            if (current.size() > SINGLE_THREADED_THRESHOLD)
            {
              parallel_for(size_t(0), children.size(), [&] (const range<size_t>& r) {
                  for (size_t i=r.begin(); i<r.end(); i++) {
                    const auto child = recurse(children[i],nullptr,true);
                    updateAlignedNode4D(node,i,std::make_tuple(child.first,cbounds[i],children[i].prims.time_range)); 
                    _mm_mfence(); // to allow non-temporal stores during build
                  }                
                });
            }
            /* ... continue sequential */
            else {
              for (size_t i=0; i<children.size(); i++) {
                const auto child = recurse(children[i],alloc,false);
                updateAlignedNode4D(node,i,std::make_tuple(child.first,cbounds[i],children[i].prims.time_range));
              }
            }

            const LBBox3fa bounds = current.prims.linearBounds(recalculatePrimRef);
            return std::make_pair(node,bounds);
          }
          
          /* create aligned node */
          else if (aligned) 
          {
            const NodeRef node = createAlignedNode(alloc);
            
            LBBox3fa cbounds[MAX_BRANCHING_FACTOR];
            for (size_t i=0; i<children.size(); i++)
              cbounds[i] = children[i].prims.linearBounds(recalculatePrimRef);
            
            /* spawn tasks or ... */
            if (current.size() > SINGLE_THREADED_THRESHOLD)
            {
              parallel_for(size_t(0), children.size(), [&] (const range<size_t>& r) {
                  for (size_t i=r.begin(); i<r.end(); i++) {
                    const auto child = recurse(children[i],nullptr,true);
                    updateAlignedNode(node,i,std::make_tuple(child.first,cbounds[i],children[i].prims.time_range)); 
                    _mm_mfence(); // to allow non-temporal stores during build
                  }                
                });
            }
            /* ... continue sequential */
            else {
              for (size_t i=0; i<children.size(); i++) {
                const auto child = recurse(children[i],alloc,false);
                updateAlignedNode(node,i,std::make_tuple(child.first,cbounds[i],children[i].prims.time_range));
              }
            }

            const LBBox3fa bounds = current.prims.linearBounds(recalculatePrimRef);
            return std::make_pair(node,bounds);
          }
          
          /* create unaligned node */
          else 
          {
            const NodeRef node = createUnalignedNode(alloc);

            /* spawn tasks or ... */
            if (current.size() > SINGLE_THREADED_THRESHOLD)
            {              
              parallel_for(size_t(0), children.size(), [&] (const range<size_t>& r) {
                  for (size_t i=r.begin(); i<r.end(); i++) {
                    const LinearSpace3fa space = unalignedHeuristic.computeAlignedSpaceMB(scene,children[i].prims); 
                    const LBBox3fa lbounds = unalignedHeuristic.linearBounds(scene,children[i].prims,space);
                    const auto child = recurse(children[i],nullptr,true);
                    updateUnalignedNode(node,i,child.first,space,lbounds,children[i].prims.time_range);
                    _mm_mfence(); // to allow non-temporal stores during build
                  }                
                });
            }
            /* ... continue sequentially */
            else
            {
              for (size_t i=0; i<children.size(); i++) {
                const LinearSpace3fa space = unalignedHeuristic.computeAlignedSpaceMB(scene,children[i].prims); 
                const LBBox3fa lbounds = unalignedHeuristic.linearBounds(scene,children[i].prims,space);
                const auto child = recurse(children[i],alloc,false);
                updateUnalignedNode(node,i,child.first,space,lbounds,children[i].prims.time_range);
              }
            }

            const LBBox3fa bounds = current.prims.linearBounds(recalculatePrimRef);
            return std::make_pair(node,bounds);
          }
        }
        
      private:      
        Scene* scene;
        const RecalculatePrimRef& recalculatePrimRef;
        const CreateAllocFunc& createAlloc;
        const CreateAlignedNodeFunc& createAlignedNode;
        const UpdateAlignedNodeFunc& updateAlignedNode;
        const CreateUnalignedNodeFunc& createUnalignedNode;
        const UpdateUnalignedNodeFunc& updateUnalignedNode;
        const CreateAlignedNode4DFunc& createAlignedNode4D;
        const UpdateAlignedNode4DFunc& updateAlignedNode4D;
        const CreateLeafFunc& createLeaf;
        const ProgressMonitor& progressMonitor;
        
      private:
        HeuristicBinning alignedHeuristic;
        UnalignedHeuristicBinning unalignedHeuristic;
        HeuristicTemporal temporalSplitHeuristic;
        };
      
        template<typename NodeRef,
        typename RecalculatePrimRef,
        typename CreateAllocFunc,
        typename CreateAlignedNodeFunc, 
        typename UpdateAlignedNodeFunc, 
        typename CreateUnalignedNodeFunc, 
        typename UpdateUnalignedNodeFunc, 
        typename CreateAlignedNode4DFunc, 
        typename UpdateAlignedNode4DFunc, 
        typename CreateLeafFunc, 
        typename ProgressMonitor>
        
          static std::pair<NodeRef,LBBox3fa> build (Scene* scene, mvector<PrimRefMB>& prims, const PrimInfoMB& pinfo,
                                                    const RecalculatePrimRef& recalculatePrimRef,
                                                    const CreateAllocFunc& createAlloc,
                                                    const CreateAlignedNodeFunc& createAlignedNode, 
                                                    const UpdateAlignedNodeFunc& updateAlignedNode, 
                                                    const CreateUnalignedNodeFunc& createUnalignedNode, 
                                                    const UpdateUnalignedNodeFunc& updateUnalignedNode, 
                                                    const CreateAlignedNode4DFunc& createAlignedNode4D, 
                                                    const UpdateAlignedNode4DFunc& updateAlignedNode4D, 
                                                    const CreateLeafFunc& createLeaf, 
                                                    const ProgressMonitor& progressMonitor,
                                                    const Settings settings)
      {
        typedef BuilderT<NodeRef,RecalculatePrimRef,CreateAllocFunc,
          CreateAlignedNodeFunc,UpdateAlignedNodeFunc,
          CreateUnalignedNodeFunc,UpdateUnalignedNodeFunc,
          CreateAlignedNode4DFunc,UpdateAlignedNode4DFunc,
          CreateLeafFunc,ProgressMonitor> Builder;
        Builder builder(scene,recalculatePrimRef,createAlloc,createAlignedNode,updateAlignedNode,createUnalignedNode,updateUnalignedNode,createAlignedNode4D,updateAlignedNode4D,createLeaf,progressMonitor,settings);
        return builder(prims,pinfo);
      }
    };
  }
}
