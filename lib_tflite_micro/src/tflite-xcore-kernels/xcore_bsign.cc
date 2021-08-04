

#include "flatbuffers/flexbuffers.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "xcore_custom_options.h"
#include "xcore_dispatcher.h"
#include "xcore_utils.h"

extern "C" {
#include "lib_nn/api/nn_operator.h"
}

namespace tflite {
namespace ops {
namespace micro {
namespace xcore {
namespace bsign {

// -------------------------------------------------------------------- //
// kernel argument type
// -------------------------------------------------------------------- //

struct BSign8Args {
  int32_t* Y;
  const int8_t* X;
  int8_t zero_point_vec[VPU_INT8_EPV];
};

// -------------------------------------------------------------------- //
// thread data type and worker functions
// -------------------------------------------------------------------- //

struct BSign8ThreadData {
  const BSign8Args* args;
  const nn_bsign_8_job_t* job;
};

extern "C" {
ATTRIBUTE_THREAD_FUNCTION void bsign_8_thread_worker(void* context) {
  auto* td = (BSign8ThreadData*)context;
  auto* args = td->args;
  bsign_8(args->Y, args->X, args->zero_point_vec, td->job);
}
}

// -------------------------------------------------------------------- //
// op data types
// -------------------------------------------------------------------- //

struct BSign8OpData {
  BSign8Args args;
  PersistentArray<nn_bsign_8_job_t> jobs;
  PersistentArray<BSign8ThreadData> threads;
  size_t stack_size;  // The amount of stack required to run all thread workers
  int stack_scratch_index = -1;
};

// -------------------------------------------------------------------- //
// op function implementations
// -------------------------------------------------------------------- //

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  auto* op_data = construct_persistent_object<BSign8OpData>(context);

  // TODO parse data for parallelism
  // in this op we have one job per thread
  int n_threads = 1;
  op_data->jobs.allocate(context, n_threads).initialize();
  op_data->threads.allocate(context, n_threads);
  for (auto& job : op_data->jobs) {
    op_data->threads.append({&op_data->args, &job});
  }

  return op_data;
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 1);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  auto* op_data = reinterpret_cast<BSign8OpData*>(node->user_data);

  const auto* input = GetInput(context, node, 0);
  const int32_t input_size = input->bytes / sizeof(int8_t);
  bsign_8_prepare(op_data->jobs.begin(), op_data->args.zero_point_vec,
                  input_size, input->params.zero_point, op_data->jobs.size());

  /* Allocate the stack for thread workers */
  GET_THREAD_FUNCTION_STACKSIZE(op_data->stack_size, bsign_8_thread_worker);
  TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
      context, op_data->stack_size * op_data->threads.size(),
      &op_data->stack_scratch_index));

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  auto* op_data = reinterpret_cast<BSign8OpData*>(node->user_data);

  op_data->args.X = tflite::micro::GetTensorData<int8_t>(
      tflite::micro::GetEvalInput(context, node, 0));
  op_data->args.Y = tflite::micro::GetTensorData<int32_t>(
      tflite::micro::GetEvalOutput(context, node, 0));

  Dispatcher* dispatcher = GetDispatcher();

  // initialize the dispatcher
  auto* stack = static_cast<char*>(
      context->GetScratchBuffer(context, op_data->stack_scratch_index));
  TF_LITE_ENSURE(context, stack);
  dispatcher->InitializeTasks(bsign_8_thread_worker, stack,
                              op_data->stack_size);

  for (auto& thread : op_data->threads) {
    dispatcher->AddTask(reinterpret_cast<void*>(&thread));
  }
  dispatcher->JoinTasks();

  return kTfLiteOk;
}

}  // namespace bsign

TfLiteRegistration* Register_BSign_8() {
  static TfLiteRegistration r = {bsign::Init, nullptr, bsign::Prepare,
                                 bsign::Eval};
  return &r;
}

}  // namespace xcore
}  // namespace micro
}  // namespace ops
}  // namespace tflite
