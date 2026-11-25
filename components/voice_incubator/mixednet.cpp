// Vendored from https://github.com/synesthesiam/mixednet-executor, with the
// namespace changed and the requantization tables read from the received blob
// (mixednet_blob.cpp) instead of derived from scales. Keep the kernels in step.

// ESPHome builds at -Os. These loops run once per model every 30 ms, and -O2
// halves them on the Voice PE (okay_nabu: 3.8 ms -> 1.9 ms per invoke) for 16
// bytes of flash; -O3 adds only another 3%.
#pragma GCC optimize("O2")

#include "mixednet.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <climits>

#ifdef MIXEDNET_USE_ESP_NN
#include <esp_nn.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
#endif

namespace esphome::voice_incubator {

namespace {

// TFLite has two requantization roundings, chosen at build time by
// TFLITE_SINGLE_ROUNDING. ESPHome does not define it, so esp-tflite-micro and
// esp-nn both use the older double-rounding (gemmlowp) path, which is the
// default here. Define MIXEDNET_SINGLE_ROUNDING to match a desktop TFLite build.
#ifdef MIXEDNET_SINGLE_ROUNDING

int32_t multiply_by_quantized_multiplier(int32_t x, int32_t multiplier, int32_t shift) {
  const int64_t total_shift = 31 - shift;
  const int64_t round = int64_t{1} << (total_shift - 1);
  return static_cast<int32_t>((static_cast<int64_t>(x) * multiplier + round) >> total_shift);
}

#else

// Port of gemmlowp's SaturatingRoundingDoublingHighMul, matching esp_nn_sat_round_doubling_high_mul.
int32_t saturating_rounding_doubling_high_mul(int32_t a, int32_t b) {
  const bool overflow = (a == b) && (a == INT32_MIN);
  const int64_t ab = static_cast<int64_t>(a) * static_cast<int64_t>(b);
  const int32_t nudge = ab >= 0 ? (1 << 30) : (1 - (1 << 30));
  const int32_t result = static_cast<int32_t>((ab + nudge) / (int64_t{1} << 31));
  return overflow ? INT32_MAX : result;
}

int32_t rounding_divide_by_pot(int32_t x, int32_t exponent) {
  if (exponent == 0)
    return x;
  const int32_t mask = (int32_t{1} << exponent) - 1;
  const int32_t remainder = x & mask;
  const int32_t threshold = (mask >> 1) + (x < 0 ? 1 : 0);
  return (x >> exponent) + (remainder > threshold ? 1 : 0);
}

int32_t multiply_by_quantized_multiplier(int32_t x, int32_t multiplier, int32_t shift) {
  const int32_t left_shift = shift > 0 ? shift : 0;
  const int32_t right_shift = shift > 0 ? 0 : -shift;
  return rounding_divide_by_pot(
      saturating_rounding_doubling_high_mul(x * (int32_t{1} << left_shift), multiplier), right_shift);
}

#endif

bool aligned16(const void *p) { return (reinterpret_cast<uintptr_t>(p) & 15) == 0; }

#ifdef MIXEDNET_USE_ESP_NN

// A depthwise op is a [taps x channels] window with a single output frame.
void describe_depthwise(const MixedNetOp &op, data_dims_t &input_dims, data_dims_t &filter_dims,
                        data_dims_t &output_dims, dw_conv_params_t &params) {
  input_dims = {1, op.src_nt, op.src_nc, 1};
  filter_dims = {1, op.src_nt, op.src_nc, 1};
  output_dims = {1, 1, op.out_channels, 1};
  params = {};
  params.in_offset = op.in_offset;
  params.out_offset = op.out_offset;
  params.ch_mult = 1;
  params.stride = {1, 1};
  params.padding = {0, 0};
  params.dilation = {1, 1};
  params.activation = {op.act_min, op.act_max};
}

// A dense op is a matrix-vector product, which esp-nn computes as a 1x1
// convolution over a 1x1 input of `row` channels. Going through esp_nn_conv_s8
// rather than the fully-connected entry point matters: only conv has an
// architecture-optimized implementation.
void describe_dense(const MixedNetOp &op, data_dims_t &input_dims, data_dims_t &filter_dims,
                    data_dims_t &output_dims, conv_params_t &params) {
  const int32_t row = static_cast<int32_t>(op.src_nt) * op.src_nc;
  input_dims = {1, 1, row, 1};
  filter_dims = {1, 1, row, op.out_channels};
  output_dims = {1, 1, op.out_channels, 1};
  params = {};
  params.in_offset = op.in_offset;
  params.out_offset = op.out_offset;
  params.stride = {1, 1};
  params.padding = {0, 0};
  params.dilation = {1, 1};
  params.activation = {op.act_min, op.act_max};
}

// esp-nn reads rows contiguously, so a channel-split view has to stay scalar.
bool depthwise_is_contiguous(const MixedNetOp &op) { return op.src_c0 == 0 && op.src_nc == op.src_stride; }

// The blob's tables are const; esp-nn only reads them.
quant_data_t quant_data(const MixedNetOp &op) {
  return {const_cast<int32_t *>(op.shift), const_cast<int32_t *>(op.mult)};
}

uint8_t *alloc_scratch(size_t size) {
  size = (size + 15) & ~size_t{15};
#ifdef ESP_PLATFORM
  // PSRAM is fine: with aligned weights and input, esp-nn asks for scratch
  // sized for copies it then does not make.
  void *p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr)
    p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);
  return static_cast<uint8_t *>(p);
#else
  return static_cast<uint8_t *>(std::aligned_alloc(16, size));
#endif
}

