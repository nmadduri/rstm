/**
 *  Copyright (C) 2011
 *  University of Rochester Department of Computer Science
 *    and
 *  Lehigh University Department of Computer Science and Engineering
 *
 * License: Modified BSD
 *          Please see the file LICENSE.RSTM for licensing information
 */

/**
 *  OrecFair Implementation
 *
 *    This STM is the reader-record variant of the Patient STM with starvation
 *    avoidance, from Spear et al. PPoPP 2009.
 *
 *    NB: this uses traditional TL2-style timestamps, instead of those from
 *        Wang et al. CGO 2007.
 *
 *    NB: This algorithm could cut a lot of latency if we made special
 *        versions of the read/write/commit functions to cover when the
 *        transaction does not have priority.
 */

#include "../../alt-license/rand_r_32.h"
#include "algs.hpp"
#include "../../include/abstract_timing.hpp"
#include "../cm.hpp"

namespace stm
{
  TM_FASTCALL void* OrecFairReadRO(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void* OrecFairReadRW(TX_FIRST_PARAMETER STM_READ_SIG(,));
  TM_FASTCALL void OrecFairWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void OrecFairWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
  TM_FASTCALL void OrecFairCommitRO(TX_LONE_PARAMETER);
  TM_FASTCALL void OrecFairCommitRW(TX_LONE_PARAMETER);
  NOINLINE void OrecFairValidate(TxThread*);
  NOINLINE void OrecFairValidateCommitTime(TxThread*);

  /**
   *  OrecFair begin:
   *
   *    When a transaction aborts, it releases its priority.  Here we
   *    re-acquire priority.
   */
  void OrecFairBegin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();
      tx->start_time = timestamp.val;
      // get priority
      long prio_bump = tx->consec_aborts / KARMA_FACTOR;
      if (prio_bump) {
          faiptr(&prioTxCount.val);
          tx->prio = prio_bump;
      }
  }

  /**
   *  OrecFair commit (read-only):
   *
   *    Read-only commits are easy... we just make sure to give up any priority
   *    we have.
   */
  void
  OrecFairCommitRO(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // If I had priority, release it
      if (tx->prio) {
          // decrease prio count
          faaptr(&prioTxCount.val, -1);

          // give up my priority
          tx->prio = 0;

          // clear metadata, reset list
          foreach (RRecList, i, tx->myRRecs)
              (*i)->unsetbit(tx->id-1);
          tx->myRRecs.reset();
      }
      tx->r_orecs.reset();
      OnROCommit(tx);
  }

