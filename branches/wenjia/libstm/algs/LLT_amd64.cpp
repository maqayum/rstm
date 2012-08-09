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
 * Like LLT, but we use the tick counter instead of a timestamp
 */

/**
 *  LLT_amd64 Implementation
 *
 *    This STM very closely resembles the GV1 variant of TL2.  That is, it uses
 *    orecs and lazy acquire.  Its clock requires everyone to increment it to
 *    commit writes, but this allows for read-set validation to be skipped at
 *    commit time.  Most importantly, there is no in-flight validation: if a
 *    timestamp is greater than when the transaction sampled the clock at begin
 *    time, the transaction aborts.
 */

#include "../profiling.hpp"
#include "../algs.hpp"
#include "../RedoRAWUtils.hpp"

using stm::TxThread;
using stm::WriteSet;
using stm::OrecList;
using stm::UNRECOVERABLE;
using stm::WriteSetEntry;
using stm::orec_t;
using stm::get_orec;


/**
 *  Declare the functions that we're going to implement, so that we can avoid
 *  circular dependencies.
 */
namespace {
  struct LLT_amd64
  {
      static void begin(TX_LONE_PARAMETER);
      static TM_FASTCALL void* read_ro(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void* read_rw(TX_FIRST_PARAMETER STM_READ_SIG(,));
      static TM_FASTCALL void write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(,,));
      static TM_FASTCALL void commit_ro(TX_LONE_PARAMETER);
      static TM_FASTCALL void commit_rw(TX_LONE_PARAMETER);

      static void rollback(STM_ROLLBACK_SIG(,,));
      static bool irrevoc(TxThread*);
      static void onSwitchTo();
      static NOINLINE void validate(TxThread*);
  };

  /**
   *  LLT_amd64 begin:
   */
  void LLT_amd64::begin(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      tx->allocator.onTxBegin();
      // get a start time
      tx->start_time = tickp();
  }

  /**
   *  LLT_amd64 commit (read-only):
   */
  void
  LLT_amd64::commit_ro(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // read-only, so just reset lists
      tx->r_orecs.reset();
      OnReadOnlyCommit(tx);
  }

