#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "sdp_full_outer_driver.h"

static void expect(bool value,const char *message) {if (!value) throw std::runtime_error(message);}
struct Row {int identity;std::optional<int> key;std::string value;};
struct Saved {Row row;bool matched;};
using Pair=std::pair<int,int>;

// This adapter supplies deterministic physical records and failing/interrupted
// I/O to the exact same driver used by the native HashJoinChunk adapter.
struct Io {
  std::vector<Row> left,right;
  std::vector<Saved> a,b,*input=&a,*output=&b;
  Row left_record{},right_record{};
  bool left_null=false,right_null=false,killed=false;
  uint64_t matrix=0;
  size_t left_at=0,right_at=0,initializations=0,left_reads=0,right_reads=0,predicates=0,closed=0;
  int fail_read_at=-1;
  std::vector<unsigned> calls;
  bool initialize() {
    ++initializations;left_at=0;right_at=0;input=&a;output=&b;
    a.clear();b.clear();left_null=right_null=false;
    for (const Row &row:right) {a.push_back({row,false});++right_reads;}
    calls.assign(left.size()*right.size(),0);return false;
  }
  bool interrupted() {return killed;}
  int read_left() {
    left_null=right_null=false;
    if (left_at==left.size()) return -1;
    left_record=left[left_at++];++left_reads;return 0;
  }
  uint64_t right_rows() const {return input->size();}
  bool start_scan() {right_at=0;output->clear();return false;}
  bool read_right(bool *matched) {
    if (fail_read_at>=0 && static_cast<int>(right_at)==fail_read_at) return true;
    right_null=false;
    const auto &saved=input->at(right_at++);right_record=saved.row;*matched=saved.matched;
    return false;
  }
  bool matches(bool *matched) {
    const auto index=left_record.identity*right.size()+right_record.identity;
    ++predicates;++calls.at(index);
    // SQL UNKNOWN from a nullable predicate is false at a join condition.
    std::optional<bool> value;
    if (left_record.key && right_record.key) value=(matrix&(uint64_t{1}<<index))!=0;
    *matched=value.value_or(false);return false;
  }
  bool write_right(bool matched) {output->push_back({right_record,matched});return false;}
  void end_scan() {std::swap(input,output);}
  bool start_unmatched() {right_at=0;return false;}
  void null_left() {left_null=true;}
  void null_right() {right_null=true;}
  void finish() {a.clear();b.clear();++closed;}
  Pair current() const {return {left_null?-1:left_record.identity,right_null?-1:right_record.identity};}
};

static std::vector<Pair> run(Io &io,Sdp_full_outer_driver<Io> &driver) {
  expect(!driver.init(),"driver init failed");std::vector<Pair> result;
  for (;;) {
    const int status=driver.read();
    if (status<0) break;
    expect(status==0,"unexpected driver failure");
    result.push_back(io.current());
    if (!io.right_null) expect(io.right_record.value==io.right.at(io.right_record.identity).value,"spooled binary/UTF8 payload changed");
  }
  std::sort(result.begin(),result.end());return result;
}

int main() {
  try {
    size_t graphs=0;
    for (size_t left=0;left<=3;++left) for (size_t right=0;right<=3;++right) {
      const uint64_t variants=uint64_t{1}<<(left*right);
      for (uint64_t edges=0;edges<variants;++edges) {
        Io io;io.matrix=edges;
        for (size_t i=0;i<left;++i) io.left.push_back({static_cast<int>(i),1,"duplicate"});
        for (size_t j=0;j<right;++j) io.right.push_back({static_cast<int>(j),1,"duplicate"});
        std::vector<Pair> expected;
        for (size_t i=0;i<left;++i) {
          bool matched=false;
          for (size_t j=0;j<right;++j) if (edges&(uint64_t{1}<<(i*right+j))) {
            expected.emplace_back(static_cast<int>(i),static_cast<int>(j));matched=true;
          }
          if (!matched) expected.emplace_back(static_cast<int>(i),-1);
        }
        for (size_t j=0;j<right;++j) {
          bool matched=false;
          for (size_t i=0;i<left;++i) matched|=(edges&(uint64_t{1}<<(i*right+j)))!=0;
          if (!matched) expected.emplace_back(-1,static_cast<int>(j));
        }
        std::sort(expected.begin(),expected.end());
        Sdp_full_outer_driver<Io> driver(&io);
        expect(run(io,driver)==expected,"FULL bag/cardinality differs from explicit relation graph");
        expect(io.initializations==1 && io.left_reads==left && io.right_reads==right,"a source was evaluated more than once");
        expect(io.predicates==left*right && std::all_of(io.calls.begin(),io.calls.end(),[](unsigned n){return n==1;}),"volatile ON was skipped or repeated for a physical row pair");
        expect(io.a.empty() && io.b.empty(),"EOF retained native spool state");
        expect(run(io,driver)==expected && io.predicates==2*left*right,"prepared reinitialization reused previous match bits");
        ++graphs;
      }
    }
    Io nullable;nullable.matrix=15;
    nullable.left={{0,std::nullopt,"same"},{1,std::nullopt,"same"}};
    nullable.right={{0,std::nullopt,"same"},{1,std::nullopt,"same"}};
    Sdp_full_outer_driver<Io> null_driver(&nullable);
    expect(run(nullable,null_driver)==std::vector<Pair>({{-1,0},{-1,1},{0,-1},{1,-1}}),"NULL keys collapsed or matched unknown predicates");
    Io binary;binary.matrix=0;
    binary.left={{0,1,"left"}};
    binary.right={{0,2,std::string("nul\0Türkçe",11)+std::string(1024*1024,'x')}};
    Sdp_full_outer_driver<Io> binary_driver(&binary);
    expect(run(binary,binary_driver)==std::vector<Pair>({{-1,0},{0,-1}}),"large binary/Unicode unmatched row changed");
    Io failed;failed.left={{0,1,"l"}};failed.right={{0,1,"r"}};failed.fail_read_at=0;
    Sdp_full_outer_driver<Io> failure_driver(&failed);
    expect(!failure_driver.init() && failure_driver.read()==1 && failure_driver.read()==-1,"spool I/O failure was hidden or retried");
    expect(failed.a.empty() && failed.b.empty(),"I/O failure retained spool data");
    Io interrupted;interrupted.left={{0,1,"l"}};interrupted.right={{0,1,"r"}};
    Sdp_full_outer_driver<Io> interrupt_driver(&interrupted);
    expect(!interrupt_driver.init(),"interrupt fixture init failed");interrupted.killed=true;
    expect(interrupt_driver.read()==1 && interrupted.a.empty(),"cancelled FULL scan kept reading or retained spools");
    std::cout<<"PASS FULL driver: "<<graphs<<" explicit relation graphs, physical duplicates, NULL, once-only inputs/predicates, reinit, binary payload and I/O/cancel cleanup\n";
  } catch (const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}