  /**
   *  OrecFair commit (writing context):
   *
   *    This algorithm commits a transaction by first getting all locks, then
   *    checking if any lock conflicts with a higher-priority reader.  If there
   *    are no conflicts, then we commit, otherwise we self-abort.  Also, when
   *    acquiring locks, if we fail because a lower-priority transaction has the
   *    lock, we wait, because al writes are also reads, and thus we can simply
   *    wait for that thread to detect our conflict and abort itself.
   */
  void
  OrecFairCommitRW(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // try to lock every location in the write set
      WriteSet::iterator i = tx->writes.begin(), e = tx->writes.end();
      while (i != e) {
          // get orec, read its version#
          orec_t* o = get_orec(i->addr);
          id_version_t ivt;
          ivt.all = o->v.all;

          // if orec not locked, lock it.  for simplicity, abort if timestamp
          // too new.
          if (ivt.all <= tx->start_time) {
              if (!bcasptr(&o->v.all, ivt.all, tx->my_lock.all)) {
                  spin64();
                  continue;
              }
              // save old version to o->p, log lock
              o->p = ivt.all;
              tx->locks.insert(o);
          }
          // else if we don't hold the lock abort
          else if (ivt.all != tx->my_lock.all) {
              if (!ivt.fields.lock)
                  tmabort();
              // priority test... if I have priority, and the last unlocked
              // version of the orec was the one I read, and the current
              // owner has less priority than me, wait
              if (o->p <= tx->start_time) {
                  if (threads[ivt.fields.id-1]->prio < tx->prio) {
                      spin64();
                      continue;
                  }
              }
              tmabort();
          }
          ++i;
      }

      // fail if our writes conflict with a higher priority txn's reads
      if (prioTxCount.val > 0) {
          // \exist prio txns.  accumulate read bits covering addresses in my
          // write set
          rrec_t accumulator = {{0}};
          foreach (WriteSet, j, tx->writes) {
              int index = (((uintptr_t)j->addr) >> 3) % NUM_RRECS;
              accumulator |= rrecs[index];
          }

          // check the accumulator for bits that represent higher-priority
          // transactions
          for (unsigned slot = 0; slot < MAX_THREADS; ++slot) {
              unsigned bucket = slot / rrec_t::BITS;
              unsigned mask = 1lu<<(slot % rrec_t::BITS);
              if (accumulator.bits[bucket] & mask) {
                  if (threads[slot]->prio > tx->prio)
                      tmabort();
              }
          }
      }

      // increment the global timestamp if we have writes
      unsigned end_time = 1 + faiptr(&timestamp.val);

      // skip validation if nobody else committed
      if (end_time != (tx->start_time + 1))
          OrecFairValidateCommitTime(tx);

      // run the redo log
      tx->writes.writeback();

      // NB: if we did the faa, then released writelocks, then released
      //     readlocks, we might be faster

      // If I had priority, release it
      if (tx->prio) {
          // decrease prio count
          faaptr(&prioTxCount.val, -1);

          // give up my priority
          tx->prio = 0;

          // clear metadata, reset list
          foreach (RRecList, j, tx->myRRecs)
              (*j)->unsetbit(tx->id-1);
          tx->myRRecs.reset();
      }

      // release locks
      foreach (OrecList, j, tx->locks)
          (*j)->v.all = end_time;

      // remember that this was a commit
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      OnRWCommit(tx);
      ResetToRO(tx, OrecFairReadRO, OrecFairWriteRO, OrecFairCommitRO);
  }

  /**
   *  OrecFair read (read-only transaction)
   *
   *    This read is like OrecLazy, except that (1) we use traditional
   *    "check-twice" timestamps, and (2) if the caller has priority, it must
   *    mark the location before reading it.
   *
   *    NB: We could poll the 'set' bit first, which might afford some
   *        optimizations for priority transactions
   */
  void*
  OrecFairReadRO(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      // CM instrumentation
      if (tx->prio > 0) {
          // get the rrec for this address, set the bit, log it
          rrec_t* rrec = get_rrec(addr);
          rrec->setbit(tx->id-1);
          tx->myRRecs.insert(rrec);
      }

      // get the orec addr
      orec_t* o = get_orec(addr);
      while (true) {
          // read the orec BEFORE we read anything else
          id_version_t ivt;
          ivt.all = o->v.all;
          CFENCE;

          // read the location
          void* tmp = *addr;
          CFENCE;

          // re-read the orec
          unsigned ivt2 = o->v.all;

          // common case: new read to uncontended location
          if ((ivt.all == ivt2) && (ivt.all <= tx->start_time)) {
              tx->r_orecs.insert(o);
              return tmp;
          }

          // if lock held, spin and retry
          if (ivt.fields.lock) {
              yield_cpu();
              continue;
          }

          // unlocked but too new... validate and scale forward
          unsigned newts = timestamp.val;
          OrecFairValidate(tx);
          tx->start_time = newts;
      }
  }

