#include "pvd/session.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::duration d) {return std::chrono::duration<double,std::milli>(d).count();}
struct Memory {
  std::uint64_t working = 0, peak_working = 0, private_bytes = 0,
                peak_commit = 0;
};
Memory memory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if(GetProcessMemoryInfo(GetCurrentProcess(),
      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters)))
    return {static_cast<std::uint64_t>(counters.WorkingSetSize),
            static_cast<std::uint64_t>(counters.PeakWorkingSetSize),
            static_cast<std::uint64_t>(counters.PrivateUsage),
            static_cast<std::uint64_t>(counters.PeakPagefileUsage)};
#endif
  return {};
}
void write_memory(const char *name,Memory value) {
  std::cout<<"  \""<<name<<"\": {\"working_set_bytes\": "<<value.working
           <<", \"peak_working_set_bytes\": "<<value.peak_working
           <<", \"private_bytes\": "<<value.private_bytes
           <<", \"peak_commit_bytes\": "<<value.peak_commit<<"}";
}
void distribution(const char *name,std::vector<double> values) {
  std::sort(values.begin(),values.end());
  const auto n=values.size();
  std::cout<<"  \""<<name<<"\": {\"samples\": "<<n;
  if(n)std::cout<<", \"mean_ms\": "<<std::accumulate(values.begin(),values.end(),0.)/double(n)
      <<", \"median_ms\": "<<values[n/2]<<", \"p95_ms\": "<<values[(n-1)*95/100]
      <<", \"max_ms\": "<<values.back();
  std::cout<<"}";
}
}
int main(int argc,char **argv) {
  try {
    if(argc<2)throw std::invalid_argument("usage: pvd_benchmark capture.pxd2 [seek_samples=256] [--no-checkpoints] [--load-only]");
    std::size_t sample_count=256;
    bool no_checkpoints=false,load_only=false,have_count=false;
    for(int i=2;i<argc;++i) {
      const std::string option=argv[i];
      if(option=="--no-checkpoints")no_checkpoints=true;
      else if(option=="--load-only")load_only=true;
      else {
        if(have_count)throw std::invalid_argument("unexpected benchmark argument");
        std::size_t end=0;auto n=std::stoull(option,&end);
        if(end!=option.size()||n==0||n>10000)throw std::invalid_argument("seek_samples must be 1..10000");
        sample_count=static_cast<std::size_t>(n);have_count=true;
      }
    }
    const auto bytes=std::filesystem::file_size(std::filesystem::u8path(argv[1]));
    pvd::SessionOptions options;if(no_checkpoints)options.max_checkpoints=0;
    std::cerr<<"Loading capture...\n"<<std::flush;
    auto started=Clock::now();auto session=pvd::Session::load_file(argv[1],options);auto loaded=Clock::now();
    const auto load_memory=memory();
    std::cerr<<"Loaded "<<session.event_count()<<" events, "<<session.frames().size()
             <<" native frames, "<<session.checkpoint_count()<<" checkpoints\n"<<std::flush;
    std::vector<double> random,reverse;
    std::uint64_t checksum=0,seed=0x5f34d19;
    auto visit=[&](std::size_t index,std::vector<double> &times){
      auto begin=Clock::now();auto snapshot=session.snapshot_at_frame(index);times.push_back(ms(Clock::now()-begin));
      checksum^=static_cast<std::uint64_t>(snapshot.event_count)+static_cast<std::uint64_t>(snapshot.instances.size())*131;
    };
    if(!load_only&&!session.frames().empty()) {
      random.reserve(sample_count);reverse.reserve(std::min(sample_count,session.frames().size()));
      for(std::size_t i=0;i<sample_count;++i){seed=seed*6364136223846793005ull+1442695040888963407ull;visit(static_cast<std::size_t>(seed%session.frames().size()),random);}
      for(std::size_t i=0;i<std::min(sample_count,session.frames().size());++i)visit(session.frames().size()-1-i,reverse);
    }
    const auto load_ms=ms(loaded-started);
    std::cout<<std::fixed<<std::setprecision(4)<<"{\n  \"input_bytes\": "<<bytes
      <<",\n  \"events\": "<<session.event_count()<<",\n  \"native_frames\": "<<session.frames().size()
      <<",\n  \"load_ms\": "<<load_ms<<",\n  \"load_mib_per_second\": "<<(load_ms>0?double(bytes)/1048576.*1000/load_ms:0)
      <<",\n  \"checkpoints\": "<<session.checkpoint_count()
      <<",\n  \"estimated_checkpoint_bytes\": "<<session.estimated_checkpoint_bytes()
      <<",\n  \"checkpoints_disabled\": "<<(no_checkpoints?"true":"false")
      <<",\n  \"peak_working_set_bytes\": "<<memory().peak_working<<",\n";
    write_memory("after_load_memory",load_memory);std::cout<<",\n";
    write_memory("finished_memory",memory());std::cout<<",\n";
    distribution("random_seek",random);std::cout<<",\n";distribution("reverse_seek",reverse);
    std::cout<<",\n  \"checksum\": "<<checksum<<"\n}\n";
    return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
