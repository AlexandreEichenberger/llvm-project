// RUN: mlir-opt -test-patterns -normalize-memrefs %s | FileCheck %s

#map0 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2 floordiv 32, d3 floordiv 64, d2 mod 32, d3 mod 64)>

// CHECK-LABEL: test_norm
func @test_norm(%arg0 : memref<1x16x14x14xf32, #map0>) -> () {
    %0 = alloc() : memref<1x16x14x14xf32, #map0>
    "test.op_norm"(%arg0, %0) : (memref<1x16x14x14xf32, #map0>, memref<1x16x14x14xf32, #map0>) -> ()
    dealloc %0 :  memref<1x16x14x14xf32, #map0>

    // CHECK: %0 = alloc() : memref<1x16x1x1x32x64xf32>
    // CHECK: "test.op_norm"(%arg0, %0) : (memref<1x16x1x1x32x64xf32>, memref<1x16x1x1x32x64xf32>) -> ()
    // CHECK: dealloc %0 : memref<1x16x1x1x32x64xf32>
    return
}

// CHECK-LABEL: test_nonnorm
func @test_nonnorm(%arg0 : memref<1x16x14x14xf32, #map0>) -> () {
    %0 = alloc() : memref<1x16x14x14xf32, #map0>
    "test.op_nonnorm"(%arg0, %0) : (memref<1x16x14x14xf32, #map0>, memref<1x16x14x14xf32, #map0>) -> ()
    dealloc %0 :  memref<1x16x14x14xf32, #map0>

    // CHECK: %0 = alloc() : memref<1x16x14x14xf32, #map0>
    // CHECK: "test.op_nonnorm"(%arg0, %0) : (memref<1x16x14x14xf32, #map0>, memref<1x16x14x14xf32, #map0>) -> ()
    // CHECK: dealloc %0 : memref<1x16x14x14xf32, #map0>
    return
}

// CHECK-LABEL: test_norm_mix
func @test_norm_mix(%arg0 : memref<1x16x1x1x32x64xf32>) -> () {
    %0 = alloc() : memref<1x16x14x14xf32, #map0>
    "test.op_norm"(%arg0, %0) : (memref<1x16x1x1x32x64xf32>, memref<1x16x14x14xf32, #map0>) -> ()
    dealloc %0 :  memref<1x16x14x14xf32, #map0>

    // CHECK: %0 = alloc() : memref<1x16x1x1x32x64xf32>
    // CHECK: "test.op_norm"(%arg0, %0) : (memref<1x16x1x1x32x64xf32>, memref<1x16x1x1x32x64xf32>) -> ()
    // CHECK: dealloc %0 : memref<1x16x1x1x32x64xf32>
    return
}