#endif

}  // namespace

#ifdef MIXEDNET_USE_ESP_NN
void MixedNetExecutor::ScratchFree::operator()(void *p) const {
#ifdef ESP_PLATFORM
  heap_caps_free(p);
#else
  std::free(p);
#endif
}
#endif

bool MixedNetExecutor::setup(const MixedNetModel *model) {
  this->model_ = model;
  this->offsets_ = std::make_unique<uint16_t[]>(model->num_buffers);
  size_t offset = 0;
  for (uint8_t i = 0; i < model->num_buffers; i++) {
    const MixedNetBuffer &b = model->buffers[i];
    // Every buffer starts 16-byte aligned, so esp-nn can read a window that
    // starts at its first frame in place.
    offset = (offset + 15) & ~size_t{15};
    if (offset > 0xFFFF)
      return false;
    this->offsets_[i] = static_cast<uint16_t>(offset);
    offset += static_cast<size_t>(b.frames) * b.channels;
  }
  this->arena_size_ = offset;
  this->arena_storage_ = std::make_unique<int8_t[]>(offset + 15);
  if (this->arena_storage_ == nullptr)
    return false;
  this->arena_ = reinterpret_cast<int8_t *>((reinterpret_cast<uintptr_t>(this->arena_storage_.get()) + 15) &
                                            ~uintptr_t{15});
  if (!this->choose_kernels_())
    return false;
  this->reset();
  return true;
}

// Decides, once, which ops run on esp-nn, and sizes its scratch buffer for them.
bool MixedNetExecutor::choose_kernels_() {
  const MixedNetModel *m = this->model_;
  this->esp_nn_ = std::make_unique<uint8_t[]>(m->num_ops);
  std::fill(this->esp_nn_.get(), this->esp_nn_.get() + m->num_ops, 0);
  this->esp_nn_ops_ = 0;
  this->scratch_size_ = 0;
#ifdef MIXEDNET_USE_ESP_NN
  this->scratch_.reset();
  int needed = 0;
  for (uint8_t i = 0; i < m->num_ops; i++) {
    const MixedNetOp &op = m->ops[i];
    const int8_t *src = op.src_buf == MIXEDNET_SRC_EXTERNAL
                            ? nullptr
                            : this->buffer_(op.src_buf) + static_cast<size_t>(op.src_t0) * op.src_stride + op.src_c0;
    // The ESP32-S3 kernels load input and weights with vector instructions
    // that drop the low address bits: unaligned data computes garbage, not an
    // error. A blob from an older packer only aligns weights to 4 bytes.
    const bool in_place = src != nullptr && aligned16(src) && aligned16(op.weights);
    data_dims_t in_dims, filter_dims, out_dims;
    int size = 0;
    if (op.type == MIXEDNET_OP_DENSE && this->dense_esp_nn_ && in_place) {
      conv_params_t params;
      describe_dense(op, in_dims, filter_dims, out_dims, params);
      size = esp_nn_get_conv_scratch_size(&in_dims, &filter_dims, &out_dims, &params);
    } else if (op.type == MIXEDNET_OP_DEPTHWISE && this->depthwise_esp_nn_ && in_place &&
               depthwise_is_contiguous(op)) {
      dw_conv_params_t params;
      describe_depthwise(op, in_dims, filter_dims, out_dims, params);
      size = esp_nn_get_depthwise_conv_scratch_size(&in_dims, &filter_dims, &out_dims, &params);
    } else {
      continue;
    }
    this->esp_nn_[i] = 1;
    this->esp_nn_ops_++;
    needed = std::max(needed, size);
  }
  // esp-nn refuses to run without a scratch buffer, even where it asks for none.
  if (this->esp_nn_ops_ > 0) {
    this->scratch_size_ = std::max<size_t>(static_cast<size_t>(needed), 16);
    this->scratch_.reset(alloc_scratch(this->scratch_size_));
    if (this->scratch_ == nullptr)
      return false;
  }
#endif
  return true;
}

