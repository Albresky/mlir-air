# Copyright (C) 2024, Advanced Micro Devices, Inc.
# SPDX-License-Identifier: MIT

from air.ir import *
from air.dialects.air import *
from air.dialects.memref import AllocOp, DeallocOp, load, store
from air.dialects.func import FuncOp
from air.dialects.scf import for_, yield_

range_ = for_

VECTOR_LEN = 32
VECTOR_SIZE = [VECTOR_LEN, 1]
VECTOR_OUT_SIZE = [VECTOR_LEN * 2, 1]


@module_builder
def build_module():
    memrefTyIn = MemRefType.get(VECTOR_SIZE, T.i32())
    memrefTyOut = MemRefType.get(VECTOR_OUT_SIZE, T.i32())

    # We want to store our data in L1 memory
    mem_space_l1 = IntegerAttr.get(T.i32(), MemorySpace.L1)

    # This is the type definition of the tile
    image_type_l1 = MemRefType.get(
        shape=VECTOR_SIZE,
        element_type=T.i32(),
        memory_space=mem_space_l1,
    )

    # Create two channels which will send/receive the
    # input/output data respectively
    ChannelOp("ChanInA")
    ChannelOp("ChanInB")
    ChannelOp("ChanOutC")

    # We will send an image worth of data in and out
    @FuncOp.from_py_func(memrefTyIn, memrefTyIn, memrefTyOut)
    def copy(arg0, arg1, arg2):

        # The arguments are the input and output
        @launch(operands=[arg0, arg1, arg2])
        def launch_body(a, b, c):
            ChannelPut("ChanInA", a)
            ChannelPut("ChanInB", b)

            ChannelGet("ChanOutC", c)

            @segment(name="seg")
            def segment_body():

                @herd(name="addherd", sizes=[1, 1])
                def herd_body(tx, ty, sx, sy):

                    image_in_a = AllocOp(image_type_l1, [], [])
                    image_in_b = AllocOp(image_type_l1, [], [])
                    image_out_a = AllocOp(image_type_l1, [], [])
                    image_out_b = AllocOp(image_type_l1, [], [])

                    ChannelGet("ChanInA", image_in_a)
                    ChannelGet("ChanInB", image_in_b)

                    # Access every value in the tile
                    c0 = arith.ConstantOp.create_index(0)
                    for j in range_(VECTOR_LEN):
                        val_a = load(image_in_a, [c0, j])
                        val_b = load(image_in_b, [c0, j])

                        val_outa = arith.addi(val_a, val_b)
                        store(val_outa, image_out_a, [c0, j])

                        val_outb = arith.muli(val_a, val_b)
                        store(val_outb, image_out_b, [c0, j])

                        yield_([])

                    ChannelPut("ChanOutC", image_out_a)
                    ChannelPut("ChanOutC", image_out_b)

                    DeallocOp(image_in_a)
                    DeallocOp(image_in_b)
                    DeallocOp(image_out_a)
                    DeallocOp(image_out_b)


if __name__ == "__main__":
    module = build_module()
    print(module)
