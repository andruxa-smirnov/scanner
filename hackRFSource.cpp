
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <iostream>
#include <limits>
#include <stdarg.h>
#include <cassert>
#include <vector>
#include <algorithm>
#include "messageQueue.h"
#include "signalSource.h"
#include "hackRFSource.h"

#define HANDLE_ERROR(format, ...) this->handle_error(status, format, ##__VA_ARGS__)

void HackRFSource::handle_error(int status, const char * format, ...)
{
  if (status != 0) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsprintf(buffer, format, args);
    fprintf(stderr, buffer, hackrf_error_name(static_cast<hackrf_error>(status)));
    hackrf_close(this->m_dev);
    exit(1);
  }
}

HackRFSource::HackRFSource(std::string args,
                           uint32_t sampleRate, 
                           uint32_t sampleCount, 
                           double startFrequency, 
                           double stopFrequency)
  : SignalSource(sampleRate, sampleCount, startFrequency, stopFrequency),
    m_dev(nullptr),
    m_streamingState(Illegal),
    m_bufferIndex(0),
    m_didRetune(false)
{
  this->m_buffer = new int8_t[sampleCount][2];
  int status;

  status = hackrf_init();
  HANDLE_ERROR("hackrf_init() failed: %%s\n");

  status = hackrf_open( &this->m_dev );
  HANDLE_ERROR("Failed to open HackRF device: %%s\n");

  uint8_t board_id;
  status = hackrf_board_id_read( this->m_dev, &board_id );
  HANDLE_ERROR("Failed to get HackRF board id: %%s\n");

  char version[128];
  memset(version, 0, sizeof(version));
  status = hackrf_version_string_read( this->m_dev, version, sizeof(version));
  HANDLE_ERROR("Failed to read version string: %%s\n");

  this->set_sample_rate(sampleRate);

  /* range 0-40 step 8d, IF gain in osmosdr  */
  hackrf_set_lna_gain(this->m_dev, 8);

  /* range 0-62 step 2db, BB gain in osmosdr */
  hackrf_set_vga_gain(this->m_dev, 20);

  /* Parameter value shall be 0=Disable BiasT or 1=Enable BiasT */
  /* antenna port power control */
  hackrf_set_amp_enable(this->m_dev, 0);

  if (args.find("bias")) {
    status = hackrf_set_amp_enable(this->m_dev, 1);
    HANDLE_ERROR("Failed to enable DC bias: %%s\n");
  }

  double centerFrequency = this->GetCurrentFrequency();
  this->Retune(centerFrequency);
}

HackRFSource::~HackRFSource()
{
  if (this->m_dev != nullptr) {
    int status = hackrf_stop_rx(this->m_dev);
    double centerFrequency = this->GetCurrentFrequency();
    HANDLE_ERROR("Failed to stop RX streaming at %u: %%s\n", centerFrequency);
    status = hackrf_close(this->m_dev);
    HANDLE_ERROR("Error closing hackrf: %%s\n");
  }
}

bool HackRFSource::Start()
{
  if (this->m_streamingState != Streaming) {
    int status = hackrf_start_rx(this->m_dev, 
                                 _hackRF_rx_callback, 
                                 (void *)this);
    HANDLE_ERROR("Failed to start RX streaming: %%s\n");
    this->m_streamingState = Streaming;
  }
  return true;
}

double HackRFSource::set_sample_rate( double rate )
{
  assert(this->m_dev != nullptr);

  int status = HACKRF_SUCCESS;
  double _sample_rates[] = {
    8e6,
    10e6,
    12.5e6,
    16e6,
    20e6};

  bool found_supported_rate = false;
  for( unsigned int i = 0; i < sizeof(_sample_rates)/sizeof(double); i++ ) {
    if(_sample_rates[i] == rate) {
      found_supported_rate = true;
      break;
    }
  }

  if (!found_supported_rate) {
    status = HACKRF_ERROR_OTHER;
    HANDLE_ERROR("Unsupported samplerate: %gMsps", rate/1e6);
  }

  status = hackrf_set_sample_rate( this->m_dev, rate);
  HANDLE_ERROR("Error setting sample rate to %gMsps: %%s\n", rate/1e6);
}