  /**
   *  LLT_amd64 commit (writing context):
   *
   *    Get all locks, validate, do writeback.  Use the counter to avoid some
   *    validations.
   */
  void
  LLT_amd64::commit_rw(TX_LONE_PARAMETER)
  {
      TX_GET_TX_INTERNAL;
      // acquire locks
      foreach (WriteSet, i, tx->writes) {
          // get orec, read its version#
          orec_t* o = get_orec(i->addr);
          uintptr_t ivt = o->v.all;

          // lock all orecs, unless already locked
          if (ivt <= tx->start_time) {
              // abort if cannot acquire
              if (!bcasptr(&o->v.all, ivt, tx->my_lock.all))
                  stm::tmabort();
              // save old version to o->p, remember that we hold the lock
              o->p = ivt;
              tx->locks.insert(o);
          }
          // else if we don't hold the lock abort
          else if (ivt != tx->my_lock.all) {
              stm::tmabort();
          }
      }

      // increment the global timestamp since we have writes
      uintptr_t end_time = tickp();

      // validate
      validate(tx);

      // run the redo log
      tx->writes.writeback();

      // release locks
      CFENCE;
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = end_time;

      // clean-up
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      OnReadWriteCommit(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  LLT_amd64 read (read-only transaction)
   *
   *    We use "check twice" timestamps in LLT_amd64
   */
  void*
  LLT_amd64::read_ro(TX_FIRST_PARAMETER STM_READ_SIG(addr,))
  {
      TX_GET_TX_INTERNAL;
      // get the orec addr
      orec_t* o = get_orec(addr);

      // read orec, then val, then orec
      uintptr_t ivt = o->v.all;
      CFENCE;
      void* tmp = *addr;
      CFENCE;
      uintptr_t ivt2 = o->v.all;
      // if orec never changed, and isn't too new, the read is valid
      if ((ivt <= tx->start_time) && (ivt == ivt2)) {
          // log orec, return the value
          tx->r_orecs.insert(o);
          return tmp;
      }
      // unreachable
      stm::tmabort();
      return NULL;
  }

  /**
   *  LLT_amd64 read (writing transaction)
   */
  void*
  LLT_amd64::read_rw(TX_FIRST_PARAMETER STM_READ_SIG(addr,mask))
  {
      TX_GET_TX_INTERNAL;
      // check the log for a RAW hazard, we expect to miss
      WriteSetEntry log(STM_WRITE_SET_ENTRY(addr, NULL, mask));
      bool found = tx->writes.find(log);
      REDO_RAW_CHECK(found, log, mask);

      // get the orec addr
      orec_t* o = get_orec(addr);

      // read orec, then val, then orec
      uintptr_t ivt = o->v.all;
      CFENCE;
      void* tmp = *addr;
      CFENCE;
      uintptr_t ivt2 = o->v.all;

      // fixup is here to minimize the postvalidation orec read latency
      REDO_RAW_CLEANUP(tmp, found, log, mask);
      // if orec never changed, and isn't too new, the read is valid
      if ((ivt <= tx->start_time) && (ivt == ivt2)) {
          // log orec, return the value
          tx->r_orecs.insert(o);
          return tmp;
      }
      stm::tmabort();
      // unreachable
      return NULL;
  }

  /**
   *  LLT_amd64 write (read-only context)
   */
  void
  LLT_amd64::write_ro(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // add to redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
      stm::OnFirstWrite(tx, read_rw, write_rw, commit_rw);
  }

  /**
   *  LLT_amd64 write (writing context)
   */
  void
  LLT_amd64::write_rw(TX_FIRST_PARAMETER STM_WRITE_SIG(addr,val,mask))
  {
      TX_GET_TX_INTERNAL;
      // add to redo log
      tx->writes.insert(WriteSetEntry(STM_WRITE_SET_ENTRY(addr, val, mask)));
  }

  /**
   *  LLT_amd64 unwinder:
   */
  void
  LLT_amd64::rollback(STM_ROLLBACK_SIG(tx, except, len))
  {
      PreRollback(tx);

      // Perform writes to the exception object if there were any... taking the
      // branch overhead without concern because we're not worried about
      // rollback overheads.
      STM_ROLLBACK(tx->writes, except, len);

      // release the locks and restore version numbers
      foreach (OrecList, i, tx->locks)
          (*i)->v.all = (*i)->p;

      // undo memory operations, reset lists
      tx->r_orecs.reset();
      tx->writes.reset();
      tx->locks.reset();
      PostRollback(tx, read_ro, write_ro, commit_ro);
  }

  /**
   *  LLT_amd64 in-flight irrevocability:
   */
  bool
  LLT_amd64::irrevoc(TxThread*)
  {
      return false;
  }

  /**
   *  LLT_amd64 validation
   */
  void
  LLT_amd64::validate(TxThread* tx)
  {
      // validate
      foreach (OrecList, i, tx->r_orecs) {
          uintptr_t ivt = (*i)->v.all;
          // if unlocked and newer than start time, abort
          if ((ivt > tx->start_time) && (ivt != tx->my_lock.all))
              stm::tmabort();
      }
  }

  /**
   *  Switch to LLT_amd64:
   *
   *    The timestamp must be >= the maximum value of any orec.  Some algs use
   *    timestamp as a zero-one mutex.  If they do, then they back up the
   *    timestamp first, in timestamp_max.
   */
  void
  LLT_amd64::onSwitchTo()
  {
      // timestamp.val = MAXIMUM(timestamp.val, timestamp_max.val);
  }
}

namespace stm {
  /**
   *  LLT_amd64 initialization
   */
  template<>
  void initTM<LLT_amd64>()
  {
      // set the name
      stms[LLT_amd64].name      = "LLT_amd64";

      // set the pointers
      stms[LLT_amd64].begin     = ::LLT_amd64::begin;
      stms[LLT_amd64].commit    = ::LLT_amd64::commit_ro;
      stms[LLT_amd64].read      = ::LLT_amd64::read_ro;
      stms[LLT_amd64].write     = ::LLT_amd64::write_ro;
      stms[LLT_amd64].rollback  = ::LLT_amd64::rollback;
      stms[LLT_amd64].irrevoc   = ::LLT_amd64::irrevoc;
      stms[LLT_amd64].switcher  = ::LLT_amd64::onSwitchTo;
      stms[LLT_amd64].privatization_safe = false;
  }
}

#ifdef STM_ONESHOT_ALG_LLT_amd64
DECLARE_AS_ONESHOT_NORMAL(LLT_amd64)
#endif