  /**
   *  OrecFair read (writing transaction)
   *
   *    This read is like OrecLazy, except that (1) we use traditional
   *    "check-twice" timestamps, and (2) if the caller has priority, it must
   *    mark the location before reading it.
   *
   *    NB: As above, we could poll the 'set' bit if we had a priority-only
   *        version of this function
   */
  void*
  OrecFairReadRW(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // CM instrumentation
      if (tx->prio > 0) {
          // get the rrec for this address, set the bit, log it
          rrec_t* rrec = get_rrec(addr);
          rrec->setbit(tx->id-1);
          tx->myRRecs.insert(rrec);
      }

      // get the orec addr
      orec_t* o = get_orec(addr);
      while (true) {
          // read the orec BEFORE we read anything else
          id_version_t ivt;
          ivt.all = o->v.all;
          CFENCE;

          // read the location
          void* tmp = *addr;
          CFENCE;

          // re-read the orec
          unsigned ivt2 = o->v.all;

          // common case: new read to uncontended location
          if ((ivt.all == ivt2) && (ivt.all <= tx->start_time)) {
              tx->r_orecs.insert(o);
              // cleanup the value as late as possible.
              REDO_RAW_CLEANUP(tmp, found, log, mask);
              return tmp;
          }

          // if lock held, spin and retry
          if (ivt.fields.lock) {
              yield_cpu();
              continue;
          }

          // unlocked but too new... validate and scale forward
          unsigned newts = timestamp.val;
          OrecFairValidate(tx);
          tx->start_time = newts;
      }
  }

  /**
   *  OrecFair write (read-only context)
   *
   *    Every write is also a read.  Doing so makes commis much faster.
   *    However, it also means that writes have much more overhead than
   *    OrecLazy, especially when we have priority.
   *
   *    NB: We could use the rrec to know when we don't have to check the
   *        timestamp and scale.  Also, it looks like this mechanism has some
   *        redundancy with the checks in the lock acquisition code.
   */
  void
  OrecFairWriteRO(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // CM instrumentation
      if (tx->prio > 0) {
          // get the rrec for this address, set the bit, log it
          rrec_t* rrec = get_rrec(addr);
          rrec->setbit(tx->id-1);
          tx->myRRecs.insert(rrec);
      }

      // ensure that the orec isn't newer than we are... if so, validate
      orec_t* o = get_orec(addr);
      while (true) {
          // read the orec version number
          id_version_t ivt;
          ivt.all = o->v.all;
          // if locked, spin and continue
          if (!ivt.fields.lock) {
              // do we need to scale the start time?
              if (ivt.all > tx->start_time) {
                  unsigned newts = timestamp.val;
                  OrecFairValidate(tx);
                  tx->start_time = newts;
                  continue;
              }
              break;
          }
          spin64();
      }

      // Record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      OnFirstWrite(tx, OrecFairReadRW, OrecFairWriteRW, OrecFairCommitRW);
  }

  /**
   *  OrecFair write (writing context)
   *
   *    Same as the RO case, only without the switch at the end.  The same
   *    concerns apply as above.
   */
  void
  OrecFairWriteRW(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // CM instrumentation
      if (tx->prio > 0) {
          // get the rrec for this address, set the bit, log it
          rrec_t* rrec = get_rrec(addr);
          rrec->setbit(tx->id-1);
          tx->myRRecs.insert(rrec);
      }

      // ensure that the orec isn't newer than we are... if so, validate
      orec_t* o = get_orec(addr);
      while (true) {
          // read the orec version number
          id_version_t ivt;
          ivt.all = o->v.all;
          // if locked, spin and continue
          if (!ivt.fields.lock) {
              // do we need to scale the start time?
              if (ivt.all > tx->start_time) {
                  unsigned newts = timestamp.val;
                  OrecFairValidate(tx);
                  tx->start_time = newts;
                  continue;
              }
              break;
          }
          spin64();
      }

      // Record the new value in a redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  OrecFair unwinder:
   *
   *    To unwind, any rrecs or orecs that are marked must be unmarked.
   *
   *    NB: Unlike most of our algorithms, there is baked-in exponential
   *        backoff in this function, rather than deferring such backoff to a
   *        templated contention manager.  That is because we are trying to
   *        be completely faithful to [Spear PPoPP 2009]
   */
  void
  OrecFairRollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking
      // the branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // release the locks and restore version numbers
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = (*i)->p;

      // If I had priority, release it
      if (tx->prio > 0) {
          // give up my priority, unset all my read bits
          faaptr(&prioTxCount.val, -1);
          tx->prio = 0;
          foreach (RRecList, i, tx->myRRecs)
              (*i)->unsetbit(tx->id-1);
          tx->myRRecs.reset();
      }

      // undo memory operations, reset lists
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();

      // randomized exponential backoff
      exp_backoff(tx);

      PostRollback(tx);
      ResetToRO(tx, OrecFairReadRO, OrecFairWriteRO, OrecFairCommitRO);
  }

