// Native disk-backed FULL OUTER iterator. Planner/binder opt-in is separate.
#ifndef SQL_ITERATORS_SDP_FULL_OUTER_ITERATOR_H
#define SQL_ITERATORS_SDP_FULL_OUTER_ITERATOR_H
#include <optional>
#include <utility>
#include "sql/item.h"
#include "sql/iterators/row_iterator.h"
#include "sql/iterators/hash_join_chunk.h"
#include "sql/sdp_full_outer_driver.h"
#include "sql/sql_class.h"
#include "sql/pack_rows.h"
#include "sql_string.h"

// Evaluate each source once, each candidate pair once, and preserve every
// duplicate and SQL NULL via native packed TABLE records. Two native temp chunks
// rotate while accumulating right-row match flags; no unbounded in-memory row
// list and no second execution of an ON predicate in a UNION/anti-join branch.
class Sdp_full_outer_iterator final : public RowIterator {
  friend class Sdp_full_outer_driver<Sdp_full_outer_iterator>;
  unique_ptr_destroy_only<RowIterator> m_left,m_right;
  pack_rows::TableCollection m_left_tables,m_right_tables;
  Item *m_condition;
  std::optional<HashJoinChunk> m_a,m_b;
  HashJoinChunk *m_input{nullptr},*m_output{nullptr};
  // Loaded BLOB fields can point into read_buffer. Packing into that same buffer
  // would overwrite a field before copying it, so the write buffer is distinct.
  String m_read_buffer,m_write_buffer;
  Sdp_full_outer_driver<Sdp_full_outer_iterator> m_driver{this};
 public:
  Sdp_full_outer_iterator(THD *thd,unique_ptr_destroy_only<RowIterator> left,
      unique_ptr_destroy_only<RowIterator> right,pack_rows::TableCollection left_tables,
      pack_rows::TableCollection right_tables,Item *condition)
      :RowIterator(thd),m_left(std::move(left)),m_right(std::move(right)),
       m_left_tables(std::move(left_tables)),m_right_tables(std::move(right_tables)),m_condition(condition) {}
  void SetNullRowFlag(bool null) override {m_left->SetNullRowFlag(null);m_right->SetNullRowFlag(null);}
  void EndPSIBatchModeIfStarted() override {
    m_left->EndPSIBatchModeIfStarted();m_right->EndPSIBatchModeIfStarted();
  }
  void UnlockRow() override {
    // As in HashJoinIterator, a restored cached row cannot safely unlock the
    // original handler's current row. Locking SELECT support needs its own gate.
  }
 private:
  bool DoInit() override {return m_driver.init();}
  int DoRead() override {return m_driver.read();}
  bool interrupted() {
    if (!thd()->killed) return false;
    thd()->send_kill_message();return true;
  }
  bool initialize() {
    finish();SetNullRowFlag(false);
    if (!m_right_tables.has_blob_column()) {
      const size_t bytes= pack_rows::ComputeRowSizeUpperBoundSansBlobs(m_right_tables);
      m_write_buffer.length(0);
      if (m_write_buffer.reserve(bytes)) {
        my_error(ER_OUTOFMEMORY,MYF(ME_FATALERROR),bytes);
        return true;
      }
    }
    m_a.emplace();m_b.emplace();m_input=&*m_a;m_output=&*m_b;
    m_right_tables.PrepareForRequestRowId();m_left_tables.PrepareForRequestRowId();
    // Initialize/materialize both inputs before reading a base-table row.
    // A left Knowledge source takes locking catalog reads during Init();
    // starting the right base scan first would classify the shared InnoDB
    // transaction as autocommit-nonlocking before adding those catalog locks.
    if (m_input->Init(m_right_tables,true) || m_left->Init() || m_right->Init())
      return true;
    for (;;) {
      if (interrupted()) return true;
      m_right->SetNullRowFlag(false);
      const int error=m_right->Read();
      if (error>0) return true;
      if (error<0) break;
      m_right_tables.RequestRowId();
      if (m_input->WriteRowToChunk(&m_write_buffer,false)) return true;
    }
    m_right->EndPSIBatchModeIfStarted();
    return false;
  }
  int read_left() {
    SetNullRowFlag(false);
    const int result=m_left->Read();
    if (!result) m_left_tables.RequestRowId();
    return result;
  }
  uint64_t right_rows() const {return m_input->NumRows();}
  bool start_scan() {return m_input->Rewind() || m_output->Init(m_right_tables,true);}
  bool read_right(bool *matched) {
    m_right->SetNullRowFlag(false);
    return m_input->LoadRowFromChunk(&m_read_buffer,matched);
  }
  bool matches(bool *matched) {
    *matched=m_condition==nullptr || m_condition->val_bool();
    return thd()->is_error();
  }
  bool write_right(bool matched) {return m_output->WriteRowToChunk(&m_write_buffer,matched);}
  void end_scan() {std::swap(m_input,m_output);}
  bool start_unmatched() {return m_input->Rewind();}
  void null_right() {m_right->SetNullRowFlag(true);}
  void null_left() {m_left->SetNullRowFlag(true);}
  void finish() {
    m_a.reset();m_b.reset();m_input=nullptr;m_output=nullptr;
    m_left->EndPSIBatchModeIfStarted();m_right->EndPSIBatchModeIfStarted();
  }
};
#endif
