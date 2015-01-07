// ======================================================================== //
// Copyright 2009-2014 Intel Corporation                                    //
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

#include "range.h"
#include "builders/workstack.h"

namespace embree
{
  template<typename Continuation>
  class ParallelContinue {
  public:
    virtual void operator() (const Continuation& c) = 0;
  };

  template<typename Continuation, typename Index, typename Func>
    class ParallelContinueTask
  {
    static const size_t SIZE_WORK_STACK = 64;

  public:
    __forceinline ParallelContinueTask (Continuation* continuations, const Index taskCount, const Func& func)
      : continuations(continuations), cntr(0), taskCount(taskCount), func(func)
    {
      LockStepTaskScheduler* scheduler = LockStepTaskScheduler::instance();
      size_t threadCount = scheduler->getNumThreads();
      threadStack = new WorkStack<Continuation,SIZE_WORK_STACK>[threadCount]; 
      scheduler->dispatchTask(_task,this);
      delete[] threadStack;
    }

    void task(const size_t threadIndex, const size_t threadCount) 
    {
      struct Recurse : public ParallelContinue<Continuation>
      {
        ParallelContinueTask* parent;
        __forceinline Recurse(ParallelContinueTask* parent) : parent(parent) {}
        void operator() (const Continuation& c) { parent->func(c,*this); } 
      };
    
      struct Select : public ParallelContinue<Continuation>
      {
        ParallelContinueTask* parent;
        __forceinline Select(ParallelContinueTask* parent) : parent(parent) {}
        void operator() (const Continuation& c) 
        {
          const size_t threadIndex = LockStepTaskScheduler::threadIndex();
          if (c.final() || !parent->threadStack[threadIndex].push(c)) {
            Recurse r(parent); r(c); 
          }
        }
      };

      while (true) 
      {
        Continuation cont;
        Index taskIndex = atomic_add(&cntr,1);
        if (taskIndex < taskCount) 
          cont = continuations[taskIndex];

        /* global work queue empty => try to steal from neighboring queues */	 
        else
        {
          bool success = false;
          for (size_t i=0; i<threadCount; i++)
          {
            if (threadStack[(threadIndex+i)%threadCount].pop(cont)) {
              success = true;
              break;
            }
          }
          /* found nothing to steal ? */
          if (!success) return; // FIXME: may loose threads
        }

        Select select(this); func(cont,select);
	while (threadStack[threadIndex].pop(cont)) {
          Select select(this); func(cont,select);
        }
      }
    }

    static void _task(void* data, const size_t threadIndex, const size_t threadCount) {
      ((ParallelContinueTask*)data)->task(threadIndex,threadCount);
    }
    
  private:
    Continuation* continuations;
    atomic_t cntr;
    Index taskCount;
    const Func& func;
  private:
    __aligned(64) WorkStack<Continuation,SIZE_WORK_STACK>* threadStack;
  };
  
  /* parallel continue */
  template<typename Continuation, typename Index, typename Func>
    __forceinline void parallel_continue( Continuation* continuations, const Index N, const Func& func)
  {
    ParallelContinueTask<Continuation,Index,Func> task(continuations,N,func);
  }
}
