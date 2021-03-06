/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef DATASET_ENGINE_DATASETOPS_SOURCE_MINDRECORD_OP_H_
#define DATASET_ENGINE_DATASETOPS_SOURCE_MINDRECORD_OP_H_
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dataset/engine/data_schema.h"
#include "dataset/engine/datasetops/parallel_op.h"
#include "dataset/engine/datasetops/source/io_block.h"
#include "dataset/util/queue.h"
#include "dataset/util/status.h"
#include "mindrecord/include/shard_error.h"
#include "mindrecord/include/shard_reader.h"
#include "mindrecord/include/common/shard_utils.h"
#include "dataset/util/wait_post.h"

namespace mindspore {
namespace dataset {
// Forward declares
template <typename T>
class Queue;
class DataBuffer;

using mindrecord::ShardOperator;
using mindrecord::ShardReader;
using ShardTuple = std::vector<std::tuple<std::vector<uint8_t>, mindrecord::json>>;  // Row of data from ShardReader

const int32_t LOG_INTERVAL = 19;

class MindRecordOp : public ParallelOp {
 public:
  // The nested builder class inside of the MindRecordOp is used to help manage all of the arguments
  // for constructing it.  Use the builder by setting each argument with the provided set methods,
  // and then finally call the build method to execute the actual construction.
  class Builder {
   public:
    Builder();

    ~Builder() = default;

    Status Build(std::shared_ptr<MindRecordOp> *);

    Builder &SetRowsPerBuffer(int rows_per_buffer) {
      build_rows_per_buffer_ = rows_per_buffer;
      return *this;
    }

    Builder &SetNumMindRecordWorkers(int32_t num_mind_record_workers) {
      build_num_mind_record_workers_ = num_mind_record_workers;
      return *this;
    }

    Builder &SetOpConnectorQueueSize(int32_t queue_size) {
      build_op_connector_queue_size_ = queue_size;
      return *this;
    }

    Builder &SetDatasetFile(const std::string &file) {
      build_dataset_file_ = file;
      return *this;
    }

    Builder &SetColumnsToLoad(const std::vector<std::string> &columns) {
      build_columns_to_load_ = columns;
      return *this;
    }

    Builder &SetOperators(const std::vector<std::shared_ptr<ShardOperator>> &operators) {
      build_operators_ = operators;
      return *this;
    }

    Builder &SetBlockReader() {
      build_block_reader_ = true;
      return *this;
    }

    Status SanityCheck() const;

    static int32_t num_mind_record_workers() { return kDefaultMindRecordWorkers; }

   private:
    static constexpr int32_t kDefaultMindRecordWorkers = 4;
    // The builder saves all MindRecordOp construction arguments internally.
    // The following are the arguments.
    int32_t build_num_mind_record_workers_;
    int32_t builder_num_workers_;
    int32_t build_rows_per_buffer_;
    int32_t build_op_connector_queue_size_;
    std::string build_dataset_file_;
    std::vector<std::string> build_columns_to_load_;
    std::vector<std::shared_ptr<ShardOperator>> build_operators_;
    bool build_block_reader_;
  };

  // Constructor of the MindRecordOp.
  // @note The builder class should be used to call it
  // @param num_mind_record_workers - The number of workers for the op (run by ShardReader)
  // @param rows_per_buffer - The requested number of rows per buffer
  // @param dataset_file - A shard file
  // @param op_connector_queue_size - The output connector queue size
  // @param columns_to_load - The list of columns to use (column name)
  // @param operators - ShardOperators for Shuffle, Category, Sample
  MindRecordOp(int32_t num_mind_record_workers, int32_t rows_per_buffer, std::string dataset_file,
               int32_t op_connector_queue_size, const std::vector<std::string> &columns_to_load,
               const std::vector<std::shared_ptr<ShardOperator>> &operators, const bool &block_reader);

  // Destructor
  ~MindRecordOp() override;

  // A print method typically used for debugging
  // @param out - The output stream to write output to
  // @param show_all - A bool to control if you want to show all info or just a summary
  void Print(std::ostream &out, bool show_all) const override;

  // << Stream output operator overload
  // @notes This allows you to write the debug print info using stream operators
  // @param out - reference to the output stream being overloaded
  // @param op - reference to the MindRecordOp to display
  // @return - the output stream must be returned
  friend std::ostream &operator<<(std::ostream &out, const MindRecordOp &op) {
    op.Print(out, false);
    return out;
  }

  // Worker thread pulls a number of IOBlock from IOBlock Queue, make a buffer and push it to Connector
  // @param int32_t workerId - id of each worker
  // @return Status - The error code return
  Status WorkerEntry(int32_t worker_id) override;

  // Class functor operator () override.
  // All DatasetOps operate by launching a thread (see ExecutionTree). This class functor will
  // provide the master loop that drives the logic for performing the work.
  // @return Status - The error code return
  Status operator()() override;

  // Called first when function is called
  // @return
  Status LaunchThreadAndInitOp();

  // Overrides base class reset method.  When an operator does a reset, it cleans up any state
  // info from it's previous execution and then initializes itself so that it can be executed
  // again.
  // @return Status - The error code return
  Status Reset() override;