void MixedNetExecutor::reset() {
  for (uint8_t i = 0; i < this->model_->num_buffers; i++) {
    const MixedNetBuffer &b = this->model_->buffers[i];
    std::memset(this->buffer_(i), static_cast<int>(b.init), static_cast<size_t>(b.frames) * b.channels);
  }
}

void MixedNetExecutor::ring_push_(const MixedNetOp &op, const int8_t *features) {
  const MixedNetBuffer &dst_buf = this->model_->buffers[op.dst_buf];
  int8_t *dst = this->buffer_(op.dst_buf) + static_cast<size_t>(dst_buf.keep) * dst_buf.channels;
  if (op.src_buf == MIXEDNET_SRC_EXTERNAL) {
    std::memcpy(dst, features, static_cast<size_t>(op.src_nt) * op.src_nc);
    return;
  }
  const int8_t *src = this->buffer_(op.src_buf) + static_cast<size_t>(op.src_t0) * op.src_stride + op.src_c0;
  for (uint16_t t = 0; t < op.src_nt; t++) {
    std::memcpy(dst + static_cast<size_t>(t) * dst_buf.channels, src + static_cast<size_t>(t) * op.src_stride,
                op.src_nc);
  }
}

void MixedNetExecutor::copy_(const MixedNetOp &op) {
  const int8_t *src = this->buffer_(op.src_buf) + static_cast<size_t>(op.src_t0) * op.src_stride + op.src_c0;
  int8_t *dst = this->buffer_(op.dst_buf) + op.dst_off;
  std::memcpy(dst, src, op.src_nc);
}

void MixedNetExecutor::dense_(const MixedNetOp &op, uint8_t index) {
  const int8_t *src = this->buffer_(op.src_buf) + static_cast<size_t>(op.src_t0) * op.src_stride;
  const uint32_t row = static_cast<uint32_t>(op.src_nt) * op.src_nc;
  int8_t *dst = this->buffer_(op.dst_buf) + op.dst_off;
#ifdef MIXEDNET_USE_ESP_NN
  if (this->esp_nn_[index]) {
    data_dims_t in_dims, filter_dims, out_dims;
    conv_params_t params;
    describe_dense(op, in_dims, filter_dims, out_dims, params);
    const quant_data_t quant = quant_data(op);
    esp_nn_conv_s8(&in_dims, src, &filter_dims, op.weights, op.bias, &out_dims, dst, &params, &quant);
    return;
  }
#endif
  for (uint16_t oc = 0; oc < op.out_channels; oc++) {
    const int8_t *w = op.weights + static_cast<size_t>(oc) * row;
    int32_t acc = op.bias[oc];
    for (uint32_t i = 0; i < row; i++) {
      acc += static_cast<int32_t>(w[i]) * (static_cast<int32_t>(src[i]) + op.in_offset);
    }
    acc = multiply_by_quantized_multiplier(acc, op.mult[oc], op.shift[oc]) + op.out_offset;
    dst[oc] = static_cast<int8_t>(std::min(std::max(acc, op.act_min), op.act_max));
  }
}

