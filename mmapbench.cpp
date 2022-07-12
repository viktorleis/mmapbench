// vim: set tabstop=2 softtabstop=2 shiftwidth=2:
// sudo apt install libtbb-dev
// g++ -O3 -g mmapbench.cpp -o mmapbench -ltbb -pthread

#include <atomic>
#include <boost/algorithm/string.hpp>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <linux/fs.h>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "tbb/enumerable_thread_specific.h"

using namespace std;

#define check(expr) if (!(expr)) { perror(#expr); throw; }


template<typename... Msgs>
void handle_errno(int errsv, Msgs... msgs) {
  if (errsv)
    (std::cerr << ... << msgs) << ": " << std::strerror(errsv) << std::endl;
  std::exit(EXIT_FAILURE);
}

struct ProcFile
{
  friend void swap(ProcFile &first, ProcFile &second) {
    using std::swap;
    swap(first.path_,    second.path_);
    swap(first.fd_,      second.fd_);
    swap(first.buf_,     second.buf_);
    swap(first.bufsize_, second.bufsize_);
  }

  private:
  std::filesystem::path path_;
  int fd_ = -1;
  char *buf_ = nullptr;
  std::size_t bufsize_;

  public:
  ProcFile() = default;
  ProcFile(std::filesystem::path path, std::size_t buffer_size) : path_(std::move(path)), bufsize_(buffer_size) {
    /*----- Open the file. -----*/
    fd_ = open(path_.c_str(), O_RDONLY);
    if (fd_ == -1)
      handle_errno(errno, "Failed to open ", path_);

    /*----- Allocate a *sufficiently large* buffer to fit in the file. -----*/
    buf_ = new char[bufsize_];
    // std::cerr << "Allocated buffer of " << bufsize_ << " bytes\n";
  }

  ProcFile(const ProcFile&) = delete;
  ProcFile(ProcFile &&other) : ProcFile() { swap(*this, other); }

  ProcFile & operator=(ProcFile &&other) { swap(*this, other); return *this; }

  ~ProcFile() {
    if (fd_ != -1) {
      if (close(fd_))
        handle_errno(errno, "Failed to close ", path_);
    }
    delete[] buf_;
  }

  std::string read() {
    std::size_t bytes_read = 0;
    do {
      const auto n = pread(fd_, buf_ + bytes_read, bufsize_ - 1 - bytes_read, bytes_read); // may read less bytes
      if (n == -1)
        handle_errno(errno, "Failed to read ", path_);
      if (n == 0) break; // no bytes read? then EOF
      bytes_read += n;
    } while (bytes_read != bufsize_); // remaining bytes to read?
    buf_[bytes_read] = 0; // terminating NUL byte
    return std::string(buf_);
  }
};


double gettime() {
  struct timeval now_tv;
  gettimeofday (&now_tv,NULL);
  return ((double)now_tv.tv_sec) + ((double)now_tv.tv_usec)/1000000.0;
}

