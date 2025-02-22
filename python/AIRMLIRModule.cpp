//===- AIRMLIRModule.cpp ----------------------------------------*- C++ -*-===//
//
// Copyright (C) 2021-2022, Xilinx Inc. All rights reserved.
// Copyright (C) 2022, Advanced Micro Devices, Inc. All rights reserved.
// SPDX-License-Identifier: MIT
//
//===----------------------------------------------------------------------===//

#include "mlir/Bindings/Python/NanobindAdaptors.h"

#include "air-c/Dialects.h"
#include "air-c/Registration.h"
#include "air-c/Runner.h"
#include "air-c/Transform.h"

namespace nb = nanobind;
using namespace nb::literals;
using namespace mlir::python;

NB_MODULE(_air, m) {

  ::airRegisterAllPasses();

  m.doc() = R"pbdoc(
    AIR MLIR Python bindings
    --------------------------

    .. currentmodule:: _air

    .. autosummary::
        :toctree: _generate
  )pbdoc";

  m.def(
      "register_dialect",
      [](MlirDialectRegistry registry) { airRegisterAllDialects(registry); },
      "registry"_a);

  // AIR types bindings
  nanobind_adaptors::mlir_type_subclass(m, "AsyncTokenType",
                                        mlirTypeIsAIRAsyncTokenType)
      .def_classmethod(
          "get",
          [](const nb::object &cls, MlirContext ctx) {
            return cls(mlirAIRAsyncTokenTypeGet(ctx));
          },
          "Get an instance of AsyncTokenType in given context.",
          nb::arg("self"), nb::arg("ctx") = nb::none());

  m.def("run_transform", ::runTransform);

  m.attr("__version__") = "dev";

  // AIR Runner bindings
  auto air_runner = m.def_submodule("runner", "air-runner bindings");
  air_runner.def("run", [](MlirModule module, const std::string &json,
                           const std::string &outfile,
                           const std::string &function,
                           const std::string &sim_granularity, bool verbose) {
    airRunnerRun(module, json.c_str(), outfile.c_str(), function.c_str(),
                 sim_granularity.c_str(), verbose);
  });
}