  /**
   *  OrecFair in-flight irrevocability: use abort-and-restart
   */
  bool OrecFairIrrevoc(TxThread*)
  {
      return false;
  }

  /**
   *  OrecFair validation
   *
   *    This is a lot like regular Orec validation, except that we must be
   *    ready for the possibility that someone with low priority grabbed a
   *    lock that we have an RRec on, in which case we just wait for them to
   *    go away, instead of aborting.
   */
  void
  OrecFairValidate(TxThread* tx)
  {
      OrecList::iterator i = tx->r_orecs.begin(), e = tx->r_orecs.end();
      while (i != e) {
          // read this orec
          id_version_t ivt;
          ivt.all = (*i)->v.all;
          // only a problem if locked or newer than start time
          if (ivt.all > tx->start_time) {
              if (!ivt.fields.lock)
                  tmabort();
              // priority test... if I have priority, and the last unlocked
              // orec was the one I read, and the current owner has less
              // priority than me, wait
              if ((*i)->p <= tx->start_time) {
                  if (threads[ivt.fields.id-1]->prio < tx->prio) {
                      spin64();
                      continue;
                  }
              }
              tmabort();
          }
          ++i;
      }
  }

  /**
   *  OrecFair validation (commit time)
   *
   *    This is a lot like the above code, except we need to handle when the
   *    caller holds locks
   */
  void
  OrecFairValidateCommitTime(TxThread* tx)
  {
      if (tx->prio) {
          OrecList::iterator i = tx->r_orecs.begin(), e = tx->r_orecs.end();
        while (i != e) {
              // read this orec
              id_version_t ivt;
              ivt.all = (*i)->v.all;
              // if unlocked and newer than start time, abort
              if (!ivt.fields.lock && (ivt.all > tx->start_time))
                  tmabort();

              // if locked and not by me, do a priority test
              if (ivt.fields.lock && (ivt.all != tx->my_lock.all)) {
                  // priority test... if I have priority, and the last
                  // unlocked orec was the one I read, and the current
                  // owner has less priority than me, wait
                  if (((*i)->p <= tx->start_time) &&
                      (threads[ivt.fields.id-1]->prio < tx->prio))
                  {
                      spin64();
                      continue;
                  }
                  tmabort();
              }
              ++i;
          }
      }
      else {
          foreach (OrecList, i, tx->r_orecs) {
              // read this orec
              id_version_t ivt;
              ivt.all = (*i)->v.all;
              // if unlocked and newer than start time, abort
              if ((ivt.all > tx->start_time) && (ivt.all != tx->my_lock.all))
                  tmabort();
          }
      }
  }

  /**
   *  Switch to OrecFair:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   */
  void
  OrecFairOnSwitchTo()
  {
      timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
  }

  /**
   *  OrecFair initialization
   */
  template<>
  void initTM<OrecFair>()
  {
      // set the name
      stms[OrecFair].name      = "OrecFair";

      // set the pointers
      stms[OrecFair].begin     = OrecFairBegin;
      stms[OrecFair].commit    = OrecFairCommitRO;
      stms[OrecFair].read      = OrecFairReadRO;
      stms[OrecFair].write     = OrecFairWriteRO;
      stms[OrecFair].rollback  = OrecFairRollback;
      stms[OrecFair].irrevoc   = OrecFairIrrevoc;
      stms[OrecFair].switcher  = OrecFairOnSwitchTo;
      stms[OrecFair].privatization_safe = false;
  }
}

#ifdef STM_ONESHOT_ALG_OrecFair
DECLARE_AS_ONESHOT_NORMAL(OrecFair)
#endif
