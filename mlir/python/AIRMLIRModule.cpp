#include <pybind11/pybind11.h>

#include "mlir-c/BuiltinAttributes.h"
#include "mlir-c/BuiltinTypes.h"
#include "mlir-c/Diagnostics.h"

#include "air-c/Registration.h"

namespace py = pybind11;

PYBIND11_MODULE(_airMlir, m) {

  ::airRegisterAllPasses();

  m.doc() = R"pbdoc(
    Xilinx AIR MLIR Python bindings
    --------------------------

    .. currentmodule:: AIRMLIR_

    .. autosummary::
        :toctree: _generate
  )pbdoc";

  m.def("register_all_dialects", ::airRegisterAllDialects);
  m.def("_register_all_passes", ::airRegisterAllPasses);
  m.attr("__version__") = "dev";
}