int HackRFSource::_hackRF_rx_callback(hackrf_transfer* transfer) 
{
  HackRFSource * obj = (HackRFSource *)transfer->rx_ctx;
  return obj->hackRF_rx_callback(transfer);
}

int HackRFSource::hackRF_rx_callback(hackrf_transfer* transfer)
{
  time_t startTime;
  printf("hackRF_rx_callback valid_length:%d\n", transfer->valid_length);
  if (!this->GetIsDone()) { 
    double centerFrequency = this->GetCurrentFrequency();
    bool isScanStart = this->GetIsScanStart();
    uint32_t startIndex = this->m_bufferIndex;
    uint32_t count = transfer->valid_length/2;
    if (this->m_didRetune) {
      // Re-tune time is 5ms. So need to discard 5ms worth of samples.
      uint32_t discardCount = this->m_sampleRate/200;
      this->m_didRetune = false;
      startIndex += discardCount;
      count -= discardCount;
      startTime = time(NULL);
    }
    // count = std::min<uint32_t>(count, this->m_sampleCount - startIndex);
    printf("hackRF_rx_callback count:%d\n", count);
    if (count < this->m_sampleCount) {
      if (this->m_bufferIndex < this->m_sampleCount) {
        memcpy(this->m_buffer + startIndex,
               transfer->buffer, 
               sizeof(fftwf_complex) * count);
        this->m_bufferIndex += count;
      }
      if (this->m_bufferIndex == this->m_sampleCount) {
        double nextFrequency = this->GetNextFrequency();
        if (this->GetFrequencyCount() > 1) {
          this->Retune(nextFrequency);
          this->m_didRetune = true;
        }
        this->m_sampleQueue->AppendSamples(this->m_buffer, 
                                           centerFrequency,
                                           (isScanStart ? startTime : 0));
        this->m_bufferIndex = 0;
      }
    } else {
      printf("count >= this->m_sampleCount\n");
      double nextFrequency = this->GetNextFrequency();
      if (this->GetFrequencyCount() > 1) {
        // this->Retune(nextFrequency);
        // this->m_didRetune = true;
      }
      for (uint32_t i = 0; i < count; i+= this->m_sampleCount) {
        printf("hackRF_rx_callback appending[%d]\n", i);
        this->m_sampleQueue->AppendSamples(reinterpret_cast<int8_t (*)[2]>(&transfer->buffer[2*i]),
                                           centerFrequency,
                                           (isScanStart ? startTime : 0));
      }
    }
  } else {
    this->m_streamingState = Done;
  }
  return 0; // TODO: return -1 on error/stop
}

bool HackRFSource::GetNextSamples(SampleQueue * sampleQueue, double & centerFrequency)
{
  int status;
  uint32_t delta = 100;
  centerFrequency = this->GetNextFrequency();
  // sleep(0.010);
  //fprintf(stderr, "Tuned to %u\n", frequencies[j]);
  /* ... Handle signals at current frequency ... */
  this->m_streamingState = Streaming;

  while (this->m_streamingState != Done) {
  }

  if (this->GetFrequencyCount() > 1) {
    this->Retune(this->GetNextFrequency());
  }
  return true;
}

bool HackRFSource::StartStreaming(uint32_t numIterations, SampleQueue & sampleQueue)
{
  auto result = this->StartThread(numIterations, sampleQueue);
  this->Start();
  return result;
}

void HackRFSource::ThreadWorker()
{
  while (this->m_streamingState != Done) {
  };
  int status = hackrf_stop_rx(this->m_dev);
  HANDLE_ERROR("Failed to stop RX streaming: %%s\n");
}

double HackRFSource::Retune(double centerFrequency)
{
  int status;
  printf("Retuning to %.0f\n", centerFrequency);
  status = hackrf_set_freq(this->m_dev, uint64_t(centerFrequency));
  HANDLE_ERROR("Failed to tune to %.0f Hz: %%s\n", 
               centerFrequency);
  printf("Retuned to %.0f\n", centerFrequency);
  return centerFrequency;
}