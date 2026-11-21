#ifndef SQL_SDP_FULL_OUTER_DRIVER_H
#define SQL_SDP_FULL_OUTER_DRIVER_H
#include <cstdint>

// Shared state machine for the native iterator and its deterministic I/O tests.
// Right rows are physical row instances, each with its own persistent match bit.
// No key, NULL sentinel or DISTINCT step identifies or collapses an input row.
template<class Adapter> class Sdp_full_outer_driver {
  enum class State {left,scan,unmatched,done};
  Adapter *m_io;
  State m_state{State::done};
  uint64_t m_row{0};
  bool m_left_matched{false};
  int fail() {m_io->finish();m_state=State::done;return 1;}
 public:
  explicit Sdp_full_outer_driver(Adapter *io):m_io(io) {}
  bool init() {
    m_state=State::done;m_row=0;m_left_matched=false;
    if (m_io->initialize()) {m_io->finish();return true;}
    m_state=State::left;return false;
  }
  int read() {
    for (;;) {
      if (m_state==State::done) return -1;
      if (m_io->interrupted()) return fail();
      switch (m_state) {
        case State::left: {
          const int error=m_io->read_left();
          if (error>0) return fail();
          if (error<0) {
            if (m_io->start_unmatched()) return fail();
            m_row=0;m_state=State::unmatched;continue;
          }
          m_left_matched=false;
          if (m_io->right_rows()==0) {m_io->null_right();return 0;}
          if (m_io->start_scan()) return fail();
          m_row=0;m_state=State::scan;continue;
        }
        case State::scan: {
          if (m_row==m_io->right_rows()) {
            m_io->end_scan();m_state=State::left;
            if (!m_left_matched) {m_io->null_right();return 0;}
            continue;
          }
          bool previous=false,matched=false;
          if (m_io->read_right(&previous) || m_io->matches(&matched)) return fail();
          ++m_row;
          if (m_io->write_right(previous || matched)) return fail();
          if (matched) {m_left_matched=true;return 0;}
          continue;
        }
        case State::unmatched: {
          while (m_row<m_io->right_rows()) {
            if (m_io->interrupted()) return fail();
            bool matched=false;
            if (m_io->read_right(&matched)) return fail();
            ++m_row;
            if (!matched) {m_io->null_left();return 0;}
          }
          m_io->finish();m_state=State::done;return -1;
        }
        case State::done:return -1;
      }
    }
  }
};
#endif
