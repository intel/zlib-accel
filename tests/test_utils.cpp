// Copyright (C) 2025 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "test_utils.h"

#include <cstring>
#include <iostream>
#include <string>

#ifdef DEBUG_LOG
void Log(std::string message) { std::cout << message << std::endl; }
#endif

int ZlibCompress(const char* input, size_t input_length, std::string* output,
                 int window_bits, int flush, size_t* output_upper_bound,
                 ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  int st =
      deflateInit2(&stream, -1, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
  if (st != Z_OK) {
    deflateEnd(&stream);
    return st;
  }

  stream.next_in = (Bytef*)input;
  stream.avail_in = static_cast<unsigned int>(input_length);

  *output_upper_bound =
      deflateBound(&stream, static_cast<unsigned long>(input_length));
  output->resize(*output_upper_bound);
  stream.avail_out = static_cast<unsigned int>(*output_upper_bound);
  stream.next_out = reinterpret_cast<Bytef*>(&(*output)[0]);

  st = deflate(&stream, flush);
  *execution_path = GetDeflateExecutionPath(&stream);
  if (st != Z_STREAM_END) {
    deflateEnd(&stream);
    return st;
  }
  output->resize(stream.total_out);

  deflateEnd(&stream);
  return st;
}

int ZlibUncompress(const char* input, size_t input_length, size_t output_length,
                   char** uncompressed, size_t* uncompressed_length,
                   size_t* input_consumed, int window_bits, int flush,
                   int input_chunks, ExecutionPath* execution_path) {
  z_stream stream;
  memset(&stream, 0, sizeof(z_stream));

  // The caller owns a buffer on Z_STREAM_END and on the Z_OK partial return,
  // and nothing on any error, which leaves *uncompressed null rather than
  // handing back a buffer no caller checks the status before releasing.
  *uncompressed = nullptr;

  int st = inflateInit2(&stream, window_bits);
  if (st != Z_OK) {
    inflateEnd(&stream);
    return st;
  }

  *uncompressed = new char[output_length];
  *uncompressed_length = 0;
  unsigned int input_chunk_size = input_length / input_chunks;
  for (int input_chunk = 0; input_chunk < input_chunks; input_chunk++) {
    unsigned int input_offset = input_chunk * input_chunk_size;
    unsigned int input_remaining = input_length - input_offset;
    if (input_chunk == (input_chunks - 1)) {
      input_chunk_size = input_remaining;
    }
    stream.next_in = (Bytef*)(input + input_offset);
    stream.avail_in = static_cast<unsigned int>(input_chunk_size);

    stream.next_out = (Bytef*)(*uncompressed + stream.total_out);
    stream.avail_out =
        static_cast<unsigned int>(output_length - stream.total_out);

    st = inflate(&stream, flush);
    *execution_path = GetInflateExecutionPath(&stream);

    // Z_OK on the last chunk means the input held less than a whole stream, so
    // the prefix that came back is the result the caller asked for rather than
    // a failure. Report its size and hand it over; the other two stop
    // conditions are errors and own nothing.
    bool partial_progress = (st == Z_OK && input_chunk == (input_chunks - 1));
    bool premature_end =
        (st == Z_STREAM_END && input_chunk < (input_chunks - 1));
    bool failed = (st != Z_OK && st != Z_STREAM_END);
    if (partial_progress || premature_end || failed) {
      if (partial_progress) {
        *uncompressed_length = stream.total_out;
        *input_consumed = stream.total_in;
      } else {
        delete[] *uncompressed;
        *uncompressed = nullptr;
      }
      inflateEnd(&stream);
      return st;
    }
  }
  *uncompressed_length = stream.total_out;
  *input_consumed = stream.total_in;

  inflateEnd(&stream);
  return st;
}