uint64_t readTLBShootdownCount() {
  static ProcFile interrupts("/proc/interrupts", 40'000);
  std::string contents = interrupts.read();

  /*----- Extract TLB data. -----*/
  const std::string::size_type pos = contents.find("TLB:");
  if (pos == std::string::npos) [[unlikely]] {
    return 0;
  }
  const std::string::size_type EOL = contents.find('\n', pos);
  const std::string TLB_line = contents.substr(pos + 4, EOL);

  /*----- Parse TLB shootdown counts. -----*/
  int off = 0;
  const char *cstr = TLB_line.c_str();
  uint64_t count = 0;
  for (;;) {
    uint64_t tlb;
    int n;
    if (sscanf(cstr + off, "%lu %n", &tlb, &n) != 1)
      break;
    off += n;
    count += tlb;
  }

  return count;
}

uint64_t readIObytesOne() {
  std::ifstream stat("/sys/block/nvme8c8n1/stat");
  assert (!!stat);

  for (std::string line; std::getline(stat, line); ) {
    std::vector<std::string> strs;
    boost::split(strs, line, boost::is_any_of("\t "), boost::token_compress_on);
    std::stringstream ss(strs[2]);
    uint64_t c;
    ss >> c;
    return c*512;
  }
  return 0;
}

uint64_t readIObytes() {
  static ProcFile diskstats("/proc/diskstats", 4000);
  std::string contents = diskstats.read();

  std::string::size_type pos = 0;
  uint64_t sum = 0;
  for (;;) {
    pos = contents.find("nvme", pos); // next occurrence of "nvme"
    if (pos == std::string::npos)
      break;
    const auto EOL = contents.find('\n', pos);
    const std::string line = contents.substr(pos, EOL); // extract line with "nvme"
    unsigned long num_sectors_read;
    sscanf(line.c_str(), "%*s %*u %*u %lu", &num_sectors_read); // parse sectors read from extracted line
    sum += num_sectors_read;
    pos = EOL;
  }
  return sum * 512; // sectors to bytes
}


int main(int argc, char** argv) {
  if (argc < 5) {
    cerr << "dev threads seq hint" << endl;
    return 1;
  }

  int fd = open(argv[1], O_RDONLY);
  check(fd != -1);

  unsigned threads = atoi(argv[2]);

  struct stat sb;
  check(stat(argv[1], &sb) != -1);
  //uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
  //if (fileSize == 0) ioctl(fd, BLKGETSIZE64, &fileSize);

  uint64_t fileSize = 2ull * 1024 * 1024 * 1024 * 1024;

  char* p = (char*)mmap(nullptr, fileSize, PROT_READ, MAP_SHARED, fd, 0);
  assert(p != MAP_FAILED);

  int hint = (argc > 4) ? atoi(argv[4]) : 0;
  if (hint == 1)
    madvise(p, fileSize, MADV_RANDOM);
  else if (hint == 2)
    madvise(p, fileSize, MADV_SEQUENTIAL);
  else
    madvise(p, fileSize, MADV_NORMAL);

  int seq = (argc > 3) ? atoi(argv[3]) : 0;
   
  tbb::enumerable_thread_specific<atomic<uint64_t>> counts;
  tbb::enumerable_thread_specific<atomic<uint64_t>> sums;

  atomic<uint64_t> seqScanPos(0);

  vector<thread> t;
  for (unsigned i=0; i<threads; i++) {
    t.emplace_back([&]() {
      atomic<uint64_t>& count = counts.local();
      atomic<uint64_t>& sum = sums.local();

      if (seq) {
	while (true) {
	  uint64_t scanBlock = 128*1024*1024;
	  uint64_t pos = (seqScanPos += scanBlock) % fileSize;

	  for (uint64_t j=0; j<scanBlock; j+=4096) {
	    sum += p[pos + j];
	    count++;
	  }
	}
      } else {
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<uint64_t> rnd(0, fileSize);

	while (true) {
	  sum += p[rnd(gen)];
	  count++;
	}
      }
    });
  }

  atomic<uint64_t> cpuWork(0);
  t.emplace_back([&]() {
    while (true) {
      double x = cpuWork.load();
      for (uint64_t r=0; r<10000; r++) {
	x = exp(log(x));
      }
      cpuWork++;
    }
  });

  cout << "dev,seq,hint,threads,time,workGB,tlb,readGB,CPUwork" << endl;
  auto lastShootdowns = readTLBShootdownCount();
  auto lastIObytes = readIObytes();
  double start = gettime();
  while (true) {
    sleep(1);
    uint64_t shootdowns = readTLBShootdownCount();
    uint64_t IObytes = readIObytes();
    uint64_t workCount = 0;
    for (auto& x : counts)
      workCount += x.exchange(0);
    double t = gettime() - start;
    cout << argv[1] << "," << seq << "," << hint << "," << threads  << "," << t << "," << (workCount*4096)/(1024.0*1024*1024) << "," << (shootdowns - lastShootdowns) << "," << (IObytes-lastIObytes)/(1024.0*1024*1024) << "," << cpuWork.exchange(0) << endl;
    lastShootdowns = shootdowns;
    lastIObytes = IObytes;
  }

  return 0;
}