  // Getter method
  int32_t num_rows() const { return num_rows_; }

  // Getter method
  static Status CountTotalRows(const std::string dataset_path, int64_t *count);

  // Getter method
  int32_t rows_per_buffer() const { return rows_per_buffer_; }

  // Getter method
  std::string dataset_file() const { return dataset_file_; }

  // Getter method
  std::vector<std::string> columns_to_load() const { return columns_to_load_; }

  bool block_reader() const { return block_reader_; }

  Status Init();

  Status SetColumnsBlob();

 private:
  Status GetBufferFromReader(std::unique_ptr<DataBuffer> *fetched_buffer, int64_t buffer_id, int32_t worker_id);

  // Parses a single cell and puts the data into a tensor
  // @param tensor - the tensor to put the parsed data in
  // @param i_col - the id of column to parse
  // @param columns_blob - the blob data received from the reader
  // @param columns_json - the data for fields received from the reader
  template <typename T>
  Status LoadFeature(std::shared_ptr<Tensor> *tensor, int32_t i_col, const std::vector<uint8_t> &columns_blob,
                     const mindrecord::json &columns_json) const;

  Status SwitchLoadFeature(const DataType &type, std::shared_ptr<Tensor> *tensor, int32_t i_col,
                           const std::vector<uint8_t> &columns_blob, const mindrecord::json &columns_json) const;

  static Status LoadBlob(TensorShape *new_shape, const unsigned char **data, const std::vector<uint8_t> &columns_blob,
                         const int32_t pos, const ColDescriptor &column);

  // Get shape and data (scalar or array) for tensor to be created (for floats and doubles)
  // @param new_shape - the shape of tensor to be created.
  // @param array_data - the array where data should be put in
  // @param column_name - name of current column to be processed
  // @param columns_json - the data for fields received from the reader
  // @param column - description of current column from schema
  // @param use_double - boolean to choose between float32 and float64
  template <typename T>
  static Status LoadFloat(TensorShape *new_shape, std::unique_ptr<T[]> *array_data, const std::string &column_name,
                          const mindrecord::json &columns_json, const ColDescriptor &column, bool use_double);

  // Get shape and data (scalar or array) for tensor to be created (for integers)
  // @param new_shape - the shape of tensor to be created.
  // @param array_data - the array where data should be put in
  // @param column_name - name of current column to be processed
  // @param columns_json - the data for fields received from the reader
  // @param column - description of current column from schema
  template <typename T>
  static Status LoadInt(TensorShape *new_shape, std::unique_ptr<T[]> *array_data, const std::string &column_name,
                        const mindrecord::json &columns_json, const ColDescriptor &column);

  static Status LoadByte(TensorShape *new_shape, std::string *string_data, const std::string &column_name,
                         const mindrecord::json &columns_json);

  // Get a single float value from the given json
  // @param value - the float to put the value in
  // @param arrayData - the given json containing the float
  // @param use_double - boolean to choose between float32 and float64
  template <typename T>
  static Status GetFloat(T *value, const mindrecord::json &data, bool use_double);

  // Get a single integer value from the given json
  // @param value - the integer to put the value in
  // @param arrayData - the given json containing the integer
  template <typename T>
  static Status GetInt(T *value, const mindrecord::json &data);

  Status FetchBlockBuffer(const int32_t &buffer_id);

  int32_t rows_per_buffer_;                                // The number of requested rows per buffer.
  std::string dataset_file_;                               // A dataset file
  std::vector<std::string> columns_to_load_;               // Columns to load from dataset
  std::vector<std::shared_ptr<ShardOperator>> operators_;  // ShardOperators to use
  int32_t num_mind_record_workers_;                        // number of workers to be spawned by ShardReader
  bool block_reader_;                                      // block reader switch
  int32_t buffers_needed_;                                 // Counter for the buffers that were fetched
  int64_t buf_cnt_;                                        // Buffer counter
  int32_t num_rows_;                                       // One more than the last row id in the range for this cache
  std::atomic<int32_t> ended_worker_;
  std::atomic<int32_t> buffer_water_mark_;

  std::unique_ptr<DataSchema> data_schema_;  // Data schema for column typing
  std::vector<std::string> columns_blob_;    // Blob Columns to load from dataset
  std::vector<int32_t> columns_blob_index_;  // Blob Columns to load from dataset

  std::unordered_map<std::string, int32_t> column_name_mapping_;
  std::unique_ptr<ShardReader> shard_reader_;
  WaitPost shard_reader_wait_post_;
  QueueList<std::unique_ptr<IOBlock>> io_blk_queues_;

  // For block reader
  std::mutex mtx_block_reader_;
  std::condition_variable cv_reader_;
  std::vector<std::unique_ptr<std::vector<ShardTuple>>> block_buffer_;
  std::unordered_set<int32_t> block_set_;

  std::mutex ended_worker_mutex_;
};
}  // namespace dataset
}  // namespace mindspore
#endif  // DATASET_ENGINE_DATASETOPS_SOURCE_MINDRECORD_OP_H_