void MixedNetExecutor::depthwise_(const MixedNetOp &op, uint8_t index) {
  const int8_t *src = this->buffer_(op.src_buf) + static_cast<size_t>(op.src_t0) * op.src_stride + op.src_c0;
  int8_t *dst = this->buffer_(op.dst_buf) + op.dst_off;
#ifdef MIXEDNET_USE_ESP_NN
  if (this->esp_nn_[index]) {
    data_dims_t in_dims, filter_dims, out_dims;
    dw_conv_params_t params;
    describe_depthwise(op, in_dims, filter_dims, out_dims, params);
    const quant_data_t quant = quant_data(op);
    esp_nn_depthwise_conv_s8(&in_dims, src, &filter_dims, op.weights, op.bias, &out_dims, dst, &params, &quant);
    return;
  }
#endif
  for (uint16_t c = 0; c < op.out_channels; c++) {
    int32_t acc = op.bias[c];
    for (uint16_t k = 0; k < op.src_nt; k++) {
      acc += static_cast<int32_t>(op.weights[static_cast<size_t>(k) * op.src_nc + c]) *
             (static_cast<int32_t>(src[static_cast<size_t>(k) * op.src_stride + c]) + op.in_offset);
    }
    acc = multiply_by_quantized_multiplier(acc, op.mult[c], op.shift[c]) + op.out_offset;
    dst[c] = static_cast<int8_t>(std::min(std::max(acc, op.act_min), op.act_max));
  }
}

// Max pooling keeps the input's quantization, so no requantization: the int8
// maximum of K frames, channel by channel.
void MixedNetExecutor::max_pool_(const MixedNetOp &op) {
  const int8_t *src = this->buffer_(op.src_buf) + static_cast<size_t>(op.src_t0) * op.src_stride + op.src_c0;
  int8_t *dst = this->buffer_(op.dst_buf) + op.dst_off;
  for (uint16_t c = 0; c < op.src_nc; c++) {
    int32_t best = src[c];
    for (uint16_t k = 1; k < op.src_nt; k++) {
      best = std::max(best, static_cast<int32_t>(src[static_cast<size_t>(k) * op.src_stride + c]));
    }
    dst[c] = static_cast<int8_t>(std::min(std::max(best, op.act_min), op.act_max));
  }
}

uint8_t MixedNetExecutor::invoke(const int8_t *features) {
  const MixedNetModel *m = this->model_;
#ifdef MIXEDNET_USE_ESP_NN
  // esp-nn keeps its scratch pointer in a global, and several models share the
  // inference task, so point it at this model's buffer every time.
  if (this->esp_nn_ops_ > 0) {
    esp_nn_set_conv_scratch_buf(this->scratch_.get());
    esp_nn_set_depthwise_conv_scratch_buf(this->scratch_.get());
  }
#endif
  for (uint8_t i = 0; i < m->num_ops; i++) {
    const MixedNetOp &op = m->ops[i];
    switch (op.type) {
      case MIXEDNET_OP_RING_PUSH:
        this->ring_push_(op, features);
        break;
      case MIXEDNET_OP_DENSE:
        this->dense_(op, i);
        break;
      case MIXEDNET_OP_DEPTHWISE:
        this->depthwise_(op, i);
        break;
      case MIXEDNET_OP_COPY:
        this->copy_(op);
        break;
      case MIXEDNET_OP_MAX_POOL:
        this->max_pool_(op);
        break;
      default:
        break;
    }
  }

  this->last_logit_ = *this->buffer_(m->logit_buffer);
  const uint8_t probability = m->sigmoid_lut[static_cast<uint8_t>(this->last_logit_)];

  // Advance every ring window now that all reads of this invoke's data are done.
  for (uint8_t i = 0; i < m->num_shifts; i++) {
    const uint8_t index = m->shift_buffers[i];
    const MixedNetBuffer &b = m->buffers[index];
    int8_t *base = this->buffer_(index);
    std::memmove(base, base + static_cast<size_t>(b.push) * b.channels,
                 static_cast<size_t>(b.keep) * b.channels);
  }
  return probability;
}

}  // namespace esphome::voice_incubator
