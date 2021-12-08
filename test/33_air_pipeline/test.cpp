// (c) Copyright 2021 Xilinx Inc. All Rights Reserved.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dlfcn.h>

#include <xaiengine.h>

#include "air_host.h"
#include "air_tensor.h"

int
main(int argc, char *argv[])
{
  auto col = 3;
  auto row = 3;

  /*aie_libxaie_ctx_t *xaie = */air_init_libxaie1();

  //// create the queue
  //queue_t *q = nullptr;
  //auto ret = air_queue_create(MB_QUEUE_SIZE, HSA_QUEUE_TYPE_SINGLE, &q, AIR_VCK190_SHMEM_BASE);
  //assert(ret == 0 && "failed to create queue!");

  #define DATA_LENGTH 1024
  #define DATA_TYPE int

  tensor_t<DATA_TYPE,1> input_a, input_b;
  tensor_t<DATA_TYPE,1> output;
  input_a.shape[0] = DATA_LENGTH;
  input_a.d = input_a.aligned = (DATA_TYPE*)malloc(sizeof(DATA_TYPE)*input_a.shape[0]);

  input_b.shape[0] = DATA_LENGTH;
  input_b.d = input_b.aligned = (DATA_TYPE*)malloc(sizeof(DATA_TYPE)*input_b.shape[0]);

  output.shape[0] = input_a.shape[0];
  output.d = output.aligned = (DATA_TYPE*)malloc(sizeof(DATA_TYPE)*output.shape[0]);
  DATA_TYPE d = 1;
  for (int i=0; i<input_a.shape[0]; i++) {
    input_a.d[i] = d;
    input_b.d[i] = ((DATA_TYPE)DATA_LENGTH)+d;
    output.d[i] = -1;
    d += 1;
  }

  printf("loading aie_ctrl.so\n");
  auto handle = air_module_load_from_file(nullptr);
  assert(handle && "failed to open aie_ctrl.so");

  auto launch = (void (*)(void*,void *,void *))dlsym((void*)handle, "_mlir_ciface_launch");
  assert(launch && "failed to locate _mlir_ciface_launch in .so");

  launch((void*)&input_a, (void*)&input_b, (void*)&output);

  int errors = 0;

  for (int i=0;i<DATA_LENGTH;i++) {
    DATA_TYPE ref = (input_a.d[i]*input_b.d[i]) + (DATA_TYPE)1 + (DATA_TYPE)2 + (DATA_TYPE)3;
    if (output.d[i] != ref) {
      printf("output[%d] = %d (expected %d)\n", i, output.d[i], ref);
      errors++;
    }
  }

  if (!errors) {
    printf("PASS!\n");
    return 0;
  }
  else {
    printf("fail %d/%d.\n", (DATA_LENGTH-errors), DATA_LENGTH);
    return -1;
  }
}
